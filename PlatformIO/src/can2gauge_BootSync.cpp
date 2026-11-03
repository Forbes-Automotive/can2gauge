// Boot sync - the row's boot logos start together, and Auto CAN termination. See can2gauge_BootSync.h for how the
// gauges agree and the frame.

#include <can2gauge_BootSync.h>
#include <CANLine.h>         // canSend / canOnFrame / canStats / canIsListenOnly
#include <GaugeUI.h>         // uiBootLogoClock / uiBootLogoCatchUp / uiBootLogoRestart
#include <Display_ST7701.h>  // displaySetBacklight
#include <can2gauge_IO.h>    // backlightTarget / canTerminationApply
#include <can2gauge_Power.h> // powerCarFrameSeen

// ===== Shared with other files =====
uint8_t bootGaugeCount = 0; // gauges on CAN at the last power-up, this one included (read / saved by the EEP code)

// ===== Variables for this file =====
// static = private to this file. volatile = written by the CAN receive task and read by the boot sync task (or the io
// task): each read fetches the latest value.
static volatile uint32_t goStartMs = 0;           // when every gauge starts its logo, millis() here (0 = no GO yet)
static volatile uint32_t earliestLogoStartMs = 0; // the earliest logo heard running, millis() here (0 = none)
static uint32_t preparedForMs = 0;                // the start the logo was last set up for (0 = not yet)
static volatile uint16_t framesHeard = 0;         // boot sync frames heard from the other gauges (for the boot log)
static uint16_t myId = 0;                         // this gauge's id: the last 4 hex of its MAC, as on the end of its WiFi name
static uint16_t heardIds[BOOT_SYNC_MAX_GAUGES];   // every other gauge heard in a boot sync frame (under idsMux)
static uint8_t heardCount = 0;                    // how many gauges are in heardIds (under idsMux)
static volatile bool isHoldingDark = false;       // the screen is kept dark while the gauges wait for each other
static volatile bool isAutoTermOn = false;        // Auto termination switched our 120R on (silent bus at power-up)
static bool isTermDeciding = false;               // Auto termination is set: listen first (the boot sync task)
static bool isLogoSyncOn = false;                 // the boot logo handshake / catch-up runs this boot
static bool isReady[BOOT_SYNC_MAX_GAUGES];        // whether each gauge in heardIds said READY (under idsMux)
static portMUX_TYPE idsMux = portMUX_INITIALIZER_UNLOCKED; // the lock the CAN receive task and the boot sync task take turns with for heardIds / isReady / heardCount

// ===== Functions in this file =====
static void bootSyncTask(void *parameter);                                // Auto termination, then the logo handshake and catch-up
static void decideTermination();                                          // Auto termination: listen, then 120R on if nothing is heard
static void holdUntilAllReady();                                          // steps 1-3: screen dark until every gauge is ready
static void startLogo(const char *why, uint32_t startAtMs);               // the logo from the beginning and the backlight on at startAtMs
static void catchUp();                                                    // step 4: jump our logo forward to the earliest one heard
static void bootSyncOnFrame(const twai_message_t &frame);                 // a boot sync frame heard (CAN receive task)
static void noteGauge(uint16_t id, bool saidReady);                       // a gauge heard, and whether it said READY
static uint8_t readyGauges();                                             // how many gauges have said READY, this one included
static void sendBootSync(uint8_t type, uint32_t value, uint8_t expected); // one boot sync frame out
static uint16_t makeMyId();                                               // this gauge's id from its MAC

// decides at power-up whether to hold the screen dark for the logo handshake and / or to listen for Auto termination,
// and starts the boot sync task if either is on (after powerWakeProbe(), before setupDisplay())
void setupBootSync()
{
  const bool isCanSending = canStats().up && !canIsListenOnly(); // CAN running and allowed to send (not a wake check)
  isLogoSyncOn = settings.chainEnabled && settings.bootLogo && isCanSending && !runtime.wokeFromSleep;
  // Auto termination - decided at power-up. A wake from deep sleep happens on the car's own bus traffic, so that bus is
  // terminated already: left off.
  isTermDeciding = settings.canTermination == CAN_TERM_AUTO && isCanSending && !runtime.wokeFromSleep;
  if (!isLogoSyncOn && !isTermDeciding)
  {
    DEBUG("BOOT SYNC: off (chain %d, boot logo %d, CAN up %d, woken %d)", settings.chainEnabled, settings.bootLogo,
          canStats().up, runtime.wokeFromSleep);
    return; // no chain, no boot logo, no CAN, or a wake from deep sleep (the wake clip): nothing to line up
  }
  DEBUG("Setting up boot sync...");
  myId = makeMyId();
  if (isLogoSyncOn)
  {
    isHoldingDark = true; // from now on backlightTarget() gives 0, so setupDisplay leaves the screen dark
  }
  canOnFrame(bootSyncOnFrame);
  xTaskCreatePinnedToCore(bootSyncTask, // the function the task runs
                          "bootSync",   // its name (shows in crash reports)
                          8192,         // stack size in bytes: uiBootLogoRestart draws the logo's first frame in this
                                        // task (3 KB overflowed: boot loop, 2026-10-31). The task ends a few seconds
                                        // after power-on, so the RAM comes back.
                          NULL,         // nothing passed in
                          3,            // priority: above the house jobs, below CAN receive
                          NULL,         // no handle kept (it ends by itself)
                          0);           // core 0
  DEBUG("Set up boot sync!");
}

