#include <can2gauge_Power.h>
#include <can2gauge_IO.h>
#include <can2gauge_EEP.h>
#include <can2gauge_KLine.h>
#include <can2gauge_DSG.h>
#include <can2gauge_VagDiag.h> // vdiagRestart() at standby
#include <can2gauge_Link.h> // the row wave: our place among the gauges in the row
#include <can2gauge_ChainUpdate.h> // 0.25.0: stay awake through a chain update
#include <GaugeUI.h>
#include <CANLine.h>
#include <IOX.h>
#include <Display_ST7701.h>
#include <ota_manager.h> // otaInProgress / otaWebClientActive
#include <WiFi.h>        // AP stations
#include <esp_sleep.h>
#include <driver/gpio.h>

enum powerState_t : uint8_t
{
  PWR_ON = 0,
  PWR_SHUTDOWN, // the shutdown sprite is playing
  PWR_STANDBY,  // dark, waiting for the ignition (or deep sleep)
};
static powerState_t state = PWR_ON;
static uint32_t stateEnteredMs = 0;     // millis() the state was entered
static uint32_t offSinceMs = 0;         // ignition off since (0 = on)
static uint32_t shutdownDurationMs = 0; // how long the shutdown sprite plays
static volatile bool testShutdownRunning = false; // powerTestShutdown: the sprites only, no standby

// terminal 15 / bus traffic as the CAN hook sees it (RX task)
static volatile uint32_t terminal15Ms = 0; // last frame carrying terminal 15 (0 = none yet)
static volatile bool terminal15 = false;
static volatile uint32_t trafficMs = 0;    // last car frame (11-bit, not the gauge chain's)
static volatile uint32_t chainIgnitionMs = 0; // 0.29.2: last chain heartbeat on CAN from a gauge whose own ignition is on

static void canHook(const twai_message_t &frame)
{
  const uint32_t now = millis() | 1;
  const uint32_t id = frame.identifier;
  const bool isChainFrame = !frame.extd && id >= settings.linkCanBaseId && id < settings.linkCanBaseId + 0x30u;
  if (!frame.extd && !isChainFrame)
  {
    trafficMs = now;
  }
  // 0.29.2: a chain heartbeat (base + 0x10 to base + 0x1F) in our group, with byte 2 bit 6 set = that gauge has its
  // own ignition and it is on. Watched here as well as in the link so a gauge woken from deep sleep can check it
  // before the chain is running.
  const bool isChainHeartbeat = isChainFrame && id >= settings.linkCanBaseId + 0x10u && id < settings.linkCanBaseId + 0x20u;
  if (isChainHeartbeat && frame.data_length_code >= 8)
  {
    const uint16_t group = frame.data[0] | (frame.data[1] << 8); // bytes 0-1: the chain group
    if (group == settings.chainGroup && (frame.data[2] & 0x40)) // byte 2 bit 6: its ignition is on
    {
      chainIgnitionMs = now;
    }
  }
  switch (settings.ignCanMode)
  {
  case IGN_CAN_PQ: // BSG_Last 0x570 ZAS_Klemme_15 / ZAS_1 0x572 Klemme_15: byte 0 bit 1
    if (!frame.extd && (id == 0x570 || id == 0x572) && frame.data_length_code >= 1)
    {
      terminal15 = frame.data[0] & 0x02;
      terminal15Ms = now;
    }
    break;
  case IGN_CAN_MQB: // Klemmen_Status_01 0x3C0 ZAS_Kl_15: start bit 17 = byte 2 bit 1
    if (!frame.extd && id == 0x3C0 && frame.data_length_code >= 3)
    {
      terminal15 = frame.data[2] & 0x02;
      terminal15Ms = now;
    }
    break;
  case IGN_CAN_CUSTOM:
  {
    const bool wantExt = settings.ignCanId & 0x80000000UL;
    const uint8_t byteIndex = settings.ignCanBit >> 3;
    if (frame.extd == wantExt && id == (settings.ignCanId & 0x1FFFFFFFUL) && byteIndex < frame.data_length_code)
    {
      terminal15 = (((frame.data[byteIndex] >> (settings.ignCanBit & 7)) & 1) != 0) != (settings.ignCanInvert != 0);
      terminal15Ms = now;
    }
    break;
  }
  default:
    break;
  }
}

static bool canIgnition()
{
  if (!canStats().up)
  {
    return true; // CAN is off: nothing to go on, so never shut down on it (the web UI says so)
  }
  const uint32_t now = millis();
  if (settings.ignCanMode == IGN_CAN_TRAFFIC)
  {
    return trafficMs && now - trafficMs < 3000;
  }
  return terminal15Ms && now - terminal15Ms < 2000 && terminal15; // the frame stopping (bus asleep) counts as off
}

