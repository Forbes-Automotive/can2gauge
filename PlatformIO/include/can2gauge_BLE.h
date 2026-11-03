#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// BleElmLink - BLE central transport to an ELM327 / STN "iPhone compatible" OBD adapter (NimBLE-Arduino 2.x).
// The ESP32-S3 has no Bluetooth Classic, so SPP-only adapters can't work.
//  - service: FFF0, then FFE0, then 18F0, then the Vgate E7810A71-..., then any custom service with a notify + a write
//    characteristic. RX / TX are picked by their GATT properties, never by UUID (vendors disagree on FFF1 vs FFF2, and
//    FFE1-type adapters use one characteristic for both).
//  - writes are chunked to MTU - 3 and use write-with-response when the characteristic offers it
//  - notifications are a byte stream (bleRead); the ELM '>' prompt often arrives in a notification of its own
// All calls block; use them from the ELM task only. NimBLE stays out of this header.
// ---------------------------------------------------------------------------

struct BleFound
{
  char mac[18];
  uint8_t addressType; // BLE address type (needed to reconnect by address)
  char name[24];
  int8_t rssi;
  bool likely;  // advertises a known OBD service or has an OBD-ish name
};

bool bleInit();               // starts Bluetooth the first time (false: not enough RAM, or it failed - see bleStartError)
void bleShutdown();           // 0.24.12: stops Bluetooth and hands its internal RAM back (it starts again when needed)
const char *bleStartError();  // why Bluetooth didn't start ("" when it did, or hasn't been tried)
bool bleNeedsRestart();       // 0.24.22: true = it didn't start for lack of RAM; it will after a restart (it starts first then)
bool bleRunning();            // 0.37.15: Bluetooth is up now (started for the OBD adapter, a scan or the nav link)
uint8_t bleScan(uint32_t durationMs, BleFound *out, uint8_t maxResults); // active scan, likely adapters first, then by RSSI
bool bleConnect(const char *mac, uint8_t addressType, char *err, size_t errLen);
void bleDisconnect();
bool bleConnected();
bool bleWrite(const uint8_t *data, size_t len);
size_t bleRead(uint8_t *buf, size_t maxLen, uint32_t timeoutMs);
void bleFlush();
int bleRssi();
const char *bleLinkInfo(); // "fff0: rx fff1 / tx fff2, mtu 23"
