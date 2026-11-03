#include <can2gauge_API.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_EEP.h>
#include <can2gauge_IO.h>
#include <can2gauge_KLine.h>
#include <can2gauge_Triggers.h>
#include <can2gauge_WiFi.h>
#include <can2gauge_OTA.h>
#include <wifi_manager.h> // shared: Home WiFi bridge routes + static UI / recovery page
#include <can2gauge_ME7Read.h> // ME7 flash read (optional; delete this include + the ME7Read block below to drop it)
#include <can2gauge_DSG.h>
#include <can2gauge_VagDiag.h> // the VAG diag tab (/api/vdiag)
#include <can2gauge_ELM.h>
#include <can2gauge_Link.h>
#include <can2gauge_Shared.h> // 0.30.4: shared settings - in step with the other gauges?
#include <can2gauge_ChainUpdate.h> // 0.25.0: Update All Gauges (Chain tab)
#include <can2gauge_OHX.h>
#include <can2gauge_CanRelay.h>
#include <can2gauge_ECU.h> // an Ignitron / MaxxECU / Link / Haltech / EMU PRO over WiFi from usb2ip
#include <can2gauge_FirstBoot.h> // first boot setup: /api/firstboot, and a factory reset asks again
#include <can2gauge_CanMon.h>
#include <can2gauge_Labels.h>
#include <can2gauge_UserSig.h>
#include <can2gauge_Display.h>
#include <can2gauge_ADC2CAN.h>
#include <can2gauge_Power.h>
#include <can2gauge_themes.h>
#include <can2gauge_Logger.h>  // 0.16.0: the Log tab (/api/log)
#include <can2gauge_Service.h> // 0.16.0: the Service tab (/api/service)
#include <can2gauge_Vehicle.h> // 0.35.0: Your Car (/api/vehicle)
#include <can2gauge_CarScan.h> // 0.37.0: Scan My Car (/api/carscan)
#include <can2gauge_Drag.h>    // 0.38.0: the drag timer's scoreboard (/api/drag)
#include <GaugeUI.h>
#include <LVGL_Rotate.h> // 0.25.1: Upside Down / Rotation Trim (SCREEN_ROTATION 0 leaves it out)
#include <FsUnzip.h> // 0.24.0: files that ship zipped (logos, label index)
#include <CANLine.h>
#include <IOX.h>
#include <LVGL_Driver.h>
#include <ME7Logger.h>
#include <ArduinoJson.h> // JSON responses
#include <can2gauge_RAM.h> // 0.25.4: every JsonDocument below is built in PSRAM (psramJsonAllocator), plus the RAM note
#include <can2gauge_Units.h> // 0.27.0: a unit per quantity - a change turns the pages' ranges over
#include <can2gauge_Nav.h>   // 0.30.0: nav link status, Show Test Turn
#include <can2gauge_BLE.h>   // 0.30.0: bleNeedsRestart / bleStartError for the nav link
#include <LittleFS.h>    // file list / upload / download / static UI
#include <esp_littlefs.h> // esp_littlefs_info(): the filesystem's size and use in one walk (0.17.4)
#include <WiFi.h>        // AP client count in /api/status
#include <esp_system.h>  // esp_reset_reason (Diag)
#include <esp_heap_caps.h> // 0.24.1: the least internal RAM free since boot (/api/status heapMin)
#include <lv_blend_esp32s3.h> // the SIMD blend switch (Diag)

AsyncWebServer webServer(80);

// ---------------------------------------------------------------------------
// 0.17.4: filesystem size / used, kept here and worked out on the house task (see can2gauge_API.h)
// ---------------------------------------------------------------------------
static volatile bool fsUsageStale = true; // true = work the numbers out again (starts true: not done yet)
static size_t fsTotalBytes = 0;           // the filesystem's size
static size_t fsUsedBytes = 0;            // bytes in use

void fsUsage(size_t &total, size_t &used)
{
  total = fsTotalBytes;
  used = fsUsedBytes;
}

void fsUsageChanged()
{
  fsUsageStale = true;
}

void fsUsageTick()
{
  if (!fsUsageStale)
  {
    return;
  }
  fsUsageStale = false; // cleared first: a change during the walk asks for another one
  const uint32_t startMs = millis();
  size_t total = 0;
  size_t used = 0;
  if (esp_littlefs_info("spiffs", &total, &used) == ESP_OK) // one walk for both ("spiffs" = the partition's name)
  {
    fsTotalBytes = total;
    fsUsedBytes = used;
  }
  DEBUG("Files: %u KB used of %u KB (worked out in %lu ms)", (unsigned)(used / 1024), (unsigned)(total / 1024),
        (unsigned long)(millis() - startMs));
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static void sendJSON(AsyncWebServerRequest *request, int code, const JsonDocument &doc)
{
  // 0.17.3: no request = the USB bench console asked (apiConsoleRequest below): the reply goes out on the USB serial as
  // one line "@@C2G <code> <json>". Built in a String first so it goes out in one write, and the debug lines other
  // tasks print can't land in the middle of it.
  if (request == NULL)
  {
    // 0.25.4: both Strings made full size up front - one piece each, from PSRAM (512 bytes and up go there). Grown a
    // bit at a time, the first steps were small pieces of internal RAM (Tiguan drive: a watchdog asking for /api/elm
    // every 10 s over USB, and the gauge froze twice with internal RAM gone)
    const size_t replyLength = measureJson(doc);
    String json;
    json.reserve(replyLength + 1);
    serializeJson(doc, json); // 0.17.4: into its own String - ArduinoJson 7 empties the String it writes into, so
                              // writing straight into line wiped the "@@C2G 200 " in front (the script got no replies)
    String line;
    line.reserve(replyLength + 24); // "@@C2G 200 " + the reply + the newline
    line = CONSOLE_MARK " ";
    line += code;
    line += ' ';
    line += json;
    line += '\n';
    Serial.write((const uint8_t *)line.c_str(), line.length());
    return;
  }
  // the reply's buffer made the right size up front. It starts at 1.4 KB otherwise, and ArduinoJson writes one character
  // at a time: past 1.4 KB every character made the library build a bigger buffer and copy the whole reply into it, so
  // /api/themes (~20 KB) took over 5 s and the watchdog reset the gauge (2026-10-14). +64: a little spare room.
  const size_t replyLength = measureJson(doc);
  AsyncResponseStream *response = request->beginResponseStream("application/json", replyLength + 64);
  serializeJson(doc, *response);
  response->setCode(code);
  request->send(response);
}

static void sendOk(AsyncWebServerRequest *request, const char *message = "ok")
{
  JsonDocument doc(psramJsonAllocator());
  doc["status"] = message;
  sendJSON(request, 200, doc);
}

// collect a JSON body across chunks then call done(request, body)
static void parseJSON(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total,
                      void (*done)(AsyncWebServerRequest *, const String &))
{
  // gathered in a plain malloc'd buffer: if the phone drops part-way, the web server frees _tempObject itself with
  // free(), which only gives everything back for memory from malloc
  char *body = (char *)request->_tempObject;
  if (index == 0)
  {
    body = (char *)malloc(total + 1);
    request->_tempObject = body;
    if (body == NULL)
    {
      request->send(507); // no memory for it
      return;
    }
  }
  if (body == NULL || index + len > total)
  {
    return; // no buffer, or more than the request said it would send
  }
  memcpy(body + index, data, len);
  if (index + len == total)
  {
    body[total] = '\0';
    done(request, String(body));
    free(body);
    request->_tempObject = NULL;
  }
}

// ---------------------------------------------------------------------------
// GET /api/status - live data for the dashboard (polled every 500 ms)
// ---------------------------------------------------------------------------
static void statusOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  doc["version"] = CAN2GAUGE_VERSION;
  doc["uptime"] = millis() / 1000;
  doc["otherScanners"] = runtime.otherScanners; // 0.37.0: Enable Other Scanners is on
  doc["heap"] = ESP.getFreeHeap();
  doc["heapMin"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL); // 0.24.1: the least internal RAM free since boot
  doc["heapBlock"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); // and its biggest single piece now
  doc["psram"] = ESP.getFreePsram();
  doc["fps"] = lvglFps(); // frames drawn in the last second (LVGL redraws only what changed, so a still page reads ~0)
  {
    // what drawing costs (0.11.0): the last second, and the last benchmark (Diag > Run benchmark)
    const LvglPerf &lvglPerfStats = lvglPerf();
    const UiPerf &uiPerfStats = uiPerf();
    JsonObject perfObj = doc["perf"].to<JsonObject>();
    perfObj["mode"] = lvglDirect() ? "direct" : "partial"; // running now (settings.renderMode applies at boot)
    perfObj["simd"] = c2gSimdEnabled() != 0;
#if SCREEN_ROTATION
    perfObj["rotation"] = lvglRotationState(); // 0.25.1: ROTATION_OFF / ON / NO_ROOM / PARTIAL (no key = feature left out)
#endif
    perfObj["fps"] = lvglPerfStats.fps;
    perfObj["frameAvgUs"] = lvglPerfStats.frameAvgUs;
    perfObj["frameMaxUs"] = lvglPerfStats.frameMaxUs;
    perfObj["flushUs"] = lvglPerfStats.flushUs;
    perfObj["pixels"] = lvglPerfStats.pixels;
    perfObj["busyPct"] = lvglPerfStats.busyPct;
    perfObj["tickAvgUs"] = uiPerfStats.tickAvgUs;
    perfObj["tickMaxUs"] = uiPerfStats.tickMaxUs;
    perfObj["decodes"] = uiPerfStats.decodes;
    perfObj["decodeAvgUs"] = uiPerfStats.decodeAvgUs;
    perfObj["unzipAvgUs"] = uiPerfStats.unzipAvgUs; // 0.24.7: the unzip part of decodeAvgUs
    perfObj["decodeMaxUs"] = uiPerfStats.decodeMaxUs;
    perfObj["slideFrames"] = lvglLastTransitionFrames(); // 0.24.23: the last page slide: frames shown ...
    perfObj["slideMs"] = lvglLastTransitionMs();         // ... in this long
    JsonObject benchObj = perfObj["bench"].to<JsonObject>();
    benchObj["running"] = uiPerfStats.benchRunning;
    if (uiPerfStats.benchMs)
    {
      benchObj["ageS"] = (millis() - uiPerfStats.benchMs) / 1000;
    }
    else
    {
      benchObj["ageS"] = -1; // never run
    }
    benchObj["screen"] = uiPerfStats.benchScreen;
    benchObj["fps"] = uiPerfStats.benchFps;
    benchObj["frameAvgUs"] = uiPerfStats.benchFrameAvgUs;
    benchObj["frameMaxUs"] = uiPerfStats.benchFrameMaxUs;
    benchObj["flushUs"] = uiPerfStats.benchFlushUs;
    benchObj["busyPct"] = uiPerfStats.benchBusyPct;
    benchObj["decodeAvgUs"] = uiPerfStats.benchDecodeAvgUs;
  }
  {
    // display health (Diag > Display). The glass can't answer back, so this is what the ESP32 side can see.
    static const char *stepNames[] = {"not started", "SPI bus up", "registers sent", "RGB frame buffers made", "running"};
    const DisplayHealth health = displayHealth();
    JsonObject displayObj = doc["display"].to<JsonObject>();
    displayObj["ready"] = lvglReady(); // false = running without the screen (display init failed)
    displayObj["step"] = health.step;
    if (health.step <= DISPLAY_STEP_READY)
    {
      displayObj["stepName"] = stepNames[health.step];
    }
    if (health.panel == DISPLAY_PANEL_WAVESHARE)
    {
      displayObj["panel"] = "Waveshare 2.1 (dev board)";
    }
    else
    {
      displayObj["panel"] = "YouriTech ET021CR01-Z";
    }
    displayObj["spiErrors"] = health.spiErrors;
    // frames sent to the glass per second, worked out between two status polls
    static uint32_t lastFrames = 0;
    static uint32_t lastFramesMs = 0;
    static uint32_t framesPerSec = 0;
    const uint32_t nowMs = millis();
    if (nowMs - lastFramesMs >= 400)
    {
      if (lastFramesMs != 0)
      {
        framesPerSec = (health.frames - lastFrames) * 1000 / (nowMs - lastFramesMs);
      }
      lastFrames = health.frames;
      lastFramesMs = nowMs;
    }
    displayObj["scanHz"] = framesPerSec; // about 57 when the RGB stream runs
    displayObj["asleep"] = health.asleep;
    displayObj["backlight"] = displayGetBacklight(); // % asked for
    displayObj["expander"] = ioxPresent();           // TCA9554: drives the panel's reset / CS and the touch reset
    if (lvglReady() && lvglLock(20))
    {
      lv_mem_monitor_t memory;
      lv_mem_monitor(&memory); // LVGL's own memory pool (objects, styles)
      lvglUnlock();
      displayObj["lvglMemUsedPct"] = memory.used_pct;
      displayObj["lvglMemFree"] = memory.free_size;
      displayObj["lvglMemFragPct"] = memory.frag_pct;
    }
    const TouchStats touch = touchStats();
    JsonObject touchObj = displayObj["touch"].to<JsonObject>();
    touchObj["found"] = touchPresent();
    touchObj["chipId"] = touchChipId();
    touchObj["reads"] = touch.reads;
    touchObj["readErrors"] = touch.readErrors;
    touchObj["presses"] = touch.presses;
    touchObj["pressedNow"] = touch.pressedNow;
    touchObj["x"] = touch.lastX;
    touchObj["y"] = touch.lastY;
    touchObj["gesture"] = touch.lastGesture;
    if (touch.lastPressMs != 0)
    {
      touchObj["ageMs"] = nowMs - touch.lastPressMs; // since the last press
    }
    else
    {
      touchObj["ageMs"] = -1; // no press yet
    }
  }
  {
    static const char *resets[] = {"unknown", "power-on", "external", "software", "crash", "interrupt watchdog",
                                   "task watchdog", "watchdog", "deep sleep", "brownout", "SDIO"};
    const int resetReason = (int)esp_reset_reason();
    doc["reset"] = resetReason < (int)(sizeof(resets) / sizeof(resets[0])) ? resets[resetReason] : "other";
  }
  {
    // 0.25.4: memory requests that couldn't be met - this boot's, and the one before a reset (what ran out, in which task)
    const RamFailNote &failNow = ramFailNow();
    const RamFailNote &failLast = ramFailLastBoot();
    JsonObject ramObj = doc["ramFail"].to<JsonObject>();
    ramObj["count"] = failNow.count;
    if (failNow.count > 0)
    {
      ramObj["size"] = failNow.size;
      ramObj["caps"] = failNow.caps;
      ramObj["task"] = (const char *)failNow.task; // copied by the JSON (it can change while this is sent)
      ramObj["atS"] = failNow.uptimeS;
    }
    if (failLast.count > 0)
    {
      JsonObject lastObj = ramObj["lastBoot"].to<JsonObject>();
      lastObj["count"] = failLast.count;
      lastObj["size"] = failLast.size;
      lastObj["caps"] = failLast.caps;
      lastObj["task"] = (const char *)failLast.task;
      lastObj["atS"] = failLast.uptimeS;
    }
  }

  doc["page"] = runtime.activePage;
  doc["alert"] = runtime.alertActive ? runtime.alertText : "";

  // what the glass shows, for Live Data: a gauge page (its settings.pages index) or a mode screen with its rows
  {
    static const char *kinds[] = {"boot", "page", "menu", "mode", "haldex", "dsg", "settings", "other", "saver", "dark", "game",
                                  "drag"}; // 0.38.0: + the drag timer
    uint8_t item = 0;
    const uint8_t kind = uiScreenKind(&item);
    JsonObject screenObj = doc["screen"].to<JsonObject>();
    screenObj["kind"] = kinds[kind < 12 ? kind : 7];
    screenObj["item"] = item;
    EXT_RAM_BSS_ATTR static UiModeView view;
    uint8_t viewItem = 0;
    if (kind == UI_SCR_MODE && displayLiveView(view, viewItem))
    {
      static const char *items[MENU_ITEM_COUNT] = {"haldex", "can", "kline", "output", "dsg", "ble", "chain", "gauges",
                                                   "adc", "drag", "nav", "ecu", "oil", "triggers"}; // uiMenuItem_t order
      screenObj["mode"] = viewItem < MENU_ITEM_COUNT ? items[viewItem] : "";
      screenObj["title"] = view.title;
      screenObj["status"] = view.status;
      screenObj["header"] = view.header;
      screenObj["sub"] = view.sub;
      JsonArray rows = screenObj["rows"].to<JsonArray>();
      for (uint8_t i = 0; i < view.rows && i < UI_MODE_ROWS; i++)
      {
        JsonArray rowArr = rows.add<JsonArray>();
        rowArr.add(view.rowName[i]);
        rowArr.add(view.rowValue[i]);
      }
      if (view.bytes)
      {
        JsonArray bytesArr = screenObj["bytes"].to<JsonArray>();
        for (uint8_t i = 0; i < view.bytes && i < 8; i++)
        {
          bytesArr.add(view.data[i]);
        }
      }
    }
  }

  JsonObject canObj = doc["can"].to<JsonObject>();
  const CanStats &stats = canStats();
  canObj["up"] = stats.up;
  canObj["rx"] = stats.rx;
  canObj["tx"] = stats.tx;
  canObj["err"] = stats.err;
  canObj["rxPerSec"] = stats.rxPerSec;
  canObj["busOff"] = stats.busOff;
  canObj["haldexLink"] = signalValid(SIG_HALDEX_ACTUAL);
  canObj["haldexVia"] = haldexHeardVia(); // "CAN", "WiFi", "CAN + WiFi", ""
  canObj["termination"] = settings.canTermination;  // 0 Off, 1 On, 2 Auto (0.30.15)
  canObj["terminationOn"] = canTerminationIsOn();    // 0.30.15: the 120R in circuit right now
  canObj["listenOnly"] = canIsListenOnly(); // standby: nothing sent on the car's bus
  canObj["profile"] = settings.canProfile;
  { // OBD-II polling on the wire (profile OBD-II PIDs)
    const CanObdStats &obdStats = canObdStats();
    JsonObject obdObj = canObj["obd"].to<JsonObject>();
    obdObj["ecuFound"] = obdStats.ecuFound;
    obdObj["ecuId"] = obdStats.ecuId;                  // the engine ECU's answer id (0x7E8)
    obdObj["pidsPerRequest"] = obdStats.pidsPerRequest; // 6, or 1 when the ECU didn't cope with 6
    obdObj["answersPerSec"] = obdStats.answersPerSec;
    obdObj["pidsPerSec"] = obdStats.pidsPerSec;         // PID values decoded per second
    obdObj["requests"] = obdStats.requests;
    obdObj["misses"] = obdStats.misses;
    obdObj["answered"] = obdStats.answeredMask; // the ECUs that answered "0100": bit n = answer id 0x7E8 + n
  }
  { // the car's CAN frames relayed over WiFi (OpenHaldex / adc2can)
    const CanAirStats &airStats = canAirStats();
    JsonObject relayObj = canObj["relay"].to<JsonObject>();
    relayObj["on"] = settings.useCanRelay != 0;
    relayObj["perSec"] = airStats.perSec; // decoded
    relayObj["rx"] = airStats.rx;
    relayObj["ignored"] = airStats.ignored; // ids heard on the wire / from another sender
    relayObj["dropped"] = airStats.dropped;
    CanRelaySender senders[CANRELAY_SENDERS];
    const uint8_t senderCount = canRelaySenders(senders, CANRELAY_SENDERS);
    JsonArray sendersArr = relayObj["senders"].to<JsonArray>();
    for (uint8_t i = 0; i < senderCount; i++)
    {
      JsonObject senderObj = sendersArr.add<JsonObject>();
      char mac[20];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", senders[i].mac[0], senders[i].mac[1], senders[i].mac[2],
               senders[i].mac[3], senders[i].mac[4], senders[i].mac[5]);
      senderObj["kind"] = canRelayKindName(senders[i].kind);
      senderObj["mac"] = mac;
      senderObj["rssi"] = senders[i].rssi;
      senderObj["ageMs"] = senders[i].ageMs;
      senderObj["perSec"] = senders[i].perSec;
      senderObj["frames"] = senders[i].frames;
      senderObj["airLost"] = senders[i].airLost; // packets
      senderObj["srcLost"] = senders[i].srcLost; // frames
    }
  }

  JsonObject ecuObj = doc["ecu"].to<JsonObject>(); // an aftermarket ECU over WiFi from usb2ip (Diag tab)
  ecuObj["wireless"] = settings.ecuWireless;
  ecuObj["heard"] = ecuNowHeard();
  ecuObj["kind"] = ecuNowKindName(ecuNowKind());

  // the adc2can boards in a few words, for the Dashboard's ADC tile: a board can be heard while none of its sensors
  // has a valid value (e.g. on USB power: no 12 V, so the sensor rails are dead and ratiometric / NTC inputs read nothing)
  JsonObject adcObj = doc["adc"].to<JsonObject>();
  uint8_t boardsHeard = 0;  // adc2can boards heard in the last 2 s (CAN or WiFi)
  uint8_t sensorsValid = 0; // sensors those boards say are valid
  bool hasNo12v = false;    // a heard board says it has no 12 V (USB power only)
  for (uint8_t boardIndex = 0; boardIndex < ADC2CAN_BOARDS; boardIndex++)
  {
    const Adc2canBoard &board = adc2canBoard(boardIndex);
    if (!board.seen)
    {
      continue;
    }
    boardsHeard++;
    for (uint8_t channel = 0; channel < 8; channel++)
    {
      if (board.validMask & (1 << channel)) // bit n = sensor n+1 valid
      {
        sensorsValid++;
      }
    }
    if (board.flags & (1 << 6)) // status flags bit 6 = no 12 V (USB power only)
    {
      hasNo12v = true;
    }
  }
  adcObj["heard"] = boardsHeard;
  adcObj["valid"] = sensorsValid;
  adcObj["no12v"] = hasNo12v;

  JsonObject klineObj = doc["kline"].to<JsonObject>();
  klineObj["up"] = runtime.klineUp;
  klineObj["state"] = runtime.klineState;
  klineObj["ecu"] = runtime.klineEcuId;
  klineObj["samples"] = runtime.klineSamples;
  klineObj["me7Logging"] = runtime.me7Logging;
  klineObj["me7LogFile"] = runtime.me7LogFile;
  klineObj["me7Rate"] = runtime.me7SampleRate;

  JsonObject signalsObj = doc["signals"].to<JsonObject>();
  for (int i = 1; i < SIG_COUNT; i++)
  {
    signalId_t id = (signalId_t)i;
    if (!signalValid(id))
    {
      continue;
    }
    JsonObject signalObj = signalsObj[signalInfo(id).key].to<JsonObject>();
    signalObj["v"] = serialized(String(signalGet(id), 3));
    signalObj["n"] = signalName(id);
    signalObj["u"] = signalUnit(id);
    // 0.36.14: every value's decimals (the same rule as the screen: lambda 1.00, an ADC sensor its own), so the web UI
    // shows 1.00 rather than 1 (was ADC sensors only, from 0.17.0)
    signalObj["d"] = signalDecimals(id);
  }

  JsonArray outputsArr = doc["outputs"].to<JsonArray>();
  for (int i = 0; i < IOX_CHANNELS; i++)
  {
    outputsArr.add(ioxGet(i));
  }

  JsonObject ioObj = doc["io"].to<JsonObject>();
  ioObj["outputMode"] = settings.outputMode;
  ioObj["outputFault"] = ioxFault();
  ioObj["dashLights"] = dashLightsOn();
  ioObj["night"] = nightActive();

  JsonObject powerObj = doc["power"].to<JsonObject>(); // ignition on permanent power (src/can2gauge_Power.cpp)
  powerObj["state"] = powerState();
  powerObj["ignition"] = ignitionOn();
  powerObj["sleepInS"] = powerSleepInS();
  {
    char detail[80];
    powerDetail(detail, sizeof(detail));
    powerObj["detail"] = detail;
  }

  doc["saver"] = uiSaverActive();
  JsonArray triggersArr = doc["triggers"].to<JsonArray>();
  for (int i = 0; i < MAX_TRIGGERS; i++)
  {
    triggersArr.add(triggerActive(i));
  }

  JsonObject wifiObj = doc["wifi"].to<JsonObject>();
  wifiObj["staConnected"] = runtime.wifiStaConnected;
  wifiObj["staIP"] = runtime.wifiStaIP;
  wifiObj["apClients"] = WiFi.softAPgetStationNum();

  JsonObject dsgObj = doc["dsg"].to<JsonObject>();
  dsgObj["state"] = dsgStatus().state;
  dsgObj["selector"] = signalValid(SIG_SELECTOR) ? selectorLetter((uint8_t)signalGet(SIG_SELECTOR)) : "-";

  JsonObject elmObj = doc["elm"].to<JsonObject>();
  elmObj["enabled"] = settings.elmEnabled;
  elmObj["connected"] = elmConnected();
  elmObj["state"] = elmStatus().state;

  // 0.30.0: nav link (Google Maps turns from the can2gauge Link phone app). A static copy (in PSRAM: NavStatus is
  // ~180 bytes and this runs in the web server's task)
  EXT_RAM_BSS_ATTR static NavStatus nav;
  navStatus(nav);
  JsonObject navObj = doc["nav"].to<JsonObject>();
  navObj["mode"] = settings.navMode;
  navObj["started"] = nav.started;
  navObj["phone"] = nav.phoneConnected;
  navObj["advertising"] = nav.advertising;     // 0.30.5: the phone can see the gauge
  navObj["advertRestarts"] = nav.advertRestarts; // 0.30.5: times the advert had stopped and was restarted
  navObj["navigating"] = nav.navigating;
  navObj["card"] = nav.cardShowing;
  navObj["test"] = nav.testShowing;
  navObj["dist"] = nav.distText;
  navObj["instruction"] = nav.instruction;
  navObj["eta"] = nav.etaText;
  if (nav.routeProgress == NAV_PROGRESS_UNKNOWN) // 0.30.8: journey done, % (the ring), -1 = not known
  {
    navObj["progress"] = -1;
  }
  else
  {
    navObj["progress"] = nav.routeProgress / 10;
  }
  navObj["packets"] = nav.packets;
  navObj["ageMs"] = nav.ageMs;
  // switched on after boot and Bluetooth didn't fit, or (0.37.15) Bluetooth was already in use by the OBD adapter
  navObj["needsRestart"] = (navWanted() && !nav.started && bleNeedsRestart()) || navNeedsRestart();
  navObj["error"] = bleStartError();

  JsonObject chainObj = doc["chain"].to<JsonObject>();
  chainObj["enabled"] = settings.chainEnabled;
  chainObj["node"] = linkNodeId();
  chainObj["role"] = linkRoleName();

  JsonObject oilObj = doc["oil"].to<JsonObject>();
  oilObj["source"] = settings.oilSource;
  oilObj["state"] = runtime.oil.state;
  oilObj["connected"] = runtime.oil.connected;
  oilObj["connecting"] = runtime.oil.connecting;
  sendJSON(request, 200, doc);
}

