// Nav link (0.30.0): Google Maps turn-by-turn from the can2gauge Link phone app over Bluetooth LE, shown on the glass
// as an Android Auto style card. See include/can2gauge_Nav.h and docs/nav-link/NAV_LINK.md.
#include <can2gauge_Nav.h>
#include <can2gauge_BLE.h>
#include <GaugeUI.h>
#include <NimBLEDevice.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR: a buffer marked with it lives in PSRAM, keeping internal RAM for WiFi / Bluetooth

// ===== What the phone last sent =====
// The Bluetooth task writes these (a packet arriving) and the logic task reads them (navTick), so every read or write
// goes inside navMux: portENTER_CRITICAL stops the other task getting in half way through a copy.
static portMUX_TYPE navMux = portMUX_INITIALIZER_UNLOCKED;
static NavStatus latest;                  // distance, road, arrival time, flags (the arrow is kept below)
static uint32_t latestMs = 0;             // millis() the last STATE packet came in (0 = none)
static uint32_t latestArrowHash = 0;      // the arrow the latest STATE packet asks for (0 = no arrow)
static uint32_t changeCount = 0;          // goes up by one for every change, so navTick knows to redraw

// the turn arrow, 1 bit a dot (layout in GaugeUI.h UiNavView). It can arrive before or after the STATE packet that
// asks for it, so the newest one is held here and matched by its hash (a checksum the app works out).
EXT_RAM_BSS_ATTR static uint8_t arrowBits[(NAV_ARROW_MAX_SIDE / 8) * NAV_ARROW_MAX_SIDE];
static uint8_t arrowWidth = 0;            // 0 = no arrow received yet
static uint8_t arrowHeight = 0;
static uint32_t arrowHash = 0;            // the app's checksum of this arrow

// ===== This gauge's side =====
static bool serviceStarted = false;       // the nav service is made and advertising
static bool waitingForRestart = false;    // 0.37.15: switched on while Bluetooth was already running: starts after a restart
static volatile bool phoneConnected = false; // the phone app is connected (set in the Bluetooth task)
static uint32_t testUntilMs = 0;          // Show Test Turn: the test card is up until this millis() (0 = no test)
static bool cardWanted = false;           // navTick put the card up (and hasn't taken it down)
static char shownInstruction[96] = "";    // what the card showed last, to tell a new turn from an update
static uint32_t shownArrowKey = 0;        // ... and its arrow (a stored arrow's number, or the dots' hash)
static uint32_t shownChangeCount = 0;     // changeCount when the card was last drawn
static uint32_t lastAdvertCheckMs = 0;    // 0.30.5: millis() navTick last checked the gauge is still advertising
static volatile bool isAdvertising = false; // 0.30.5: the phone can see the gauge now (checked every few seconds)
static uint32_t advertRestarts = 0;       // 0.30.5: times advertising had stopped and was started again (web UI)
#define NAV_ADVERT_CHECK_MS 5000          // how often navTick checks the advert is still going
EXT_RAM_BSS_ATTR static UiNavView view;   // the card's contents (660 bytes: kept off the logic task's stack)

// 0.30.1: the stored arrows' files, in NAV_ARROW_* order (index 0 = NAV_ARROW_STRAIGHT). Drawn by tools/nav_arrows.py.
static const char *arrowFiles[NAV_ARROW_COUNT] = {
    "/nav/straight.bin",     // 1 NAV_ARROW_STRAIGHT
    "/nav/left.bin",         // 2 NAV_ARROW_LEFT
    "/nav/right.bin",        // 3 NAV_ARROW_RIGHT
    "/nav/slight_left.bin",  // 4 NAV_ARROW_SLIGHT_LEFT
    "/nav/slight_right.bin", // 5 NAV_ARROW_SLIGHT_RIGHT
    "/nav/uturn.bin",        // 6 NAV_ARROW_UTURN
    "/nav/roundabout1.bin",  // 7 roundabout, 1st exit
    "/nav/roundabout2.bin",  // 8 2nd exit
    "/nav/roundabout3.bin",  // 9 3rd exit
    "/nav/roundabout4.bin",  // 10 4th exit
    "/nav/roundabout5.bin",  // 11 5th exit
    "/nav/arrive.bin",       // 12 NAV_ARROW_ARRIVE
};

