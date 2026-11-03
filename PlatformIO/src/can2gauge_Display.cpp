#include <lv_blend_esp32s3.h> // c2gSimdEnable
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_Display.h>
#include <can2gauge_Units.h> // 0.27.0: the glass's Units button turns the pages' ranges over
#include <can2gauge_IO.h>
#include <can2gauge_EEP.h>
#include <can2gauge_KLine.h>
#include <can2gauge_ELM.h>
#include <can2gauge_Link.h>
#include <can2gauge_OHX.h>
#include <can2gauge_CanMon.h>
#include <can2gauge_Triggers.h>
#include <can2gauge_ADC2CAN.h>
#include <can2gauge_Drag.h> // 0.38.0: the drag timer's best time on the menu
#include <LVGL_Driver.h>
#include <LVGL_Rotate.h> // 0.25.1: Upside Down / Rotation Trim (SCREEN_ROTATION 0 leaves it out)
#include <GaugeUI.h>
#include <CANLine.h>
#include <IOX.h>
#include <Wire.h> // 0.24.2: I2C bus scan when the touch controller doesn't answer

// ---------------------------------------------------------------------------
// info pages (long-press menu) - text providers. K-line and output/lights come from their own modules
// (klineInfoProvider, ioInfoProvider); Haldex and CAN are here.
// ---------------------------------------------------------------------------
static void haldexInfo(char *buf, size_t len)
{
  static const char *modes[] = {"STOCK", "FWD", "50:50", "60:40", "75:25", "EXPERT"};
  const bool link = signalValid(SIG_HALDEX_ACTUAL);
  int mode = (int)signalGet(SIG_HALDEX_MODE);
  char head[40];
  if (link)
  {
    snprintf(head, sizeof(head), "OpenHaldex linked (%s)", haldexHeardVia());
  }
  else
  {
    copyStr(head, sizeof(head), ohxHeard() ? "OpenHaldex: no Haldex CAN" : "NO LINK (0x6B0 / WiFi off?)");
  }
  snprintf(buf, len, "%s\n\nmode %s\nlock target %d %%\nengagement %d %%\nspeed %d km/h\npedal %d %%\n\nbroadcast 0x%03X  ctrl 0x%03X",
           head, (mode >= 0 && mode < 6) ? modes[mode] : "-",
           (int)signalGet(SIG_HALDEX_TARGET), (int)signalGet(SIG_HALDEX_ACTUAL), (int)signalGet(SIG_HALDEX_SPEED),
           (int)signalGet(SIG_HALDEX_PEDAL), settings.haldexBroadcastId, settings.haldexControlId);
}

// 0.30.15: the CAN termination for the info screen: Off / ON, or Auto and what it chose
static const char *termText()
{
  if (settings.canTermination == CAN_TERM_AUTO)
  {
    if (canTerminationIsOn())
    {
      return "auto, ON (120R)";
    }
    return "auto, off";
  }
  if (settings.canTermination == CAN_TERM_ON)
  {
    return "ON (120R)";
  }
  return "off";
}

static void canInfo(char *buf, size_t len)
{
  static const char *profileNames[canProfile_t_MAX] = {"VAG PQ (listen)", "OBD-II PIDs", "listen only", "VAG MQB (listen)"};
  const CanStats &stats = canStats();
  snprintf(buf, len, "%s  %u kbps\n%s\n\nrx %lu  (%lu/s)\ntx %lu   err %lu\nbus-off %lu\n\ntermination %s\n\nrpm %d   speed %d\ncoolant %d C",
           stats.up ? "bus up" : "bus DOWN", settings.canKbps, profileNames[settings.canProfile < canProfile_t_MAX ? settings.canProfile : 2],
           (unsigned long)stats.rx, (unsigned long)stats.rxPerSec, (unsigned long)stats.tx, (unsigned long)stats.err, (unsigned long)stats.busOff,
           termText(), (int)signalGet(SIG_RPM), (int)signalGet(SIG_SPEED), (int)signalGet(SIG_COOLANT));
  if (settings.canProfile == CAN_PROFILE_OBD2 || settings.obdOverCan)
  {
    // OBD-II polling on the wire: PID values per second, and how many PIDs go in each request
    const CanObdStats &obdStats = canObdStats();
    size_t written = strlen(buf);
    if (obdStats.ecuFound)
    {
      appendf(buf, len, written, "\n\nOBD %lu PIDs/s (%u per req)", (unsigned long)obdStats.pidsPerSec, obdStats.pidsPerRequest);
    }
    else
    {
      appendf(buf, len, written, "\n\nOBD: looking for the ECU");
    }
  }
}

