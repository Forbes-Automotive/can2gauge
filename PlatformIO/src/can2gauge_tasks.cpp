#include <can2gauge_tasks.h>
#include <can2gauge_Triggers.h>
#include <can2gauge_IO.h>
#include <can2gauge_EEP.h>
#include <can2gauge_OTA.h>
#include <can2gauge_WiFi.h>
#include <can2gauge_Link.h>
#include <can2gauge_UserSig.h>
#include <GaugeUI.h>
#include <CANLine.h>
#include <can2gauge_FirstBoot.h> // firstBootObdProbe
#include <can2gauge_Service.h>   // serviceCanBusy
#include <Display_ST7701.h>
#include <can2gauge_API.h>       // fsUsageTick (0.17.4)
#include <can2gauge_Nav.h>       // navTick (0.30.0)
#include <can2gauge_Shared.h>    // sharedTick (0.30.4)
#include <can2gauge_Vehicle.h>   // vehicleTick (0.35.0)

// io poll: dash-light input, night dimming, output fault (ioTick lives in can2gauge_IO.cpp).
// Started here rather than in setupIO() because ioTick() drives the backlight, so the display must be up first.
static void ioTaskFn(void *)
{
  for (;;)
  {
    ioTick();
    vTaskDelay(pdMS_TO_TICKS(ioRefresh));
  }
}

// gauge value refresh (~30 fps)
static void uiTaskFn(void *)
{
  for (;;)
  {
    uiTick();
    vTaskDelay(pdMS_TO_TICKS(uiRefresh));
  }
}

// tells the OBD polling (wired CAN, K-line, OBD adapter) which PIDs the page on screen shows, so they join the fast
// group. None while no page is showing (the menu, a mode screen).
// 0.35.0: plus the PIDs the switched-on triggers watch, page showing or not, so a trigger on an OBD page value can fire
static void updatePagePids()
{
  uint8_t pids[8];
  uint8_t count = 0;
  const uint32_t seenMs = runtime.pageSeenMs;
  if (seenMs != 0 && millis() - seenMs < 1000)
  {
    count = userSigPagePids(runtime.pageCfgIndex, pids, sizeof(pids));
  }
  count = userSigTriggerPids(pids, count, sizeof(pids));
  obdSetPagePids(pids, count);
}

