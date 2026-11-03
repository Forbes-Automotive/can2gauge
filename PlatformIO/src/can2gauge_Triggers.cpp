// Set-point triggers: each one watches a signal against a threshold (above or below, with a hysteresis band and a
// hold time) and, once it fires, puts an alert on the screen and / or switches an output on.

#include <can2gauge_Triggers.h>
#include <can2gauge_IO.h> // outputRequest: the trigger outputs
#include <GaugeUI.h>      // uiAlert / uiClearAlert: the alert on the screen

// ===== Settings for this file =====
// how long after power-on a missing reading is ignored even by a "No Reading: Trigger" trigger - the CAN bus,
// the adc2can and the page sources take a moment to send their first values
#define NO_READING_GRACE_MS 5000

// where one trigger is up to (this file's own, never saved)
struct TriggerState
{
  uint32_t sinceMs;  // when the condition first became true (0 = it isn't)
  bool active;       // the trigger has fired (its alert / output are on)
  bool acknowledged; // the driver tapped its alert away: no alert again until the condition has cleared
};

// ===== File variables (static: private to this file) =====
static uint32_t acksSeen = 0;                    // runtime.alertAcks as last read (a change = the driver tapped an alert)
static uint32_t awakeSinceMs = 0;                // when the gauge came out of standby (or started): the readings take a moment
static bool isAlertShown = false;                // the triggers' alert is on the screen
static char alertShownText[NAME_LEN];            // ... with this text
static TriggerState triggerStates[MAX_TRIGGERS]; // where each trigger is up to

// ===== Functions in this file =====
static void acknowledgeTriggers(); // every fired trigger counts as acknowledged (its alert stays away)

// every trigger starts not fired
void setupTriggers()
{
  DEBUG("Setting up the triggers...");
  memset(triggerStates, 0, sizeof(triggerStates));
  acksSeen = runtime.alertAcks;
  DEBUG("Set up the triggers!");
}

// checks every trigger (logic task, every triggerRefresh ms): fires / lets go of each one, switches the outputs and
// puts up / takes down the alert on the screen
void triggersTick()
{
  const uint32_t now = millis();
  bool anyAlert = false;
  const char *alertText = NULL;
  bool alertLatch = false;
  bool outputWanted[IOX_CHANNELS] = {0};

  // ignition off (standby): nothing is checked, as the car's readings stop when its bus sleeps. Coming back, the
  // readings get the same moment to arrive as at power-up before "No Reading" counts.
  if (runtime.standby)
  {
    for (int triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
    {
      triggerStates[triggerIndex].sinceMs = 0;
    }
    awakeSinceMs = 0;
    return;
  }
  if (awakeSinceMs == 0)
  {
    awakeSinceMs = now | 1; // never 0 (0 = not started)
  }

  // the driver tapped the alert away since the last pass
  if (runtime.alertAcks != acksSeen)
  {
    acksSeen = runtime.alertAcks;
    acknowledgeTriggers();
    isAlertShown = false;
  }

  for (int triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    const TriggerCfg &trigger = settings.triggers[triggerIndex];
    TriggerState &state = triggerStates[triggerIndex];
    const signalId_t id = (signalId_t)trigger.signal;
    if (!trigger.enabled || id == SIG_NONE)
    {
      // switched off = off, latched or not
      state.active = false;
      state.acknowledged = false;
      state.sinceMs = 0;
      continue;
    }

    // is the condition met? (release = far enough back past the threshold to let go, the hysteresis band)
    bool condition = false;
    bool release = true;
    if (signalValid(id))
    {
      const float value = signalGet(id);
      if (trigger.above)
      {
        condition = value >= trigger.threshold;
        release = value < trigger.threshold - trigger.hysteresis;
      }
      else
      {
        condition = value <= trigger.threshold;
        release = value > trigger.threshold + trigger.hysteresis;
      }
    }
    else if (settings.triggers[triggerIndex].noReading && (int32_t)(now - awakeSinceMs) > NO_READING_GRACE_MS)
    {
      // no reading (sensor unplugged, shorted or faulted, the bus quiet) and the trigger is set to count that.
      // A shorted adc2can sensor reads as "no value", so a "below 3.5 V" alarm never saw the 0 V it was waiting for.
      condition = true;
      release = false;
    }
    // (no reading and set to ignore it: condition false, release true - the trigger waits)

    if (condition)
    {
      if (state.sinceMs == 0)
      {
        state.sinceMs = now;
        if (state.sinceMs == 0)
        {
          state.sinceMs = 1; // 0 means "not started", so the very first millisecond counts as 1
        }
      }
      if (!state.active && now - state.sinceMs >= trigger.holdMs)
      {
        state.active = true;
        state.acknowledged = false;
        DEBUG("TRIGGER %s fired", trigger.label);
      }
    }
    else
    {
      state.sinceMs = 0;
      // let go once past the hysteresis band: straight away, or for a latched one once it has been acknowledged
      if (state.active && release && (!trigger.latch || state.acknowledged))
      {
        state.active = false;
        state.acknowledged = false;
      }
    }

    const bool usesAlert = trigger.action == ACT_ALERT || trigger.action == ACT_ALERT_OUT;
    const bool usesOutput = (trigger.action == ACT_OUTPUT || trigger.action == ACT_ALERT_OUT) && trigger.output < IOX_CHANNELS;
    if (state.active && !state.acknowledged && usesAlert && !anyAlert)
    {
      anyAlert = true; // the first trigger that wants an alert is the one shown
      alertText = trigger.label;
      alertLatch = trigger.latch;
    }
    if (usesOutput && state.active)
    {
      outputWanted[trigger.output] = true; // the output stays on while the trigger is active, acknowledged or not
    }
  }

  // every channel, used by a trigger or not: one whose trigger was just switched off (or changed to alert only) goes off
  for (int channel = 0; channel < IOX_CHANNELS; channel++)
  {
    outputRequest(channel, outputWanted[channel], true);
  }

  // the alert on the screen
  if (isAlertShown && !runtime.alertActive)
  {
    isAlertShown = false; // taken off by something else (a test alert cleared ...): put it back below if still wanted
  }
  if (anyAlert)
  {
    if (!isAlertShown || strcmp(alertShownText, alertText) != 0)
    {
      if (uiAlert(alertText, alertLatch)) // false = the screen was busy: tried again on the next pass
      {
        isAlertShown = true;
        copyStr(alertShownText, sizeof(alertShownText), alertText);
      }
    }
  }
  else if (isAlertShown)
  {
    uiClearAlert();
    isAlertShown = false;
  }
}

// has this trigger fired? (the web UI, the screen's trigger marks)
bool triggerActive(uint8_t index)
{
  return index < MAX_TRIGGERS && triggerStates[index].active;
}

// the web UI's Clear Alerts: the same as tapping the alert away
void triggersReset()
{
  acknowledgeTriggers();
  uiClearAlert();
  isAlertShown = false;
}

// Every trigger that has fired counts as acknowledged: its alert stays away until its condition clears (then it
// re-arms), so the alert doesn't come straight back on the next pass. A latched one lets go then too.
static void acknowledgeTriggers()
{
  for (int triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    if (triggerStates[triggerIndex].active)
    {
      triggerStates[triggerIndex].acknowledged = true;
    }
  }
}
