#pragma once
// ---------------------------------------------------------------------------
// ECUN - an aftermarket ECU's live data over ESP-NOW (wire format v1). SHARED FILE: identical copies live in
//   usb2ip/<version>/include/ecunow_proto.h  and  can2gauge/include/ecunow_proto.h
// Change both together and bump ECUN_PROTO on any layout change. Goes with ecunow_library.h (also shared, generated).
//
// usb2ip (the wireless USB bridge for an Ignitron or MaxxECU) broadcasts the ECU's live values, so a can2gauge with
// power only - no CAN, no OBD - shows them, the same way it shows OpenHaldex (ohx_proto.h) and adc2can (adcnow_proto.h).
//   - Broadcast, no pairing: every gauge in range on the sender's WiFi channel hears it. can2gauge's access point uses
//     channel 1, and so does usb2ip's by default (from usb2ip 2.0). If either joins a home network, both must join it.
//   - One-way: nothing is ever sent back to the ECU this way.
//   - ECUN_T_DATA every ECUN_DATA_MS while the ECU's data is fresh: the everyday values in fixed units (ecunValue_t,
//     NAN = this ECU doesn't have it). They fill the gauge's own signals (RPM, boost, lambda ...).
//   - ECUN_T_RAW every ECUN_RAW_MS: EVERY channel of the ECU's library (ecunow_library.h), as the ECU's own raw 16-bit
//     words, in library row order, split over as many packets as it takes (Ignitron 4, MaxxECU 6, Link 8, Haltech 8, EMU PRO 13). The gauge decodes
//     them with its copy of the library, so any channel can go on a gauge page.
//   - ECUN_T_INFO every ECUN_INFO_MS: who is sending, and which library it uses. A gauge whose library ID differs
//     (one of the two hasn't been updated) ignores the RAW packets and says so; DATA still works.
// ---------------------------------------------------------------------------
#include <stdint.h>

#define ECUN_MAGIC0 'E'
#define ECUN_MAGIC1 'U'
#define ECUN_PROTO 1
#define ECUN_DATA_MS 50     // everyday values: 20 Hz
#define ECUN_RAW_MS 100     // the whole library: 10 Hz
#define ECUN_INFO_MS 2000   // sender info

enum ecunType_t : uint8_t
{
  ECUN_T_DATA = 1,
  ECUN_T_RAW = 2,
  ECUN_T_INFO = 3,
};

enum ecunKind_t : uint8_t // which ECU usb2ip is reading (also picks the library in ecunow_library.h)
{
  ECUN_K_UNKNOWN = 0,
  ECUN_K_IGNITRON = 1,
  ECUN_K_MAXXECU = 2,    // MaxxECU
  ECUN_K_LINK = 3,       // Link G4X / G5
  ECUN_K_HALTECH = 4,    // Haltech Elite / Nexus
  ECUN_K_EMU_PRO = 5,    // ECUMaster EMU PRO
};

// flags
#define ECUN_F_LIVE 0x01     // the values are fresh (usb2ip is reading the ECU right now)
#define ECUN_F_FROM_PC 0x02  // decoded from a PC session over USB/IP (Ignitron / MTune) rather than usb2ip's own poll

// The everyday values, index into EcunData.value[]. Units are fixed here, whatever the ECU uses.
enum ecunValue_t : uint8_t
{
  ECUN_V_RPM = 0,        // rpm
  ECUN_V_SPEED,          // km/h
  ECUN_V_COOLANT,        // degC
  ECUN_V_IAT,            // degC intake air
  ECUN_V_MAP,            // kPa absolute manifold pressure
  ECUN_V_PEDAL,          // % accelerator pedal
  ECUN_V_THROTTLE,       // % throttle position
  ECUN_V_LOAD,           // % engine load
  ECUN_V_LAMBDA,         // lambda
  ECUN_V_LAMBDA_TARGET,  // lambda
  ECUN_V_TIMING,         // deg BTDC ignition advance
  ECUN_V_KNOCK_RETARD,   // deg, the highest of any cylinder (0 = no retard)
  ECUN_V_INJ_DUTY,       // % injector duty (primary)
  ECUN_V_BATTERY,        // V
  ECUN_V_OIL_TEMP,       // degC
  ECUN_V_OIL_PRESSURE,   // bar
  ECUN_V_FUEL_PRESSURE,  // bar
  ECUN_V_EGT,            // degC, the highest exhaust gas temperature
  ECUN_V_ETHANOL,        // % ethanol content
  ECUN_V_GEAR,           // 0 = neutral / unknown, 1..10
  ECUN_VALUES
};

struct __attribute__((packed)) EcunHdr
{
  uint8_t magic[2]; // ECUN_MAGIC0, ECUN_MAGIC1
  uint8_t type;     // ecunType_t
  uint8_t proto;    // ECUN_PROTO
  uint8_t kind;     // ecunKind_t
  uint8_t seq;      // +1 per packet (all types): a gap = packets lost in the air
  uint8_t flags;    // ECUN_F_*
  uint8_t fw[2];    // usb2ip firmware major, minor
  uint8_t spare;
};

struct __attribute__((packed)) EcunData
{
  EcunHdr h;
  float value[ECUN_VALUES]; // ecunValue_t order; NAN = not available from this ECU
};

#define ECUN_RAW_WORDS 117  // library rows per RAW packet
struct __attribute__((packed)) EcunRaw
{
  EcunHdr h;
  uint16_t libraryId;            // ECUN_LIB_*_ID of the library the rows belong to
  uint16_t firstRow;             // library row of word[0]
  uint8_t count;                 // words that follow (1..ECUN_RAW_WORDS)
  uint16_t word[ECUN_RAW_WORDS]; // raw 16-bit values, library row order; decode with the row's scale / bias / flags
};

struct __attribute__((packed)) EcunInfo
{
  EcunHdr h;
  uint16_t libraryId;  // ECUN_LIB_*_ID
  uint16_t rows;       // library rows sent in RAW packets
  char source[24];     // usb2ip's access point name (SSID), to tell several apart
  char ecu[24];        // the ECU as usb2ip identified it, e.g. "Ignitron I22", "MaxxECU GEN2 1.161"
};

static_assert(sizeof(EcunHdr) == 10, "ECUN header layout");
static_assert(sizeof(EcunData) <= 250, "ECUN data must fit one ESP-NOW v1 packet");
static_assert(sizeof(EcunRaw) <= 250, "ECUN raw must fit one ESP-NOW v1 packet");
static_assert(sizeof(EcunInfo) <= 250, "ECUN info must fit one ESP-NOW v1 packet");
