// First Boot setup: the state in NVS, the question on the screen, and GET / POST /api/firstboot (see can2gauge_FirstBoot.h).
// The checks of each source and the suggested settings run in the web UI (data/firstboot.js), on the endpoints the
// other tabs already use; this file only remembers where the driver is in it.
#include <can2gauge_FirstBoot.h>
#include <can2gauge_API.h> // webServer
#include <ArduinoJson.h>
#include <can2gauge_RAM.h> // psramJsonAllocator(): the JSON built in PSRAM, not internal RAM
#include <Preferences.h>
#include <FirstBootPrompt.h> // the cards on the screen (lib/GaugeUI)
#include <LVGL_Driver.h>     // lvglTouchPresent(): no touch, no question the screen can't answer
#include <GaugeUI.h>         // uiProgress(): the "SETTING UP" ring while the setup runs on the phone

// ===== Settings for this file =====
#define FIRSTBOOT_NAMESPACE "c2gfirstboot" // NVS namespace of its own (15 characters at most)
#define FIRSTBOOT_KEY "state"              // FIRSTBOOT_ASK / _WAITING / _DONE
#define FIRSTBOOT_ADDRESS "192.168.1.1"    // the gauge's own access point (src/can2gauge_WiFi.cpp softAPConfig)
#define FIRSTBOOT_ASK_DELAY_MS 2700        // question after the boot logo (2.2 s) and its fade into the first page
#define FIRSTBOOT_OBD_PROBE_MS 8000        // how long the First Boot check asks the engine for OBD-II over CAN

// ===== Variables for this file =====
static uint32_t obdProbeUntilMs = 0;               // millis() the OBD-II over CAN check ends (0 = not running)
static uint8_t firstBootStateNow = FIRSTBOOT_DONE; // static = private to this file; read with firstBootState()

// ===== Small helpers =====
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

// ===== Functions in this file =====
static void answeredOnGlass(bool yes);                         // the driver's Yes / Not Now on the screen
static void firstBootOutgoing(AsyncWebServerRequest *request); // GET /api/firstboot
static void firstBootPostDone(AsyncWebServerRequest *request); // POST /api/firstboot, the request itself
static void firstBootIncoming(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total); // POST body
static void sendFirstBootJson(AsyncWebServerRequest *request, int code, JsonDocument &doc); // sends a JSON reply

// Runs before readEEP(), because readEEP() writes the default settings on a brand-new gauge: once it has run, every
// gauge "has settings". A gauge that had settings before First Boot existed is already set up, so it never asks.
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

  // no state saved yet: a brand-new gauge, or one set up on firmware from before First Boot existed
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

// after setupDisplay(): a gauge not set up yet asks "Set this gauge up from your phone?" on the screen, or shows how to
// log in when the driver already said Yes
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
  // the login card straight away when Yes was said on an earlier boot - or when the screen has no touch, so the
  // Yes / Not Now question could never be answered (the card goes once the setup page opens on the phone)
  bool loginFirst = false;
  if (firstBootStateNow == FIRSTBOOT_WAITING || !lvglTouchPresent())
  {
    loginFirst = true;
  }
  uiFirstBootAsk(settings.apSsid, password, FIRSTBOOT_ADDRESS, localName, answeredOnGlass, FIRSTBOOT_ASK_DELAY_MS,
                 loginFirst);
}

// the web UI's GET / POST /api/firstboot (called from setupWebServer, before the server starts)
void setupFirstBootRoutes()
{
  webServer.on("/api/firstboot", HTTP_GET, firstBootOutgoing);
  webServer.on("/api/firstboot", HTTP_POST, firstBootPostDone, NULL, firstBootIncoming);
}

// a new First Boot state, saved to NVS at once
void firstBootSetState(uint8_t state)
{
  firstBootStateNow = state;
  Preferences prefs;
  prefs.begin(FIRSTBOOT_NAMESPACE, false);
  prefs.putUChar(FIRSTBOOT_KEY, state);
  prefs.end();
  DEBUG("First Boot: %s", stateName(state));
}

// where the gauge is in the setup: FIRSTBOOT_ASK / _WAITING / _DONE
uint8_t firstBootState()
{
  return firstBootStateNow;
}

// true while the First Boot check is asking the engine for OBD-II over CAN (a few seconds after the phone asks)
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

// the driver answered the question on the screen (runs in the display task)
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

// GET /api/firstboot: where the driver is in the setup, and how the screen told them to log in
static void firstBootOutgoing(AsyncWebServerRequest *request)
{
  JsonDocument doc(psramJsonAllocator());
  doc["state"] = stateName(firstBootStateNow);
  doc["glass"] = uiFirstBootShowing(); // the question / login card is on the screen
  doc["wifiName"] = settings.apSsid;
  doc["address"] = FIRSTBOOT_ADDRESS;
  doc["local"] = settings.hostname; // + ".local"
  sendFirstBootJson(request, 200, doc);
}

// POST /api/firstboot, the request itself: the reply is sent from the body below
static void firstBootPostDone(AsyncWebServerRequest *request)
{
  (void)request; // not used: firstBootIncoming sends the reply
}

// POST /api/firstboot body:
//   {"opened": true}    the setup page is open on the phone (the login card on the screen goes)
//   {"obdProbe": true}  ask the engine for OBD-II over CAN for a few seconds (the check's "OBD-II over CAN" row),
//                       whatever the CAN profile
//   {"state": "waiting" | "done" | "ask"}  Settings > First Boot setup / Finish / Skip. {"state": "done",
//                       "applied": true} = Apply (the ring says SET UP, then goes), without "applied" = Skip
//   {"screen": {"detail": "Scanning 01 Engine", "pct": 45}}  the setup's progress on the screen (a ring that stays
//                       until the setup ends), with "title", "hideMs", "failed" for the Scan My Car tab
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
    uiProgress("SETTING UP", "finish on your phone", 0); // the screen says the setup is running
  }
  JsonObjectConst screenObj = doc["screen"];
  if (!screenObj.isNull()) // the setup's progress, as the phone reports it
  {
    // also the Scan My Car tab's scan: its own title ("SCANNING"), and at the end how long the last word stays up
    // before the ring fades (hideMs), red for a scan that found no way in (failed)
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
    // a setup update that arrives after Apply / Skip (it was on its way when the setup ended) is dropped, or it would
    // put the ring back up with nothing to take it away (bench: stuck at 52 % after saving)
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
    // the setup ring ends: Apply shows it finished for 3 s, Skip just takes it away
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
    firstBootSetState(FIRSTBOOT_ASK); // asks on the screen at the next boot
  }
  reply["status"] = "ok";
  reply["state"] = stateName(firstBootStateNow);
  sendFirstBootJson(request, 200, reply);
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
