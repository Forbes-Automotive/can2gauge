#pragma once
#include <can2gauge_defs.h>
#include <ecunow_proto.h> // the packets (ecunow_library.h, the channel lists, is only included by can2gauge_ECU.cpp)

// ---------------------------------------------------------------------------
// An aftermarket ECU's live data over WiFi (ESP-NOW, wire format include/ecunow_proto.h) from usb2ip, the wireless USB
// bridge for an Ignitron, MaxxECU, Link, Haltech or ECUMaster EMU PRO. usb2ip broadcasts to every gauge in range; this gauge follows one usb2ip
// (settings.ecuMac - the first one heard, then saved) and:
//   - DATA (20 a second): the everyday values. Rpm, speed, coolant, boost, lambda ... fill the gauge's own signals while
//     nothing else on the gauge provides them (settings.ecuCarSignals), so a gauge with power only, no CAN wire, works.
//     Knock retard, injector duty and lambda target always go on their signals.
//   - RAW (10 a second): every channel of the ECU, as the ECU's own 16-bit words in the order of the channel list in
//     include/ecunow_library.h. The latest word of each channel is kept, and any channel can go on a gauge page as a
//     user signal (USRC_ECU: the ECU kind + its channel number).
//   - INFO (every 2 s): usb2ip's WiFi name, the ECU it found, and which channel list (library) it uses. If that isn't
//     the list this firmware was built with, the RAW packets are ignored and the web UI says to update one of them.
// One-way: nothing is ever sent back to the ECU. The ESP-NOW stack is the gauge chain's (src/can2gauge_Link.cpp).
// ---------------------------------------------------------------------------

#define ECU_OTHERS 4        // other usb2ips heard (car meets ...), listed in the web UI to switch to
#define ECU_HEARD_MS 1500   // no packet from the followed usb2ip for this long = not heard
#define ECU_ROW_FRESH_MS 2000 // a channel whose RAW word is older than this has no value (RAW comes 10 times a second)

// everything the web UI shows about the link (GET /api/ecu), copied out in one go by ecuNowView()
struct EcuNowView
{
  bool heard;                      // a packet from the followed usb2ip within ECU_HEARD_MS
  bool everHeard;                  // anything heard from it since it was followed (the rest is only filled in then)
  uint32_t ageMs;                  // since its last packet
  int8_t rssi;                     // signal strength of its last packet, dBm
  uint8_t mac[6];                  // the followed usb2ip (all zero = none yet)
  uint8_t kind;                    // ecunKind_t: Ignitron, MaxxECU, Link, Haltech or EMU PRO
  bool live;                       // its last DATA packet said it is reading the ECU right now
  bool fromPc;                     // ... decoded from a PC session over USB/IP (Ignitron / MTune) rather than its own poll
  uint8_t fwMajor;                 // usb2ip firmware, major
  uint8_t fwMinor;                 // usb2ip firmware, minor
  char source[24];                 // its WiFi access point name ("" until an INFO packet arrives)
  char ecu[24];                    // the ECU as usb2ip identified it ("Ignitron I22" ...), "" until an INFO packet
  uint16_t theirLibraryId;         // the channel list usb2ip says it uses (0 = not heard yet)
  uint16_t ourLibraryId;           // the one this firmware has for that ECU
  bool libraryMismatch;            // they differ: RAW packets are ignored (update can2gauge or usb2ip)
  uint16_t rowsTotal;              // channels in this firmware's list for that ECU
  uint16_t rowsReceiving;          // ... of them with a fresh RAW word
  uint32_t packets;                // packets received from it
  uint32_t lost;                   // packets lost in the air (gaps in its packet counter)
  uint8_t myChannel;               // this gauge's WiFi channel
  float value[ECUN_VALUES];        // its everyday values in the gauge's units (NAN = not sent / this ECU doesn't have it)
  bool valueUsed[ECUN_VALUES];     // true = on the gauge; false = another source on the gauge provides it
  uint8_t others;                  // how many other usb2ips are in range
  uint8_t otherMac[ECU_OTHERS][6]; // ... their MACs
  char otherName[ECU_OTHERS][24];  // ... their WiFi names ("" until their INFO packet arrives)
  int8_t otherRssi[ECU_OTHERS];    // ... their signal strength, dBm
};

// one channel of an ECU's channel list, for the web UI and the gauge pages
struct EcuChannelInfo
{
  uint16_t row;         // its place in the list (what RAW packets count in)
  uint16_t channel;     // the ECU's own channel number
  const char *key;      // short id ("rpm", "map", "mx_...")
  const char *name;     // full name, as the ECU's PC software shows it
  const char *unit;     // as the library has it (UTF-8, e.g. degree sign + "C")
  float min;            // sensible gauge range
  float max;
  uint8_t decimals;     // decimals to show it with
  uint8_t group;        // index into the group names (ecuNowGroupName)
};

void setupEcuNow();       // clears the state (call once at start-up)
bool ecuNowWantsEspNow(); // ESP-NOW must be up for usb2ip (settings.ecuWireless)
// from the shared ESP-NOW receive callback (WiFi task): a packet that starts with the 'E' 'U' magic
void ecuNowOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len);
bool ecuNowHeard();       // the followed usb2ip was heard within ECU_HEARD_MS
uint8_t ecuNowKind();     // ecunKind_t of the followed usb2ip's ECU (ECUN_K_UNKNOWN until heard)
void ecuNowView(EcuNowView &view);       // copy of the state for the web UI
void ecuNowFollow(const uint8_t mac[6]); // follow this usb2ip; all zero = forget it, follow the next one heard
const char *ecuNowKindName(uint8_t kind); // "Ignitron", "MaxxECU", "Link", "Haltech", "EMU PRO" or "ECU"
signalId_t ecuNowValueSignal(uint8_t valueIndex); // the gauge signal an everyday value (ecunValue_t) goes to

// the channel lists (include/ecunow_library.h)
uint16_t ecuNowLibraryRows(uint8_t kind);    // channels in that ECU's list (0 = unknown ECU)
uint16_t ecuNowLibraryId(uint8_t kind);      // that list's ID
uint8_t ecuNowGroupCount();                  // how many groups the channels are sorted into
const char *ecuNowGroupName(uint8_t group);  // "Engine", "Air & Fuel" ...
bool ecuNowRowInfo(uint8_t kind, uint16_t row, EcuChannelInfo &info);         // false = no such row
bool ecuNowChannelInfo(uint8_t kind, uint16_t channel, EcuChannelInfo &info); // by the ECU's channel number
bool ecuNowRowValue(uint8_t kind, uint16_t row, float &value); // its latest value; false = not received lately
// gauge units for an ECU unit: degree-sign C -> "C", degree sign -> "deg", "Bar" -> "bar" ... (the gauge converts
// "C" / "bar" for the imperial option and its fonts have no other special characters)
void ecuNowGaugeUnit(const char *ecuUnit, char *unit, size_t unitSize);
// the gauge pages changed (userSigApply): find the list row of every USRC_ECU page value again
void ecuNowUserSigsChanged();
// why a USRC_ECU page value has no value yet, for under the gauge ("usb2ip not heard", "usb2ip reads a MaxxECU" ...)
void ecuNowNote(uint8_t kind, uint16_t channel, char *buf, size_t len);