// the oil page among the built (enabled) pages, -1 if none - uiGotoPage() counts enabled pages only
static int oilPageIndex(int *configIndex = NULL)
{
  int enabledCount = 0;
  for (int i = 0; i < MAX_PAGES; i++)
  {
    if (!settings.pages[i].enabled)
    {
      continue;
    }
    if (settings.pages[i].style == STYLE_OIL)
    {
      if (configIndex)
      {
        *configIndex = i;
      }
      return enabledCount;
    }
    enabledCount++;
  }
  return -1;
}

// GET /api/oil - OEM oil temp / level: source status, cluster session, raw measuring-block fields, Kombi_2
static void oilOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  const OilStatus &oil = runtime.oil;
  doc["source"] = settings.oilSource;
  doc["state"] = oil.state;
  doc["connecting"] = oil.connecting;
  doc["connected"] = oil.connected;
  doc["clusterId"] = oil.clusterId;
  doc["group"] = oil.group;
  doc["tempField"] = oil.tempField;
  doc["levelField"] = oil.levelField;
  doc["reads"] = oil.reads;
  doc["errors"] = oil.errors;
  doc["connects"] = oil.connects;
  doc["klineState"] = runtime.klineState;
  const uint32_t seen = runtime.oilPageSeenMs;
  doc["pageShowing"] = seen && millis() - seen < 500;
  int configIndex = -1;
  doc["page"] = oilPageIndex(&configIndex); // index among enabled pages, -1 = no oil page
  doc["pageCfg"] = configIndex;             // index in settings.pages
  if (signalValid(SIG_OIL_TEMP))
  {
    doc["temp"] = serialized(String(signalGet(SIG_OIL_TEMP), 1));
  }
  if (signalValid(SIG_OIL_LEVEL))
  {
    doc["levelOk"] = signalGet(SIG_OIL_LEVEL) >= 0.5f;
  }
  JsonArray fieldsArr = doc["fields"].to<JsonArray>();
  for (uint8_t i = 0; i < oil.fieldCount && i < 4; i++)
  {
    JsonObject fieldObj = fieldsArr.add<JsonObject>();
    fieldObj["formula"] = oil.formula[i];
    fieldObj["a"] = oil.a[i];
    fieldObj["b"] = oil.b[i];
    fieldObj["value"] = serialized(String(oil.value[i], 2));
    fieldObj["unit"] = oil.unit[i];
  }
  JsonObject kombi2Obj = doc["kombi2"].to<JsonObject>();
  kombi2Obj["seen"] = oil.kombi2Ms != 0;
  kombi2Obj["ageMs"] = oil.kombi2Ms ? millis() - oil.kombi2Ms : 0;
  kombi2Obj["count"] = oil.kombi2Count;
  kombi2Obj["raw"] = oil.kombi2OilRaw;
  kombi2Obj["err"] = oil.kombi2OilErr;
  kombi2Obj["canEnabled"] = settings.canEnabled;
  kombi2Obj["pqProfile"] = settings.canProfile == CAN_PROFILE_VAG_PQ;
  sendJSON(request, 200, doc);
}

static void dsgFieldsJson(JsonArray fieldsArr, const DsgField *fields, uint8_t fieldCount)
{
  for (uint8_t i = 0; i < fieldCount; i++)
  {
    JsonObject fieldObj = fieldsArr.add<JsonObject>();
    fieldObj["formula"] = fields[i].formula;
    fieldObj["a"] = fields[i].a;
    fieldObj["b"] = fields[i].b;
    fieldObj["value"] = serialized(String(fields[i].value, 2));
    fieldObj["unit"] = fields[i].unit;
  }
}

// GET /api/dsg - live gearbox view for the DSG tab
static void dsgOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  const DsgStatus &status = dsgStatus();
  doc["state"] = status.state;
  doc["detail"] = status.detail;
  doc["ok"] = status.okCount;
  doc["err"] = status.errCount;
  doc["nrc"] = status.lastNrc;
  doc["speedSupported"] = status.speedSupported;
  doc["gearSupported"] = status.gearSupported;
  // 0.26.0: the newer reads, their raw numbers (to work out a gearbox's codes) and the lock-up / shift / input speed
  doc["tempSupported"] = status.tempSupported;
  doc["lockupSupported"] = status.lockupSupported;
  doc["selectorSupported"] = status.selectorSupported;
  doc["lockupRaw"] = status.lockupRaw;     // -1 = none yet
  doc["selectorRaw"] = status.selectorRaw; // -1 = none yet
  if (signalValid(SIG_LOCKUP))
  {
    doc["lockup"] = (int)lroundf(signalGet(SIG_LOCKUP)); // 0 open, 1 slipping, 2 locked
  }
  if (signalValid(SIG_SHIFTING))
  {
    doc["shifting"] = signalGet(SIG_SHIFTING) > 0.5f;
  }
  if (signalValid(SIG_INPUT_RPM))
  {
    doc["inputRpm"] = (int)lroundf(signalGet(SIG_INPUT_RPM));
  }
  if (signalValid(SIG_CONVERTER_SLIP))
  {
    doc["slip"] = (int)lroundf(signalGet(SIG_CONVERTER_SLIP));
  }
  doc["tp20Tx"] = status.tp20Tx;
  doc["tp20Rx"] = status.tp20Rx;
  doc["selector"] = signalValid(SIG_SELECTOR) ? selectorLetter((uint8_t)signalGet(SIG_SELECTOR)) : "-";
  if (signalValid(SIG_GEAR))
  {
    doc["gear"] = (int)lroundf(signalGet(SIG_GEAR));
  }
  if (signalValid(SIG_TCU_SPEED))
  {
    doc["tcuSpeed"] = serialized(String(signalGet(SIG_TCU_SPEED), 1));
  }
  if (signalValid(SIG_SPEED))
  {
    doc["speed"] = serialized(String(signalGet(SIG_SPEED), 1));
  }
  if (signalValid(SIG_GEARBOX_TEMP))
  {
    doc["gboxTemp"] = serialized(String(signalGet(SIG_GEARBOX_TEMP), 0));
  }
  doc["master"] = linkIsMaster();
  dsgFieldsJson(doc["fields"].to<JsonArray>(), status.fields, status.fieldCount);
  dsgFieldsJson(doc["gearFields"].to<JsonArray>(), status.gearFields, status.gearFieldCount);
  sendJSON(request, 200, doc);
}

// GET /api/vdiag - the VAG diag tab: session state, the module's identification and each value. Polling it keeps the
// session open (VDIAG_RUN_SHOWN), like the K-line tab's live blocks.
static void vdiagOutgoing(AsyncWebServerRequest *request)
{
  runtime.vdiagSeenMs = millis() | 1; // | 1: never 0 (0 = never seen)
  const VdiagStatus &status = vdiagStatus();
  JsonDocument doc(psramJsonAllocator());
  doc["state"] = status.state;
  doc["detail"] = status.detail;
  doc["protocol"] = status.protocol;
  doc["txId"] = status.txId;               // 11 or 29-bit (0.34.0)
  doc["rxId"] = status.rxId;
  doc["viaAdapter"] = status.viaAdapter;   // 0.34.0: the session runs through the OBD adapter
  doc["partNumber"] = status.partNumber;
  doc["component"] = status.component;
  doc["swVersion"] = status.swVersion;
  doc["asamDataset"] = status.asamDataset;
  doc["asamVersion"] = status.asamVersion;
  doc["groupSize"] = status.groupSize;
  doc["turboGap"] = status.turboGap; // 0.38.0: Turbo stepped back to 1 ms gaps
  doc["ok"] = status.okCount;
  doc["err"] = status.errCount;
  doc["rate"] = status.samplesPerSec;
  doc["master"] = linkIsMaster();
  JsonArray valuesArr = doc["values"].to<JsonArray>();
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    const VdiagValueStatus &valueStatus = status.values[i];
    JsonObject valueObj = valuesArr.add<JsonObject>();
    valueObj["valid"] = valueStatus.valid;
    valueObj["supported"] = valueStatus.supported;
    valueObj["nrc"] = valueStatus.lastNrc;
    valueObj["unit"] = valueStatus.unit;
    if (valueStatus.valid)
    {
      valueObj["value"] = serialized(String(valueStatus.value, 2));
    }
    char rawHex[3 * 8 + 1] = ""; // the raw bytes as hex, for working out a DID's layout
    size_t written = 0;
    for (uint8_t b = 0; b < valueStatus.rawLen; b++)
    {
      appendf(rawHex, sizeof(rawHex), written, "%02X ", valueStatus.raw[b]);
    }
    valueObj["raw"] = rawHex;
    valueObj["didLen"] = valueStatus.didLen;
  }
  JsonObject probeObj = doc["probe"].to<JsonObject>(); // the "Try a DID" answer
  probeObj["id"] = status.probeId;
  probeObj["text"] = status.probeText;
  char probeHex[3 * 40 + 1] = "";
  size_t probeWritten = 0;
  for (uint8_t b = 0; b < status.probeLen; b++)
  {
    appendf(probeHex, sizeof(probeHex), probeWritten, "%02X ", status.probe[b]);
  }
  probeObj["hex"] = probeHex;
  sendJSON(request, 200, doc);
}

// GET /api/carscan (0.37.0) - the car scan job running (or the last one) and what came back, item by item
// (?brief=1: the progress only, while the web UI waits for the job)
static void carScanOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  carScanToJson(doc, !request->hasParam("brief"));
  sendJSON(request, 200, doc);
}

// POST /api/carscan (0.37.0) - starts a job: {"job":"uds","via":"can","items":[{"tx":2016,"req":"22F187"}, ...]},
// {"job":"tp20Blocks","addr":1,"blocks":[1,2,3]} ... (can2gauge_CarScan.h), or {"job":"stop"}
static void carScanIncoming(AsyncWebServerRequest *request, const String &body)
{
  JsonDocument doc(psramJsonAllocator());
  JsonDocument reply(psramJsonAllocator());
  if (deserializeJson(doc, body))
  {
    reply["error"] = "bad json";
    return sendJSON(request, 400, reply);
  }
  if (!strcmp(doc["job"] | "", "stop"))
  {
    carScanStop();
    return sendOk(request, "stopping");
  }
  char why[64] = "";
  if (!carScanStart(doc.as<JsonObjectConst>(), why, sizeof(why)))
  {
    reply["error"] = why;
    return sendJSON(request, 409, reply);
  }
  sendOk(request, "started");
}

// GET /api/lbl/find?addr=1&ident=4B0907551K (0.37.0) - the label file that fits a module, best first, for a module the
// car scan found over TP2.0 (the VAG diag task doesn't read files, so the web UI asks here)
static void labelFindOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  JsonArray filesArr = doc["files"].to<JsonArray>();
  if (request->hasParam("addr") && request->hasParam("ident") && labelsPrepare())
  {
    const uint8_t address = (uint8_t)request->getParam("addr")->value().toInt();
    char candidates[LBL_MAX_CANDS][LBL_NAME_LEN];
    const uint8_t count = labelsFind(address, request->getParam("ident")->value().c_str(), candidates, LBL_MAX_CANDS);
    for (uint8_t i = 0; i < count; i++)
    {
      filesArr.add(candidates[i]);
    }
  }
  sendJSON(request, 200, doc);
}

// a POST body for /api/carscan, put together and handed to carScanIncoming
static void carScanBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
  parseJSON(request, data, len, index, total, carScanIncoming);
}

// the POST itself is answered from carScanIncoming, once its body is in
static void carScanPostDone(AsyncWebServerRequest *request)
{
  (void)request;
}

// GET /api/vehicle (0.35.0) - the VIN the gauge read (and from where), and the car the driver confirmed
static void vehicleOutgoing(AsyncWebServerRequest *request)
{
  VehicleInfo info;
  CarChoice car;
  vehicleDetectedCopy(info);
  vehicleCarCopy(car);
  JsonDocument doc(psramJsonAllocator());
  JsonObject detectedObj = doc["detected"].to<JsonObject>();
  detectedObj["vin"] = info.vin;
  detectedObj["source"] = vehicleSourceName(info.source);
  detectedObj["make"] = info.make;
  detectedObj["makeKey"] = info.makeKey;
  detectedObj["model"] = info.model;
  detectedObj["modelCode"] = info.modelCode;
  detectedObj["year"] = info.year;
  JsonObject carObj = doc["car"].to<JsonObject>();
  carObj["confirmed"] = car.confirmed;
  carObj["vin"] = car.vin;
  carObj["makeKey"] = car.makeKey;
  carObj["make"] = car.make;
  carObj["modelCode"] = car.modelCode;
  carObj["model"] = car.model;
  carObj["year"] = car.year;
  carObj["body"] = car.body;
  carObj["paint"] = car.paint; // 0xRRGGBB
  char carLabel[60];
  vehicleCarLabel(carLabel, sizeof(carLabel));
  carObj["label"] = carLabel;                          // 0.36.2: "SEAT Ibiza IV", "" = no car saved
  doc["carLogo"] = fsExists(CAR_LOGO_PATH);              // 0.36.2: My Car's boot picture is on the gauge
  doc["canReading"] = vehicleCanReadBusy(); // the one-off VIN read over the gauge's CAN is still going
  sendJSON(request, 200, doc);
}

// GET /api/elm - adapter, vehicle, scan results and the terminal
static void elmOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  const ElmStatus &status = elmStatus();
  doc["enabled"] = settings.elmEnabled;
  doc["connected"] = elmConnected();
  doc["state"] = status.state;
  doc["mac"] = settings.elmMac;
  doc["link"] = elmLinkInfo();
  doc["transport"] = settings.elmTransport;
  doc["wifiSsid"] = settings.elmWifiSsid;
  doc["adapter"] = status.adapter;
  doc["stn"] = status.stn;
  doc["volts"] = serialized(String(status.volts, 1));
  doc["protocol"] = elmProtocolName(status.protocol);
  doc["vin"] = status.vin;
  doc["make"] = status.make;
  doc["model"] = status.model;
  doc["modelCode"] = status.modelCode;
  doc["year"] = status.year;
  doc["makeKey"] = status.makeKey;
  doc["reqPerSec"] = serialized(String(status.reqPerSec, 1));
  doc["requests"] = status.requests;
  doc["noData"] = status.noData;
  // Live OBD list: every standard value decoded in the last 10 s, [pid, name, unit, value]. The page asks for this
  // about once a second while the Bluetooth OBD tab is open, which also tells the polling to ask for every PID the car
  // has - not just the ones the gauge shows - for the next few seconds (obdWantAllPids)
  obdWantAllPids();
  JsonArray pidsArr = doc["pids"].to<JsonArray>();
  for (uint8_t tableIndex = 0; tableIndex < OBD_PID_COUNT; tableIndex++)
  {
    ObdPidInfo pidInfo;
    float value;
    uint32_t ageMs;
    if (!obdPidInfoAt(tableIndex, pidInfo) || !obdPidLatest(pidInfo.pid, value, ageMs) || ageMs > 10000)
    {
      continue;
    }
    JsonArray pidArr = pidsArr.add<JsonArray>();
    pidArr.add(pidInfo.pid);
    pidArr.add(pidInfo.name);
    pidArr.add(pidInfo.unit);
    pidArr.add(serialized(String(value, 3)));
    pidArr.add(ageMs); // 0.24.1: ms since this PID last came in (how fresh it is - the web UI ignores it)
  }
  doc["errors"] = status.errors;
  doc["lastError"] = status.lastError;
  doc["needsRestart"] = bleNeedsRestart(); // 0.24.22: Bluetooth didn't fit in RAM after boot - the web UI offers Restart Now
  doc["scanning"] = elmScanning();
  doc["master"] = linkIsMaster();
  JsonArray supportedArr = doc["supported"].to<JsonArray>();
  for (uint8_t pid = 1; pid <= 0x60; pid++)
  {
    const uint8_t block = (pid - 1) / 0x20;
    const uint8_t bit = (pid - 1) % 0x20;
    if ((status.supported[block] >> (31 - bit)) & 1)
    {
      supportedArr.add(pid);
    }
  }
  BleFound found[12];
  const uint8_t foundCount = elmFound(found, 12);
  JsonArray foundArr = doc["found"].to<JsonArray>();
  for (uint8_t i = 0; i < foundCount; i++)
  {
    JsonObject foundObj = foundArr.add<JsonObject>();
    foundObj["mac"] = found[i].mac;
    foundObj["type"] = found[i].addressType;
    foundObj["name"] = found[i].name;
    foundObj["rssi"] = found[i].rssi;
    foundObj["likely"] = found[i].likely;
  }
  // taken from PSRAM the first time this runs and kept (2.9 KB off internal RAM)
  static char *terminalBuf = (char *)psramCalloc(40 * 74, sizeof(char));
  elmTerminalText(terminalBuf, 40 * 74); // 40 * 74: the buffer's size (sizeof would only give the pointer's)
  doc["terminal"] = terminalBuf;
  const ElmTestStatus &test = elmTestStatus();
  JsonObject testObj = doc["test"].to<JsonObject>();
  testObj["running"] = test.running;
  testObj["runs"] = test.runs;
  testObj["kind"] = test.kind;
  testObj["step"] = test.step;
  testObj["file"] = test.file;
  testObj["result"] = test.result;
  sendJSON(request, 200, doc);
}

