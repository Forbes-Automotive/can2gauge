#include <can2gauge_Triggers.h>
#include <can2gauge_IO.h>
#include <GaugeUI.h>

// 0.32.0: how long after power-on a missing reading is ignored even by a "No Reading: Trigger" trigger - the CAN bus,
// the adc2can and the page sources take a moment to send their first values
#define NO_READING_GRACE_MS 5000

struct TriggerState
{
  bool active;       // the trigger has fired (its alert / output are on)
  bool acknowledged; // 0.32.0: the driver tapped its alert away: no alert again until the condition has cleared
  uint32_t sinceMs;  // when the condition first became true (0 = it isn't)
};
static TriggerState triggerStates[MAX_TRIGGERS];

static uint32_t acksSeen = 0;          // 0.32.0: runtime.alertAcks as last read (a change = the driver tapped an alert)
static bool alertShown = false;        // 0.32.0: the triggers' alert is on the screen
static char alertShownText[NAME_LEN];  // ... with this text

void setupTriggers()
{
  memset(triggerStates, 0, sizeof(triggerStates));
  acksSeen = runtime.alertAcks;
}

bool triggerActive(uint8_t index)
{
  return index < MAX_TRIGGERS && triggerStates[index].active;
}

// 0.32.0: every trigger that has fired counts as acknowledged: its alert stays away until its condition clears (then
// it re-arms). A latched one lets go then too. Before, the alert came straight back on the next pass.
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

// the web UI's Clear Alerts: the same as tapping the alert away
void triggersReset()
{
  acknowledgeTriggers();
  uiClearAlert();
  alertShown = false;
}

void triggersTick()
{
  const uint32_t now = millis();
  bool anyAlert = false;
  const char *alertText = NULL;
  bool alertLatch = false;
  bool outputWanted[IOX_CHANNELS] = {0};
  bool outputUsed[IOX_CHANNELS] = {0};

  // 0.32.0: the driver tapped the alert away since the last pass
  if (runtime.alertAcks != acksSeen)
  {
    acksSeen = runtime.alertAcks;
    acknowledgeTriggers();
    alertShown = false;
  }

  for (int triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    const TriggerCfg &trigger = settings.triggers[triggerIndex];
    TriggerState &state = triggerStates[triggerIndex];
    const signalId_t id = (signalId_t)trigger.signal;
    if (!trigger.enabled || id == SIG_NONE)
    {
      // 0.32.0: switched off = off, latched or not (before, a latched one stayed on with its output)
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
    else if (settings.triggers[triggerIndex].noReading && now > NO_READING_GRACE_MS)
    {
      // 0.32.0: no reading (sensor unplugged, shorted or faulted, the bus quiet) and the trigger is set to count that.
      // A shorted adc2can sensor reads as "no value", so a "below 3.5 V" alarm never saw the 0 V it was waiting for.
      condition = true;
      release = false;
    }
    // (no reading and set to ignore it: condition false, release true - the trigger waits, as before)

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
    if (usesOutput)
    {
      outputUsed[trigger.output] = true;
      if (state.active)
      {
        outputWanted[trigger.output] = true; // the output stays on while the trigger is active, acknowledged or not
      }
    }
  }

  for (int channel = 0; channel < IOX_CHANNELS; channel++)
  {
    if (outputUsed[channel])
    {
      outputRequest(channel, outputWanted[channel], true);
    }
  }

  // the alert on the screen
  if (alertShown && !runtime.alertActive)
  {
    alertShown = false; // taken off by something else (a test alert cleared ...): put it back below if still wanted
  }
  if (anyAlert)
  {
    if (!alertShown || strcmp(alertShownText, alertText) != 0)
    {
      if (uiAlert(alertText, alertLatch)) // 0.32.0: false = the screen was busy: tried again on the next pass
      {
        alertShown = true;
        copyStr(alertShownText, sizeof(alertShownText), alertText);
      }
    }
  }
  else if (alertShown)
  {
    uiClearAlert();
    alertShown = false;
  }
}