// the mode menu's status line, one per item (docs/ui-concept: "image, name, one line of text, live status"). The
// panel fonts carry ASCII, the degree sign and the bullet, so the separator is a bullet.
static int8_t menuStatus(uint8_t item, char *buf, size_t len)
{
  static const char *modes[] = {"STOCK", "FWD", "50:50", "60:40", "75:25", "EXPERT"};
  switch (item)
  {
  case MENU_HALDEX:
  {
    const bool link = signalValid(SIG_HALDEX_ACTUAL);
    const int mode = (int)signalGet(SIG_HALDEX_MODE);
    if (link)
    {
      snprintf(buf, len, "%s • %s", haldexHeardOnCan() ? "Link OK" : "WiFi", (mode >= 0 && mode < 6) ? modes[mode] : "-");
    }
    else if (ohxHeard())
    {
      snprintf(buf, len, "OpenHaldex • no Haldex data");
    }
    else
    {
      snprintf(buf, len, settings.ohxWireless ? "No link (0x%03X / WiFi)" : "No link (0x%03X)", settings.haldexBroadcastId);
    }
    return link;
  }
  case MENU_CAN:
  {
    const CanStats &stats = canStats();
    const uint32_t air = settings.useCanRelay ? canAirStats().perSec : 0; // relayed over WiFi (OpenHaldex / adc2can)
    if (air && !(settings.canEnabled && stats.up && stats.rxPerSec > 0))
    {
      snprintf(buf, len, "WiFi relay • %lu/s", (unsigned long)air);
    }
    else if (!settings.canEnabled)
    {
      snprintf(buf, len, "CAN off");
    }
    else if (!stats.up)
    {
      snprintf(buf, len, "Bus down");
    }
    else
    {
      snprintf(buf, len, "%lu/s • %u kbps", (unsigned long)stats.rxPerSec, settings.canKbps);
    }
    return (settings.canEnabled && stats.up && stats.rxPerSec > 0) || air > 0;
  }
  case MENU_KLINE:
    snprintf(buf, len, "%s", runtime.klineState);
    return runtime.klineUp ? 1 : 0;
  case MENU_OUTPUT:
    snprintf(buf, len, "OUT %s • lights %s%s", ioxGet(0) ? "on" : "off", dashLightsOn() ? "on" : "off",
             ioxFault() ? " • FAULT" : "");
    return ioxFault() ? 0 : 1;
  case MENU_DSG:
    if (signalValid(SIG_SELECTOR) && signalValid(SIG_GEAR) && signalGet(SIG_GEAR) > 0)
    {
      snprintf(buf, len, "Selector %s \xE2\x80\xA2 gear %d", selectorLetter((uint8_t)signalGet(SIG_SELECTOR)), (int)lroundf(signalGet(SIG_GEAR)));
    }
    else if (signalValid(SIG_SELECTOR))
    {
      snprintf(buf, len, "Selector %s", selectorLetter((uint8_t)signalGet(SIG_SELECTOR)));
    }
    else
    {
      snprintf(buf, len, "No gearbox data");
    }
    return signalValid(SIG_SELECTOR) ? 1 : 0;
  case MENU_BLE:
    if (elmConnected() || settings.elmEnabled) // auto-connect on, or connected for a screen / page right now
    {
      snprintf(buf, len, "%s", !strcmp(elmStatus().state, "polling") ? "Connected" : elmStatus().state);
    }
    else
    {
      snprintf(buf, len, "%s", settings.elmMac[0] ? "Paired - connects when opened" : "No adapter paired");
    }
    return elmConnected() ? 1 : 0;
  case MENU_ADC:
  {
    uint8_t valid = 0;
    float busVolts = 0.0f;
    for (uint8_t board = 0; board < ADC2CAN_BOARDS; board++)
    {
      if (adc2canBoard(board).seen)
      {
        valid += __builtin_popcount(adc2canBoard(board).validMask);
        busVolts = adc2canBoard(board).busV;
      }
    }
    if (!adc2canAny())
    {
      snprintf(buf, len, "No adc2can heard");
      return 0;
    }
    snprintf(buf, len, "%u sensors \xE2\x80\xA2 %.2f V", valid, busVolts);
    return 1;
  }
  case MENU_DRAG:
  {
    // 0.38.0: the best time of the run the screen times (the speed row's unit picks 0-100 km/h or 0-60 mph)
    uint8_t kind = DRAG_KIND_KMH;
    const char *title = "0-100 km/h";
    if (settings.unitSpeed == UNIT_IMPERIAL)
    {
      kind = DRAG_KIND_MPH;
      title = "0-60 mph";
    }
    if (dragScoreCount(kind) == 0)
    {
      snprintf(buf, len, "%s \xE2\x80\xA2 no runs yet", title);
      return -1;
    }
    const uint32_t bestMs = dragScore(kind, 0).timeMs;
    snprintf(buf, len, "%s \xE2\x80\xA2 best %lu.%02lu s", title, (unsigned long)(bestMs / 1000),
             (unsigned long)((bestMs % 1000) / 10));
    return 1;
  }
  case MENU_GAUGES:
  {
    uint8_t enabledCount = 0;
    for (uint8_t pageIndex = 0; pageIndex < MAX_PAGES; pageIndex++)
    {
      enabledCount += settings.pages[pageIndex].enabled ? 1 : 0;
    }
    uint8_t shownPage = 0; // the page showing: runtime.activePage counts enabled pages only
    for (uint8_t pageIndex = 0, enabledIndex = 0; pageIndex < MAX_PAGES; pageIndex++)
    {
      if (settings.pages[pageIndex].enabled && enabledIndex++ == runtime.activePage)
      {
        shownPage = pageIndex;
      }
    }
    const GaugePageCfg &page = settings.pages[shownPage];
    snprintf(buf, len, "%u page%s • %s", enabledCount, enabledCount == 1 ? "" : "s", page.label[0] ? page.label : uiStyleName(page.style));
    return -1;
  }
  case MENU_CHAIN:
    if (settings.chainEnabled)
    {
      snprintf(buf, len, "G%u • %s", linkNodeId(), linkRoleName());
    }
    else
    {
      snprintf(buf, len, "Off • standalone");
    }
    return settings.chainEnabled ? 1 : 0;
  default: // Settings
    snprintf(buf, len, "%s.local", settings.hostname);
    return -1;
  }
}

// ---------------------------------------------------------------------------
// mode screens (menu > K-line / CAN / Bluetooth OBD). The provider is called ~10x/s while one shows, and being called
// is what claims the source: the K-line session and the Bluetooth link run while the screen shows and are released
// MODE_RELEASE_MS after it closes. Text is ASCII plus the degree sign and the bullet (the panel fonts).
// ---------------------------------------------------------------------------
#define BULLET " \xE2\x80\xA2 "

static uint32_t modeStamp()
{
  const uint32_t now = millis();
  return now ? now : 1;
}

static bool freshOpen(uint32_t &lastCall) // first call after the screen was opened (no call for a second)
{
  const uint32_t now = millis();
  const bool fresh = !lastCall || now - lastCall > 1000;
  lastCall = now;
  return fresh;
}

// ---- K-line: the module picked in the web UI, one measuring block at a time ----
static const struct
{
  uint8_t addr;
  const char *name;
} klineModules[] = {{0x01, "Engine"}, {0x02, "Gearbox"}, {0x03, "ABS"}, {0x08, "Climate"}, {0x15, "Airbag"}, {0x16, "Steering wheel"}, {0x17, "Instruments"}, {0x19, "Gateway"}, {0x22, "AWD"}, {0x25, "Immobiliser"}, {0x35, "Central locking"}, {0x37, "Navigation"}, {0x46, "Comfort"}, {0x55, "Headlights"}, {0x56, "Radio"}, {0x65, "Tyre pressure"}, {0x76, "Park assist"}};

static const char *klineModuleName(uint8_t addr)
{
  for (const auto &module : klineModules)
  {
    if (module.addr == addr)
    {
      return module.name;
    }
  }
  return "Module";
}