// true while waiting for the other gauges (backlightTarget() then gives 0)
bool bootSyncHoldingDark()
{
  return isHoldingDark;
}

// true when Auto termination has switched this gauge's 120R on
bool bootSyncAutoTermOn()
{
  return isAutoTermOn;
}

// io task: Auto switched the 120R on (silent bus at power-up), but car frames have turned up since - the car's bus woke
// after the gauge, and it is terminated already: off again for the rest of the drive
void bootSyncTermTick()
{
  if (!isAutoTermOn || settings.canTermination != CAN_TERM_AUTO)
  {
    return;
  }
  if (powerCarFrameSeen())
  {
    isAutoTermOn = false;
    canTerminationApply(false);
    DEBUG("CAN TERM: car frames heard - 120R off again (the car's bus is terminated)");
  }
}

// the boot sync task: Auto termination first, then the logo handshake and catch-up; it ends itself when done
static void bootSyncTask(void *parameter)
{
  (void)parameter;
  if (isTermDeciding)
  {
    decideTermination(); // first, so the handshake's frames have a terminated bus
  }
  if (!isLogoSyncOn)
  {
    vTaskDelete(NULL); // termination only (no chain or no boot logo): done
    return;
  }
  if (isHoldingDark)
  {
    holdUntilAllReady();
  }
  catchUp();
  // learn how many gauges to wait for next time: the ones heard this boot, this one included
  portENTER_CRITICAL(&idsMux);
  const uint8_t gaugesHeard = heardCount + 1;
  portEXIT_CRITICAL(&idsMux);
  if (gaugesHeard != bootGaugeCount)
  {
    DEBUG("BOOT SYNC: %u gauges on CAN (was %u) - saved for next time", (unsigned)gaugesHeard, (unsigned)bootGaugeCount);
    bootGaugeCount = gaugesHeard;
    settingsDirty = true; // saved by eepTick
  }
  DEBUG("BOOT SYNC: done - %u frames heard from the other gauges", (unsigned)framesHeard);
  vTaskDelete(NULL); // NULL = this task: it ends here
}

// ===== Detail functions =====