// POST /api/elm/action {"action":"scan"|"pair"|"forget"|"reidentify"|"cmd", "mac", "type", "cmd"}
static void elmActionIncoming(AsyncWebServerRequest *request, const String &body)
{
  JsonDocument doc(psramJsonAllocator());
  if (deserializeJson(doc, body) || !doc["action"].is<const char *>())
  {
    JsonDocument errorDoc(psramJsonAllocator());
    errorDoc["error"] = "bad json";
    sendJSON(request, 400, errorDoc);
    return;
  }
  const char *action = doc["action"];
  if (!strcmp(action, "scan"))
  {
    elmRequestScan();
    sendOk(request, "scanning (5 s)");
  }
  else if (!strcmp(action, "pair") && doc["mac"].is<const char *>())
  {
    elmPair(doc["mac"], doc["type"] | 0);
    sendOk(request, "adapter saved - connecting");
  }
  else if (!strcmp(action, "forget"))
  {
    elmForget();
    sendOk(request, "adapter forgotten");
  }
  else if (!strcmp(action, "reidentify"))
  {
    elmReidentify();
    sendOk(request, "re-identifying");
  }
  else if (!strcmp(action, "cmd") && doc["cmd"].is<const char *>())
  {
    sendOk(request, elmQueueCommand(doc["cmd"]) ? "sent" : "busy");
  }
  else if (!strcmp(action, "test") && doc["kind"].is<const char *>())
  {
    // bench tests (0.8.0): {"kind":"all"|"probe"|"raw"|"dtc"|"tp20"|"kline", "module":1, "group":1}
    if (elmStartTest(doc["kind"], doc["module"] | 0, doc["group"] | 0))
    {
      sendOk(request, "test started - the log goes to /logs");
    }
    else
    {
      JsonDocument errorDoc(psramJsonAllocator());
      errorDoc["error"] = elmConnected() ? "a test is already running (or unknown kind)" : "adapter not connected";
      sendJSON(request, 409, errorDoc);
    }
  }
  else
  {
    JsonDocument errorDoc(psramJsonAllocator());
    errorDoc["error"] = "unknown action";
    sendJSON(request, 400, errorDoc);
  }
}

// GET /api/haldex - OpenHaldex: how it is heard (CAN / WiFi), its full state over WiFi, the last command's answer
static void macStr(char *buf, size_t len, const uint8_t *mac)
{
  snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void haldexOutgoing(AsyncWebServerRequest *request)
{
  EXT_RAM_BSS_ATTR static OhxView view; // ~400 bytes: off the async_tcp stack
  ohxView(view);
  const OhxStatus &status = view.status;
  JsonDocument doc(psramJsonAllocator());
  char mac[20];
  doc["via"] = haldexHeardVia();
  doc["canHeard"] = haldexHeardOnCan();
  doc["wireless"] = settings.ohxWireless;
  doc["carSignals"] = settings.ohxCarSignals;
  doc["keySet"] = settings.ohxKey[0] != '\0';
  doc["heard"] = view.heard;
  doc["ageMs"] = view.ageMs;
  doc["myChannel"] = view.myChannel;
  macStr(mac, sizeof(mac), view.mac);
  doc["mac"] = (view.mac[0] | view.mac[1] | view.mac[2] | view.mac[3] | view.mac[4] | view.mac[5]) ? mac : "";
  if (view.ageMs) // a status has been heard: everything it carries (the web UI greys it out once stale)
  {
    JsonObject ohxObj = doc["ohx"].to<JsonObject>();
    ohxObj["name"] = status.name;
    ohxObj["rssi"] = view.rssi;
    ohxObj["channel"] = status.channel;
    char fw[16];
    snprintf(fw, sizeof(fw), "%u.%02u.%u", status.fw[0], status.fw[1], status.fw[2]);
    ohxObj["fw"] = fw;
    ohxObj["generation"] = status.generation;
    ohxObj["generationName"] = ohxGenerationName(status.generation);
    ohxObj["mode"] = status.mode;
    ohxObj["standalone"] = (status.flags[0] & OHX_F0_STANDALONE) != 0;
    ohxObj["chassisCan"] = (status.flags[0] & OHX_F0_CHASSIS_CAN) != 0;
    ohxObj["haldexCan"] = (status.flags[0] & OHX_F0_HALDEX_CAN) != 0;
    ohxObj["controllerOff"] = (status.flags[0] & OHX_F0_CTRL_OFF) != 0;
    ohxObj["busFailure"] = (status.flags[0] & OHX_F0_BUS_FAIL) != 0;
    ohxObj["override"] = (status.flags[0] & OHX_F0_OVERRIDE) != 0;
    ohxObj["canBroadcast"] = (status.flags[0] & OHX_F0_CAN_BCAST) != 0;
    ohxObj["learned"] = (status.flags[0] & OHX_F0_LEARNED) != 0;
    ohxObj["tcForce"] = (status.flags[1] & OHX_F1_TC_FORCE) != 0;
    ohxObj["hazard"] = (status.flags[1] & OHX_F1_HAZARD) != 0;
    ohxObj["extButton"] = (status.flags[1] & OHX_F1_EXT_BTN) != 0;
    ohxObj["brake"] = (status.flags[1] & OHX_F1_BRAKE) != 0;
    ohxObj["handbrake"] = (status.flags[1] & OHX_F1_HANDBRAKE) != 0;
    ohxObj["tempProtection"] = (status.flags[1] & OHX_F1_TEMP_PROT) != 0;
    ohxObj["couplingOpen"] = (status.flags[1] & OHX_F1_COUPLING_OPEN) != 0;
    ohxObj["speedLimit"] = (status.flags[1] & OHX_F1_SPEED_LIMIT) != 0;
    ohxObj["control"] = (status.flags[2] & OHX_F2_CONTROL) != 0;
    ohxObj["keyRequired"] = (status.flags[2] & OHX_F2_KEY) != 0;
    ohxObj["fixHunting"] = (status.flags[2] & OHX_F2_FIX_HUNTING) != 0;
    ohxObj["steerScale"] = (status.flags[2] & OHX_F2_STEER_SCALE) != 0;
    ohxObj["analyzer"] = (status.flags[2] & OHX_F2_ANALYZER) != 0;
    ohxObj["liveDiag"] = (status.flags[2] & OHX_F2_LIVE_DIAG) != 0;
    ohxObj["asrOff"] = (status.flags[3] & OHX_F3_ASR_OFF) != 0;
    ohxObj["lockTarget"] = status.lockTarget;
    if (status.lockActual != OHX_NONE8)
    {
      ohxObj["lockActual"] = status.lockActual;
    }
    ohxObj["engagementRaw"] = status.engagementRaw;
    ohxObj["haldexState"] = status.haldexState;
    if (status.speed != OHX_NONE16)
    {
      ohxObj["speed"] = status.speed;
    }
    if (status.rpm != OHX_NONE16)
    {
      ohxObj["rpm"] = status.rpm;
    }
    if (status.boost != OHX_NONE16)
    {
      ohxObj["boost"] = status.boost;
    }
    if (status.throttle != OHX_NONE8)
    {
      ohxObj["throttle"] = status.throttle;
    }
    if (status.steering != OHX_NONE16)
    {
      ohxObj["steering"] = status.steering;
    }
    JsonArray slipArr = ohxObj["slip"].to<JsonArray>();
    for (int i = 0; i < 4; i++)
    {
      if (status.slip[i] == -128)
      {
        slipArr.add(nullptr);
      }
      else
      {
        slipArr.add(status.slip[i]);
      }
    }
    ohxObj["disableThrottle"] = status.disableThrottle;
    ohxObj["disengageUnder"] = status.disengageUnderSpeed;
    ohxObj["disengageAbove"] = status.disengageAboveSpeed;
    JsonObject learnObj = ohxObj["learn"].to<JsonObject>();
    learnObj["active"] = (status.flags[2] & OHX_F2_LEARN) != 0;
    learnObj["step"] = status.learnStep;
    JsonObject longLearnObj = ohxObj["longLearn"].to<JsonObject>();
    longLearnObj["active"] = (status.flags[2] & OHX_F2_LONG_LEARN) != 0;
    longLearnObj["phase"] = status.longPhase;
    longLearnObj["sweep"] = status.longSweep;
    longLearnObj["total"] = status.longTotal;
    if (status.clutchTemp != OHX_NONE_T)
    {
      ohxObj["clutchTemp"] = status.clutchTemp / 10.0f;
    }
    if (status.oilTemp != OHX_NONE_T)
    {
      ohxObj["oilTemp"] = status.oilTemp / 10.0f;
    }
    if (status.moduleTemp != OHX_NONE_T)
    {
      ohxObj["moduleTemp"] = status.moduleTemp / 10.0f;
    }
    if (status.supplyMv != OHX_NONE16)
    {
      ohxObj["supply"] = status.supplyMv / 1000.0f;
    }
  }

  JsonObject cmdObj = doc["cmd"].to<JsonObject>();
  static const char *states[] = {"none", "sending", "done", "no answer"};
  cmdObj["state"] = states[view.cmdState < 4 ? view.cmdState : 0];
  cmdObj["cmd"] = view.cmd;
  cmdObj["arg"] = view.arg;
  cmdObj["ok"] = view.cmdState == OHX_CS_DONE && view.result == OHX_R_OK;
  cmdObj["text"] = view.cmdState == OHX_CS_NO_ANSWER ? "no answer from the OpenHaldex (out of range / other WiFi channel?)" : ohxResultText(view.result);
  cmdObj["ageMs"] = view.cmdAgeMs;

  JsonArray othersArr = doc["others"].to<JsonArray>();
  for (uint8_t i = 0; i < view.others; i++)
  {
    JsonObject otherObj = othersArr.add<JsonObject>();
    macStr(mac, sizeof(mac), view.otherMac[i]);
    otherObj["mac"] = mac;
    otherObj["name"] = view.otherName[i];
    otherObj["rssi"] = view.otherRssi[i];
  }
  sendJSON(request, 200, doc);
}

// GET /api/ecu - an aftermarket ECU over WiFi from usb2ip: which usb2ip is followed, how it is heard, its everyday
// values, whether its channel list matches this firmware's, and the others in range
static void ecuOutgoing(AsyncWebServerRequest *request)
{
  EXT_RAM_BSS_ATTR static EcuNowView view; // static: about 300 bytes, kept off the web server's small stack
  ecuNowView(view);
  JsonDocument doc(psramJsonAllocator());
  char text[24];
  doc["wireless"] = settings.ecuWireless;
  doc["carSignals"] = settings.ecuCarSignals;
  doc["savedKind"] = settings.ecuKind; // the ECU the last usb2ip heard reads (the page editor lists its channels)
  doc["heard"] = view.heard;
  doc["everHeard"] = view.everHeard;
  doc["ageMs"] = view.ageMs;
  doc["myChannel"] = view.myChannel;
  bool hasMac = false;
  for (uint8_t i = 0; i < 6; i++)
  {
    if (view.mac[i] != 0)
    {
      hasMac = true;
    }
  }
  if (hasMac)
  {
    macStr(text, sizeof(text), view.mac);
    doc["mac"] = text;
  }
  else
  {
    doc["mac"] = "";
  }
  if (view.everHeard) // something has been heard from it: everything it said (the web UI greys it out once stale)
  {
    JsonObject usbObj = doc["usb2ip"].to<JsonObject>();
    usbObj["kind"] = view.kind;
    usbObj["kindName"] = ecuNowKindName(view.kind);
    usbObj["source"] = view.source;
    usbObj["ecu"] = view.ecu;
    usbObj["rssi"] = view.rssi;
    snprintf(text, sizeof(text), "%u.%u", view.fwMajor, view.fwMinor);
    usbObj["fw"] = text;
    usbObj["live"] = view.live;
    usbObj["fromPc"] = view.fromPc;
    usbObj["packets"] = view.packets;
    usbObj["lost"] = view.lost;
    snprintf(text, sizeof(text), "%04X", view.theirLibraryId);
    usbObj["theirLibrary"] = text; // "0000" = not said yet
    snprintf(text, sizeof(text), "%04X", view.ourLibraryId);
    usbObj["ourLibrary"] = text;
    usbObj["libraryMismatch"] = view.libraryMismatch;
    usbObj["rowsTotal"] = view.rowsTotal;
    usbObj["rowsReceiving"] = view.rowsReceiving;
    JsonArray valuesArr = doc["values"].to<JsonArray>();
    for (uint8_t i = 0; i < ECUN_VALUES; i++)
    {
      if (!isfinite(view.value[i]))
      {
        continue; // not sent: this ECU doesn't have it
      }
      signalId_t id = ecuNowValueSignal(i);
      JsonObject valueObj = valuesArr.add<JsonObject>();
      valueObj["key"] = signalInfo(id).key;
      valueObj["name"] = signalInfo(id).name;
      valueObj["unit"] = signalInfo(id).unit;
      valueObj["value"] = serialized(String(view.value[i], 3));
      valueObj["used"] = view.valueUsed[i]; // false = another source on the gauge provides it
    }
  }
  JsonArray othersArr = doc["others"].to<JsonArray>();
  for (uint8_t i = 0; i < view.others; i++)
  {
    JsonObject otherObj = othersArr.add<JsonObject>();
    macStr(text, sizeof(text), view.otherMac[i]);
    otherObj["mac"] = text;
    otherObj["name"] = view.otherName[i];
    otherObj["rssi"] = view.otherRssi[i];
  }
  sendJSON(request, 200, doc);
}

// a whole-number query parameter (?kind=1), or fallback when it isn't there
static int queryInt(AsyncWebServerRequest *request, const char *name, int fallback)
{
  if (!request->hasParam(name))
  {
    return fallback;
  }
  const AsyncWebParameter *parameter = request->getParam(name); // the web server hands it over by its address
  return parameter->value().toInt();
}

// GET /api/ecu/library?kind=1                the channel list of that ECU: its ID, size and groups
// GET /api/ecu/library?kind=1&group=2&page=0 the channels of one group, ECU_LIBRARY_PAGE at a time ("more": true =
//                                            ask for the next page). Small pieces keep the JSON small on the heap
//                                            (an EMU PRO's Engine group has 500+ channels).
#define ECU_LIBRARY_PAGE 100
static void ecuLibraryOutgoing(AsyncWebServerRequest *request)
{
  int kind = queryInt(request, "kind", settings.ecuKind);
  int group = queryInt(request, "group", -1);
  int page = queryInt(request, "page", 0);
  uint16_t rows = ecuNowLibraryRows(kind);
  JsonDocument doc(psramJsonAllocator());
  doc["kind"] = kind;
  doc["kindName"] = ecuNowKindName(kind);
  doc["rows"] = rows;
  char text[8];
  snprintf(text, sizeof(text), "%04X", ecuNowLibraryId(kind));
  doc["id"] = text;
  EcuChannelInfo info;
  if (group < 0)
  {
    JsonArray groupsArr = doc["groups"].to<JsonArray>();
    for (uint8_t groupIndex = 0; groupIndex < ecuNowGroupCount(); groupIndex++)
    {
      uint16_t count = 0;
      for (uint16_t row = 0; row < rows; row++)
      {
        if (ecuNowRowInfo(kind, row, info) && info.group == groupIndex)
        {
          count++;
        }
      }
      JsonObject groupObj = groupsArr.add<JsonObject>();
      groupObj["name"] = ecuNowGroupName(groupIndex);
      groupObj["count"] = count;
    }
  }
  else
  {
    doc["group"] = group;
    doc["page"] = page;
    JsonArray channelsArr = doc["channels"].to<JsonArray>();
    uint16_t found = 0;   // channels of this group so far
    bool hasMore = false; // more after this page
    for (uint16_t row = 0; row < rows; row++)
    {
      if (!ecuNowRowInfo(kind, row, info) || info.group != group)
      {
        continue;
      }
      found++;
      if (found <= page * ECU_LIBRARY_PAGE)
      {
        continue; // on an earlier page
      }
      if (found > (page + 1) * ECU_LIBRARY_PAGE)
      {
        hasMore = true; // on a later page
        break;
      }
      JsonObject channelObj = channelsArr.add<JsonObject>();
      channelObj["c"] = info.channel;  // the ECU's channel number: what a page stores ("ecu:<kind>:<channel>")
      channelObj["k"] = info.key;
      channelObj["n"] = info.name;
      channelObj["u"] = info.unit;     // as the ECU's software writes it (UTF-8)
      channelObj["d"] = info.decimals;
      channelObj["lo"] = info.min;
      channelObj["hi"] = info.max;
      float value = 0;
      if (ecuNowRowValue(kind, row, value))
      {
        unsigned int decimals = info.decimals; // String() wants this type for the number of decimals
        channelObj["v"] = serialized(String(value, decimals)); // being received right now
      }
    }
    doc["more"] = hasMore;
  }
  sendJSON(request, 200, doc);
}

// GET /api/chain - this gauge + every other gauge heard on the link
static void chainOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  const LinkStats &stats = linkStats();
  doc["enabled"] = settings.chainEnabled;
  doc["node"] = linkNodeId();
  doc["role"] = linkRoleName();
  doc["name"] = settings.nodeName;
  doc["group"] = settings.chainGroup;
  doc["espNowUp"] = stats.espNowUp;
  doc["canHeard"] = stats.canHeard;
  doc["txCan"] = stats.txCan;
  doc["rxCan"] = stats.rxCan;
  doc["txNow"] = stats.txNow;
  doc["rxNow"] = stats.rxNow;
  doc["rejected"] = stats.rejected;
  doc["collisions"] = stats.collisions;
  // the row wave: where this gauge stands among the gauges in the row heard right now (place 0 = leftmost)
  uint8_t rowPlace = 0;
  uint8_t rowGauges = 0;
  const bool inRow = linkRowPlace(rowPlace, rowGauges);
  doc["rowPosition"] = settings.rowPosition;
  doc["rowInRow"] = inRow;
  doc["rowPlace"] = rowPlace + 1; // 1 = leftmost, as the web UI shows it
  doc["rowGauges"] = rowGauges;
  doc["roleSetting"] = settings.chainRole;             // 0.29.0: as set (0 Auto, 1 Master, 2 Follower) for the gauge table
  doc["hasShutdown"] = settings.shutdownSprite[0] != 0; // 0.29.0: a shutdown sprite is picked (can join Play It Now)
  // 0.30.10: the last 4 hex of its MAC, the same as on the end of its WiFi name (can2gauge-XXYY), so it's easy to spot
  const uint64_t efuseMac = ESP.getEfuseMac(); // byte 0 = first octet
  char myMacText[5];
  snprintf(myMacText, sizeof(myMacText), "%02X%02X", (uint8_t)((efuseMac >> 32) & 0xFF),
           (uint8_t)((efuseMac >> 40) & 0xFF));
  doc["mac"] = myMacText;
  doc["ignSource"] = settings.ignSource;                // 0.30.4: ignSource_t (3 = follows another gauge)
  JsonArray sharedArr = doc["shared"].to<JsonArray>();
  for (int i = 1; i < SIG_COUNT; i++)
  {
    if (signalValid((signalId_t)i) && signalOrigin((signalId_t)i) == SIG_ORIGIN_REMOTE)
    {
      JsonObject sharedObj = sharedArr.add<JsonObject>();
      sharedObj["key"] = signalInfo((signalId_t)i).key;
      sharedObj["from"] = signalSourceNode((signalId_t)i);
    }
  }
  LinkNode nodes[16];
  const uint8_t nodeCount = linkNodes(nodes, 16);
  JsonArray nodesArr = doc["nodes"].to<JsonArray>();
  for (uint8_t i = 0; i < nodeCount; i++)
  {
    const LinkNode &node = nodes[i];
    JsonObject nodeObj = nodesArr.add<JsonObject>();
    nodeObj["node"] = node.node;
    nodeObj["name"] = node.name;
    if (node.role == 1)
    {
      nodeObj["role"] = "master";
    }
    else if (node.role == 2)
    {
      nodeObj["role"] = "follower";
    }
    else
    {
      nodeObj["role"] = "standalone";
    }
    nodeObj["fw"] = String(node.fwMajor) + "." + String(node.fwMinor);
    nodeObj["proto"] = node.proto;
    nodeObj["sources"] = node.flags;
    nodeObj["owned"] = node.owned;
    nodeObj["via"] = String((node.via & 1) ? "CAN " : "") + ((node.via & 2) ? "ESP-NOW" : "");
    nodeObj["rssi"] = node.rssi;
    nodeObj["age"] = millis() - node.lastSeen;
    nodeObj["uptime"] = node.uptimeS;
    nodeObj["row"] = node.rowPosition; // its place in the row wave, 0 = not in a row
    // 0.29.0: its role as set on it (from the heartbeat's flags): 0 Auto, 1 Master, 2 Follower
    if (node.flags & LINK_SRC_FORCED)
    {
      nodeObj["roleSetting"] = (uint8_t)CHAIN_MASTER; // written as a plain number (ArduinoJson takes numbers, not enum names)
    }
    else if (node.flags & LINK_SRC_FOLLOWER)
    {
      nodeObj["roleSetting"] = (uint8_t)CHAIN_FOLLOWER;
    }
    else
    {
      nodeObj["roleSetting"] = (uint8_t)CHAIN_AUTO;
    }
    // 0.29.0: a shutdown sprite picked on it. Only 0.29.0 and later send this bit, so an older gauge counts as "has one"
    const uint32_t nodeVersion = node.version[0] * 10000u + node.version[1] * 100u + node.version[2];
    nodeObj["hasShutdown"] = (node.flags & LINK_SRC_SHUTDOWN) != 0 || nodeVersion < 2900;
    // 0.25.0: full version (ESP-NOW heartbeats from 0.25.0 on; "" = an older gauge or heard on CAN only)
    if (node.updateProto >= 1)
    {
      nodeObj["version"] = String(node.version[0]) + "." + String(node.version[1]) + "." + String(node.version[2]);
    }
    else
    {
      nodeObj["version"] = "";
    }
    nodeObj["canUpdate"] = node.updateProto >= 1 && (node.via & 2) != 0; // can take a chain update (bit 1 = ESP-NOW)
    // 0.30.4: its MAC address (heard over WiFi only; "" on CAN), so a gauge in the table can be told from the others.
    // 0.30.10: just the last 4 hex, as on the end of its WiFi name. It's heard from its WiFi access point MAC, which
    // is the chip's own MAC with 1 added to the last byte, so 1 comes off again to match the name.
    bool hasMac = false;
    for (uint8_t i = 0; i < 6; i++)
    {
      if (node.mac[i] != 0)
      {
        hasMac = true;
      }
    }
    if (hasMac)
    {
      const uint8_t lastByte = node.mac[5] - 1; // a uint8_t wraps round like the chip does (0x00 - 1 = 0xFF)
      char macText[5];
      snprintf(macText, sizeof(macText), "%02X%02X", node.mac[4], lastByte);
      nodeObj["mac"] = macText;
    }
    else
    {
      nodeObj["mac"] = "";
    }
    // 0.30.4: its ignition source (-1 = it doesn't say: older, or heard on CAN only) and whether its shared settings
    // match this gauge's
    if (node.ignKind > 0)
    {
      nodeObj["ignSource"] = node.ignKind - 1;
      nodeObj["sharedSame"] = node.sharedCheck == sharedMyCheck();
    }
    else
    {
      nodeObj["ignSource"] = -1;
    }
    nodeObj["sameBoard"] = node.board == chainUpdateBoardId();
  }
  // 0.25.0: Update All Gauges - what's happening and how each gauge got on
  doc["version"] = CAN2GAUGE_VERSION;
  EXT_RAM_BSS_ATTR static ChainUpdateStatus updateStatus; // static: ~1.2 KB, too big for the web server task's stack
  chainUpdateGetStatus(updateStatus);
  JsonObject updateObj = doc["update"].to<JsonObject>();
  updateObj["sending"] = updateStatus.sending;
  updateObj["receiving"] = updateStatus.receiving;
  updateObj["percent"] = updateStatus.percent;
  updateObj["text"] = updateStatus.text;
  JsonArray resultsArr = updateObj["results"].to<JsonArray>();
  for (uint8_t i = 0; i < updateStatus.resultCount; i++)
  {
    JsonObject resultObj = resultsArr.add<JsonObject>();
    resultObj["node"] = updateStatus.results[i].node;
    resultObj["finished"] = updateStatus.results[i].finished;
    resultObj["worked"] = updateStatus.results[i].worked;
    resultObj["text"] = updateStatus.results[i].text;
  }
  sendJSON(request, 200, doc);
}

