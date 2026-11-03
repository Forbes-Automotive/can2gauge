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
#include <can2gauge_API.h>       // fsUsageTick
#include <can2gauge_Nav.h>       // navTick, navHouseTick
#include <can2gauge_Shared.h>    // sharedTick
#include <can2gauge_Vehicle.h>   // vehicleTick

// ===== Settings for this file =====
#define MAX_TASKS_LISTED 40 // tasks the stack report lists (more than the gauge runs: ours + WiFi / Bluetooth / system)

// ===== Functions in this file =====
static void ioTask(void *parameter);    // dash-light input, night dimming, output fault, ignition
static void uiTask(void *parameter);    // gauge value refresh
static void logicTask(void *parameter); // OBD polling, triggers, DBC pages, nav card, board temperature
static void houseTask(void *parameter); // settings save, shared settings, OTA, WiFi, the nav link on / off
static void updatePagePids();           // which PIDs the OBD polling asks first
static void printTaskStacks();          // the boot log's report of each task's spare stack

// starts the gauge's four tasks (last in setup(), once the hardware, the screen and the web server are up)
void setupTasks()
{
  DEBUG("Setting up the tasks...");
  // io stays in internal RAM: the ignition-off shutdown it runs (powerTick) saves the settings to flash
  xTaskCreatePinnedToCore(ioTask, // the task's function
                          "io",   // name (shows in crash reports)
                          3072,   // stack size in bytes
                          NULL,   // no parameter
                          3,      // priority
                          NULL,   // no handle kept
                          0);     // core 0
  // uiTick stays in internal RAM: opening the screensaver / a sprite page reads files from flash
  xTaskCreatePinnedToCore(uiTask,   // the task's function
                          "uiTick", // name (shows in crash reports)
                          6144,     // stack size in bytes
                          NULL,     // no parameter
                          3,        // priority
                          NULL,     // no handle kept
                          1);       // core 1, with LVGL
  // logic's stack in PSRAM (saves internal RAM for WiFi and the web server): nothing in it reads or writes flash
  xTaskCreatePinnedToCoreWithCaps(logicTask,          // the task's function
                                  "logic",            // name (shows in crash reports)
                                  4096,               // stack size in bytes
                                  NULL,               // no parameter
                                  3,                  // priority
                                  NULL,               // no handle kept
                                  0,                  // core 0 (LVGL has core 1)
                                  MALLOC_CAP_SPIRAM); // stack in PSRAM
  // house stays in internal RAM: it saves the settings and handles OTA
  xTaskCreatePinnedToCore(houseTask, // the task's function
                          "house",   // name (shows in crash reports)
                          6144,      // stack size in bytes: WiFi / mDNS restarting needs the room
                          NULL,      // no parameter
                          2,         // priority
                          NULL,      // no handle kept
                          1);        // core 1
  DEBUG("Set up the tasks!");
}

// io: dash-light input, night dimming, output fault (ioTick in can2gauge_IO.cpp). Started here, not in setupIO(),
// because ioTick drives the backlight, so the screen must be up first.
static void ioTask(void *parameter)
{
  (void)parameter;
  while (1)
  {
    ioTick();
    vTaskDelay(pdMS_TO_TICKS(ioRefresh));
  }
}

// the gauge values on the screen (~50 a second)
static void uiTask(void *parameter)
{
  (void)parameter;
  while (1)
  {
    uiTick();
    vTaskDelay(pdMS_TO_TICKS(uiRefresh));
  }
}

// the car side: who polls OBD-II, triggers, DBC pages, the nav card, board temperature and the OpenHaldex link
static void logicTask(void *parameter)
{
  (void)parameter;
  uint32_t lastPollMs = 0;
  uint32_t lastTriggerMs = 0;
  uint32_t lastUserSigMs = 0;
  uint32_t lastNavMs = 0;
  uint32_t lastTempMs = 0;
  while (1)
  {
    const uint32_t now = millis();
    if (now - lastPollMs >= obdPollRefresh)
    {
      lastPollMs = now;
      // one gauge per chain asks for OBD PIDs (the requests go out in the CAN receive task, one straight after each
      // answer); followers read the answers as they pass. Not while the Service tab reads / clears a module's faults
      // on CAN (its answers share the ids), nor with Enable Other Scanners on.
      canSetObdPolling(linkIsMaster() && !runtime.standby && !serviceCanBusy() && !runtime.otherScanners);
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
      navTick(); // the nav card up / down / updated from the phone's latest turn
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

// housekeeping: settings saves, shared settings, the VIN broadcast, OTA confirm / rollback, WiFi upkeep, and the nav
// link started / stopped after a mode picked on the screen
static void houseTask(void *parameter)
{
  (void)parameter;
  bool hasPrintedStacks = false;
  while (1)
  {
    eepTick();
    sharedTick();  // shared settings another gauge in the chain sent (theme, units, screensaver ...)
    vehicleTick(); // a VIN the CAN broadcast (VIN_01) or the VAG diag read delivered
    otaTick();
    pollWiFi();
    fsUsageTick(); // the filesystem's size / used, worked out here after a change, not in a web request
    navHouseTick(); // a nav mode picked on the screen: the phone link started or stopped here (Bluetooth needs the stack)
    if (!hasPrintedStacks && millis() > 120000)
    {
      hasPrintedStacks = true;
      printTaskStacks();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// tells the OBD polling (wired CAN, K-line, OBD adapter) which PIDs the page on the screen shows, so they join the
// fast group - none while no page shows (the menu, a mode screen) - and the PIDs the switched-on triggers watch
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

// the boot log, once, 2 minutes in: how much of each task's stack has never been touched (bytes). A task with
// thousands spare can have a smaller stack, which frees internal RAM for WiFi / the web UI / Bluetooth. Use the gauge
// and its web page in those 2 minutes so the numbers include real work.
static void printTaskStacks()
{
  // the list is ~2 KB: borrowed from PSRAM for the moment rather than taken from this task's stack
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
