// First Boot setup: the state in NVS, the question on the glass, and GET / POST /api/firstboot (see can2gauge_FirstBoot.h).
// The checks of each source and the suggested settings run in the web UI (data/firstboot.js), on the endpoints the
// other tabs already use; this file only remembers where the driver is in it.
#include <can2gauge_FirstBoot.h>
#include <can2gauge_API.h> // webServer
#include <ArduinoJson.h>
#include <can2gauge_RAM.h> // 0.25.4: psramJsonAllocator() - the JSON built in PSRAM, not internal RAM
#include <Preferences.h>
#include <FirstBootPrompt.h> // the cards on the glass (lib/GaugeUI)
#include <LVGL_Driver.h>     // 0.24.11: lvglTouchPresent() - no touch, no question the glass can't answer
#include <GaugeUI.h>         // 0.37.3: uiProgress() - the "SETTING UP" ring while the setup runs on the phone

#define FIRSTBOOT_NAMESPACE "c2gfirstboot" // NVS namespace of its own (15 characters at most)
#define FIRSTBOOT_KEY "state"              // FIRSTBOOT_ASK / _WAITING / _DONE
#define FIRSTBOOT_ADDRESS "192.168.1.1"    // the gauge's own access point (src/can2gauge_WiFi.cpp softAPConfig)
#define FIRSTBOOT_ASK_DELAY_MS 2700        // question after the boot logo (2.2 s) and its fade into the first page

#define FIRSTBOOT_OBD_PROBE_MS 8000        // how long the First Boot check asks the engine for OBD-II over CAN

static uint8_t firstBootStateNow = FIRSTBOOT_DONE; // static = private to this file; read with firstBootState()
static uint32_t obdProbeUntilMs = 0;               // millis() the OBD-II over CAN check ends (0 = not running)

// the state as the web UI names it
static const char *stateName(uint8_t state)
{
  if (state == FIRSTBOOT_ASK)
  {
    return "ask";
  }
  if (state == FIRSTBOOT_WAITING)
  {
    return "waiting";
  }
  return "done";
}

void firstBootSetState(uint8_t state)
{
  firstBootStateNow = state;
  Preferences prefs;
  prefs.begin(FIRSTBOOT_NAMESPACE, false);
  prefs.putUChar(FIRSTBOOT_KEY, state);
  prefs.end();
  DEBUG("First Boot: %s", stateName(state));
}

uint8_t firstBootState()
{
  return firstBootStateNow;
}

bool firstBootObdProbe()
{
  if (obdProbeUntilMs == 0)
  {
    return false;
  }
  if ((int32_t)(millis() - obdProbeUntilMs) >= 0) // the cast keeps the sum right when millis() wraps round (49 days)
  {
    obdProbeUntilMs = 0;
    return false;
  }
  return true;
}

// Runs before readEEP(), because readEEP() writes the default settings on a brand-new gauge: once it has run, every
// gauge "has settings". A gauge that had settings before this firmware arrived is already set up, so it never asks.
void firstBootCheck()
{
  DEBUG("Checking first boot...");
  Preferences prefs;
  prefs.begin(FIRSTBOOT_NAMESPACE, false);
  const bool hasState = prefs.isKey(FIRSTBOOT_KEY);
  if (hasState)
  {
    firstBootStateNow = prefs.getUChar(FIRSTBOOT_KEY, FIRSTBOOT_DONE);
  }
  prefs.end();
  if (hasState)
  {
    DEBUG("Checked first boot: %s", stateName(firstBootStateNow));
    return;
  }

  // no state saved yet: a brand-new gauge, or one set up on firmware from before first boot existed
  bool hadSettings = false;
  Preferences settingsPrefs;
  if (settingsPrefs.begin("can2gauge", true)) // read-only; false when the namespace doesn't exist yet (brand new)
  {
    hadSettings = settingsPrefs.isKey("cfg"); // the settings blob (src/can2gauge_EEP.cpp)
    settingsPrefs.end();
  }
  if (hadSettings)
  {
    firstBootSetState(FIRSTBOOT_DONE);
  }
  else
  {
    firstBootSetState(FIRSTBOOT_ASK);
  }
  DEBUG("Checked first boot!");
}

// the driver answered the question on the glass (runs in the display task)
static void answeredOnGlass(bool yes)
{
  if (yes)
  {
    firstBootSetState(FIRSTBOOT_WAITING);
  }
  else
  {
    firstBootSetState(FIRSTBOOT_DONE);
  }
}

void firstBootPrompt()
{
  if (runtime.wokeFromSleep)
  {
    return; // a deep-sleep wake with the ignition on: straight back to the gauge, no questions
  }
  if (firstBootStateNow == FIRSTBOOT_DONE)
  {
    return;
  }
  // the AP only has a password when it is 8 characters or more (src/can2gauge_WiFi.cpp), otherwise it is open
  const char *password = "";
  if (strlen(settings.apPassword) >= 8)
  {
    password = settings.apPassword;
  }
  char localName[NAME_LEN + 8];
  snprintf(localName, sizeof(localName), "%s.local", settings.hostname); // c2g.local on a new gauge
  // the login card straight away when Yes was said on an earlier boot - or (0.24.11) when the glass has no touch, so
  // the Yes / Not Now question could never be answered (the card goes once the setup page opens on the phone)
  bool loginFirst = false;
  if (firstBootStateNow == FIRSTBOOT_WAITING || !lvglTouchPresent())
  {
    loginFirst = true;
  }
  uiFirstBootAsk(settings.apSsid, password, FIRSTBOOT_ADDRESS, localName, answeredOnGlass, FIRSTBOOT_ASK_DELAY_MS,
                 loginFirst);
}