// vehicle logic: who polls OBD-II, trigger evaluation, board temperature / link status
static void logicTaskFn(void *)
{
  uint32_t lastTriggerMs = 0;
  uint32_t lastPollMs = 0;
  uint32_t lastTempMs = 0;
  uint32_t lastUserSigMs = 0;
  uint32_t lastNavMs = 0;
  for (;;)
  {
    const uint32_t now = millis();
    if (now - lastPollMs >= obdPollRefresh)
    {
      lastPollMs = now;
      // one gauge per chain asks for OBD PIDs on the wire (the requests run in the CAN RX task, one straight after
      // each answer); followers decode the answers passively
      // (0.16.0: and not while the Service tab reads / clears a module's faults on CAN - its answers share the ids)
      canSetObdPolling(linkIsMaster() && !runtime.standby && !serviceCanBusy() && !runtime.otherScanners); // 0.37.0: + Enable Other Scanners
      // OBD-II PIDs next to the VAG / MaxxECU profile: switched on (CAN tab), or First Boot's few-second check
      canSetObdAlongside(settings.obdOverCan != 0 || firstBootObdProbe());
      // which ECU it reads: 0 = the engine, 1..8 = answer id 0x7E8..0x7EF (First Boot's check always asks the engine)
      uint16_t obdEcuId = 0;
      if (settings.obdEcu >= 1 && settings.obdEcu <= 8 && !firstBootObdProbe())
      {
        obdEcuId = 0x7E8 + settings.obdEcu - 1;
      }
      canSetObdEcu(obdEcuId);
      updatePagePids();
    }
    if (now - lastTriggerMs >= triggerRefresh)
    {
      lastTriggerMs = now;
      triggersTick();
    }
    if (now - lastUserSigMs >= 50)
    {
      lastUserSigMs = now;
      userSigTick(); // gauge pages reading a DBC signal
    }
    if (now - lastNavMs >= 100)
    {
      lastNavMs = now;
      navTick(); // 0.30.0: the nav card up / down / updated from the phone's latest turn
    }
    if (now - lastTempMs >= 1000)
    {
      lastTempMs = now;
      signalSet(SIG_BOARD_TEMP, temperatureRead());
      runtime.haldexLink = signalValid(SIG_HALDEX_ACTUAL);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Boot log, once, 2 minutes in: how much of each task's stack has never been touched (bytes). A task with thousands
// spare can be given a smaller stack, which frees internal RAM for WiFi / the web UI / Bluetooth. Open the web page and
// use the gauge in those 2 minutes so the numbers include real work.
#define MAX_TASKS_LISTED 40 // more than the gauge runs (ours + WiFi / Bluetooth / system)
static void printTaskStacks()
{
  // the list is ~2 KB, so it's borrowed from PSRAM for the moment rather than taken from this task's stack
  TaskStatus_t *taskList = (TaskStatus_t *)psramCalloc(MAX_TASKS_LISTED, sizeof(TaskStatus_t));
  if (!taskList)
  {
    return;
  }
  const UBaseType_t taskCount = uxTaskGetSystemState(taskList, MAX_TASKS_LISTED, NULL);
  printInternalRam("2 minutes");
  DEBUG("Task stacks - never used so far, in bytes:");
  for (UBaseType_t i = 0; i < taskCount; i++)
  {
    DEBUG("  %-16s %6u", taskList[i].pcTaskName, (unsigned)taskList[i].usStackHighWaterMark);
  }
  free(taskList);
}

// housekeeping: settings persistence, OTA confirm/rollback, WiFi AP + STA bridge upkeep (all were in loop()).
static void houseTaskFn(void *)
{
  bool hasPrintedStacks = false;
  for (;;)
  {
    eepTick();
    sharedTick(); // 0.30.4: shared settings sent by another gauge in the chain (theme, units, screensaver ...)
    vehicleTick(); // 0.35.0: a VIN the CAN broadcast (VIN_01) delivered
    otaTick();
    pollWiFi();
    fsUsageTick(); // 0.17.4: filesystem size / used, worked out here (core 1) after a change, not in a web request
    if (!hasPrintedStacks && millis() > 120000)
    {
      hasPrintedStacks = true;
      printTaskStacks();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void setupTasks()
{
  // io stays in internal RAM: the ignition-off shutdown it runs (powerTick) saves the settings to flash
  xTaskCreatePinnedToCore(ioTaskFn, "io", 3072, NULL, 3, NULL, 0);
  // uiTick stays in internal RAM: opening the screensaver / a sprite page reads files from flash
  xTaskCreatePinnedToCore(uiTaskFn, "uiTick", 6144, NULL, 3, NULL, 1);
  // logic's stack in PSRAM (saves internal RAM for WiFi and the web server): OBD polling choices, set points and DBC
  // gauge pages only - nothing in it reads or writes flash
  xTaskCreatePinnedToCoreWithCaps(logicTaskFn,       // task function
                                  "logic",           // name (shows in crash reports)
                                  4096,              // stack size in bytes
                                  NULL,              // no parameter
                                  3,                 // priority
                                  NULL,              // no handle kept
                                  0,                 // core 0 (LVGL has core 1)
                                  MALLOC_CAP_SPIRAM); // stack in PSRAM
  // house stays in internal RAM: it saves the settings and handles OTA
  xTaskCreatePinnedToCore(houseTaskFn, "house", 6144, NULL, 2, NULL, 1); // WiFi/MDNS re-init needs the headroom
}
