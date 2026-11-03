#include <can2gauge_IO.h>
#include <can2gauge_Link.h>
#include <can2gauge_Power.h>
#include <can2gauge_BootSync.h> // 0.30.14: the glass stays dark while the gauges wait for each other at power-up
#include <IOX.h>
#include <TCA9554.h>
#include <LVGL_Driver.h>
#include <GaugeUI.h>
#include <CANLine.h>
#include <Wire.h>

static bool triggerState[IOX_CHANNELS];
static bool lightsOn = false;
static bool nightMode = false;
static bool outputFault = false;
static uint8_t lightsChangeCount = 0;
static uint8_t faultChangeCount = 0; // polls in a row the output's fault pin has read differently (debounce)

// Brings up every I/O bus the rest of the firmware needs: the I2C bus + TCA9554 expander (the display's reset/CS
// lines hang off it, so this MUST run before setupDisplay()), the DRV8876 output, the CAN termination FET and the
// CAN bus itself. No poll task here - ioTick() runs from the io task in setupTasks(), started after the display is up
// (ioTick() drives the backlight). (This board has no ADC; the 0-5 V front ends were removed in V3.4.)
void setupIO()
{
  // I2C bus + expander first
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_HZ);
  uint8_t exioDefaults = 0x0F; // P0-P3 high: LCD_RST / TP_RST / LCD_CS released, CAN_TERM_EN high (= off); P4/P5 low = DRV8876 coast
  if (!tca9554Init(TCA9554_ADDR, exioDefaults, EXIO_INPUT_MASK)) // P6 dash lights, P7 output fault = inputs
  {
    DEBUG("TCA9554 NOT found - display reset/CS + output unavailable");
  }

  // output (DRV8876 via the expander) + CAN termination FET
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
  canTerminationFromSettings(); // 0.30.15: Off / On / Auto (Auto starts off: the boot sync decides)

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

bool dashLightsOn()
{
  return lightsOn;
}

bool nightActive()
{
  return nightMode;
}

uint8_t backlightTarget()
{
  if (bootSyncHoldingDark())
  {
    return 0; // 0.30.14: power-up, waiting for the other gauges so the boot logos start together
  }
  return nightMode ? settings.nightBrightness : settings.brightness;
}

void ioTick()
{
  bootSyncTermTick(); // 0.30.15: Auto CAN termination - off again if car frames turn up
  // dash lights: the hardware already holds the 58d PWM peak (~1 s), so 3 equal polls in a row are enough
  bool lightsRaw = ioxDashLightsRaw() != (settings.dimInvert != 0);
  if (lightsRaw != lightsOn)
  {
    if (++lightsChangeCount >= 3)
    {
      lightsOn = lightsRaw;
      lightsChangeCount = 0;
      DEBUG("IO: dash lights %s", lightsOn ? "ON" : "off");
    }
  }
  else
  {
    lightsChangeCount = 0;
  }
  signalSet(SIG_DASH_LIGHTS, lightsOn ? 1 : 0);

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
      DEBUG("IO: output %s", outputFault ? "FAULT (over-current / over-temperature)" : "fault cleared");
    }
  }
  else
  {
    faultChangeCount = 0;
  }
  signalSet(SIG_OUT_FAULT, outputFault ? 1 : 0);
  signalSet(SIG_OUTPUT, ioxGet(IOX_OUT) ? 1 : 0);
  powerTick(); // ignition on permanent power: shutdown / standby / deep sleep
}

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
    DEBUG("IO: %s -> %d (%s)", ioxChannelName(channel), level, mode == 2 ? "auto" : "manual");
#endif
  }
}

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

static bool isTerminationOnNow = false; // 0.30.15: the 120R is in circuit right now (Auto can change it)

// 0.30.15: is the 120R in circuit right now? (with Auto, the setting alone doesn't say)
bool canTerminationIsOn()
{
  return isTerminationOnNow;
}

// 0.30.15: the 120R as the setting says: Off, On, or Auto - on only while the boot sync has switched it on (the bus
// was silent at power-up and no car frames have been heard since)
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

void canTerminationApply(bool enabled)
{
  isTerminationOnNow = enabled;
  // Q_TERM gate: LOW = 120R in circuit. TCA9554 P3 (EXIO4)
  tca9554Write(EXIO_CAN_TERM_EN, !enabled);
  DEBUG("CAN termination %s", enabled ? "ON (120R)" : "off");
}

void ioInfoProvider(char *buf, size_t len)
{
  size_t written = 0;
  appendf(buf, len, written, "Output (J_OUT 1): %s, %s%s\n",
          settings.outputMode == 1 ? "low-side" : "high-side", ioxGet(IOX_OUT) ? "ON" : "off",
          outputFault ? "  FAULT" : "");
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
  appendf(buf, len, written, "\nDash lights (J_OUT 3): %s\n", lightsOn ? "ON" : "off");
  if (written < len)
  {
    const char *nightText;
    if (settings.ignSource == IGN_SRC_WIRE)
    {
      nightText = "off (input = ignition)";
    }
    else if (settings.nightSource == 1)
    {
      nightText = nightMode ? "active" : "armed";
    }
    else
    {
      nightText = "off";
    }
    appendf(buf, len, written, "Night dimming: %s", nightText);
  }
}
