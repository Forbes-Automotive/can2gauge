#include <can2gauge_EEP.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_themes.h>
#include <can2gauge_Units.h> // 0.27.0: a unit per quantity - sanity check
#include <can2gauge_Nav.h>   // 0.30.0: NAV_POPUP_* (the nav link's pop-up distance)
#include <GaugeUI.h>
#include <Preferences.h> // NVS settings blob (ArduinoJson comes via can2gauge_EEP.h)
#include <can2gauge_Shared.h> // 0.30.4: the shared settings' revision is saved with the settings
#include <can2gauge_BootSync.h> // 0.30.14: how many gauges to wait for at power-up

EXT_RAM_BSS_ATTR Settings settings;
EXT_RAM_BSS_ATTR RuntimeStatus runtime;
SemaphoreHandle_t settingsMutex = NULL;
bool settingsDirty = false;
static Preferences prefs;
static uint32_t lastSave = 0;
// 0.17.4: one save at a time. A settings POST saves straight away while the house task's eepTick() saves the same
// change (settingsDirty); both used the one prefs, so one task's prefs.end() closed it under the other's putBytes()
// ("nvs_commit fail: cfg INVALID_HANDLE" on the bench) and that save was lost.
static SemaphoreHandle_t saveMutex = NULL;

// a gauge page as it comes out of the box: on, showing one catalogue signal with that signal's own range
static void defaultPage(GaugePageCfg &page, uint8_t style, signalId_t signalId, const char *label)
{
  const SignalInfo &info = signalInfo(signalId);
  page.enabled = 1;
  page.style = style;
  page.source = pageSourceOf(signalId); // the Source the web UI lists it under
  page.signal = signalId;
  page.signal2 = SIG_NONE;
  copyStr(page.label, NAME_LEN, label);
  page.minVal = info.defMin;
  page.maxVal = info.defMax;
  page.redline = info.defRedline;
  if (signalId == SIG_BOOST_REL || signalId == SIG_LAMBDA || signalId == SIG_BATTERY)
  {
    page.decimals = 2;
  }
  else
  {
    page.decimals = 0;
  }
  page.slots[0] = SIG_COOLANT;   // Custom centre stack: top
  page.slots[1] = SIG_BATTERY;   // middle
  page.slots[2] = SIG_OIL_PRESS; // bottom
  if (style == STYLE_QUAD)
  {
    // a Quad page shows its signal and the next three in the catalogue as values 2-4
    for (uint8_t slot = 0; slot < 3; slot++)
    {
      if (signalId + 1 + slot < SIG_USER_1)
      {
        page.slots[slot] = signalId + 1 + slot;
      }
      else
      {
        page.slots[slot] = SIG_NONE;
      }
    }
  }
  page.spriteOn = 0;   // every page starts as its gauge, not its sprite
  page.sprite[0] = '\0'; // "" = the first pack on the gauge
}

// one gearbox read that is switched off (svc 0)
static void dsgReadOff(DsgReadSpec &spec)
{
  memset(&spec, 0, sizeof(spec));
  spec.len = 1;
  spec.scale = 1.0f;
}

// every setting as a new gauge has it, in the order of the Settings struct (used on first run and by Factory Reset)
void setDefaults(Settings &target)
{
  memset(&target, 0, sizeof(target)); // anything not set below starts at 0 / "" / off
  target.magic = SETTINGS_MAGIC;
  target.version = SETTINGS_VERSION;

  // ===== Display =====
  target.brightness = 80;
  target.nightBrightness = 35;
  target.theme = THEME_TEAL;
  target.themeDial = 0xF2F4F8; // Custom starts as the common modern look: white scale, red needle
  target.themeNeedle = 0xFF3B3B;
  target.gaugeFeel = 0;        // analog
  target.swipeAnimMs = 800;    // 0.24.26: was 250 - slides are smooth enough now to be seen (Adam 2026-10-27)
  target.longPressMs = 2000;   // a short tap tickles a sprite page, so the menu wants a deliberate hold
  target.startPage = 0;
  target.upsideDown = 0;
  target.rotateTrim = 0;
  target.renderMode = 1;       // direct: partial's two strips take 77 KB of internal RAM that WiFi needs
  target.simdBlend = 0;        // C until the benchmark says otherwise

  // ===== Units =====
  target.units = UNITS_METRIC;
  target.unitSpeed = UNIT_METRIC;
  target.unitBoost = UNIT_METRIC;
  target.unitPressure = UNIT_METRIC;
  target.unitTemp = UNIT_METRIC;
  target.unitMixture = UNIT_METRIC;
  target.unitDistance = UNIT_METRIC;

  // ===== Boot logo =====
  target.bootLogo = 6; // the can2gauge logo
  copyStr(target.bootText, sizeof(target.bootText), "can2gauge");
  target.logoMake[0] = '\0'; // follow the theme / the VIN's make
  target.bootFadeMs = 2000;  // 0.25.3: was 1500

  // ===== Screensaver =====
  target.saverEnabled = 0; // the screensaver is opt-in
  target.saverDelayS = 30;
  target.saverSprite[0] = '\0'; // the first pack on the gauge
  target.saverReact = 1;        // docs/mascot/MASCOT.md: React to car data, on

  // ===== Power and ignition =====
  target.ignSource = IGN_SRC_OFF; // switched power
  target.ignCanMode = IGN_CAN_PQ;
  target.ignCanId = 0x570;
  target.ignCanBit = 1;
  target.ignCanInvert = 0;
  target.ignOffDelayS = 2;
  target.standbyMin = 10;
  // 0.30.2: Rev's shutdown / wake clips on. On switched power it never plays (the power is gone first), so it only
  // shows on a gauge with an ignition source - and Play It Now / the row wave work out of the box
  copyStr(target.shutdownSprite, sizeof(target.shutdownSprite), "rev");

  // ===== Output and dash lights =====
  target.outputMode = 0; // high-side
  for (int channel = 0; channel < IOX_CHANNELS; channel++)
  {
    target.outputManual[channel] = 2; // auto (triggers)
  }
  target.nightSource = 1; // the dash-light input dims the screen
  target.dimInvert = 0;

  // ===== Gauge pages =====
  defaultPage(target.pages[0], STYLE_HALDEX, SIG_HALDEX_ACTUAL, "HALDEX");
  defaultPage(target.pages[1], STYLE_ARC, SIG_BOOST_REL, "BOOST");
  defaultPage(target.pages[2], STYLE_NEEDLE, SIG_RPM, "RPM");
  defaultPage(target.pages[3], STYLE_ARC, SIG_COOLANT, "COOLANT");
  defaultPage(target.pages[4], STYLE_QUAD, SIG_COOLANT, "ENGINE"); // quad = 4 signals from this one on
  defaultPage(target.pages[5], STYLE_BAR, SIG_SPEED, "SPEED");
  target.pages[5].enabled = 0;
  defaultPage(target.pages[6], STYLE_NUMERIC, SIG_BATTERY, "BATTERY");
  target.pages[6].enabled = 0;
  defaultPage(target.pages[7], STYLE_ARC, SIG_OIL_TEMP, "OIL TEMP"); // 0.35.0: was ME7 1, which nothing fills any more (ME7 values come in as page values)
  target.pages[7].enabled = 0;

  // ===== Triggers ===== one example: coolant over 110 C -> alert
  target.triggers[0].enabled = 1;
  target.triggers[0].signal = SIG_COOLANT;
  target.triggers[0].above = 1;
  target.triggers[0].threshold = 110;
  target.triggers[0].hysteresis = 5;
  target.triggers[0].action = ACT_ALERT;
  target.triggers[0].latch = 0;
  target.triggers[0].noReading = 0; // a missing reading never fires it
  target.triggers[0].holdMs = 500;
  copyStr(target.triggers[0].label, NAME_LEN, "COOLANT HOT");

  // ===== Menu and web UI tabs =====
  target.menuMask = MENU_ALL;
  target.tabsShown = TAB_SHOW_ALL;

  // ===== CAN bus =====
  target.canEnabled = 1;
  target.canKbps = 500;
  target.canProfile = CAN_PROFILE_VAG_PQ;
  target.canTermination = CAN_TERM_AUTO; // stays off on a car's bus
  target.useCanRelay = 1;                // use relayed CAN frames when an OpenHaldex / adc2can sends them
  target.maxxBaseId = 0x520;             // MaxxECU's default CAN output
  // dbcSlotBase: 0 = as written in the DBC

  // ===== OBD-II on the gauge's CAN =====
  target.obdOverCan = 0; // off until First Boot or the CAN tab turns it on
  target.obdEcu = 0;     // the engine

  // ===== K-line =====
  target.klineProtocol = KL_OFF;
  target.klineEcuAddr = 0x01;
  target.klineModeAddr = 0x01; // engine
  target.klineBaud = 10400;
  target.klineFastObd = 0;     // Fast OBD off until it's been bench tested on the car

  // ===== ME7 logger =====
  copyStr(target.me7EcuFile, FILE_LEN, "/me7/ecu.ecu");
  copyStr(target.me7CfgFile, FILE_LEN, "/me7/log.cfg");
  target.me7SamplesPerSecond = 10;
  target.me7LogToFile = 0;
  target.me7FastMode = 0; // Normal until the owner arms Fast

  // ===== OBD adapter =====
  target.elmEnabled = 0;
  target.elmTransport = ELM_TR_BLE;
  target.elmPollObd = 1;
  target.elmFastObd = 1;      // 0.37.6: Fast OBD on (it drops back to standard timing by itself if answers go missing)
  copyStr(target.elmWifiHost, sizeof(target.elmWifiHost), "192.168.0.10");
  target.elmWifiPort = 35000; // the common WiFi ELM327 default (Vgate iCar WiFi, the "WiFi_OBDII" clones)
  target.elmExtraSource = ELM_EXTRA_OFF;
  target.elmKlAddr = 0x01;    // engine
  target.elmKlFast = 0;       // 5-baud init (what VAG K-line modules expect)

  // ===== Gearbox ===== passive broadcast only until the user picks a diagnostic source
  target.dsgGearbox = GEARBOX_DSG;
  target.dsgSource = DSG_SRC_CAN_ONLY;
  target.dsgUdsSession = 0x01;
  target.dsgSpeedPref = 0;
  target.dsgTxId = 0x7E1;
  target.dsgRxId = 0x7E9;
  target.dsgTxExtAddr = 0; // normal addressing
  target.dsgRxExtAddr = 0;
  target.dsgPollMs = 100;
  // gearbox speed: UDS 22 F40D = the OBD vehicle-speed PID mirrored as a DID (ISO 27145), 1 byte km/h
  dsgReadOff(target.dsgSpeed);
  target.dsgSpeed.svc = 0x22;
  target.dsgSpeed.id = 0xF40D;
  dsgReadOff(target.dsgGear);     // off: the gear comes from the CAN broadcast
  dsgReadOff(target.dsgTemp);     // off (MQB / MLB send it on the CAN)
  dsgReadOff(target.dsgLockup);   // off
  dsgReadOff(target.dsgSelector); // off (the CAN broadcast gives it)
  // 02E DQ250 (bench capture, docs/DSG_UDS_gap_report.md): block 1 field 4, formula 0x07 = speed in km/h
  target.dsgTp20Module = 0x02;
  target.dsgTp20Group = 1;
  target.dsgTp20SpeedField = 3;
  target.dsgTp20SpeedDiv = 0; // use the VAG formula byte
  target.dsgTp20GearGroup = 0;
  target.dsgTp20GearField = 0;

  // ===== VAG diag ===== no values set up yet
  target.vdiagRun = VDIAG_RUN_SHOWN;       // talks to the module only while a page in the rotation uses it (or the tab is open)
  target.vdiagAddr = 0x01;                 // the engine
  target.vdiagProtocol = VDIAG_PROTO_AUTO; // UDS, else TP2.0
  target.vdiagTurbo = 0;                   // normal mode until the owner tries Turbo
  target.vdiagGroupSize = 4;               // Turbo: 4 DIDs per request (a 9-byte request: two CAN frames)
  target.vdiagSession = 0x03;              // extended session, as the capture opens it
  target.vdiagTp20Module = 0;              // from the address table
  target.vdiagLink = VDIAG_LINK_CAN;       // 0.34.0: the gauge's own CAN (the OBD adapter is the other choice)
  target.vdiagTxExt = 0;                   // 0.34.0: no extended addressing (only BMW uses it)
  target.vdiagRxExt = 0;
  target.vdiagTxId = 0;                    // from the address table
  target.vdiagRxId = 0;                    // from the address table / worked out from the request id
  target.vdiagPollMs = 50;                 // normal mode: 50 ms after each answer
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    target.vdiagValues[i].scale = 1.0f; // every slot empty (VDIAG_KIND_NONE)
    target.vdiagValues[i].bitLen = 8;   // one byte
  }

  // ===== OEM oil =====
  target.oilSource = OIL_SRC_CAN; // passive until the user picks the K-line
  target.oilClusterAddr = 0x17;
  target.oilGroup = 0;            // auto from the cluster part number
  target.oilTempField = 2;        // 1J0-920 group 3: 1 coolant, 2 oil level, 3 oil temp (label file 1J0-920-xx0-17.lbl)
  target.oilLevelField = 1;
  target.oilLevelOkZero = 0;
  target.oilPollMs = 500;

  // ===== adc2can =====
  target.adcBaseId = 0x0AD2C000UL; // the adc2can default
  target.adcWireless = 1;          // adc2can over ESP-NOW: costs nothing, and a gauge with power only needs it
  for (int channel = 0; channel < ADC_SENSORS; channel++)
  {
    target.adc[channel].scale = 1.0f; // linear starts as plain volts
    target.adc[channel].decimals = 2;
  }

  // ===== OpenHaldex =====
  target.haldexBroadcastId = 0x6B0;
  target.haldexControlId = 0x6A0;
  target.ohxWireless = 1;   // listen for OpenHaldex over WiFi: costs nothing, and a gauge with power only needs it
  target.ohxCarSignals = 1; // speed / rpm / boost / pedal from it while the gauge has no other source
  target.ohxKey[0] = '\0';  // OpenHaldex ships with an open AP

  // ===== usb2ip =====
  target.ecuWireless = 1;   // listen for usb2ip over WiFi: costs nothing, and a gauge with power only needs it
  target.ecuCarSignals = 1; // its rpm / boost / coolant ... while the gauge has no other source for them

  // ===== Nav link =====
  target.navMode = NAV_MODE_OFF;
  target.navPopupM = NAV_POPUP_DEFAULT_M;

  // ===== Data logger ===== the engine values most people want to see on a pull, ten times a second, started by hand
  for (uint8_t channel = 0; channel < LOG_CHANNELS; channel++)
  {
    target.logSignals[channel] = SIG_NONE; // channel not used
  }
  target.logSignals[0] = SIG_RPM;      // engine speed
  target.logSignals[1] = SIG_SPEED;    // road speed
  target.logSignals[2] = SIG_BOOST;    // boost (bar)
  target.logSignals[3] = SIG_PEDAL;    // accelerator pedal
  target.logSignals[4] = SIG_THROTTLE; // throttle plate
  target.logSignals[5] = SIG_TIMING;   // ignition timing
  target.logSignals[6] = SIG_LAMBDA;   // lambda
  target.logSignals[7] = SIG_IAT;      // intake air temperature
  target.logSignals[8] = SIG_COOLANT;  // coolant temperature
  target.logRateHz = 10;               // ten samples a second
  target.logAutoStart = 0;             // started from the web UI's Log tab
  target.logManualChannels = 0;        // log the active gauge pages (the channels above are the fallback on the Log tab)

  // ===== Daisy chain =====
  target.chainEnabled = 1; // 0.25.2: on from first boot so a new gauge finds the others (nothing goes on the car's CAN until linkCanTx)
  target.chainRole = CHAIN_AUTO;
  target.nodeId = 0xFF;    // auto
  target.nodeName[0] = '\0';
  target.chainGroup = 0xC26A;
  target.linkTransport = LINK_AUTO;
  target.linkCanTx = 0;    // ESP-NOW only until the user allows frames on the (car's) CAN
  target.linkCanBaseId = 0x6C0;
  target.followPage = 0;
  target.followNight = 1;
  target.failover = 1;
  target.rowPosition = 0;  // not in a row: the normal goodbye

  // ===== WiFi =====
  target.wifiMode = 0;
  // the WiFi name is unique per gauge (last 4 hex of the MAC) so a chain of gauges doesn't show one name several times
  const uint64_t mac = ESP.getEfuseMac(); // byte 0 = first octet
  const uint8_t macByte4 = (mac >> 32) & 0xFF;
  const uint8_t macByte5 = (mac >> 40) & 0xFF;
  snprintf(target.apSsid, WIFI_LEN, "can2gauge-%02X%02X", macByte4, macByte5);
  // every Forbes Automotive product starts open, at 192.168.1.1, with a short .local name (2026-10-13)
  copyStr(target.apPassword, WIFI_LEN, ""); // no password until the owner sets one
  copyStr(target.hostname, NAME_LEN, "c2g"); // http://c2g.local

  // ===== Learnt by the gauge ===== nothing learnt yet
  target.themeMatched = THEME_NONE; // no VIN read
  target.ecuKind = 0;               // ECUN_K_UNKNOWN: no usb2ip heard yet
  // elmMac "" = bind the first adapter found; ohxMac / ecuMac all zero = follow the first one heard; runnerBest 0
}

