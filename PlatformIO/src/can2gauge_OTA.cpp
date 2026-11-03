#include <can2gauge_OTA.h>
#include <can2gauge_API.h> // AsyncWebServer + the shared webServer instance
#include <can2gauge_ME7Read.h>
#include <can2gauge_ChainUpdate.h> // 0.25.0: not while another gauge is updating this one (or this one the others)
#include <GaugeUI.h>     // progress ring on the glass while an update is written
#include <Display_ST7701.h> // 0.36.9: displaySetUpdateClock() - a slower screen clock while flash is written
#include <ota_manager.h>   // the shared Forbes Automotive OTA module (_SharedTheme v2): /api/ota/*, filesystem guards
#include "esp_ota_ops.h"

// Updates work the way every Forbes Automotive product does (_SharedTheme/README_OTA.md): the OTA tab (data/ota.js)
// checks GitHub for a release (Releases/releases.json, written by tools/make_release.py) or takes the two files by hand,
// web UI first, then firmware, through the shared ota_manager routes. This file only configures it and adds what
// can2gauge needs on top: the safety hook and the rollback confirmation (a new image boots "pending verify" and is
// confirmed after 60 s of healthy running; a crash-loop rolls back to the previous slot).
static uint32_t bootMs = 0;
static bool confirmed = false;

// ota_manager hook: refuse an upload while the K-line belongs to an ECU flash read
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

void otaOnStart(bool filesystem)
{
  DEBUG("OTA: %s upload starting", filesystem ? "web UI" : "firmware");
  uiProgress(filesystem ? "UPDATING WEB UI" : "UPDATING FIRMWARE", "keep the gauge powered", 0);
  displaySetUpdateClock(true); // 0.36.9: slower screen clock while flash is written (no lines on the screen)
}

void otaOnEnd(bool filesystem, bool success)
{
  DEBUG("OTA: %s upload %s", filesystem ? "web UI" : "firmware", success ? "done" : "FAILED");
  displaySetUpdateClock(false); // 0.36.9: the screen's normal clock back
  uiProgress(filesystem ? "WEB UI" : "FIRMWARE", success ? (filesystem ? "installed" : "installed - restarting") : "update failed",
             success ? 100 : 0, !success, 4000);
}

void setupOTA()
{
  bootMs = millis();
  static ota_config_t otaConfig = otaDefaultConfig();
  otaConfig.fwVersion = CAN2GAUGE_VERSION;
  otaConfig.product = "can2gauge";
  otaConfig.githubRepo = "Forbes-Automotive/can2gauge"; // Releases/ + releases.json live here
  otaManagerInit(&otaConfig);
  otaManagerAttach(webServer); // first, so its activity filter sees every request
}

void otaTick()
{
  if (confirmed || millis() - bootMs < 60000)
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
  confirmed = true;
}
