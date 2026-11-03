#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// can2gauge ME7 fast logging (B7 RAM routine) - 0.19.0 (in progress)
//
// The normal ME7 logger reads each sample the documented way (2C define + 21/23 reads), 3-4 requests a sample,
// ~34/s. This feature does what ME7Logger's "builtin logging" does: it writes a tiny machine-code routine into the
// ECU's RAM once, repoints one of the ECU's own function pointers at it, and then one 1-byte B7 request runs that
// routine and streams the whole configured block back - 1 request a sample, so a much higher rate.
//
// The routine is OUR OWN clean-room C166 code (see tools/me7fast / docs/me7_ramroutine_re.md 6g), assembled with
// the Ghidra C166 module. It is NOT ME7Logger's bytes and NOT NefMoto's GPL code. Addresses from an REA of the
// ECU images (16 of them): the mechanism is the same across ME7.1 / ME7.5 / 2.7T.
//
// Sequence (all over the running K-line KWP2000 session, same transport as the logger):
//   1. write the routine blob into RAM at ME7FAST_SCRATCH (0x387A00) with 0x3D WriteMemoryByAddress, in chunks
//   2. read + save the ECU's mapping-table base pointer at ME7FAST_POINTER (0xE228, an EXTP offset+page pair)
//   3. write our blob's EXTP {offset, page} into that pointer -> the ECU now runs our routine for mapping requests
//   4. poll: one 0xB7 per sample -> the routine writes the block at r12 (0xE0A8), returns the length in RL4, the
//      ECU frames the reply
//   5. on stop/error: write the saved original pointer back (un-hook); it also clears at ignition-off
//
// RISK: this writes into a live ECU's RAM and redirects an internal pointer. A wrong address or bad blob can hang
// the ECU until the ignition is cycled (RAM clears then). Bench only, engine off, until proven. All writes are
// RAM - nothing in flash changes.
//
// Two handlers are available:
//   v0 TEST LINK - writes the fixed marker "C2G!" (43 32 47 21). Install it and poll B7 -> those 4 bytes come
//     back, proving the whole write/hook/poll chain on real hardware. Use it FIRST on any new ECU.
//   v1 FAST - reads the selected variables (each a 2-byte word) and streams them back in order. This is the
//     real fast log. It carries a config block (built here from the variables' addresses) right after its code.
// me7FastInstall() picks v0 when given no variables, v1 when given some.
// ---------------------------------------------------------------------------

#define ME7FAST_SCRATCH 0x387A00u // RAM scratch the routine blob is written to (ME7.5; from the REA)
// The ECU's measurement mapping-table BASE pointer. Disassembly of the 8N (2026-10-22, docs §9.4) shows it is a
// DATA pointer the dispatch reads with EXTP: offset word at 0xE228, page word at 0xE22A (physical = page<<14 |
// (offset & 0x3FFF)). Stock it is {0x04B0, 0x0000} = the table at 0x04B0. We repoint it at our blob. It is NOT a
// linear 24-bit far pointer and NOT at 0xE226 (that address holds a computed index the dispatch overwrites) - writing
// a linear address, or at 0xE226, is what crashed/did-nothing on the bench before this was understood.
#define ME7FAST_POINTER 0xE228u   // the mapping-table base pointer (offset word; page word at 0xE228+2)
#define ME7FAST_VERADDR 0x00E1B0u // read 4 bytes here for the bootrom version, for the log only (60 02 00 00 = ver
                                  //   3/4, 00 00 86 2B = ver 1/2): it does NOT change where we hook
#define ME7FAST_CHUNK 0x30        // bytes of data per 0x3D write while uploading the blob (48). KWP2000 on K-line can
                                  // only frame a 60-byte request (kwp2000Request rejects more), and a 0x3D write is
                                  // 5 header bytes + this much data, so the data must stay <= 55. 0x80 was refused
                                  // locally before it ever reached the ECU (bench 2026-10-22).
#define ME7FAST_MAX_VARS 32       // most variables in one fast-log block (bounds the config + the B7 reply size)

enum me7FastState_t : uint8_t
{
  M7F_IDLE = 0,
  M7F_UPLOADED, // blob written + verified in RAM
  M7F_HOOKED,   // mapping-table pointer repointed at our blob
  M7F_ERROR
};

struct Me7FastStatus
{
  me7FastState_t state;
  uint8_t pointerSaved[4]; // the ECU's original mapping-table pointer (restored on un-hook)
  bool haveSaved;          // true once pointerSaved holds the original
  char message[64];        // last human-readable status / error
};

// Write the routine blob into ECU RAM, verify it, save the original pointer and repoint it at the blob.
// varCount == 0 installs the v0 marker (Test link); varCount > 0 installs v1 configured to read those
// variables (varAddrs = their 24-bit ECU addresses; the reply has 2 bytes per variable, in the same order).
// Returns true when installed + hooked. Call inside a live KWP2000 session (the K-line task).
bool me7FastInstall(const uint32_t *varAddrs, uint8_t varCount);

// Un-hook: write the saved original pointer back. Safe to call even if not hooked. Returns true if it wrote it.
bool me7FastUnhook();

// Send one 0xB7 and read the routine's block into out (up to cap bytes). Returns the byte count, or -1 on failure.
int me7FastPoll(uint8_t *out, uint8_t cap);

const Me7FastStatus &me7FastStatusGet();