// GET /api/signals - the catalogue (for dropdowns)
static void signalsOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  JsonArray signalsArr = doc["signals"].to<JsonArray>();
  for (int i = 0; i < SIG_COUNT; i++)
  {
    signalId_t id = (signalId_t)i;
    JsonObject signalObj = signalsArr.add<JsonObject>();
    signalObj["key"] = signalInfo(id).key;
    signalObj["name"] = signalName(id);
    signalObj["unit"] = signalUnit(id);
    signalObj["min"] = signalInfo(id).defMin;
    signalObj["max"] = signalInfo(id).defMax;
    signalObj["redline"] = signalInfo(id).defRedline; // 0.16.0: a new trigger's starting threshold (0 = none)
  }
  // every standard OBD value the gauge can read (CANLine's table), for a page's "OBD" source picker:
  // [pid, name, unit, gauge min, gauge max, the gauge signal's key ("" = none)]
  JsonArray obdArr = doc["obdPids"].to<JsonArray>();
  for (uint8_t tableIndex = 0; tableIndex < OBD_PID_COUNT; tableIndex++)
  {
    ObdPidInfo pidInfo;
    if (!obdPidInfoAt(tableIndex, pidInfo))
    {
      continue;
    }
    JsonArray pidArr = obdArr.add<JsonArray>();
    pidArr.add(pidInfo.pid);
    pidArr.add(pidInfo.name);
    pidArr.add(pidInfo.unit);
    pidArr.add(pidInfo.gaugeMin);
    pidArr.add(pidInfo.gaugeMax);
    if (pidInfo.signal != SIG_NONE)
    {
      pidArr.add(signalInfo(pidInfo.signal).key);
    }
    else
    {
      pidArr.add("");
    }
  }
  JsonArray stylesArr = doc["styles"].to<JsonArray>();
  for (int i = 0; i < gaugeStyle_t_MAX; i++)
  {
    stylesArr.add(uiStyleName(i));
  }
  JsonArray outputsArr = doc["outputs"].to<JsonArray>();
  for (int i = 0; i < IOX_CHANNELS; i++)
  {
    outputsArr.add(ioxChannelName(i));
  }
  sendJSON(request, 200, doc);
}

// GET /api/themes - every theme (display order) and the make logos, with which logo files are on LittleFS
static void themesOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  char hex[8];
  JsonArray themesArr = doc["themes"].to<JsonArray>();
  for (uint8_t i = 0; i < themeCount(); i++)
  {
    const ThemeDef &theme = themeAt(i);
    JsonObject themeObj = themesArr.add<JsonObject>();
    themeObj["id"] = theme.id;
    themeObj["name"] = theme.name;
    themeObj["make"] = theme.make ? theme.make : "";
    themeObj["era"] = theme.era;
    snprintf(hex, sizeof(hex), "#%06lX", (unsigned long)theme.dial);
    themeObj["dial"] = hex;
    snprintf(hex, sizeof(hex), "#%06lX", (unsigned long)theme.needle);
    themeObj["needle"] = hex;
    themeObj["conf"] = theme.conf;
  }
  JsonArray logosArr = doc["logos"].to<JsonArray>();
  for (uint8_t i = 0; i < logoCount(); i++)
  {
    const LogoDef &logo = logoAt(i);
    JsonObject logoObj = logosArr.add<JsonObject>();
    logoObj["key"] = logo.key;
    logoObj["name"] = logo.name;
    String logoPath = String("/logos/") + logo.key + ".bin";
    logoObj["present"] = fsExists(logoPath.c_str()); // 0.24.0: shipped zipped (.bin.gz)
  }
  doc["matched"] = settings.themeMatched;
  doc["matchedName"] = settings.themeMatched != THEME_NONE ? themeName(settings.themeMatched) : "";
  doc["matchedLogo"] = settings.matchedLogo;
  doc["vin"] = elmStatus().vin;
  doc["logoKey"] = themeLogoKey() ? themeLogoKey() : "";
  sendJSON(request, 200, doc);
}

// GET /api/sprites - the sprite packs on the gauge (/sprites/<key>.c2gs, include/c2gs.h): what each one can show
static void spritesOutgoing(AsyncWebServerRequest *request)
{
  static const char *roles[c2gsRole_t_MAX] = {"mood", "event", "tap", "saver", "shutdown", "sleep", "wake", "rowLeft", "rowRight"};
  JsonDocument doc(psramJsonAllocator());
  JsonArray spritesArr = doc["sprites"].to<JsonArray>();
  File dir = LittleFS.open("/sprites");
  for (File file = dir ? dir.openNextFile() : File(); file; file = dir.openNextFile())
  {
    const char *fileName = file.name();
    const char *dot = strrchr(fileName, '.');
    if (file.isDirectory() || !dot || strcasecmp(dot, ".c2gs"))
    {
      continue;
    }
    JsonObject spriteObj = spritesArr.add<JsonObject>();
    spriteObj["key"] = String(fileName).substring(0, dot - fileName);
    spriteObj["size"] = file.size();
    C2gsHeader header;
    if (dot - fileName >= SPRITE_NAME_LEN)
    {
      spriteObj["error"] = "file name too long (15 characters at most)";
    }
    else if (file.read((uint8_t *)&header, sizeof(header)) != sizeof(header) || memcmp(header.magic, C2GS_MAGIC, 4) ||
             (header.version != C2GS_VERSION && header.version != C2GS_VERSION_ZIPPED) || !header.stateCount || header.stateCount > C2GS_MAX_STATES || !header.clipCount ||
             header.clipCount > C2GS_MAX_CLIPS)
    {
      spriteObj["error"] = "not a C2GS sprite pack";
    }
    else
    {
      char name[sizeof(header.name) + 1];
      memcpy(name, header.name, sizeof(header.name));
      name[sizeof(header.name)] = '\0';
      spriteObj["name"] = name;
      spriteObj["w"] = header.w;
      spriteObj["h"] = header.h;
      spriteObj["frames"] = header.frameCount;
      spriteObj["clips"] = header.clipCount;
      spriteObj["alpha"] = (header.flags & C2GS_ALPHA) != 0;
      spriteObj["themed"] = (header.flags & C2GS_LAYERED) != 0; // takes the theme's accent
      JsonArray statesArr = spriteObj["states"].to<JsonArray>();
      file.seek(sizeof(header) + header.clipCount * sizeof(C2gsClip));
      for (uint8_t i = 0; i < header.stateCount; i++)
      {
        C2gsState state;
        if (file.read((uint8_t *)&state, sizeof(state)) != sizeof(state))
        {
          break;
        }
        JsonObject stateObj = statesArr.add<JsonObject>();
        stateObj["role"] = state.role < c2gsRole_t_MAX ? roles[state.role] : "?";
        stateObj["fps"] = state.fpsLo;
      }
    }
  }
  size_t total = 0;
  size_t used = 0;
  fsUsage(total, used); // 0.17.4: the house task's numbers (LittleFS.usedBytes() here took over 5 s - watchdog reset)
  doc["total"] = total;
  doc["used"] = used;
  sendJSON(request, 200, doc);
}

// GET /api/settings, POST /api/settings (partial merge)
static void settingsOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  settingsToJson(doc);
  sendJSON(request, 200, doc);
}

static void applyRuntime(bool wifiChanged, bool pagesChanged, bool themeChanged, bool canChanged, bool klineChanged)
{
  if (lvglLock(50))
  {
    lv_indev_set_long_press_time(lvglTouch(), settings.longPressMs);
    c2gSimdEnable(settings.simdBlend); // takes effect from the next frame
#if SCREEN_ROTATION
    lvglSetRotation(settings.upsideDown == 1, settings.rotateTrim / 2.0f); // 0.25.1: turns the picture from the next frame
#endif
    lvglUnlock();
  }
  canTerminationFromSettings(); // 0.30.15: Off / On / Auto
  outputsApply();
  if (pagesChanged)
  {
    userSigApply(); // page sources: names / units / DBC lookups
  }
  adc2canApply();   // adc2can channel names / units (web UI > ADC)
  if (themeChanged)
  {
    uiApplyTheme(); // every screen, incl. pages + menu
  }
  else if (pagesChanged)
  {
    uiRebuildPages();
    uiRebuildMenu();
  }
  if (canChanged)
  {
    canEnd();
    if (settings.canEnabled)
    {
      canBegin(CAN_PIN_TX, CAN_PIN_RX, settings.canKbps);
    }
    canSetProfile(settings.canProfile);
    canSetMaxxBase(settings.maxxBaseId);
    canSetHaldexIds(settings.haldexBroadcastId, settings.haldexControlId);
  }
  if (klineChanged)
  {
    klineRestart();
  }
  if (wifiChanged)
  {
    wifiRestart();
  }
  writeEEP();
}

static SemaphoreHandle_t settingsApplyMutex = NULL; // 0.38.7: one settings save at a time (web UI, USB console, a setup
                                                    // from another gauge): they share the "before" copy below

// a settings JSON (any part of it) applied as a web UI save would: kept, and every module it touches restarted.
// wifiChanged: the WiFi settings changed (the AP restarts). false = not JSON
static bool settingsApplyJson(const String &body, bool &wifiChanged)
{
  wifiChanged = false;
  JsonDocument doc(psramJsonAllocator());
  if (deserializeJson(doc, body))
  {
    return false;
  }
  if (settingsApplyMutex)
  {
    xSemaphoreTake(settingsApplyMutex, portMAX_DELAY);
  }
  // 0.36.3: the settings as they were, to see what changed. Kept in PSRAM, not on the async_tcp stack: at ~4.9 KB it
  // plus the reply overflowed the 8 KB stack (bench: picking Quad on the ADC source crashed the gauge, coredump).
  // static = one copy for the file; settingsApplyMutex keeps two saves from sharing it at once.
  EXT_RAM_BSS_ATTR static Settings before;
  before = settings;
  settingsFromJson(doc);
  // 0.27.0: units. Metric / Imperial picked: every row follows it. A row that changed turns each page's Min / Max /
  // Redline over into the new unit - unless the same request brought the pages too (a backup import: they're already in
  // its units). Then the quick choice says Custom when the rows are mixed.
  if (before.units != settings.units && settings.units != UNITS_CUSTOM)
  {
    unitsSetQuick(settings.units);
  }
  const UnitChoices unitsBefore = unitsOf(before);
  const UnitChoices unitsAfter = unitsOf(settings);
  const bool unitsChanged = memcmp(&unitsBefore, &unitsAfter, sizeof(UnitChoices)) != 0;
  const bool bodyHasPages = doc["pages"].is<JsonArrayConst>();
  if (unitsChanged && !bodyHasPages)
  {
    unitsConvertPages(unitsBefore);
  }
  settings.units = unitsQuickFromRows();
  // 0.28.0: a DBC slot's base id moved (a relocatable ECU): load the DBCs again with their ids moved to match
  if (memcmp(before.dbcSlotBase, settings.dbcSlotBase, sizeof(settings.dbcSlotBase)) != 0)
  {
    canDbcLoadAll();
  }
  wifiChanged = memcmp(before.apSsid, settings.apSsid, WIFI_LEN) || memcmp(before.apPassword, settings.apPassword, WIFI_LEN) ||
      memcmp(before.staSsid, settings.staSsid, WIFI_LEN) || memcmp(before.staPassword, settings.staPassword, WIFI_LEN) ||
      memcmp(before.hostname, settings.hostname, NAME_LEN);
  // a page holds everything about itself (style, values, sources, slots, sprite), so one compare covers it
  const bool pagesChanged = memcmp(before.pages, settings.pages, sizeof(settings.pages)) || before.units != settings.units ||
      unitsChanged || before.menuMask != settings.menuMask;
  // Custom colours only matter while Custom (or Match car without a VIN) is what's showing
  const bool themeChanged = before.theme != settings.theme ||
      ((before.themeDial != settings.themeDial || before.themeNeedle != settings.themeNeedle) &&
       themeEffective(settings.theme) == THEME_CUSTOM);
  // gearbox / BLE / chain groups: restart just that module (compare the whole v5 block per group)
  const bool dsgChanged = before.dsgSource != settings.dsgSource || before.dsgUdsSession != settings.dsgUdsSession ||
      before.dsgTxId != settings.dsgTxId || before.dsgRxId != settings.dsgRxId ||
      memcmp(&before.dsgSpeed, &settings.dsgSpeed, sizeof(DsgReadSpec)) ||
      memcmp(&before.dsgGear, &settings.dsgGear, sizeof(DsgReadSpec)) ||
      before.dsgTp20Module != settings.dsgTp20Module || before.dsgTp20Group != settings.dsgTp20Group ||
      before.dsgTp20GearGroup != settings.dsgTp20GearGroup ||
      // 0.26.0: the gearbox type, BMW extended addressing and the three newer reads
      before.dsgGearbox != settings.dsgGearbox || before.dsgTxExtAddr != settings.dsgTxExtAddr ||
      before.dsgRxExtAddr != settings.dsgRxExtAddr ||
      memcmp(&before.dsgTemp, &settings.dsgTemp, sizeof(DsgReadSpec)) ||
      memcmp(&before.dsgLockup, &settings.dsgLockup, sizeof(DsgReadSpec)) ||
      memcmp(&before.dsgSelector, &settings.dsgSelector, sizeof(DsgReadSpec));
  const bool elmChanged = before.elmEnabled != settings.elmEnabled || strcmp(before.elmMac, settings.elmMac) != 0 ||
      before.elmAddrType != settings.elmAddrType || before.elmTransport != settings.elmTransport ||
      strcmp(before.elmWifiSsid, settings.elmWifiSsid) != 0 || strcmp(before.elmWifiPass, settings.elmWifiPass) != 0 ||
      strcmp(before.elmWifiHost, settings.elmWifiHost) != 0 || before.elmWifiPort != settings.elmWifiPort ||
      before.elmExtraSource != settings.elmExtraSource || before.elmKlAddr != settings.elmKlAddr ||
      before.elmKlFast != settings.elmKlFast;
  const bool chainChanged = before.chainEnabled != settings.chainEnabled || before.nodeId != settings.nodeId ||
      before.chainGroup != settings.chainGroup || before.linkTransport != settings.linkTransport ||
      before.linkCanBaseId != settings.linkCanBaseId;

  if (dsgChanged)
  {
    dsgRestart();
  }
  // VAG diag: any change to its block (module, protocol, Turbo, the value list) opens the session again
  const bool vdiagChanged = before.vdiagRun != settings.vdiagRun || before.vdiagAddr != settings.vdiagAddr ||
      before.vdiagProtocol != settings.vdiagProtocol || before.vdiagTurbo != settings.vdiagTurbo ||
      before.vdiagGroupSize != settings.vdiagGroupSize || before.vdiagSession != settings.vdiagSession ||
      before.vdiagTp20Module != settings.vdiagTp20Module || before.vdiagTxId != settings.vdiagTxId ||
      before.vdiagRxId != settings.vdiagRxId || before.vdiagPollMs != settings.vdiagPollMs ||
      before.vdiagLink != settings.vdiagLink || before.vdiagTxExt != settings.vdiagTxExt ||
      before.vdiagRxExt != settings.vdiagRxExt ||
      memcmp(before.vdiagValues, settings.vdiagValues, sizeof(settings.vdiagValues)) != 0;
  if (vdiagChanged)
  {
    vdiagRestart();
  }
  if (elmChanged)
  {
    elmRestart();
  }
  if (before.navMode != settings.navMode) // 0.30.0: nav link switched on / off (the other nav settings apply as is)
  {
    navRestart();
  }
  if (chainChanged)
  {
    linkRestart();
  }
  const bool canChanged = before.canEnabled != settings.canEnabled || before.canKbps != settings.canKbps || before.canProfile != settings.canProfile ||
      before.maxxBaseId != settings.maxxBaseId ||
      before.haldexBroadcastId != settings.haldexBroadcastId || before.haldexControlId != settings.haldexControlId;
  const bool klineChanged = before.klineProtocol != settings.klineProtocol || before.klineEcuAddr != settings.klineEcuAddr ||
      before.klineBaud != settings.klineBaud || memcmp(before.me7EcuFile, settings.me7EcuFile, FILE_LEN) ||
      memcmp(before.me7CfgFile, settings.me7CfgFile, FILE_LEN) || before.me7SamplesPerSecond != settings.me7SamplesPerSecond ||
      before.me7FastMode != settings.me7FastMode || // the ME7 session reconnects to install / drop the fast path
      // an open cluster session picks up a new address / block layout by reconnecting
      ((runtime.oil.connected || runtime.oil.connecting) &&
       (before.oilClusterAddr != settings.oilClusterAddr || before.oilGroup != settings.oilGroup ||
        before.oilTempField != settings.oilTempField || before.oilLevelField != settings.oilLevelField));
  // a VAG diag value renamed / re-united on its tab: the gauge pages showing it take the new name (userSigApply)
  applyRuntime(wifiChanged, pagesChanged || vdiagChanged, themeChanged, canChanged, klineChanged);
  if (settingsApplyMutex)
  {
    xSemaphoreGive(settingsApplyMutex);
  }
  return true;
}