// 0.29.2: "Ignition from another gauge": on while a gauge with its own ignition says it is on (over CAN or WiFi)
static bool chainIgnition()
{
  if (!settings.chainEnabled)
  {
    return true; // the chain is off: nothing to go on, so never shut down on it (the web UI says so)
  }
  const uint32_t now = millis();
  if (chainIgnitionMs && now - chainIgnitionMs < 3000)
  {
    return true; // heard on CAN
  }
  char whoName[NAME_LEN];
  return linkIgnitionFrom(whoName, sizeof(whoName)); // heard over WiFi (or CAN, through the link)
}

bool ignitionOn()
{
  switch (settings.ignSource)
  {
  case IGN_SRC_CAN: return canIgnition();
  case IGN_SRC_WIRE: return dashLightsOn(); // debounced, after the input's invert setting
  case IGN_SRC_CHAIN: return chainIgnition();
  default: return true;
  }
}

// 0.29.2: a gauge following another gauge's ignition can only wake from deep sleep on that gauge's CAN heartbeat, so
// with the chain on WiFi only it stays in standby (dark) instead, listening over WiFi
static bool canDeepSleep()
{
  if (settings.ignSource != IGN_SRC_CHAIN)
  {
    return true;
  }
  return settings.linkTransport != LINK_ESPNOW && settings.linkTransport != LINK_OFF;
}

// 0.30.15: has a car frame (11-bit, not the gauge chain's own) been heard since power-up? (Auto CAN termination)
bool powerCarFrameSeen()
{
  return trafficMs != 0;
}

static bool busQuiet(uint32_t quietMs)
{
  const uint32_t lastRxMs = canStats().lastRxMs;
  return !lastRxMs || millis() - lastRxMs >= quietMs;
}

// the shared power_manager rule (ota_manager.h): the radio stays up while someone is on the web UI or an update runs
static bool powerIsBusy()
{
  return WiFi.softAPgetStationNum() > 0 || otaInProgress() || otaWebClientActive() || chainUpdateBusy();
}

// never returns. CAN ignition: wake on the first bit on the bus (or after timerS, if given); wire: every POWER_WIRE_POLL_S
static void deepSleep(uint32_t timerS = 0)
{
#if enableDebug // the wake reason is only used by the DEBUG line
  const char *wakeText;
  if (settings.ignSource == IGN_SRC_WIRE)
  {
    wakeText = "checking the wire every few s";
  }
  else if (timerS)
  {
    wakeText = "timer";
  }
  else
  {
    wakeText = "wake on CAN";
  }
  DEBUG("power: deep sleep (%s)", wakeText);
#endif
  if (settingsDirty)
  {
    writeEEP();
  }
  ioxSet(IOX_OUT, false); // DRV8876 coast (the TCA9554 holds it through the sleep)
  displaySetBacklight(0);
  canEnd();
  // hold the pins that matter through the sleep: a floating AL8860 CTRL is full brightness, a floating CAN TXD could
  // drive the bus dominant. Released at the top of setup().
  ledcDetach(LCD_PIN_BL_PWM);
  pinMode(LCD_PIN_BL_PWM, OUTPUT);
  digitalWrite(LCD_PIN_BL_PWM, LOW);
  gpio_hold_en((gpio_num_t)LCD_PIN_BL_PWM);
  pinMode(CAN_PIN_TX, OUTPUT);
  digitalWrite(CAN_PIN_TX, HIGH); // recessive
  gpio_hold_en((gpio_num_t)CAN_PIN_TX);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON); // the pad holds and the ext0 input
  if (settings.ignSource == IGN_SRC_WIRE)
  {
    esp_sleep_enable_timer_wakeup((uint64_t)POWER_WIRE_POLL_S * 1000000ULL);
  }
  else if (timerS)
  {
    esp_sleep_enable_timer_wakeup((uint64_t)timerS * 1000000ULL);
  }
  else
  {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)CAN_PIN_RX, 0); // RXD low = a dominant bit
  }
#if enableDebug
  Serial.flush();
#endif
  esp_deep_sleep_start();
}

void setupPower()
{
  canOnFrame(canHook);
}

