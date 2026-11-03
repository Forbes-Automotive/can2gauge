#include <can2gauge_KLine.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <KLine.h>
#include <ME7Logger.h>
#include <can2gauge_ME7Fast.h> // 0.19.0: the B7 RAM-routine install/hook, for the fast-path bench diagnostic
#include <can2gauge_ME7Read.h>
#include <can2gauge_Labels.h> // measuring-block labels for K-line mode
#include <can2gauge_UserSig.h> // gauge pages with a K-line source
#include <can2gauge_Service.h> // 0.16.0: fault codes / basic settings from the web UI take the line
#include <CANLine.h> // the shared OBD-II PID picker and decoder (KWP2000 / OBD mode)
#include <can2gauge_Vehicle.h> // 0.35.0: a VIN in a text block (ME7 block 081) goes to the vehicle module
#include <can2gauge_CarScan.h> // 0.37.0: Scan My Car's K-line jobs take the line like a service job
#include <LittleFS.h> // ME7 .ecu/.cfg definition files + CSV logs // ME7 flash read (optional feature; remove this include + the me7ReadService() call to drop it)

static TaskHandle_t klineTask = NULL;
static volatile bool restartFlag = false;
// 0.24.11: the background protocol stops trying after KLINE_GIVE_UP_TRIES wake-ups in a row get no answer, if no ECU has
// answered since power-up - a gauge with nothing on its K-line no longer tries for ever (each try holds the line ~3 s,
// and on the bench it never ends). Once an ECU has answered it keeps trying (the car may only be switched off). The
// K-line settings changing, the web UI's reconnect, or an ignition off / on cycle start it again.
#define KLINE_GIVE_UP_TRIES 3
static volatile uint8_t noEcuTries = 0;      // wake-ups in a row that got no answer (volatile: cleared by the web task)
static volatile bool klineStopped = false;   // gave up: the background protocol waits for a restart
static bool klineEverConnected = false;      // an ECU has answered since power-up
static volatile bool logRequest = false;
static volatile bool logStopRequest = false;
static char lastError[64] = "";

// KWP1281 measuring blocks for a Mk4 ME7.x (1.8T, 06A-906-032-AWP / BAM / AMU label files; fields 0-based here):
// group 1 = rpm, coolant, lambda %, basic-setting bits; group 3 = rpm, MAF, throttle, ignition; group 4 = rpm, supply
// voltage, coolant, intake air; group 5 = rpm, load, speed, load status; group 115 = rpm, load, boost specified, boost
// actual (mbar). Other ECUs differ - see docs/FIRMWARE.md.
static const KwpGroupMap groupMap[] = {
    {1, 0, SIG_RPM}, {1, 1, SIG_COOLANT}, {3, 1, SIG_MAF}, {3, 2, SIG_THROTTLE}, {3, 3, SIG_TIMING}, {4, 1, SIG_BATTERY}, {4, 3, SIG_IAT}, {5, 1, SIG_LOAD}, {5, 2, SIG_SPEED}, {115, 3, SIG_BOOST}};
static const uint8_t engineGroups[] = {1, 3, 4, 115}; // speed / load (group 5) normally come from CAN

uint8_t klineEngineGroups(const uint8_t **groups)
{
  *groups = engineGroups;
  return sizeof(engineGroups);
}

void klineApplyGroup(uint8_t group, const float *values, uint8_t count)
{
  for (const KwpGroupMap &entry : groupMap)
  {
    if (entry.group == group && entry.field < count)
    {
      signalSet((signalId_t)entry.signal, values[entry.field]);
    }
  }
}

static void setState(const char *state)
{
  copyStr(runtime.klineState, sizeof(runtime.klineState), state);
}

// 0.17.4: the shared KWP1281 session (defined after klineSessionList, further down)
static bool kwp1281Open(uint8_t addr, char *id, size_t idLen);
static void kwp1281Release(uint8_t addr);
static void kwp1281CloseOpen();
// 0.19.0: protocol-aware session for the measuring-block modes (dashboard groups, K-line browse, gauge pages).
// Routes to the faster VAG KWP2000 when the protocol is KWP2000-VAG and the target is the engine (0x01), else KWP1281.
static bool klineModeOpen(uint8_t addr, char *id, size_t idLen);
static bool klineModeConnected();
static bool klineModeReadGroup(uint8_t group, Kwp1281Group &out);
static void klineModeRelease(uint8_t addr);
// 0.39.6: menu > K-line when the engine doesn't answer at 0x01: OBD-II at 0x33 (defined after runKwp2000, further down)
static bool runKlineBrowseObd();
static bool isBrowseObdFirst = false; // 0x01 didn't answer and 0x33 did last time: 0x33 is tried first next time

// 0.17.3: one more sample read, and the samples per second the K-line tab and the glass show (runtime.me7SampleRate,
// the name it had when only the ME7 logger filled it). Before, only the KWP2000 and ME7 loops worked the rate out, so
// KWP1281, K-line mode, gauge pages and the oil page showed a sample count but 0/s.
static uint32_t rateSecondStartMs = 0;        // when the current second started
static uint32_t rateSamplesAtSecondStart = 0; // runtime.klineSamples at that moment
static void countSample()
{
  runtime.klineSamples++;
  const uint32_t now = millis();
  if (now - rateSecondStartMs >= 1000)
  {
    runtime.me7SampleRate = runtime.klineSamples - rateSamplesAtSecondStart;
    rateSamplesAtSecondStart = runtime.klineSamples;
    rateSecondStartMs = now;
  }
}

// 0.17.3: a session starts or ends: the rate starts again from 0
static void resetSampleRate()
{
  rateSecondStartMs = millis();
  rateSamplesAtSecondStart = runtime.klineSamples;
  runtime.me7SampleRate = 0;
}

// ---------------------------------------------------------------------------
// K-line ownership. One session at a time: the oil page (STYLE_OIL, oil source = K-line) owns the line while it is on
// screen - GaugeUI stamps runtime.oilPageSeenMs on every refresh - and for oilReleaseMs after it leaves (a menu peek or a
// swipe back does not cost a new 5-baud init). Otherwise the configured protocol (KWP1281 engine / KWP2000 / ME7
// logger) runs. Every session is closed with its proper end (KWP1281 end-output block, KWP2000 StopCommunication)
// before the other one starts, so nothing is left open on the line.
// ---------------------------------------------------------------------------
static bool oilWanted()
{
  const uint32_t seen = runtime.oilPageSeenMs;
  return settings.oilSource == OIL_SRC_KLINE && seen && millis() - seen < oilReleaseMs;
}

static bool klineModeWanted() // menu > K-line on the glass, or the web UI's live measuring blocks
{
  const uint32_t seen = runtime.klineModeSeenMs;
  return seen && millis() - seen < MODE_RELEASE_MS;
}

static bool klinePageWanted() // a gauge page with a K-line source is on the glass
{
  const uint32_t seen = runtime.klinePageSeenMs;
  return seen && millis() - seen < MODE_RELEASE_MS && userSigPageUses(runtime.klinePageCfgIndex, USRC_KLINE);
}

// 0.15.0: the one module that every K-line value on every page in the rotation comes from, or 0 when the session can't
// stay open all the time. With a module here the K-line page session runs whether or not its page is showing, so
// swiping to a K-line page shows a value straight away instead of waiting 2-3 s for the 5-baud init.
// It can't when: the ignition is off, the oil page (cluster over K-line) is in the rotation, the K-line tab runs KWP2000
// or the ME7 logger (a different kind of session), the K-line tab's KWP1281 talks to another module, or the pages use
// more than one module (the K-line is one wire: one session to one module at a time).
static uint8_t rotationKlineModule()
{
  if (runtime.standby)
  {
    return 0; // ignition off: the line stays quiet
  }
  if (settings.klineProtocol != KL_OFF && settings.klineProtocol != KL_KWP1281)
  {
    return 0; // KWP2000 / ME7 logger can't share a KWP1281 session
  }
  uint8_t module = 0;
  for (uint8_t pageIndex = 0; pageIndex < MAX_PAGES; pageIndex++)
  {
    const GaugePageCfg &page = settings.pages[pageIndex];
    if (!page.enabled)
    {
      continue; // not in the rotation
    }
    if (page.style == STYLE_OIL && settings.oilSource == OIL_SRC_KLINE)
    {
      return 0; // the oil page needs its own session to the cluster
    }
    // the page's values: main, second, and for Quad / Custom the three slots
    uint8_t ids[5];
    ids[0] = page.signal;
    ids[1] = page.signal2;
    ids[2] = SIG_NONE;
    ids[3] = SIG_NONE;
    ids[4] = SIG_NONE;
    if (page.style == STYLE_QUAD || page.style == STYLE_TRIPLE || page.style == STYLE_CUSTOM)
    {
      ids[2] = settings.pages[pageIndex].slots[0];
      ids[3] = settings.pages[pageIndex].slots[1];
      ids[4] = settings.pages[pageIndex].slots[2];
    }
    for (uint8_t i = 0; i < 5; i++)
    {
      const UserSigCfg *signalConfigPointer = userSigConfig((signalId_t)ids[i]);
      if (signalConfigPointer == NULL)
      {
        continue; // not a page-source value
      }
      const UserSigCfg &signalConfig = *signalConfigPointer; // the value's settings, read with dots
      if (signalConfig.source != USRC_KLINE)
      {
        continue;
      }
      if (module == 0)
      {
        module = signalConfig.klineAddr; // the first K-line value sets the module
      }
      else if (signalConfig.klineAddr != module)
      {
        return 0; // two different modules: can't hold both
      }
    }
  }
  // 0.35.0: the switched-on triggers' K-line values count too (their page may be switched off), so a trigger on a K-line
  // value keeps the session open and its value coming
  for (uint8_t triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    const TriggerCfg &trigger = settings.triggers[triggerIndex];
    if (!trigger.enabled)
    {
      continue;
    }
    const UserSigCfg *signalConfigPointer = userSigConfig((signalId_t)trigger.signal);
    if (signalConfigPointer == NULL)
    {
      continue; // one of the gauge's own signals
    }
    const UserSigCfg &signalConfig = *signalConfigPointer; // the value's settings, read with dots
    if (signalConfig.source != USRC_KLINE)
    {
      continue;
    }
    if (module == 0)
    {
      module = signalConfig.klineAddr;
    }
    else if (signalConfig.klineAddr != module)
    {
      return 0; // a trigger on another module: can't hold both
    }
  }
  if (module != 0 && settings.klineProtocol == KL_KWP1281 && settings.klineEcuAddr != module)
  {
    return 0; // the K-line tab's KWP1281 wants a different module
  }
  return module;
}

