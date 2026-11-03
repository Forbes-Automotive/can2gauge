#pragma once
#include <can2gauge_defs.h>

// ===== First Boot setup =====
// A brand-new (or factory-reset) gauge asks on the screen "Set this gauge up from your phone?". Yes shows how to log
// in to the web UI, and the web UI opens its First Boot page by itself: it checks every source (CAN, K-line,
// Bluetooth / WiFi OBD, OpenHaldex, adc2can, usb2ip), shows ticks and crosses, then suggests the gauge pages, sprites,
// boot logo, dimming and menu, which the driver can change before applying. Settings > First Boot setup runs it again.
// The state lives in its own NVS namespace ("c2gfirstboot"), not in the settings blob, so a restored settings backup
// doesn't skip it and the blob layout doesn't change.

// the states (saved in NVS)
#define FIRSTBOOT_ASK 0     // not set up yet: the screen asks at boot
#define FIRSTBOOT_WAITING 1 // the driver said Yes: the web UI opens the First Boot page when it loads
#define FIRSTBOOT_DONE 2    // set up, or the driver said Not Now

// at boot, in this order
void firstBootCheck();       // call BEFORE readEEP(): a gauge that already has settings counts as set up
void firstBootPrompt();      // call after setupDisplay(): puts the question on the screen when it's needed
void setupFirstBootRoutes(); // GET / POST /api/firstboot (call from setupWebServer, before the server starts)

// the state
uint8_t firstBootState();              // FIRSTBOOT_ASK / _WAITING / _DONE
void firstBootSetState(uint8_t state); // saved to NVS at once

// the First Boot check's OBD-II over CAN
bool firstBootObdProbe(); // the First Boot check is asking for OBD-II over CAN right now (a few seconds)