// what a measuring-block field is, from the unit its formula gives (the module's label file isn't on the gauge)
static const char *klineFieldKind(const char *unit)
{
  static const struct
  {
    const char *unit, *kind;
  } kinds[] = {{"rpm", "Engine speed"}, {"C", "Temperature"}, {"V", "Voltage"}, {"%", "Percent"}, {"ms", "Time"}, {"deg", "Angle"}, {"deg k/w", "Angle"}, {"ATDC", "Angle"}, {"g/s", "Air mass"}, {"mbar", "Pressure"}, {"bar", "Pressure"}, {"Bar", "Pressure"}, {"km/h", "Speed"}, {"l/h", "Fuel use"}, {"A", "Current"}, {"Nm", "Torque"}, {"kW", "Power"}, {"Kw", "Power"}, {"Ohm", "Resistance"}, {"km", "Distance"}, {"l", "Volume"}, {"mm", "Distance"}, {"s", "Time"}, {"sec", "Time"}, {"h", "Time"}};
  for (const auto &entry : kinds)
  {
    if (!strcmp(unit, entry.unit))
    {
      return entry.kind;
    }
  }
  return "Value";
}

static void klineFormatValue(float value, const char *unit, char *buf, size_t len)
{
  const char *shown;
  if (!strcmp(unit, "C"))
  {
    shown = "\xC2\xB0"
            "C";
  }
  else if (!strcmp(unit, "deg") || !strcmp(unit, "Deg"))
  {
    shown = "\xC2\xB0";
  }
  else
  {
    shown = unit;
  }
  const float magnitude = fabsf(value);
  int decimals;
  if (unitDecimals(unit) > 0)
  {
    decimals = unitDecimals(unit); // 0.36.14: the unit says (lambda 1.00, 13.8 V), even on a whole number
  }
  else if (fabsf(value - lroundf(value)) < 0.005f)
  {
    decimals = 0;
  }
  else if (magnitude >= 100.0f)
  {
    decimals = 0;
  }
  else if (magnitude >= 10.0f)
  {
    decimals = 1;
  }
  else
  {
    decimals = 2;
  }
  snprintf(buf, len, shown[0] ? "%.*f %s" : "%.*f", decimals, valueNoMinusZero(value, decimals), shown);
}

static void klineView(UiModeView &view)
{
  static uint32_t lastCall = 0;
  KlineBrowse &browse = runtime.klineBrowse;
  const bool fresh = freshOpen(lastCall);
  runtime.klineModeSeenMs = modeStamp(); // claims the K-line (wantedOwner in can2gauge_KLine.cpp)
  const uint8_t addr = settings.klineModeAddr ? settings.klineModeAddr : 0x01;
  if (fresh && !browse.connected && !browse.connecting)
  {
    snprintf(browse.state, sizeof(browse.state), "Starting the K-line session");
  }

  char name[24];
  copyStr(name, sizeof(name), klineModuleName(addr));
  for (char *character = name; *character; character++)
  {
    *character = (char)toupper((unsigned char)*character);
  }
  snprintf(view.title, sizeof(view.title), "K-LINE" BULLET "%02X %s", addr, name);
  copyStr(view.status, sizeof(view.status), browse.state);
  if (browse.connecting || (browse.connected && browse.searching) || (!browse.connected && fresh))
  {
    view.state = UI_MS_BUSY;
  }
  else
  {
    view.state = browse.connected ? UI_MS_OK : UI_MS_BAD;
  }
  if (!browse.connected)
  {
    return;
  }

  int group = browse.group + browse.step; // a swipe not yet picked up by the K-line task shows straight away
  if (group > 255)
  {
    group = 1;
  }
  else if (group < 1)
  {
    group = 255;
  }
  snprintf(view.header, sizeof(view.header), "Block %d", group);
  copyStr(view.sub, sizeof(view.sub), browse.title[0] && !browse.step && !browse.searching ? browse.title : browse.ecuId); // the label file's title
  if (!browse.searching && !browse.step && browse.text[0] != '\0')
  {
    // 0.17.4: a text block (ME7 81 = VIN): the text across the rows, as many characters as a row name holds
    const size_t perRow = sizeof(view.rowName[0]) - 2; // leaves room for the end of the text ('\0')
    const size_t textLength = strlen(browse.text);
    view.rows = 0;
    for (size_t start = 0; start < textLength && view.rows < UI_MODE_ROWS; start += perRow)
    {
      copyStr(view.rowName[view.rows], perRow + 1, browse.text + start); // copyStr stops at perRow characters
      view.rowValue[view.rows][0] = '\0';
      view.rows++;
    }
    return;
  }
  if (browse.searching || browse.step || browse.fieldCount == 0)
  {
    return;
  }

  view.rows = browse.fieldCount < UI_MODE_ROWS ? browse.fieldCount : UI_MODE_ROWS;
  for (uint8_t row = 0; row < view.rows; row++)
  {
    if (browse.fieldName[row][0]) // from the label file
    {
      copyStr(view.rowName[row], sizeof(view.rowName[row]), browse.fieldName[row]);
    }
    else
    {
      snprintf(view.rowName[row], sizeof(view.rowName[row]), "%u  %s", row + 1, klineFieldKind(browse.unit[row]));
    }
    klineFormatValue(browse.value[row], browse.unit[row], view.rowValue[row], sizeof(view.rowValue[row]));
  }
}

// ---- CAN: every id on the bus, decoded with the car's DBC (web UI CAN tab) ----
#define CAN_LIVE_MS 2000
static uint32_t canCurrentId = 0;
static uint8_t canCurrentPage = 0;
static volatile int8_t canStep = 0;
static CanMonEntry *canLive = (CanMonEntry *)psramCalloc(CANMON_MAX_IDS, sizeof(CanMonEntry)); // in PSRAM (4.5 KB off internal RAM)

// the signals worth showing for this frame: not counters / checksums, and in the current multiplexer page. Values
// (more than one bit) come first, then the 1-bit flags, so the first page shows numbers (Brake_1: the wheel speed).
static uint8_t canVisible(const CanDbcMessage *message, const CanMonEntry &entry, uint16_t *out, uint8_t max)
{
  uint8_t count = 0;
  if (!message)
  {
    return 0;
  }
  for (int flags = 0; flags < 2; flags++)
  {
    for (uint16_t signalIndex = 0; signalIndex < message->signalCount && count < max; signalIndex++)
    {
      const CanDbcSignal &signal = canDbcSignal(message->firstSignal + signalIndex);
      float value;
      if ((signal.len == 1) == (flags == 1) && !canDbcIsHousekeeping(signal) && signal.mux != -2 &&
          canDbcDecode(*message, signal, entry.data, entry.dlc, value))
      {
        out[count++] = message->firstSignal + signalIndex;
      }
    }
  }
  return count;
}

