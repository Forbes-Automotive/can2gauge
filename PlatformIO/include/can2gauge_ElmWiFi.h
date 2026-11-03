#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// ElmWifiLink (0.8.0) - TCP transport to a WiFi ELM327-type adapter (Vgate iCar WiFi, the "WiFi_OBDII" clones ...).
// Same calls as the BLE link, so the ELM task doesn't care which one carries it (settings.elmTransport).
//  - The adapter is an access point (usually open, 192.168.0.10, TCP 35000, DHCP for its clients). The gauge joins it
//    as a station alongside its own AP: wifiStaClaim() pauses the Home WiFi bridge meanwhile. Then it opens one socket.
//  - The radio follows the adapter's channel, so phones on the gauge's AP and the gauge chain's ESP-NOW move with it,
//    exactly as with the Home WiFi bridge.
//  - With no SSID saved, the first network whose name looks like an OBD adapter is joined and saved.
//  - No DHCP answer within 8 s: one more try with a fixed address (adapter's subnet, .123). [U: the adapters seen so
//    far all run DHCP]
//  - Refused when the adapter's network is the gauge's own AP subnet (WiFi.softAPIP(), 192.168.1.x).
// All calls block; use them from the ELM task only.
// ---------------------------------------------------------------------------

bool elmWifiConnect(char *err, size_t errLen);
void elmWifiDisconnect(); // closes the socket, leaves the network, gives the station back to the Home WiFi bridge
bool elmWifiConnected();
bool elmWifiWrite(const uint8_t *data, size_t len);
size_t elmWifiRead(uint8_t *buf, size_t maxLen, uint32_t timeoutMs);
void elmWifiFlush();
const char *elmWifiLinkInfo(); // "WiFi \"WiFi_OBDII\" ch 6, 192.168.0.10:35000, -48 dBm"