// Auto termination - listen for any CAN frame; none heard = this gauge switches its 120R on (see the header)
static void decideTermination()
{
  const uint32_t listenMs = CAN_TERM_LISTEN_MS + (myId & 7) * CAN_TERM_STAGGER_MS; // (myId & 7) = 0..7, per gauge
  const uint32_t startMs = millis();
  while (millis() - startMs < listenMs)
  {
    if (canStats().lastRxMs != 0)
    {
      DEBUG("CAN TERM: frames heard - the bus is terminated already, 120R left off");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  isAutoTermOn = true;
  canTerminationApply(true);
  DEBUG("CAN TERM: nothing heard in %lu ms - 120R on (Auto)", (unsigned long)listenMs);
  if (isLogoSyncOn)
  {
    return; // the handshake goes on telling the others (TERM while the logo is built, then READY / GO)
  }
  const uint32_t announceStartMs = millis();
  while (millis() - announceStartMs < CAN_TERM_ANNOUNCE_MS)
  {
    sendBootSync(BOOT_SYNC_TERM, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(CAN_TERM_ANNOUNCE_EVERY_MS));
  }
}

// steps 1-3: hold the screen dark until every gauge is ready, then start together
static void holdUntilAllReady()
{
  uint8_t expected = bootGaugeCount;
  if (expected < 1)
  {
    expected = 1; // not known yet: just this one
  }
  // wait for our own logo to be built
  const uint32_t waitStartMs = millis();
  uint32_t logoMs = 0;
  uint32_t lastTermMs = 0;
  while (!uiBootLogoClock(logoMs))
  {
    if (isAutoTermOn && millis() - lastTermMs >= CAN_TERM_ANNOUNCE_EVERY_MS)
    {
      sendBootSync(BOOT_SYNC_TERM, 0, 0); // we terminated the bus - tell the gauges still listening
      lastTermMs = millis();
    }
    if (millis() - waitStartMs > BOOT_LOGO_WAIT_MS)
    {
      isHoldingDark = false; // no logo after all: never leave the screen dark
      displaySetBacklight(backlightTarget());
      DEBUG("BOOT SYNC: no boot logo came up - backlight on");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  const uint32_t holdStartMs = millis();
  while (1)
  {
    const uint32_t now = millis();
    if (earliestLogoStartMs != 0)
    {
      startLogo("the others had started", now); // missed the GO: start now, the catch-up lines it up
      return;
    }
    if (goStartMs == 0 && readyGauges() >= expected)
    {
      goStartMs = (now + BOOT_SYNC_COUNTDOWN_MS) | 1; // everyone's here: start the count-down
    }
    const uint32_t startAtMs = goStartMs; // one read (the CAN receive task can move it earlier)
    if (startAtMs != 0)
    {
      const int32_t untilStartMs = (int32_t)(startAtMs - now);
      if (untilStartMs <= BOOT_SYNC_PREPARE_MS)
      {
        // nearly time: set the logo up for the start and draw its first frame (redone if an earlier GO comes in)
        if (preparedForMs != startAtMs)
        {
          uiBootLogoRestart(startAtMs);
          preparedForMs = startAtMs;
        }
      }
      if (untilStartMs <= BOOT_SYNC_HOLD_EVERY_MS)
      {
        startLogo("GO", startAtMs); // waits the last few ms, then the backlight
        return;
      }
      sendBootSync(BOOT_SYNC_GO, (uint32_t)untilStartMs, expected); // the count-down, so the others start with us
    }
    else
    {
      sendBootSync(BOOT_SYNC_READY, 0, expected);
      if (now - holdStartMs > BOOT_HOLD_MAX_MS)
      {
        goStartMs = (now + BOOT_SYNC_COUNTDOWN_MS) | 1; // a gauge is missing: don't keep the rest dark - count down
        DEBUG("BOOT SYNC: only %u of %u gauges ready - starting anyway", (unsigned)readyGauges(), (unsigned)expected);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(BOOT_SYNC_HOLD_EVERY_MS));
  }
}

// light the screen at startAtMs (millis()): the logo starts from the beginning on every gauge together. Its first
// frame is drawn ahead of time (uiBootLogoRestart, unless already done for this start), so only the backlight is left
// to do at the moment itself.
static void startLogo(const char *why, uint32_t startAtMs)
{
  if (preparedForMs != startAtMs)
  {
    uiBootLogoRestart(startAtMs);
    preparedForMs = startAtMs;
  }
  const int32_t untilStartMs = (int32_t)(startAtMs - millis());
  if (untilStartMs > 0)
  {
    vTaskDelay(pdMS_TO_TICKS(untilStartMs)); // to the agreed moment
  }
  isHoldingDark = false;
  displaySetBacklight(backlightTarget());
  DEBUG("BOOT SYNC: logo started (%s), %u of %u gauges ready", why, (unsigned)readyGauges(), (unsigned)bootGaugeCount);
}

// step 4: send our logo time every 20 ms and jump our logo forward to the earliest one heard
static void catchUp()
{
  while (1)
  {
    uint32_t myLogoMs = 0;
    if (!uiBootLogoClock(myLogoMs) || myLogoMs > BOOT_SYNC_WINDOW_MS)
    {
      return; // the logo has gone, or we're past the window
    }
    const uint32_t heardStartMs = earliestLogoStartMs;
    if (heardStartMs != 0)
    {
      const uint32_t theirLogoMs = millis() - heardStartMs;
      if (theirLogoMs > myLogoMs + BOOT_SYNC_SLACK_MS)
      {
        DEBUG("BOOT SYNC: %lu ms behind another gauge - jumping the logo forward", (unsigned long)(theirLogoMs - myLogoMs));
        uiBootLogoCatchUp(heardStartMs); // works out the point itself once it has the screen
        uiBootLogoClock(myLogoMs);       // where it is now, to send below
      }
    }
    sendBootSync(BOOT_SYNC_LOGO_TIME, myLogoMs, 0);
    vTaskDelay(pdMS_TO_TICKS(BOOT_SYNC_EVERY_MS));
  }
}

// runs in the CAN receive task for every frame - kept short
static void bootSyncOnFrame(const twai_message_t &frame)
{
  if (frame.extd || frame.identifier != (uint32_t)settings.linkCanBaseId + BOOT_SYNC_ID_OFFSET)
  {
    return; // not a boot sync frame
  }
  if (frame.data_length_code < 5)
  {
    return;
  }
  const uint16_t group = frame.data[0] | (frame.data[1] << 8); // bytes 0-1: the chain group
  if (group != settings.chainGroup)
  {
    return; // another chain
  }
  framesHeard++;
  const uint32_t now = millis();
  const uint32_t value = frame.data[2] | (frame.data[3] << 8); // bytes 2-3: logo ms / ms to the start
  const uint8_t type = frame.data[4];                          // byte 4: what kind of frame
  uint16_t id = 0;
  if (frame.data_length_code >= 7)
  {
    id = (frame.data[5] << 8) | frame.data[6]; // bytes 5-6: the sender's id
  }
  if (type == BOOT_SYNC_READY)
  {
    noteGauge(id, true);
  }
  else if (type == BOOT_SYNC_GO)
  {
    noteGauge(id, true); // a gauge saying GO is ready too
    const uint32_t startMs = (now + value) | 1; // | 1: never 0, which means "no GO yet"
    if (goStartMs == 0 || (int32_t)(startMs - goStartMs) < 0)
    {
      goStartMs = startMs; // the earliest GO heard wins
    }
  }
  else if (type == BOOT_SYNC_TERM)
  {
    noteGauge(id, false); // that gauge terminated the bus (it counts as heard, like any frame)
  }
  else if (type == BOOT_SYNC_LOGO_TIME)
  {
    noteGauge(id, false);
    const uint32_t theirStartMs = now - value; // when its logo went up, on our clock
    if (earliestLogoStartMs == 0 || (int32_t)(theirStartMs - earliestLogoStartMs) < 0)
    {
      earliestLogoStartMs = theirStartMs | 1;
    }
  }
}

// note a gauge heard (and whether it said READY). Runs in the CAN receive task.
static void noteGauge(uint16_t id, bool saidReady)
{
  if (id == 0 || id == myId)
  {
    return; // an older gauge that sends no id, or our own frame
  }
  portENTER_CRITICAL(&idsMux);
  int8_t found = -1;
  for (uint8_t i = 0; i < heardCount; i++)
  {
    if (heardIds[i] == id)
    {
      found = i;
    }
  }
  if (found < 0 && heardCount < BOOT_SYNC_MAX_GAUGES)
  {
    found = heardCount;
    heardIds[heardCount] = id;
    isReady[heardCount] = false;
    heardCount++;
  }
  if (found >= 0 && saidReady)
  {
    isReady[found] = true;
  }
  portEXIT_CRITICAL(&idsMux);
}

// how many gauges have said READY, this one included
static uint8_t readyGauges()
{
  uint8_t count = 1; // this one
  portENTER_CRITICAL(&idsMux);
  for (uint8_t i = 0; i < heardCount; i++)
  {
    if (isReady[i])
    {
      count++;
    }
  }
  portEXIT_CRITICAL(&idsMux);
  return count;
}

// one boot sync frame out (sent whatever settings.linkCanTx says - see the header)
static void sendBootSync(uint8_t type, uint32_t value, uint8_t expected)
{
  uint8_t data[8];
  data[0] = settings.chainGroup & 0xFF; // chain group low byte
  data[1] = settings.chainGroup >> 8;   // chain group high byte
  data[2] = value & 0xFF;               // logo ms / ms to the start, low byte
  data[3] = (value >> 8) & 0xFF;        // ... high byte
  data[4] = type;                       // BOOT_SYNC_LOGO_TIME / READY / GO / TERM
  data[5] = myId >> 8;                  // our id, high byte
  data[6] = myId & 0xFF;                // our id, low byte
  data[7] = expected;                   // READY: how many gauges we wait for
  canSend(settings.linkCanBaseId + BOOT_SYNC_ID_OFFSET, data, 8);
}

// this gauge's id (the same 2 bytes as on the end of its WiFi name, can2gauge-XXYY)
static uint16_t makeMyId()
{
  const uint64_t mac = ESP.getEfuseMac(); // byte 0 = first octet
  const uint8_t macByte4 = (mac >> 32) & 0xFF;
  const uint8_t macByte5 = (mac >> 40) & 0xFF;
  return (macByte4 << 8) | macByte5;
}