// sends a JSON reply. AsyncWebServer hands every request over as a pointer, so this one uses the arrow (->) to reach it.
static void sendFirstBootJson(AsyncWebServerRequest *request, int code, JsonDocument &doc)
{
  // the buffer made the right size up front, so it never has to grow and copy (see sendJSON in can2gauge_API.cpp)
  const size_t replyLength = measureJson(doc);
  AsyncResponseStream *response = request->beginResponseStream("application/json", replyLength + 64);
  serializeJson(doc, *response);
  response->setCode(code);
  request->send(response);
}

// GET /api/firstboot: where the driver is in the setup, and how the glass told them to log in
static void firstBootOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  doc["state"] = stateName(firstBootStateNow);
  doc["glass"] = uiFirstBootShowing(); // the question / login card is on the glass
  doc["wifiName"] = settings.apSsid;
  doc["address"] = FIRSTBOOT_ADDRESS;
  doc["local"] = settings.hostname; // + ".local"
  sendFirstBootJson(request, 200, doc);
}

// POST /api/firstboot, the request itself: the reply is sent from the body below
static void firstBootPostDone(AsyncWebServerRequest *request)
{
}

// POST /api/firstboot body: {"opened": true} the setup page is open (the glass card goes); {"obdProbe": true} ask the
// engine for OBD-II over CAN for a few seconds (the check's "OBD-II over CAN" row), whatever the CAN profile;
// {"state": "waiting" | "done" | "ask"} Settings > First Boot setup / Finish / Skip.
// 0.37.3: {"screen": {"detail": "Scanning 01 Engine", "pct": 45}} the setup's progress on the screen (a ring that stays
// until the setup ends; 0.37.5: + "title", "hideMs", "failed" for the Scan My Car tab); {"state": "done", "applied": true} = Apply (the ring says SET UP, then goes), without = Skip
static void firstBootIncoming(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
  JsonDocument reply(psramJsonAllocator());
  if (index != 0 || len != total) // the body is a few bytes: it always comes in one piece
  {
    reply["error"] = "body too long";
    sendFirstBootJson(request, 400, reply);
    return;
  }
  JsonDocument doc(psramJsonAllocator());
  const DeserializationError error = deserializeJson(doc, data, len);
  if (error)
  {
    reply["error"] = "bad JSON";
    sendFirstBootJson(request, 400, reply);
    return;
  }
  const bool opened = doc["opened"] | false;
  if (opened)
  {
    uiFirstBootHide(); // the driver is in the web UI: the login card has done its job
    uiProgress("SETTING UP", "finish on your phone", 0); // 0.37.3: the screen says the setup is running
  }
  JsonObjectConst screenObj = doc["screen"];
  if (!screenObj.isNull()) // 0.37.3: the setup's progress, as the phone reports it
  {
    // 0.37.5: also the Scan My Car tab's scan: its own title ("SCANNING"), and at the end how long the last word stays
    // up before the ring fades (hideMs), red for a scan that found no way in (failed)
    const char *title = screenObj["title"] | "SETTING UP";
    const bool isSetup = !strcmp(title, "SETTING UP");
    const uint32_t hideMs = screenObj["hideMs"] | 0;
    const bool failed = screenObj["failed"] | false;
    int percent = screenObj["pct"] | 0;
    if (percent > 99 && hideMs == 0)
    {
      percent = 99; // 100 only with the last word (the ring then goes by itself)
    }
    if (percent > 100)
    {
      percent = 100;
    }
    if (percent < 0)
    {
      percent = 0;
    }
    // 0.37.7: a setup update that arrives after Apply / Skip (it was on its way when the setup ended) is dropped, or
    // it would put the ring back up with nothing to take it away (bench: stuck at 52 % after saving)
    if (isSetup && firstBootStateNow == FIRSTBOOT_DONE)
    {
      DEBUG("First Boot: a late setup update dropped (the setup has ended)");
    }
    else
    {
      uiProgress(title, screenObj["detail"] | "", percent, failed, hideMs);
    }
  }
  const bool obdProbe = doc["obdProbe"] | false;
  if (obdProbe)
  {
    obdProbeUntilMs = millis() + FIRSTBOOT_OBD_PROBE_MS; // the logic task asks on 0x7DF until then (src/can2gauge_tasks.cpp)
    if (obdProbeUntilMs == 0)
    {
      obdProbeUntilMs = 1; // 0 means "not running"
    }
  }
  const char *newState = doc["state"] | "";
  if (!strcmp(newState, "waiting"))
  {
    firstBootSetState(FIRSTBOOT_WAITING);
  }
  else if (!strcmp(newState, "done"))
  {
    firstBootSetState(FIRSTBOOT_DONE);
    uiFirstBootHide();
    // 0.37.3: the setup ring ends: Apply shows it finished for 3 s, Skip just takes it away
    const bool applied = doc["applied"] | false;
    if (applied)
    {
      uiProgress("SET UP", "your gauges are ready", 100, false, 3000);
    }
    else
    {
      uiProgress("", "", -1); // a percent below 0 hides the ring
    }
  }
  else if (!strcmp(newState, "ask"))
  {
    firstBootSetState(FIRSTBOOT_ASK); // asks on the glass at the next boot
  }
  reply["status"] = "ok";
  reply["state"] = stateName(firstBootStateNow);
  sendFirstBootJson(request, 200, reply);
}

void setupFirstBootRoutes()
{
  webServer.on("/api/firstboot", HTTP_GET, firstBootOutgoing);
  webServer.on("/api/firstboot", HTTP_POST, firstBootPostDone, NULL, firstBootIncoming);
}