static void canView(UiModeView &view)
{
  const CanStats &stats = canStats();
  const uint16_t idCount = canMonitorSnapshot(canLive, CANMON_MAX_IDS, CAN_LIVE_MS);
  copyStr(view.title, sizeof(view.title), "CAN BUS");
  if (!settings.canEnabled)
  {
    copyStr(view.status, sizeof(view.status), "CAN is off - switch it on in the web UI");
    view.state = UI_MS_BAD;
    return;
  }
  if (!stats.up)
  {
    snprintf(view.status, sizeof(view.status), "Bus down (%u kbps)", settings.canKbps);
    view.state = UI_MS_BAD;
    return;
  }

  canDbcLock();
  const bool dbcLoaded = canDbcLoaded();
  if (dbcLoaded)
  {
    snprintf(view.status, sizeof(view.status), "%u IDs" BULLET "%lu/s" BULLET "%s", idCount, (unsigned long)stats.rxPerSec, canDbcName());
  }
  else
  {
    snprintf(view.status, sizeof(view.status), "%u IDs" BULLET "%lu/s" BULLET "no DBC", idCount, (unsigned long)stats.rxPerSec);
  }
  if (!idCount)
  {
    canDbcUnlock();
    snprintf(view.status, sizeof(view.status), "Listening at %u kbps - no frames yet", settings.canKbps);
    view.state = UI_MS_BUSY;
    return;
  }
  view.state = UI_MS_OK;

  uint16_t index = 0; // the current id, or the next one up if it went quiet
  while (index < idCount && canLive[index].id < canCurrentId)
  {
    index++;
  }
  if (index >= idCount)
  {
    index = 0;
  }
  if (canLive[index].id != canCurrentId)
  {
    canCurrentPage = 0;
  }
  uint16_t visible[64];
  auto pagesOf = [&](uint16_t entryIndex) -> uint8_t
  {
    const uint8_t visibleCount = canVisible(dbcLoaded ? canDbcFind(canLive[entryIndex].id) : NULL, canLive[entryIndex], visible, 64);
    return visibleCount ? (visibleCount + UI_MODE_ROWS - 1) / UI_MODE_ROWS : 1;
  };
  const int8_t step = canStep;
  canStep = 0;
  if (step > 0)
  {
    if (canCurrentPage + 1 < pagesOf(index))
    {
      canCurrentPage++;
    }
    else
    {
      index = (index + 1) % idCount;
      canCurrentPage = 0;
    }
  }
  else if (step < 0)
  {
    if (canCurrentPage > 0)
    {
      canCurrentPage--;
    }
    else
    {
      index = (index + idCount - 1) % idCount;
      canCurrentPage = pagesOf(index) - 1;
    }
  }

  const CanMonEntry &entry = canLive[index];
  canCurrentId = entry.id;
  if (entry.id & 0x80000000UL)
  {
    snprintf(view.header, sizeof(view.header), "0x%08lX", (unsigned long)(entry.id & 0x1FFFFFFF));
  }
  else
  {
    snprintf(view.header, sizeof(view.header), "0x%03lX", (unsigned long)entry.id);
  }
  snprintf(view.hint, sizeof(view.hint), "%u of %u", index + 1, idCount);
  const CanDbcMessage *message = dbcLoaded ? canDbcFind(entry.id) : NULL;
  const uint8_t visibleCount = canVisible(message, entry, visible, 64);
  const uint8_t pages = visibleCount ? (visibleCount + UI_MODE_ROWS - 1) / UI_MODE_ROWS : 1;
  if (canCurrentPage >= pages)
  {
    canCurrentPage = pages - 1;
  }
  if (message && visibleCount)
  {
    if (pages > 1)
    {
      snprintf(view.sub, sizeof(view.sub), "%s  %u/%u", message->name, canCurrentPage + 1, pages);
    }
    else
    {
      copyStr(view.sub, sizeof(view.sub), message->name);
    }
    for (uint8_t visibleIndex = canCurrentPage * UI_MODE_ROWS; visibleIndex < visibleCount && view.rows < UI_MODE_ROWS; visibleIndex++)
    {
      const CanDbcSignal &signal = canDbcSignal(visible[visibleIndex]);
      float value = 0;
      canDbcDecode(*message, signal, entry.data, entry.dlc, value);
      copyStr(view.rowName[view.rows], sizeof(view.rowName[0]), signal.name);
      for (char *character = view.rowName[view.rows]; *character; character++)
      {
        if (*character == '_')
        {
          *character = ' ';
        }
      }
      canDbcFormat(signal, value, view.rowValue[view.rows], sizeof(view.rowValue[0]));
      view.rows++;
    }
  }
  else
  {
    // raw bytes: no DBC, an id the DBC doesn't have, or nothing decodable in it
    if (message)
    {
      copyStr(view.sub, sizeof(view.sub), message->name);
    }
    else
    {
      snprintf(view.sub, sizeof(view.sub), dbcLoaded ? "Not in %s" : "No DBC - pick the car in the web UI", canDbcName());
    }
    view.bytes = entry.dlc;
    memcpy(view.data, entry.data, 8);
  }
  canDbcUnlock();
}

// ---- Bluetooth OBD: the paired ELM327, pages of live PIDs ----
static const struct
{
  uint8_t pid;
  signalId_t sig;
  uint8_t decimals; // decimals
} obdRows[] = {{0x0C, SIG_RPM, 0}, {0x0D, SIG_SPEED, 0}, {0x05, SIG_COOLANT, 0}, {0x0B, SIG_BOOST, 0}, {0x0F, SIG_IAT, 0}, {0x11, SIG_THROTTLE, 0}, {0x04, SIG_LOAD, 0}, {0x10, SIG_MAF, 1}, {0x0E, SIG_TIMING, 1}, {0x42, SIG_BATTERY, 1}, {0x2F, SIG_FUEL_LEVEL, 0}, {0x5C, SIG_OIL_TEMP, 0}, {0x44, SIG_LAMBDA, 2}, {0x46, SIG_AMBIENT, 0}};
static uint8_t obdPage = 0;
static volatile int8_t obdStep = 0;

static bool obdSupported(const ElmStatus &status, uint8_t pid)
{
  if (!status.supported[0])
  {
    return true; // not read yet: show everything
  }
  const uint8_t maskIndex = (pid - 1) / 32;
  return pid && maskIndex < 3 && ((status.supported[maskIndex] >> (31 - (pid - 1) % 32)) & 1);
}