// POST /api/settings - any part of the settings JSON
static void settingsIncoming(AsyncWebServerRequest *request, const String &body)
{
  bool wifiChanged = false;
  if (!settingsApplyJson(body, wifiChanged))
  {
    JsonDocument errorDoc(psramJsonAllocator());
    errorDoc["error"] = "bad json";
    sendJSON(request, 400, errorDoc);
    return;
  }
  if (wifiChanged)
  {
    sendOk(request, "ok - wifi restarting");
  }
  else
  {
    sendOk(request, "ok");
  }
}

bool apiSettingsApply(const String &body)
{
  bool wifiChanged = false;
  return settingsApplyJson(body, wifiChanged);
}

static void chainSetupIncoming(AsyncWebServerRequest *request, const String &body); // below

// POST /api/chain/setup: the request itself needs nothing - the reply comes from chainSetupIncoming once the body is in
static void chainSetupRequest(AsyncWebServerRequest *request)
{
  (void)request;
}

// POST /api/chain/setup: the body, a piece at a time, gathered by parseJSON and then handed to chainSetupIncoming
static void chainSetupBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
  parseJSON(request, data, len, index, total, chainSetupIncoming);
}

// POST /api/chain/setup - 0.38.7: First Boot's "Set Up the Other Gauges the Same". The body is what the other gauges
// get (the web UI leaves out what each gauge keeps its own); saved, then sent to each one over ESP-NOW. This gauge's
// own start page goes by its place in the row, as theirs do.
static void chainSetupIncoming(AsyncWebServerRequest *request, const String &body)
{
  JsonDocument doc(psramJsonAllocator());
  bool written = false;
  File file = LittleFS.open(CHAIN_SETUP_FILE, "w");
  if (file)
  {
    written = file.print(body) == body.length();
    file.close();
  }
  if (!written)
  {
    doc["error"] = "couldn't save the setup (is the gauge's storage full?)";
    sendJSON(request, 507, doc);
    return;
  }
  chainSetupStartPage();
  String reason;
  if (!chainSetupStart(reason))
  {
    doc["error"] = reason;
    sendJSON(request, 409, doc);
    return;
  }
  doc["status"] = reason;
  sendJSON(request, 200, doc);
}

// GET /api/kline/block - K-line mode: the module session and the block showing. Polling it keeps the session open (the
// web UI's live measuring blocks), the same as the K-line screen on the glass.
static void klineBlockOutgoing(AsyncWebServerRequest *request)
{
  runtime.klineModeSeenMs = millis() | 1;
  const KlineBrowse &browse = runtime.klineBrowse;
  JsonDocument doc(psramJsonAllocator());
  doc["addr"] = settings.klineModeAddr;
  doc["state"] = browse.state;
  doc["connecting"] = browse.connecting;
  doc["connected"] = browse.connected;
  doc["searching"] = browse.searching;
  doc["ecuId"] = browse.ecuId;
  doc["group"] = browse.group;
  doc["reads"] = browse.reads;
  doc["errors"] = browse.errors;
  doc["ageMs"] = browse.blockMs ? millis() - browse.blockMs : 0;
  doc["labelFile"] = browse.labelFile;
  doc["title"] = browse.title;
  doc["text"] = browse.text; // 0.17.4: a text block (ME7 81 = VIN) instead of fields, "" = none
  doc["obd"] = browse.isObd;  // 0.39.6: the engine didn't answer at 0x01: OBD-II at 0x33, "group" is a page of PIDs
  doc["pages"] = browse.obdPageCount;
  JsonArray labelCandidatesArr = doc["labelCands"].to<JsonArray>();
  for (uint8_t i = 0; i < browse.labelCandidateCount && i < LBL_MAX_CANDS; i++)
  {
    labelCandidatesArr.add(browse.labelCandidates[i]);
  }
  JsonArray fieldsArr = doc["fields"].to<JsonArray>();
  for (uint8_t i = 0; i < browse.fieldCount && i < 4 && !browse.searching; i++)
  {
    JsonObject fieldObj = fieldsArr.add<JsonObject>();
    fieldObj["formula"] = browse.formula[i];
    fieldObj["a"] = browse.a[i];
    fieldObj["b"] = browse.b[i];
    if (isnan(browse.value[i]))
    {
      fieldObj["value"] = "--"; // OBD-II: the ECU didn't answer that PID ("nan" would break the JSON)
    }
    else
    {
      fieldObj["value"] = serialized(String(browse.value[i], 2));
    }
    fieldObj["unit"] = browse.unit[i];
    fieldObj["name"] = browse.fieldName[i];
    fieldObj["spec"] = browse.fieldSpec[i];
  }
  sendJSON(request, 200, doc);
}

// GET /api/adc - the adc2can boards and their 16 channels: status, connector volts, the value the gauge shows
static void adcOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  doc["baseId"] = settings.adcBaseId;
  JsonArray boardsArr = doc["boards"].to<JsonArray>();
  JsonArray channelsArr = doc["ch"].to<JsonArray>();
  for (uint8_t boardIndex = 0; boardIndex < ADC2CAN_BOARDS; boardIndex++)
  {
    const Adc2canBoard &board = adc2canBoard(boardIndex);
    JsonObject boardObj = boardsArr.add<JsonObject>();
    boardObj["seen"] = board.seen;
    boardObj["valid"] = board.validMask;
    boardObj["railOn"] = board.railOnMask;
    boardObj["railFault"] = board.railFaultMask;
    boardObj["range"] = board.rangeMask;
    boardObj["flags"] = board.flags;
    boardObj["busV"] = serialized(String(board.busV, 2));
    boardObj["vbat"] = serialized(String(board.vbat, 2));
    // 0.8.0: how it's heard - "can", "wifi" (ESP-NOW, adc2can 1.3+) or "both" (CAN is used)
    const uint32_t nowMs = millis();
    const bool onCan = board.lastCanMs && nowMs - board.lastCanMs < 2000;
    const bool onAir = board.lastNowMs && nowMs - board.lastNowMs < 2000;
    if (onCan && onAir)
    {
      boardObj["via"] = "both";
    }
    else if (onCan)
    {
      boardObj["via"] = "can";
    }
    else if (onAir)
    {
      boardObj["via"] = "wifi";
    }
    else
    {
      boardObj["via"] = "";
    }
    if (onAir)
    {
      boardObj["rssi"] = board.nowRssi;
    }
    boardObj["shares"] = board.seen && board.info; // adc2can 1.1+: its settings come across (editable here over CAN only)
    if (board.info)
    {
      char fw[12];
      snprintf(fw, sizeof(fw), "%u.%u", board.fwMajor, board.fwMinor);
      boardObj["fw"] = fw;
      boardObj["rev"] = board.rev;
    }
    for (uint8_t channel = 0; channel < 8; channel++)
    {
      const signalId_t id = (signalId_t)(SIG_ADC_1 + boardIndex * 8 + channel);
      JsonObject channelObj = channelsArr.add<JsonObject>();
      channelObj["name"] = signalName(id);
      channelObj["unit"] = signalUnit(id);
      channelObj["sentName"] = board.name[channel];
      channelObj["sentUnit"] = board.unit[channel];
      if (!isnan(board.pinV[channel]) && board.seen)
      {
        channelObj["v"] = serialized(String(board.pinV[channel], 3));
      }
      if (signalValid(id))
      {
        channelObj["value"] = serialized(String(signalGet(id), 3));
      }
      channelObj["railOn"] = (board.railOnMask >> channel) & 1;
      channelObj["fault"] = (board.railFaultMask >> channel) & 1;
      channelObj["range"] = (board.rangeMask >> channel) & 1;
      if (adc2canConfigComplete(boardIndex, channel)) // the adc2can's own settings for this channel
      {
        const AdcRemoteCfg &remoteConfig = board.cfg[channel];
        JsonObject configObj = channelObj["cfg"].to<JsonObject>();
        configObj["mode"] = remoteConfig.mode;
        configObj["decimals"] = remoteConfig.decimals;
        configObj["railOn"] = (remoteConfig.flags & 1) != 0;
        configObj["ratiometric"] = (remoteConfig.flags & 2) != 0;
        configObj["smoothing"] = remoteConfig.smoothing;
        configObj["scale"] = remoteConfig.scale;
        configObj["offset"] = remoteConfig.offset;
        if (remoteConfig.lowWarn > -1e8f)
        {
          configObj["lowWarn"] = remoteConfig.lowWarn;
        }
        if (remoteConfig.highWarn < 1e8f)
        {
          configObj["highWarn"] = remoteConfig.highWarn;
        }
        configObj["trim"] = remoteConfig.trim;
        configObj["ntcR25"] = remoteConfig.ntcR25;
        configObj["ntcBeta"] = remoteConfig.ntcBeta;
        configObj["ntcPullup"] = remoteConfig.ntcPullup;
        JsonArray tableArr = configObj["table"].to<JsonArray>();
        for (uint8_t pointIndex = 0; pointIndex < 8 && !isnan(remoteConfig.tblV[pointIndex]) && !isnan(remoteConfig.tblVal[pointIndex]);
             pointIndex++)
        {
          JsonArray pointArr = tableArr.add<JsonArray>();
          pointArr.add(remoteConfig.tblV[pointIndex]);
          pointArr.add(remoteConfig.tblVal[pointIndex]);
        }
        configObj["name"] = remoteConfig.name;
        configObj["unit"] = remoteConfig.unit;
        // 0.36.4: the dial range the adc2can uses for this sensor (adc2can 1.4+; left out for an older one). A gauge page
        // picking this sensor takes it as it is.
        if (adc2canHasDialRange(boardIndex, channel))
        {
          configObj["dialLo"] = remoteConfig.dialLo;
          configObj["dialHi"] = remoteConfig.dialHi;
          configObj["dialMinSet"] = (remoteConfig.dialByHand & 1) != 0; // false = Auto
          configObj["dialMaxSet"] = (remoteConfig.dialByHand & 2) != 0;
        }
      }
    }
  }
  sendJSON(request, 200, doc);
}

// POST /api/adc/set {"board", "ch", and any of: mode, decimals, railOn, ratiometric, smoothing, scale, offset, lowWarn,
// highWarn, trim, ntcR25, ntcBeta, ntcPullup, table [[volts, value] ...], name, unit, railReset, dialMin / dialMax
// (0.36.4, null = Auto)} - edits the adc2can's own
// settings over CAN (it applies, saves and echoes them back; the ADC tab shows the echo)
// the document has this key at all, even with a null value (JsonDocument::containsKey is deprecated in ArduinoJson 7)
static bool hasKey(const JsonDocument &doc, const char *key)
{
  for (JsonPairConst pair : doc.as<JsonObjectConst>())
  {
    if (!strcmp(pair.key().c_str(), key))
    {
      return true;
    }
  }
  return false;
}

static void adcSetIncoming(AsyncWebServerRequest *request, const String &body)
{
  JsonDocument doc(psramJsonAllocator());
  // 0.36.6: a board this gauge takes (0 .. ADC2CAN_BOARDS - 1), not just 0 / 1
  if (deserializeJson(doc, body) || !doc["board"].is<int>() || !doc["ch"].is<int>() || doc["board"].as<int>() < 0 ||
      doc["board"].as<int>() >= ADC2CAN_BOARDS)
  {
    JsonDocument errorDoc(psramJsonAllocator());
    errorDoc["error"] = "bad json";
    return sendJSON(request, 400, errorDoc);
  }
  const uint8_t boardIndex = (uint8_t)doc["board"].as<int>();
  const uint8_t channel = doc["ch"].as<int>() & 7;
  const Adc2canBoard &board = adc2canBoard(boardIndex);
  if (!board.seen || !board.info)
  {
    return sendOk(request, "the adc2can isn't sharing its settings (not heard, or firmware before 1.1)");
  }
  const AdcRemoteCfg &remoteConfig = board.cfg[channel];
  if (doc["railReset"].is<bool>() && doc["railReset"].as<bool>())
  {
    adc2canRailReset(boardIndex, channel);
  }
  if (doc["mode"].is<int>() || doc["decimals"].is<int>() || doc["railOn"].is<bool>() || doc["ratiometric"].is<bool>() ||
      doc["smoothing"].is<int>())
  {
    const uint8_t flags = ((doc["railOn"].is<bool>() ? doc["railOn"].as<bool>() : (remoteConfig.flags & 1)) ? 1 : 0) |
        ((doc["ratiometric"].is<bool>() ? doc["ratiometric"].as<bool>() : ((remoteConfig.flags >> 1) & 1)) ? 2 : 0);
    const uint8_t partBytes[6] = {(uint8_t)(doc["mode"] | (int)remoteConfig.mode),
                                  (uint8_t)(doc["decimals"] | (int)remoteConfig.decimals), flags,
                                  (uint8_t)(doc["smoothing"] | (int)remoteConfig.smoothing), 0, 0};
    adc2canSetPart(boardIndex, channel, 0, partBytes);
  }
  static const char *floats[] = {NULL, "scale", "offset", "lowWarn", "highWarn", "trim", "ntcR25", "ntcBeta", "ntcPullup"};
  for (uint8_t part = 1; part <= 8; part++)
  {
    if (doc[floats[part]].is<float>() || doc[floats[part]].is<int>())
    {
      adc2canSetFloat(boardIndex, channel, part, doc[floats[part]].as<float>());
    }
    else if (doc[floats[part]].isNull() && hasKey(doc, floats[part])) // null = no warn limit
    {
      adc2canSetFloat(boardIndex, channel, part, NAN);
    }
  }
  if (doc["table"].is<JsonArrayConst>())
  {
    JsonArrayConst tableArr = doc["table"];
    for (uint8_t pointIndex = 0; pointIndex < 8; pointIndex++)
    {
      const bool hasPoint = pointIndex < tableArr.size();
      adc2canSetFloat(boardIndex, channel, 9 + pointIndex, hasPoint ? tableArr[pointIndex][0].as<float>() : NAN);
      adc2canSetFloat(boardIndex, channel, 17 + pointIndex, hasPoint ? tableArr[pointIndex][1].as<float>() : NAN);
    }
  }
  if (doc["name"].is<const char *>())
  {
    adc2canSetText(boardIndex, channel, false, doc["name"]);
  }
  if (doc["unit"].is<const char *>())
  {
    adc2canSetText(boardIndex, channel, true, doc["unit"]);
  }
  // 0.36.4: Dial Min / Max (parts 30 / 31): a number, or null for Auto. Only to an adc2can that has them (1.4+: 32 parts)
  if (board.parts >= 32)
  {
    static const char *dialKeys[] = {"dialMin", "dialMax"};
    for (uint8_t which = 0; which < 2; which++)
    {
      if (doc[dialKeys[which]].is<float>() || doc[dialKeys[which]].is<int>())
      {
        adc2canSetFloat(boardIndex, channel, 30 + which, doc[dialKeys[which]].as<float>());
      }
      else if (doc[dialKeys[which]].isNull() && hasKey(doc, dialKeys[which])) // null = back to Auto
      {
        adc2canSetFloat(boardIndex, channel, 30 + which, NAN);
      }
    }
  }
  sendOk(request, "sent to the adc2can");
}

// GET /api/can/ids - every id heard in the last 5 s: [id, dlc, rate, "data hex", DBC message name or null], plus the DBC
static void canIdsOutgoing(AsyncWebServerRequest *request)
{
  static CanMonEntry *live = (CanMonEntry *)psramCalloc(CANMON_MAX_IDS, sizeof(CanMonEntry)); // PSRAM, taken the first time (4.5 KB off internal RAM)
  const uint16_t idCount = canMonitorSnapshot(live, CANMON_MAX_IDS, 5000);
  const CanStats &stats = canStats();
  JsonDocument doc(psramJsonAllocator());
  doc["enabled"] = settings.canEnabled;
  doc["up"] = stats.up;
  doc["rate"] = stats.rxPerSec;
  doc["kbps"] = settings.canKbps;
  canDbcLock();
  JsonObject dbcObj = doc["dbc"].to<JsonObject>();
  dbcObj["name"] = canDbcName();
  dbcObj["messages"] = canDbcMessageCount();
  dbcObj["signals"] = canDbcSignalCount();
  // 0.28.0: the three DBC slots (car, gearbox, extra) - what each holds, or why it isn't loaded
  JsonArray slotsArr = dbcObj["slots"].to<JsonArray>();
  for (uint8_t slot = 0; slot < CANDBC_SLOTS; slot++)
  {
    const CanDbcSlotInfo &info = canDbcSlot(slot);
    JsonObject slotObj = slotsArr.add<JsonObject>();
    slotObj["loaded"] = info.loaded;
    slotObj["name"] = info.name;
    slotObj["messages"] = info.messages;
    slotObj["keyed"] = info.keyed;             // signals that feed the gauge
    slotObj["bus"] = info.busSpeed;            // GaugeBusSpeed (0 = not given)
    slotObj["reloc"] = info.relocatable;       // its base id can be moved (settings can.dbcBase)
    slotObj["fileBase"] = info.fileBase;       // the base as written in the DBC
    slotObj["error"] = info.error;
  }
  JsonArray idsArr = doc["ids"].to<JsonArray>();
  for (uint16_t i = 0; i < idCount; i++)
  {
    JsonArray idArr = idsArr.add<JsonArray>();
    idArr.add(live[i].id & 0x1FFFFFFF);
    idArr.add(live[i].dlc);
    idArr.add(serialized(String(canMonitorRate(live[i]), 1)));
    char hex[25];
    int hexLen = 0;
    for (uint8_t byteIndex = 0; byteIndex < live[i].dlc; byteIndex++)
    {
      hexLen += snprintf(hex + hexLen, sizeof(hex) - hexLen, byteIndex ? " %02X" : "%02X", live[i].data[byteIndex]);
    }
    hex[hexLen] = '\0';
    idArr.add(hex);
    const CanDbcMessage *message = canDbcLoaded() ? canDbcFind(live[i].id) : NULL;
    if (message)
    {
      idArr.add(message->name);
    }
    else
    {
      idArr.add(nullptr);
    }
    if (live[i].id & 0x80000000UL)
    {
      idArr.add(1); // extended
    }
  }
  canDbcUnlock();
  sendJSON(request, 200, doc);
}

// 0.17.6: POST /api/me7/probe {"hex": "21 F0", "repeats": 20} - USB console only: one hand-made KWP2000 request sent
// inside the running ME7 logger session (me7ProbeQueue in can2gauge_KLine.cpp). The bytes are the service id and data,
// without header or checksum.
// 0.24.1 bench (USB console only): POST /api/kline/listen {"ms":3000} has the K-line task listen to the line for that
// long (it sends nothing - an OBD dongle drives the line); GET /api/kline/listen gives back
// {"done":true,"count":n,"t":[microseconds from the start ...],"b":[the bytes ...]}
static void klineListenIncoming(const String &body)
{
  JsonDocument in(psramJsonAllocator());
  deserializeJson(in, body);
  uint32_t ms = in["ms"] | 3000;
  if (ms > 20000)
  {
    ms = 20000;
  }
  JsonDocument reply(psramJsonAllocator());
  reply["started"] = klineListenStart(ms);
  sendJSON(NULL, 200, reply);
}

static void klineListenOutgoing()
{
  JsonDocument reply(psramJsonAllocator());
  reply["done"] = klineListenDone();
  const uint16_t count = klineListenCount();
  reply["count"] = count;
  JsonArray times = reply["t"].to<JsonArray>();
  JsonArray values = reply["b"].to<JsonArray>();
  if (klineListenDone())
  {
    for (uint16_t i = 0; i < count; i++)
    {
      uint32_t microsFromStart = 0;
      uint8_t value = 0;
      klineListenByte(i, microsFromStart, value);
      times.add(microsFromStart);
      values.add(value);
    }
  }
  sendJSON(NULL, 200, reply);
}

static void me7ProbeIncoming(AsyncWebServerRequest *request, const String &body)
{
  JsonDocument in(psramJsonAllocator());
  deserializeJson(in, body);
  const char *hexText = in["hex"] | "";
  uint8_t bytes[60];
  uint8_t count = 0;
  const char *position = hexText;
  while (*position && count < sizeof(bytes))
  {
    char *end = NULL;
    const unsigned long value = strtoul(position, &end, 16); // one hex byte, spaces between
    if (end == position)
    {
      break; // nothing more that reads as a number
    }
    bytes[count] = (uint8_t)value;
    count++;
    position = end;
  }
  const uint16_t repeats = in["repeats"] | 1;
  if (!me7ProbeQueue(bytes, count, repeats))
  {
    return sendOk(request, "not queued - the ME7 logger isn't logging (or a probe is already waiting)");
  }
  sendOk(request, "queued");
}