// the Source a catalogue signal belongs under in the web UI (a default page, or picked as a catalogue signal)
uint8_t pageSourceOf(signalId_t id)
{
  if (id >= SIG_ADC_1 && id < SIG_ADC_1 + ADC_SENSORS) // 0.36.6: every adc2can sensor, however many boards
  {
    return PSRC_ADC;
  }
  switch (id)
  {
  case SIG_BOARD_TEMP:
  case SIG_UPTIME:
  case SIG_DASH_LIGHTS:
  case SIG_OUTPUT:
  case SIG_OUT_FAULT:
  case SIG_TCU_SPEED:
  case SIG_ME7_1:
  case SIG_ME7_2:
  case SIG_ME7_3:
  case SIG_ME7_4:
  case SIG_ME7_5:
  case SIG_ME7_6:
  case SIG_ME7_7:
  case SIG_ME7_8:
  case SIG_OIL_LEVEL:
    return PSRC_GAUGE;
  case SIG_KNOCK_RETARD: // only usb2ip sends these three so far
  case SIG_INJ_DUTY:
  case SIG_LAMBDA_TARGET:
    return PSRC_ECU;
  default:
    return PSRC_CAN;
  }
}

static const char *sourceKeys[pageSource_t_MAX] = {"can", "kline", "obd", "gauge", "adc", "ecu", "diag", "car"}; // 0.37.0: "car"

// the settings of user signal number `userIndex` (0..USER_SIGS_ALL-1): 0..15 = page main / second values (page =
// index / 2), 16..39 = page slots (page = (index - 16) / 3)
static UserSigCfg &userSigAt(Settings &target, uint8_t userIndex)
{
  if (userIndex < USER_SIGS)
  {
    return target.pages[userIndex / 2].valueSigs[userIndex % 2];
  }
  const uint8_t slotIndex = userIndex - USER_SIGS;
  return target.pages[slotIndex / 3].slotSigs[slotIndex % 3];
}

// a page value as the web UI writes it: a catalogue key ("coolant"), "dbc:<id>:<signal>", "kl:<addr>:<block>:<field 1-4>",
// "pid:<pid>", "ecu:<kind>:<channel>", "diag:<VAG diag value 1-32>" or "me7:<variable>"
static void pageSigToStr(Settings &stored, uint8_t id, char *buf, size_t len)
{
  if (id < SIG_USER_1 || id >= SIG_USER_1 + USER_SIGS_ALL)
  {
    copyStr(buf, len, signalInfo((signalId_t)id).key); // a catalogue signal
    return;
  }
  const UserSigCfg &userSig = userSigAt(stored, id - SIG_USER_1);
  switch (userSig.source)
  {
  case USRC_CAN_DBC: snprintf(buf, len, "dbc:%lu:%s", (unsigned long)userSig.canId, userSig.sigName); break;
  case USRC_KLINE: snprintf(buf, len, "kl:%u:%u:%u", userSig.klineAddr, userSig.klineGroup, userSig.klineField + 1); break;
  case USRC_OBD: snprintf(buf, len, "pid:%u", userSig.pid); break;
  case USRC_ECU: snprintf(buf, len, "ecu:%u:%u", userSig.ecuKind, userSig.ecuChannel); break;
  case USRC_DIAG: snprintf(buf, len, "diag:%u", userSig.diagValue); break;
  case USRC_ME7: snprintf(buf, len, "me7:%s", userSig.sigName); break; // 0.17.6: an ME7 logger variable
  default: copyStr(buf, len, "none"); break;
  }
}