static void elmView(UiModeView &view)
{
  static uint32_t lastCall = 0;
  freshOpen(lastCall);
  runtime.elmModeSeenMs = modeStamp(); // the ELM task connects while this shows (can2gauge_ELM.cpp elmActive)
  copyStr(view.title, sizeof(view.title), "BLUETOOTH OBD");
  if (!linkIsMaster())
  {
    copyStr(view.status, sizeof(view.status), "The chain master holds the adapter");
    view.state = UI_MS_BAD;
    return;
  }
  if (!settings.elmMac[0])
  {
    copyStr(view.status, sizeof(view.status), "No adapter paired");
    view.state = UI_MS_BAD;
    copyStr(view.header, sizeof(view.header), "Not paired");
    copyStr(view.sub, sizeof(view.sub), "Web UI > Bluetooth OBD: Scan, then Pair");
    return;
  }

  const ElmStatus &status = elmStatus();
  const bool polling = elmConnected() && !strcmp(status.state, "polling");
  static const struct
  {
    const char *state, *text;
    int8_t uiState;
  } steps[] = {{"connecting", "Connecting to %s", UI_MS_BUSY},
               {"init", "Setting up the adapter", UI_MS_BUSY},
               {"protocol search", "Finding the car's OBD protocol", UI_MS_BUSY},
               {"identifying", "Reading the VIN", UI_MS_BUSY},
               {"not connected", "No answer from %s - retrying", UI_MS_BAD},
               {"init failed", "The adapter didn't set up - retrying", UI_MS_BAD},
               {"off", "Starting Bluetooth", UI_MS_BUSY}};
  view.state = UI_MS_BAD;
  snprintf(view.status, sizeof(view.status), "%s", status.state);
  for (const auto &entry : steps)
  {
    if (!strcmp(status.state, entry.state))
    {
      snprintf(view.status, sizeof(view.status), entry.text, settings.elmMac);
      view.state = entry.uiState;
    }
  }
  if (!polling)
  {
    return;
  }

  snprintf(view.status, sizeof(view.status), "%s" BULLET "%.1f V", elmProtocolName(status.protocol), status.volts);
  view.state = UI_MS_OK;
  uint8_t supportedRows[sizeof(obdRows) / sizeof(obdRows[0])];
  uint8_t supportedCount = 0;
  for (uint8_t row = 0; row < sizeof(obdRows) / sizeof(obdRows[0]); row++)
  {
    if (obdSupported(status, obdRows[row].pid))
    {
      supportedRows[supportedCount++] = row;
    }
  }
  const uint8_t pages = supportedCount ? (supportedCount + UI_MODE_ROWS - 1) / UI_MODE_ROWS : 1;
  const int8_t step = obdStep;
  obdStep = 0;
  obdPage = (uint8_t)((obdPage + pages + step) % pages);
  snprintf(view.header, sizeof(view.header), "Page %u / %u", obdPage + 1, pages);
  if (status.make[0])
  {
    snprintf(view.sub, sizeof(view.sub), "%s %s", status.make, status.model);
  }
  else
  {
    copyStr(view.sub, sizeof(view.sub), status.vin[0] ? status.vin : status.adapter);
  }

  for (uint8_t listIndex = obdPage * UI_MODE_ROWS; listIndex < supportedCount && view.rows < UI_MODE_ROWS; listIndex++)
  {
    const signalId_t id = obdRows[supportedRows[listIndex]].sig;
    copyStr(view.rowName[view.rows], sizeof(view.rowName[0]), signalInfo(id).name);
    if (signalValid(id))
    {
      const char *unit = "";
      const float value = signalDisplayValue(id, signalGet(id), &unit);
      const int decimals = !strcmp(unit, "psi") ? 1 : obdRows[supportedRows[listIndex]].decimals;
      const char *shown;
      if (!strcmp(unit, "C"))
      {
        shown = "\xC2\xB0"
                "C";
      }
      else if (!strcmp(unit, "F"))
      {
        shown = "\xC2\xB0"
                "F";
      }
      else if (!strcmp(unit, "deg"))
      {
        shown = "\xC2\xB0";
      }
      else
      {
        shown = unit;
      }
      snprintf(view.rowValue[view.rows], sizeof(view.rowValue[0]), shown[0] ? "%.*f %s" : "%.*f", decimals,
               valueNoMinusZero(value, decimals), shown);
    }
    else
    {
      copyStr(view.rowValue[view.rows], sizeof(view.rowValue[0]), "--");
    }
    view.rows++;
  }
}

// ---- Output / Illumination ----
static void outputView(UiModeView &view)
{
  const bool on = ioxGet(IOX_OUT);
  const bool fault = ioxFault();
  const bool lights = dashLightsOn();
  const bool night = nightActive();
  copyStr(view.title, sizeof(view.title), "OUTPUT / ILLUMINATION");
  // the state at a glance: green = on, red = off (or a fault)
  if (fault)
  {
    copyStr(view.status, sizeof(view.status), "Fault: over-current or too hot");
  }
  else
  {
    copyStr(view.status, sizeof(view.status), on ? "Output on" : "Output off");
  }
  view.state = on && !fault ? UI_MS_OK : UI_MS_ERR;
  copyStr(view.header, sizeof(view.header), on ? "ON" : "OFF");
  view.headerTone = on && !fault ? 1 : 2;
  copyStr(view.sub, sizeof(view.sub), settings.outputMode == 1 ? "Low-side: switches the load to ground" : "High-side: switches +12 V to the load");

  const uint8_t manual = settings.outputManual[IOX_OUT];
  int8_t activeTrigger = -1;
  for (uint8_t trigger = 0; trigger < MAX_TRIGGERS && activeTrigger < 0; trigger++)
  {
    if (settings.triggers[trigger].enabled && triggerActive(trigger))
    {
      activeTrigger = trigger;
    }
  }
  const char *controlText;
  if (manual == 2)
  {
    controlText = "Auto (triggers)";
  }
  else if (manual == 1)
  {
    controlText = "Manual on";
  }
  else
  {
    controlText = "Manual off";
  }
  const char *dimmingText;
  if (settings.nightSource != 1)
  {
    dimmingText = "Off";
  }
  else if (night)
  {
    dimmingText = "Active";
  }
  else
  {
    dimmingText = "Armed";
  }
  const char *triggerText;
  if (activeTrigger >= 0 && settings.triggers[activeTrigger].label[0])
  {
    triggerText = settings.triggers[activeTrigger].label;
  }
  else if (activeTrigger >= 0)
  {
    triggerText = "Active";
  }
  else
  {
    triggerText = "None";
  }

  static const char *rowNames[] = {"Control", "Illumination", "Dimming", "Brightness", "Trigger"};
  view.rows = 5;
  copyStr(view.rowName[0], sizeof(view.rowName[0]), rowNames[0]);
  copyStr(view.rowValue[0], sizeof(view.rowValue[0]), controlText);
  copyStr(view.rowName[1], sizeof(view.rowName[1]), rowNames[1]);
  copyStr(view.rowValue[1], sizeof(view.rowValue[1]), lights ? "On" : "Off");
  copyStr(view.rowName[2], sizeof(view.rowName[2]), rowNames[2]);
  copyStr(view.rowValue[2], sizeof(view.rowValue[2]), dimmingText);
  copyStr(view.rowName[3], sizeof(view.rowName[3]), rowNames[3]);
  snprintf(view.rowValue[3], sizeof(view.rowValue[3]), night ? "%u %% night" : "%u %%", backlightTarget());
  copyStr(view.rowName[4], sizeof(view.rowName[4]), rowNames[4]);
  copyStr(view.rowValue[4], sizeof(view.rowValue[4]), triggerText);
}