// who should have the line right now: the oil page, the K-line mode screen, a page with a K-line source (or every page
// in the rotation sharing one module), then the configured protocol (only one screen shows at a time, so the first
// three never compete)
enum lineOwner_t : uint8_t
{
  OWN_BG = 0, // settings.klineProtocol (KWP1281 engine groups / KWP2000 / ME7 logger / off)
  OWN_OIL,
  OWN_KLMODE,
  OWN_PAGE,
  OWN_SERVICE, // 0.16.0: a fault read / clear or basic setting from the web UI's Service tab (src/can2gauge_Service.cpp)
  OWN_QUIET,   // 0.37.0: Enable Other Scanners: nobody - every session ends and the line stays quiet
};
static lineOwner_t wantedOwner()
{
  if (runtime.otherScanners)
  {
    return OWN_QUIET; // 0.37.0: another scan tool has the car: whatever runs now ends
  }
  if (serviceKlinePending() || carScanKlinePending()) // 0.37.0: a car scan job takes the line the same way
  {
    return OWN_SERVICE; // a service job comes first: every other session closes and hands the line over
  }
  if (oilWanted())
  {
    return OWN_OIL;
  }
  if (klineModeWanted())
  {
    return OWN_KLMODE;
  }
  // 0.17.6: with the ME7 logger picked on the K-line tab, the logger keeps the line: a gauge page with a K-line block
  // source no longer takes it (on the bench, gauge 1 = engine block 1 held a KWP1281 session and the logger never
  // started). The page shows "ME7 logger has the K-line" instead (userSigNote). K-line mode, the oil page and the
  // Service tab still take over while they're in use (above) and hand back after.
  if (settings.klineProtocol == KL_ME7LOGGER)
  {
    return OWN_BG;
  }
  if (klinePageWanted())
  {
    return OWN_PAGE;
  }
  if (rotationKlineModule() != 0)
  {
    return OWN_PAGE; // 0.15.0: the rotation's K-line values all come from one module: keep its session open
  }
  return OWN_BG;
}

