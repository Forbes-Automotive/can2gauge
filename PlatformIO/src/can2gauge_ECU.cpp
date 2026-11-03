// An aftermarket ECU's live data over WiFi from usb2ip (Ignitron, MaxxECU, Link, Haltech or EMU PRO) - see
// include/can2gauge_ECU.h.
// Forbes Automotive, 2026.
#include <can2gauge_ECU.h>
#include <ecunow_library.h> // the ECUs' channel lists: big tables, so only this file includes it
#include <esp_wifi.h>       // esp_wifi_get_channel, for the web UI's "same WiFi channel" hint
#include <math.h>           // isfinite, NAN

// ===== Which gauge signal each everyday value goes to =====
struct EcuValueMap
{
  uint8_t value;      // ecunValue_t: where it sits in the DATA packet
  signalId_t signal;  // the gauge's own signal
  float multiply;     // usb2ip's unit x this = the gauge signal's unit
  bool isCarSignal;   // true = other sources on the gauge fill it too (CAN, OBD, K-line): only used while they don't
};

// static = private to this file, const = never changes (it stays in flash)
static const EcuValueMap valueMap[] = {
    {ECUN_V_RPM, SIG_RPM, 1.0f, true},                    // rpm
    {ECUN_V_SPEED, SIG_SPEED, 1.0f, true},                // km/h
    {ECUN_V_COOLANT, SIG_COOLANT, 1.0f, true},            // degC
    {ECUN_V_IAT, SIG_IAT, 1.0f, true},                    // degC
    {ECUN_V_MAP, SIG_BOOST, 10.0f, true},                 // kPa absolute -> mbar absolute (SIG_BOOST_REL follows from it)
    {ECUN_V_PEDAL, SIG_PEDAL, 1.0f, true},                // %
    {ECUN_V_THROTTLE, SIG_THROTTLE, 1.0f, true},          // %
    {ECUN_V_LOAD, SIG_LOAD, 1.0f, true},                  // %
    {ECUN_V_LAMBDA, SIG_LAMBDA, 1.0f, true},              // lambda
    {ECUN_V_LAMBDA_TARGET, SIG_LAMBDA_TARGET, 1.0f, false}, // lambda: only usb2ip sends it
    {ECUN_V_TIMING, SIG_TIMING, 1.0f, true},              // deg BTDC
    {ECUN_V_KNOCK_RETARD, SIG_KNOCK_RETARD, 1.0f, false}, // deg: only usb2ip sends it
    {ECUN_V_INJ_DUTY, SIG_INJ_DUTY, 1.0f, false},         // %: only usb2ip sends it
    {ECUN_V_BATTERY, SIG_BATTERY, 1.0f, true},            // V
    {ECUN_V_OIL_TEMP, SIG_OIL_TEMP, 1.0f, true},          // degC
    {ECUN_V_OIL_PRESSURE, SIG_OIL_PRESSURE, 1.0f, true},  // bar
    {ECUN_V_FUEL_PRESSURE, SIG_FUEL_PRESSURE, 1.0f, true}, // bar
    {ECUN_V_EGT, SIG_EGT, 1.0f, true},                    // degC
    {ECUN_V_ETHANOL, SIG_ETHANOL, 1.0f, true},            // %
    {ECUN_V_GEAR, SIG_GEAR, 1.0f, true},                  // 0 = neutral / unknown, 1..10
};
// a check while compiling: one row for every everyday value in ecunow_proto.h
static_assert(sizeof(valueMap) / sizeof(valueMap[0]) == ECUN_VALUES, "valueMap needs a row per ecunValue_t");