// ---------------------------------------------------------------------------
// reading the phone's packets (layout: docs/nav-link/NAV_LINK.md)
// ---------------------------------------------------------------------------

// a 32-bit number from 4 bytes, low byte first (the order the phone sends them)
static uint32_t readNumber32(const uint8_t *data, uint16_t pos)
{
  uint32_t value = data[pos];               // lowest byte
  value |= (uint32_t)data[pos + 1] << 8;
  value |= (uint32_t)data[pos + 2] << 16;
  value |= (uint32_t)data[pos + 3] << 24;   // highest byte
  return value;
}

// one text field: a length byte, then that many bytes of text. Copies what fits into dst (always ends with a 0) and
// moves pos past the field. false = the packet stops short (cut off, or not a nav packet).
static bool readText(const uint8_t *data, uint16_t len, uint16_t &pos, char *dst, uint16_t dstLen)
{
  if (pos >= len)
  {
    return false;
  }
  const uint8_t textLen = data[pos];
  pos++;
  if (pos + textLen > len)
  {
    return false;
  }
  uint16_t copyLen = textLen;
  if (copyLen > dstLen - 1)
  {
    copyLen = dstLen - 1;
  }
  // a cut mustn't split a letter made of several bytes (UTF-8: its follow-on bytes start with the bits 10)
  while (copyLen > 0 && copyLen < textLen && (data[pos + copyLen] & 0xC0) == 0x80)
  {
    copyLen--;
  }
  memcpy(dst, data + pos, copyLen);
  dst[copyLen] = '\0';
  pos = pos + textLen;
  return true;
}

// STATE packet: version, flags, stored arrow, metres to the turn (4 bytes), arrow hash (4 bytes), then three text
// fields: distance as shown, the instruction, the arrival time line
static void takeStatePacket(const uint8_t *data, uint16_t len)
{
  if (len < 13 || data[0] != NAV_PROTO_VERSION)
  {
    return; // too short for the fixed part, or a version this gauge doesn't know
  }
  const uint8_t flags = data[1];                      // byte 1: NAV_FLAG_* bits
  uint8_t arrowId = data[2];                          // byte 2: the stored arrow (NAV_ARROW_*), 0 = the dots
  if (arrowId > NAV_ARROW_COUNT)
  {
    arrowId = NAV_ARROW_NONE; // a number this gauge has no arrow for: show the dots, if any
  }
  const uint32_t distToTurnM = readNumber32(data, 3); // bytes 3-6: metres to the turn
  const uint32_t wantedArrow = readNumber32(data, 7); // bytes 7-10: which dots go with this turn (0 = none)
  uint16_t routeProgress = data[11] | (data[12] << 8); // 0.30.8: bytes 11-12: journey done, thousandths, low byte first
  if (routeProgress > 1000)
  {
    routeProgress = NAV_PROGRESS_UNKNOWN; // 0xFFFF from the app, or nonsense: no ring
  }
  // the text goes into locals first, so a packet that turns out to be broken changes nothing
  char distText[16];
  char instruction[96];
  char etaText[40];
  uint16_t pos = 13; // 0.30.8: was 11 (the journey progress went in before the text)
  if (!readText(data, len, pos, distText, sizeof(distText)) ||
      !readText(data, len, pos, instruction, sizeof(instruction)) ||
      !readText(data, len, pos, etaText, sizeof(etaText)))
  {
    return;
  }

  portENTER_CRITICAL(&navMux);
  latest.navigating = (flags & NAV_FLAG_NAVIGATING) != 0; // bit 0
  latest.rerouting = (flags & NAV_FLAG_REROUTING) != 0;   // bit 1
  latest.arrived = (flags & NAV_FLAG_ARRIVED) != 0;       // bit 2
  latest.distToTurnM = distToTurnM;
  latest.routeProgress = routeProgress;
  latest.arrowId = arrowId;
  memcpy(latest.distText, distText, sizeof(distText));
  memcpy(latest.instruction, instruction, sizeof(instruction));
  memcpy(latest.etaText, etaText, sizeof(etaText));
  latest.packets++;
  latestArrowHash = wantedArrow;
  latestMs = millis();
  if (latestMs == 0)
  {
    latestMs = 1; // 0 means "nothing yet"
  }
  changeCount++;
  portEXIT_CRITICAL(&navMux);
}