// the other way round: a user-signal string goes into user signal `slot` (0..USER_SIGS_ALL-1: page * 2 + which for the main /
// second value, USER_SIGS + page * 3 + k for a slot) and the page points at SIG_USER_1 + slot
static uint8_t pageSigFromStr(Settings &target, uint8_t slot, const char *str)
{
  UserSigCfg &userSig = userSigAt(target, slot);
  if (!strncmp(str, "ecu:", 4)) // a channel of the ECU usb2ip reads: "ecu:<kind>:<channel>"
  {
    memset(&userSig, 0, sizeof(userSig));
    unsigned kind = 0;
    unsigned channel = 0;
    sscanf(str + 4, "%u:%u", &kind, &channel);
    userSig.ecuKind = (uint8_t)kind;
    userSig.ecuChannel = (uint16_t)channel;
    userSig.source = USRC_ECU;
    return SIG_USER_1 + slot;
  }
  if (!strncmp(str, "me7:", 4)) // 0.17.6: an ME7 logger variable by its .ecu name: "me7:nmot_w"
  {
    memset(&userSig, 0, sizeof(userSig));
    copyStr(userSig.sigName, sizeof(userSig.sigName), str + 4);
    if (!userSig.sigName[0])
    {
      return SIG_NONE; // no name: the slot stays free
    }
    userSig.source = USRC_ME7;
    return SIG_USER_1 + slot;
  }
  if (!strncmp(str, "diag:", 5)) // a VAG diag value: "diag:<1-32>" (0.37.0: was 1-12)
  {
    memset(&userSig, 0, sizeof(userSig));
    unsigned valueNumber = (unsigned)strtoul(str + 5, NULL, 10);
    if (valueNumber < 1 || valueNumber > VDIAG_VALUES)
    {
      return SIG_NONE; // not a value the tab has: the slot stays free
    }
    userSig.diagValue = (uint8_t)valueNumber;
    userSig.source = USRC_DIAG;
    return SIG_USER_1 + slot;
  }
  if (!strncmp(str, "dbc:", 4) || !strncmp(str, "kl:", 3) || !strncmp(str, "pid:", 4))
  {
    memset(&userSig, 0, sizeof(userSig));
    if (str[0] == 'd')
    {
      char *end = NULL;
      userSig.canId = strtoul(str + 4, &end, 10);
      if (end && *end == ':')
      {
        copyStr(userSig.sigName, sizeof(userSig.sigName), end + 1);
      }
      userSig.source = userSig.sigName[0] ? USRC_CAN_DBC : USRC_NONE;
    }
    else if (str[0] == 'k')
    {
      unsigned addr = 1;
      unsigned group = 1;
      unsigned field = 1;
      sscanf(str + 3, "%u:%u:%u", &addr, &group, &field);
      userSig.klineAddr = (uint8_t)addr;
      if (group < 1)
      {
        userSig.klineGroup = (uint8_t)1;
      }
      else if (group > 255)
      {
        userSig.klineGroup = (uint8_t)255;
      }
      else
      {
        userSig.klineGroup = (uint8_t)group;
      }
      if (field < 1)
      {
        userSig.klineField = (uint8_t)0;
      }
      else if (field > 4)
      {
        userSig.klineField = (uint8_t)3;
      }
      else
      {
        userSig.klineField = (uint8_t)(field - 1);
      }
      userSig.source = USRC_KLINE;
    }
    else
    {
      userSig.pid = (uint8_t)strtoul(str + 4, NULL, 10);
      userSig.source = USRC_OBD;
    }
    return SIG_USER_1 + slot;
  }
  memset(&userSig, 0, sizeof(userSig)); // a catalogue signal: the slot is free
  return signalFromKey(str);
}

// NVS -> settings. The saved blob is only used when it is this firmware's layout (same size, magic and
// SETTINGS_VERSION); anything else - a new gauge, or settings saved by an older layout - starts from the defaults.
void readEEP()
{
  if (!settingsMutex)
  {
    settingsMutex = xSemaphoreCreateMutex();
  }
  if (!saveMutex)
  {
    saveMutex = xSemaphoreCreateMutex(); // made here, in setup(), before any task can save
  }

  prefs.begin("can2gauge", false);
  const size_t len = prefs.getBytesLength("cfg");
  bool ok = false;
  if (len == sizeof(Settings))
  {
    prefs.getBytes("cfg", &settings, sizeof(Settings));
    ok = settings.magic == SETTINGS_MAGIC && settings.version == SETTINGS_VERSION;
  }
  if (!ok)
  {
    DEBUG("EEP: no settings of this layout (%u bytes stored) - defaults", (unsigned)len);
    setDefaults(settings);
    prefs.putBytes("cfg", &settings, sizeof(Settings));
  }
  // 0.38.0: the Drag Timer is new on the menu. A gauge set up before it has no bit for it in menuMask, so it's switched
  // on once here; "menuDrag" marks that done, so switched off later in the web UI it stays off.
  if (!prefs.isKey("menuDrag"))
  {
    settings.menuMask = settings.menuMask | (1UL << MENU_DRAG);
    prefs.putBytes("cfg", &settings, sizeof(Settings));
    prefs.putUChar("menuDrag", 1);
  }

  // kept under their own names next to the settings blob (see writeEEP)
  sharedSettingsRev = prefs.getUInt("sharedRev", 0); // 0.30.4: the shared settings' revision
  if (prefs.isKey("ignMac")) // 0.30.10: the gauge followed for the ignition (not saved yet = none)
  {
    prefs.getBytes("ignMac", ignitionGaugeMac, 6);
  }
  bootGaugeCount = prefs.getUChar("bootGauges", 0); // 0.30.14: gauges on CAN at the last power-up (0 = not known)
  prefs.end();
  DEBUG("EEP: settings loaded (%u bytes)", (unsigned)sizeof(Settings));
}

void writeEEP()
{
  if (saveMutex)
  {
    xSemaphoreTake(saveMutex, portMAX_DELAY); // wait for a save another task is doing
  }
  settingsDirty = false; // cleared before the save: a change made while it runs marks it dirty again
  prefs.begin("can2gauge", false);
  prefs.putBytes("cfg", &settings, sizeof(Settings));
  prefs.putUInt("sharedRev", sharedSettingsRev); // 0.30.4: the shared settings' revision (can2gauge_Shared.h)
  prefs.putBytes("ignMac", ignitionGaugeMac, 6);  // 0.30.10: the gauge followed for the ignition
  prefs.putUChar("bootGauges", bootGaugeCount);   // 0.30.14: gauges on CAN at power-up (the boot sync's handshake)
  prefs.end();
  lastSave = millis();
  if (saveMutex)
  {
    xSemaphoreGive(saveMutex);
  }
#if detailedDebugEEP
  DEBUG("EEP: saved");
#endif
}