// ===== State =====
// ecuMux guards everything below that the WiFi task writes and the web server reads
static portMUX_TYPE ecuMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t lastPacketMs = 0; // when the followed usb2ip was last heard (0 = never); volatile: the WiFi task writes it
static int8_t lastRssi = 0;                 // its signal strength, dBm
static uint8_t lastSeq = 0;                 // its last packet counter
static uint8_t lastKind = ECUN_K_UNKNOWN;   // the ECU it reads
static uint8_t lastFlags = 0;               // ECUN_F_* of its last DATA packet
static uint8_t lastFw[2] = {0, 0};          // its firmware major, minor
static uint32_t packetCount = 0;            // packets received from it
static uint32_t lostCount = 0;              // packets lost in the air
static char sourceName[24];                 // its WiFi name (INFO)
static char ecuName[24];                    // the ECU as it identified it (INFO)
static uint16_t theirLibraryId = 0;         // the channel list it says it uses (INFO / RAW), 0 = not heard yet
static float lastValue[ECUN_VALUES];        // its everyday values, gauge units (NAN = not sent)
static bool lastValueUsed[ECUN_VALUES];     // true = went on the gauge
static uint32_t carWrote[SIG_COUNT];        // when this module last wrote a car signal (to step aside for other sources)

// the latest raw word of every channel, and when it arrived (0 = never). One store, as big as the biggest list (the largest
// aftermarket ECU's): only one usb2ip is followed, so only one ECU's channels arrive at a time. storeKind says whose they are.
#define ECU_ROWS_MAX ECUN_LIB_EMU_PRO_ROWS
// checks while compiling: every ECU's list fits in the store
static_assert(ECUN_LIB_IGNITRON_ROWS <= ECU_ROWS_MAX, "Ignitron list bigger than the store");
static_assert(ECUN_LIB_MAXXECU_ROWS <= ECU_ROWS_MAX, "MaxxECU list bigger than the store");
static_assert(ECUN_LIB_LINK_ROWS <= ECU_ROWS_MAX, "Link list bigger than the store");
static_assert(ECUN_LIB_HALTECH_ROWS <= ECU_ROWS_MAX, "Haltech list bigger than the store");
static uint16_t *rowWords = (uint16_t *)psramCalloc(ECU_ROWS_MAX, sizeof(uint16_t)); // each channel's last value, in PSRAM
static uint32_t *rowMs = (uint32_t *)psramCalloc(ECU_ROWS_MAX, sizeof(uint32_t));      // when it arrived (0 = never), in PSRAM
static uint8_t storeKind = ECUN_K_UNKNOWN;  // the ECU whose channels are in rowWords / rowMs

// the gauge pages' ECU values (user signals): which ECU and which list row each one reads (worked out by
// ecuNowUserSigsChanged, so a RAW packet doesn't have to search the list)
static uint8_t userSigKind[USER_SIGS_ALL]; // ecunKind_t, ECUN_K_UNKNOWN = not an ECU value
static int16_t userSigRow[USER_SIGS_ALL];  // row in that ECU's list, -1 = the channel isn't in this firmware's list

// other usb2ips in range
struct OtherSender
{
  uint8_t mac[6];  // its MAC
  char name[24];   // its WiFi name ("" until its INFO packet)
  int8_t rssi;     // signal strength, dBm
  uint32_t heardMs; // when it was last heard (0 = free slot)
};
static OtherSender others[ECU_OTHERS];

// ===== Helpers =====

// true if all six bytes are zero (no usb2ip saved)
static bool isZeroMac(const uint8_t *mac)
{
  for (uint8_t i = 0; i < 6; i++)
  {
    if (mac[i] != 0)
    {
      return false;
    }
  }
  return true;
}