// ARROW packet: version, width, height, hash (4 bytes), then the dots, 1 bit each, a row at a time
static void takeArrowPacket(const uint8_t *data, uint16_t len)
{
  if (len < 7 || data[0] != NAV_PROTO_VERSION)
  {
    return;
  }
  const uint8_t width = data[1];  // byte 1: width in dots
  const uint8_t height = data[2]; // byte 2: height in dots
  const uint32_t hash = readNumber32(data, 3); // bytes 3-6: the app's checksum of the arrow
  if (width == 0 || height == 0 || width > NAV_ARROW_MAX_SIDE || height > NAV_ARROW_MAX_SIDE)
  {
    return;
  }
  const uint16_t bytesNeeded = ((width + 7) / 8) * height; // each row padded to whole bytes
  if (len < 7 + bytesNeeded)
  {
    return;
  }
  portENTER_CRITICAL(&navMux);
  memcpy(arrowBits, data + 7, bytesNeeded);
  arrowWidth = width;
  arrowHeight = height;
  arrowHash = hash;
  changeCount++;
  portEXIT_CRITICAL(&navMux);
}

// ---------------------------------------------------------------------------
// Bluetooth callbacks. NimBLE only offers these as classes with "virtual" functions we fill in (override), so each is
// kept as small as possible and just hands over to the plain functions above. They run in the Bluetooth task.
// ---------------------------------------------------------------------------
class NavServerCallbacks : public NimBLEServerCallbacks
{
  // the phone app connected
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connection) override
  {
    (void)server;
    (void)connection;
    phoneConnected = true;
    DEBUG("Nav: phone connected");
  }
  // the phone app went (out of range, Bluetooth off, phone off): NimBLE starts advertising again by itself
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connection, int reason) override
  {
    (void)server;
    (void)connection;
    phoneConnected = false;
    portENTER_CRITICAL(&navMux);
    latest.navigating = false; // don't leave an old turn on the glass
    changeCount++;
    portEXIT_CRITICAL(&navMux);
    DEBUG("Nav: phone disconnected (reason %d)", reason);
  }
};

class NavStateCallbacks : public NimBLECharacteristicCallbacks
{
  // a STATE packet written by the phone
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connection) override
  {
    (void)connection;
    NimBLEAttValue value = characteristic->getValue();
    takeStatePacket(value.data(), value.length());
  }
};

class NavArrowCallbacks : public NimBLECharacteristicCallbacks
{
  // an ARROW packet written by the phone
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connection) override
  {
    (void)connection;
    NimBLEAttValue value = characteristic->getValue();
    takeArrowPacket(value.data(), value.length());
  }
};

static NavServerCallbacks serverCallbacks;
static NavStateCallbacks stateCallbacks;
static NavArrowCallbacks arrowCallbacks;

// ---------------------------------------------------------------------------
// starting and stopping
// ---------------------------------------------------------------------------

bool navWanted()
{
  return settings.navMode != NAV_MODE_OFF;
}

uint32_t navPopupMetres()
{
  return settings.navPopupM;
}

