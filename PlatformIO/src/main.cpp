/*
can2gauge - Forbes Automotive
2.1" 1000-nits Round Touch Gauge - supporting  CAN / K-line / OBD-II - OpenHaldex companion
Includes sprites for 'fun'
Version: see include/can2gauge_ver.h
*/

#include <can2gauge_defs.h>
#include <esp_heap_caps.h> // heap_caps_*: internal RAM figures in the boot log
#include <esp_rom_sys.h>   // esp_rom_printf: the bench RAM listing (debugRamRegions)
#include <esp_heap_trace.h>                  // bench: who made each boot-time allocation (CONFIG_HEAP_TRACING_STANDALONE)
#include <esp_private/startup_internal.h>    // ESP_SYSTEM_INIT_FN: our own hook at the very start of the framework's start-up
#include <can2gauge_EEP.h>
#include <can2gauge_IO.h>
#include <can2gauge_Display.h>
#include <can2gauge_Triggers.h>
#include <can2gauge_KLine.h>
#include <can2gauge_WiFi.h>
#include <can2gauge_API.h>
#include <can2gauge_DSG.h>
#include <can2gauge_VagDiag.h>
#include <can2gauge_Vehicle.h> // 0.35.0: setupVehicle
#include <can2gauge_Drag.h>    // 0.38.0: setupDrag (the drag timer's scoreboard)
#include <can2gauge_ELM.h>
#include <can2gauge_Nav.h> // 0.30.0: Google Maps turn-by-turn from the phone app
#include <can2gauge_Link.h>
#include <can2gauge_ChainUpdate.h> // 0.25.0: gauge-to-gauge updates over ESP-NOW
#include <can2gauge_OHX.h>
#include <can2gauge_CanRelay.h>
#include <can2gauge_ECU.h>
#include <can2gauge_FirstBoot.h>
#include <can2gauge_CanMon.h>
#include <can2gauge_UserSig.h>
#include <can2gauge_ADC2CAN.h>
#include <can2gauge_Logger.h>
#include <CANLine.h> // obdSetPidHook
#include <can2gauge_tasks.h>
#include <ota_manager.h> // guarded LittleFS mount (shared OTA module)
#include <can2gauge_Power.h>
#include <can2gauge_Console.h> // 0.17.3: USB bench console
#include <can2gauge_RAM.h>     // 0.25.4: the note of failed memory requests
#include <can2gauge_BootSync.h> // 0.30.11: the row's boot logos lined up over CAN
#include <esp_sleep.h>
#include <driver/gpio.h>

#if debugRamRegions
// 0.24.17 bench: who holds the internal RAM before setup() runs. Each block of 1 KB or more that is in use is printed
// with its address and size, and named after the task whose stack it is (most big blocks are task stacks).
#define BIG_BLOCK_BYTES 1024
#define MAX_TASKS_SEEN 40
static TaskStatus_t tasksSeen[MAX_TASKS_SEEN]; // the tasks running now (static: kept off loopTask's stack)
static UBaseType_t tasksSeenCount = 0;

