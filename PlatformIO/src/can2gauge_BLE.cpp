#include <can2gauge_BLE.h>
#include <can2gauge_Nav.h> // 0.30.0: navBluetoothStopped
#include <NimBLEDevice.h>
#include <freertos/stream_buffer.h>

static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *rxCharacteristic = nullptr;
static NimBLERemoteCharacteristic *txCharacteristic = nullptr;
static bool txWithResponse = false;
static StreamBufferHandle_t rxStream = NULL;
static volatile bool linkUp = false;
static uint16_t mtu = 23;
static char linkInfo[128] = ""; // 0.24.1: 80 -> 128 (the interval and timeout added)
static bool bleStarted = false;  // Bluetooth is running (0.24.12: file-wide, so bleShutdown() can stop it)
// 0.30.6: only one task may start Bluetooth at a time. At boot the OBD task (connecting to the adapter) and the nav
// link both started it at once and the gauge crashed in a boot loop. bleStarting says "someone is starting it now";
// bleStartMux makes the check-and-set of it one step, so two tasks can't both see "nobody".
static portMUX_TYPE bleStartMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool bleStarting = false;

// services tried in order (16-bit ones as short strings)
static const char *knownServices[] = {"fff0", "ffe0", "18f0", "e7810a71-73ae-499d-8c15-faa9aef0c3f2"};
// advertised names seen on BLE OBD adapters [U: the list is from app forums, confirm with nRF Connect]
static const char *knownNames[] = {"OBD", "ELM", "VLINK", "V-LINK", "VGATE", "OBDLINK", "VEEPEAK", "KONNWEI", "ICAR", "IOS-VLINK"};

class ClientCallbacks : public NimBLEClientCallbacks
{
  void onDisconnect(NimBLEClient *disconnectedClient, int reason) override
  {
    (void)disconnectedClient;
    linkUp = false;
    DEBUG("BLE: disconnected (reason %d)", reason);
  }
};
static ClientCallbacks clientCallbacks;

static void onNotify(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t len, bool isNotify)
{
  (void)characteristic;
  (void)isNotify;
  if (rxStream)
  {
    xStreamBufferSend(rxStream, data, len, 0);
  }
}

// Bluetooth's radio controller, host task and locks must come from internal RAM, the same RAM WiFi and the web UI live
// on. It only starts when at least this much is free, so it can never take the web UI down (the only way to change
// settings in the car). 2026-10-14: with ~50 KB free (in pieces of 31 KB or less) it failed half way and crashed the
// gauge. Starting guess - the boot log's "BLE: started, used .. KB" line shows the real cost, to tune it.
#define BLE_START_MIN_FREE (64 * 1024)  // internal RAM free before Bluetooth may start
#define BLE_START_MIN_PIECE (24 * 1024) // and its largest single piece
static bool startFailed = false;        // tried and failed this boot: never again until a reboot (a half-started
                                        // Bluetooth can't be started again)
static char startError[64] = "";        // why it didn't start, for the ELM terminal / web UI