// make the nav service and start advertising it (Bluetooth already running)
static void startService()
{
  if (serviceStarted)
  {
    NimBLEDevice::startAdvertising(); // made before (switched off and on again): just advertise again
    return;
  }
  DEBUG("Setting up the nav link...");
  NimBLEServer *server = NimBLEDevice::createServer(); // returns the one already made, if there is one
  server->setCallbacks(&serverCallbacks, false);       // false: they're ours (static), NimBLE mustn't delete them
  server->advertiseOnDisconnect(true);                 // the phone dropping out starts advertising again

  NimBLEService *service = server->createService(NAV_SERVICE_UUID);
  NimBLECharacteristic *stateCharacteristic = service->createCharacteristic(
      NAV_STATE_UUID,                                  // STATE packets
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, // the phone writes, with or without an answer
      NAV_STATE_MAX_BYTES);                            // biggest packet taken
  stateCharacteristic->setCallbacks(&stateCallbacks);
  NimBLECharacteristic *arrowCharacteristic = service->createCharacteristic(
      NAV_ARROW_UUID,                                  // ARROW packets
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
      7 + (NAV_ARROW_MAX_SIDE / 8) * NAV_ARROW_MAX_SIDE); // header + the biggest arrow (519 bytes)
  arrowCharacteristic->setCallbacks(&arrowCallbacks);
  service->start();

  // the advert carries the service id (the app looks for it); the name goes in the scan answer, as the 128-bit id and
  // the name don't both fit in one 31-byte advert
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(NAV_SERVICE_UUID);
  advertising->enableScanResponse(true);
  advertising->setName("can2gauge");
  serviceStarted = true;
  if (advertising->start())
  {
    DEBUG("Set up the nav link! Advertising to the can2gauge Link app");
  }
  else
  {
    DEBUG("Nav: advertising didn't start - navTick tries again in a few seconds"); // 0.30.5: was never checked
  }
}

// Bluetooth was stopped (src/can2gauge_BLE.cpp bleShutdown): the service went with it and has to be made again
void navBluetoothStopped()
{
  serviceStarted = false;
  phoneConnected = false;
}

// 0.37.15: the web UI shows Restart Now while this is true
bool navNeedsRestart()
{
  if (navWanted() && !serviceStarted && waitingForRestart)
  {
    return true;
  }
  return false;
}

void setupNav()
{
  memset(&latest, 0, sizeof(latest));
  latest.distToTurnM = NAV_DIST_UNKNOWN;
  if (!navWanted())
  {
    return; // off: Bluetooth isn't started for it (it may still start later for the OBD adapter)
  }
  if (!bleInit())
  {
    DEBUG("Nav: Bluetooth didn't start - %s", bleStartError());
    return;
  }
  startService();
}

