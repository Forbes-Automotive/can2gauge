#pragma once
#include <can2gauge_defs.h>

// AP always (can2gauge-XXXX, open by default, http://192.168.1.1 or http://c2g.local) + optional STA bridge to a home
// network (same scheme as OpenHaldex-C6: short aggressive window after boot, then a slow retry so the AP stays clean)
void setupWiFi();
void pollWiFi();       // call from loop()
void wifiRestart();    // after AP / STA settings change
bool wifiStaConnected();
// 0.8.0: the station for a WiFi OBD adapter. want = true stops the Home WiFi bridge and hands the station over (the AP
// stays up), false gives it back. Blocks up to waitMs for pollWiFi() to do it; returns whether it happened.
bool wifiStaClaim(bool want, uint32_t waitMs);
