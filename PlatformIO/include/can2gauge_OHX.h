#pragma once
#include <can2gauge_defs.h>
#include <ohx_proto.h>

// ---------------------------------------------------------------------------
// OpenHaldex over WiFi (ESP-NOW, wire format include/ohx_proto.h). OpenHaldex broadcasts its live state every 100 ms;
// this gauge follows one controller (settings.ohxMac - the first heard, then saved) and:
//   - fills the Haldex signals (lock target / actual, mode, speed, pedal, clutch / oil temperature, supply), plus speed /
//     rpm / boost / pedal while nothing else on the gauge provides them (settings.ohxCarSignals) - a gauge with power
//     only, no CAN, still works
//   - sends signed commands: mode, generation, learn, long learn, controller on / off (key = OpenHaldex's AP password)
// The ESP-NOW stack is the gauge chain's (src/can2gauge_Link.cpp): it runs when the chain or this module wants it.
// ---------------------------------------------------------------------------

#define OHX_OTHERS 4 // other OpenHaldex controllers heard (car meets ...), listed in the web UI to switch to

struct OhxView
{
  bool heard;       // a status from the followed OpenHaldex within haldexTimeout
  uint32_t ageMs;   // since the last one (0 = never)
  int8_t rssi;
  uint8_t mac[6];   // the followed controller (all zero = none yet)
  OhxStatus status;     // its latest status
  uint8_t myChannel;
  // the last command sent from this gauge
  uint8_t cmd, arg;
  uint8_t cmdState; // OHX_CS_*
  uint8_t result;   // ohxResult_t
  uint32_t cmdAgeMs;
  // other controllers in range
  uint8_t others;
  uint8_t otherMac[OHX_OTHERS][6];
  char otherName[OHX_OTHERS][24];
  int8_t otherRssi[OHX_OTHERS];
};
enum : uint8_t
{
  OHX_CS_NONE = 0,
  OHX_CS_SENDING, // waiting for the answer (resent with a fresh nonce when stale / unanswered)
  OHX_CS_DONE,    // answered: see result
  OHX_CS_NO_ANSWER,
};

void setupOpenHaldexEspNow();
bool ohxWantsEspNow(); // ESP-NOW must be up for OpenHaldex
// from the shared ESP-NOW receive callback (WiFi task): a packet that starts with the 'O' 'H' magic
void ohxOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len);
bool ohxHeard();
// queue a command (ohxCmd_t). false = no OpenHaldex heard, wireless off, or another command still waiting for its answer
// (a mode change replaces a mode change still in flight: the latest swipe wins)
bool ohxCommand(uint8_t cmd, uint8_t arg0 = 0);
void ohxView(OhxView &view);
void ohxFollow(const uint8_t mac[6]); // follow this controller; all zero = forget, take the next one heard
const char *ohxResultText(uint8_t result);
const char *ohxGenerationName(uint8_t generation);

// Haldex mode change from the glass / web UI: over CAN (0x6A0, or through the chain master) when OpenHaldex is heard on
// CAN, otherwise over WiFi when it is heard there, otherwise CAN as before. false = not sent.
bool haldexRequestMode(uint8_t mode);
bool haldexHeardOnCan();
const char *haldexHeardVia(); // "CAN", "WiFi", "CAN + WiFi" or "" (not heard)