// 0.17.6: GET /api/me7/probe - the last probe's answer and timings
static void me7ProbeOutgoing(AsyncWebServerRequest *request)
{
  const Me7Probe &result = me7ProbeResult();
  JsonDocument doc(psramJsonAllocator());
  doc["pending"] = result.pending;
  doc["done"] = result.done;
  doc["ok"] = result.ok;
  doc["nrc"] = result.nrc;
  String answerHex;
  for (uint8_t i = 0; i < result.answerLength; i++)
  {
    char byteText[4];
    snprintf(byteText, sizeof(byteText), "%02X ", result.answer[i]);
    answerHex += byteText;
  }
  doc["answer"] = answerHex;
  doc["repeats"] = result.repeats;
  doc["answered"] = result.answered;
  doc["sendUs"] = result.sendUs;
  doc["waitMinUs"] = result.waitMinUs;
  doc["waitAvgUs"] = result.waitAvgUs;
  doc["waitMaxUs"] = result.waitMaxUs;
  doc["answerUs"] = result.answerUs;
  doc["cycleAvgUs"] = result.cycleAvgUs;
  sendJSON(request, 200, doc);
}

// 0.19.0: POST /api/me7/fastdiag - USB console only. Queues the one-shot fast-path diagnostic (install + hook the
// Test-link routine, probe 3E / RAM / B7 / BA raw while hooked, un-hook). GET returns the raw steps once done.
static void me7FastDiagIncoming(AsyncWebServerRequest *request, const String &body)
{
  // optional {"addrs":[12345,...]} - install v1 with just these variable addresses (to isolate one var's read);
  // no addrs = the logger's own variables
  uint32_t addrs[8];
  uint8_t count = 0;
  if (body.length() > 0)
  {
    JsonDocument in(psramJsonAllocator());
    deserializeJson(in, body); // a malformed body just leaves addrs empty -> falls back to the logger's variables
    JsonArray arr = in["addrs"].as<JsonArray>();
    for (JsonVariant v : arr)
    {
      if (count < 8)
      {
        addrs[count++] = v.as<uint32_t>();
      }
    }
  }
  if (!me7FastDiagQueue(addrs, count))
  {
    return sendOk(request, "not queued - the ME7 logger isn't logging (or a diagnostic is already waiting)");
  }
  sendOk(request, "queued");
}

// one request/answer as a hex string, e.g. "23 00 E1 B0 04"
static String diagHex(const uint8_t *bytes, uint8_t count)
{
  String text;
  for (uint8_t i = 0; i < count; i++)
  {
    char byteText[4];
    snprintf(byteText, sizeof(byteText), "%02X ", bytes[i]);
    text += byteText;
  }
  return text;
}

// 0.19.0: GET /api/me7/fastdiag - the last diagnostic's raw steps
static void me7FastDiagOutgoing(AsyncWebServerRequest *request)
{
  const Me7FastDiag &result = me7FastDiagResult();
  JsonDocument doc(psramJsonAllocator());
  doc["pending"] = result.pending;
  doc["done"] = result.done;
  doc["installed"] = result.installed;
  doc["installMsg"] = result.installMsg;
  JsonArray steps = doc["steps"].to<JsonArray>();
  for (uint8_t i = 0; i < result.stepCount; i++)
  {
    const Me7DiagStep &step = result.steps[i];
    JsonObject stepObj = steps.add<JsonObject>();
    stepObj["label"] = step.label;
    stepObj["request"] = diagHex(step.request, step.requestLen);
    stepObj["answer"] = diagHex(step.answer, step.answerLen);
    stepObj["answerLen"] = step.answerLen;
    stepObj["checksumOk"] = step.checksumOk;
  }
  sendJSON(request, 200, doc);
}

// 0.19.0: POST /api/me7/fast {"on": true, "test": true} - the ME7 logger's Fast log choice (the K-line tab).
// "on" alone = Fast (installs the v1 RAM routine); "on" + "test" = Test link (installs the v0 test routine, a B7
// should return "C2G!" - prove the chain first); neither = Normal. me7SetFastMode() just sets the mode; the
// install/hook happens at the next me7Connect. Normal / Fast is saved (settings.me7FastMode) so it comes back after
// a restart; Test link is a one-off and leaves the saved choice alone.
// 0.20.0: POST /api/me7/cfg {"vars":["nmot_w","tmot", ...]} - rewrite the chosen .cfg's [LogVariables] with these
// variable names (the checkbox picker in the ME7 logger card). The aliases/addresses come from the .ecu at load, so
// just the names are written. Keeps the sample rate, then reconnects the logger so it picks up the new list.
static void me7CfgIncoming(AsyncWebServerRequest *request, const String &body)
{
  if (settings.me7CfgFile[0] == '\0')
  {
    return sendOk(request, "no .cfg file chosen on the K-line tab");
  }
  JsonDocument in(psramJsonAllocator());
  deserializeJson(in, body);
  JsonArray vars = in["vars"].as<JsonArray>();
  File file = LittleFS.open(settings.me7CfgFile, "w");
  if (!file)
  {
    return sendOk(request, "couldn't open the .cfg to write");
  }
  file.println("[Configuration]");
  file.print("SamplesPerSecond = ");
  file.println(me7SamplesWanted()); // keep the rate the logger is using
  file.println();
  file.println("[LogVariables]");
  uint8_t written = 0;
  for (JsonVariant v : vars)
  {
    const char *name = v.as<const char *>();
    if (name == NULL || name[0] == '\0' || written >= ME7_MAX_VARS)
    {
      continue; // skip blanks and stop at the logger's limit
    }
    file.println(name);
    written++;
  }
  file.close();
  klineRestart(); // reload the logger with the new variable list
  char message[48];
  snprintf(message, sizeof(message), "%u ME7 variables saved - reconnecting", written);
  sendOk(request, message);
}

static void me7FastIncoming(AsyncWebServerRequest *request, const String &body)
{
  JsonDocument in(psramJsonAllocator());
  deserializeJson(in, body);
  const bool on = in["on"] | false;
  const bool testLink = in["test"] | false;
  uint8_t mode = ME7_MODE_NORMAL;
  if (on && testLink)
  {
    mode = ME7_MODE_TEST_LINK; // v0 test routine (B7 -> "C2G!")
  }
  else if (on)
  {
    mode = ME7_MODE_FAST; // v1 fast routine (real logging)
  }
  me7SetFastMode(mode);
  if (mode != ME7_MODE_TEST_LINK) // Test link is never saved
  {
    if (mode == ME7_MODE_FAST)
    {
      settings.me7FastMode = 1; // Fast comes back after a restart
    }
    else
    {
      settings.me7FastMode = 0; // Normal logging after a restart
    }
    settingsDirty = true; // the EEP task saves it
  }
  if (mode == ME7_MODE_TEST_LINK)
  {
    sendOk(request, "Test link armed - restart logging, the ECU should answer C2G!");
  }
  else if (mode == ME7_MODE_FAST)
  {
    sendOk(request, "Fast logging saved - restart logging to install it");
  }
  else
  {
    sendOk(request, "Normal logging saved");
  }
}

// GET /api/can/recent - the last 16 frames heard, newest first (the CAN tab's Recent frames)
static void canRecentOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  JsonArray framesArr = doc["frames"].to<JsonArray>();
  CanFrameLog log[16];
  uint8_t frameCount = canRecentFrames(log, 16);
  for (uint8_t i = 0; i < frameCount; i++)
  {
    JsonObject frameObj = framesArr.add<JsonObject>();
    char id[12];   // up to 8 hex digits (a 29-bit id) and the end
    char data[32]; // 8 bytes as "XX " and the end
    snprintf(id, sizeof(id), "%03lX", (unsigned long)log[i].id);
    data[0] = '\0'; // a frame with no data bytes shows as empty
    size_t dataLen = 0;
    uint8_t byteCount = log[i].len;
    if (byteCount > 8)
    {
      byteCount = 8; // a classic CAN frame carries 8 at most (the length code can say up to 15)
    }
    for (uint8_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
      appendf(data, sizeof(data), dataLen, "%02X ", log[i].data[byteIndex]);
    }
    frameObj["id"] = id;
    frameObj["data"] = data;
    frameObj["age"] = millis() - log[i].ms;
    if (log[i].air)
    {
      frameObj["air"] = 1; // relayed over WiFi
    }
  }
  sendJSON(request, 200, doc);
}

// GET /api/can/dbc?slot=1-3 - a DBC slot's .c2d file as it is on the gauge (no slot = slot 1), for the page editor's
// signal lists. 0.28.0: was a lambda that only ever sent /candbc.c2d
static void canDbcFileOutgoing(AsyncWebServerRequest *request)
{
  uint8_t slot = 0;
  if (request->hasArg("slot"))
  {
    const long slotNumber = request->arg("slot").toInt();
    if (slotNumber >= 1 && slotNumber <= CANDBC_SLOTS)
    {
      slot = (uint8_t)(slotNumber - 1);
    }
  }
  const char *path = canDbcSlotPath(slot);
  if (!LittleFS.exists(path))
  {
    request->send(404, "text/plain", "no DBC in that slot");
    return;
  }
  request->send(LittleFS, path, "text/plain");
}

// POST /api/can/dbc?slot=1-3 - body = a .c2d file (text/plain). Saved as that slot's file (0.28.0: slot 1 = /candbc.c2d,
// as before, when no slot is given) and every slot loaded again; DELETE-style clear below.
static File dbcUpload;
static bool dbcUploadTooBig = false;
static uint8_t dbcUploadSlot = 0; // 0.28.0: the slot the upload goes to (0-2)
static void canDbcBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
  if (index == 0)
  {
    dbcUploadSlot = 0;
    if (request && request->hasArg("slot"))
    {
      const long slotNumber = request->arg("slot").toInt(); // 1-3 as the web UI counts them
      if (slotNumber >= 1 && slotNumber <= CANDBC_SLOTS)
      {
        dbcUploadSlot = (uint8_t)(slotNumber - 1);
      }
    }
    dbcUploadTooBig = total > 1500000;
    if (dbcUpload)
    {
      dbcUpload.close();
    }
    if (!dbcUploadTooBig)
    {
      dbcUpload = LittleFS.open("/candbc.tmp", "w");
    }
  }
  if (dbcUpload && len)
  {
    dbcUpload.write(data, len);
  }
  if (index + len == total && dbcUpload)
  {
    dbcUpload.close();
  }
}

static void canDbcDone(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  char error[48] = "";
  if (dbcUploadTooBig)
  {
    copyStr(error, sizeof(error), "too big (1.5 MB max)");
  }
  else if (!canDbcCheckFile("/candbc.tmp", error, sizeof(error)))
  {
    // error filled
  }
  else
  {
    const char *slotPath = canDbcSlotPath(dbcUploadSlot);
    LittleFS.remove(slotPath);
    LittleFS.rename("/candbc.tmp", slotPath);
    canDbcLoadAll(); // 0.28.0: every slot again, merged (an id another slot has leaves this one out - see its error)
  }
  LittleFS.remove("/candbc.tmp");
  if (error[0])
  {
    doc["error"] = error;
    sendJSON(request, 400, doc);
    return;
  }
  const CanDbcSlotInfo &slot = canDbcSlot(dbcUploadSlot);
  if (slot.error[0])
  {
    doc["error"] = slot.error; // read fine, but not loaded (an id clash with another slot)
    sendJSON(request, 400, doc);
    return;
  }
  doc["status"] = "ok";
  doc["slot"] = dbcUploadSlot + 1;
  doc["name"] = slot.name;
  doc["messages"] = slot.messages;
  doc["signals"] = slot.signals;
  doc["keyed"] = slot.keyed; // signals that feed the gauge
  sendJSON(request, 200, doc);
}

// POST /api/action {"action": "..."} - one-shot commands
static void actionIncoming(AsyncWebServerRequest *request, const String &body)
{
  JsonDocument doc(psramJsonAllocator());
  if (deserializeJson(doc, body) || !doc["action"].is<const char *>())
  {
    JsonDocument errorDoc(psramJsonAllocator());
    errorDoc["error"] = "bad json";
    sendJSON(request, 400, errorDoc);
    return;
  }
  const char *action = doc["action"];
  if (!strcmp(action, "reboot"))
  {
    sendOk(request, "rebooting");
    if (settingsDirty)
    {
      writeEEP(); // 0.39.7: a change not saved yet (the house task saves every 2 s) isn't lost - the glass's Reboot did this
    }
    delay(200);
    ESP.restart();
  }
  else if (!strcmp(action, "factoryReset"))
  {
    setDefaults(settings);
    sharedSettingsRev = 0; // like a new gauge: it takes the chain's shared settings rather than pushing its defaults
    writeEEP();
    // 0.39.7: like a new gauge, it knows no car: My Car (kept apart from the settings) and the last car scan go too.
    // Before, a car saved earlier (a BMW tried on the bench) came back after the reset and won over the VIN read.
    vehicleForgetCar();
    LittleFS.remove("/car/values.json");
    firstBootSetState(FIRSTBOOT_ASK); // like a new gauge: the glass asks to set it up after the reboot
    sendOk(request, "defaults restored - rebooting");
    delay(200);
    ESP.restart();
  }
  else if (!strcmp(action, "nextPage"))
  {
    uiNextPage();
    sendOk(request);
  }
  else if (!strcmp(action, "prevPage"))
  {
    uiPrevPage();
    sendOk(request);
  }
  // 0.38.0: Dashboard > Gauge Control's pretend finger (the screens have no touch yet): through the touch input, so the
  // screen showing answers as it will to a real finger. The long press is held half a second past the menu's hold time.
  else if (!strcmp(action, "tap"))
  {
    lvglFakeTouch(LVGL_FAKE_TAP, 0);
    sendOk(request, "tapped the screen");
  }
  else if (!strcmp(action, "doubleTap"))
  {
    lvglFakeTouch(LVGL_FAKE_DOUBLE_TAP, 0);
    sendOk(request, "double-tapped the screen");
  }
  else if (!strcmp(action, "longPress"))
  {
    lvglFakeTouch(LVGL_FAKE_LONG_PRESS, settings.longPressMs + 500);
    sendOk(request, "held the screen");
  }
  else if (!strcmp(action, "swipeLeft")) // Next Gauge: the next page, or the next menu item
  {
    lvglFakeTouch(LVGL_FAKE_SWIPE_LEFT, 0);
    sendOk(request, "swiped left");
  }
  else if (!strcmp(action, "swipeRight")) // Previous Gauge: the previous page / menu item
  {
    lvglFakeTouch(LVGL_FAKE_SWIPE_RIGHT, 0);
    sendOk(request, "swiped right");
  }
  else if (!strcmp(action, "gotoPage"))
  {
    uiGotoPage(doc["page"] | 0);
    sendOk(request);
  }
  else if (!strcmp(action, "clearAlerts"))
  {
    triggersReset();
    sendOk(request);
  }
  else if (!strcmp(action, "testAlert"))
  {
    uiAlert(doc["text"] | "TEST ALERT", false);
    sendOk(request);
  }
  else if (!strcmp(action, "navTest")) // 0.30.0: Show Test Turn - the nav card on the glass, no phone needed
  {
    navShowTest();
    sendOk(request);
  }
  else if (!strcmp(action, "haldexMode"))
  {
    // over CAN when OpenHaldex is heard there (a chain follower asks the master, the only gauge that sends 0x6A0),
    // otherwise over WiFi
    const bool viaCan = haldexHeardOnCan() || !ohxHeard();
    const bool ok = haldexRequestMode(doc["mode"] | 0);
    const char *text;
    if (!ok)
    {
      text = viaCan ? "CAN down" : "busy - try again";
    }
    else if (!viaCan)
    {
      text = "sent over WiFi";
    }
    else if (linkIsMaster())
    {
      text = "sent";
    }
    else
    {
      text = "sent to the chain master";
    }
    sendOk(request, text);
  }
  else if (!strcmp(action, "haldexCmd")) // OpenHaldex over WiFi: {"cmd": ohxCmd_t, "arg": n}
  {
    const int cmd = doc["cmd"] | 0;
    if (cmd < OHX_C_MODE || cmd > OHX_C_CONTROLLER)
    {
      return sendOk(request, "unknown command");
    }
    if (!ohxHeard())
    {
      return sendOk(request, settings.ohxWireless ? "OpenHaldex not heard over WiFi" : "OpenHaldex over WiFi is off");
    }
    sendOk(request, ohxCommand((uint8_t)cmd, (uint8_t)(doc["arg"] | 0)) ? "sent" : "busy - the last command is still waiting");
  }
  else if (!strcmp(action, "haldexFollow")) // {"mac": "AA:BB:CC:DD:EE:FF"} follow that OpenHaldex, "" = the next one heard
  {
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    const char *macText = doc["mac"] | "";
    unsigned octets[6];
    if (*macText &&
        sscanf(macText, "%x:%x:%x:%x:%x:%x", &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5]) == 6)
    {
      for (int i = 0; i < 6; i++)
      {
        mac[i] = (uint8_t)octets[i];
      }
    }
    ohxFollow(mac);
    sendOk(request, *macText ? "following that OpenHaldex" : "forgotten - the next OpenHaldex heard is followed");
  }
  else if (!strcmp(action, "ecuFollow")) // {"mac": "AA:BB:CC:DD:EE:FF"} follow that usb2ip, "" = the next one heard
  {
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    const char *macText = doc["mac"] | "";
    unsigned octets[6];
    int found = sscanf(macText, "%x:%x:%x:%x:%x:%x", &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5]);
    if (found == 6)
    {
      for (uint8_t i = 0; i < 6; i++)
      {
        mac[i] = (uint8_t)octets[i];
      }
    }
    ecuNowFollow(mac);
    if (found == 6)
    {
      sendOk(request, "following that usb2ip");
    }
    else
    {
      sendOk(request, "forgotten - the next usb2ip heard is followed");
    }
  }
  else if (!strcmp(action, "dsgRestart"))
  {
    dsgRestart();
    sendOk(request, "gearbox session restarted");
  }
  else if (!strcmp(action, "vehicleConfirm"))
  { // 0.35.0: {"action":"vehicleConfirm","vin":"...","makeKey":"vw","make":"Volkswagen","modelCode":"AD","model":"Tiguan II",
    //  "year":2019,"body":"suv","paint":13111342} - the driver's car, saved
    CarChoice car;
    memset(&car, 0, sizeof(car));
    copyStr(car.vin, sizeof(car.vin), doc["vin"] | "");
    copyStr(car.makeKey, sizeof(car.makeKey), doc["makeKey"] | "");
    copyStr(car.make, sizeof(car.make), doc["make"] | "");
    copyStr(car.modelCode, sizeof(car.modelCode), doc["modelCode"] | "");
    copyStr(car.model, sizeof(car.model), doc["model"] | "");
    copyStr(car.body, sizeof(car.body), doc["body"] | "");
    car.year = doc["year"] | 0;
    car.paint = doc["paint"] | 0x8A96A3; // the pictures' grey when none was picked
    vehicleSetCar(car);
    sendOk(request, "car saved");
  }
  else if (!strcmp(action, "vehicleForget"))
  {
    vehicleForgetCar();
    sendOk(request, "car forgotten");
  }
  else if (!strcmp(action, "vehicleDetect"))
  { // 0.35.0: read the VIN over the gauge's CAN once (the adapter and the K-line read theirs on their own)
    vehicleRequestCanRead();
    sendOk(request, "reading");
  }
  else if (!strcmp(action, "vdiagRestart"))
  {
    vdiagRestart();
    sendOk(request, "VAG diag session restarted");
  }
  else if (!strcmp(action, "vdiagProbe"))
  { // {"action":"vdiagProbe","did":61854} - one 22 <did> read on the open UDS session
    int did = doc["did"] | -1;
    if (did < 0 || did > 0xFFFF)
    {
      sendOk(request, "no DID given");
    }
    else
    {
      vdiagProbe((uint16_t)did);
      sendOk(request, "asking the module");
    }
  }
  else if (!strcmp(action, "logStart")) // 0.16.0 data logger: a new recording (the old one is dropped)
  {
    if (loggerStart())
    {
      sendOk(request, "recording");
    }
    else
    {
      LoggerStatus logStatus;
      loggerStatusCopy(logStatus);
      sendOk(request, logStatus.error);
    }
  }
  else if (!strcmp(action, "logStop"))
  {
    loggerStop();
    sendOk(request, "stopped - the recording stays until the next Start");
  }
  else if (!strcmp(action, "logClear"))
  {
    loggerClear();
    sendOk(request, "recording cleared");
  }
  else if (!strcmp(action, "otherScanners"))
  { // 0.37.0: {"action":"otherScanners","on":1} - Enable Other Scanners (not saved: off again after a restart)
    runtime.otherScanners = (doc["on"] | 0) != 0;
    if (runtime.otherScanners)
    {
      carScanStop(); // a scan running now stops
    }
    vdiagRestart(); // the diagnostics session closes (or starts again)
    sendOk(request, "ok");
  }
  else if (!strcmp(action, "serviceStart"))
  { // 0.16.0: {"action":"serviceStart","job":1,"via":0,"addr":1,"group":60} - see can2gauge_Service.h
    const uint8_t job = doc["job"] | 0;
    const uint8_t via = doc["via"] | 0;
    const uint8_t address = doc["addr"] | 1;
    const uint8_t group = doc["group"] | 0;
    char why[64];
    if (serviceStart(job, via, address, group, why, sizeof(why)))
    {
      sendOk(request, "started");
    }
    else
    {
      sendOk(request, why);
    }
  }
  else if (!strcmp(action, "serviceStop"))
  {
    serviceStop();
    sendOk(request, "stopping");
  }
  else if (!strcmp(action, "output"))
  { // {"action":"output","channel":n,"state":0|1|2}
    int channel = doc["channel"] | 0;
    int state = doc["state"] | 2;
    if (state < 0 || state > 2)
    {
      state = 2; // not a state: auto
    }
    if (channel >= 0 && channel < IOX_CHANNELS)
    {
      settings.outputManual[channel] = state;
      settingsDirty = true;
      outputsApply();
    }
    sendOk(request);
  }
  else if (!strcmp(action, "me7Start"))
  {
    sendOk(request, me7StartLogging()); // 0.17.6: "logging", or why it hasn't started yet
  }
  else if (!strcmp(action, "me7Stop"))
  {
    me7StopLogging();
    sendOk(request);
  }
  else if (!strcmp(action, "klineRestart"))
  {
    klineRestart();
    sendOk(request);
  }
  else if (!strcmp(action, "klStep")) // K-line mode: {"dir": 1 | -1} next / previous measuring block
  {
    runtime.klineBrowse.step = (doc["dir"] | 1) < 0 ? -1 : 1;
    sendOk(request);
  }
  else if (!strcmp(action, "klGoto")) // K-line mode: {"group": 1..255}
  {
    const int group = doc["group"] | 1;
    if (group < 1)
    {
      runtime.klineBrowse.gotoGroup = 1;
    }
    else if (group > 255)
    {
      runtime.klineBrowse.gotoGroup = 255;
    }
    else
    {
      runtime.klineBrowse.gotoGroup = (int16_t)group;
    }
    sendOk(request);
  }
  else if (!strcmp(action, "klLabel")) // K-line mode: {"file": "06A-906-032-AWP"} for the connected module, "" = automatic
  {
    if (!runtime.klineBrowse.connected)
    {
      return sendOk(request, "connect to the module first");
    }
    labelsSetChoice(runtime.klineBrowse.ecuId, runtime.klineBrowse.addr, doc["file"] | "");
    runtime.klineBrowse.labelReload = true;
    sendOk(request, "label file saved");
  }
  else if (!strcmp(action, "bootShow")) // Display > Boot logo > Show now
  {
    if (!settings.bootLogo)
    {
      return sendOk(request, "boot logo is set to None");
    }
    uiShowBootLogo();
    sendOk(request, "showing the boot logo");
  }
  else if (!strcmp(action, "saverShow")) // Settings > Sprites > Show the screensaver now
  {
    if (uiScreenKind(NULL) != UI_SCR_PAGE)
    {
      return sendOk(request, "show a gauge page on the glass first");
    }
    uiSaverShow();
    sendOk(request, uiSaverActive() ? "screensaver on - touch the glass to leave it" : "no sprite pack on the gauge");
  }
  else if (!strcmp(action, "benchmark")) // Diag > Run benchmark: the whole glass redrawn every refresh for a few seconds
  {
    uiBenchmark();
    sendOk(request, "benchmark running - leave the glass alone for 6 s");
  }
  else if (!strcmp(action, "touchRedetect")) // Diag > Display > Look for touch again (after re-seating the flex)
  {
    if (!lvglReady())
    {
      return sendOk(request, "the display isn't running, so touch can't be checked");
    }
    lvglTouchRedetect();
    sendOk(request, "looking for the touch controller - the result shows in a moment");
  }
  else if (!strcmp(action, "panelCommand")) // 0.24.3 bench: {"cmd":35,"data":[...],"bitBang":false} one command to the glass
  {
    uint8_t data[16];
    uint8_t count = 0;
    JsonArray list = doc["data"].as<JsonArray>();
    for (size_t i = 0; i < list.size() && count < sizeof(data); i++)
    {
      data[count] = list[i] | 0;
      count++;
    }
    const bool bitBang = doc["bitBang"] | false;
    displaySendCommand((uint8_t)(doc["cmd"] | 0), data, count, bitBang);
    if (displayBitBang())
    {
      sendOk(request, "sent (by hand on SCK / SDA)");
    }
    else
    {
      sendOk(request, "sent (SPI peripheral)");
    }
  }
  else if (!strcmp(action, "panelReload")) // 0.24.3 bench: {"bitBang":false} reset the glass, send its registers again
  {
    displayReloadRegisters(doc["bitBang"] | false);
    if (displayBitBang())
    {
      sendOk(request, "glass reset and registers sent again (by hand on SCK / SDA)");
    }
    else
    {
      sendOk(request, "glass reset and registers sent again (SPI peripheral)");
    }
  }
  else if (!strcmp(action, "dragClear")) // 0.38.0: the Log tab's Drag Timer card: Clear All for one kind
  {
    dragClear(doc["kind"] | 0);
    sendOk(request, "drag times cleared");
  }
  else if (!strcmp(action, "dragDelete")) // 0.38.0: ... one time off the board (place 0 = the best)
  {
    dragDelete(doc["kind"] | 0, doc["place"] | 0);
    sendOk(request, "drag time deleted");
  }
  else if (!strcmp(action, "runnerReset")) // Settings > Sprites > Rev Runner: reset the high score
  {
    settings.runnerBest = 0;
    settingsDirty = true;
    sendOk(request, "high score reset");
  }
  else if (!strcmp(action, "spritesChanged")) // a pack was uploaded / deleted: pages showing "the first pack" pick it up
  {
    uiRebuildPages();
    sendOk(request, "sprites updated");
  }
  else if (!strcmp(action, "shutdownShow")) // ... play the shutdown and wake sprites
  {
    if (!settings.shutdownSprite[0])
    {
      return sendOk(request, "pick a shutdown sprite first");
    }
    if (linkRowTest()) // in a row of gauges: they all play it together (the row wave)
    {
      char text[160];
      linkRowTestText(text, sizeof(text)); // 0.29.0: says which gauges join, and which can't (no shutdown sprite)
      sendOk(request, text);
      return;
    }
    powerTestShutdown();
    sendOk(request, "playing the shutdown sprite, then the wake sprite");
  }
  else if (!strcmp(action, "chainSetRow")) // 0.29.0: {"node": G number, "row": 0-15} - any gauge's place in the row
  {
    const int nodeId = doc["node"] | -1;
    const int row = doc["row"] | -1;
    if (nodeId < 0 || nodeId > 15 || row < 0 || row > ROW_MAX_POSITION)
    {
      return sendOk(request, "pick a gauge and a number");
    }
    linkSetRow((uint8_t)nodeId, (uint8_t)row);
    if (row == 0)
    {
      sendOk(request, "taken out of the row");
    }
    else
    {
      sendOk(request, "number set - it shows on that gauge");
    }
  }
  else if (!strcmp(action, "chainSetRole")) // 0.29.0: {"node": G number, "role": 0 Auto, 1 Master, 2 Follower}
  {
    const int nodeId = doc["node"] | -1;
    const int role = doc["role"] | -1;
    if (nodeId < 0 || nodeId > 15 || role < 0 || role >= chainRole_t_MAX)
    {
      return sendOk(request, "pick a gauge and a role");
    }
    linkSetRole((uint8_t)nodeId, (uint8_t)role);
    if (role == CHAIN_MASTER)
    {
      sendOk(request, "master set - any other gauge set to Master is back on Auto");
    }
    else
    {
      sendOk(request, "role set");
    }
  }
  else if (!strcmp(action, "chainShowNumbers")) // 0.29.0: every gauge shows its number on its glass
  {
    if (!settings.chainEnabled)
    {
      return sendOk(request, "turn the chain on first");
    }
    linkShowNumbers(true);
    sendOk(request, "every gauge shows its number for 15 s (tap one to hide it)");
  }
  else if (!strcmp(action, "chainHideNumbers")) // ... and hides it again
  {
    linkShowNumbers(false);
    sendOk(request, "numbers hidden");
  }
  else if (!strcmp(action, "chainUpdate")) // Update All Gauges: the newest firmware in the chain to every gauge
  {
    String reason;
    chainUpdateStart(reason);
    sendOk(request, reason.c_str());
  }
  else if (!strcmp(action, "chainUpdateStop")) // ... stop now: the gauge being worked on drops what it has and keeps its old firmware
  {
    chainUpdateStop();
    sendOk(request, "stopping");
  }
  else if (!strcmp(action, "canDbcClear"))
  {
    // 0.28.0: {"slot": 1-3} removes one DBC slot (no slot = slot 1, as before); the other slots stay
    uint8_t slot = 0;
    if (doc["slot"].is<int>() && doc["slot"].as<int>() >= 1 && doc["slot"].as<int>() <= CANDBC_SLOTS)
    {
      slot = doc["slot"].as<int>() - 1;
    }
    canDbcClearSlot(slot);
    sendOk(request, "DBC removed");
  }
  else if (!strcmp(action, "oilShow"))
  { // put the oil page on the glass (with oil source = K-line that is what starts the cluster session)
    const int pageIndex = oilPageIndex();
    if (pageIndex >= 0)
    {
      uiGotoPage(pageIndex);
    }
    sendOk(request, pageIndex >= 0 ? "oil page shown" : "no oil page enabled - add one first");
  }
  else
  {
    JsonDocument errorDoc(psramJsonAllocator());
    errorDoc["error"] = "unknown action";
    sendJSON(request, 400, errorDoc);
  }
}