// the channel list of an ECU kind, and how many rows it has (NULL / 0 for an unknown ECU)
static const EcunLibRow *libraryFor(uint8_t kind, uint16_t &rows)
{
  if (kind == ECUN_K_IGNITRON)
  {
    rows = ECUN_LIB_IGNITRON_ROWS;
    return ECUN_LIB_IGNITRON;
  }
  if (kind == ECUN_K_MAXXECU)
  {
    rows = ECUN_LIB_MAXXECU_ROWS;
    return ECUN_LIB_MAXXECU;
  }
  if (kind == ECUN_K_LINK)
  {
    rows = ECUN_LIB_LINK_ROWS;
    return ECUN_LIB_LINK;
  }
  if (kind == ECUN_K_HALTECH)
  {
    rows = ECUN_LIB_HALTECH_ROWS;
    return ECUN_LIB_HALTECH;
  }
  if (kind == ECUN_K_EMU_PRO)
  {
    rows = ECUN_LIB_EMU_PRO_ROWS;
    return ECUN_LIB_EMU_PRO;
  }
  rows = 0;
  return NULL;
}

// a raw 16-bit word to the channel's value, the way the header of ecunow_library.h says
static float decodeRaw(const EcunLibRow &row, uint16_t raw)
{
  if ((row.flags & ECUN_LIB_BITEXT) && (raw & 0x8000)) // MaxxECU: bit 15 set means the low 15 bits count in tens
  {
    return (raw & 0x7FFF) * 10.0f * row.scale;
  }
  if (row.flags & ECUN_LIB_SIGNED) // the word is a signed number (minus values possible)
  {
    int16_t signedRaw = (int16_t)raw;
    return signedRaw * row.scale + row.bias;
  }
  return raw * row.scale + row.bias;
}

// the settings slot of user signal userSigIndex (0..15 page main / second values, 16..39 page slots)
static const UserSigCfg &userSigSettings(uint8_t userSigIndex)
{
  if (userSigIndex < USER_SIGS)
  {
    return settings.pages[userSigIndex / 2].valueSigs[userSigIndex % 2]; // page main / second value
  }
  const uint8_t slotIndex = userSigIndex - USER_SIGS;
  return settings.pages[slotIndex / 3].slotSigs[slotIndex % 3]; // a page slot
}

// An everyday value from the ECU: only while no other source on this gauge (its own CAN decode, OBD, K-line ...) is
// writing the same signal - the same rule as OpenHaldex's. Returns true if it went on the gauge.
static bool setCarSignal(signalId_t id, float value, uint32_t now)
{
  bool otherSourceFresh = false;
  if (signalValid(id))
  {
    if (carWrote[id] == 0)
    {
      otherSourceFresh = true; // fresh, and we never wrote it: another source has it
    }
    else if (signalAge(id) < now - carWrote[id])
    {
      otherSourceFresh = true; // someone else wrote it after we last did
    }
  }
  if (otherSourceFresh)
  {
    return false;
  }
  signalSetBus(id, value);
  carWrote[id] = now | 1; // | 1: never 0, which means "never written"
  return true;
}

// ===== Receive (WiFi task: keep it short) =====

// another usb2ip in range: list it for the web UI (its name comes with its INFO packets)
static void noteOther(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len, uint8_t type, uint32_t now)
{
  char name[24];
  bool hasName = false;
  if (type == ECUN_T_INFO && len >= (int)sizeof(EcunInfo))
  {
    EcunInfo info;
    memcpy(&info, data, sizeof(info)); // a copy to read with dots
    memcpy(name, info.source, sizeof(name));
    name[sizeof(name) - 1] = 0;
    hasName = true;
  }
  portENTER_CRITICAL(&ecuMux);
  int slot = -1;
  for (uint8_t i = 0; i < ECU_OTHERS; i++) // its own slot ...
  {
    if (slot < 0 && others[i].heardMs != 0 && memcmp(others[i].mac, mac, 6) == 0)
    {
      slot = i;
    }
  }
  for (uint8_t i = 0; i < ECU_OTHERS; i++) // ... or a free / silent (10 s) one
  {
    if (slot < 0 && (others[i].heardMs == 0 || now - others[i].heardMs > 10000))
    {
      slot = i;
      memset(&others[i], 0, sizeof(OtherSender));
      memcpy(others[i].mac, mac, 6);
    }
  }
  if (slot >= 0)
  {
    others[slot].rssi = rssi;
    others[slot].heardMs = now | 1;
    if (hasName)
    {
      memcpy(others[slot].name, name, sizeof(name));
    }
  }
  portEXIT_CRITICAL(&ecuMux);
}