// the actual start (bleInit below makes sure only one task is in here at a time)
static bool bleStartNow()
{
  if (bleStarted)
  {
    return true; // another task started it while this one waited its turn
  }
  if (startFailed)
  {
    return false;
  }
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t largestPiece = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (freeBefore < BLE_START_MIN_FREE || largestPiece < BLE_START_MIN_PIECE)
  {
    // not marked as failed: nothing was started, so it can try again when more RAM is free
    snprintf(startError, sizeof(startError), "not enough RAM (%u KB free, needs %u)", (unsigned)(freeBefore / 1024),
             (unsigned)(BLE_START_MIN_FREE / 1024));
    DEBUG("BLE: not started - %s", startError);
    return false;
  }
  DEBUG("Setting up Bluetooth...");
  if (!NimBLEDevice::init("can2gauge"))
  {
    startFailed = true;
    snprintf(startError, sizeof(startError), "Bluetooth failed to start (reboot to try again)");
    DEBUG("BLE: %s", startError);
    return false;
  }
  NimBLEDevice::setMTU(247); // ask for more; expect the adapter to stay at 23
  rxStream = xStreamBufferCreate(1024, 1);
  bleStarted = rxStream != NULL;
  startError[0] = '\0'; // started: no error to show
  DEBUG("BLE: started, used %u KB of internal RAM (%u KB left)",
        (unsigned)((freeBefore - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  return bleStarted;
}

bool bleInit()
{
  if (bleStarted)
  {
    return true;
  }
  if (startFailed)
  {
    return false;
  }
  // 0.30.6: wait for our turn if another task is starting Bluetooth right now
  while (1)
  {
    bool isMyTurn = false;
    portENTER_CRITICAL(&bleStartMux);
    if (!bleStarting)
    {
      bleStarting = true; // ours now
      isMyTurn = true;
    }
    portEXIT_CRITICAL(&bleStartMux);
    if (isMyTurn)
    {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // the other task is still starting it: wait
  }
  const bool isStarted = bleStartNow();
  bleStarting = false; // let the next one in
  return isStarted;
}

// 0.24.12: Bluetooth off again, its internal RAM handed back (40-80 KB). For a scan with the OBD adapter switched off
// (First Boot's check, the Bluetooth tab's Scan): Bluetooth used to stay running after it until a reboot, and with the
// web UI open that left WiFi too little RAM to answer. Connecting or scanning again starts it again.
void bleShutdown()
{
  if (!bleStarted)
  {
    return;
  }
  bleDisconnect();
  client = nullptr; // NimBLE deletes its clients itself (deinit(true) below)
  NimBLEDevice::deinit(true);
  navBluetoothStopped(); // 0.30.0: the nav link's service went too (deinit deletes it)
  if (rxStream)
  {
    vStreamBufferDelete(rxStream);
    rxStream = NULL;
  }
  bleStarted = false;
  DEBUG("BLE: stopped, %u KB of internal RAM free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

// 0.24.22: Bluetooth was asked for but didn't start because internal RAM was short. At boot it starts before WiFi and
// the web server take theirs, so a restart always fixes it.
bool bleNeedsRestart()
{
  if (!strncmp(startError, "not enough RAM", 14))
  {
    return true;
  }
  return false;
}

// 0.37.15: Bluetooth is up now. The nav link asks before it adds its service: NimBLE can only add one while it isn't
// connected or scanning, so it mustn't be added to a Bluetooth the OBD adapter is already using.
bool bleRunning()
{
  return bleStarted;
}

const char *bleStartError()
{
  return startError;
}

static bool nameLooksLikeObd(const std::string &name)
{
  char upperName[32];
  size_t i = 0;
  for (; i < name.size() && i < sizeof(upperName) - 1; i++)
  {
    upperName[i] = toupper((unsigned char)name[i]);
  }
  upperName[i] = '\0';
  for (const char *keyword : knownNames)
  {
    if (strstr(upperName, keyword))
    {
      return true;
    }
  }
  return false;
}

uint8_t bleScan(uint32_t durationMs, BleFound *out, uint8_t maxResults)
{
  if (!bleInit())
  {
    return 0;
  }
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true); // many adapters put the service UUID in the scan response
  NimBLEScanResults results = scan->getResults(durationMs, false);
  uint8_t foundCount = 0;
  for (int i = 0; i < results.getCount() && foundCount < maxResults; i++)
  {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (!device)
    {
      continue;
    }
    bool likely = nameLooksLikeObd(device->getName());
    for (const char *serviceUuid : knownServices)
    {
      if (!likely && device->isAdvertisingService(NimBLEUUID(serviceUuid)))
      {
        likely = true;
      }
    }
    if (!likely && device->getName().empty())
    {
      continue; // anonymous beacons, phones, ... - not worth listing
    }
    BleFound &found = out[foundCount++];
    copyStr(found.mac, sizeof(found.mac), device->getAddress().toString().c_str());
    found.addressType = device->getAddress().getType();
    copyStr(found.name, sizeof(found.name), device->getName().c_str());
    found.rssi = (int8_t)device->getRSSI();
    found.likely = likely;
  }
  scan->clearResults();

  // likely adapters first, strongest first
  for (uint8_t slot = 0; slot < foundCount; slot++)
  {
    for (uint8_t candidate = slot + 1; candidate < foundCount; candidate++)
    {
      if ((out[candidate].likely && !out[slot].likely) ||
          (out[candidate].likely == out[slot].likely && out[candidate].rssi > out[slot].rssi))
      {
        BleFound temp = out[slot];
        out[slot] = out[candidate];
        out[candidate] = temp;
      }
    }
  }
  return foundCount;
}

static bool isStandardService(const NimBLEUUID &uuid)
{
  return uuid == NimBLEUUID((uint16_t)0x1800) || uuid == NimBLEUUID((uint16_t)0x1801) ||
      uuid == NimBLEUUID((uint16_t)0x180A) || uuid == NimBLEUUID((uint16_t)0x180F);
}

static bool pickCharacteristics(NimBLERemoteService *service)
{
  rxCharacteristic = nullptr;
  txCharacteristic = nullptr;
  const std::vector<NimBLERemoteCharacteristic *> &characteristics = service->getCharacteristics(true);
  for (NimBLERemoteCharacteristic *characteristic : characteristics)
  {
    if (!rxCharacteristic && (characteristic->canNotify() || characteristic->canIndicate()))
    {
      rxCharacteristic = characteristic;
    }
  }
  // TX: prefer a writable characteristic other than RX (FFF1 notify / FFF2 write), else the shared one (FFE1)
  for (NimBLERemoteCharacteristic *characteristic : characteristics)
  {
    if (!txCharacteristic && characteristic != rxCharacteristic &&
        (characteristic->canWrite() || characteristic->canWriteNoResponse()))
    {
      txCharacteristic = characteristic;
    }
  }
  if (!txCharacteristic && rxCharacteristic && (rxCharacteristic->canWrite() || rxCharacteristic->canWriteNoResponse()))
  {
    txCharacteristic = rxCharacteristic;
  }
  return rxCharacteristic && txCharacteristic;
}

bool bleConnect(const char *mac, uint8_t addressType, char *err, size_t errLen)
{
  if (!bleInit())
  {
    if (bleStartError()[0] != '\0')
    {
      snprintf(err, errLen, "Bluetooth %s", bleStartError()); // e.g. "not enough RAM (52 KB free, needs 64)"
    }
    else
    {
      snprintf(err, errLen, "Bluetooth didn't start");
    }
    return false;
  }
  bleDisconnect();
  if (!client)
  {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks, false);
    // 0.24.1: 7.5-15 ms interval (was a fixed 15 ms: every command's round trip is a few of these), and a 4 s
    // supervision timeout (was 1.5 s: a short squeeze on the radio - WiFi traffic, a flash write - dropped the link)
    client->setConnectionParams(6, 6, 0, 400, 64, 16); // 7.5 ms asked for (6-12 gave 15); 1.25 ms steps, timeout in 10 ms steps
    // 0.24.21: the last two are how the radio listens for the adapter while connecting (0.625 ms steps): 10 ms in every
    // 40 ms. NimBLE's own 16 / 16 listened ALL the time - WiFi and Bluetooth share one aerial, so with the adapter out
    // of range (retrying every 5 s) the web UI's taps took seconds (bench 2026-10-27: CPU idle, RAM fine, radio busy).
    client->setConnectTimeout(5000);
  }
  if (!client->connect(NimBLEAddress(std::string(mac), addressType)))
  {
    snprintf(err, errLen, "no connection to %s", mac);
    return false;
  }

  NimBLERemoteService *service = nullptr;
  for (const char *serviceUuid : knownServices)
  {
    service = client->getService(NimBLEUUID(serviceUuid));
    if (service && pickCharacteristics(service))
    {
      break;
    }
    service = nullptr;
  }
  if (!service)
  {
    const std::vector<NimBLERemoteService *> &allServices = client->getServices(true);
    for (NimBLERemoteService *candidate : allServices)
    {
      if (!isStandardService(candidate->getUUID()) && pickCharacteristics(candidate))
      {
        service = candidate;
        break;
      }
    }
  }
  if (!service)
  {
    snprintf(err, errLen, "no ELM-style service (notify + write) found");
    client->disconnect();
    return false;
  }

  // 0.24.1: write without response whenever the adapter takes it - a write with response waits for the adapter's
  // acknowledgement, one more radio round trip on every command (it was used whenever the adapter allowed both)
  if (txCharacteristic->canWriteNoResponse())
  {
    txWithResponse = false;
  }
  else
  {
    txWithResponse = true;
  }
  xStreamBufferReset(rxStream);
  if (!rxCharacteristic->subscribe(rxCharacteristic->canNotify(), onNotify))
  {
    snprintf(err, errLen, "subscribe failed");
    client->disconnect();
    return false;
  }
  mtu = client->getMTU();
  linkUp = true;
  // 0.24.1: the interval and supervision timeout the adapter agreed to (it may not take what we asked for)
  NimBLEConnInfo connection = client->getConnInfo();
  const unsigned intervalUs = connection.getConnInterval() * 1250; // 1.25 ms steps
  const unsigned timeoutMs = connection.getConnTimeout() * 10;     // 10 ms steps
  snprintf(linkInfo, sizeof(linkInfo), "%s: rx %s / tx %s%s, mtu %u, every %u.%02u ms, timeout %u ms",
           service->getUUID().toString().c_str(), rxCharacteristic->getUUID().toString().c_str(),
           txCharacteristic->getUUID().toString().c_str(), txWithResponse ? "" : " (no rsp)", mtu,
           intervalUs / 1000, (intervalUs % 1000) / 10, timeoutMs);
  DEBUG("BLE: connected %s  %s", mac, linkInfo);
  return true;
}

void bleDisconnect()
{
  if (client && client->isConnected())
  {
    client->disconnect();
    // NimBLE only lets go once the disconnect event arrives: wait for it (up to 2 s), or the next connect is refused
    const uint32_t waitStartMs = millis();
    while (client->isConnected() && millis() - waitStartMs < 2000)
    {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  linkUp = false;
  rxCharacteristic = nullptr;
  txCharacteristic = nullptr;
}

bool bleConnected()
{
  return linkUp && client && client->isConnected();
}

bool bleWrite(const uint8_t *data, size_t len)
{
  if (!bleConnected() || !txCharacteristic)
  {
    return false;
  }
  size_t chunk = mtu > 23 ? mtu - 3 : 20;
  if (chunk > 244)
  {
    chunk = 244;
  }
  for (size_t offset = 0; offset < len; offset += chunk)
  {
    const size_t chunkLen = len - offset < chunk ? len - offset : chunk;
    if (!txCharacteristic->writeValue(data + offset, chunkLen, txWithResponse))
    {
      return false;
    }
  }
  return true;
}

size_t bleRead(uint8_t *buf, size_t maxLen, uint32_t timeoutMs)
{
  if (!rxStream)
  {
    return 0;
  }
  return xStreamBufferReceive(rxStream, buf, maxLen, pdMS_TO_TICKS(timeoutMs));
}

void bleFlush()
{
  if (rxStream)
  {
    xStreamBufferReset(rxStream);
  }
}

int bleRssi()
{
  return bleConnected() ? client->getRssi() : 0;
}

const char *bleLinkInfo()
{
  return linkInfo;
}