// ---------------------------------------------------------------------------
// GET /api/log - the data logger (0.16.0): is it recording, its channels, how many samples it holds
// ---------------------------------------------------------------------------
static void logOutgoing(AsyncWebServerRequest *request)
{
  LoggerStatus logStatus;
  loggerStatusCopy(logStatus);
  JsonDocument doc(psramJsonAllocator());
  doc["running"] = logStatus.running;
  doc["paused"] = logStatus.paused;   // recording, but the ignition is off
  doc["session"] = logStatus.session; // changes at every Start / Clear
  doc["rateHz"] = logStatus.rateHz;
  doc["recordBytes"] = logStatus.recordBytes;
  doc["capacity"] = logStatus.capacity; // samples the memory holds
  doc["total"] = logStatus.total;       // samples taken since Start
  uint32_t oldestKept = 0;
  if (logStatus.total > logStatus.capacity)
  {
    oldestKept = logStatus.total - logStatus.capacity; // the older ones have been written over
  }
  doc["oldest"] = oldestKept;
  doc["startMs"] = logStatus.startMs;
  doc["nowMs"] = millis(); // so the web UI can put the phone's clock time on the samples
  doc["bufferKb"] = logStatus.bufferBytes / 1024;
  doc["memoryKb"] = loggerMemoryBytes() / 1024; // 0.17.0: the room for a recording, shown before Start too
  doc["error"] = logStatus.error;
  JsonArray channelsArr = doc["channels"].to<JsonArray>();
  for (uint8_t channel = 0; channel < logStatus.channelCount; channel++)
  {
    JsonObject channelObj = channelsArr.add<JsonObject>();
    channelObj["key"] = signalInfo((signalId_t)logStatus.signals[channel]).key;
    channelObj["name"] = logStatus.names[channel];
    channelObj["unit"] = logStatus.units[channel];
  }
  sendJSON(request, 200, doc);
}

// GET /api/log/data?from=<sample number>&max=<samples> - a piece of the recording, binary (can2gauge_Logger.h:
// a 16-byte header, then per sample the time in ms and a float per channel, all little-endian)
static void logDataOutgoing(AsyncWebServerRequest *request)
{
  const uint32_t from = (uint32_t)request->arg("from").toInt();
  long maxCount = request->arg("max").toInt();
  if (maxCount <= 0)
  {
    maxCount = 2000; // no max given
  }
  AsyncResponseStream *response = request->beginResponseStream("application/octet-stream", LOG_SEND_MAX + 16);
  loggerSend(*response, from, (uint32_t)maxCount);
  request->send(response);
}

// ---------------------------------------------------------------------------
// GET /api/service - the Service tab (0.16.0): the fault list or the basic setting's fields, and what's happening.
// Asking also tells a running basic setting that the tab is still open.
// ---------------------------------------------------------------------------
// 0.38.0: the drag timer's scoreboard, both kinds: {"kinds": [{"runs": 12, "times": [{"ms": 4870, "run": 7}, ...]}, ...]}
// (kind 0 = 0-100 km/h, 1 = 0-60 mph), and which one the screen times now (the speed row's unit)
static void dragOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  uint8_t kindNow = DRAG_KIND_KMH;
  if (settings.unitSpeed == UNIT_IMPERIAL)
  {
    kindNow = DRAG_KIND_MPH;
  }
  doc["now"] = kindNow;
  JsonArray kindsArr = doc["kinds"].to<JsonArray>();
  for (uint8_t kind = 0; kind < DRAG_KINDS; kind++)
  {
    JsonObject kindObj = kindsArr.add<JsonObject>();
    kindObj["runs"] = dragRunCount(kind);
    JsonArray timesArr = kindObj["times"].to<JsonArray>();
    for (uint8_t place = 0; place < dragScoreCount(kind); place++)
    {
      const DragRun run = dragScore(kind, place);
      JsonObject timeObj = timesArr.add<JsonObject>();
      timeObj["ms"] = run.timeMs;
      timeObj["run"] = run.runNumber;
    }
  }
  sendJSON(request, 200, doc);
}

// 0.38.0: app.js zipped. The filesystem image has it twice (tools/fs_pack.py): plain, because the shared OTA module
// checks /app.js is there before it serves the web UI, and zipped, which is what goes to the browser (257 -> 79 kB).
// The library's static files would pick between the two by guessing, so this sends the zipped one for certain, with an
// ETag so a reload gets "not changed" (304) instead of the whole file again.
static void appJsOutgoing(AsyncWebServerRequest *request)
{
  if (!LittleFS.exists("/app.js.gz"))
  {
    request->send(LittleFS, "/app.js", "application/javascript"); // an image without the zipped copy
    return;
  }
  File file = LittleFS.open("/app.js.gz", "r");
  const size_t fileSize = file.size();
  file.close();
  char tag[48];
  snprintf(tag, sizeof(tag), "\"%s-%u\"", CAN2GAUGE_VERSION, (unsigned)fileSize); // the version and size: a new image = a new tag
  if (request->hasHeader("If-None-Match") && request->header("If-None-Match") == tag)
  {
    request->send(304); // the browser's copy is this one
    return;
  }
  AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/app.js.gz", "application/javascript");
  response->addHeader("Content-Encoding", "gzip"); // (the library's response object: the arrow reaches into it)
  response->addHeader("Cache-Control", "no-cache");
  response->addHeader("ETag", tag);
  request->send(response);
}

static void serviceOutgoing(AsyncWebServerRequest *request)
{
  EXT_RAM_BSS_ATTR static ServiceStatus job; // static: too big for the web server task's stack (only this handler uses it)
  serviceStatusCopy(job);
  JsonDocument doc(psramJsonAllocator());
  doc["job"] = job.job;
  doc["via"] = job.via;
  doc["addr"] = job.address;
  doc["group"] = job.group;
  doc["busy"] = job.busy;
  doc["done"] = job.done;
  doc["ok"] = job.ok;
  doc["state"] = job.state;
  doc["protocol"] = job.protocol;
  doc["moduleId"] = job.moduleId;
  JsonArray faultsArr = doc["faults"].to<JsonArray>();
  for (uint8_t i = 0; i < job.faultCount; i++)
  {
    JsonObject faultObj = faultsArr.add<JsonObject>();
    faultObj["code"] = job.faults[i].code;
    faultObj["status"] = job.faults[i].status;
  }
  if (job.job == SVC_JOB_SCAN) // 0.34.0: Scan Modules - how far it got and what answered
  {
    doc["scanDone"] = job.scanDone;
    doc["scanTotal"] = job.scanTotal;
    JsonArray modulesArr = doc["modules"].to<JsonArray>();
    for (uint8_t i = 0; i < job.moduleCount && i < SERVICE_MAX_MODULES; i++)
    {
      const ServiceModule &module = job.modules[i];
      JsonObject moduleObj = modulesArr.add<JsonObject>();
      moduleObj["addr"] = module.address;
      moduleObj["txId"] = module.requestId;
      moduleObj["rxId"] = module.answerId;
      moduleObj["name"] = module.name;
      moduleObj["part"] = module.partNumber;
      moduleObj["component"] = module.component;
      moduleObj["faults"] = module.faults; // -1 = it wouldn't list them
    }
  }
  if (job.job == SVC_JOB_BASIC_SETTING)
  {
    uint32_t seconds = 0;
    if (job.startedMs != 0)
    {
      seconds = (millis() - job.startedMs) / 1000;
    }
    doc["seconds"] = seconds;
    doc["reads"] = job.reads;
    doc["title"] = job.title;
    JsonArray fieldsArr = doc["fields"].to<JsonArray>();
    for (uint8_t field = 0; field < job.fieldCount && field < 4; field++)
    {
      JsonObject fieldObj = fieldsArr.add<JsonObject>();
      fieldObj["name"] = job.fieldName[field];
      fieldObj["value"] = job.value[field];
      fieldObj["unit"] = job.unit[field];
      fieldObj["formula"] = job.formula[field];
      fieldObj["a"] = job.a[field];
      fieldObj["b"] = job.b[field];
    }
  }
  sendJSON(request, 200, doc);
}

// ---------------------------------------------------------------------------
// files: /api/files (list), /api/files/upload (POST multipart), /api/files/delete, /download?path=
// used for ME7 .ecu / .cfg definitions, /logo.bin and to fetch CSV logs
// ---------------------------------------------------------------------------
static void filesOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  JsonArray filesArr = doc["files"].to<JsonArray>();
  const char *dirs[] = {"/", "/me7", "/logs", "/reads", "/logos", "/sprites"};
  for (const char *dirName : dirs)
  {
    File dir = LittleFS.open(dirName);
    if (!dir)
    {
      continue;
    }
    File file = dir.openNextFile();
    while (file)
    {
      if (!file.isDirectory())
      {
        JsonObject fileObj = filesArr.add<JsonObject>();
        String path = String(dirName) + (dirName[strlen(dirName) - 1] == '/' ? "" : "/") + file.name();
        fileObj["path"] = path;
        fileObj["size"] = file.size();
      }
      file = dir.openNextFile();
    }
  }
  size_t total = 0;
  size_t used = 0;
  fsUsage(total, used); // 0.17.4: the house task's numbers, as in spritesOutgoing
  doc["total"] = total;
  doc["used"] = used;
  sendJSON(request, 200, doc);
}

static File uploadFile;          // the upload being written
static String uploadPath;        // where it goes once it's all in
static bool uploadFailed = false; // the file couldn't be opened or a write fell short

// the reply once the upload is in: an error if it couldn't be written (the filesystem full ...)
static void filesUploadDone(AsyncWebServerRequest *request)
{
  if (uploadFailed)
  {
    JsonDocument doc(psramJsonAllocator());
    doc["error"] = "couldn't write the file (is the gauge's storage full?)";
    sendJSON(request, 507, doc);
    return;
  }
  sendOk(request, "uploaded");
}

static void filesUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  if (index == 0)
  {
    String dir;
    if (request->hasParam("dir", true))
    {
      dir = request->getParam("dir", true)->value();
    }
    else if (request->hasParam("dir"))
    {
      dir = request->getParam("dir")->value();
    }
    else
    {
      dir = "/me7";
    }
    if (!dir.startsWith("/"))
    {
      dir = "/" + dir;
    }
    if (dir != "/" && !LittleFS.exists(dir))
    {
      LittleFS.mkdir(dir);
    }
    String path = dir + (dir.endsWith("/") ? "" : "/") + filename;
    uploadPath = path;
    // written beside the old file first, so a failed upload leaves the old one as it was
    uploadFile = LittleFS.open(path + ".part", "w");
    uploadFailed = !uploadFile;
    DEBUG("upload -> %s", path.c_str());
  }
  if (uploadFile && len)
  {
    if (uploadFile.write(data, len) != len)
    {
      uploadFailed = true; // the filesystem is full
    }
  }
  if (final)
  {
    if (uploadFile)
    {
      uploadFile.close();
    }
    if (uploadFailed)
    {
      LittleFS.remove(uploadPath + ".part");
    }
    else
    {
      LittleFS.remove(uploadPath);
      LittleFS.rename(uploadPath + ".part", uploadPath);
    }
    fsUsageChanged(); // the space used changed
  }
}

// a file the gauge needs to run (0.14.0): the web UI itself (index.html, app.js, style.css, version.json ... in the top
// folder), the DBC library, the label pack and the make logos. These come with the web UI image and are never
// deleted from the web UI - only replaced by the next filesystem update.
static bool isProtectedFile(const String &path)
{
  if (path.startsWith("/dbc/") || path.startsWith("/lbl/") || path.startsWith("/logos/") ||
      path.startsWith("/vdid/") || path.startsWith("/cars/")) // 0.34.0: the DID library; 0.35.0: the car pictures
  {
    return true;
  }
  const bool inTopFolder = path.lastIndexOf('/') == 0; // "/app.js" yes, "/me7/x.cfg" no
  if (!inTopFolder)
  {
    return false;
  }
  if (path.endsWith(".html") || path.endsWith(".js") || path.endsWith(".css") || path.endsWith(".json") ||
      path.endsWith(".gz"))
  {
    return true;
  }
  if (path == "/fs_manifest.txt")
  {
    return true; // 0.25.0: the release's file list - a chain update needs it
  }
  return false;
}