// DATA: the everyday values
static void handleData(const uint8_t *data, uint32_t now)
{
  EcunData packet;
  memcpy(&packet, data, sizeof(packet)); // a copy to read with dots
  if ((packet.h.flags & ECUN_F_LIVE) == 0)
  {
    return; // usb2ip isn't reading the ECU right now: nothing fresh to show
  }
  float gaugeValue[ECUN_VALUES];
  bool used[ECUN_VALUES];
  for (uint8_t i = 0; i < ECUN_VALUES; i++)
  {
    const EcuValueMap &map = valueMap[i];
    gaugeValue[map.value] = NAN;
    used[map.value] = false;
    float value = packet.value[map.value];
    if (!isfinite(value))
    {
      continue; // NAN = this ECU doesn't have it
    }
    gaugeValue[map.value] = value * map.multiply;
    if (map.isCarSignal)
    {
      if (settings.ecuCarSignals)
      {
        used[map.value] = setCarSignal(map.signal, gaugeValue[map.value], now);
      }
    }
    else
    {
      signalSetBus(map.signal, gaugeValue[map.value]); // heard by every gauge in range, like a frame on the shared bus
      used[map.value] = true;
    }
  }
  portENTER_CRITICAL(&ecuMux);
  for (uint8_t i = 0; i < ECUN_VALUES; i++)
  {
    lastValue[i] = gaugeValue[i];
    lastValueUsed[i] = used[i];
  }
  portEXIT_CRITICAL(&ecuMux);
}

// RAW: a run of the ECU's channels as raw words
static void handleRaw(const uint8_t *data, int len, uint8_t kind, uint32_t now)
{
  static EcunRaw packet; // static: 249 bytes, kept off the WiFi task's stack (only that task uses it)
  const int wordsStart = sizeof(EcunRaw) - sizeof(packet.word); // bytes before word[0]
  if (len < wordsStart)
  {
    return;
  }
  memset(&packet, 0, sizeof(packet));
  int copyLength = len;
  if (copyLength > (int)sizeof(packet))
  {
    copyLength = sizeof(packet);
  }
  memcpy(&packet, data, copyLength); // a copy to read with dots
  if (packet.count == 0 || packet.count > ECUN_RAW_WORDS || len < wordsStart + packet.count * 2)
  {
    return; // cut short or not a RAW packet we understand
  }

  uint16_t rows = 0;
  const EcunLibRow *library = libraryFor(kind, rows);
  if (library == NULL)
  {
    return; // an ECU this firmware has no channel list for
  }
  portENTER_CRITICAL(&ecuMux);
  theirLibraryId = packet.libraryId;
  portEXIT_CRITICAL(&ecuMux);
  if (packet.libraryId != ecuNowLibraryId(kind) || packet.firstRow + packet.count > rows)
  {
    return; // a different channel list (one of the two needs updating): the words would land on the wrong channels
  }

  portENTER_CRITICAL(&ecuMux);
  if (kind != storeKind) // a different ECU's channels from now on: the old ones no longer count
  {
    memset(rowMs, 0, ECU_ROWS_MAX * sizeof(uint32_t));
    storeKind = kind;
  }
  for (uint8_t i = 0; i < packet.count; i++)
  {
    rowWords[packet.firstRow + i] = packet.word[i];
    rowMs[packet.firstRow + i] = now | 1; // | 1: never 0, which means "never received"
  }
  portEXIT_CRITICAL(&ecuMux);

  // the gauge pages' ECU channels in this packet go straight onto their signals
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL; userSigIndex++)
  {
    if (userSigKind[userSigIndex] != kind)
    {
      continue;
    }
    int16_t row = userSigRow[userSigIndex];
    if (row < (int16_t)packet.firstRow || row >= (int16_t)(packet.firstRow + packet.count))
    {
      continue;
    }
    float value = decodeRaw(library[row], packet.word[row - packet.firstRow]);
    signalSetBus((signalId_t)(SIG_USER_1 + userSigIndex), value);
  }
}

