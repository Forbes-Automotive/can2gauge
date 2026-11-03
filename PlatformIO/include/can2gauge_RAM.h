#pragma once
#include <can2gauge_defs.h>
#include <ArduinoJson.h>

// Internal RAM helpers (Tiguan drive 2026-10-28: the gauge froze twice with Bluetooth OBD up and a phone on the WiFi,
// internal RAM down to 2.4 KB at its lowest):
// - The JSON replies (/api/status, /api/elm ...) built in PSRAM. ArduinoJson copies every text value into its own small
//   piece of memory, and small pieces (under 512 bytes) came from internal RAM: a 3 KB /api/elm reply took a few KB of
//   it in 50-80 byte bites. Every JsonDocument in the API is made with psramJsonAllocator() so they all go to PSRAM.
// - A note of the last time internal RAM ran out (how much was asked for, what kind, which task). It is kept through a
//   reset, printed at the next boot and shown in /api/status ("ramFail"), so a freeze leaves evidence behind.

// ===== The note of failed memory requests =====
// what the last failed memory request was (count = 0: none since power-up). Kept in RTC memory through a reset, so
// the fields stay in this order.
struct RamFailNote
{
  uint32_t count;   // failed requests since boot
  uint32_t size;    // the last one: bytes asked for
  uint32_t caps;    // the last one: the kind of memory asked for (MALLOC_CAP_... bits, 0x800 = internal)
  uint32_t uptimeS; // the last one: seconds after boot
  char task[16];    // the last one: the task that asked
};

void setupRam();                      // start noting failed memory requests; print the last boot's note if there is one
const RamFailNote &ramFailNow();      // this boot's failed requests
const RamFailNote &ramFailLastBoot(); // the boot before this one's (count = 0: none, or a power-on)

// ===== JSON in PSRAM =====
// the memory source for a JsonDocument: PSRAM first, internal RAM only if PSRAM is full.
// Use: JsonDocument doc(psramJsonAllocator());
ArduinoJson::Allocator *psramJsonAllocator();
