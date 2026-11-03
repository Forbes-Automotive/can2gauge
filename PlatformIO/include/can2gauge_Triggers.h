#pragma once
#include <can2gauge_defs.h>

// set-point triggers: evaluated every triggerRefresh ms against the signal catalogue
void setupTriggers();
void triggersTick();
bool triggerActive(uint8_t index);
void triggersReset(); // the web UI's Clear Alerts: acknowledges every fired trigger (0.32.0), as a tap on the alert does