// called by heap_caps_walk for every block in internal RAM
static bool bigBlockWalker(walker_heap_into_t heapInfo, walker_block_info_t block, void *userData)
{
  (void)heapInfo;
  (void)userData;
  if (!block.used || block.size < BIG_BLOCK_BYTES)
  {
    return true; // free, or small: keep walking
  }
  const char *owner = "?";
  for (UBaseType_t taskIndex = 0; taskIndex < tasksSeenCount; taskIndex++)
  {
    const uint8_t *stackStart = (const uint8_t *)tasksSeen[taskIndex].pxStackBase;
    const uint8_t *blockStart = (const uint8_t *)block.ptr;
    if (stackStart >= blockStart && stackStart < blockStart + block.size)
    {
      owner = tasksSeen[taskIndex].pcTaskName; // this block is that task's stack
    }
  }
  esp_rom_printf("  block at %p: %u bytes  %s\n", block.ptr, (unsigned)block.size, owner);
  if (owner[0] == '?') // not a task stack: its first 48 bytes, as hex and as text, to tell whose it is
  {
    const uint8_t *bytes = (const uint8_t *)block.ptr;
    char text[49];
    esp_rom_printf("    ");
    for (uint8_t byteIndex = 0; byteIndex < 48; byteIndex++)
    {
      esp_rom_printf("%02x", bytes[byteIndex]);
      if (bytes[byteIndex] >= 32 && bytes[byteIndex] < 127)
      {
        text[byteIndex] = (char)bytes[byteIndex];
      }
      else
      {
        text[byteIndex] = '.';
      }
    }
    text[48] = '\0';
    esp_rom_printf("\n    %s\n", text);
    // who points at it: every word in internal RAM that holds an address inside this block (the owner keeps one)
    const uint32_t blockFrom = (uint32_t)block.ptr;
    const uint32_t blockTo = blockFrom + block.size;
    uint8_t found = 0;
    for (uint32_t address = 0x3FC88000; address < 0x3FCF8000 && found < 6; address += 4)
    {
      if (address >= blockFrom && address < blockTo)
      {
        continue; // inside the block itself
      }
      const uint32_t word = *(const volatile uint32_t *)address;
      if (word >= blockFrom && word < blockTo)
      {
        esp_rom_printf("    pointed at from %p\n", (void *)address);
        found++;
      }
    }
  }
  return true;
}

// heap tracing from the first moment of start-up (CORE stage, priority 1 - before the framework's own start-up code),
// so the boot log can say which code made each big allocation (platformio.ini: CONFIG_HEAP_TRACING_STANDALONE)
#if CONFIG_HEAP_TRACING_STANDALONE
#define TRACE_RECORDS 300
static heap_trace_record_t traceRecords[TRACE_RECORDS]; // the allocations seen (static: there is no heap for it yet)
static esp_err_t traceInitResult = ESP_FAIL;            // what heap_trace_init_standalone answered
static esp_err_t traceStartResult = ESP_FAIL;           // what heap_trace_start answered

// SECONDARY stage, priority 101: just after the heap's and esp_timer's own start-up (100), before Arduino's start-up
// and the global constructors. (CORE stage priority 1 recorded nothing.)
ESP_SYSTEM_INIT_FN(c2gHeapTraceStart, SECONDARY, BIT(0), 101)
{
  traceInitResult = heap_trace_init_standalone(traceRecords, TRACE_RECORDS);
  traceStartResult = heap_trace_start(HEAP_TRACE_LEAKS); // keeps only what is still allocated
  return ESP_OK;
}

// every traced allocation of 2 KB or more still held, with the code addresses that made it (decode with addr2line)
static void printTracedBlocks()
{
  heap_trace_stop();
  const size_t count = heap_trace_get_count();
  esp_rom_printf("traced allocations of 2 KB or more still held (%u traced, init %d start %d):\n", (unsigned)count,
                 (int)traceInitResult, (int)traceStartResult);
  for (size_t recordIndex = 0; recordIndex < count; recordIndex++)
  {
    heap_trace_record_t record;
    if (heap_trace_get(recordIndex, &record) != ESP_OK || record.size < 2048)
    {
      continue;
    }
    esp_rom_printf("  %p %u bytes from", record.address, (unsigned)record.size);
    for (uint8_t depth = 0; depth < CONFIG_HEAP_TRACING_STACK_DEPTH; depth++)
    {
      esp_rom_printf(" %p", record.alloced_by[depth]);
    }
    esp_rom_printf("\n");
  }
}
#endif

static void printBigInternalBlocks()
{
#if CONFIG_HEAP_TRACING_STANDALONE
  printTracedBlocks();
#endif
  tasksSeenCount = uxTaskGetSystemState(tasksSeen, MAX_TASKS_SEEN, NULL);
  esp_rom_printf("internal RAM blocks of 1 KB or more in use (%u tasks running):\n", (unsigned)tasksSeenCount);
  heap_caps_walk(MALLOC_CAP_INTERNAL, bigBlockWalker, NULL);
}
#endif

