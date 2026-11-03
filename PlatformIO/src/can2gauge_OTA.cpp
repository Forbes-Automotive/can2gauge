// Updates work the way every Forbes Automotive product does (_SharedTheme/README_OTA.md): the OTA tab (data/ota.js)
// checks GitHub for a release (Releases/releases.json, written by tools/make_release.py) or takes the two files by hand,
// web UI first, then firmware, through the shared ota_manager routes. This file only configures it and adds what
// can2gauge needs on top: the safety hook and the rollback confirmation (a new image boots "pending verify" and is
// confirmed after 60 s of healthy running; a crash-loop rolls back to the previous slot).

#include <can2gauge_OTA.h>
#include <can2gauge_API.h>         // AsyncWebServer + the shared webServer instance
#include <can2gauge_ME7Read.h>     // me7ReadPending: no update during an ECU flash read
#include <can2gauge_ChainUpdate.h> // not while another gauge is updating this one (or this one the others)
#include <GaugeUI.h>               // progress ring on the screen while an update is written
#include <Display_ST7701.h>        // displaySetUpdateClock() - a slower screen clock while flash is written
#include <ota_manager.h>           // the shared Forbes Automotive OTA module (_SharedTheme v2): /api/ota/*, filesystem guards
#include "esp_ota_ops.h"           // the running image's "pending verify" state and its confirmation

// ===== File variables (static: private to this file) =====
static uint32_t bootMs = 0;          // millis() at setupOTA: the new image is confirmed 60 s after this
static bool hasCheckedImage = false; // true once the running image has been checked (and confirmed if it was new)

// sets up the shared OTA module with can2gauge's version and GitHub repo, and adds its web routes
void setupOTA()
{
  DEBUG("Setting up OTA...");
  bootMs = millis();
  static ota_config_t otaConfig = otaDefaultConfig(); // the shared module's defaults, then can2gauge's own below
  otaConfig.fwVersion = CAN2GAUGE_VERSION;
  otaConfig.product = "can2gauge";
  otaConfig.githubRepo = "Forbes-Automotive/can2gauge"; // Releases/ + releases.json live here
  otaManagerInit(&otaConfig);
  otaManagerAttach(webServer); // first, so its activity filter sees every request
  DEBUG("Set up OTA!");
}

// once, 60 s after boot: a freshly installed image that is still "pending verify" is marked good, so no rollback
void otaTick()
{
  if (hasCheckedImage || millis() - bootMs < 60000)
  {
    return;
  }
  const esp_partition_t *runningPartition = esp_ota_get_running_partition();
  esp_ota_img_states_t imageState;
  if (esp_ota_get_state_partition(runningPartition, &imageState) == ESP_OK && imageState == ESP_OTA_IMG_PENDING_VERIFY)
  {
    esp_ota_mark_app_valid_cancel_rollback();
    DEBUG("OTA: new firmware confirmed");
  }
  hasCheckedImage = true;
}

// ota_manager hook: refuse an upload while the K-line belongs to an ECU flash read, or a chain update is running
bool otaIsSafe(String &reason)
{
  if (me7ReadPending() || !strcmp(runtime.klineState, "ECU read"))
  {
    reason = "an ECU flash read is running on the K-line";
    return false;
  }
  if (chainUpdateBusy())
  {
    reason = "a chain update is running (Daisy Chain tab)";
    return false;
  }
  return true;
}

// ota_manager hook: an upload is starting - the progress screen goes up and the screen clock slows
void otaOnStart(bool filesystem)
{
  // what is being updated, for the log
  const char *whatUpdated = "firmware";
  if (filesystem)
  {
    whatUpdated = "web UI";
  }
  DEBUG("OTA: %s upload starting", whatUpdated);
  // the progress screen's title
  const char *title = "UPDATING FIRMWARE";
  if (filesystem)
  {
    title = "UPDATING WEB UI";
  }
  uiProgress(title, "keep the gauge powered", 0);
  displaySetUpdateClock(true); // slower screen clock while flash is written (no lines on the screen)
}

// ota_manager hook: an upload has finished or failed - the screen's normal clock back, and the result on the screen
void otaOnEnd(bool filesystem, bool success)
{
  // what was updated and how it went, for the log
  const char *whatUpdated = "firmware";
  if (filesystem)
  {
    whatUpdated = "web UI";
  }
  const char *result = "FAILED";
  if (success)
  {
    result = "done";
  }
  DEBUG("OTA: %s upload %s", whatUpdated, result);
  displaySetUpdateClock(false); // the screen's normal clock back
  // the progress screen: title, message and how full the ring is
  const char *title = "FIRMWARE";
  if (filesystem)
  {
    title = "WEB UI";
  }
  const char *message = "update failed";
  int percent = 0;
  if (success)
  {
    percent = 100;
    if (filesystem)
    {
      message = "installed";
    }
    else
    {
      message = "installed - restarting"; // the OTA module restarts the gauge after a firmware update
    }
  }
  uiProgress(title, message, percent, !success, 4000); // hides itself after 4 s
}
