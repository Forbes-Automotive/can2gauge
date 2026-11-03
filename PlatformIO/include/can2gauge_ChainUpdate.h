#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// Chain update (0.25.0, docs/can2gauge_GaugeChain_design.md section 7). A gauge that has just been updated passes its
// own firmware and web UI files on to the other gauges in the chain over ESP-NOW, one gauge at a time.
//   - Firmware: read straight out of this gauge's running slot and written into the other gauge's spare slot. That
//     gauge only switches over once the whole image is in and its fingerprint (SHA-256) matches ours, so a power cut
//     on the way leaves it on its old firmware. Skipped when it already runs the same firmware.
//   - Web UI files: only the files listed in /fs_manifest.txt (the release's own files, written by tools/fs_pack.py)
//     are compared, and only the ones that differ are sent. Each gauge's own files (.ecu, log.cfg, logs) are left
//     alone. Files a new release dropped are deleted.
//   - The start of the job and every file carry a code made with the chain's built-in key (CHAIN_KEY, 0.28.3; was the
//     user's command key). The car must be parked (no RPM, no road speed).
// Wire format: 'C' 'U' packets, one request at a time, each answered by the other gauge (unicast) - see the .cpp.
// ---------------------------------------------------------------------------

#define CHAIN_UPDATE_PROTO 1  // sent in the heartbeat: 0 = a gauge that can't take a chain update (before 0.25.0)
#define CHAIN_UPDATE_RESULTS 16 // gauges listed in the web UI's result list
#define CHAIN_SETUP_FILE "/fbsetup.json" // 0.38.7: First Boot's setup for the other gauges (POST /api/chain/setup)

// how one gauge got on in the last "Update All Gauges"
struct ChainUpdateResult
{
  uint8_t node;   // its node id
  bool finished;  // done with it (worked or not)
  bool worked;    // updated (or was already up to date)
  char text[64];  // what happened, for the web UI
};

// what the web UI shows (GET /api/chain "update")
struct ChainUpdateStatus
{
  bool sending;       // this gauge is updating the others
  bool receiving;     // this gauge is being updated by another one
  uint8_t percent;    // progress of the gauge being worked on
  char text[80];      // what's happening right now
  uint8_t resultCount; // entries in results[]
  ChainUpdateResult results[CHAIN_UPDATE_RESULTS];
};

void setupChainUpdate();                                                    // the queue for incoming packets
void chainUpdateOnEspNow(const uint8_t *mac, const uint8_t *data, int len); // a 'C' 'U' packet (from the WiFi task)
bool chainUpdateStart(String &reason); // web UI "Update All Gauges": this gauge sends, or (0.39.5) asks the gauge with newer
                                      // firmware to
void chainUpdateStop();                                                     // web UI "Stop"
bool chainUpdateBusy();                                                     // sending or receiving (no sleep, no web update)
void chainUpdateGetStatus(ChainUpdateStatus &out);                          // a copy for the web UI
uint8_t chainUpdateBoardId();                                               // which board this firmware is built for
bool chainSetupStart(String &reason); // 0.38.7: First Boot's setup (in /fbsetup.json) to every other gauge in the chain
void chainSetupStartPage();           // 0.38.7: this gauge's start page by its place in the row (a shared setup)
void chainUpdateVersion(uint8_t version[3]);                                // CAN2GAUGE_VERSION as 3 numbers
