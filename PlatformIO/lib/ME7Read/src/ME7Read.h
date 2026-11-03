#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// ME7Read - read the full flash of a Bosch ME7 ECU over K-line into PSRAM (0.17.4; the app saves it as a .bin when
// the filesystem has room, and serves it for download from memory either way).
//
// This is a clean-room reimplementation of the read path in the open-source NefMoto ME7 flasher
// (github.com/NefMoto/NefMotoOpenSource, GPLv3). Only the read side is here - nothing writes to the ECU.
// The VAG security key tables and the KWP2000 sequence are taken from that project; see the comments by
// each step. can2gauge is itself open source, so the GPL carries over cleanly.
//
// Sequence (all over the KLine transport, KWP2000 / ISO 14230):
//   1. fast or slow init (ME7.5: slow init to 0x11, then talk to 0x10 - Me7ReadParams.target)
//   2. (0.17.4, NefMoto's ReadExternalFlashOperation order) default timing 83 01, SecurityAccess 0x27 in the default
//      session: request seed 0x01, reply with the VAG "external RAM" key
//   3. StartDiagnosticSession 0x85 (programming) with the new K-line baud rate
//   4. AccessTimingParameters -> the ECU's fastest limits (P2max especially, or transfers stall)
//   5. SecurityAccess again inside the programming session (a zero seed, but the key still has to be sent)
//   6. per 64 KB block: RequestUpload 0x35 (addr,size) -> TransferData 0x36 (repeat) -> RequestTransferExit 0x37
//   7. each block goes into a 1 MB PSRAM buffer at its place in the flash (me7ReadDump() hands it out)
//
// NOT every ECU allows this: later software with the upload service disabled will fail at step 6, and the
// module reports that instead of hanging. Reading never changes the flash, but the ECU stays in its
// programming session on the RAM kernel until the ignition is cycled, so the engine will not start until then.
// Do it engine off, ignition on, battery on a charger.
//
// 0.18.0 - read a locked ECU with ReadMemoryByAddress (params.ramUnlock), for ECUs that REFUSE SecurityAccess
// (bench ME7.5/ME7.1 answer 0x27 with 7F 27 10, so the normal RequestUpload read above never starts). This path
// reads the flash with 0x23 from the flash window (flashBase) instead, which needs no 0x27 login.
//
// An REA of 16 ECU images (docs/me7_ramroutine_re.md 6e.1-6e.3) found the flash window is gated by an INTEGRITY
// word at permitAddr (0xE048), not a login - and on a stock, checksum-valid ECU that word already holds the value
// the gate wants. So on a stock ECU the flash reads straight away with NOTHING written to it. The read only
// forges (in RAM with WriteMemoryByAddress 0x3D, allowed with no security, cleared at ignition-off) as a FALLBACK
// if the plain read fails - e.g. a modified flash whose permit word no longer matches - setting the grant bit and
// writing permitValue, then retrying.
//
// The addresses are the same across every ME7 (VR6 ME7.1, 1.8T ME7.5, 2.7T ME7.1): permitAddr 0xE048, grantAddr
// 0xE074 bit 15, flashBase 0x800000. permitValue is a word baked per image and is ONLY needed for the fallback
// forge: 022 = 0x2620, 06A..HJ = 0x26A2, 8N0 (TT) = 0x25B4, 066 = 0x2370, others 0x25A6/0x27C0/0x2ABA; set it per
// ECU from the API / the web UI "Permit" box when a plain read won't do.
//
// The whole feature lives in this library plus src/can2gauge_ME7Read.cpp, include/can2gauge_ME7Read.h and
// data/me7read.js. Remove those files and the references listed in library.json and it is gone.
// ---------------------------------------------------------------------------