// woken from deep sleep: the glass is still dark (setup() has not reached the display). Ignition on -> carry on booting;
// still off -> back to sleep. CAN: a sleeping car's bus wakes for other reasons too (unlocking, the comfort bus), so
// keep listening (listen-only, nothing sent) while it is busy and sleep once it has been quiet for POWER_BUS_QUIET_MS.
void powerWakeProbe(bool fromSleep)
{
  if (!fromSleep || settings.ignSource == IGN_SRC_OFF)
  {
    canSetListenOnly(false);
    return;
  }
  if (settings.ignSource == IGN_SRC_WIRE)
  {
    for (int i = 0; i < 3; i++) // the input's peak detector holds 58d PWM for ~1 s: three reads are plenty
    {
      if (ioxDashLightsRaw() != (settings.dimInvert != 0))
      {
        DEBUG("power: woken with the ignition on");
        runtime.wokeFromSleep = true;
        canSetListenOnly(false); // main.cpp started the bus listen-only for the check
        return;
      }
      delay(100);
    }
    deepSleep();
  }
  if (!canStats().up)
  {
    canBegin(CAN_PIN_TX, CAN_PIN_RX, settings.canKbps); // listen-only (main.cpp set it before setupIO)
  }
  const uint32_t probeStartMs = millis();
  for (;;)
  {
    bool isOn = false; // the ignition (or, following the chain, another gauge's ignition) is on
    if (settings.ignSource == IGN_SRC_CHAIN)
    {
      isOn = chainIgnitionMs && millis() - chainIgnitionMs < 3000; // the link isn't running yet: the CAN hook only
      if (!settings.chainEnabled)
      {
        isOn = true; // the chain was switched off: nothing to follow, so stay awake
      }
    }
    else
    {
      isOn = canIgnition();
    }
    if (isOn)
    {
      DEBUG("power: woken with the ignition on");
      runtime.wokeFromSleep = true;
      canSetListenOnly(false);
      if (!settings.canEnabled)
      {
        canEnd(); // the probe borrowed the bus
      }
      return;
    }
    const uint32_t probeMs = millis() - probeStartMs;
    if (probeMs > 1500 && busQuiet(POWER_BUS_QUIET_MS))
    {
      deepSleep();
    }
    if (probeMs > 30UL * 60000UL) // the bus has been busy for half an hour with the ignition off: look again every minute
    {
      deepSleep(60);
    }
    delay(50);
  }
}

// backlight from where it is to `target` over durationMs (the glass keeps animating: this runs on the io task)
static void rampBacklight(uint8_t target, uint32_t durationMs)
{
  const int from = displayGetBacklight();
  const int steps = durationMs / 30 > 0 ? durationMs / 30 : 1;
  for (int step = 1; step <= steps; step++)
  {
    displaySetBacklight((uint8_t)(from + (target - from) * step / steps));
    delay(30);
  }
}

static void enterStandby()
{
  rampBacklight(0, 1500); // docs/mascot/MASCOT.md: the sleep clip keeps looping while the glass fades out
  uiStandby(true);
  displaySleep(true);
  runtime.standby = true;
  canSetListenOnly(true); // watch for terminal 15 without keeping the car's bus awake
  ioxSet(IOX_OUT, false); // outputRequest keeps it off while in standby
  klineRestart();         // close any K-line session (the task idles in standby)
  dsgRestart();           // and any gearbox session
  vdiagRestart();         // and any VAG diag session
  state = PWR_STANDBY;
  stateEnteredMs = millis();
  DEBUG("power: standby");
}

static void leaveStandby()
{
  runtime.standby = false;
  canSetListenOnly(false);
  displaySleep(false);
  uiStandby(false); // the wake clip (if the pack has one), then the gauge page
  rampBacklight(backlightTarget(), 300);
  outputsApply();
  state = PWR_ON;
  stateEnteredMs = millis();
  DEBUG("power: ignition on - awake");
}

// the shutdown sprite, with the row wave first when this gauge is in a row (docs/SPRITES.md "A row of gauges"):
// how long it all plays
static uint32_t startShutdownSprite()
{
  uint8_t placeFromLeft = 0; // 0 = the leftmost gauge in the row
  uint8_t gaugesInRow = 1;   // 1 = on its own: the normal goodbye
  linkRowPlace(placeFromLeft, gaugesInRow);
  return uiShutdown(placeFromLeft, gaugesInRow);
}

void powerTestShutdown()
{
  if (state != PWR_ON || testShutdownRunning)
  {
    return;
  }
  shutdownDurationMs = startShutdownSprite();
  stateEnteredMs = millis();
  testShutdownRunning = true;
  state = PWR_SHUTDOWN;
}

