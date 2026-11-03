#pragma once
#include <can2gauge_defs.h>

// Updates go through the shared Forbes Automotive OTA module (include/ota_manager.h, _SharedTheme v2): /api/ota/*,
// guarded filesystem, recovery page, guided GitHub update in data/ota.js. otaInProgress() / otaWebClientActive() come
// from ota_manager.h.
void setupOTA(); // configure + attach the shared OTA routes (call first in setupWebServer)
void otaTick();  // confirm a freshly installed image once the device has run for a while
