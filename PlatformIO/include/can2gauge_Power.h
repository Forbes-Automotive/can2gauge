#pragma once
#include <can2gauge_defs.h>

// Ignition on permanent power (settings.ignSource, web UI > Output / Illumination > Ignition).
// With the gauge on a permanent feed, the ignition comes from terminal 15 on the CAN bus or from the illumination input
// rewired to an ignition feed, or (0.29.2) from another gauge in the chain that has one of those - its heartbeat carries
// "my ignition is on". Ignition off (for settings.ignOffDelayS):
//   shutdown  the shutdown sprite (settings.shutdownSprite) plays, or the glass fades to black
//   standby   backlight off, panel asleep, CAN listen-only, the K-line / OBD adapter / gearbox reads stand down and the
//             output is off. Ignition on wakes it at once (the wake sprite, then the gauge page).
//   deep sleep after settings.standbyMin, once nobody is on the web UI (and, for CAN, the bus has gone quiet). CAN
//             wakes the ESP32 on the first bit on the bus (CAN RX is an RTC pin); the illumination input sits on the
//             TCA9554, whose INT is not wired on V3.4, so the wire is checked every POWER_WIRE_POLL_S instead. A wake
//             with the ignition still off goes straight back to sleep without lighting the glass.
// V3.4 keeps the CAN transceiver, the DRV8876, the AMS1117 and the buck powered, so even deep sleep draws a few mA.
#define POWER_WIRE_POLL_S 3       // deep sleep, illumination-input ignition: wake this often to check the wire
#define POWER_BUS_QUIET_MS 5000   // deep sleep, CAN ignition: the bus must have been quiet this long first
#define POWER_BOOT_GRACE_MS 5000  // after a power-up, the ignition sources get this long before "off" counts

void setupPower();                 // before setupIO(): the CAN hook that watches terminal 15
void powerWakeProbe(bool fromSleep); // after setupIO(): woken from deep sleep with the ignition still off -> back to sleep
void powerTick();                  // from the io task (ioRefresh): the ignition and the shutdown / standby / sleep steps
bool ignitionOn();                 // true with no ignition source set
const char *powerState();          // "switched power", "on", "shutdown", "standby"
uint32_t powerSleepInS();          // standby: seconds until deep sleep is due (it then also waits for the web UI to
                                   // close and, on CAN, for the bus to go quiet); 0 outside standby
void powerDetail(char *buf, size_t len); // what the ignition source is seeing, for the web UI
void powerTestShutdown();          // web UI: play the shutdown sprite, then the wake sprite, and carry on (no standby)
bool powerCarFrameSeen();          // 0.30.15: a car frame (not the gauge chain's own) heard since power-up