void powerTick()
{
  const uint32_t now = millis();
  if (testShutdownRunning)
  {
    if (now - stateEnteredMs >= shutdownDurationMs + 600) // hold the last frame a moment, as the backlight fade would
    {
      uiStandby(false); // the wake sprite, then the page
      state = PWR_ON;
      stateEnteredMs = now;
      testShutdownRunning = false;
    }
    return;
  }

  if (settings.ignSource == IGN_SRC_OFF)
  {
    if (state == PWR_STANDBY)
    {
      leaveStandby(); // the source was switched off from the web UI while dark
    }
    else if (state == PWR_SHUTDOWN)
    {
      uiStandby(false);
      state = PWR_ON;
    }
    offSinceMs = 0;
    return;
  }

  const bool on = ignitionOn() || now < POWER_BOOT_GRACE_MS;
  if (on)
  {
    offSinceMs = 0;
  }
  else if (!offSinceMs)
  {
    offSinceMs = now | 1;
  }

  switch (state)
  {
  case PWR_ON:
    // never in the middle of an ECU flash read or an update
    if (offSinceMs && now - offSinceMs >= settings.ignOffDelayS * 1000UL && !otaInProgress() &&
        strcmp(runtime.klineState, "ECU read"))
    {
      DEBUG("power: ignition off - shutting down");
      shutdownDurationMs = startShutdownSprite();
      state = PWR_SHUTDOWN;
      stateEnteredMs = now;
    }
    break;
  case PWR_SHUTDOWN:
    if (on)
    { // back on before the glass went dark: straight back to the gauge
      uiStandby(false);
      state = PWR_ON;
      stateEnteredMs = now;
    }
    else if (now - stateEnteredMs >= shutdownDurationMs)
    {
      enterStandby();
    }
    break;
  case PWR_STANDBY:
    if (on)
    {
      leaveStandby();
    }
    else if (powerSleepInS() == 0 && now - stateEnteredMs >= settings.standbyMin * 60000UL && !powerIsBusy() &&
             canDeepSleep())
    {
      if (settings.ignSource == IGN_SRC_WIRE || busQuiet(POWER_BUS_QUIET_MS))
      {
        deepSleep();
      }
      else if (now - stateEnteredMs >= settings.standbyMin * 60000UL + 30UL * 60000UL)
      {
        deepSleep(60); // the bus never went quiet: look again every minute
      }
    }
    break;
  }
}

const char *powerState()
{
  if (settings.ignSource == IGN_SRC_OFF)
  {
    return "switched power";
  }
  static const char *stateNames[] = {"on", "shutdown", "standby"};
  return stateNames[state];
}

uint32_t powerSleepInS()
{
  if (state != PWR_STANDBY || settings.ignSource == IGN_SRC_OFF)
  {
    return 0;
  }
  const uint32_t elapsedMs = millis() - stateEnteredMs;
  const uint32_t dueMs = settings.standbyMin * 60000UL;
  return elapsedMs >= dueMs ? 0 : (dueMs - elapsedMs + 999) / 1000;
}

void powerDetail(char *buf, size_t len)
{
  const uint32_t now = millis();
  switch (settings.ignSource)
  {
  // 0.17.1: as few words as possible, Every Word Capitalised (the web UI's Settings > Ignition and Diag show it)
  case IGN_SRC_WIRE:
    snprintf(buf, len, "Illumination %s", dashLightsOn() ? "On" : "Off");
    break;
  case IGN_SRC_CAN:
    if (!canStats().up)
    {
      snprintf(buf, len, "CAN Off: Never Shuts Down");
    }
    else if (settings.ignCanMode == IGN_CAN_TRAFFIC)
    {
      snprintf(buf, len, trafficMs ? "Last Frame %lu ms Ago" : "No Frames Yet", (unsigned long)(now - trafficMs));
    }
    else if (!terminal15Ms)
    {
      snprintf(buf, len, "No Terminal 15 Frame Yet");
    }
    else
    {
      snprintf(buf, len, "Terminal 15 %s (%lu ms Ago)", terminal15 ? "On" : "Off", (unsigned long)(now - terminal15Ms));
    }
    break;
  case IGN_SRC_CHAIN:
  {
    char whoName[NAME_LEN];
    if (!settings.chainEnabled)
    {
      snprintf(buf, len, "Gauge Chain Off: Never Shuts Down");
    }
    else if (linkIgnitionFrom(whoName, sizeof(whoName)))
    {
      snprintf(buf, len, "On From %s", whoName);
    }
    else if (chainIgnitionMs && now - chainIgnitionMs < 3000)
    {
      snprintf(buf, len, "On From Another Gauge");
    }
    else
    {
      snprintf(buf, len, "No Gauge With Its Ignition On");
    }
    break;
  }
  default:
    snprintf(buf, len, "Following Supply");
    break;
  }
}