void eepTick()
{
  if (settingsDirty && millis() - lastSave > eepRefresh)
  {
    writeEEP();
  }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
static const char *menuKeys[MENU_ITEM_COUNT] = {"haldex", "can", "kline", "output", "dsg", "ble", "chain", "gauges", "adc",
                                                "drag"}; // 0.38.0: + the drag timer

static void specToJson(JsonObject specObj, const DsgReadSpec &spec)
{
  specObj["svc"] = spec.svc;
  specObj["id"] = spec.id;
  specObj["pos"] = spec.pos;
  specObj["len"] = spec.len;
  specObj["fmt"] = spec.fmt;
  specObj["scale"] = spec.scale;
  specObj["offset"] = spec.offset;
}

// every setting as JSON for the web UI and the backup file, one group per section of the Settings struct
void settingsToJson(JsonDocument &doc)
{
  Settings &stored = settings;
  doc["version"] = CAN2GAUGE_VERSION;

  JsonObject displayObj = doc["display"].to<JsonObject>();
  displayObj["brightness"] = stored.brightness;
  displayObj["nightBrightness"] = stored.nightBrightness;
  displayObj["theme"] = stored.theme;
  char hex[8];
  snprintf(hex, sizeof(hex), "#%06lX", (unsigned long)(stored.themeDial & 0xFFFFFF));
  displayObj["themeDial"] = hex;
  snprintf(hex, sizeof(hex), "#%06lX", (unsigned long)(stored.themeNeedle & 0xFFFFFF));
  displayObj["themeNeedle"] = hex;
  displayObj["gaugeFeel"] = stored.gaugeFeel;
  displayObj["swipeAnimMs"] = stored.swipeAnimMs;
  displayObj["longPressMs"] = stored.longPressMs;
  displayObj["startPage"] = stored.startPage;
  displayObj["upsideDown"] = stored.upsideDown;        // 0.25.1: screen rotation
  displayObj["rotateTrim"] = stored.rotateTrim / 2.0f; // stored in half degrees, sent in degrees
  displayObj["renderMode"] = stored.renderMode;
  displayObj["simdBlend"] = stored.simdBlend;

  JsonObject unitsObj = doc["units"].to<JsonObject>(); // 0.27.0: a unit per quantity (src/can2gauge_Units.cpp)
  unitsObj["quick"] = stored.units;                    // 0 Metric, 1 Imperial, 2 Custom
  unitsObj["speed"] = stored.unitSpeed;
  unitsObj["boost"] = stored.unitBoost;
  unitsObj["pressure"] = stored.unitPressure;
  unitsObj["temp"] = stored.unitTemp;
  unitsObj["mixture"] = stored.unitMixture;
  unitsObj["distance"] = stored.unitDistance;

  JsonObject bootObj = doc["boot"].to<JsonObject>(); // the boot logo
  bootObj["logo"] = stored.bootLogo;
  bootObj["text"] = stored.bootText;
  bootObj["make"] = stored.logoMake;
  bootObj["fadeMs"] = stored.bootFadeMs;

  JsonObject saverObj = doc["saver"].to<JsonObject>(); // the screensaver
  saverObj["enabled"] = stored.saverEnabled;
  saverObj["delayS"] = stored.saverDelayS;
  saverObj["sprite"] = stored.saverSprite;
  saverObj["react"] = stored.saverReact;

  JsonObject powerObj = doc["power"].to<JsonObject>(); // ignition on permanent power (src/can2gauge_Power.cpp)
  powerObj["ignSource"] = stored.ignSource;
  powerObj["canMode"] = stored.ignCanMode;
  powerObj["canId"] = stored.ignCanId;
  powerObj["canBit"] = stored.ignCanBit;
  powerObj["canInvert"] = stored.ignCanInvert;
  powerObj["offDelayS"] = stored.ignOffDelayS;
  powerObj["standbyMin"] = stored.standbyMin;
  powerObj["shutdownSprite"] = stored.shutdownSprite;

  JsonObject ioObj = doc["io"].to<JsonObject>(); // the output and the dash-light input
  ioObj["outputMode"] = stored.outputMode;
  JsonArray outputsArr = ioObj["outputs"].to<JsonArray>();
  for (int channel = 0; channel < IOX_CHANNELS; channel++)
  {
    outputsArr.add(stored.outputManual[channel]);
  }
  ioObj["nightSource"] = stored.nightSource;
  ioObj["dimInvert"] = stored.dimInvert;

  JsonArray pagesArr = doc["pages"].to<JsonArray>();
  for (int pageIndex = 0; pageIndex < MAX_PAGES; pageIndex++)
  {
    const GaugePageCfg &page = stored.pages[pageIndex];
    JsonObject pageObj = pagesArr.add<JsonObject>();
    pageObj["enabled"] = page.enabled;
    pageObj["style"] = page.style;
    if (page.source < pageSource_t_MAX)
    {
      pageObj["source"] = sourceKeys[page.source];
    }
    else
    {
      pageObj["source"] = sourceKeys[PSRC_CAN];
    }
    char signalStr[64];
    pageSigToStr(stored, page.signal, signalStr, sizeof(signalStr));
    pageObj["signal"] = signalStr;
    pageSigToStr(stored, page.signal2, signalStr, sizeof(signalStr));
    pageObj["signal2"] = signalStr;
    pageObj["label"] = page.label;
    pageObj["min"] = page.minVal;
    pageObj["max"] = page.maxVal;
    pageObj["redline"] = page.redline;
    pageObj["decimals"] = page.decimals;
    pageObj["sprite"] = page.sprite;
    pageObj["spriteOn"] = page.spriteOn;
    JsonArray slotsArr = pageObj["slots"].to<JsonArray>(); // Quad values 2-4 / Custom centre stack: top, middle, bottom
    for (int slot = 0; slot < 3; slot++)
    {
      pageSigToStr(stored, page.slots[slot], signalStr, sizeof(signalStr));
      slotsArr.add(signalStr);
    }
  }

  JsonArray triggersArr = doc["triggers"].to<JsonArray>();
  for (int triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    const TriggerCfg &trigger = stored.triggers[triggerIndex];
    JsonObject triggerObj = triggersArr.add<JsonObject>();
    triggerObj["enabled"] = trigger.enabled;
    triggerObj["signal"] = signalInfo((signalId_t)trigger.signal).key;
    triggerObj["above"] = trigger.above;
    triggerObj["threshold"] = trigger.threshold;
    triggerObj["hysteresis"] = trigger.hysteresis;
    triggerObj["action"] = trigger.action;
    triggerObj["output"] = trigger.output;
    triggerObj["latch"] = trigger.latch;
    triggerObj["noReading"] = trigger.noReading; // 0.32.0
    triggerObj["holdMs"] = trigger.holdMs;
    triggerObj["label"] = trigger.label;
  }

  JsonObject menuObj = doc["menu"].to<JsonObject>(); // 1 = shown
  for (int menuItem = 0; menuItem < MENU_ITEM_COUNT; menuItem++)
  {
    menuObj[menuKeys[menuItem]] = (stored.menuMask >> menuItem) & 1;
  }
  // web UI tabs with no item on the screen's menu
  menuObj["oil"] = (stored.tabsShown & TAB_SHOW_OIL) ? 1 : 0;
  menuObj["ecu"] = (stored.tabsShown & TAB_SHOW_ECU) ? 1 : 0;
  menuObj["triggers"] = (stored.tabsShown & TAB_SHOW_TRIGGERS) ? 1 : 0;

  JsonObject canObj = doc["can"].to<JsonObject>();
  canObj["enabled"] = stored.canEnabled;
  canObj["kbps"] = stored.canKbps;
  canObj["profile"] = stored.canProfile;
  canObj["termination"] = stored.canTermination;
  canObj["relay"] = stored.useCanRelay; // CAN frames relayed over WiFi by an OpenHaldex / adc2can
  canObj["maxxBaseId"] = stored.maxxBaseId;
  JsonArray dbcBaseArr = canObj["dbcBase"].to<JsonArray>(); // 0.28.0: each DBC slot's base id (0 = as in the DBC)
  for (uint8_t slot = 0; slot < 3; slot++)
  {
    dbcBaseArr.add(stored.dbcSlotBase[slot]);
  }

  JsonObject obdObj = doc["obd"].to<JsonObject>(); // OBD-II on the gauge's own CAN
  obdObj["overCan"] = stored.obdOverCan;           // PIDs on 0x7DF next to the profile (the OBD-II profile always asks)
  obdObj["ecu"] = stored.obdEcu;                   // 0 = the engine, 1..8 = answer id 0x7E8..0x7EF

  JsonObject klineObj = doc["kline"].to<JsonObject>();
  klineObj["protocol"] = stored.klineProtocol;
  klineObj["ecuAddr"] = stored.klineEcuAddr;
  klineObj["modeAddr"] = stored.klineModeAddr;
  klineObj["baud"] = stored.klineBaud;
  klineObj["fastObd"] = stored.klineFastObd;

  JsonObject me7Obj = doc["me7"].to<JsonObject>(); // the ME7 logger
  me7Obj["ecuFile"] = stored.me7EcuFile;
  me7Obj["cfgFile"] = stored.me7CfgFile;
  me7Obj["sps"] = stored.me7SamplesPerSecond;
  me7Obj["logToFile"] = stored.me7LogToFile;
  me7Obj["fast"] = stored.me7FastMode; // 1 = Fast (B7 RAM-routine logging), 0 = Normal

  JsonObject elmObj = doc["elm"].to<JsonObject>(); // the OBD adapter (the paired one is under "learnt")
  elmObj["enabled"] = stored.elmEnabled;
  elmObj["transport"] = stored.elmTransport;
  elmObj["pollObd"] = stored.elmPollObd;
  elmObj["fastObd"] = stored.elmFastObd;
  elmObj["wifiSsid"] = stored.elmWifiSsid;
  elmObj["wifiPassSet"] = stored.elmWifiPass[0] != '\0'; // write-only, like the other passwords
  elmObj["wifiHost"] = stored.elmWifiHost;
  elmObj["wifiPort"] = stored.elmWifiPort;
  elmObj["extraSource"] = stored.elmExtraSource; // ELM_EXTRA_OFF / KLINE / CAN_MONITOR
  elmObj["klAddr"] = stored.elmKlAddr;
  elmObj["klFast"] = stored.elmKlFast;

  JsonObject dsgObj = doc["dsg"].to<JsonObject>();
  dsgObj["gearbox"] = stored.dsgGearbox; // 0.26.0: GEARBOX_DSG / ZF_VAG / ZF_BMW
  dsgObj["source"] = stored.dsgSource;
  dsgObj["session"] = stored.dsgUdsSession;
  dsgObj["speedPref"] = stored.dsgSpeedPref;
  dsgObj["txId"] = stored.dsgTxId;
  dsgObj["rxId"] = stored.dsgRxId;
  dsgObj["txExtAddr"] = stored.dsgTxExtAddr; // 0.26.0: BMW extended addressing (0 = off)
  dsgObj["rxExtAddr"] = stored.dsgRxExtAddr;
  dsgObj["pollMs"] = stored.dsgPollMs;
  specToJson(dsgObj["speed"].to<JsonObject>(), stored.dsgSpeed);
  specToJson(dsgObj["gear"].to<JsonObject>(), stored.dsgGear);
  specToJson(dsgObj["temp"].to<JsonObject>(), stored.dsgTemp);
  specToJson(dsgObj["lockup"].to<JsonObject>(), stored.dsgLockup);
  specToJson(dsgObj["selector"].to<JsonObject>(), stored.dsgSelector);
  dsgObj["tp20Module"] = stored.dsgTp20Module;
  dsgObj["tp20Group"] = stored.dsgTp20Group;
  dsgObj["tp20SpeedField"] = stored.dsgTp20SpeedField;
  dsgObj["tp20SpeedDiv"] = stored.dsgTp20SpeedDiv;
  dsgObj["tp20GearGroup"] = stored.dsgTp20GearGroup;
  dsgObj["tp20GearField"] = stored.dsgTp20GearField;

  JsonObject vdiagObj = doc["vdiag"].to<JsonObject>(); // VAG diag (0.13.0): UDS / TP2.0 measuring values from one module
  vdiagObj["run"] = stored.vdiagRun;
  vdiagObj["addr"] = stored.vdiagAddr;
  vdiagObj["protocol"] = stored.vdiagProtocol;
  vdiagObj["turbo"] = stored.vdiagTurbo;
  vdiagObj["groupSize"] = stored.vdiagGroupSize;
  vdiagObj["session"] = stored.vdiagSession;
  vdiagObj["tp20Module"] = stored.vdiagTp20Module;
  vdiagObj["txId"] = stored.vdiagTxId;   // 11-bit or 29-bit (0.34.0)
  vdiagObj["rxId"] = stored.vdiagRxId;
  vdiagObj["link"] = stored.vdiagLink;   // 0.34.0: 0 = the gauge's CAN, 1 = the OBD adapter
  vdiagObj["txExt"] = stored.vdiagTxExt; // 0.34.0: extended address bytes (BMW), 0 = none
  vdiagObj["rxExt"] = stored.vdiagRxExt;
  vdiagObj["pollMs"] = stored.vdiagPollMs;
  JsonArray vdiagValuesArr = vdiagObj["values"].to<JsonArray>();
  for (int valueIndex = 0; valueIndex < VDIAG_VALUES; valueIndex++)
  {
    const VdiagValue &value = stored.vdiagValues[valueIndex];
    JsonObject valueObj = vdiagValuesArr.add<JsonObject>();
    valueObj["kind"] = value.kind;
    valueObj["id"] = value.id;
    valueObj["field"] = value.field;
    valueObj["bit"] = value.bitPos;   // 0.34.0: first bit in the DID's data (byte n = bit n x 8)
    valueObj["bits"] = value.bitLen;  // 0.34.0: how many bits long
    valueObj["signed"] = (value.flags & VDIAG_FLAG_SIGNED) ? 1 : 0;
    valueObj["lsb"] = (value.flags & VDIAG_FLAG_LSB_FIRST) ? 1 : 0; // 0.34.0: least significant byte first
    valueObj["scale"] = value.scale;
    valueObj["offset"] = value.offset;
    valueObj["name"] = value.name;
    valueObj["unit"] = value.unit;
    // 0.37.0: the value's own module (all 0 = the tab's module)
    valueObj["tx"] = value.txId;
    valueObj["rx"] = value.rxId;
    valueObj["txExt"] = value.txExt;
    valueObj["rxExt"] = value.rxExt;
    valueObj["addr"] = value.addr;
    valueObj["proto"] = value.protocol;
  }

  JsonObject oilObj = doc["oil"].to<JsonObject>();
  oilObj["source"] = stored.oilSource;
  oilObj["clusterAddr"] = stored.oilClusterAddr;
  oilObj["group"] = stored.oilGroup;
  oilObj["tempField"] = stored.oilTempField;
  oilObj["levelField"] = stored.oilLevelField;
  oilObj["levelOkZero"] = stored.oilLevelOkZero;
  oilObj["pollMs"] = stored.oilPollMs;

  JsonObject adcObj = doc["adc"].to<JsonObject>(); // adc2can channels (web UI > ADC)
  adcObj["baseId"] = stored.adcBaseId;
  adcObj["wireless"] = stored.adcWireless;
  JsonArray channelsArr = adcObj["ch"].to<JsonArray>();
  for (int channel = 0; channel < ADC_SENSORS; channel++)
  {
    JsonObject channelObj = channelsArr.add<JsonObject>();
    channelObj["mode"] = stored.adc[channel].mode;
    channelObj["decimals"] = stored.adc[channel].decimals;
    channelObj["scale"] = stored.adc[channel].scale;
    channelObj["offset"] = stored.adc[channel].offset;
    channelObj["name"] = stored.adc[channel].name;
    channelObj["unit"] = stored.adc[channel].unit;
  }

  JsonObject haldexObj = doc["haldex"].to<JsonObject>(); // OpenHaldex, on CAN and over WiFi
  haldexObj["broadcastId"] = stored.haldexBroadcastId;
  haldexObj["controlId"] = stored.haldexControlId;
  haldexObj["wireless"] = stored.ohxWireless;
  haldexObj["carSignals"] = stored.ohxCarSignals;
  haldexObj["keySet"] = stored.ohxKey[0] != '\0'; // the key itself is write-only

  // an aftermarket ECU over WiFi from usb2ip (the followed one is set by the ecuFollow action, like the OpenHaldex's)
  JsonObject ecuObj = doc["ecu"].to<JsonObject>();
  ecuObj["wireless"] = stored.ecuWireless;
  ecuObj["carSignals"] = stored.ecuCarSignals;

  // 0.30.0: nav link (Google Maps turns from the can2gauge Link phone app, src/can2gauge_Nav.cpp)
  JsonObject navObj = doc["nav"].to<JsonObject>();
  navObj["mode"] = stored.navMode;     // 0 off, 1 pop up near a turn, 2 whole route
  navObj["popupM"] = stored.navPopupM; // the pop-up distance in metres

  JsonObject logObj = doc["log"].to<JsonObject>(); // the data logger (0.16.0)
  JsonArray logSignalsArr = logObj["signals"].to<JsonArray>(); // one signal key per channel, "none" = not used
  for (uint8_t channel = 0; channel < LOG_CHANNELS; channel++)
  {
    logSignalsArr.add(signalInfo((signalId_t)stored.logSignals[channel]).key);
  }
  logObj["rateHz"] = stored.logRateHz;
  logObj["autoStart"] = stored.logAutoStart;
  logObj["manualChannels"] = stored.logManualChannels; // 0 = the active gauges, 1 = the channels above

  JsonObject chainObj = doc["chain"].to<JsonObject>();
  chainObj["enabled"] = stored.chainEnabled;
  chainObj["role"] = stored.chainRole;
  chainObj["nodeId"] = stored.nodeId;
  chainObj["nodeName"] = stored.nodeName;
  chainObj["group"] = stored.chainGroup;
  chainObj["transport"] = stored.linkTransport;
  chainObj["canTx"] = stored.linkCanTx;
  chainObj["canBaseId"] = stored.linkCanBaseId;
  chainObj["followPage"] = stored.followPage;
  chainObj["followNight"] = stored.followNight;
  chainObj["failover"] = stored.failover;
  chainObj["rowPosition"] = stored.rowPosition;

  JsonObject wifiObj = doc["wifi"].to<JsonObject>();
  wifiObj["mode"] = stored.wifiMode;
  wifiObj["apSsid"] = stored.apSsid;
  wifiObj["apPassword"] = stored.apPassword;
  wifiObj["staSsid"] = stored.staSsid;
  wifiObj["staPasswordSet"] = stored.staPassword[0] != '\0';
  wifiObj["hostname"] = stored.hostname;

  JsonObject learntObj = doc["learnt"].to<JsonObject>(); // worked out / paired by the gauge itself
  learntObj["themeMatched"] = stored.themeMatched; // read-only: set from the VIN
  learntObj["matchedLogo"] = stored.matchedLogo;   // read-only
  learntObj["ecuKind"] = stored.ecuKind;           // read-only: the ECU the last usb2ip heard reads (1 Ignitron, 2 MaxxECU, 0 none yet)
  learntObj["elmMac"] = stored.elmMac;             // the paired OBD adapter ("" = the first found)
  learntObj["elmAddrType"] = stored.elmAddrType;
  learntObj["elmProtocol"] = stored.elmProtocol;
  learntObj["runnerBest"] = stored.runnerBest;     // read-only (reset: action runnerReset)
}

#define SET_U8(obj, key, field)          \
  if (obj[key].is<int>())                \
  {                                      \
    field = (uint8_t)obj[key].as<int>(); \
    changed = true;                      \
  }
#define SET_U16(obj, key, field)          \
  if (obj[key].is<int>())                 \
  {                                       \
    field = (uint16_t)obj[key].as<int>(); \
    changed = true;                       \
  }
#define SET_F(obj, key, field)                    \
  if (obj[key].is<float>() || obj[key].is<int>()) \
  {                                               \
    field = obj[key].as<float>();                 \
    changed = true;                               \
  }
#define SET_STR(obj, key, field, len)                 \
  if (obj[key].is<const char *>())                    \
  {                                                   \
    copyStr(field, len, obj[key].as<const char *>()); \
    changed = true;                                   \
  }

// one web UI tab switch from the "menu" JSON (1 / true = shown) into settings.tabsShown (a set bit = shown)
static void tabFromJson(JsonObjectConst menuObj, const char *key, uint8_t showBit, uint8_t &tabsShown, bool &changed)
{
  if (!menuObj[key].is<int>() && !menuObj[key].is<bool>())
  {
    return; // not in this document: leave it
  }
  bool shown = false;
  if (menuObj[key].is<bool>())
  {
    shown = menuObj[key].as<bool>();
  }
  else
  {
    shown = menuObj[key].as<int>() != 0;
  }
  if (shown)
  {
    tabsShown = tabsShown | showBit;
  }
  else
  {
    tabsShown = tabsShown & ~showBit;
  }
  changed = true;
}

static bool specFromJson(JsonObjectConst specObj, DsgReadSpec &spec)
{
  if (specObj.isNull())
  {
    return false;
  }
  bool changed = false;
  SET_U8(specObj, "svc", spec.svc);
  SET_U16(specObj, "id", spec.id);
  SET_U8(specObj, "pos", spec.pos);
  SET_U8(specObj, "len", spec.len);
  SET_U8(specObj, "fmt", spec.fmt);
  SET_F(specObj, "scale", spec.scale);
  SET_F(specObj, "offset", spec.offset);
  return changed;
}

// JSON from the web UI (or a backup file) into the settings: only the groups and keys present are changed, so a
// partial document is fine. Then every value is brought back into its range.
bool settingsFromJson(const JsonDocument &doc)
{
  Settings &target = settings;
  bool changed = false;

  JsonObjectConst displayObj = doc["display"];
  if (!displayObj.isNull())
  {
    SET_U8(displayObj, "brightness", target.brightness);
    SET_U8(displayObj, "nightBrightness", target.nightBrightness);
    SET_U8(displayObj, "theme", target.theme);
    if (displayObj["themeDial"].is<const char *>())
    {
      target.themeDial = themeParseHex(displayObj["themeDial"], target.themeDial);
      changed = true;
    }
    if (displayObj["themeNeedle"].is<const char *>())
    {
      target.themeNeedle = themeParseHex(displayObj["themeNeedle"], target.themeNeedle);
      changed = true;
    }
    SET_U8(displayObj, "gaugeFeel", target.gaugeFeel);
    SET_U16(displayObj, "swipeAnimMs", target.swipeAnimMs);
    SET_U16(displayObj, "longPressMs", target.longPressMs);
    SET_U8(displayObj, "startPage", target.startPage);
    SET_U8(displayObj, "upsideDown", target.upsideDown); // 0.25.1: screen rotation
    if (displayObj["rotateTrim"].is<float>() || displayObj["rotateTrim"].is<int>())
    {
      // sent in degrees, kept in half degrees (rounded to the nearest half)
      long halfDegrees = lroundf(displayObj["rotateTrim"].as<float>() * 2.0f);
      if (halfDegrees > 30)
      {
        halfDegrees = 30; // +15 degrees at most
      }
      if (halfDegrees < -30)
      {
        halfDegrees = -30; // -15 degrees at most
      }
      target.rotateTrim = (int8_t)halfDegrees;
      changed = true;
    }
    SET_U8(displayObj, "renderMode", target.renderMode);
    SET_U8(displayObj, "simdBlend", target.simdBlend);
  }

  JsonObjectConst unitsObj = doc["units"]; // 0.27.0 (the API turns the pages' ranges over after a change)
  if (!unitsObj.isNull())
  {
    SET_U8(unitsObj, "quick", target.units);
    SET_U8(unitsObj, "speed", target.unitSpeed);
    SET_U8(unitsObj, "boost", target.unitBoost);
    SET_U8(unitsObj, "pressure", target.unitPressure);
    SET_U8(unitsObj, "temp", target.unitTemp);
    SET_U8(unitsObj, "mixture", target.unitMixture);
    SET_U8(unitsObj, "distance", target.unitDistance);
  }

  JsonObjectConst bootObj = doc["boot"];
  if (!bootObj.isNull())
  {
    SET_U8(bootObj, "logo", target.bootLogo);
    SET_STR(bootObj, "text", target.bootText, sizeof(target.bootText));
    SET_STR(bootObj, "make", target.logoMake, sizeof(target.logoMake));
    SET_U16(bootObj, "fadeMs", target.bootFadeMs);
  }

  JsonObjectConst saverObj = doc["saver"];
  if (!saverObj.isNull())
  {
    SET_U8(saverObj, "enabled", target.saverEnabled);
    SET_U16(saverObj, "delayS", target.saverDelayS);
    SET_STR(saverObj, "sprite", target.saverSprite, SPRITE_NAME_LEN);
    SET_U8(saverObj, "react", target.saverReact);
  }

  JsonObjectConst powerObj = doc["power"];
  if (!powerObj.isNull())
  {
    SET_U8(powerObj, "ignSource", target.ignSource);
    SET_U8(powerObj, "canMode", target.ignCanMode);
    if (powerObj["canId"].is<uint32_t>())
    {
      target.ignCanId = powerObj["canId"].as<uint32_t>();
      changed = true;
    }
    SET_U8(powerObj, "canBit", target.ignCanBit);
    SET_U8(powerObj, "canInvert", target.ignCanInvert);
    SET_U8(powerObj, "offDelayS", target.ignOffDelayS);
    SET_U8(powerObj, "standbyMin", target.standbyMin);
    SET_STR(powerObj, "shutdownSprite", target.shutdownSprite, SPRITE_NAME_LEN);
  }

  JsonObjectConst ioObj = doc["io"];
  if (!ioObj.isNull())
  {
    if (ioObj["outputMode"].is<int>())
    {
      if (ioObj["outputMode"].as<int>())
      {
        target.outputMode = 1; // low-side
      }
      else
      {
        target.outputMode = 0; // high-side
      }
      changed = true;
    }
    JsonArrayConst outputsArr = ioObj["outputs"];
    for (size_t channel = 0; channel < outputsArr.size() && channel < IOX_CHANNELS; channel++)
    {
      if (!outputsArr[channel].is<int>())
      {
        continue; // like every other field: a missing / non-number entry leaves the channel alone
      }
      target.outputManual[channel] = outputsArr[channel].as<int>();
      changed = true;
    }
    SET_U8(ioObj, "nightSource", target.nightSource);
    SET_U8(ioObj, "dimInvert", target.dimInvert);
  }

  JsonArrayConst pagesArr = doc["pages"];
  for (size_t pageIndex = 0; pageIndex < pagesArr.size() && pageIndex < MAX_PAGES; pageIndex++)
  {
    JsonObjectConst pageObj = pagesArr[pageIndex];
    GaugePageCfg &page = target.pages[pageIndex];
    SET_U8(pageObj, "enabled", page.enabled);
    SET_U8(pageObj, "style", page.style);
    if (pageObj["source"].is<const char *>())
    {
      for (uint8_t sourceIndex = 0; sourceIndex < pageSource_t_MAX; sourceIndex++)
      {
        if (!strcmp(pageObj["source"], sourceKeys[sourceIndex]))
        {
          page.source = sourceIndex;
          changed = true;
        }
      }
    }
    if (pageObj["signal"].is<const char *>())
    {
      page.signal = pageSigFromStr(target, pageIndex * 2, pageObj["signal"]);
      changed = true;
    }
    if (pageObj["signal2"].is<const char *>())
    {
      page.signal2 = pageSigFromStr(target, pageIndex * 2 + 1, pageObj["signal2"]);
      changed = true;
    }
    SET_STR(pageObj, "label", page.label, NAME_LEN);
    SET_F(pageObj, "min", page.minVal);
    SET_F(pageObj, "max", page.maxVal);
    SET_F(pageObj, "redline", page.redline);
    SET_U8(pageObj, "decimals", page.decimals);
    SET_STR(pageObj, "sprite", page.sprite, SPRITE_NAME_LEN);
    SET_U8(pageObj, "spriteOn", page.spriteOn);
    JsonArrayConst slotsArr = pageObj["slots"];
    for (size_t slot = 0; slot < slotsArr.size() && slot < 3; slot++)
    {
      if (slotsArr[slot].is<const char *>())
      {
        page.slots[slot] = pageSigFromStr(target, USER_SIGS + pageIndex * 3 + slot, slotsArr[slot]); // "none" = empty slot
        changed = true;
      }
    }
  }

  JsonArrayConst triggersArr = doc["triggers"];
  for (size_t triggerIndex = 0; triggerIndex < triggersArr.size() && triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    JsonObjectConst triggerObj = triggersArr[triggerIndex];
    TriggerCfg &trigger = target.triggers[triggerIndex];
    SET_U8(triggerObj, "enabled", trigger.enabled);
    if (triggerObj["signal"].is<const char *>())
    {
      trigger.signal = signalFromKey(triggerObj["signal"]);
      changed = true;
    }
    SET_U8(triggerObj, "above", trigger.above);
    SET_F(triggerObj, "threshold", trigger.threshold);
    SET_F(triggerObj, "hysteresis", trigger.hysteresis);
    SET_U8(triggerObj, "action", trigger.action);
    SET_U8(triggerObj, "output", trigger.output);
    SET_U8(triggerObj, "latch", trigger.latch);
    SET_U8(triggerObj, "noReading", trigger.noReading); // 0.32.0: 0 ignore, 1 trigger
    SET_U16(triggerObj, "holdMs", trigger.holdMs);
    SET_STR(triggerObj, "label", trigger.label, NAME_LEN);
  }

  JsonObjectConst menuObj = doc["menu"];
  if (!menuObj.isNull())
  {
    for (int menuItem = 0; menuItem < MENU_ITEM_COUNT; menuItem++)
    {
      JsonVariantConst itemValue = menuObj[menuKeys[menuItem]];
      if (!itemValue.is<int>() && !itemValue.is<bool>())
      {
        continue; // not in this document: leave it
      }
      bool shown = false;
      if (itemValue.is<bool>())
      {
        shown = itemValue.as<bool>();
      }
      else
      {
        shown = itemValue.as<int>() != 0;
      }
      if (shown)
      {
        target.menuMask = target.menuMask | (1UL << menuItem);
      }
      else
      {
        target.menuMask = target.menuMask & ~(1UL << menuItem);
      }
      changed = true;
    }
    // the web UI tabs that aren't on the screen's menu
    tabFromJson(menuObj, "oil", TAB_SHOW_OIL, target.tabsShown, changed);
    tabFromJson(menuObj, "ecu", TAB_SHOW_ECU, target.tabsShown, changed);
    tabFromJson(menuObj, "triggers", TAB_SHOW_TRIGGERS, target.tabsShown, changed);
  }

  JsonObjectConst canObj = doc["can"];
  if (!canObj.isNull())
  {
    SET_U8(canObj, "enabled", target.canEnabled);
    SET_U16(canObj, "kbps", target.canKbps);
    SET_U8(canObj, "profile", target.canProfile);
    SET_U8(canObj, "termination", target.canTermination);
    SET_U8(canObj, "relay", target.useCanRelay);
    SET_U16(canObj, "maxxBaseId", target.maxxBaseId);
    JsonArrayConst dbcBaseArr = canObj["dbcBase"]; // 0.28.0: each DBC slot's base id
    for (uint8_t slot = 0; slot < 3 && slot < dbcBaseArr.size(); slot++)
    {
      const uint16_t base = dbcBaseArr[slot].as<uint16_t>();
      if (base != target.dbcSlotBase[slot])
      {
        target.dbcSlotBase[slot] = base;
        changed = true;
      }
    }
  }

  JsonObjectConst obdObj = doc["obd"];
  if (!obdObj.isNull())
  {
    SET_U8(obdObj, "overCan", target.obdOverCan);
    SET_U8(obdObj, "ecu", target.obdEcu);
  }

  JsonObjectConst klineObj = doc["kline"];
  if (!klineObj.isNull())
  {
    SET_U8(klineObj, "protocol", target.klineProtocol);
    SET_U8(klineObj, "ecuAddr", target.klineEcuAddr);
    SET_U8(klineObj, "modeAddr", target.klineModeAddr);
    SET_U16(klineObj, "baud", target.klineBaud);
    SET_U8(klineObj, "fastObd", target.klineFastObd);
  }

  JsonObjectConst me7Obj = doc["me7"];
  if (!me7Obj.isNull())
  {
    SET_STR(me7Obj, "ecuFile", target.me7EcuFile, FILE_LEN);
    SET_STR(me7Obj, "cfgFile", target.me7CfgFile, FILE_LEN);
    SET_U8(me7Obj, "sps", target.me7SamplesPerSecond);
    SET_U8(me7Obj, "logToFile", target.me7LogToFile);
    SET_U8(me7Obj, "fast", target.me7FastMode);
  }

  JsonObjectConst elmObj = doc["elm"];
  if (!elmObj.isNull())
  {
    SET_U8(elmObj, "enabled", target.elmEnabled);
    SET_U8(elmObj, "transport", target.elmTransport);
    SET_U8(elmObj, "pollObd", target.elmPollObd);
    SET_U8(elmObj, "fastObd", target.elmFastObd);
    SET_STR(elmObj, "wifiSsid", target.elmWifiSsid, sizeof(target.elmWifiSsid));
    SET_STR(elmObj, "wifiPass", target.elmWifiPass, sizeof(target.elmWifiPass));
    SET_STR(elmObj, "wifiHost", target.elmWifiHost, sizeof(target.elmWifiHost));
    SET_U16(elmObj, "wifiPort", target.elmWifiPort);
    SET_U8(elmObj, "extraSource", target.elmExtraSource);
    SET_U8(elmObj, "klAddr", target.elmKlAddr);
    SET_U8(elmObj, "klFast", target.elmKlFast);
  }

  JsonObjectConst dsgObj = doc["dsg"];
  if (!dsgObj.isNull())
  {
    SET_U8(dsgObj, "gearbox", target.dsgGearbox); // 0.26.0
    SET_U8(dsgObj, "source", target.dsgSource);
    SET_U8(dsgObj, "session", target.dsgUdsSession);
    SET_U8(dsgObj, "speedPref", target.dsgSpeedPref);
    SET_U16(dsgObj, "txId", target.dsgTxId);
    SET_U16(dsgObj, "rxId", target.dsgRxId);
    SET_U8(dsgObj, "txExtAddr", target.dsgTxExtAddr);
    SET_U8(dsgObj, "rxExtAddr", target.dsgRxExtAddr);
    SET_U16(dsgObj, "pollMs", target.dsgPollMs);
    changed |= specFromJson(dsgObj["speed"], target.dsgSpeed);
    changed |= specFromJson(dsgObj["gear"], target.dsgGear);
    changed |= specFromJson(dsgObj["temp"], target.dsgTemp);
    changed |= specFromJson(dsgObj["lockup"], target.dsgLockup);
    changed |= specFromJson(dsgObj["selector"], target.dsgSelector);
    SET_U8(dsgObj, "tp20Module", target.dsgTp20Module);
    SET_U8(dsgObj, "tp20Group", target.dsgTp20Group);
    SET_U8(dsgObj, "tp20SpeedField", target.dsgTp20SpeedField);
    SET_F(dsgObj, "tp20SpeedDiv", target.dsgTp20SpeedDiv);
    SET_U8(dsgObj, "tp20GearGroup", target.dsgTp20GearGroup);
    SET_U8(dsgObj, "tp20GearField", target.dsgTp20GearField);
  }

  JsonObjectConst vdiagObj = doc["vdiag"];
  if (!vdiagObj.isNull())
  {
    SET_U8(vdiagObj, "run", target.vdiagRun);
    SET_U8(vdiagObj, "addr", target.vdiagAddr);
    SET_U8(vdiagObj, "protocol", target.vdiagProtocol);
    SET_U8(vdiagObj, "turbo", target.vdiagTurbo);
    SET_U8(vdiagObj, "groupSize", target.vdiagGroupSize);
    SET_U8(vdiagObj, "session", target.vdiagSession);
    SET_U8(vdiagObj, "tp20Module", target.vdiagTp20Module);
    // 0.34.0: the ids are 29-bit capable (the SET_ helpers stop at 16 bits)
    if (vdiagObj["txId"].is<uint32_t>())
    {
      target.vdiagTxId = vdiagObj["txId"].as<uint32_t>();
      changed = true;
    }
    if (vdiagObj["rxId"].is<uint32_t>())
    {
      target.vdiagRxId = vdiagObj["rxId"].as<uint32_t>();
      changed = true;
    }
    SET_U8(vdiagObj, "link", target.vdiagLink);
    SET_U8(vdiagObj, "txExt", target.vdiagTxExt);
    SET_U8(vdiagObj, "rxExt", target.vdiagRxExt);
    SET_U16(vdiagObj, "pollMs", target.vdiagPollMs);
    JsonArrayConst vdiagValuesArr = vdiagObj["values"];
    for (size_t valueIndex = 0; valueIndex < vdiagValuesArr.size() && valueIndex < VDIAG_VALUES; valueIndex++)
    {
      JsonObjectConst valueObj = vdiagValuesArr[valueIndex];
      VdiagValue &value = target.vdiagValues[valueIndex];
      SET_U8(valueObj, "kind", value.kind);
      SET_U16(valueObj, "id", value.id);
      SET_U8(valueObj, "field", value.field);
      // a backup from before 0.34.0 has the value as whole bytes: first byte "pos", "len" bytes
      if (valueObj["pos"].is<int>() && !valueObj["bit"].is<int>())
      {
        value.bitPos = (uint16_t)(valueObj["pos"].as<int>() * 8);
        changed = true;
      }
      if (valueObj["len"].is<int>() && !valueObj["bits"].is<int>())
      {
        value.bitLen = (uint8_t)(valueObj["len"].as<int>() * 8);
        changed = true;
      }
      SET_U16(valueObj, "bit", value.bitPos);
      SET_U8(valueObj, "bits", value.bitLen);
      if (valueObj["signed"].is<int>())
      {
        value.flags = value.flags & ~VDIAG_FLAG_SIGNED; // clear the signed bit, then set it if asked
        if (valueObj["signed"].as<int>())
        {
          value.flags = value.flags | VDIAG_FLAG_SIGNED;
        }
        changed = true;
      }
      if (valueObj["lsb"].is<int>())
      {
        value.flags = value.flags & ~VDIAG_FLAG_LSB_FIRST;
        if (valueObj["lsb"].as<int>())
        {
          value.flags = value.flags | VDIAG_FLAG_LSB_FIRST;
        }
        changed = true;
      }
      SET_F(valueObj, "scale", value.scale);
      SET_F(valueObj, "offset", value.offset);
      SET_STR(valueObj, "name", value.name, VDIAG_NAME_LEN);
      SET_STR(valueObj, "unit", value.unit, VDIAG_UNIT_LEN);
      // 0.37.0: the value's own module. The ids can be 29-bit (the SET_ helpers stop at 16 bits)
      if (valueObj["tx"].is<uint32_t>())
      {
        value.txId = valueObj["tx"].as<uint32_t>();
        changed = true;
      }
      if (valueObj["rx"].is<uint32_t>())
      {
        value.rxId = valueObj["rx"].as<uint32_t>();
        changed = true;
      }
      SET_U8(valueObj, "txExt", value.txExt);
      SET_U8(valueObj, "rxExt", value.rxExt);
      SET_U8(valueObj, "addr", value.addr);
      SET_U8(valueObj, "proto", value.protocol);
    }
  }

  JsonObjectConst oilObj = doc["oil"];
  if (!oilObj.isNull())
  {
    SET_U8(oilObj, "source", target.oilSource);
    SET_U8(oilObj, "clusterAddr", target.oilClusterAddr);
    SET_U8(oilObj, "group", target.oilGroup);
    SET_U8(oilObj, "tempField", target.oilTempField);
    SET_U8(oilObj, "levelField", target.oilLevelField);
    SET_U8(oilObj, "levelOkZero", target.oilLevelOkZero);
    SET_U16(oilObj, "pollMs", target.oilPollMs);
  }

  JsonObjectConst adcObj = doc["adc"];
  if (!adcObj.isNull())
  {
    if (adcObj["baseId"].is<uint32_t>())
    {
      target.adcBaseId = adcObj["baseId"].as<uint32_t>() & 0x1FFFFFFFUL;
      changed = true;
    }
    SET_U8(adcObj, "wireless", target.adcWireless);
    JsonArrayConst channelsArr = adcObj["ch"];
    for (size_t channel = 0; channel < channelsArr.size() && channel < ADC_SENSORS; channel++)
    {
      JsonObjectConst channelObj = channelsArr[channel];
      SET_U8(channelObj, "mode", target.adc[channel].mode);
      SET_U8(channelObj, "decimals", target.adc[channel].decimals);
      SET_F(channelObj, "scale", target.adc[channel].scale);
      SET_F(channelObj, "offset", target.adc[channel].offset);
      SET_STR(channelObj, "name", target.adc[channel].name, sizeof(target.adc[channel].name));
      SET_STR(channelObj, "unit", target.adc[channel].unit, sizeof(target.adc[channel].unit));
      if (target.adc[channel].mode > ADC_MODE_LINEAR)
      {
        target.adc[channel].mode = ADC_MODE_SENT;
      }
      if (target.adc[channel].decimals > 3)
      {
        target.adc[channel].decimals = 3;
      }
    }
  }

  JsonObjectConst haldexObj = doc["haldex"];
  if (!haldexObj.isNull())
  {
    SET_U16(haldexObj, "broadcastId", target.haldexBroadcastId);
    SET_U16(haldexObj, "controlId", target.haldexControlId);
    SET_U8(haldexObj, "wireless", target.ohxWireless);
    SET_U8(haldexObj, "carSignals", target.ohxCarSignals);
    SET_STR(haldexObj, "key", target.ohxKey, sizeof(target.ohxKey));
  }

  JsonObjectConst ecuObj = doc["ecu"];
  if (!ecuObj.isNull())
  {
    SET_U8(ecuObj, "wireless", target.ecuWireless);
    SET_U8(ecuObj, "carSignals", target.ecuCarSignals);
  }

  JsonObjectConst navObj = doc["nav"]; // 0.30.0: nav link
  if (!navObj.isNull())
  {
    SET_U8(navObj, "mode", target.navMode);
    SET_U16(navObj, "popupM", target.navPopupM); // kept to 10..2550 m in 10 m steps below
  }

  JsonObjectConst logObj = doc["log"]; // the data logger (0.16.0)
  if (!logObj.isNull())
  {
    JsonArrayConst logSignalsArr = logObj["signals"];
    for (size_t channel = 0; channel < logSignalsArr.size() && channel < LOG_CHANNELS; channel++)
    {
      if (logSignalsArr[channel].is<const char *>())
      {
        target.logSignals[channel] = signalFromKey(logSignalsArr[channel].as<const char *>()); // unknown key = SIG_NONE
        changed = true;
      }
    }
    SET_U8(logObj, "rateHz", target.logRateHz);
    SET_U8(logObj, "autoStart", target.logAutoStart);
    SET_U8(logObj, "manualChannels", target.logManualChannels); // 0 = the active gauges, 1 = the channels above
  }

  JsonObjectConst chainObj = doc["chain"];
  if (!chainObj.isNull())
  {
    SET_U8(chainObj, "enabled", target.chainEnabled);
    SET_U8(chainObj, "role", target.chainRole);
    SET_U8(chainObj, "nodeId", target.nodeId);
    SET_STR(chainObj, "nodeName", target.nodeName, NAME_LEN);
    SET_U16(chainObj, "group", target.chainGroup);
    SET_U8(chainObj, "transport", target.linkTransport);
    SET_U8(chainObj, "canTx", target.linkCanTx);
    SET_U16(chainObj, "canBaseId", target.linkCanBaseId);
    SET_U8(chainObj, "followPage", target.followPage);
    SET_U8(chainObj, "followNight", target.followNight);
    SET_U8(chainObj, "failover", target.failover);
    SET_U8(chainObj, "rowPosition", target.rowPosition);
  }

  JsonObjectConst wifiObj = doc["wifi"];
  if (!wifiObj.isNull())
  {
    SET_U8(wifiObj, "mode", target.wifiMode);
    SET_STR(wifiObj, "apSsid", target.apSsid, WIFI_LEN);
    // the AP password: "" = open, or 8+ characters (the WPA2 minimum). A shorter one is ignored, so a typo never leaves
    // the gauge open without the owner knowing - the old password stays
    if (wifiObj["apPassword"].is<const char *>())
    {
      const char *newPassword = wifiObj["apPassword"].as<const char *>();
      const size_t newLength = strlen(newPassword);
      if (newLength == 0 || newLength >= 8)
      {
        copyStr(target.apPassword, WIFI_LEN, newPassword);
        changed = true;
      }
    }
    SET_STR(wifiObj, "staSsid", target.staSsid, WIFI_LEN);
    SET_STR(wifiObj, "staPassword", target.staPassword, WIFI_LEN);
    SET_STR(wifiObj, "hostname", target.hostname, NAME_LEN);
  }

  // learnt: only the paired adapter can be set (a backup carries it); the rest the gauge works out itself
  JsonObjectConst learntObj = doc["learnt"];
  if (!learntObj.isNull())
  {
    SET_STR(learntObj, "elmMac", target.elmMac, sizeof(target.elmMac));
    SET_U8(learntObj, "elmAddrType", target.elmAddrType);
    SET_U8(learntObj, "elmProtocol", target.elmProtocol);
  }

  // sanity
  if (target.brightness > 100)
  {
    target.brightness = 100;
  }
  if (target.nightBrightness > 100)
  {
    target.nightBrightness = 100;
  }
  if (target.nightSource > 1)
  {
    target.nightSource = 1;
  }
  if (target.outputMode > 1)
  {
    target.outputMode = 0;
  }
  for (int channel = 0; channel < IOX_CHANNELS; channel++)
  {
    if (target.outputManual[channel] > 2)
    {
      target.outputManual[channel] = 2;
    }
  }
  if (!themeValid(target.theme))
  {
    target.theme = THEME_TEAL;
  }
  if (target.bootLogo > 7) // 0.36.2: 7 = My Car
  {
    target.bootLogo = 6; // the can2gauge logo, as on a new gauge
  }
  if (target.logoMake[0] && !logoName(target.logoMake))
  {
    target.logoMake[0] = '\0';
  }
  if (target.canProfile >= canProfile_t_MAX)
  {
    target.canProfile = CAN_PROFILE_VAG_PQ;
  }
  for (uint8_t slot = 0; slot < 3; slot++)
  {
    if (target.dbcSlotBase[slot] > 0x7FF)
    {
      target.dbcSlotBase[slot] = 0; // 0.28.0: an 11-bit id, or 0 = as written in the DBC
    }
  }
  if (target.maxxBaseId < 0x100 || target.maxxBaseId > 0x7DC)
  {
    target.maxxBaseId = 0x520; // its frames run to base + 0x22
  }
  if (target.klineProtocol >= klineProtocol_t_MAX)
  {
    target.klineProtocol = KL_OFF;
  }
  if (target.me7FastMode > 1)
  {
    target.me7FastMode = 0; // only Normal (0) or Fast (1) is ever saved
  }
  if (target.startPage >= MAX_PAGES)
  {
    target.startPage = 0;
  }
  for (int pageIndex = 0; pageIndex < MAX_PAGES; pageIndex++)
  {
    if (target.pages[pageIndex].style >= gaugeStyle_t_MAX)
    {
      target.pages[pageIndex].style = STYLE_ARC;
    }
    if (target.pages[pageIndex].source >= pageSource_t_MAX)
    {
      target.pages[pageIndex].source = PSRC_CAN;
    }
    if (target.pages[pageIndex].spriteOn > 1)
    {
      target.pages[pageIndex].spriteOn = 1;
    }
    for (int slot = 0; slot < 3; slot++)
    {
      if (target.pages[pageIndex].slots[slot] >= SIG_COUNT)
      {
        target.pages[pageIndex].slots[slot] = SIG_NONE;
      }
    }
  }
  for (int triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    if (target.triggers[triggerIndex].action >= triggerAction_t_MAX)
    {
      target.triggers[triggerIndex].action = ACT_ALERT;
    }
    if (target.triggers[triggerIndex].output >= IOX_CHANNELS)
    {
      target.triggers[triggerIndex].output = 0;
    }
  }
  if (strlen(target.apPassword) > 0 && strlen(target.apPassword) < 8)
  {
    copyStr(target.apPassword, WIFI_LEN, ""); // too short for WPA2 (8 minimum), e.g. from an old backup: the AP is open
  }
  target.menuMask &= MENU_ALL;
  if (target.dsgSource >= dsgSource_t_MAX)
  {
    target.dsgSource = DSG_SRC_CAN_ONLY;
  }
  if (target.dsgUdsSession != 0x01 && target.dsgUdsSession != 0x03)
  {
    target.dsgUdsSession = 0x01;
  }
  if (target.dsgSpeedPref > 2)
  {
    target.dsgSpeedPref = 0;
  }
  if (target.dsgPollMs < 20)
  {
    target.dsgPollMs = 20;
  }
  target.dsgTxId &= 0x7FF;
  target.dsgRxId &= 0x7FF;
  if (target.dsgGearbox >= GEARBOX_TYPES)
  {
    target.dsgGearbox = GEARBOX_DSG; // 0.26.0
  }
  unitsSanitize(target); // 0.27.0: the units and their six rows
  for (DsgReadSpec *spec : {&target.dsgSpeed, &target.dsgGear, &target.dsgTemp, &target.dsgLockup, &target.dsgSelector})
  {
    if (spec->svc != 0x22 && spec->svc != 0x01)
    {
      spec->svc = 0;
    }
    if (spec->len < 1 || spec->len > 2)
    {
      spec->len = 1;
    }
    if (spec->fmt > DSG_FMT_TABLE)
    {
      spec->fmt = DSG_FMT_SCALED;
    }
    if (spec->scale == 0)
    {
      spec->scale = 1.0f;
    }
  }
  if (target.dsgTp20SpeedField > 3 && target.dsgTp20SpeedField != 0xFF)
  {
    target.dsgTp20SpeedField = 0;
  }
  if (target.dsgTp20GearField > 3)
  {
    target.dsgTp20GearField = 0;
  }
  if (target.dsgTp20SpeedDiv < 0)
  {
    target.dsgTp20SpeedDiv = 0;
  }
  // VAG diag (0.13.0)
  if (target.vdiagRun > VDIAG_RUN_ALWAYS)
  {
    target.vdiagRun = VDIAG_RUN_SHOWN;
  }
  if (target.vdiagProtocol > VDIAG_PROTO_TP20)
  {
    target.vdiagProtocol = VDIAG_PROTO_AUTO;
  }
  if (target.vdiagTurbo > 1)
  {
    target.vdiagTurbo = 1;
  }
  if (target.vdiagGroupSize < 1 || target.vdiagGroupSize > 8)
  {
    target.vdiagGroupSize = 4; // 8 DIDs = a 17-byte request (3 CAN frames), the most this firmware sends
  }
  if (target.vdiagSession != 0x01 && target.vdiagSession != 0x03)
  {
    target.vdiagSession = 0x03;
  }
  target.vdiagTxId &= 0x1FFFFFFFUL; // 0.34.0: 11-bit or 29-bit CAN ids
  target.vdiagRxId &= 0x1FFFFFFFUL;
  if (target.vdiagLink > VDIAG_LINK_ADAPTER)
  {
    target.vdiagLink = VDIAG_LINK_CAN;
  }
  if (target.vdiagPollMs < 10)
  {
    target.vdiagPollMs = 10;
  }
  if (target.vdiagPollMs > 2000)
  {
    target.vdiagPollMs = 2000;
  }
  for (int valueIndex = 0; valueIndex < VDIAG_VALUES; valueIndex++)
  {
    VdiagValue &value = target.vdiagValues[valueIndex];
    if (value.kind > VDIAG_KIND_OBD) // 0.37.6: OBD PID is the newest kind
    {
      value.kind = VDIAG_KIND_NONE;
    }
    if (value.bitLen < 1 || value.bitLen > 32)
    {
      value.bitLen = 8; // a number of 1 to 32 bits (anything longer isn't one value)
    }
    if (value.field < 1 || value.field > 4)
    {
      value.field = 1;
    }
    value.flags = value.flags & (VDIAG_FLAG_SIGNED | VDIAG_FLAG_LSB_FIRST); // only the two flags there are
    if (value.scale == 0)
    {
      value.scale = 1.0f; // 0 would make every value 0
    }
  }
  if (target.oilSource >= oilSource_t_MAX)
  {
    target.oilSource = OIL_SRC_CAN;
  }
  if (target.oilClusterAddr == 0 || target.oilClusterAddr > 0x7F)
  {
    target.oilClusterAddr = 0x17;
  }
  if (target.oilTempField > 3)
  {
    target.oilTempField = 2;
  }
  if (target.oilLevelField > 3 && target.oilLevelField != 0xFF)
  {
    target.oilLevelField = 0xFF;
  }
  if (target.oilLevelOkZero > 1)
  {
    target.oilLevelOkZero = 0;
  }
  // under ~1 s keeps the KWP1281 session alive by itself and the value inside engineTimeout (2 s)
  if (target.oilPollMs < 100)
  {
    target.oilPollMs = 100;
  }
  if (target.oilPollMs > 1000)
  {
    target.oilPollMs = 1000;
  }
  if (target.chainRole >= chainRole_t_MAX)
  {
    target.chainRole = CHAIN_AUTO;
  }
  if (target.rowPosition > ROW_MAX_POSITION)
  {
    target.rowPosition = 0; // out of range: not in a row
  }
  if (target.ohxWireless > 1)
  {
    target.ohxWireless = 1;
  }
  if (target.ohxCarSignals > 1)
  {
    target.ohxCarSignals = 1;
  }
  if (target.ecuWireless > 1)
  {
    target.ecuWireless = 1;
  }
  if (target.ecuCarSignals > 1)
  {
    target.ecuCarSignals = 1;
  }
  if (target.useCanRelay > 1)
  {
    target.useCanRelay = 1;
  }
  if (target.navPopupM < NAV_POPUP_STEP_M || target.navPopupM > 255 * NAV_POPUP_STEP_M)
  {
    target.navPopupM = NAV_POPUP_DEFAULT_M; // 10..2550 m
  }
  // rounded to the nearest 10 m step
  target.navPopupM = ((target.navPopupM + NAV_POPUP_STEP_M / 2) / NAV_POPUP_STEP_M) * NAV_POPUP_STEP_M;
  if (target.bootFadeMs < 100 || target.bootFadeMs > 5000)
  {
    target.bootFadeMs = 2000; // the web UI allows 100..5000 ms: back to the new-gauge fade
  }
  target.tabsShown &= TAB_SHOW_ALL;
  if (target.klineFastObd > 1)
  {
    target.klineFastObd = 0;
  }
  if (target.elmFastObd > 1)
  {
    target.elmFastObd = 0;
  }
  if (target.elmTransport >= elmTransport_t_MAX)
  {
    target.elmTransport = ELM_TR_BLE;
  }
  if (target.elmExtraSource > ELM_EXTRA_CAN_MONITOR)
  {
    target.elmExtraSource = ELM_EXTRA_OFF;
  }
  if (target.elmKlAddr == 0 || target.elmKlAddr > 0x7F)
  {
    target.elmKlAddr = 0x01;
  }
  if (target.elmKlFast > 1)
  {
    target.elmKlFast = 0;
  }
  if (target.adcWireless > 1)
  {
    target.adcWireless = 1;
  }
  if (!target.elmWifiPort)
  {
    target.elmWifiPort = 35000;
  }
  if (!target.elmWifiHost[0])
  {
    copyStr(target.elmWifiHost, sizeof(target.elmWifiHost), "192.168.0.10");
  }
  if (target.nodeId > 15 && target.nodeId != 0xFF)
  {
    target.nodeId = 0xFF;
  }
  if (target.saverEnabled > 1)
  {
    target.saverEnabled = 1;
  }
  if (target.saverReact > 1)
  {
    target.saverReact = 1;
  }
  if (target.gaugeFeel > 1)
  {
    target.gaugeFeel = 0;
  }
  if (target.renderMode > 1)
  {
    target.renderMode = 1; // direct, as on a new gauge
  }
  if (target.simdBlend > 1)
  {
    target.simdBlend = 0;
  }
  if (target.upsideDown > 1)
  {
    target.upsideDown = 0;
  }
  if (target.rotateTrim > 30)
  {
    target.rotateTrim = 30; // +15 degrees
  }
  if (target.rotateTrim < -30)
  {
    target.rotateTrim = -30; // -15 degrees
  }
  if (target.longPressMs < 200)
  {
    target.longPressMs = 200;
  }
  if (target.longPressMs > 5000)
  {
    target.longPressMs = 5000;
  }
  if (target.saverDelayS < 5)
  {
    target.saverDelayS = 5;
  }
  if (target.saverDelayS > 3600)
  {
    target.saverDelayS = 3600;
  }
  if (target.ignSource >= ignSource_t_MAX)
  {
    target.ignSource = IGN_SRC_OFF;
  }
  if (target.ignCanMode >= ignCanMode_t_MAX)
  {
    target.ignCanMode = IGN_CAN_PQ;
  }
  if (target.ignCanBit > 63)
  {
    target.ignCanBit = 63;
  }
  if (target.ignCanInvert > 1)
  {
    target.ignCanInvert = 1;
  }
  if (target.ignOffDelayS > 60)
  {
    target.ignOffDelayS = 60;
  }
  target.ignCanId &= (target.ignCanId & 0x80000000UL) ? 0x9FFFFFFFUL : 0x7FFUL; // bit 31 = extended (29-bit id)
  if (target.linkTransport >= linkTransport_t_MAX)
  {
    target.linkTransport = LINK_AUTO;
  }
  target.linkCanBaseId &= 0x7C0; // DATA / HEARTBEAT / COMMAND blocks of 16 ids each must stay inside 11 bits
  if (target.linkCanBaseId < 0x100)
  {
    target.linkCanBaseId = 0x6C0;
  }

  if (changed)
  {
    settingsDirty = true;
  }
  return changed;
}