void navRestart()
{
  if (navWanted())
  {
    // 0.37.15: Bluetooth already running (the OBD adapter connected, or a scan) and the nav service never made: adding
    // it now crashed the gauge (coredump: NimBLE's "assert failed: ble_svc_gap_init", it can only add a service while
    // nothing is connected or scanning). Leave it for a restart: setupNav makes it at boot, before the OBD task starts.
    if (!serviceStarted && bleRunning())
    {
      waitingForRestart = true;
      DEBUG("Nav: Bluetooth is already in use - the nav link starts after a restart");
      return;
    }
    if (!bleInit())
    {
      // switched on after boot and Bluetooth didn't fit: the web UI says to restart (bleNeedsRestart)
      DEBUG("Nav: Bluetooth didn't start - %s", bleStartError());
      return;
    }
    startService();
    return;
  }
  // switched off: stop advertising and let the phone go. Bluetooth itself stays up until the next restart (the OBD
  // adapter may be using it); the next restart doesn't start it for the nav link.
  waitingForRestart = false; // 0.37.15: off again: no restart needed for it
  if (serviceStarted)
  {
    NimBLEDevice::stopAdvertising();
    NimBLEServer *server = NimBLEDevice::getServer();
    if (server)
    {
      const uint8_t connectedCount = server->getConnectedCount(); // phones connected to us (normally 0 or 1)
      for (uint8_t i = 0; i < connectedCount; i++)
      {
        NimBLEConnInfo connection = server->getPeerInfo(i);
        server->disconnect(connection.getConnHandle());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Show Test Turn (web UI): a right turn, 250 m, no phone needed (0.30.1: the stored right-turn arrow)
// ---------------------------------------------------------------------------

void navShowTest()
{
  testUntilMs = millis() + NAV_TEST_MS;
  if (testUntilMs == 0)
  {
    testUntilMs = 1; // 0 means "no test"
  }
  shownInstruction[0] = '\0'; // count it as a new turn, so a card tapped away comes back for it
}

// ---------------------------------------------------------------------------
// the card
// ---------------------------------------------------------------------------

// 0.30.5: every few seconds, make sure a phone can find the gauge. With nav on and no phone connected the gauge
// should always be advertising; if it isn't (a start that failed, Bluetooth busy with the OBD adapter) the app would
// wait for ever, so start it again.
static void checkAdvertising(uint32_t now)
{
  if (now - lastAdvertCheckMs < NAV_ADVERT_CHECK_MS)
  {
    return;
  }
  lastAdvertCheckMs = now;
  if (!serviceStarted || !navWanted())
  {
    isAdvertising = false;
    return;
  }
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  isAdvertising = advertising->isAdvertising();
  if (isAdvertising || phoneConnected)
  {
    return; // fine: visible, or the phone is already here
  }
  advertRestarts++;
  DEBUG("Nav: wasn't advertising (no phone connected) - starting it again (%lu)", (unsigned long)advertRestarts);
  if (advertising->start())
  {
    isAdvertising = true;
  }
}

void navTick()
{
  const uint32_t now = millis();
  checkAdvertising(now);
  // the test card's time is up
  if (testUntilMs != 0 && (int32_t)(now - testUntilMs) >= 0)
  {
    testUntilMs = 0;
    shownInstruction[0] = '\0'; // the real turn (if any) counts as new again
  }

  // a copy of the latest, taken in one go
  portENTER_CRITICAL(&navMux);
  // the app repeats the turn every 10 s while navigating: nothing for NAV_TIMEOUT_MS means it's gone (phone flat,
  // Maps closed, out of range)
  if (latest.navigating && now - latestMs > NAV_TIMEOUT_MS)
  {
    latest.navigating = false;
    changeCount++;
  }
  const bool navigating = latest.navigating;
  const bool rerouting = latest.rerouting;
  const bool arrived = latest.arrived;
  const uint32_t distToTurnM = latest.distToTurnM;
  const uint32_t nowChangeCount = changeCount;
  bool arrowReady = false; // the arrow held matches the one this turn asks for
  if (latestArrowHash != 0 && arrowHash == latestArrowHash && arrowWidth > 0)
  {
    arrowReady = true;
  }
  portEXIT_CRITICAL(&navMux);

  // should the card be up?
  bool wanted = false;
  if (testUntilMs != 0)
  {
    wanted = true; // Show Test Turn, whatever the setting
  }
  else if (navigating && navWanted() && !runtime.standby)
  {
    if (settings.navMode == NAV_MODE_WHOLE_ROUTE)
    {
      wanted = true;
    }
    else if (rerouting || arrived)
    {
      wanted = true; // worth a look whatever the distance
    }
    else if (distToTurnM != NAV_DIST_UNKNOWN && distToTurnM <= navPopupMetres())
    {
      wanted = true; // a turn coming up
    }
  }

  if (!wanted)
  {
    if (cardWanted)
    {
      // 0.30.5: only count it as gone once it really went (the screen can be busy for a moment); before, a busy
      // screen left the card up for good ("Show Test Turn doesn't clear")
      if (uiNavHide())
      {
        cardWanted = false;
      }
    }
    return;
  }
  if (cardWanted && nowChangeCount == shownChangeCount && testUntilMs == 0)
  {
    return; // up already and nothing new
  }

  // fill the card
  memset(&view, 0, sizeof(view));
  uint32_t viewArrowKey = 0; // which arrow this is: a stored arrow's number, or the dots' hash (0 = none)
  if (testUntilMs != 0)
  {
    copyStr(view.distText, sizeof(view.distText), "250 m");
    copyStr(view.instruction, sizeof(view.instruction), "Turn right onto Test Street");
    copyStr(view.etaText, sizeof(view.etaText), "12 min - 5.2 km - 22:41");
    copyStr(view.arrowFile, sizeof(view.arrowFile), arrowFiles[NAV_ARROW_RIGHT - 1]);
    viewArrowKey = NAV_ARROW_RIGHT;
    view.hasProgress = true; // 0.30.8: the ring, 40 % of the journey done
    view.routeProgress = 400;
  }
  else
  {
    portENTER_CRITICAL(&navMux);
    view.rerouting = latest.rerouting;
    view.arrived = latest.arrived;
    memcpy(view.distText, latest.distText, sizeof(view.distText));
    memcpy(view.instruction, latest.instruction, sizeof(view.instruction));
    memcpy(view.etaText, latest.etaText, sizeof(view.etaText));
    if (latest.routeProgress != NAV_PROGRESS_UNKNOWN)
    {
      view.hasProgress = true; // 0.30.8: the ring round the edge: how much of the journey is done
      view.routeProgress = latest.routeProgress;
    }
    const uint8_t arrowId = latest.arrowId;
    if (arrowId != NAV_ARROW_NONE)
    {
      viewArrowKey = arrowId; // a stored arrow (1..12; a dots hash is never that small in practice)
    }
    else if (arrowReady)
    {
      view.arrowWidth = arrowWidth; // Maps' own arrow, as the phone sent it
      view.arrowHeight = arrowHeight;
      memcpy(view.arrowBits, arrowBits, sizeof(view.arrowBits));
      viewArrowKey = arrowHash;
    }
    portEXIT_CRITICAL(&navMux);
    if (arrowId != NAV_ARROW_NONE)
    {
      copyStr(view.arrowFile, sizeof(view.arrowFile), arrowFiles[arrowId - 1]); // outside navMux: no need to hold it
    }
  }

  // a new turn (new road or new arrow) brings back a card that was tapped away; an update to the same turn doesn't
  bool newTurn = false;
  if (viewArrowKey != shownArrowKey || strcmp(view.instruction, shownInstruction) != 0)
  {
    newTurn = true;
  }
  if (cardWanted && testUntilMs != 0 && !newTurn)
  {
    return; // the test card is up and doesn't change
  }
  if (!uiNavShow(view, newTurn))
  {
    return; // 0.30.5: the screen was busy: try again on the next tick (nothing below is noted as shown)
  }
  cardWanted = true;
  shownChangeCount = nowChangeCount;
  shownArrowKey = viewArrowKey;
  copyStr(shownInstruction, sizeof(shownInstruction), view.instruction);
}

void navStatus(NavStatus &out)
{
  portENTER_CRITICAL(&navMux);
  out = latest;
  const uint32_t lastMs = latestMs;
  portEXIT_CRITICAL(&navMux);
  out.started = serviceStarted;
  out.phoneConnected = phoneConnected;
  out.advertising = isAdvertising;     // 0.30.5
  out.advertRestarts = advertRestarts; // 0.30.5
  out.testShowing = testUntilMs != 0;
  out.cardShowing = cardWanted;
  if (lastMs != 0)
  {
    out.ageMs = millis() - lastMs;
  }
  else
  {
    out.ageMs = 0;
  }
}