// ---- Daisy Chain: this gauge and the others on the link ----
static void chainView(UiModeView &view)
{
  static const char *transportNames[] = {"Auto", "CAN", "ESP-NOW", "CAN + ESP-NOW", "Off"};
  copyStr(view.title, sizeof(view.title), "DAISY CHAIN");
  if (!settings.chainEnabled)
  {
    copyStr(view.status, sizeof(view.status), "Off - this gauge runs on its own");
    view.state = UI_MS_BAD;
    view.nodes = 1;
    copyStr(view.nodeName[0], sizeof(view.nodeName[0]), "G?");
    view.nodeFlags[0] = UI_NODE_SELF | UI_NODE_HEARD;
    copyStr(view.sub, sizeof(view.sub), "Turn it on in the web UI (Daisy Chain)");
    return;
  }

  LinkNode nodeList[UI_MODE_NODES];
  const uint8_t heardCount = linkNodes(nodeList, UI_MODE_NODES - 1);
  // circles in node order, this gauge among them
  uint8_t nodeIds[UI_MODE_NODES];
  uint8_t nodeFlags[UI_MODE_NODES];
  uint8_t nodeCount = 0;
  nodeIds[nodeCount] = linkNodeId();
  nodeFlags[nodeCount++] = UI_NODE_SELF | UI_NODE_HEARD | (linkIsMaster() ? UI_NODE_MASTER : 0);
  for (uint8_t i = 0; i < heardCount && nodeCount < UI_MODE_NODES; i++)
  {
    nodeIds[nodeCount] = nodeList[i].node;
    nodeFlags[nodeCount++] = UI_NODE_HEARD | (nodeList[i].role == 1 ? UI_NODE_MASTER : 0);
  }
  for (uint8_t i = 1; i < nodeCount; i++) // by node id
  {
    for (uint8_t j = i; j > 0 && nodeIds[j - 1] > nodeIds[j]; j--)
    {
      const uint8_t swapId = nodeIds[j];
      const uint8_t swapFlags = nodeFlags[j];
      nodeIds[j] = nodeIds[j - 1];
      nodeFlags[j] = nodeFlags[j - 1];
      nodeIds[j - 1] = swapId;
      nodeFlags[j - 1] = swapFlags;
    }
  }
  view.nodes = nodeCount;
  for (uint8_t i = 0; i < nodeCount; i++)
  {
    snprintf(view.nodeName[i], sizeof(view.nodeName[i]), "G%u", nodeIds[i]);
    view.nodeFlags[i] = nodeFlags[i];
  }

  const uint8_t transport = settings.linkTransport < linkTransport_t_MAX ? settings.linkTransport : 0;
  if (heardCount)
  {
    snprintf(view.status, sizeof(view.status), "%u gauges linked" BULLET "%s", heardCount + 1, transportNames[transport]);
    view.state = UI_MS_OK;
  }
  else
  {
    snprintf(view.status, sizeof(view.status), "Looking for other gauges" BULLET "%s", transportNames[transport]);
    view.state = UI_MS_BUSY;
  }
  if (settings.rowPosition != 0) // 0.29.0: its number in the row too
  {
    snprintf(view.sub, sizeof(view.sub), "No. %u" BULLET "G%u%s%s" BULLET "%s", settings.rowPosition, linkNodeId(), // short: 48 characters at most
             settings.nodeName[0] ? " " : "", settings.nodeName, linkRoleName());
  }
  else
  {
    snprintf(view.sub, sizeof(view.sub), "This gauge: G%u%s%s" BULLET "%s", linkNodeId(), settings.nodeName[0] ? " " : "",
             settings.nodeName, linkRoleName());
  }

  const LinkStats &stats = linkStats();
  view.rows = 4;
  copyStr(view.rowName[0], sizeof(view.rowName[0]), "Link");
  snprintf(view.rowValue[0], sizeof(view.rowValue[0]), "%s%s%s", stats.canHeard ? "CAN" : "", stats.canHeard && stats.espNowUp ? " + " : "",
           stats.espNowUp ? "ESP-NOW" : (stats.canHeard ? "" : "-"));
  copyStr(view.rowName[1], sizeof(view.rowName[1]), "Frames in / out");
  snprintf(view.rowValue[1], sizeof(view.rowValue[1]), "%lu / %lu", (unsigned long)(stats.rxCan + stats.rxNow),
           (unsigned long)(stats.txCan + stats.txNow));
  copyStr(view.rowName[2], sizeof(view.rowName[2]), "Follow page");
  copyStr(view.rowValue[2], sizeof(view.rowValue[2]), settings.followPage ? "On" : "Off");
  copyStr(view.rowName[3], sizeof(view.rowName[3]), "Follow night");
  copyStr(view.rowValue[3], sizeof(view.rowValue[3]), settings.followNight ? "On" : "Off");
}

// ---- ADC: the adc2can sensors, four per page (board 1 ch 1-4, 5-8, board 2 ...)
static uint8_t adcPage = 0;
static volatile int8_t adcStep = 0;

