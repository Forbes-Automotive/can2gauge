#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// DSG / gearbox diagnostics. The selector (P R N D S M) and the gear come for free from the passive CAN broadcast
// (lib/CANLine, per CAN profile). This module adds an ACTIVE read of the gearbox for its own speed (and optionally the
// gear; 0.26.0 also the oil temperature, the converter lock-up and the selector, for the ZF 8HP - settings.dsgGearbox
// says which gearbox, and BMW's extended addressing puts the EGS's address 0x18 in front of every frame),
// picked by settings.dsgSource:
//   DSG_SRC_UDS  - UDS / OBD over ISO-TP on the gauge's CAN: request settings.dsgTxId (0x7E1), response dsgRxId (0x7E9)
//                  with the read described by settings.dsgSpeed / dsgGear (default 22 F40D = OBD vehicle speed as a DID)
//   DSG_SRC_TP20 - KWP2000 over VW TP2.0 (PQ35 DQ250, module 0x02): channel setup on 0x200, measuring block
//                  settings.dsgTp20Group, field dsgTp20SpeedField
//   DSG_SRC_ELM  - the same UDS / OBD read, sent by the BLE ELM327 module (it calls dsgBuildRequest / dsgHandleResponse)
//   DSG_SRC_ELM_TP20 (0.8.0) - the TP2.0 read through the BLE ELM327 in raw CAN mode: the ELM task runs Tp20Client
//                  over ElmRawLink and reports with dsgTp20Channel / dsgTp20Result / dsgSetState
// Only the chain master polls (linkIsMaster()); a follower gets SIG_TCU_SPEED over the link.
// ---------------------------------------------------------------------------

struct DsgField
{
  uint8_t formula, a, b;
  float value;
  char unit[8];
};

// 0.26.0: the diagnostic reads, by number (each has a DsgReadSpec in settings - see dsgReadSpec())
#define DSG_READ_SPEED 0    // settings.dsgSpeed -> SIG_TCU_SPEED
#define DSG_READ_GEAR 1     // settings.dsgGear -> SIG_GEAR
#define DSG_READ_TEMP 2     // settings.dsgTemp -> SIG_GEARBOX_TEMP
#define DSG_READ_LOCKUP 3   // settings.dsgLockup -> SIG_LOCKUP
#define DSG_READ_SELECTOR 4 // settings.dsgSelector -> SIG_SELECTOR
#define DSG_READ_COUNT 5    // how many there are
#define DSG_TEMP_EVERY 10   // the temperature is only read on every 10th pass (it changes slowly; each read costs ~50 ms over Bluetooth)

struct DsgStatus
{
  char state[24];      // "off", "polling", "no answer", "not supported", ...
  char detail[64];     // last error / negotiated ids / NRC
  uint32_t okCount;    // good responses
  uint32_t errCount;   // timeouts / negative responses
  uint8_t lastNrc;     // last negative response code (0 = none)
  bool speedSupported; // false after the gearbox rejected the speed read (NRC 0x11 / 0x12 / 0x31 / 0x7F)
  bool gearSupported;
  bool tempSupported;     // 0.26.0: the same for the three newer reads
  bool lockupSupported;
  bool selectorSupported;
  int32_t lockupRaw;      // 0.26.0: the lock-up read's raw number before the gearbox type's table (-1 = none yet)
  int32_t selectorRaw;    // ... and the selector read's (for working out a new gearbox's codes on the tab)
  uint16_t tp20Tx, tp20Rx; // negotiated TP2.0 channel ids
  uint8_t fieldCount;      // TP2.0: fields of the last speed block
  DsgField fields[4];
  uint8_t gearFieldCount;  // TP2.0: fields of the last gear block
  DsgField gearFields[4];
};

void setupDSG();      // registers the CAN RX hook and starts the gearbox task (core 0)
void dsgRestart();    // settings changed: close the session and start again with the new parameters
bool dsgPolling();    // an active gearbox read is configured on this gauge (chain: counts as an active source)
bool dsgCanSessionOpen(); // 0.17.2: the DSG has a UDS session / TP2.0 channel open on the gauge's CAN right now
// 0.37.10: the DSG's TP2.0 channel, shared. A module allows one channel: a diagnostics value from the module the DSG
// reads over TP2.0 (on the gauge's CAN) uses the DSG's channel. dsgTp20Shares: does the DSG read this module?
// dsgTp20Read: one block through its channel - the DSG's own answer when it read that block within maxAgeMs, else a
// read on the channel. false = the channel isn't open now, or the block wasn't read (nrc = its refusal code).
bool dsgTp20Shares(uint8_t module);
bool dsgTp20Read(uint8_t group, DsgField *fields, uint8_t &count, uint8_t &nrc, uint32_t maxAgeMs);
const DsgStatus &dsgStatus();
void dsgInfoProvider(char *buf, size_t len); // text for the web UI / info page

// shared with the BLE ELM path
uint8_t dsgBuildRequest(const DsgReadSpec &spec, uint8_t *out); // request bytes (svc + id), returns length (0 = off)
const DsgReadSpec &dsgReadSpec(uint8_t which); // 0.26.0: the settings for read DSG_READ_x
// 0.26.0: should read DSG_READ_x be asked for on this pass? (set up, not refused by the gearbox, and due - the
// temperature only every DSG_TEMP_EVERY passes)
bool dsgReadDue(uint8_t which, uint32_t pass);
// response = the positive (or 7F) response payload to read DSG_READ_x. Updates the signals + status.
void dsgHandleResponse(uint8_t which, const uint8_t *response, uint16_t len);
// TP2.0 results (gauge CAN or the ELM): a measuring block read -> fields + SIG_TCU_SPEED / SIG_GEAR, or an error count
// (nrc = the 7F code, 0 = timeout / bad answer)
void dsgTp20Result(bool gearBlock, bool ok, const DsgField *fields, uint8_t fieldCount, uint8_t nrc);
void dsgTp20Channel(uint16_t tx, uint16_t rx); // negotiated channel ids, for the status
void dsgSetState(const char *state, const char *detail);
uint32_t dsgGeneration(); // bumped by dsgRestart(): the ELM path reopens its channel when it changes
