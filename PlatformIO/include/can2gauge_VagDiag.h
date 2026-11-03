#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// VAG diag (0.13.0) - measuring values from one VW / Audi / Skoda / Seat module over the gauge's own CAN, the way
// a dealer scan tool's measuring values screen reads them (the capture notes in docs/ describe the bench captures this follows):
//   UDS    (MQB era)  - ISO-TP on the module's request / answer ids (0x7E0 / 0x7E8 engine, 0x70F / 0x779 AWD ...):
//                       10 03, the identification DIDs (F187 part number, F19E ASAM dataset ...), then 22 <DID> reads
//   TP2.0  (PQ era)   - channel setup on 0x200, 10 89, 1A 9B identification, then 21 <block> measuring blocks
//   Normal mode       - one value per request with settings.vdiagPollMs between them (as the capture without its fast mode)
//   Turbo             - requests back to back; UDS DIDs packed several to a request (22 D1 D1 D2 D2 ...), up to
//                       settings.vdiagGroupSize (the capture's fast mode with grouped UDS requests)
// The values go to gauge pages as user signals (source USRC_DIAG, "diag:<n>"). Only the chain master sends requests.
// 0.34.0: the requests go out on the gauge's own CAN or through the OBD adapter (settings.vdiagLink), to 11-bit or
// 29-bit ids, with BMW-style extended addressing if set; a value can be any run of bits in the DID's data (OBDb's
// layout), and the Service tab's fault jobs and module scan use the same code.
// ---------------------------------------------------------------------------

// where UDS requests go: one module's ids, and how it is addressed
struct DiagTarget
{
  uint32_t requestId; // the CAN id the requests go out on (0x7E0, 0x18DA01F1, 0x17FC0076 ...)
  uint32_t answerId;  // the CAN id the module answers on (0x7E8, 0x18DAF101, 0x17FE0076 ...)
  bool is29Bit;       // the ids are 29-bit (extended) CAN ids
  uint8_t txExt;      // extended address byte in front of every request (BMW: the module), 0 = none
  uint8_t rxExt;      // extended address byte every answer starts with (BMW: F1), 0 = none
};

struct VdiagValueStatus
{
  bool valid;        // a value has arrived since the session opened
  bool supported;    // false after the module refused it for good (NRC 0x31 request out of range ...)
  uint8_t lastNrc;   // last negative answer for it (0 = none; 0x22 = conditions not correct, e.g. engine off)
  float value;       // scaled value
  char unit[VDIAG_UNIT_LEN]; // the unit in use (KWP: from the formula byte)
  uint8_t raw[8];    // UDS: the DID's first data bytes; KWP: formula, a, b
  uint8_t rawLen;
  uint8_t didLen;    // UDS: the DID's whole data length, learnt from a single read (needed to split grouped answers)
};

struct VdiagStatus
{
  char state[24];          // "off", "connecting", "polling", "no answer" ...
  char detail[64];         // the ids in use / the last error
  uint8_t protocol;        // VDIAG_PROTO_UDS / TP20 once connected, 0 before
  uint32_t txId, rxId;     // UDS ids (11 or 29-bit), or the TP2.0 channel ids
  bool viaAdapter;         // 0.34.0: the session runs through the OBD adapter, not the gauge's CAN
  char partNumber[24];     // UDS F187 / KWP 1A 9B: "4M0907401AM"
  char component[24];      // UDS F197 / KWP 1A 9B: "3.0l V6 TDI", "Haldex4Motion"
  char swVersion[8];       // UDS F189
  char asamDataset[32];    // UDS F19E: "EV_ECM30TDI0114M0907401AM" - the name of the module's data in the dealer tools
  char asamVersion[8];     // UDS F1A2: "001002"
  uint8_t groupSize;       // DIDs per UDS request in use now (Turbo halves it after a refused grouped request)
  bool turboGap;           // 0.38.0: Turbo has gone back to 1 ms gaps between answer frames this session (frames were lost)
  uint32_t okCount, errCount;
  uint32_t samplesPerSec;  // answers per second, measured over the last second
  VdiagValueStatus values[VDIAG_VALUES];
  // the "Try a DID" tool on the tab: the raw answer to one read
  uint16_t probeId;
  uint8_t probeLen;
  uint8_t probe[40];
  char probeText[48];
};

void setupVagDiag();       // registers the CAN RX hook and starts the VAG diag task (core 0)
void vdiagRestart();       // settings changed / standby: close the session and start again
bool vdiagPolling();       // requests are configured on this gauge (the chain counts it as an active source)
void vdiagProbe(uint16_t did); // the tab's "Try a DID": one 22 <did> read on the open UDS session, answer in the status
const VdiagStatus &vdiagStatus();
bool vdiagWantsAdapter();      // 0.34.0: the tab (or a Service job) needs the OBD adapter connected

// 0.34.0: the module address table (VW diagnostic address -> 11-bit UDS ids and a name), for the Service tab's scan
uint8_t vdiagModuleCount();
bool vdiagModuleAt(uint8_t index, uint8_t &address, uint16_t &requestId, uint16_t &answerId, const char *&name);
