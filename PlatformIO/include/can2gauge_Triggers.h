#pragma once
#include <can2gauge_defs.h>

// set-point triggers: evaluated every triggerRefresh ms against the signal catalogue

// ===== Set-up and the regular check =====
void setupTriggers(); // every trigger starts not fired (in setup)
void triggersTick();  // checks every trigger: alert, outputs (logic task, every triggerRefresh ms)

// ===== For the web UI and the screen =====
bool triggerActive(uint8_t index); // has this trigger fired?
void triggersReset();              // the web UI's Clear Alerts: acknowledges every fired trigger, as a tap on the alert does
