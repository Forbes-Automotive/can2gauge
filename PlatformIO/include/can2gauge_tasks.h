#pragma once
#include <can2gauge_defs.h>

// Starts the gauge's periodic tasks: io (dash lights, dimming, output fault, ignition), the gauge refresh, the car
// logic (OBD polling, triggers, temperature) and housekeeping (settings saves, OTA confirm, WiFi upkeep). Call last in
// setup(), once the hardware, the screen and the web server are up.
void setupTasks();
