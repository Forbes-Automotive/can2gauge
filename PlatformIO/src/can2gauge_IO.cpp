// The gauge's I/O (board V3.4): the I2C bus and TCA9554 expander, the DRV8876 output, the dash-light input, the
// output's fault pin, the CAN termination FET and the CAN bus itself.
#include <can2gauge_IO.h>
#include <can2gauge_Link.h>
#include <can2gauge_Power.h>
#include <can2gauge_BootSync.h> // the screen stays dark while the gauges wait for each other at power-up
#include <IOX.h>
#include <TCA9554.h>
#include <LVGL_Driver.h>
#include <GaugeUI.h>
#include <CANLine.h>
#include <Wire.h>

// ===== Variables for this file =====
static uint8_t lightsChangeCount = 0;   // polls in a row the dash-light input has read differently (debounce)
static uint8_t faultChangeCount = 0;    // polls in a row the output's fault pin has read differently (debounce)
static bool triggerState[IOX_CHANNELS]; // each output as the triggers last asked for it (used while it's on auto)
static bool lightsOn = false;           // the dash lights are on (debounced, after the invert setting)
static bool nightMode = false;          // night dimming is active
static bool outputFault = false;        // the output's fault pin (debounced): over-current / over-temperature
static bool isTerminationOnNow = false; // the 120R is in circuit right now (Auto can change it)

// Brings up every I/O bus the rest of the firmware needs: the I2C bus + TCA9554 expander (the screen's reset / CS
// lines hang off it, so this MUST run before setupDisplay()), the DRV8876 output, the CAN termination FET and the CAN
// bus itself. No poll task here: ioTick() runs from the io task in setupTasks(), started once the screen is up
// (ioTick() drives the backlight). This board has no ADC (the 0-5 V front ends came off at board V3.4).
void setupIO()
{
  // I2C bus + expander first
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_HZ);
  uint8_t exioDefaults = 0x0F; // P0-P3 high: LCD_RST / TP_RST / LCD_CS released, CAN_TERM_EN high (= off); P4/P5 low = DRV8876 coast
  if (!tca9554Init(TCA9554_ADDR, exioDefaults, EXIO_INPUT_MASK)) // P6 dash lights, P7 output fault = inputs
  {
    DEBUG("TCA9554 NOT found - display reset/CS + output unavailable");
  }

  // output (DRV8876 through the expander) + CAN termination FET
  if (ioxInit(TCA9554_ADDR))
  {
    DEBUG("IO: output on TCA9554 ok");
  }
  else
  {
    DEBUG("IO: TCA9554 NOT found @0x%02X - output disabled", TCA9554_ADDR);
  }
  memset(triggerState, 0, sizeof(triggerState));
  outputsApply();
  canTerminationFromSettings(); // Off / On / Auto (Auto starts off: the boot sync decides)

  // CAN bus
  if (settings.canEnabled)
  {
    if (canBegin(CAN_PIN_TX, CAN_PIN_RX, settings.canKbps))
    {
      DEBUG("CAN up at %u kbps", settings.canKbps);
    }
    else
    {
      DEBUG("CAN init FAILED");
    }
  }
  canSetProfile(settings.canProfile);
  canSetMaxxBase(settings.maxxBaseId);
  canSetHaldexIds(settings.haldexBroadcastId, settings.haldexControlId);
}

// one poll (every ioRefresh ms, from the io task): the dash-light input and night dimming, the output's fault pin, and
// the ignition on permanent power
void ioTick()
{
  bootSyncTermTick(); // Auto CAN termination: off again if car frames turn up
  // dash lights: the hardware already holds the 58d PWM peak (~1 s), so 3 equal polls in a row are enough
  bool lightsRaw = ioxDashLightsRaw(); // the input's level: true = high
  if (settings.dimInvert != 0)
  {
    lightsRaw = !lightsRaw; // inverted: the input LOW means the lights are on
  }
  if (lightsRaw != lightsOn)
  {
    lightsChangeCount++;
    if (lightsChangeCount >= 3)
    {
      lightsOn = lightsRaw;
      lightsChangeCount = 0;
      const char *lightsText = "off";
      if (lightsOn)
      {
        lightsText = "ON";
      }
      DEBUG("IO: dash lights %s", lightsText);
    }
  }
  else
  {
    lightsChangeCount = 0;
  }
  // the dash lights as a gauge value: 1 = on, 0 = off
  if (lightsOn)
  {
    signalSet(SIG_DASH_LIGHTS, 1);
  }
  else
  {
    signalSet(SIG_DASH_LIGHTS, 0);
  }

  // with the illumination input wired as the ignition it says nothing about the dash lights: no night dimming from it
  const bool wireIsIgnition = settings.ignSource == IGN_SRC_WIRE;
  bool wantNight = settings.nightSource == 1 && lightsOn && !wireIsIgnition;
  bool masterNight;
  if (settings.nightSource == 1 && settings.followNight && linkMasterNight(masterNight))
  {
    wantNight = masterNight; // gauge chain: only one gauge needs the dash-light wire
  }
  if (wantNight != nightMode)
  {
    nightMode = wantNight;
    uiSetNight(nightMode);
    if (!runtime.standby)
    {
      displaySetBacklight(backlightTarget());
    }
  }

  // output fault pin: 2 polls in a row (100 ms apart) before it counts, either way. One read on its own showed a fault
  // when Bluetooth started (2026-10-14: its radio calibration draws a burst of current, a brief dip on the pin); a
  // real short or overheat holds the pin, so it still shows within 200 ms.
  const bool faultNow = ioxFault();
  if (faultNow != outputFault)
  {
    faultChangeCount++;
    if (faultChangeCount >= 2)
    {
      outputFault = faultNow;
      faultChangeCount = 0;
      const char *faultText = "fault cleared";
      if (outputFault)
      {
        faultText = "FAULT (over-current / over-temperature)";
      }
      DEBUG("IO: output %s", faultText);
    }
  }
  else
  {
    faultChangeCount = 0;
  }
  // the output's fault and the output itself as gauge values: 1 = yes / on, 0 = no / off
  if (outputFault)
  {
    signalSet(SIG_OUT_FAULT, 1);
  }
  else
  {
    signalSet(SIG_OUT_FAULT, 0);
  }
  if (ioxGet(IOX_OUT))
  {
    signalSet(SIG_OUTPUT, 1);
  }
  else
  {
    signalSet(SIG_OUTPUT, 0);
  }
  powerTick(); // ignition on permanent power: shutdown / standby / deep sleep
}