// INFO: who is sending and which channel list it uses
static void handleInfo(const uint8_t *data)
{
  EcunInfo info;
  memcpy(&info, data, sizeof(info)); // a copy to read with dots
  portENTER_CRITICAL(&ecuMux);
  memcpy(sourceName, info.source, sizeof(sourceName));
  sourceName[sizeof(sourceName) - 1] = 0;
  memcpy(ecuName, info.ecu, sizeof(ecuName));
  ecuName[sizeof(ecuName) - 1] = 0;
  theirLibraryId = info.libraryId;
  portEXIT_CRITICAL(&ecuMux);
}

void ecuNowOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len)
{
  if (!settings.ecuWireless || mac == NULL || len < (int)sizeof(EcunHdr))
  {
    return;
  }
  EcunHdr header;
  memcpy(&header, data, sizeof(header)); // a copy to read with dots
  if (header.magic[0] != ECUN_MAGIC0 || header.magic[1] != ECUN_MAGIC1 || header.proto != ECUN_PROTO)
  {
    return;
  }
  uint32_t now = millis();

  if (isZeroMac(settings.ecuMac))
  {
    memcpy(settings.ecuMac, mac, 6); // follow the first one heard, and remember it
    settingsDirty = true;
    DEBUG("ECU: following usb2ip %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  if (memcmp(mac, settings.ecuMac, 6) != 0)
  {
    noteOther(mac, rssi, data, len, header.type, now);
    return;
  }

  // the followed usb2ip
  portENTER_CRITICAL(&ecuMux);
  if (lastPacketMs != 0 && now - lastPacketMs < 5000)
  {
    uint8_t seqGap = header.seq - lastSeq - 1; // packets missing between this one and the last (wraps at 255)
    if (seqGap < 128)                          // a big jump backwards is a usb2ip restart, not loss
    {
      lostCount += seqGap;
    }
  }
  lastSeq = header.seq;
  lastPacketMs = now | 1;
  lastRssi = rssi;
  lastKind = header.kind;
  lastFw[0] = header.fw[0];
  lastFw[1] = header.fw[1];
  if (header.type == ECUN_T_DATA)
  {
    lastFlags = header.flags;
  }
  packetCount++;
  portEXIT_CRITICAL(&ecuMux);

  if (header.kind != ECUN_K_UNKNOWN && header.kind != settings.ecuKind)
  {
    settings.ecuKind = header.kind; // remembered, so the web UI lists this ECU's channels after a restart too
    settingsDirty = true;
  }

  if (header.type == ECUN_T_DATA && len >= (int)sizeof(EcunData))
  {
    handleData(data, now);
  }
  else if (header.type == ECUN_T_RAW)
  {
    handleRaw(data, len, header.kind, now);
  }
  else if (header.type == ECUN_T_INFO && len >= (int)sizeof(EcunInfo))
  {
    handleInfo(data);
  }
}

// ===== Public =====

void setupEcuNow()
{
  DEBUG("Setting up the ECU over WiFi (usb2ip)...");
  memset(others, 0, sizeof(others));
  memset(carWrote, 0, sizeof(carWrote));
  memset(rowMs, 0, ECU_ROWS_MAX * sizeof(uint32_t));
  storeKind = ECUN_K_UNKNOWN;
  sourceName[0] = 0;
  ecuName[0] = 0;
  for (uint8_t i = 0; i < ECUN_VALUES; i++)
  {
    lastValue[i] = NAN;
    lastValueUsed[i] = false;
  }
  ecuNowUserSigsChanged();
  DEBUG("Set up the ECU over WiFi!");
}

bool ecuNowWantsEspNow()
{
  return settings.ecuWireless != 0;
}

bool ecuNowHeard()
{
  uint32_t heardMs = lastPacketMs;
  return settings.ecuWireless && heardMs != 0 && millis() - heardMs < ECU_HEARD_MS;
}

uint8_t ecuNowKind()
{
  if (lastPacketMs == 0)
  {
    return ECUN_K_UNKNOWN;
  }
  return lastKind;
}

void ecuNowView(EcuNowView &view)
{
  memset(&view, 0, sizeof(view));
  uint32_t now = millis();

  portENTER_CRITICAL(&ecuMux);
  view.everHeard = lastPacketMs != 0;
  view.heard = settings.ecuWireless && view.everHeard && now - lastPacketMs < ECU_HEARD_MS;
  if (view.everHeard)
  {
    view.ageMs = now - lastPacketMs;
  }
  view.rssi = lastRssi;
  memcpy(view.mac, settings.ecuMac, 6);
  view.kind = lastKind;
  view.live = (lastFlags & ECUN_F_LIVE) != 0;
  view.fromPc = (lastFlags & ECUN_F_FROM_PC) != 0;
  view.fwMajor = lastFw[0];
  view.fwMinor = lastFw[1];
  memcpy(view.source, sourceName, sizeof(view.source));
  memcpy(view.ecu, ecuName, sizeof(view.ecu));
  view.theirLibraryId = theirLibraryId;
  view.packets = packetCount;
  view.lost = lostCount;
  for (uint8_t i = 0; i < ECUN_VALUES; i++)
  {
    view.value[i] = lastValue[i];
    view.valueUsed[i] = lastValueUsed[i];
  }
  for (uint8_t i = 0; i < ECU_OTHERS; i++)
  {
    if (others[i].heardMs != 0 && now - others[i].heardMs < 10000)
    {
      memcpy(view.otherMac[view.others], others[i].mac, 6);
      memcpy(view.otherName[view.others], others[i].name, sizeof(view.otherName[0]));
      view.otherRssi[view.others] = others[i].rssi;
      view.others++;
    }
  }
  portEXIT_CRITICAL(&ecuMux);

  if (!view.heard)
  {
    view.live = false; // nothing fresh: whatever it last said no longer holds
  }
  view.ourLibraryId = ecuNowLibraryId(view.kind);
  view.libraryMismatch = view.theirLibraryId != 0 && view.theirLibraryId != view.ourLibraryId;
  // channels with a fresh word (each time is one 32-bit read, so no lock is needed to count them)
  view.rowsTotal = ecuNowLibraryRows(view.kind);
  if (view.kind == storeKind)
  {
    for (uint16_t row = 0; row < view.rowsTotal; row++)
    {
      if (rowMs[row] != 0 && now - rowMs[row] < ECU_ROW_FRESH_MS)
      {
        view.rowsReceiving++;
      }
    }
  }

  uint8_t channel = 0;
  wifi_second_chan_t secondChannel;
  if (esp_wifi_get_channel(&channel, &secondChannel) == ESP_OK)
  {
    view.myChannel = channel;
  }
}

void ecuNowFollow(const uint8_t mac[6])
{
  portENTER_CRITICAL(&ecuMux);
  memcpy(settings.ecuMac, mac, 6);
  lastPacketMs = 0;
  lastFlags = 0;
  packetCount = 0;
  lostCount = 0;
  sourceName[0] = 0;
  ecuName[0] = 0;
  theirLibraryId = 0;
  for (uint8_t i = 0; i < ECUN_VALUES; i++)
  {
    lastValue[i] = NAN;
    lastValueUsed[i] = false;
  }
  for (uint8_t i = 0; i < ECU_OTHERS; i++)
  {
    if (memcmp(others[i].mac, mac, 6) == 0)
    {
      others[i].heardMs = 0; // it is the followed one now
    }
  }
  memset(rowMs, 0, ECU_ROWS_MAX * sizeof(uint32_t)); // the old one's channels no longer count
  storeKind = ECUN_K_UNKNOWN;
  portEXIT_CRITICAL(&ecuMux);
  settingsDirty = true;
}

const char *ecuNowKindName(uint8_t kind)
{
  switch (kind)
  {
  case ECUN_K_IGNITRON: return "Ignitron";
  case ECUN_K_MAXXECU: return "MaxxECU";
  case ECUN_K_LINK: return "Link";
  case ECUN_K_HALTECH: return "Haltech";
  case ECUN_K_EMU_PRO: return "EMU PRO";
  default: return "ECU";
  }
}

signalId_t ecuNowValueSignal(uint8_t valueIndex)
{
  for (uint8_t i = 0; i < ECUN_VALUES; i++)
  {
    if (valueMap[i].value == valueIndex)
    {
      return valueMap[i].signal;
    }
  }
  return SIG_NONE;
}

uint16_t ecuNowLibraryRows(uint8_t kind)
{
  uint16_t rows = 0;
  libraryFor(kind, rows);
  return rows;
}

uint16_t ecuNowLibraryId(uint8_t kind)
{
  if (kind == ECUN_K_IGNITRON)
  {
    return ECUN_LIB_IGNITRON_ID;
  }
  if (kind == ECUN_K_MAXXECU)
  {
    return ECUN_LIB_MAXXECU_ID;
  }
  if (kind == ECUN_K_LINK)
  {
    return ECUN_LIB_LINK_ID;
  }
  if (kind == ECUN_K_HALTECH)
  {
    return ECUN_LIB_HALTECH_ID;
  }
  if (kind == ECUN_K_EMU_PRO)
  {
    return ECUN_LIB_EMU_PRO_ID;
  }
  return 0;
}

uint8_t ecuNowGroupCount()
{
  return ECUN_LIB_GROUPS;
}

const char *ecuNowGroupName(uint8_t group)
{
  if (group >= ECUN_LIB_GROUPS)
  {
    return "";
  }
  return ECUN_LIB_GROUP_NAMES[group];
}

bool ecuNowRowInfo(uint8_t kind, uint16_t row, EcuChannelInfo &info)
{
  uint16_t rows = 0;
  const EcunLibRow *library = libraryFor(kind, rows);
  if (library == NULL || row >= rows)
  {
    return false;
  }
  const EcunLibRow &libraryRow = library[row];
  info.row = row;
  info.channel = libraryRow.channel;
  info.key = libraryRow.key;
  info.name = libraryRow.name;
  info.unit = libraryRow.unit;
  info.min = libraryRow.min;
  info.max = libraryRow.max;
  info.decimals = libraryRow.decimals;
  info.group = libraryRow.group;
  return true;
}

bool ecuNowChannelInfo(uint8_t kind, uint16_t channel, EcuChannelInfo &info)
{
  uint16_t rows = 0;
  const EcunLibRow *library = libraryFor(kind, rows);
  for (uint16_t row = 0; row < rows; row++)
  {
    if (library[row].channel == channel)
    {
      return ecuNowRowInfo(kind, row, info);
    }
  }
  return false;
}

bool ecuNowRowValue(uint8_t kind, uint16_t row, float &value)
{
  uint16_t rows = 0;
  const EcunLibRow *library = libraryFor(kind, rows);
  if (library == NULL || row >= rows)
  {
    return false;
  }
  portENTER_CRITICAL(&ecuMux);
  uint16_t raw = rowWords[row];
  uint32_t receivedMs = rowMs[row];
  bool isThisEcu = storeKind == kind;
  portEXIT_CRITICAL(&ecuMux);
  if (!isThisEcu)
  {
    return false; // the store holds another ECU's channels
  }
  if (receivedMs == 0 || millis() - receivedMs >= ECU_ROW_FRESH_MS)
  {
    return false;
  }
  value = decodeRaw(library[row], raw);
  return true;
}

void ecuNowGaugeUnit(const char *ecuUnit, char *unit, size_t unitSize)
{
  if (strcmp(ecuUnit, "\xC2\xB0"
                      "C") == 0 ||
      strcmp(ecuUnit, "degC") == 0)
  {
    copyStr(unit, unitSize, "C"); // shown as degree-sign C, and converted to F with the imperial option
  }
  else if (strcmp(ecuUnit, "\xC2\xB0"
                           "F") == 0)
  {
    copyStr(unit, unitSize, "F");
  }
  else if (strcmp(ecuUnit, "\xC2\xB0") == 0 || strcmp(ecuUnit, "degrees") == 0)
  {
    copyStr(unit, unitSize, "deg"); // shown as the degree sign
  }
  else if (strcasecmp(ecuUnit, "bar") == 0)
  {
    copyStr(unit, unitSize, "bar"); // "Bar" -> "bar", converted to psi with the imperial option
  }
  else if (strcasecmp(ecuUnit, "rpm") == 0)
  {
    copyStr(unit, unitSize, "rpm");
  }
  else
  {
    // plain letters are kept; the micro sign becomes "u" and any other special character is left out (not in the
    // gauge's fonts)
    size_t out = 0;
    for (size_t in = 0; ecuUnit[in] != 0 && out + 1 < unitSize; in++)
    {
      uint8_t letter = (uint8_t)ecuUnit[in];
      if (letter == 0xC2 && (uint8_t)ecuUnit[in + 1] == 0xB5) // UTF-8 micro sign
      {
        unit[out] = 'u';
        out++;
        in++;
      }
      else if (letter >= 0x20 && letter < 0x7F)
      {
        unit[out] = (char)letter;
        out++;
      }
    }
    unit[out] = 0;
  }
}

void ecuNowUserSigsChanged()
{
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL; userSigIndex++)
  {
    const UserSigCfg &userSig = userSigSettings(userSigIndex);
    int16_t row = -1;
    uint8_t kind = ECUN_K_UNKNOWN;
    if (userSig.source == USRC_ECU)
    {
      EcuChannelInfo info;
      if (ecuNowChannelInfo(userSig.ecuKind, userSig.ecuChannel, info))
      {
        row = info.row;
      }
      kind = userSig.ecuKind;
    }
    userSigRow[userSigIndex] = row; // the row first, then the kind the RAW handler checks
    userSigKind[userSigIndex] = kind;
  }
}

void ecuNowNote(uint8_t kind, uint16_t channel, char *buf, size_t len)
{
  EcuChannelInfo info;
  if (!settings.ecuWireless)
  {
    copyStr(buf, len, "ECU over WiFi is off - see the web UI");
  }
  else if (!ecuNowHeard())
  {
    copyStr(buf, len, "No usb2ip heard over WiFi");
  }
  else if (ecuNowKind() != kind)
  {
    snprintf(buf, len, "usb2ip sends %s data, this page wants %s", ecuNowKindName(ecuNowKind()), ecuNowKindName(kind));
  }
  else if (!ecuNowChannelInfo(kind, channel, info))
  {
    snprintf(buf, len, "Channel %u isn't in this firmware's %s list", channel, ecuNowKindName(kind));
  }
  else
  {
    uint16_t theirs = 0;
    portENTER_CRITICAL(&ecuMux);
    theirs = theirLibraryId;
    portEXIT_CRITICAL(&ecuMux);
    if (theirs != 0 && theirs != ecuNowLibraryId(kind))
    {
      copyStr(buf, len, "Channel list differs - update can2gauge or usb2ip");
    }
    else
    {
      snprintf(buf, len, "Waiting for %s", info.key);
    }
  }
}