enum me7ReadState_t : uint8_t
{
  M7R_IDLE = 0,
  M7R_INIT,        // K-line init
  M7R_RAMUNLOCK,   // 0.18.0: forge the XRAM unlock words (for ECUs that refuse 0x27), then read via 0x23
  M7R_SESSION,     // raising baud + timing
  M7R_SECURITY,    // seed/key
  M7R_PROGRAMMING, // entering programming session
  M7R_MIRRORCHECK, // deciding 512 KB vs 1 MB
  M7R_READING,     // streaming blocks
  M7R_VERIFY,      // re-reading the first block to confirm
  M7R_DONE,
  M7R_ERROR
};

struct Me7ReadStatus
{
  me7ReadState_t state;
  uint8_t percent;      // 0..100 over the whole read
  uint32_t bytesRead;   // bytes written to the file so far
  uint32_t bytesTotal;  // expected size (512 KB or 1 MB)
  uint32_t baud;        // negotiated K-line baud
  uint32_t elapsedMs;   // since start
  uint32_t etaMs;       // estimate to completion (0 until it can be estimated)
  char ecuId[48];       // ECU hardware/software id read at connect
  char file[48];        // the .bin the app saved it to ("" = not saved: download it from memory)
  char message[64];     // last human-readable status / error
};

// Parameters for a read. Defaults match a 1.8T ME7.5 (06A906032xx) / TT 8N (8N0906018xx), both 1 MB.
struct Me7ReadParams
{
  bool fastInit = true;         // fast init works on ME7.1; ME7.5 usually needs slow init (set false)
  uint8_t address = 0x01;       // the wake-up (init) address
  uint8_t target = 0;           // 0.17.4: the address the requests go to after it; 0 = the same as address
  uint32_t baud = 56000;        // target K-line baud (10400 falls back automatically if refused)
  uint32_t flashBase = 0x800000;// ME7 flash window base
  uint32_t flashSize = 0x100000;// 1 MB; the mirror check drops it to 512 KB if the chip is smaller
  bool mirrorCheck = true;      // detect a 512 KB chip mirrored into the 1 MB window
  bool verify = true;           // re-read the first 64 bytes at the end and compare them with the dump
  const char *outDir = "/reads"; // where the app saves the .bin (when there's room)

  // 0.18.0: the non-destructive RAM-unlock read (for ECUs that refuse 0x27). Defaults = the VR6 022 image values.
  bool ramUnlock = false;       // true = forge the XRAM unlock words and read with 0x23 instead of logging in
  uint32_t permitAddr = 0xE048; // XRAM word the 0x23 handler checks before it serves the flash window (universal)
  uint16_t permitValue = 0x2620;// per-ECU gate value, ONLY used for the fallback forge (022 0x2620; 06A..HJ 0x26A2; 8N0 0x25B4)
  uint32_t grantAddr = 0xE074;  // XRAM KWP2000 status word; the "security granted" bit lives here
  uint16_t grantMask = 0x8000;  // the granted bit (bit 15) a correct key would set
};

// Kick off a read on the K-line task (see can2gauge_ME7Read.cpp). Returns false if one is already running.
bool me7ReadStart(const Me7ReadParams &params);
void me7ReadCancel();               // ask the running read to stop at the next block boundary
bool me7ReadBusy();
const Me7ReadStatus &me7ReadStatusGet();
const char *me7ReadStateName(me7ReadState_t state);

// Run one step of the read state machine. Call repeatedly from the K-line task while me7ReadBusy().
// Blocks for up to one transfer block (tens of ms). Returns false when the read has finished (done or error).
bool me7ReadPump();

// 0.17.4: the last complete read, in PSRAM: its bytes, and length set to its size (512 KB or 1 MB). NULL with length 0
// until a read has finished; it stays until the next read starts.
const uint8_t *me7ReadDump(uint32_t &length);

// The VAG "external RAM" seed->key transform (NefMoto GenerateVAGExternalRAMSendKey). Exposed for a unit test.
uint32_t me7ReadKeyFromSeed(uint32_t seed, uint8_t keyIndex = 0x3F, uint8_t numLoops = 5);