// switches an output as its manual setting says (settings.outputManual: 0 off, 1 on, 2 auto = as the triggers want).
// fromTrigger = a trigger is asking (remembered for when the output is on auto).
void outputRequest(uint8_t channel, bool on, bool fromTrigger)
{
  if (channel >= IOX_CHANNELS)
  {
    return;
  }
  if (fromTrigger)
  {
    triggerState[channel] = on;
  }
  const uint8_t mode = settings.outputManual[channel];
  bool level;
  if (mode == 1)
  {
    level = true;
  }
  else if (mode == 0)
  {
    level = false;
  }
  else
  {
    level = triggerState[channel];
  }
  if (runtime.standby)
  {
    level = false; // ignition off: nothing switched on from the battery
  }
  if (ioxGet(channel) != level)
  {
    ioxSet(channel, level);
#if detailedDebugIO
    const char *modeText = "manual";
    if (mode == 2)
    {
      modeText = "auto";
    }
    DEBUG("IO: %s -> %d (%s)", ioxChannelName(channel), level, modeText);
#endif
  }
}

// after a settings change: the output's high- / low-side mode, every output again (manual or trigger), and the
// backlight
void outputsApply()
{
  ioxSetMode(settings.outputMode == 1);
  for (uint8_t channel = 0; channel < IOX_CHANNELS; channel++)
  {
    outputRequest(channel, triggerState[channel], true);
  }
  if (!runtime.standby)
  {
    displaySetBacklight(backlightTarget());
  }
}

// the backlight level the screen should have now: dark while the gauges line up their boot logos, else the day or the
// night brightness
uint8_t backlightTarget()
{
  if (bootSyncHoldingDark())
  {
    return 0; // power-up, waiting for the other gauges so the boot logos start together
  }
  if (nightMode)
  {
    return settings.nightBrightness;
  }
  return settings.brightness;
}

// the dash lights are on (debounced, after the invert setting)
bool dashLightsOn()
{
  return lightsOn;
}

// night dimming is active (the dash lights on and night dimming switched on, or the chain master's night)
bool nightActive()
{
  return nightMode;
}

// the 120R as the setting says: Off, On, or Auto - on only while the boot sync has switched it on (the bus was silent
// at power-up and no car frames have been heard since)
void canTerminationFromSettings()
{
  if (settings.canTermination == CAN_TERM_ON)
  {
    canTerminationApply(true);
  }
  else if (settings.canTermination == CAN_TERM_AUTO)
  {
    canTerminationApply(bootSyncAutoTermOn());
  }
  else
  {
    canTerminationApply(false);
  }
}

// puts the 120R in circuit (true) or takes it out (false)
void canTerminationApply(bool enabled)
{
  isTerminationOnNow = enabled;
  // Q_TERM gate: LOW = 120R in circuit. TCA9554 P3 (EXIO4)
  tca9554Write(EXIO_CAN_TERM_EN, !enabled);
  const char *termText = "off";
  if (enabled)
  {
    termText = "ON (120R)";
  }
  DEBUG("CAN termination %s", termText);
}

// is the 120R in circuit right now? (with Auto, the setting alone doesn't say)
bool canTerminationIsOn()
{
  return isTerminationOnNow;
}

// the text of the menu's output / dash lights info page: the output, what controls it, the dash-light input and night
// dimming
void ioInfoProvider(char *buf, size_t len)
{
  size_t written = 0;
  const char *sideText = "high-side";
  if (settings.outputMode == 1)
  {
    sideText = "low-side";
  }
  const char *outputText = "off";
  if (ioxGet(IOX_OUT))
  {
    outputText = "ON";
  }
  const char *faultText = "";
  if (outputFault)
  {
    faultText = "  FAULT";
  }
  appendf(buf, len, written, "Output (J_OUT 1): %s, %s%s\n", sideText, outputText, faultText);
  const char *controlText;
  if (settings.outputManual[IOX_OUT] == 2)
  {
    controlText = "triggers";
  }
  else if (settings.outputManual[IOX_OUT])
  {
    controlText = "manual on";
  }
  else
  {
    controlText = "manual off";
  }
  appendf(buf, len, written, "Control: %s\n", controlText);
  const char *lightsText = "off";
  if (lightsOn)
  {
    lightsText = "ON";
  }
  appendf(buf, len, written, "\nDash lights (J_OUT 3): %s\n", lightsText);
  if (written < len)
  {
    const char *nightText;
    if (settings.ignSource == IGN_SRC_WIRE)
    {
      nightText = "off (input = ignition)";
    }
    else if (settings.nightSource == 1)
    {
      if (nightMode)
      {
        nightText = "active";
      }
      else
      {
        nightText = "armed";
      }
    }
    else
    {
      nightText = "off";
    }
    appendf(buf, len, written, "Night dimming: %s", nightText);
  }
}