static void adcView(UiModeView &view)
{
  copyStr(view.title, sizeof(view.title), "ADC");
  uint8_t pages[ADC2CAN_BOARDS * 2]; // board * 2 + half, for the boards heard
  uint8_t pageCount = 0;
  for (uint8_t boardIndex = 0; boardIndex < ADC2CAN_BOARDS; boardIndex++)
  {
    if (adc2canBoard(boardIndex).seen)
    {
      pages[pageCount++] = boardIndex * 2;
      pages[pageCount++] = boardIndex * 2 + 1;
    }
  }
  if (!pageCount)
  {
    snprintf(view.status, sizeof(view.status), "No adc2can heard (0x%08lX)", (unsigned long)settings.adcBaseId);
    view.state = UI_MS_BAD;
    copyStr(view.header, sizeof(view.header), "Not connected");
    copyStr(view.sub, sizeof(view.sub), "Plug an adc2can into the CAN chain");
    return;
  }

  const int8_t step = adcStep;
  adcStep = 0;
  adcPage = (uint8_t)((adcPage + pageCount + step) % pageCount);
  const uint8_t board = pages[adcPage] / 2;
  const uint8_t half = pages[adcPage] % 2;
  const Adc2canBoard &boardStatus = adc2canBoard(board);
  const bool trouble = boardStatus.railFaultMask || (boardStatus.flags & 0x39); // a rail fault, back-feed, an ADC / expander failure
  if (boardStatus.flags & 1)
  {
    copyStr(view.status, sizeof(view.status), "12 V on a sensor pin - rails off");
  }
  else if (boardStatus.railFaultMask)
  {
    snprintf(view.status, sizeof(view.status), "Rail fault (0x%02X)" BULLET "sensors %.2f V", boardStatus.railFaultMask, boardStatus.busV);
  }
  else
  {
    snprintf(view.status, sizeof(view.status), "%u/8 valid" BULLET "sensors %.2f V" BULLET "%.1f V", __builtin_popcount(boardStatus.validMask),
             boardStatus.busV, boardStatus.vbat);
  }
  view.state = trouble ? UI_MS_ERR : UI_MS_OK;
  snprintf(view.header, sizeof(view.header), "Board %u", board + 1);
  snprintf(view.sub, sizeof(view.sub), "Sensors %u-%u", half * 4 + 1, half * 4 + 4);
  if (pageCount > 2)
  {
    snprintf(view.hint, sizeof(view.hint), "%u of %u", adcPage + 1, pageCount);
  }

  view.rows = 4;
  for (uint8_t row = 0; row < 4; row++)
  {
    const uint8_t channel = half * 4 + row;
    const signalId_t id = (signalId_t)(SIG_ADC_1 + board * 8 + channel);
    copyStr(view.rowName[row], sizeof(view.rowName[row]), signalName(id));
    if (!((boardStatus.railOnMask >> channel) & 1))
    {
      copyStr(view.rowValue[row], sizeof(view.rowValue[row]), "Off");
    }
    else if ((boardStatus.railFaultMask >> channel) & 1)
    {
      copyStr(view.rowValue[row], sizeof(view.rowValue[row]), "FAULT");
    }
    else if (signalValid(id))
    {
      const char *unit = "";
      const float value = signalDisplayValue(id, signalGet(id), &unit);
      const int decimals = adc2canDecimals(board * 8 + channel); // the sensor's own decimals (adc2can or this gauge)
      snprintf(view.rowValue[row], sizeof(view.rowValue[row]), unit[0] ? "%.*f %s" : "%.*f", decimals,
               valueNoMinusZero(value, decimals), unit);
    }
    else
    {
      copyStr(view.rowValue[row], sizeof(view.rowValue[row]), "--");
    }
  }
}

// the last view drawn on the glass, for the web UI's Live Data (displayLiveView)
EXT_RAM_BSS_ATTR static UiModeView liveView;
static uint8_t liveItem = 0;
static uint32_t liveMs = 0;
static portMUX_TYPE liveMux = portMUX_INITIALIZER_UNLOCKED;

static void modeView(uint8_t item, UiModeView &view)
{
  switch (item)
  {
  case MENU_KLINE: klineView(view); break;
  case MENU_CAN: canView(view); break;
  case MENU_BLE: elmView(view); break;
  case MENU_OUTPUT: outputView(view); break;
  case MENU_CHAIN: chainView(view); break;
  case MENU_ADC: adcView(view); break;
  default: break;
  }
  portENTER_CRITICAL(&liveMux);
  liveView = view;
  liveItem = item;
  liveMs = millis();
  portEXIT_CRITICAL(&liveMux);
}

bool displayLiveView(UiModeView &view, uint8_t &item)
{
  portENTER_CRITICAL(&liveMux);
  const bool fresh = liveMs && millis() - liveMs < 1000;
  if (fresh)
  {
    view = liveView;
    item = liveItem;
  }
  portEXIT_CRITICAL(&liveMux);
  return fresh;
}

static void onModeSwipe(uint8_t item, int8_t direction)
{
  switch (item)
  {
  case MENU_KLINE:
    if (runtime.klineBrowse.connected)
    {
      runtime.klineBrowse.step = direction; // the K-line task reads the next block in that direction (skipping empty ones)
    }
    break;
  case MENU_CAN: canStep = direction; break;
  case MENU_BLE: obdStep = direction; break;
  case MENU_ADC: adcStep = direction; break;
  default: break;
  }
}

static bool onHaldexMode(uint8_t mode)
{
  return haldexRequestMode(mode);
} // CAN 0x6A0 (or via the chain master) / WiFi

// ---------------------------------------------------------------------------
// on-screen settings callbacks (Settings page on the glass)
// ---------------------------------------------------------------------------
static void onBrightness(uint8_t percent)
{
  (void)percent;
  displaySetBacklight(backlightTarget());
} // night dimming wins while active

static void onCanTermination(bool enabled)
{
  canTerminationApply(enabled);
}

// the glass's Units button: every unit to Metric or Imperial, each page's Min / Max / Redline turned over to match
// (0.27.0: they're in the shown unit now), then the pages rebuilt so the scales are relabelled
static void onUnits(bool imperial)
{
  const UnitChoices before = unitsOf(settings);
  if (imperial)
  {
    unitsSetQuick(UNITS_IMPERIAL);
  }
  else
  {
    unitsSetQuick(UNITS_METRIC);
  }
  unitsConvertPages(before);
  uiRebuildPages();
}

static void onReboot()
{
  writeEEP();
  delay(100);
  ESP.restart();
}

static void onSettingsChanged()
{
  writeEEP();
}