// sleeps up to ms; returns early (true) on a restart request or when another owner claims the line
static bool idleFor(uint32_t ms, lineOwner_t owner)
{
  const uint32_t start = millis();
  while (millis() - start < ms)
  {
    if (restartFlag || wantedOwner() != owner)
    {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return false;
}

// 0.24.11: the background protocol's wake-up got no answer: wait and try again, or give up (see KLINE_GIVE_UP_TRIES)
static void backgroundNoEcu(const char *state)
{
  noEcuTries = noEcuTries + 1; // volatile: an explicit read + write
  if (!klineEverConnected && noEcuTries >= KLINE_GIVE_UP_TRIES)
  {
    klineStopped = true;
    setState("not found - stopped");
    DEBUG("K-line: no ECU after %u tries - stopped until the K-line settings change or the ignition goes off and on",
          (unsigned)noEcuTries);
    return;
  }
  setState(state);
  idleFor(3000, OWN_BG);
}

// 0.24.11: the background protocol is connected: it keeps trying from now on
static void backgroundConnected()
{
  noEcuTries = 0;
  klineEverConnected = true;
}

static void oilState(const char *fmt, ...)
{
  char buf[sizeof(runtime.oil.state)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  copyStr(runtime.oil.state, sizeof(runtime.oil.state), buf);
}

// measuring block layout from the cluster part number (label files 1J0-920-xx0/xx5-17, 3B0-920-xx2/xx5-17:
// group 3 = coolant, oil level Ok/n.Ok, oil temp; 1J0-919, 8N, 1C0 and the rest: group 50 field 3 = oil temp, no level)
static void oilLayout(const char *clusterId, uint8_t &group, uint8_t &tempField, uint8_t &levelField)
{
  if (settings.oilGroup)
  {
    group = settings.oilGroup;
    tempField = settings.oilTempField;
    levelField = settings.oilLevelField;
    return;
  }
  const bool group3Layout = (!strncmp(clusterId, "1J", 2) || !strncmp(clusterId, "3B", 2)) && !strncmp(clusterId + 3, "920", 3);
  group = group3Layout ? 3 : 50;
  tempField = 2;
  levelField = group3Layout ? 1 : 0xFF;
}

static void runOilCluster()
{
  OilStatus &oil = runtime.oil;
  const uint8_t addr = settings.oilClusterAddr;
  runtime.klineUp = false;
  oil.connected = false;
  oil.connecting = true;
  oil.fieldCount = 0;
  setState("oil: cluster init");
  oilState("Connecting to cluster 0x%02X", addr);
  char id[sizeof(oil.clusterId)];
  if (!kwp1281Open(addr, id, sizeof(id))) // 0.17.4: carries on with an open session to the cluster, if there is one
  {
    oil.connecting = false;
    oil.errors++;
    setState("oil: no cluster");
    oilState("No answer from 0x%02X - retrying", addr);
    DEBUG("oil: cluster 0x%02X did not answer", addr);
    idleFor(3000, OWN_OIL);
    return;
  }
  copyStr(oil.clusterId, sizeof(oil.clusterId), id);
  copyStr(runtime.klineEcuId, sizeof(runtime.klineEcuId), id);
  uint8_t group;
  uint8_t tempField;
  uint8_t levelField;
  oilLayout(id, group, tempField, levelField);
  oil.group = group;
  oil.tempField = tempField;
  oil.levelField = levelField;
  oil.connecting = false;
  oil.connected = true;
  oil.connects++;
  runtime.klineUp = true;
  setState("oil: cluster up");
  oilState("Reading group %u", group);
  DEBUG("oil: %s, group %u temp field %u level field %u", id, group, tempField + 1, levelField == 0xFF ? 0 : levelField + 1);

  uint32_t lastExchange = millis();
  while (kwp1281Connected() && wantedOwner() == OWN_OIL && !restartFlag)
  {
    Kwp1281Group block;
    if (!kwp1281ReadGroup(group, block))
    {
      oil.errors++;
      break; // session lost (ReadGroup closed it)
    }
    lastExchange = millis();
    oil.reads++;
    countSample();
    oil.fieldCount = block.count;
    for (uint8_t field = 0; field < block.count; field++)
    {
      oil.formula[field] = block.fields[field].formula;
      oil.a[field] = block.fields[field].a;
      oil.b[field] = block.fields[field].b;
      oil.value[field] = block.fields[field].value;
      copyStr(oil.unit[field], sizeof(oil.unit[field]), block.fields[field].unit);
    }
    if (block.count <= tempField)
    {
      // auto layout guessed group 3 on a cluster without it: fall back to group 50 once
      if (!settings.oilGroup && group == 3)
      {
        oil.group = 50;
        group = 50;
        oil.levelField = 0xFF;
        levelField = 0xFF;
        oilState("Group 3 empty - using group 50");
        continue;
      }
      oilState("Group %u has no field %u", group, tempField + 1);
    }
    else
    {
      signalSet(SIG_OIL_TEMP, block.fields[tempField].value);
      if (levelField < block.count)
      {
        const bool ok = settings.oilLevelOkZero ? block.fields[levelField].b == 0 : block.fields[levelField].b != 0;
        signalSet(SIG_OIL_LEVEL, ok ? 1.0f : 0.0f);
      }
      oilState("Cluster 0x%02X  group %u", addr, group);
    }
    // gap between reads; the cluster drops the session after ~1 s without a block, so ACK it on longer waits
    const uint32_t gapStart = millis();
    while (millis() - gapStart < settings.oilPollMs && wantedOwner() == OWN_OIL && !restartFlag)
    {
      if (millis() - lastExchange > 700)
      {
        if (!kwp1281KeepAlive())
        {
          break;
        }
        lastExchange = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  const bool lost = !kwp1281Connected();
  kwp1281Release(addr); // 0.17.4: stays open if the next user wants the cluster too, otherwise the end block
  oil.connected = false;
  oil.connecting = false;
  runtime.klineUp = false;
  if (lost && wantedOwner() == OWN_OIL)
  {
    setState("oil: lost");
    oilState("Connection lost - reconnecting");
    idleFor(1000, OWN_OIL);
  }
  else
  {
    setState("oil: closed");
    oilState("Closed");
    DEBUG("oil: cluster session closed (%s)", restartFlag ? "restart" : "oil page left");
  }
}

// ---------------------------------------------------------------------------
// K-line mode (menu > K-line, or the web UI's live measuring blocks): a KWP1281 session to the module chosen in the
// web UI (settings.klineEcuAddr: 01 engine, 17 instruments, 03 ABS ...) while that screen is showing. Shows each step
// of the connection, the module's identification, then one measuring block at a time; a swipe steps the block, and an
// empty block moves on in the same direction until one answers.
// ---------------------------------------------------------------------------
static void klineBrowseState(const char *fmt, ...)
{
  char buf[sizeof(runtime.klineBrowse.state)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  copyStr(runtime.klineBrowse.state, sizeof(runtime.klineBrowse.state), buf);
}

static uint8_t klineBrowseNext(uint8_t group, int8_t dir) // blocks 1..255, wrapping
{
  int next = (int)group + dir;
  if (next > 255)
  {
    next = 1;
  }
  if (next < 1)
  {
    next = 255;
  }
  return (uint8_t)next;
}

// the label files that fit the connected module; loads the best one
static void klineBrowseLabels(KlineBrowse &browse)
{
  browse.labelCandidateCount = labelsFind(browse.addr, browse.ecuId, browse.labelCandidates, LBL_MAX_CANDS);
  copyStr(browse.labelFile, sizeof(browse.labelFile), browse.labelCandidateCount ? browse.labelCandidates[0] : "");
  if (!labelsLoad(browse.labelFile))
  {
    browse.labelFile[0] = '\0';
  }
  browse.title[0] = '\0';
  for (uint8_t field = 0; field < 4; field++)
  {
    browse.fieldName[field][0] = '\0';
    browse.fieldSpec[field][0] = '\0';
  }
  DEBUG("K-line mode: labels %s (%u fit)", browse.labelFile[0] ? browse.labelFile : "none", browse.labelCandidateCount);
  labelsRemember(browse.addr, browse.labelFile, browse.ecuId);
}

// the module menu > K-line reads: settings.klineModeAddr, or the engine (0x01) when that is 0
static uint8_t klineModeAddress()
{
  if (settings.klineModeAddr == 0)
  {
    return 0x01;
  }
  return settings.klineModeAddr;
}

static void runKlineBrowse()
{
  KlineBrowse &browse = runtime.klineBrowse;
  const uint8_t addr = klineModeAddress();
  runtime.klineUp = false;
  if (browse.addr != addr) // another module: start at its first block
  {
    browse.group = 1;
  }
  browse.addr = addr;
  browse.connected = false;
  browse.connecting = true;
  browse.searching = false;
  browse.isObd = false;
  browse.fieldCount = 0;
  browse.ecuId[0] = '\0';
  if (browse.group == 0)
  {
    browse.group = 1;
  }
  // 0.39.6: an engine that only answered OBD-II at 0x33 last time is asked there first (no wait for 0x01 every time)
  if (addr == 0x01 && isBrowseObdFirst && runKlineBrowseObd())
  {
    return;
  }
  setState("K-line mode: init");
  klineBrowseState("Waking module 0x%02X (5-baud init)", addr);
  char id[sizeof(browse.ecuId)];
  if (!klineModeOpen(addr, id, sizeof(id))) // 0.19.0: KWP2000-VAG if chosen + engine, else carries on / opens KWP1281
  {
    browse.connecting = false;
    browse.errors++;
    // 0.39.6: the engine doesn't answer at 0x01 (not a VW, or one that only does OBD-II on the K-line): OBD-II at 0x33,
    // the address First Boot and Scan My Car try too. Another module picked: it's that module or nothing.
    if (addr == 0x01 && !isBrowseObdFirst && runKlineBrowseObd())
    {
      isBrowseObdFirst = true;
      return;
    }
    setState("K-line mode: no answer");
    if (addr == 0x01)
    {
      klineBrowseState("No answer at 0x01 or OBD-II 0x33 - retrying");
    }
    else
    {
      klineBrowseState("No answer from 0x%02X - retrying", addr);
    }
    idleFor(3000, OWN_KLMODE);
    return;
  }
  if (addr == 0x01)
  {
    isBrowseObdFirst = false; // the engine answers at 0x01 now: its measuring blocks first again
  }
  copyStr(browse.ecuId, sizeof(browse.ecuId), id);
  copyStr(runtime.klineEcuId, sizeof(runtime.klineEcuId), id);
  klineBrowseLabels(browse);
  browse.connecting = false;
  browse.connected = true;
  runtime.klineUp = true;
  setState("K-line mode: connected");
  klineBrowseState("Connected - reading block %u", browse.group);
  DEBUG("K-line mode: 0x%02X %s", addr, id);

  int8_t direction = 1;
  uint16_t emptyBlocks = 0; // empty blocks in a row while searching
  while (klineModeConnected() && wantedOwner() == OWN_KLMODE && !restartFlag && klineModeAddress() == addr)
  {
    if (browse.gotoGroup >= 0)
    {
      browse.group = (uint8_t)browse.gotoGroup;
      browse.gotoGroup = -1;
      emptyBlocks = 0;
      browse.fieldCount = 0; // 0.17.4: the old block's values are not shown under the new number while it's read
      browse.text[0] = '\0';
      browse.blockMs = 0;
    }
    if (browse.labelReload)
    {
      browse.labelReload = false;
      klineBrowseLabels(browse);
    }
    if (browse.step)
    {
      direction = browse.step > 0 ? 1 : -1;
      browse.step = 0;
      browse.group = klineBrowseNext(browse.group, direction);
      emptyBlocks = 0;
      browse.fieldCount = 0; // 0.17.4: as for a goto above
      browse.text[0] = '\0';
      browse.blockMs = 0;
    }
    Kwp1281Group block;
    if (!klineModeReadGroup(browse.group, block))
    {
      browse.errors++;
      break; // session lost (ReadGroup closed it)
    }
    browse.reads++;
    countSample();
    if (block.count == 0 && block.text[0] == '\0') // the module has no such block: keep going the way the user swiped
    {
      browse.searching = true;
      browse.fieldCount = 0;
      browse.text[0] = '\0';
      if (++emptyBlocks >= 255)
      {
        klineBrowseState("No measuring blocks answer");
        idleFor(1000, OWN_KLMODE);
        emptyBlocks = 0;
        continue;
      }
      klineBrowseState("Block %u empty - trying %u", browse.group, klineBrowseNext(browse.group, direction));
      browse.group = klineBrowseNext(browse.group, direction);
      continue;
    }
    emptyBlocks = 0;
    browse.searching = false;
    copyStr(browse.text, sizeof(browse.text), block.text); // 0.17.4: a text block's text ("" for a normal block)
    if (block.text[0])
    {
      vehicleTakeText(block.text, VIN_SRC_KLINE); // 0.35.0: ME7 block 081 is the VIN and the immobiliser ID
    }
    for (uint8_t field = 0; field < block.count && field < 4; field++)
    {
      browse.formula[field] = block.fields[field].formula;
      browse.a[field] = block.fields[field].a;
      browse.b[field] = block.fields[field].b;
      browse.value[field] = block.fields[field].value;
      copyStr(browse.unit[field], sizeof(browse.unit[field]), block.fields[field].unit);
    }
    labelsField(browse.group, 0, browse.title, sizeof(browse.title), NULL, 0);
    for (uint8_t field = 0; field < 4; field++)
    {
      labelsField(browse.group, field + 1, browse.fieldName[field], sizeof(browse.fieldName[field]), browse.fieldSpec[field],
                  sizeof(browse.fieldSpec[field]));
    }
    browse.fieldCount = block.count;
    browse.blockMs = millis();
    klineBrowseState("Connected - block %u", browse.group);
    vTaskDelay(pdMS_TO_TICKS(20)); // back-to-back reads keep the session alive
  }
  const bool lost = !klineModeConnected();
  klineModeRelease(addr); // 0.19.0: ends the KWP2000 session, or releases/keeps the shared KWP1281 one
  browse.connected = false;
  browse.connecting = false;
  runtime.klineUp = false;
  if (lost && wantedOwner() == OWN_KLMODE)
  {
    setState("K-line mode: lost");
    klineBrowseState("Connection lost - reconnecting");
    idleFor(1000, OWN_KLMODE);
  }
  else
  {
    setState("K-line mode: closed");
    klineBrowseState("Closed");
  }
}

// ---------------------------------------------------------------------------
// gauge page with a K-line source (web UI > Gauges > Source K-line): while the page shows, a session to its module and
// the page's block(s) read back to back - the main value and, for Dual, a second field from the same module (same
// block: one read serves both). Names come from the module's label file, units from the KWP1281 formula.
// 0.15.0: when every page in the rotation takes its K-line values from one module (rotationKlineModule), the session
// stays open all the time and reads every page's blocks, so a swipe never waits for the 5-baud init.
// 0.35.0: a switched-on trigger's K-line value counts like a page in the rotation, so the trigger keeps its value. When
// the values come from more than one module only the page on screen's module is read (one wire, one session), and a
// trigger's value is read only while that's its module - the Triggers tab says so.
// ---------------------------------------------------------------------------
static void klinePageState(const char *fmt, ...)
{
  char buf[sizeof(runtime.klinePage.state)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  copyStr(runtime.klinePage.state, sizeof(runtime.klinePage.state), buf);
}

// one K-line value the session reads: which gauge signal it fills, and where it sits on the module
// (all uint8_t, so two lists can be compared byte for byte with memcmp to spot a settings change)
struct KlineValue
{
  uint8_t signal;      // signalId_t of the page value (a user signal)
  uint8_t pageIndex;   // the page it's on (index in settings.pages)
  uint8_t addr;        // module address
  uint8_t group;       // measuring block
  uint8_t field;       // field in the block, 0..3
  uint8_t isMainValue; // 1 = the page's main value: it keeps the page's own label (no rename from the label file)
};
#define KLINE_VALUES_MAX (MAX_PAGES * 5 + MAX_TRIGGERS)      // main, second and three slots on every page, and every trigger (0.35.0)
#define KLINE_GROUPS_MAX (KLINE_VALUES_MAX + sizeof(engineGroups)) // every value's block, plus the engine blocks

// the session's lists (static: kept off the K-line task's stack, only that task uses them)
static KlineValue sessionValues[KLINE_VALUES_MAX]; // the values the open session reads
static KlineValue currentValues[KLINE_VALUES_MAX]; // the same list built again every round: different = settings changed
static uint8_t sessionGroups[KLINE_GROUPS_MAX];    // the blocks to read, each once

// adds page pageIndex's K-line values to the list: main, second, and for Quad / Custom the three slots
static void addPageKlineValues(uint8_t pageIndex, KlineValue *values, uint8_t &count)
{
  if (pageIndex >= MAX_PAGES)
  {
    return;
  }
  const GaugePageCfg &page = settings.pages[pageIndex];
  uint8_t ids[5];
  ids[0] = page.signal;  // main value
  ids[1] = page.signal2; // second value
  ids[2] = SIG_NONE;
  ids[3] = SIG_NONE;
  ids[4] = SIG_NONE;
  if (page.style == STYLE_QUAD || page.style == STYLE_TRIPLE || page.style == STYLE_CUSTOM)
  {
    ids[2] = settings.pages[pageIndex].slots[0]; // Quad values 2-4 / the Custom centre stack
    ids[3] = settings.pages[pageIndex].slots[1];
    ids[4] = settings.pages[pageIndex].slots[2];
  }
  for (uint8_t i = 0; i < 5 && count < KLINE_VALUES_MAX; i++)
  {
    const UserSigCfg *signalConfigPointer = userSigConfig((signalId_t)ids[i]);
    if (signalConfigPointer == NULL)
    {
      continue; // not a page-source value
    }
    const UserSigCfg &signalConfig = *signalConfigPointer; // the value's settings, read with dots
    if (signalConfig.source != USRC_KLINE)
    {
      continue;
    }
    values[count].signal = ids[i];
    values[count].pageIndex = pageIndex;
    values[count].addr = signalConfig.klineAddr;
    values[count].group = signalConfig.klineGroup;
    values[count].field = signalConfig.klineField;
    values[count].isMainValue = 0;
    if (i == 0)
    {
      values[count].isMainValue = 1;
    }
    count++;
  }
}

// 0.35.0: adds the K-line values the switched-on triggers watch, so a trigger keeps its value coming when its page isn't
// showing or is switched off. onlyAddr = 0: any module; otherwise only values from that module (the line talks to one
// module at a time). A value already on the list (on a page too) isn't added twice.
static void addTriggerKlineValues(KlineValue *values, uint8_t &count, uint8_t onlyAddr)
{
  for (uint8_t triggerIndex = 0; triggerIndex < MAX_TRIGGERS && count < KLINE_VALUES_MAX; triggerIndex++)
  {
    const TriggerCfg &trigger = settings.triggers[triggerIndex];
    if (!trigger.enabled)
    {
      continue;
    }
    const UserSigCfg *signalConfigPointer = userSigConfig((signalId_t)trigger.signal);
    if (signalConfigPointer == NULL)
    {
      continue; // one of the gauge's own signals, not a page value
    }
    const UserSigCfg &signalConfig = *signalConfigPointer; // the value's settings, read with dots
    if (signalConfig.source != USRC_KLINE)
    {
      continue;
    }
    if (onlyAddr != 0 && signalConfig.klineAddr != onlyAddr)
    {
      continue; // another module: can't be read in this session
    }
    bool isListed = false;
    for (uint8_t i = 0; i < count; i++)
    {
      if (values[i].signal == trigger.signal)
      {
        isListed = true;
      }
    }
    if (isListed)
    {
      continue;
    }
    const uint8_t userSigIndex = trigger.signal - SIG_USER_1;
    values[count].signal = trigger.signal;
    values[count].pageIndex = 0xFE; // not read for a page on screen (0xFF = no page showing, so never matches either)
    values[count].addr = signalConfig.klineAddr;
    values[count].group = signalConfig.klineGroup;
    values[count].field = signalConfig.klineField;
    values[count].isMainValue = 0;
    if (userSigIndex < USER_SIGS && userSigIndex % 2 == 0)
    {
      values[count].isMainValue = 1; // a page's main value keeps its page's label
    }
    count++;
  }
}

// the values the K-line page session reads: every page in the rotation when they all share one module
// (rotationKlineModule), otherwise just the page on the glass, keeping only the values from its first K-line module.
// 0.35.0: plus the switched-on triggers' K-line values (all of them, or the ones from the same module)
static uint8_t klineSessionList(KlineValue *values)
{
  uint8_t count = 0;
  if (rotationKlineModule() != 0)
  {
    for (uint8_t pageIndex = 0; pageIndex < MAX_PAGES; pageIndex++)
    {
      if (settings.pages[pageIndex].enabled)
      {
        addPageKlineValues(pageIndex, values, count);
      }
    }
    addTriggerKlineValues(values, count, 0); // 0.35.0: and every switched-on trigger's K-line value
    return count;
  }
  addPageKlineValues(runtime.klinePageCfgIndex, values, count);
  uint8_t kept = 0; // one module per session: the first value's module
  for (uint8_t i = 0; i < count; i++)
  {
    if (values[i].addr == values[0].addr)
    {
      values[kept] = values[i];
      kept++;
    }
  }
  if (kept > 0)
  {
    addTriggerKlineValues(values, kept, values[0].addr); // 0.35.0: the triggers' values from the same module ride along
  }
  return kept;
}

// ---------------------------------------------------------------------------
// 0.17.4: one KWP1281 session shared by everything that reads measuring blocks: the K-line tab's engine blocks,
// K-line mode (menu > K-line, or "Show blocks here" in the web UI), gauge pages with a K-line source and the oil page.
// When the line passes to another of them that wants the same module, the session stays open and the next one
// carries on with it: no end block, no new 5-baud wake-up (~3 s). Before, "Show blocks here" closed the gauge page's
// session to the engine and woke the engine again, and back and forth.
// ---------------------------------------------------------------------------
static uint8_t openModule = 0;     // the module of the KWP1281 session left open for the next user, 0 = none
static char openModuleId[48] = ""; // its identification, handed on with it

// the module whoever should have the line now would read with KWP1281 (0 = none of the measuring-block users)
static uint8_t moduleWantedNow()
{
  const lineOwner_t owner = wantedOwner();
  if (owner == OWN_OIL)
  {
    return settings.oilClusterAddr;
  }
  if (owner == OWN_KLMODE)
  {
    return klineModeAddress();
  }
  if (owner == OWN_PAGE)
  {
    const uint8_t count = klineSessionList(currentValues); // currentValues: only scratch here, between sessions
    if (count)
    {
      return currentValues[0].addr;
    }
    return 0;
  }
  if (owner == OWN_BG && settings.klineProtocol == KL_KWP1281)
  {
    return settings.klineEcuAddr;
  }
  return 0; // the Service tab, KWP2000 / OBD, the ME7 logger, off
}

// a KWP1281 session to addr: the open one if it's to that module, otherwise a fresh 5-baud wake-up (closing an open
// one to another module first). id gets the module's identification. true = session ready.
static bool kwp1281Open(uint8_t addr, char *id, size_t idLen)
{
  if (kwp1281Connected() && openModule == addr)
  {
    copyStr(id, idLen, openModuleId);
    DEBUG("K-line: carrying on with the open session to 0x%02X", addr);
    return true;
  }
  kwp1281CloseOpen();             // another module's session (if any) ends properly
  vTaskDelay(pdMS_TO_TICKS(300)); // W5: K idle after whatever session had the line
  if (!kwp1281Connect(addr, id, idLen))
  {
    return false;
  }
  openModule = addr;
  copyStr(openModuleId, sizeof(openModuleId), id);
  return true;
}

// a user of the session to addr is done: it stays open when whoever has the line next wants the same module (and no
// restart was asked for), otherwise it ends with the end block
static void kwp1281Release(uint8_t addr)
{
  if (kwp1281Connected() && !restartFlag && moduleWantedNow() == addr)
  {
    return; // the next user carries on with it
  }
  kwp1281CloseOpen();
}

// ends the open session, if there is one (before KWP2000, the ME7 logger, a flash read, the Service tab, standby)
static void kwp1281CloseOpen()
{
  kwp1281Disconnect(); // sends the end block only if a session is up
  openModule = 0;
  openModuleId[0] = '\0';
}

// 0.19.0 "Try KWP2000" - the double slow-init (NefMoto lead, docs memory kline-double-slowinit-kwp2000): wake the
// engine at 0x01 as KWP1281, end that session properly, wait W5, then slow-init 0x01 again. Some ME7/EDC modules
// answer KWP2000 the second time, which measures faster than KWP1281. true = a KWP2000 session is up and keyBytes
// holds its key bytes; false = nothing connected. Call from the K-line task (it owns the line). This is the CONNECT
// half only - the KWP2000 measuring-block read path for generic K-line mode is the next part.
bool kwp2000TryDoubleInit(uint8_t keyBytes[2], char *id, size_t idLen)
{
  kwp1281CloseOpen();             // make sure no other session holds the line
  vTaskDelay(pdMS_TO_TICKS(300)); // W5 idle
  if (!kwp1281Connect(0x01, id, idLen)) // the first, KWP1281, session gives the module's identification
  {
    return false; // the engine didn't answer the first (KWP1281) slow init
  }
  DEBUG("K-line: Try KWP2000 - first slow init came up KWP1281 (%s), ending it for the second init", id);
  kwp1281Disconnect();             // end block
  vTaskDelay(pdMS_TO_TICKS(1500)); // W5 before waking it again
  const bool kwp2000 = kwp2000Connect(0x01, false, keyBytes); // slow init again -> KWP2000 if the module supports it
  DEBUG("K-line: Try KWP2000 - second slow init %s", kwp2000 ? "came up KWP2000" : "did NOT come up KWP2000");
  return kwp2000;
}

// 0.19.0: the measuring-block modes (dashboard groups, browse, gauge pages) open/read/close through these so they
// all use the fast VAG KWP2000 path when it's chosen and available. klineOnKwp2000 remembers which protocol came up.
static bool klineOnKwp2000 = false;

// Open a session to addr. With KWP2000-VAG chosen and the engine (0x01) as the target, try the double slow-init for
// the faster KWP2000 groups; on anything else (other module, KWP2000 didn't come up) use the normal KWP1281 session.
static bool klineModeOpen(uint8_t addr, char *id, size_t idLen)
{
  klineOnKwp2000 = false;
  if (settings.klineProtocol == KL_KWP2000VAG && addr == 0x01)
  {
    uint8_t keyBytes[2] = {0, 0};
    if (kwp2000TryDoubleInit(keyBytes, id, idLen))
    {
      klineOnKwp2000 = true; // id holds the identification the first (KWP1281) session read
      return true;
    }
    DEBUG("K-line: KWP2000-VAG not available on 0x01 - using KWP1281");
    // fall through to a normal KWP1281 session
  }
  return kwp1281Open(addr, id, idLen);
}

static bool klineModeConnected()
{
  if (klineOnKwp2000)
  {
    return kwp2000Connected();
  }
  return kwp1281Connected();
}

static bool klineModeReadGroup(uint8_t group, Kwp1281Group &out)
{
  if (klineOnKwp2000)
  {
    if (kwp2000ReadGroup(group, out))
    {
      return true;
    }
    if (kwp2000LastNrc() != 0)
    {
      return true; // refused (7F 21 xx): the ECU has no such block - the session is fine, the block is empty
    }
    kwp2000Disconnect(); // no answer: the session is gone, so the callers connect again
    return false;
  }
  return kwp1281ReadGroup(group, out);
}

static void klineModeRelease(uint8_t addr)
{
  if (klineOnKwp2000)
  {
    kwp2000Disconnect(); // the KWP2000 session isn't kept open/shared like KWP1281; it re-inits when next wanted
    klineOnKwp2000 = false;
    return;
  }
  kwp1281Release(addr);
}

// adds a block to the list unless it's already there
static void addSessionGroup(uint8_t group, uint8_t &groupCount)
{
  for (uint8_t i = 0; i < groupCount; i++)
  {
    if (sessionGroups[i] == group)
    {
      return;
    }
  }
  if (groupCount < KLINE_GROUPS_MAX)
  {
    sessionGroups[groupCount] = group;
    groupCount++;
  }
}

// is a value from this block on the page on the glass? (onScreenPage = 0xFF: no gauge page is showing)
static bool groupIsOnScreen(uint8_t group, uint8_t count, uint8_t onScreenPage)
{
  for (uint8_t i = 0; i < count; i++)
  {
    if (sessionValues[i].group == group && sessionValues[i].pageIndex == onScreenPage)
    {
      return true;
    }
  }
  return false;
}

// reads one block and hands its fields to the page values in it (and, when the K-line tab's KWP1281 shares this
// module, to the engine signals: rpm, coolant ...). false = the module stopped answering
static bool readSessionGroup(uint8_t group, uint8_t count, bool feedsEngineSignals)
{
  KlinePage &page = runtime.klinePage;
  Kwp1281Group block;
  if (!klineModeReadGroup(group, block))
  {
    page.errors++;
    return false;
  }
  page.reads++;
  countSample();
  if (block.count == 0)
  {
    klinePageState("Block %u is empty on 0x%02X", group, page.addr);
    return true;
  }
  for (uint8_t i = 0; i < count; i++) // every page value in this block
  {
    if (sessionValues[i].group == group && sessionValues[i].field < block.count)
    {
      signalSetLabel((signalId_t)sessionValues[i].signal, NULL, block.fields[sessionValues[i].field].unit);
      signalSet((signalId_t)sessionValues[i].signal, block.fields[sessionValues[i].field].value);
    }
  }
  if (feedsEngineSignals)
  {
    for (uint8_t i = 0; i < sizeof(groupMap) / sizeof(groupMap[0]); i++) // the engine block map at the top of this file
    {
      if (groupMap[i].group == group && groupMap[i].field < block.count)
      {
        signalSet((signalId_t)groupMap[i].signal, block.fields[groupMap[i].field].value);
      }
    }
  }
  klinePageState("Block %u", group);
  return true;
}

static void runKlinePage()
{
  KlinePage &page = runtime.klinePage;
  const uint8_t count = klineSessionList(sessionValues);
  if (!count)
  {
    idleFor(200, OWN_PAGE);
    return;
  }
  // 0.24.11: gave up (nothing answered since power-up) and no K-line page is on the glass: stay quiet. A K-line page
  // coming on screen tries again.
  if (klineStopped && !klinePageWanted())
  {
    setState("not found - stopped");
    idleFor(500, OWN_PAGE);
    return;
  }
  const uint8_t addr = sessionValues[0].addr;
  // held open for the whole rotation with the K-line tab's KWP1281 on the same module: its engine blocks join in
  const bool feedsEngineSignals = rotationKlineModule() != 0 &&
                                  (settings.klineProtocol == KL_KWP1281 || settings.klineProtocol == KL_KWP2000VAG);
  runtime.klineUp = false;
  page.addr = addr;
  page.connected = false;
  page.connecting = true;
  setState("K-line page: init");
  klinePageState("Waking module 0x%02X (5-baud init)", addr);
  char id[48];
  if (!klineModeOpen(addr, id, sizeof(id))) // 0.19.0: KWP2000-VAG if chosen + engine, else carries on / opens KWP1281
  {
    page.connecting = false;
    page.errors++;
    noEcuTries = noEcuTries + 1; // 0.24.11 (volatile: an explicit read + write)
    if (!klineEverConnected && noEcuTries >= KLINE_GIVE_UP_TRIES && !klinePageWanted())
    {
      // only held open for the rotation, with nothing on the K-line: stop (see KLINE_GIVE_UP_TRIES)
      klineStopped = true;
      setState("not found - stopped");
      klinePageState("No answer from 0x%02X - stopped", addr);
      DEBUG("K-line: no answer from 0x%02X after %u tries - stopped until a K-line page shows, the K-line settings change "
            "or the ignition goes off and on", addr, (unsigned)noEcuTries);
      return;
    }
    setState("K-line page: no answer");
    klinePageState("No answer from 0x%02X - retrying", addr);
    idleFor(3000, OWN_PAGE);
    return;
  }
  backgroundConnected(); // 0.24.11: an ECU answered - keep trying from now on
  page.connecting = false;
  page.connected = true;
  runtime.klineUp = true;
  copyStr(runtime.klineEcuId, sizeof(runtime.klineEcuId), id);
  setState("K-line page: connected");
  // names from the module's label file (the main value keeps the page label)
  const uint32_t labelStartMs = millis(); // 0.17.4 bench: how long the label work keeps the line quiet
  static char candidates[LBL_MAX_CANDS][LBL_NAME_LEN];
  const uint8_t candidateCount = labelsFind(addr, id, candidates, LBL_MAX_CANDS);
  const uint32_t labelFoundMs = millis();
  labelsRemember(addr, candidateCount ? candidates[0] : "", id);
  const uint32_t labelRememberedMs = millis();
  const bool hasLabels = candidateCount && labelsLoad(candidates[0]);
  DEBUG("K-line page: labels find %lu ms, remember %lu ms, load %lu ms", (unsigned long)(labelFoundMs - labelStartMs),
        (unsigned long)(labelRememberedMs - labelFoundMs), (unsigned long)(millis() - labelRememberedMs));
  if (hasLabels)
  {
    for (uint8_t valueIndex = 0; valueIndex < count; valueIndex++)
    {
      char name[40];
      if (!sessionValues[valueIndex].isMainValue &&
          labelsField(sessionValues[valueIndex].group, sessionValues[valueIndex].field + 1, name, sizeof(name), NULL, 0))
      {
        signalSetLabel((signalId_t)sessionValues[valueIndex].signal, name, NULL);
      }
    }
  }
  klinePageState("Reading block %u", sessionValues[0].group);
  uint8_t groupCount = 0; // each block once per round
  for (uint8_t valueIndex = 0; valueIndex < count; valueIndex++)
  {
    addSessionGroup(sessionValues[valueIndex].group, groupCount);
  }
  if (feedsEngineSignals)
  {
    for (uint8_t i = 0; i < sizeof(engineGroups); i++)
    {
      addSessionGroup(engineGroups[i], groupCount); // the K-line tab's engine blocks (rpm, coolant, boost ...)
    }
  }

  // each round: every block of the page on the glass, then one of the others in turn - the page you're looking at
  // stays quick, the rest keep a recent value ready for the swipe. With no gauge page showing (the menu, a mode
  // screen) that's one block per round, all of them in turn.
  uint8_t nextOtherGroup = 0; // where the turn-taking of the off-screen blocks has got to
  while (klineModeConnected() && wantedOwner() == OWN_PAGE && !restartFlag && klineSessionList(currentValues) == count &&
         !memcmp(currentValues, sessionValues, count * sizeof(KlineValue))) // a page or setting changed: new session
  {
    uint8_t onScreenPage = 0xFF; // which page is on the glass (0xFF = none)
    const uint32_t pageSeenMs = runtime.pageSeenMs;
    if (pageSeenMs != 0 && millis() - pageSeenMs < 1000)
    {
      onScreenPage = runtime.pageCfgIndex;
    }
    bool lost = false;
    // the page on the glass: all its blocks
    for (uint8_t groupIndex = 0; groupIndex < groupCount && !lost; groupIndex++)
    {
      if (groupIsOnScreen(sessionGroups[groupIndex], count, onScreenPage))
      {
        lost = !readSessionGroup(sessionGroups[groupIndex], count, feedsEngineSignals);
      }
    }
    // then the next off-screen block
    for (uint8_t tries = 0; tries < groupCount && !lost; tries++)
    {
      const uint8_t groupIndex = nextOtherGroup;
      nextOtherGroup++;
      if (nextOtherGroup >= groupCount)
      {
        nextOtherGroup = 0;
      }
      if (!groupIsOnScreen(sessionGroups[groupIndex], count, onScreenPage))
      {
        lost = !readSessionGroup(sessionGroups[groupIndex], count, feedsEngineSignals);
        break; // one per round
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // back-to-back reads keep the session alive (a lost one ends the loop above)
  }
  const bool lost = !klineModeConnected();
  // 0.17.4 bench: why the session ended
  if (lost)
  {
    DEBUG("K-line page: 0x%02X stopped answering (%lu reads so far)", addr, (unsigned long)page.reads);
  }
  else
  {
    DEBUG("K-line page: done with 0x%02X (page, settings or owner changed)", addr);
  }
  klineModeRelease(addr); // 0.19.0: ends the KWP2000 session, or releases/keeps the shared KWP1281 one
  page.connected = false;
  page.connecting = false;
  runtime.klineUp = false;
  if (lost && wantedOwner() == OWN_PAGE)
  {
    klinePageState("Connection lost - reconnecting");
    idleFor(1000, OWN_PAGE);
  }
  else
  {
    klinePageState("Closed");
  }
}

static void runKwp1281()
{
  setState("connecting");
  const uint8_t addr = settings.klineEcuAddr;
  // 0.17.4: carries on with an open session to this module, if there is one
  if (!kwp1281Open(addr, runtime.klineEcuId, sizeof(runtime.klineEcuId)))
  {
    backgroundNoEcu("no ECU");
    return;
  }
  backgroundConnected();
  runtime.klineUp = true;
  setState("KWP1281 up");
  DEBUG("KWP1281: %s", runtime.klineEcuId);
  uint8_t groupIndex = 0;
  while (kwp1281Connected() && !restartFlag && wantedOwner() == OWN_BG)
  {
    Kwp1281Group block;
    if (kwp1281ReadGroup(engineGroups[groupIndex], block))
    {
      for (const KwpGroupMap &entry : groupMap)
      {
        if (entry.group == block.group && entry.field < block.count)
        {
          signalSet((signalId_t)entry.signal, block.fields[entry.field].value);
        }
      }
      countSample();
    }
    groupIndex = (groupIndex + 1) % sizeof(engineGroups);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  kwp1281Release(addr); // 0.17.4: stays open if the next user wants this module too, otherwise the end block
  runtime.klineUp = false;
  setState("disconnected");
}

// 0.19.0: VAG KWP2000 measuring blocks. Tries the double slow-init at 0x01 first (reads the same engine groups as
// KWP1281 but faster); if the module won't come up KWP2000 it falls back to the normal KWP1281 session so the
// gauges still work. Either way the fields map to the same signals via groupMap, so the dashboard doesn't care
// which one connected. (BG engine-group path; the K-line browse / gauge-page sources still use KWP1281 for now.)
static void runKwp2000Vag()
{
  setState("connecting");
  const uint8_t addr = settings.klineEcuAddr;
  if (!klineModeOpen(addr, runtime.klineEcuId, sizeof(runtime.klineEcuId))) // double-init KWP2000 on 0x01, else KWP1281
  {
    backgroundNoEcu("no ECU");
    return;
  }
  backgroundConnected();
  runtime.klineUp = true;
  if (klineOnKwp2000)
  {
    setState("KWP2000 up");
  }
  else
  {
    setState("KWP1281 (2000 n/a)");
  }
  uint8_t groupIndex = 0;
  while (klineModeConnected() && !restartFlag && wantedOwner() == OWN_BG)
  {
    Kwp1281Group block;
    if (klineModeReadGroup(engineGroups[groupIndex], block))
    {
      for (const KwpGroupMap &entry : groupMap)
      {
        if (entry.group == block.group && entry.field < block.count)
        {
          signalSet((signalId_t)entry.signal, block.fields[entry.field].value);
        }
      }
      countSample();
    }
    groupIndex = (groupIndex + 1) % sizeof(engineGroups);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  klineModeRelease(addr);
  runtime.klineUp = false;
  setState("disconnected");
}

// KWP2000 / OBD-II timing (ISO 14230-4). P3 = the gap after the ECU's last byte before the next request, P4 = the gap
// between the bytes of a request. Standard is 55 ms / 5 ms. Fast OBD (settings.klineFastObd) shortens both; if the
// ECU then misses answers it drops back to standard for the rest of the session.
#define KWP_P3_STANDARD_MS 55 // ISO 14230-4 P3min
#define KWP_P4_STANDARD_MS 5  // ISO 14230-4 P4min
#define KWP_P3_FAST_MS 10     // Fast OBD [U: not tested on a car yet - most ECUs take the next request well inside 55 ms]
#define KWP_P4_FAST_MS 0      // Fast OBD: request bytes back to back (ISO 14230-2 allows 0)
#define KWP_FAST_MISSES 3     // missed answers in a row before Fast OBD drops back to standard timing

static ObdSchedule klineObdSchedule; // the ECU's PID lists and where the fast / slow rotation is

// asks the ECU which PIDs it has (0100, then 0120 / 0140 when the list says they exist)
static void klineReadPidLists()
{
  obdScheduleReset(klineObdSchedule);
  for (uint8_t listPid = 0x00; listPid <= 0x40; listPid += 0x20)
  {
    uint8_t data[8];
    uint8_t length = 0;
    if (!kwp2000ReadPid(listPid, data, sizeof(data), length) || length < 4)
    {
      break;
    }
    obdScheduleSetList(klineObdSchedule, listPid, data);
    if (!(data[3] & 0x01))
    {
      break; // bit 0 of the last byte = "the next list exists too"
    }
  }
  if (!obdScheduleHasList(klineObdSchedule))
  {
    obdScheduleAssumeAll(klineObdSchedule); // the ECU won't say: ask for every PID the gauge knows
  }
}

// the KWP2000 / OBD-II session's address: where it last worked (0 = not found yet), and the address setting that was
// for (a new setting starts the search again)
static uint8_t kwp2000FoundAddress = 0;
static uint8_t kwp2000FoundForSetting = 0;

// wakes the ECU at one address and reads its PID lists and VIN. Fast init first, then the 5-baud address - many
// 1999-2004 ECUs only wake for OBD that way (the bench ME7.5 answers 0x33 as ISO 9141-2). Whichever worked last time
// goes first, so a reconnect doesn't wait for the other. true = a session is up; hasVin = the ECU gave its VIN.
static bool kwp2000Open(uint8_t address, uint8_t keyBytes[2], uint32_t baud, bool &hasVin)
{
  static bool slowInitWorkedLast = false; // kept between sessions (static), false until a 5-baud wake-up has worked
  hasVin = false;
  bool connected = false;
  if (!slowInitWorkedLast)
  {
    setState("fast init");
    klSetBaud(baud);
    connected = kwp2000Connect(address, true, keyBytes);
  }
  if (!connected)
  {
    setState("5-baud init");
    klSetBaud(10400); // the 5-baud wake-up always answers at 10400
    connected = kwp2000Connect(address, false, keyBytes);
    slowInitWorkedLast = connected; // worked: first next time. Didn't: the fast init goes first again next time
  }
  if (!connected)
  {
    return false;
  }
  kwp2000SetTiming(KWP_P3_STANDARD_MS, KWP_P4_STANDARD_MS);
  klineReadPidLists();
  // the VIN (mode 09 02), once a session: any make's car on the K-line, for Your Car and Scan My Car. Many pre-2005
  // ECUs don't have mode 09; then this just finds nothing.
  char vin[18];
  if (kwp2000ReadObdVin(vin, sizeof(vin)) && vehicleVinValid(vin))
  {
    hasVin = true;
    vehicleTakeVin(vin, VIN_SRC_KLINE, true); // true: a K-line car, the older of the two model years a VIN can mean
    DEBUG("K-line: VIN %s (mode 09) at 0x%02X", vin, address);
  }
  return true;
}

static void runKwp2000()
{
  uint8_t keyBytes[2];
  // 0.17.4: only the rates the web UI offers. Anything else (the bench gauge had 10600 stored, typed in before the
  // 0.17.3 drop-down) uses ISO 14230's 10400 - 2 % off is enough for the ECU to miss the StartCommunication
  uint32_t baud = 10400;
  if (settings.klineBaud == 9600 || settings.klineBaud == 4800)
  {
    baud = settings.klineBaud;
  }
  uint8_t address = settings.klineEcuAddr;
  if (address == 0)
  {
    address = KWP2000_OBD_ADDRESS; // 0 = the OBD-II address 0x33
  }
  if (kwp2000FoundForSetting != address)
  {
    kwp2000FoundForSetting = address; // the setting changed: look again
    kwp2000FoundAddress = 0;
  }
  if (kwp2000FoundAddress != 0)
  {
    address = kwp2000FoundAddress; // found before: straight there
  }
  bool hasVin = false;
  bool connected = kwp2000Open(address, keyBytes, baud, hasVin);
  // no answer, or no VIN, at the address set (01 by default, the VW engine's KWP1281 address): the OBD-II address 0x33
  // next, where most cars answer OBD. Nothing to type in: whichever works is kept for the reconnects.
  if (!hasVin && kwp2000FoundAddress == 0 && address != KWP2000_OBD_ADDRESS)
  {
    DEBUG("K-line: no VIN at 0x%02X - trying the OBD-II address 0x33", address);
    if (connected)
    {
      kwp2000Disconnect();
    }
    bool obdHasVin = false;
    if (kwp2000Open(KWP2000_OBD_ADDRESS, keyBytes, baud, obdHasVin))
    {
      connected = true;
      address = KWP2000_OBD_ADDRESS;
    }
    else if (connected)
    {
      connected = kwp2000Open(address, keyBytes, baud, hasVin); // 0x33 silent: back to the address that answered
    }
  }
  if (!connected)
  {
    backgroundNoEcu("no ECU");
    return;
  }
  kwp2000FoundAddress = address;
  backgroundConnected();
  runtime.klineUp = true;
  if (kwp2000IsIso9141())
  {
    snprintf(runtime.klineEcuId, sizeof(runtime.klineEcuId), "ISO 9141-2 at %02X kb %02X %02X", address, keyBytes[0],
             keyBytes[1]);
  }
  else
  {
    snprintf(runtime.klineEcuId, sizeof(runtime.klineEcuId), "KWP2000 at %02X kb %02X %02X", address, keyBytes[0],
             keyBytes[1]);
  }
  setState("KWP2000 up");
  // the next request goes out as soon as the ECU's answer is in and P3 has passed (kwp2000Request waits out P3)
  bool fastTiming = false;      // Fast OBD timing in use
  bool fastGaveUp = false;      // the ECU missed answers with it: standard timing for the rest of this session
  uint8_t missesInRow = 0;
  uint32_t lastOk = millis();
  while (!restartFlag && wantedOwner() == OWN_BG)
  {
    // Fast OBD on / off follows the web UI straight away
    const bool fastWanted = settings.klineFastObd && !fastGaveUp;
    if (fastWanted != fastTiming)
    {
      fastTiming = fastWanted;
      if (fastTiming)
      {
        kwp2000SetTiming(KWP_P3_FAST_MS, KWP_P4_FAST_MS);
      }
      else
      {
        kwp2000SetTiming(KWP_P3_STANDARD_MS, KWP_P4_STANDARD_MS);
      }
    }
    uint8_t pid = 0;
    obdSchedulePick(klineObdSchedule, &pid, 1); // K-line: one PID per request
    uint8_t data[8];
    uint8_t length = 0;
    if (pid != 0 && kwp2000ReadPid(pid, data, sizeof(data), length))
    {
      lastOk = millis();
      missesInRow = 0;
      countSample(); // also works out the PID answers per second, shown as the K-line tab's "Samples /s"
      obdDecodePid(pid, data, length, signalSet); // the shared decoder (CANLine) - also feeds pages with an OBD source
    }
    else
    {
      missesInRow++;
      if (fastTiming && missesInRow >= KWP_FAST_MISSES)
      {
        fastGaveUp = true; // the ECU can't keep up with Fast OBD: back to standard timing
        setState("KWP2000 up (fast off)");
        DEBUG("K-line: Fast OBD missed %u answers in a row, back to standard timing", missesInRow);
        missesInRow = 0;
      }
      if (millis() - lastOk > 3000)
      {
        break; // lost the ECU
      }
    }
    if (pid == 0)
    {
      vTaskDelay(pdMS_TO_TICKS(100)); // nothing to ask for (can't happen with a PID list): don't spin
    }
  }
  kwp2000SetTiming(KWP_P3_STANDARD_MS, KWP_P4_STANDARD_MS); // StopCommunication at standard timing
  kwp2000Disconnect();
  kwp2000SetTiming(0, KWP_P4_STANDARD_MS); // back to the library's own default for the other K-line jobs
  runtime.klineUp = false;
  setState("disconnected");
}

// 0.39.6: menu > K-line (and the K-line tab's live view) when the engine doesn't answer at 0x01: OBD-II at 0x33, any
// make. The PIDs it has that the gauge knows are shown four to a page, the way a measuring block is (browse.group is
// the page number, a swipe steps it); each answer also goes on the gauge's own values, as the OBD session does.
// true = 0x33 answered (the session ran and has ended); false = no answer there either.
#define BROWSE_OBD_PER_PAGE 4 // PIDs a page (the four fields of a block)
static bool runKlineBrowseObd()
{
  KlineBrowse &browse = runtime.klineBrowse;
  klineBrowseState("Trying OBD-II at 0x33");
  uint8_t keyBytes[2];
  bool hasVin = false;
  if (!kwp2000Open(KWP2000_OBD_ADDRESS, keyBytes, 10400, hasVin)) // its PID lists are read into klineObdSchedule
  {
    return false;
  }
  // the PIDs it has that the gauge can show, in the table's order
  uint8_t pidList[OBD_PID_COUNT];
  uint8_t pidCount = 0;
  for (uint8_t tableIndex = 0; tableIndex < OBD_PID_COUNT; tableIndex++)
  {
    ObdPidInfo info;
    if (obdPidInfoAt(tableIndex, info) && obdScheduleSupports(klineObdSchedule, info.pid))
    {
      pidList[pidCount] = info.pid;
      pidCount++;
    }
  }
  browse.isObd = true;
  browse.addr = KWP2000_OBD_ADDRESS;
  browse.obdPageCount = (pidCount + BROWSE_OBD_PER_PAGE - 1) / BROWSE_OBD_PER_PAGE;
  if (browse.group < 1 || browse.group > browse.obdPageCount)
  {
    browse.group = 1;
  }
  browse.connecting = false;
  browse.connected = true;
  browse.searching = false;
  browse.text[0] = '\0';
  browse.labelFile[0] = '\0'; // no label files for OBD-II: the names come from the PID table
  browse.labelCandidateCount = 0;
  if (kwp2000IsIso9141())
  {
    copyStr(browse.ecuId, sizeof(browse.ecuId), "OBD-II at 33 (ISO 9141-2)");
  }
  else
  {
    copyStr(browse.ecuId, sizeof(browse.ecuId), "OBD-II at 33 (KWP2000)");
  }
  copyStr(runtime.klineEcuId, sizeof(runtime.klineEcuId), browse.ecuId);
  runtime.klineUp = true;
  setState("K-line mode: OBD-II at 0x33");
  klineBrowseState("Connected - OBD-II at 0x33");
  DEBUG("K-line mode: 0x01 silent, OBD-II at 0x33 answered (%u PIDs to show)", pidCount);

  uint32_t lastOkMs = millis();
  bool isLost = false;
  while (wantedOwner() == OWN_KLMODE && !restartFlag && klineModeAddress() == 0x01 && pidCount > 0)
  {
    // a page picked in the web UI, or a swipe
    if (browse.gotoGroup >= 0)
    {
      browse.group = (uint8_t)browse.gotoGroup;
      browse.gotoGroup = -1;
      if (browse.group < 1 || browse.group > browse.obdPageCount)
      {
        browse.group = 1;
      }
      browse.fieldCount = 0;
      browse.blockMs = 0;
    }
    if (browse.step)
    {
      int page = browse.group + browse.step; // wraps round at both ends
      browse.step = 0;
      if (page > browse.obdPageCount)
      {
        page = 1;
      }
      if (page < 1)
      {
        page = browse.obdPageCount;
      }
      browse.group = (uint8_t)page;
      browse.fieldCount = 0;
      browse.blockMs = 0;
    }
    browse.labelReload = false; // nothing to reload
    // the page's PIDs, one request each
    const uint8_t first = (browse.group - 1) * BROWSE_OBD_PER_PAGE;
    uint8_t count = 0;
    bool isAnswered = false;
    for (uint8_t slot = 0; slot < BROWSE_OBD_PER_PAGE && first + slot < pidCount; slot++)
    {
      const uint8_t pid = pidList[first + slot];
      ObdPidInfo info;
      obdPidInfo(pid, info);
      copyStr(browse.fieldName[slot], sizeof(browse.fieldName[slot]), info.name);
      copyStr(browse.unit[slot], sizeof(browse.unit[slot]), info.unit);
      browse.fieldSpec[slot][0] = '\0';
      browse.formula[slot] = 0; // no KWP1281 formula / a / b bytes for a PID
      browse.a[slot] = 0;
      browse.b[slot] = 0;
      uint8_t data[8];
      uint8_t length = 0;
      float value = NAN; // no answer: "--"
      if (kwp2000ReadPid(pid, data, sizeof(data), length))
      {
        isAnswered = true;
        lastOkMs = millis();
        countSample();
        obdDecodePid(pid, data, length, signalSet); // the gauge's own values too (pages with an OBD source, triggers)
        obdPidValueOf(pid, data, length, value);
      }
      browse.value[slot] = value;
      count++;
    }
    browse.fieldCount = count;
    snprintf(browse.title, sizeof(browse.title), "OBD-II page %u of %u", browse.group, browse.obdPageCount);
    browse.reads++;
    if (isAnswered)
    {
      browse.blockMs = millis();
      klineBrowseState("Connected - OBD-II page %u", browse.group);
    }
    if (millis() - lastOkMs > 3000)
    {
      browse.errors++;
      isLost = true; // nothing answered for 3 s: the session has gone
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // back-to-back reads keep the session alive
  }
  if (pidCount == 0)
  {
    klineBrowseState("OBD-II at 0x33: no PIDs the gauge knows");
    idleFor(3000, OWN_KLMODE);
  }
  kwp2000SetTiming(KWP_P3_STANDARD_MS, KWP_P4_STANDARD_MS); // StopCommunication at standard timing
  kwp2000Disconnect();
  kwp2000SetTiming(0, KWP_P4_STANDARD_MS); // back to the library's own default for the other K-line jobs
  browse.connected = false;
  browse.connecting = false;
  browse.isObd = false;
  runtime.klineUp = false;
  if (isLost && wantedOwner() == OWN_KLMODE)
  {
    setState("K-line mode: lost");
    klineBrowseState("Connection lost - reconnecting");
    idleFor(1000, OWN_KLMODE);
  }
  else
  {
    setState("K-line mode: closed");
    klineBrowseState("Closed");
  }
  return true;
}

// ---------------------------------------------------------------------------
// 0.17.6: gauge pages with ME7 logger variables (Gauges tab > Source K-line with the ME7 logger picked: "me7:<name>").
// The logger logs them along with the .cfg's variables, and every sample feeds them to their pages.
// ---------------------------------------------------------------------------
static int8_t me7PageVar[USER_SIGS_ALL]; // the logged variable each user signal shows, -1 = none

// the ME7 variable user signal userSigIndex shows on an enabled page (or, 0.35.0, a switched-on trigger watches), or NULL
static const char *me7PageVarName(uint8_t userSigIndex)
{
  const UserSigCfg *userSigPointer = userSigConfig((signalId_t)(SIG_USER_1 + userSigIndex));
  if (userSigPointer == NULL)
  {
    return NULL; // nothing set up here
  }
  const UserSigCfg &userSig = *userSigPointer; // the page value's settings, read with dots
  if (userSig.source != USRC_ME7 || !userSigWanted(userSigIndex))
  {
    return NULL; // another source, or not on a page that's switched on or a trigger
  }
  return userSig.sigName; // the .ecu name rides in sigName
}

// a number that changes whenever the pages' ME7 variables change (one added, removed or renamed), so the logger knows
// to load again with the new list
static uint32_t me7PageVarsKey()
{
  uint32_t key = 5381; // a simple running checksum of every page's variable name and where it is
  for (uint8_t i = 0; i < USER_SIGS_ALL; i++)
  {
    const char *name = me7PageVarName(i);
    if (name == NULL)
    {
      continue;
    }
    key = key * 33 + i;
    for (uint8_t c = 0; name[c]; c++)
    {
      key = key * 33 + (uint8_t)name[c];
    }
  }
  return key;
}

// before me7Load: the pages' variables go on the logger's list as well as the .cfg's
static void me7AddPageVars()
{
  me7ClearExtras();
  for (uint8_t i = 0; i < USER_SIGS_ALL; i++)
  {
    const char *name = me7PageVarName(i);
    if (name != NULL)
    {
      me7AddExtra(name);
    }
  }
}

// after me7Load: which logged variable each page value is, and its unit from the .ecu
static void me7MapPageVars()
{
  for (uint8_t i = 0; i < USER_SIGS_ALL; i++)
  {
    me7PageVar[i] = -1;
    const char *name = me7PageVarName(i);
    if (name == NULL)
    {
      continue;
    }
    me7PageVar[i] = me7FindVar(name); // -1 when the .ecu doesn't have it (the page's note says so)
    if (me7PageVar[i] >= 0)
    {
      signalSetLabel((signalId_t)(SIG_USER_1 + i), NULL, me7Var(me7PageVar[i]).unit); // keeps the page's label
    }
  }
}

// after every sample: each page value gets its variable's new value
static void me7FeedPages()
{
  for (uint8_t i = 0; i < USER_SIGS_ALL; i++)
  {
    if (me7PageVar[i] >= 0)
    {
      signalSet((signalId_t)(SIG_USER_1 + i), me7Var(me7PageVar[i]).value);
    }
  }
}

// ---------------------------------------------------------------------------
// 0.17.6: bench probe - one hand-made KWP2000 request inside the running logger session (me7ProbeQueue from the USB
// console). Read-only use: the console script only sends reads and block definitions (2C on identifiers the logger
// doesn't use), never memory writes.
// ---------------------------------------------------------------------------
static Me7Probe probe;
static uint8_t probeRequest[60];       // the request's bytes, service id first
static uint8_t probeRequestLength = 0; // how many

bool me7ProbeQueue(const uint8_t *request, uint8_t length, uint16_t repeats)
{
  if (me7State() != ME7_LOGGING || probe.pending || length == 0 || length > sizeof(probeRequest))
  {
    return false;
  }
  memcpy(probeRequest, request, length);
  probeRequestLength = length;
  probe.done = false;
  probe.repeats = repeats;
  if (probe.repeats == 0)
  {
    probe.repeats = 1;
  }
  probe.pending = true; // last: the K-line task picks it up after its next sample
  return true;
}

const Me7Probe &me7ProbeResult()
{
  return probe;
}

// runs the queued probe (K-line task, inside the logger session)
static void runProbe()
{
  uint32_t waitTotalUs = 0;
  uint32_t cycleTotalUs = 0;
  probe.answered = 0;
  probe.waitMinUs = 0xFFFFFFFF;
  probe.waitMaxUs = 0;
  for (uint16_t attempt = 0; attempt < probe.repeats; attempt++)
  {
    const uint32_t startUs = micros();
    uint8_t answerLength = 0;
    probe.ok = kwp2000Request(probeRequest, probeRequestLength, probe.answer, answerLength, 300, sizeof(probe.answer));
    cycleTotalUs += micros() - startUs;
    probe.answerLength = answerLength;
    probe.nrc = kwp2000LastNrc();
    uint32_t sendUs = 0;
    uint32_t waitUs = 0;
    uint32_t answerUs = 0;
    kwp2000LastTiming(sendUs, waitUs, answerUs);
    probe.sendUs = sendUs;
    probe.answerUs = answerUs;
    if (probe.ok || probe.nrc != 0)
    {
      probe.answered++; // it answered something: count its wait
      waitTotalUs += waitUs;
      if (waitUs < probe.waitMinUs)
      {
        probe.waitMinUs = waitUs;
      }
      if (waitUs > probe.waitMaxUs)
      {
        probe.waitMaxUs = waitUs;
      }
    }
  }
  probe.waitAvgUs = 0;
  if (probe.answered > 0)
  {
    probe.waitAvgUs = waitTotalUs / probe.answered;
  }
  else
  {
    probe.waitMinUs = 0;
  }
  probe.cycleAvgUs = cycleTotalUs / probe.repeats;
  probe.done = true;
  probe.pending = false;
}

// ---------------------------------------------------------------------------
// 0.19.0: ME7 fast-path bench diagnostic. See can2gauge_KLine.h Me7FastDiag for why.
// ---------------------------------------------------------------------------
EXT_RAM_BSS_ATTR static Me7FastDiag fastDiag;

bool me7FastDiagQueue(const uint32_t *addrs, uint8_t count)
{
  if (me7State() != ME7_LOGGING || fastDiag.pending)
  {
    return false; // need a running session to run inside
  }
  if (count > 8)
  {
    count = 8;
  }
  fastDiag.testAddrCount = count;
  for (uint8_t i = 0; i < count; i++)
  {
    fastDiag.testAddrs[i] = addrs[i];
  }
  fastDiag.done = false;
  fastDiag.pending = true; // the K-line task picks it up after its next sample
  return true;
}

const Me7FastDiag &me7FastDiagResult()
{
  return fastDiag;
}

// sends one raw request and records it as the next diagnostic step (raw reply bytes, even on a bad checksum)
static void diagStep(const char *label, const uint8_t *request, uint8_t requestLen)
{
  if (fastDiag.stepCount >= 16)
  {
    return;
  }
  Me7DiagStep &step = fastDiag.steps[fastDiag.stepCount];
  memset(&step, 0, sizeof(step));
  strncpy(step.label, label, sizeof(step.label) - 1);
  step.requestLen = requestLen;
  if (step.requestLen > sizeof(step.request))
  {
    step.requestLen = sizeof(step.request); // never copy more than the step can hold
  }
  memcpy(step.request, request, step.requestLen);
  bool checksumOk = false;
  step.answerLen = kwp2000RequestRaw(request, requestLen, step.answer, sizeof(step.answer), &checksumOk, 200);
  step.checksumOk = checksumOk;
  fastDiag.stepCount++;
}

// runs the whole diagnostic (K-line task, inside the logger session). Always un-hooks before it returns.
static void runFastDiag()
{
  fastDiag.installed = false;
  fastDiag.stepCount = 0;
  fastDiag.installMsg[0] = '\0';

  // v0 is already proven (the chain works, "C2G!" comes back). This diagnostic is now ONLY the v1 Fast routine,
  // with the logger's real variables, so we see the RAW B7 reply and learn why v1 returns nothing: crash
  // (nothing), buffer overflow (truncated), or wrong-target EXTP reads (clean but wrong values). Running v1 alone
  // (no prior v0 hook) keeps the install writes clean - a leftover hook makes every write echo our marker, not 7D.
  const uint8_t testerReq[1] = {0x3E};
  diagStep("3E pre", testerReq, sizeof(testerReq));

  uint32_t addrs[ME7FAST_MAX_VARS];
  uint8_t varCount;
  if (fastDiag.testAddrCount > 0)
  {
    varCount = fastDiag.testAddrCount; // a custom list from the console (isolate one var at a time)
    for (uint8_t i = 0; i < varCount; i++)
    {
      addrs[i] = fastDiag.testAddrs[i];
    }
  }
  else
  {
    varCount = me7VarCount(); // the logger's own variables
    if (varCount > ME7FAST_MAX_VARS)
    {
      varCount = ME7FAST_MAX_VARS;
    }
    for (uint8_t i = 0; i < varCount; i++)
    {
      addrs[i] = me7Var(i).address;
    }
  }
  fastDiag.installed = me7FastInstall(addrs, varCount);
  strncpy(fastDiag.installMsg, me7FastStatusGet().message, sizeof(fastDiag.installMsg) - 1);
  fastDiag.installMsg[sizeof(fastDiag.installMsg) - 1] = '\0';

  if (fastDiag.installed)
  {
    const uint8_t b7Req[1] = {0xB7};
    diagStep("B7 v1", b7Req, sizeof(b7Req));         // the real fast-log poll: expect varCount*2 value bytes
    diagStep("B7 v1 b", b7Req, sizeof(b7Req));       // again, in case the first answer is stale
    diagStep("3E v1", testerReq, sizeof(testerReq)); // still alive while v1 hooked?
    me7FastUnhook();
  }

  // prove the session recovered after un-hooking
  diagStep("3E post", testerReq, sizeof(testerReq));

  fastDiag.done = true;
  fastDiag.pending = false;
}

static void runMe7()
{
  setState("loading defs");
  char errorText[64];
  me7AddPageVars(); // 0.17.6: the gauge pages' variables are logged too
  const uint32_t pageVarsKey = me7PageVarsKey(); // and if they change, the logger loads again
  if (!me7Load(LittleFS, settings.me7EcuFile, settings.me7CfgFile, errorText, sizeof(errorText)))
  {
    copyStr(lastError, sizeof(lastError), errorText);
    setState("ME7: bad files");
    DEBUG("ME7: %s", errorText);
    idleFor(5000, OWN_BG);
    return;
  }
  me7MapPageVars(); // 0.17.6: each page value finds its variable
  const Me7EcuInfo &ecuInfo = me7EcuInfo();
  snprintf(runtime.klineEcuId, sizeof(runtime.klineEcuId), "%s %s", ecuInfo.partNumber, ecuInfo.engineId);
  // 0.19.0: the saved setting picks Fast (B7 RAM routine) or Normal logging, unless the web UI has armed a one-off
  // Test link (that stays armed until it's changed or the gauge restarts)
  if (me7GetFastMode() != ME7_MODE_TEST_LINK)
  {
    if (settings.me7FastMode)
    {
      me7SetFastMode(ME7_MODE_FAST); // v1 RAM routine
    }
    else
    {
      me7SetFastMode(ME7_MODE_NORMAL); // normal 0x21/0x23 reads
    }
  }
  setState("ME7 connecting");
  if (!me7Connect(errorText, sizeof(errorText)))
  {
    copyStr(lastError, sizeof(lastError), errorText);
    DEBUG("ME7: %s", errorText);
    backgroundNoEcu("ME7: no ECU");
    return;
  }
  backgroundConnected();
  runtime.klineUp = true;
  setState("ME7 logging");
  if (settings.me7LogToFile)
  {
    logRequest = true;
  }
  const uint32_t period = 1000 / me7SamplesWanted(); // 0.17.6: the web UI's Samples/s (was only the .cfg's)
  uint32_t nextMs = millis();
  // 0.17.6: also stops when the pages' ME7 variables change: the task comes straight back here and loads the new list
  while (me7State() == ME7_LOGGING && !restartFlag && wantedOwner() == OWN_BG && me7PageVarsKey() == pageVarsKey)
  {
    if (logRequest)
    {
      logRequest = false;
      if (me7LogStart(LittleFS, "/logs"))
      {
        copyStr(runtime.me7LogFile, sizeof(runtime.me7LogFile), me7LogFileName());
      }
      runtime.me7Logging = me7LogFileName()[0] != '\0';
    }
    if (logStopRequest)
    {
      logStopRequest = false;
      me7LogStop();
      runtime.me7Logging = false;
    }
    me7Sample();
    me7FeedPages(); // 0.17.6: the gauge pages' ME7 variables
    if (probe.pending)
    {
      runProbe(); // 0.17.6: a bench probe from the USB console, between two samples
    }
    if (fastDiag.pending)
    {
      runFastDiag(); // 0.19.0: the one-shot fast-path diagnostic (install, probe hooked, un-hook)
    }
    runtime.klineSamples = me7SampleCount();
    runtime.me7SampleRate = me7SampleRate();
    nextMs += period;
    int32_t waitMs = (int32_t)(nextMs - millis());
    if (waitMs > 0)
    {
      vTaskDelay(pdMS_TO_TICKS(waitMs));
    }
    else
    {
      nextMs = millis();
    }
  }
  me7Disconnect();
  runtime.me7Logging = false;
  runtime.klineUp = false;
  setState("ME7 stopped");
}

// 0.24.1 bench: the K-line listener (include/can2gauge_KLine.h). The K-line task records every byte it hears, with
// the time since the start in microseconds, and sends nothing.
static volatile uint32_t listenWantedMs = 0; // asked to listen this long, 0 = not asked
static volatile bool listenFinished = false;
static uint32_t *listenTimes = NULL;          // PSRAM, KL_LISTEN_MAX of each
static uint8_t *listenValues = NULL;
static volatile uint16_t listenTotal = 0;

bool klineListenStart(uint32_t ms)
{
  if (settings.klineProtocol != 0 || listenWantedMs != 0)
  {
    return false; // the gauge's own protocol has the line, or a listen is already running
  }
  if (listenTimes == NULL)
  {
    listenTimes = (uint32_t *)ps_malloc(KL_LISTEN_MAX * sizeof(uint32_t));
    listenValues = (uint8_t *)ps_malloc(KL_LISTEN_MAX);
  }
  if (listenTimes == NULL || listenValues == NULL)
  {
    return false;
  }
  listenTotal = 0;
  listenFinished = false;
  listenWantedMs = ms;
  return true;
}

bool klineListenDone()
{
  return listenFinished;
}

uint16_t klineListenCount()
{
  return listenTotal;
}

void klineListenByte(uint16_t index, uint32_t &microsFromStart, uint8_t &value)
{
  microsFromStart = listenTimes[index];
  value = listenValues[index];
}

// runs in the K-line task: hears the line for listenWantedMs, stamping each byte as the UART hands it over
static void runListen()
{
  setState("listening");
  klFlush();
  const uint32_t startUs = micros();
  const uint32_t lengthUs = listenWantedMs * 1000UL;
  uint16_t count = 0;
  while (micros() - startUs < lengthUs && count < KL_LISTEN_MAX)
  {
    const int value = klRead(20);
    if (value >= 0)
    {
      listenTimes[count] = micros() - startUs;
      listenValues[count] = (uint8_t)value;
      count++;
    }
  }
  listenTotal = count;
  listenWantedMs = 0;
  listenFinished = true;
  setState("off");
}

static void klineTaskFn(void *)
{
  labelsPrepare(); // 0.17.4: the label index into PSRAM now, not while a module waits for its next block
  for (;;)
  {
    if (listenWantedMs != 0) // 0.24.1 bench: listen only (the line is free: the protocol is off)
    {
      runListen();
      continue;
    }
    restartFlag = false;
    resetSampleRate(); // 0.17.3: every session (or none) starts from 0/s
    if (me7ReadPending()) // a flash read pre-empts the normal protocol and owns the K-line for its duration
    {
      setState("ECU read");
      runtime.klineUp = false;
      kwp1281CloseOpen(); // 0.17.4: a session left open for the next block reader ends first
      me7ReadService();
      setState("off");
      continue;
    }
    if (serviceKlinePending()) // 0.16.0: the Service tab's fault read / clear or basic setting owns the line until done
    {
      setState("service");
      runtime.klineUp = false;
      kwp1281CloseOpen(); // 0.17.4: the Service tab makes its own session
      serviceKlineRun();
      setState("off");
      continue;
    }
    if (carScanKlinePending()) // 0.37.0: the web UI's Scan My Car: K-line modules or one module's blocks
    {
      setState("car scan");
      runtime.klineUp = false;
      kwp1281CloseOpen(); // a session left open for the next block reader ends first
      carScanKlineRun();
      setState("off");
      continue;
    }
    if (runtime.otherScanners) // 0.37.0: Enable Other Scanners: the line stays quiet until it's switched off
    {
      setState("other scanners");
      runtime.klineUp = false;
      kwp1281CloseOpen(); // a session left open ends
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (runtime.standby) // ignition off on permanent power: the line stays quiet until it is back on
    {
      klineStopped = false; // 0.24.11: ignition back on later = a fresh try
      noEcuTries = 0;
      setState("standby");
      runtime.klineUp = false;
      kwp1281CloseOpen(); // 0.17.4: the line goes quiet
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    const lineOwner_t owner = wantedOwner();
    if (owner == OWN_OIL) // the oil page is showing: the cluster gets the line, the normal protocol resumes after
    {
      runOilCluster();
      continue;
    }
    if (owner == OWN_KLMODE) // menu > K-line: the chosen module's measuring blocks
    {
      runKlineBrowse();
      continue;
    }
    if (owner == OWN_PAGE) // a gauge page reading measuring blocks
    {
      runKlinePage();
      continue;
    }
    if (settings.klineProtocol != KL_KWP1281)
    {
      kwp1281CloseOpen(); // 0.17.4: KWP2000 / the ME7 logger / off - a KWP1281 session left open ends first
    }
    if (klineStopped && settings.klineProtocol != KL_OFF) // 0.24.11: gave up - wait for a restart (settings, reconnect)
    {
      runtime.klineUp = false;
      idleFor(500, OWN_BG);
      continue;
    }
    switch (settings.klineProtocol)
    {
    case KL_KWP1281: runKwp1281(); break;
    case KL_KWP2000: runKwp2000(); break;
    case KL_ME7LOGGER: runMe7(); break;
    case KL_KWP2000VAG: runKwp2000Vag(); break; // 0.19.0: VAG KWP2000 groups (double slow-init, 1281 fallback)
    default:
      setState("off");
      runtime.klineUp = false;
      idleFor(500, OWN_BG);
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void setupKLine()
{
  klBegin(KLINE_PIN_TX, KLINE_PIN_RX, 10400);
  runtime.klineBrowse.group = 1;
  runtime.klineBrowse.gotoGroup = -1;
  setState("off");
  // 0.17.4: core 1, priority 5 (was core 0, priority 4). On core 0 the web server (priority 10) and WiFi run, and a
  // slow web request held the K-line task off long enough to miss the module's byte timing (bench: "block ended with
  // E7 instead of 03", lost sessions with the web UI open). Core 1 has the display (priority 4) and the UI ticks
  // (3); this task spends nearly all its time waiting for the next K-line byte, so it doesn't hold them up.
  xTaskCreatePinnedToCore(klineTaskFn, // the function the task runs
                          "kline",     // its name (shows in crash reports)
                          8192,        // stack bytes
                          NULL,        // no parameter
                          5,           // priority: above the display, so a byte is answered at once
                          &klineTask,  // handle
                          1);          // core 1, away from WiFi and the web server
}

void klineRestart()
{
  restartFlag = true;
  klineStopped = false; // 0.24.11: settings changed / reconnect asked: try again
  noEcuTries = 0;
}

// the web UI's Start CSV Log. 0.17.6: says what happens - before, it answered "logging" even while the logger hadn't
// got the line (the request waited and started the CSV once it did, with no word of it in the web UI)
const char *me7StartLogging()
{
  if (settings.klineProtocol != KL_ME7LOGGER)
  {
    return "ME7 logger not selected";
  }
  logRequest = true; // picked up by the logging loop in runMe7()
  if (me7State() == ME7_LOGGING)
  {
    return "logging";
  }
  return "the CSV starts when the ME7 logger connects";
}

// 0.17.6: the samples a second the ME7 logger aims for: the web UI's Samples/s, 1 to ME7_MAX_SPS. Before, only the
// .cfg's SamplesPerSecond line counted (20 in log_example.cfg), so 50 in the web UI did nothing. 0 = the .cfg's.
uint8_t me7SamplesWanted()
{
  uint8_t wanted = settings.me7SamplesPerSecond;
  if (wanted == 0)
  {
    wanted = me7SamplesPerSecond(); // the .cfg's SamplesPerSecond (10 when it has none)
  }
  if (wanted == 0)
  {
    wanted = 10;
  }
  if (wanted > ME7_MAX_SPS)
  {
    wanted = ME7_MAX_SPS;
  }
  return wanted;
}

void me7StopLogging()
{
  logStopRequest = true;
}

void klineInfoProvider(char *buf, size_t len)
{
  static const char *names[] = {"off", "KWP1281", "KWP2000 / OBD", "ME7 logger", "KWP2000 VAG"}; // klineProtocol_t order
  if (runtime.oil.connecting || runtime.oil.connected)
  { // the oil page has the line (oil source = K-line)
    snprintf(buf, len, "oil: cluster 0x%02X\n%s\n%s\n\nreads %lu  errors %lu\n\n%s resumes when\nthe oil page is left",
             settings.oilClusterAddr, runtime.oil.state, runtime.oil.clusterId, (unsigned long)runtime.oil.reads,
             (unsigned long)runtime.oil.errors, names[settings.klineProtocol < klineProtocol_t_MAX ? settings.klineProtocol : 0]);
    return;
  }
  size_t written = 0;
  appendf(buf, len, written, "%s\n%s\n%s\n\nsamples %lu", names[settings.klineProtocol < klineProtocol_t_MAX ? settings.klineProtocol : 0],
          runtime.klineState, runtime.klineEcuId, (unsigned long)runtime.klineSamples);
  if (settings.klineProtocol == KL_KWP2000 || settings.klineProtocol == KL_KWP1281 ||
      settings.klineProtocol == KL_KWP2000VAG)
  {
    appendf(buf, len, written, "  %lu/s", (unsigned long)runtime.me7SampleRate); // answers per second (0.17.3: KWP1281 too)
  }
  if (settings.klineProtocol == KL_ME7LOGGER)
  {
    appendf(buf, len, written, "  %lu/s\n", (unsigned long)runtime.me7SampleRate);
    for (uint8_t varIndex = 0; varIndex < me7VarCount() && varIndex < 6 && written < len; varIndex++)
    {
      const Me7Var &var = me7Var(varIndex);
      appendf(buf, len, written, "%s %.2f %s\n", var.name, var.value, var.unit);
    }
    if (lastError[0])
    {
      appendf(buf, len, written, "\n%s", lastError);
    }
  }
}