// POST /api/files/delete path=... - removes one of the user's files (logs, ME7 files, flash reads, own logo, sprites)
static void filesDelete(AsyncWebServerRequest *request)
{
  if (!request->hasParam("path", true))
  {
    sendOk(request);
    return;
  }
  String path = request->getParam("path", true)->value();
  if (isProtectedFile(path))
  {
    JsonDocument errorDoc(psramJsonAllocator());
    errorDoc["error"] = "that file is part of the web UI - it can't be deleted";
    sendJSON(request, 400, errorDoc);
    return;
  }
  LittleFS.remove(path);
  fsUsageChanged(); // 0.17.4: the space used changed
  sendOk(request);
}

static void cannotFind(AsyncWebServerRequest *request)
{
  if (request->method() == HTTP_OPTIONS)
  {
    request->send(200);
  }
  else
  {
    request->send(404, "text/plain", "not found");
  }
}

// GET /api/me7/vars - the ME7 logger's ECU and its variables with their latest values
static void me7VarsOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  const Me7EcuInfo &ecuInfo = me7EcuInfo();
  doc["state"] = me7StateName();
  doc["partNumber"] = ecuInfo.partNumber;
  doc["engine"] = ecuInfo.engineId;
  doc["sw"] = ecuInfo.swNumber;
  doc["logSpeed"] = ecuInfo.logSpeed;
  doc["sps"] = me7SamplesWanted(); // 0.17.6: the rate aimed for (the web UI's Samples/s, was the .cfg's)
  doc["rate"] = runtime.me7SampleRate; // 0.17.3: samples per second actually reached
  doc["requests"] = me7RequestsPerSample(); // 0.17.6: requests a sample takes (~10 ms each on an ME7.5)
  doc["samples"] = runtime.klineSamples;
  JsonArray varsArr = doc["vars"].to<JsonArray>();
  for (uint8_t i = 0; i < me7VarCount(); i++)
  {
    const Me7Var &variable = me7Var(i);
    JsonObject varObj = varsArr.add<JsonObject>();
    varObj["name"] = variable.name;
    varObj["alias"] = variable.alias;
    varObj["unit"] = variable.unit;
    varObj["addr"] = variable.address;
    varObj["size"] = variable.size;
    varObj["value"] = serialized(String(variable.value, 3));
  }
  sendJSON(request, 200, doc);
}

// ---- ME7 flash read (optional feature; delete these three and their routes to remove the API) ----
// GET /api/me7read/status
static void me7ReadStatusOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  const Me7ReadStatus &status = me7ReadStatusGet();
  doc["busy"] = me7ReadBusy() || me7ReadPending(); // 0.17.4: also while queued (it showed the last read's result)
  doc["state"] = me7ReadStateName(status.state);
  if (me7ReadPending())
  {
    doc["state"] = "queued"; // the K-line task picks it up when the session it's in has closed
  }
  doc["percent"] = status.percent;
  doc["bytesRead"] = status.bytesRead;
  doc["bytesTotal"] = status.bytesTotal;
  doc["baud"] = status.baud;
  doc["elapsedMs"] = status.elapsedMs;
  doc["etaMs"] = status.etaMs;
  doc["ecuId"] = status.ecuId;
  doc["file"] = status.file;
  doc["message"] = status.message;
  doc["lastFile"] = me7ReadLastFile();
  uint32_t dumpBytes = 0;
  me7ReadDump(dumpBytes);
  doc["dumpBytes"] = dumpBytes; // 0.17.4: the last complete read, held in memory (GET /api/me7read/bin), 0 = none
  sendJSON(request, 200, doc);
}

// 0.17.4: hands the web server the next piece of the read held in memory. index = bytes already sent, maxLength =
// room in buffer; returns the bytes put in buffer (0 = all sent).
static size_t me7DumpFiller(uint8_t *buffer, size_t maxLength, size_t index)
{
  uint32_t dumpBytes = 0;
  const uint8_t *dump = me7ReadDump(dumpBytes);
  if (dump == NULL || index >= dumpBytes)
  {
    return 0;
  }
  size_t count = dumpBytes - index;
  if (count > maxLength)
  {
    count = maxLength;
  }
  memcpy(buffer, dump + index, count);
  return count;
}

// GET /api/me7read/bin (0.17.4): the last complete read as a .bin download, straight from memory
static void me7DumpOutgoing(AsyncWebServerRequest *request)
{
  uint32_t dumpBytes = 0;
  me7ReadDump(dumpBytes);
  if (dumpBytes == 0)
  {
    request->send(404, "text/plain", "no ECU read in memory");
    return;
  }
  AsyncWebServerResponse *response = request->beginResponse("application/octet-stream", dumpBytes, me7DumpFiller);
  response->addHeader("Content-Disposition", "attachment; filename=\"me7_read.bin\"");
  request->send(response);
}

// POST /api/me7read/start {fastInit, address, baud, mirrorCheck, verify}
static void me7ReadStartIncoming(AsyncWebServerRequest *request, const String &body)
{
  JsonDocument doc(psramJsonAllocator());
  deserializeJson(doc, body);
  if (me7ReadBusy())
  {
    JsonDocument errorDoc(psramJsonAllocator());
    errorDoc["error"] = "read already running";
    sendJSON(request, 409, errorDoc);
    return;
  }
  Me7ReadParams params;
  params.fastInit = doc["fastInit"] | true;
  params.address = doc["address"] | 0x01;
  params.target = doc["target"] | 0; // 0.17.4: 0 = the same as address
  if (params.target == 0 && params.address == 0x11)
  {
    params.target = 0x10; // 0.17.4: ME7 woken with 0x11 answers as 0x10 (bench ME7.5; ME7Logger's SLOW-0x11 / HM2-0x10)
  }
  params.baud = doc["baud"] | 56000;
  params.mirrorCheck = doc["mirrorCheck"] | true;
  params.verify = doc["verify"] | true;
  // 0.18.0: non-destructive RAM unlock for ECUs that refuse 0x27. The XRAM addresses default to the VR6 022 values
  // (ME7Read.h); override them here if a per-ECU REA shows they differ.
  params.ramUnlock = doc["ramUnlock"] | false;
  params.permitAddr = doc["permitAddr"] | params.permitAddr;
  params.permitValue = doc["permitValue"] | params.permitValue;
  params.grantAddr = doc["grantAddr"] | params.grantAddr;
  params.grantMask = doc["grantMask"] | params.grantMask;
  const bool ok = me7ReadRequest(params);
  klineRestart(); // break the running protocol loop so the K-line task picks up the read now
  if (ok)
  {
    sendOk(request, "read started");
  }
  else
  {
    sendOk(request, "busy");
  }
}

// POST /api/me7read/cancel
static void me7ReadCancelIncoming(AsyncWebServerRequest *request)
{
  me7ReadCancel();
  sendOk(request, "cancelling");
}

// 0.24.20: GET /api/tasks (bench console only) - where the time and the internal RAM go. For each task: its core,
// priority, the share of its core it used since the last ask (in %), and the stack it has never touched. Asked every
// couple of seconds by a script on the PC while the phone uses the web page, it shows which task holds the web server up.
#define TASKS_REPORT_MAX 40 // more than the gauge runs (ours + WiFi / Bluetooth / system)
static uint32_t tasksLastRunTime[TASKS_REPORT_MAX];    // each task's run time counter at the last ask
static TaskHandle_t tasksLastHandle[TASKS_REPORT_MAX]; // which task each counter belongs to
static uint32_t tasksLastTotal = 0;                    // the clock (microseconds) at the last ask
static void tasksOutgoing(AsyncWebServerRequest *request)
{
  // the list is ~2 KB, so it's borrowed from PSRAM for the moment rather than taken from the stack
  TaskStatus_t *taskList = (TaskStatus_t *)psramCalloc(TASKS_REPORT_MAX, sizeof(TaskStatus_t));
  if (!taskList)
  {
    JsonDocument error(psramJsonAllocator());
    error["error"] = "no memory";
    sendJSON(request, 500, error);
    return;
  }
  uint32_t totalNow = 0;
  const UBaseType_t taskCount = uxTaskGetSystemState(taskList, TASKS_REPORT_MAX, &totalNow);
  const uint32_t elapsed = totalNow - tasksLastTotal; // microseconds since the last ask (one core's worth)

  JsonDocument doc(psramJsonAllocator());
  doc["uptime"] = millis() / 1000;
  doc["periodMs"] = elapsed / 1000;
  // internal RAM, split by what it can be used for
  doc["internalFree"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);              // all internal RAM free
  doc["internalLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);  // its biggest single piece
  doc["internalMin"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);       // the least free since boot
  doc["defaultInternalFree"] = heap_caps_get_free_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL); // what plain malloc() can still take (not the 16 KB kept back)
  doc["dmaFree"] = heap_caps_get_free_size(MALLOC_CAP_DMA);                        // internal RAM the WiFi chip can write into
  doc["dmaLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
  doc["psramFree"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  doc["psramMin"] = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

  JsonArray tasks = doc["tasks"].to<JsonArray>();
  for (UBaseType_t i = 0; i < taskCount; i++)
  {
    // find this task's counter from the last ask (tasks come and go, so look it up by its handle)
    uint32_t lastRunTime = 0;
    for (uint8_t k = 0; k < TASKS_REPORT_MAX; k++)
    {
      if (tasksLastHandle[k] == taskList[i].xHandle)
      {
        lastRunTime = tasksLastRunTime[k];
        break;
      }
    }
    uint32_t cpuTenths = 0; // share of its core, in 0.1 % steps
    if (elapsed > 0 && tasksLastTotal != 0)
    {
      cpuTenths = (uint32_t)(((uint64_t)(taskList[i].ulRunTimeCounter - lastRunTime) * 1000) / elapsed);
    }
    JsonObject task = tasks.add<JsonObject>();
    task["name"] = taskList[i].pcTaskName;
    int core = (int)taskList[i].xCoreID;
    if (core > 1)
    {
      core = -1; // not pinned: runs on either core
    }
    task["core"] = core;
    task["prio"] = (unsigned)taskList[i].uxCurrentPriority;
    task["cpu"] = cpuTenths / 10.0f; // % of one core
    task["stackFree"] = (unsigned)taskList[i].usStackHighWaterMark;
  }

  // keep this ask's counters for the next one
  for (uint8_t k = 0; k < TASKS_REPORT_MAX; k++)
  {
    tasksLastHandle[k] = NULL;
    tasksLastRunTime[k] = 0;
  }
  for (UBaseType_t i = 0; i < taskCount; i++)
  {
    tasksLastHandle[i] = taskList[i].xHandle;
    tasksLastRunTime[i] = taskList[i].ulRunTimeCounter;
  }
  tasksLastTotal = totalNow;
  free(taskList);
  sendJSON(request, 200, doc);
}

// 0.17.3: the USB bench console (src/can2gauge_Console.cpp) asks the API the same questions the web UI does. The
// handlers are called with no request (NULL), so sendJSON() prints their reply on the USB serial instead. Only
// handlers that need nothing from the request but its body are listed (no query parameters, no uploads).
// false = no such route.
bool apiConsoleRequest(const char *method, const char *path, const String &body)
{
  const bool isGet = !strcmp(method, "GET");
  const bool isPost = !strcmp(method, "POST");
  if (isGet && !strcmp(path, "/api/status"))
  {
    statusOutgoing(NULL);
  }
  else if (isGet && !strcmp(path, "/api/tasks"))
  {
    tasksOutgoing(NULL); // 0.24.20: time and RAM per task (bench only)
  }
  else if (isGet && !strcmp(path, "/api/settings"))
  {
    settingsOutgoing(NULL);
  }
  else if (isPost && !strcmp(path, "/api/settings"))
  {
    settingsIncoming(NULL, body);
  }
  else if (isPost && !strcmp(path, "/api/action"))
  {
    actionIncoming(NULL, body);
  }
  else if (isGet && !strcmp(path, "/api/kline/block"))
  {
    klineBlockOutgoing(NULL); // also keeps the K-line mode session open, as the web UI's polling does
  }
  else if (isGet && !strcmp(path, "/api/can/ids"))
  {
    canIdsOutgoing(NULL); // 0.17.6: CAN bench - the IDs on the bus, their bytes and the DBC message names
  }
  else if (isGet && !strcmp(path, "/api/can/recent"))
  {
    canRecentOutgoing(NULL); // 0.17.6: CAN bench - the last 16 frames
  }
  else if (isPost && !strcmp(path, "/api/me7/probe"))
  {
    me7ProbeIncoming(NULL, body); // 0.17.6: ME7 logger bench probe (console only)
  }
  else if (isPost && !strcmp(path, "/api/kline/listen"))
  {
    klineListenIncoming(body); // 0.24.1: K-line listener (console only)
  }
  else if (isGet && !strcmp(path, "/api/kline/listen"))
  {
    klineListenOutgoing();
  }
  else if (isGet && !strcmp(path, "/api/me7/probe"))
  {
    me7ProbeOutgoing(NULL);
  }
  else if (isPost && !strcmp(path, "/api/me7/fast"))
  {
    me7FastIncoming(NULL, body); // 0.19.0: arm/disarm B7 fast logging (console)
  }
  else if (isPost && !strcmp(path, "/api/me7/cfg"))
  {
    me7CfgIncoming(NULL, body); // 0.20.0: rewrite the .cfg's logged variables (console)
  }
  else if (isPost && !strcmp(path, "/api/me7/fastdiag"))
  {
    me7FastDiagIncoming(NULL, body); // 0.19.0: queue the fast-path bench diagnostic (console)
  }
  else if (isGet && !strcmp(path, "/api/me7/fastdiag"))
  {
    me7FastDiagOutgoing(NULL); // 0.19.0: its raw steps
  }
  else if (isPost && !strcmp(path, "/api/can/dbcinstall"))
  {
    // 0.17.6: CAN bench - a .c2d the console already wrote to /candbc.tmp (kline_bench.py putfile) is loaded and kept,
    // the same as the end of the web UI's DBC upload
    dbcUploadTooBig = false;
    dbcUploadSlot = 0; // 0.28.0: the console's file goes to slot 1, as before
    canDbcDone(NULL);
  }
  else if (isGet && !strcmp(path, "/api/oil"))
  {
    oilOutgoing(NULL);
  }
  else if (isGet && !strcmp(path, "/api/service"))
  {
    serviceOutgoing(NULL);
  }
  else if (isGet && !strcmp(path, "/api/signals"))
  {
    signalsOutgoing(NULL);
  }
  else if (isGet && !strcmp(path, "/api/files"))
  {
    filesOutgoing(NULL);
  }
  else if (isGet && !strcmp(path, "/api/me7/vars"))
  {
    me7VarsOutgoing(NULL);
  }
  else if (isGet && !strcmp(path, "/api/me7read/status"))
  {
    me7ReadStatusOutgoing(NULL);
  }
  else if (isPost && !strcmp(path, "/api/me7read/start"))
  {
    me7ReadStartIncoming(NULL, body);
  }
  else if (isPost && !strcmp(path, "/api/me7read/cancel"))
  {
    me7ReadCancelIncoming(NULL);
  }
  else if (isGet && !strcmp(path, "/api/elm"))
  {
    elmOutgoing(NULL); // 0.22.0: Bluetooth OBD bench - adapter, vehicle, live PIDs, terminal and test status
  }
  else if (isPost && !strcmp(path, "/api/elm/action"))
  {
    elmActionIncoming(NULL, body); // 0.22.0: Bluetooth OBD bench - scan / pair / forget / reidentify / cmd / test
  }
  else
  {
    return false;
  }
  return true;
}

void setupWebServer()
{
  settingsApplyMutex = xSemaphoreCreateMutex(); // 0.38.7: before any route can save settings
  // LittleFS is mounted (guarded) at the top of setup(); never formatted here - a broken image gets the recovery page
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  setupOTA();                       // shared /api/ota/* first: its filter notes web activity for every request
  wifiManagerAttachSta(webServer);  // shared Home WiFi bridge: /api/wifi/sta, /api/wifi/sta/reset, /api/wifi/scan
  webServer.on("/api/status", HTTP_GET, statusOutgoing);
  setupFirstBootRoutes();           // /api/firstboot (src/can2gauge_FirstBoot.cpp)
  webServer.on("/api/signals", HTTP_GET, signalsOutgoing);
  webServer.on("/api/themes", HTTP_GET, themesOutgoing);
  webServer.on("/api/sprites", HTTP_GET, spritesOutgoing);
  webServer.on("/api/lbl/find", HTTP_GET, labelFindOutgoing); // 0.37.0 (before /api/lbl, which would catch it)
  webServer.on("/api/lbl", HTTP_GET, [](AsyncWebServerRequest *request) { // one label file out of the pack, as text
    size_t len = 0;
    char *text = request->hasParam("file") ? labelsReadFile(request->getParam("file")->value().c_str(), &len) : NULL;
    if (!text)
    {
      return request->send(404, "text/plain", "no such label file");
    }
    request->send(200, "text/plain", String(text)); // label files are a few kB
    free(text);
  });
  webServer.on("/api/settings", HTTP_GET, settingsOutgoing);
  webServer.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
               { parseJSON(request, data, len, index, total, settingsIncoming); });
  webServer.on("/api/action", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
               { parseJSON(request, data, len, index, total, actionIncoming); });
  webServer.on("/api/dsg", HTTP_GET, dsgOutgoing);
  webServer.on("/api/vdiag", HTTP_GET, vdiagOutgoing); // VAG diag tab (0.13.0)
  webServer.on("/api/vehicle", HTTP_GET, vehicleOutgoing); // 0.35.0: Your Car
  webServer.on("/api/carscan", HTTP_GET, carScanOutgoing); // 0.37.0: Scan My Car's job and results
  webServer.on("/api/carscan", HTTP_POST, carScanPostDone, NULL, carScanBody); // 0.37.0: start / stop a job
  webServer.on("/api/log/data", HTTP_GET, logDataOutgoing); // Log tab (0.16.0): a piece of the recording
  webServer.on("/api/log", HTTP_GET, logOutgoing);          // Log tab: recording status
  webServer.on("/api/service", HTTP_GET, serviceOutgoing);  // Service tab (0.16.0): faults / basic setting
  webServer.on("/api/oil", HTTP_GET, oilOutgoing);
  webServer.on("/api/kline/block", HTTP_GET, klineBlockOutgoing);
  webServer.on("/api/can/ids", HTTP_GET, canIdsOutgoing);
  webServer.on("/api/adc", HTTP_GET, adcOutgoing);
  webServer.on("/api/adc/set", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
               { parseJSON(request, data, len, index, total, adcSetIncoming); });
  webServer.on("/api/can/dbc", HTTP_POST, canDbcDone, NULL, canDbcBody);
  webServer.on("/api/can/dbc", HTTP_GET, canDbcFileOutgoing); // a DBC slot's file (.c2d), for the page editor
  webServer.on("/api/elm", HTTP_GET, elmOutgoing);
  webServer.on("/api/elm/action", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
               { parseJSON(request, data, len, index, total, elmActionIncoming); });
  webServer.on("/api/chain", HTTP_GET, chainOutgoing);
  webServer.on("/api/chain/setup", HTTP_POST, chainSetupRequest, NULL, chainSetupBody); // 0.38.7: First Boot's setup to the other gauges
  webServer.on("/api/haldex", HTTP_GET, haldexOutgoing);
  webServer.on("/api/ecu/library", HTTP_GET, ecuLibraryOutgoing); // before /api/ecu, which would match it too
  webServer.on("/api/ecu", HTTP_GET, ecuOutgoing);
  webServer.on("/api/files", HTTP_GET, filesOutgoing);
  webServer.on("/api/files/upload", HTTP_POST, filesUploadDone, filesUpload);
  webServer.on("/api/files/delete", HTTP_POST, filesDelete); // refuses the web UI's own files (0.14.0)
  webServer.on("/download", HTTP_GET, [](AsyncWebServerRequest *request)
               {
    if (!request->hasParam("path"))
    {
      return request->send(400);
    }
    String path = request->getParam("path")->value();
    if (!LittleFS.exists(path))
    {
      return request->send(404);
    }
    request->send(LittleFS, path, "application/octet-stream", true); });
  webServer.on("/api/can/recent", HTTP_GET, canRecentOutgoing); // 0.17.6: a named function so the USB console can call it
  webServer.on("/api/me7/vars", HTTP_GET, me7VarsOutgoing);
  webServer.on("/api/me7/fast", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
               [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
               { parseJSON(request, data, len, index, total, me7FastIncoming); }); // 0.19.0: web UI fast-log toggle
  webServer.on("/api/me7/cfg", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
               [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
               { parseJSON(request, data, len, index, total, me7CfgIncoming); }); // 0.20.0: the checkbox variable picker

  // ---- ME7 flash read (optional feature; delete this block to remove the API) ----
  webServer.on("/api/me7read/status", HTTP_GET, me7ReadStatusOutgoing);
  webServer.on("/api/me7read/bin", HTTP_GET, me7DumpOutgoing); // 0.17.4: the read held in memory
  webServer.on("/api/me7read/start", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
               { parseJSON(request, data, len, index, total, me7ReadStartIncoming); });
  webServer.on("/api/me7read/cancel", HTTP_POST, me7ReadCancelIncoming);

  webServer.on("/api/drag", HTTP_GET, dragOutgoing); // 0.38.0: the drag timer's scoreboard
  webServer.on("/app.js", HTTP_GET, appJsOutgoing);  // 0.38.0: before the static files, so it is sent zipped
  wifiManagerAttachStatic(webServer); // last: "/" = the web UI (or the recovery page) + static files, no-cache
  webServer.onNotFound(cannotFind);
  webServer.begin();
  DEBUG("web server up");
}
