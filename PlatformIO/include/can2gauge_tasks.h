#pragma once
#include <can2gauge_defs.h>

// Starts every periodic task: io poll, gauge refresh, vehicle logic (OBD poll / triggers / temp), and housekeeping
// (settings save, OTA confirm, WiFi upkeep - the work that used to run in loop()). Call last, once all hardware,
// the display and the web server are up.
void setupTasks();
