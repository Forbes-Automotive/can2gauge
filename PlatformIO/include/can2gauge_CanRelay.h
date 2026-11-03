#pragma once
#include <can2gauge_defs.h>
#include <cannow_proto.h>
#include <CANLine.h> // canInject, CAN_AIR_SOURCES

// The car's CAN frames relayed over WiFi (ESP-NOW, wire format include/cannow_proto.h) by an OpenHaldex or an adc2can
// on the car's bus, with "Relay CAN frames to gauges" on there. A gauge with power only - no CAN wire - decodes them as
// if wired: CANLine's profile decoders and the CAN monitor (so the DBC and user signals too), lib/CANLine canInject.
//   - Each sender gets a source slot; an id heard on this gauge's own wire, or from another sender in the last second,
//     is not taken from a sender (two relays on one bus do not double up).
//   - settings.useCanRelay = 0 ignores them (the senders are still listed).
// The ESP-NOW stack is the gauge chain's (src/can2gauge_Link.cpp).

// ===== Settings =====
#define CANRELAY_SENDERS (CAN_AIR_SOURCES - 1) // sources 1..3: the top one (4) is the OBD adapter's CAN monitor (ELM_CAN_SOURCE)

// one sender as the web UI lists it (canRelaySenders)
struct CanRelaySender
{
  uint8_t mac[6];   // its WiFi address
  uint8_t kind;     // cannKind_t
  int8_t rssi;      // signal strength of its last packet
  uint32_t ageMs;   // since its last packet
  uint32_t perSec;  // frames a second it is sending
  uint32_t frames;  // frames received from it
  uint32_t airLost; // packets lost in the air (gaps in its seq)
  uint32_t srcLost; // frames it reported it could not relay
};

// ===== Set-up =====
void setupCanRelay();       // an empty sender list, and CANLine's queue for the relayed frames
bool canRelayWantsEspNow(); // ESP-NOW must be up for the relay

// ===== Receiving (WiFi task) =====
// from the shared ESP-NOW receive callback (WiFi task): a packet that starts with the 'C' 'N' magic
void canRelayOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len);

// ===== For the web UI =====
uint8_t canRelaySenders(CanRelaySender *out, uint8_t max); // heard in the last 10 s
const char *canRelayKindName(uint8_t kind);                // "OpenHaldex" / "adc2can" / "relay"