void setup()
{
  // Arduino's framework puts every memory request under 4 KB in internal RAM - web requests, Strings, JSON, buffers -
  // the same ~300 KB that WiFi, Bluetooth and every task stack must use, while the 8 MB PSRAM sits idle. From here on,
  // anything of 512 bytes or more comes from PSRAM first (internal RAM if PSRAM is full). Things that must be internal
  // (DMA, locks, WiFi / Bluetooth buffers, interrupt code) ask for internal RAM by name, so they're not affected.
  heap_caps_malloc_extmem_enable(512);
  const unsigned long setupStartMs = millis(); // 0.24.28: boot timing - how long the framework took before setup()

  // backlight off first: the PCB pull-down on BL_PWM keeps the AL8860 CTRL low through reset and boot, 
  // keep it low so it stays dark through the serial wait and the panel init (the panel
  // sheets want the backlight on only after the init)
  pinMode(LCD_PIN_BL_PWM, OUTPUT);
  digitalWrite(LCD_PIN_BL_PWM, LOW);
  
  // waking from deep sleep (ignition on permanent power, src/can2gauge_Power.cpp): release the pins it held
  gpio_hold_dis((gpio_num_t)LCD_PIN_BL_PWM);
  gpio_hold_dis((gpio_num_t)CAN_PIN_TX);
  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  const bool fromSleep = wakeCause == ESP_SLEEP_WAKEUP_EXT0 || wakeCause == ESP_SLEEP_WAKEUP_TIMER;

  #if enableDebug
  Serial.begin(500000);
  Serial.setTxTimeoutMs(10);
  // wait (up to 2 s) for a PC to open the USB serial port, so the bench sees the whole boot log. 0.24.28: only while a
  // USB host is actually there - in the car nothing is plugged in, and this waited the full 2 s at every power-up
  // before the screen could come on. HWCDC::isPlugged() = the USB port sees a host's frames; it needs a moment to
  // notice one, so the first 100 ms are always given.
  unsigned long serialWaitStart = millis();
  while (!fromSleep && !Serial && millis() - serialWaitStart < 2000) // a wake check must not wait for a console
  {
    if (millis() - serialWaitStart > 100 && !HWCDC::isPlugged())
    {
      break; // no PC on the USB port: don't wait for one
    }
    delay(10);
  }
  DEBUG("can2gauge %s launching (%s)...", CAN2GAUGE_VERSION, CAN2GAUGE_BOARD);
  DEBUG("setup() started at %lu ms (serial wait %lu ms)", setupStartMs, millis() - serialWaitStart); // 0.24.28: boot timing
  // 0.24.17: internal RAM before any of the gauge's own set-up - the size of the whole area (what the framework's code
  // and fixed data leave over) and how much of it the framework has already used by the time setup() runs
  DEBUG("RAM at start: %u KB internal free of %u KB (largest piece %u KB)",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
#if debugRamRegions
  heap_caps_print_heap_info(MALLOC_CAP_INTERNAL); // 0.24.17 bench: every internal RAM area, its size and what's used
  printBigInternalBlocks();                       // ... and every block of 1 KB or more already taken, with its owner
#endif
#endif

  setupRam(); // 0.25.4: note any memory request that fails from here on (kept through a reset, shown at the next boot)
  signalsInit();
  firstBootCheck(); // brand new or already set up? (before readEEP, which writes the default settings on a new gauge)
  readEEP(); // settings drive everything below, so load them first
  // web UI / logos / ME7 files: mounted now (the boot logo needs it) and only after a superblock check - a half-written
  // image is never mounted, and with no usable UI the web server serves the recovery page instead (shared OTA module)
  if (!otaFsMountSafe())
  {
    DEBUG("LittleFS not usable - the web server will serve the recovery page");
  }

  printInternalRam("settings + filesystem"); // the boot log shows where the internal RAM goes (WiFi needs ~50 KB of it)
  DEBUG("settings + filesystem at %lu ms", millis()); // 0.24.28: boot timing

  setupPower();     // the CAN hook that watches terminal 15 (before the bus comes up)
  if (fromSleep)
  {
    canSetListenOnly(true); // a wake check never sends on the car's bus
  }
  setupIO();        // I2C + TCA9554 expander + output + CAN termination + CAN bus (first: display reset/CS are on the expander)
  DEBUG("IO + CAN at %lu ms", millis()); // 0.24.28: boot timing
  powerWakeProbe(fromSleep); // woken from deep sleep with the ignition still off: straight back to sleep (glass dark)
  setupBootSync();  // 0.30.11: the row's boot logos start together - 0.30.14: a handshake over CAN, so this comes before
                    // the display (it keeps the glass dark until every gauge is ready)
  setupDisplay();   // ST7701 panel + touch + LVGL + gauge UI + boot logo
  printInternalRam("display");
  firstBootPrompt(); // a gauge not set up yet asks "Set this gauge up from your phone?" after the boot logo
  setupTriggers();  // set points
  setupKLine();     // K-line task (protocol from settings)
  setupLink();      // gauge chain: node id, election, CAN link frames (ESP-NOW joins once WiFi is up)
  setupChainUpdate(); // 0.25.0: pass this gauge's firmware + web files on to the other gauges in the chain
  setupOpenHaldexEspNow();       // OpenHaldex over WiFi (rides on the chain's ESP-NOW stack)
  setupCanRelay();  // the car's CAN frames relayed over WiFi by OpenHaldex / adc2can (same ESP-NOW stack)
  setupEcuNow();    // an Ignitron / MaxxECU's data over WiFi from usb2ip (same ESP-NOW stack)
  setupCanMonitor();    // every id on the bus + the car's DBC (menu > CAN, web UI CAN tab)
  setupADC2CAN();   // adc2can sensor boards on the bus (web UI > ADC)
  userSigApply();   // gauge pages from a chosen source: names / units
  obdSetPidHook(userSigOnObdPid);
  canOnFrame(userSigOnCanFrame, true); // gauge pages reading a DBC signal: decoded the moment their frame arrives
  setupDSG();       // gearbox diagnostics task (UDS / TP2.0 on CAN; the passive gear decode is in CANLine)
  setupVagDiag();   // VAG diag task: measuring values from one module over CAN, UDS or TP2.0 (web UI > VAG diag)
  setupVehicle();   // 0.35.0: which car this is - the confirmed car from NVS, and VIN_01 heard on the CAN
  setupDrag();      // 0.38.0: the drag timer's scoreboard, from NVS
  setupNav();       // 0.30.0: nav link - Bluetooth service for the can2gauge Link phone app (if switched on; before WiFi
                    // so Bluetooth gets its internal RAM). 0.30.6: before setupELM, so Bluetooth is up before the
                    // OBD task starts (both starting it at once crashed the gauge at boot)
  setupELM();       // BLE ELM327 task (idle unless enabled in the web UI)
  setupLogger();    // data logger task: records the chosen values into PSRAM for the web UI's Log tab
  printInternalRam("CAN / K-line / chain / ELM tasks");
  setupWiFi();      // AP (+ optional STA bridge)
  printInternalRam("WiFi");
  setupWebServer(); // JSON API + OTA (shared) + the web UI or the recovery page
  setupConsole();   // 0.17.3: USB bench console - a PC script asks the same API over the USB cable
  setupTasks();     // io poll + gauge refresh + vehicle logic + housekeeping (last, once the above are up)
  printInternalRam("setup (the web server's own task adds ~12 KB at the first page)");
  DEBUG("can2gauge up - http://%s.local", settings.hostname);
}

void loop()
{
  // All work runs in the FreeRTOS tasks started by setup(), so the Arduino task that ran setup() and calls loop() has
  // nothing left to do. Ending it hands its 8 KB stack back to internal RAM (WiFi, the web UI and Bluetooth need it).
  vTaskDelete(NULL); // NULL = "this task": loop() never runs again
}