static void onPageChanged(uint8_t page)
{
  linkPageChanged(page);
}

// 0.24.2: when the touch controller doesn't answer at its address (CST8xx, 0x15), list every address on the I2C bus
// that does, so the boot log shows whether the glass's touch chip is there at another address (0x20 is the TCA9554)
// 0.24.28: bench only (debugI2cScan in can2gauge_defs.h): it ran at every boot, before the screen came on
#if debugI2cScan
static void scanI2cBus()
{
  char found[100] = "";
  uint8_t count = 0;
  for (uint8_t address = 1; address < 127; address++)
  {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) // 0 = something answered (ACK) at this address
    {
      char one[8];
      snprintf(one, sizeof(one), " 0x%02X", address);
      if (strlen(found) + strlen(one) < sizeof(found))
      {
        strcat(found, one);
      }
      count++;
    }
  }
  DEBUG("I2C scan: %u device(s) answered:%s", count, found);
}
#endif

void setupDisplay()
{
  DisplayPins pins = {LCD_PIN_SPI_SCK, LCD_PIN_SPI_SDA, LCD_PIN_HSYNC, LCD_PIN_VSYNC, LCD_PIN_DE, LCD_PIN_PCLK, {LCD_PIN_D0, LCD_PIN_D1, LCD_PIN_D2, LCD_PIN_D3, LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7, LCD_PIN_D8, LCD_PIN_D9, LCD_PIN_D10, LCD_PIN_D11, LCD_PIN_D12, LCD_PIN_D13, LCD_PIN_D14, LCD_PIN_D15}, LCD_PIN_BL_PWM, EXIO_LCD_RST, EXIO_LCD_CS,
#ifdef CAN2GAUGE_WAVESHARE
                      DISPLAY_PANEL_WAVESHARE};
#else
                      DISPLAY_PANEL_YOURITECH};
#endif
  c2gSimdEnable(settings.simdBlend); // the S3 SIMD fills / copies (include/lv_blend_esp32s3.h)
  if (!lvglInit(pins, CST8XX_ADDR, TP_PIN_INT, EXIO_TP_RST, settings.renderMode == 1))
  {
    // Carry on without the screen, so CAN, K-line, WiFi, the web UI and OTA still come up (and the web UI's Diag tab
    // shows how far the display got). Every later drawing call is skipped: lvglLock() refuses while LVGL isn't running.
    DEBUG("display init FAILED (step %u of %u) - running without the screen", displayHealth().step, DISPLAY_STEP_READY);
    return;
  }
  DEBUG("panel + LVGL up at %lu ms", millis()); // 0.24.28: boot timing (ms since power-up)
  DEBUG("touch %s (id 0x%02X)", lvglTouchPresent() ? "ok" : "not found", touchChipId());
#if debugI2cScan
  if (!lvglTouchPresent())
  {
    scanI2cBus(); // where is it, if it's there at all?
  }
#endif
  if (settings.renderMode == 0 && lvglDirect())
  {
    DEBUG("drawing direct: not enough internal RAM left for partial mode's strips"); // lib/LVGL_Driver PARTIAL_MODE_RESERVE
  }
  // 0.24.4: how the screen is being drawn, and why
  if (lvglDirectRefused())
  {
    DEBUG("frame buffers (0x%08X, 0x%08X) aren't both %u-byte aligned: LVGL can't draw into them, drawing in strips instead",
          lvglFrameBufferAddress(), lvglFrameBuffer2Address(), LV_DRAW_BUF_ALIGN);
  }
  if (lvglDirect())
  {
    DEBUG("drawing: direct into the frame buffers (0x%08X, 0x%08X)", lvglFrameBufferAddress(), lvglFrameBuffer2Address());
  }
  else if (lvglPartialInPsram())
  {
    DEBUG("drawing: strips in PSRAM (frame buffer 0x%08X)", lvglFrameBufferAddress());
  }
  else
  {
    DEBUG("drawing: strips in internal RAM (frame buffer 0x%08X)", lvglFrameBufferAddress());
  }
#if SCREEN_ROTATION
  // 0.25.1: Upside Down / Rotation Trim. Set before the pages are built, so the canvas gets its PSRAM before the sprites.
  lvglSetRotation(settings.upsideDown == 1, settings.rotateTrim / 2.0f);
  if (lvglRotationState() == ROTATION_ON)
  {
    DEBUG("screen rotation: upside down %u, trim %.1f degrees", settings.upsideDown, settings.rotateTrim / 2.0f);
  }
  else if (lvglRotationState() == ROTATION_NO_ROOM)
  {
    DEBUG("screen rotation: no PSRAM for the canvas - drawn straight");
  }
  else if (lvglRotationState() == ROTATION_PARTIAL)
  {
    DEBUG("screen rotation: not in partial drawing mode - drawn straight");
  }
#endif

  UiCallbacks callbacks = {onBrightness, onCanTermination, onUnits, onReboot, onSettingsChanged, onPageChanged, onModeSwipe, onHaldexMode};
  uiInit(callbacks);
  DEBUG("gauge pages built at %lu ms", millis());
  uiSetInfoProvider(UI_INFO_HALDEX, haldexInfo);
  uiSetInfoProvider(UI_INFO_CAN, canInfo);
  uiSetInfoProvider(UI_INFO_KLINE, klineInfoProvider);
  uiSetInfoProvider(UI_INFO_ADC, ioInfoProvider);
  uiSetInfoProvider(UI_INFO_BLE, elmInfoProvider);
  uiSetInfoProvider(UI_INFO_CHAIN, linkInfoProvider);
  uiSetMenuStatus(menuStatus);
  uiSetModeProvider(modeView);
  lvglStartTask();
  if (runtime.wokeFromSleep && settings.shutdownSprite[0])
  {
    uiStandby(false); // ignition back on after deep sleep: the shutdown pack's wake clip, then the gauge
  }
  else
  {
    uiShowBootLogo();
  }
  // 0.24.29: draw the boot screen's first frame now, before the backlight comes on. Before, the light came on as soon as
  // the logo was asked for, and until the LVGL task got round to drawing it (a frame or two) the glass showed what the
  // frame buffer still held - the first gauge page, drawn behind it: the smudge just before the logo.
  if (lvglLock(200))
  {
    lv_refr_now(lvglDisplay()); // draws the whole screen now and puts it on the glass
    lvglUnlock();
  }
  displaySetBacklight(backlightTarget());
  DEBUG("backlight on at %lu ms", millis());
}
