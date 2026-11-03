#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// Gauge pages from a chosen source (web UI > Gauges > Source). A page value that isn't one of the gauge's own decoded
// signals is a user signal: page n's main value is SIG_USER_1 + 2n, its second value SIG_USER_2 + 2n, configured in
// settings.pages[n].valueSigs[0 / 1]:
//   CAN     a signal of the DBC on the gauge, decoded the moment its frame arrives (userSigOnCanFrame)
//   K-line  a measuring-block field: while the page shows - or all the time when every page's K-line values share one
//           module (0.15.0) - the K-line task holds a session to the module and reads the block(s) (runKlinePage in
//           can2gauge_KLine.cpp)
//   OBD     a mode 01 PID, from the Bluetooth ELM327 (connected while a page in the rotation uses it, 0.15.0) or the
//           wired OBD profile
//   ECU     any channel of an Ignitron / MaxxECU, sent over WiFi by usb2ip (src/can2gauge_ECU.cpp)
//   ME7     0.17.6: a variable of the ME7 logger's .ecu by name (K-line source with the ME7 logger picked): the logger
//           logs it along with the .cfg's variables (runMe7 in can2gauge_KLine.cpp)
// The page's label names the signal; the unit comes from the source (DBC, KWP1281 formula, PID).
// 0.35.0: a trigger can watch a page value too (Triggers tab, "Page values"). A switched-on trigger keeps its value read
// even when its page isn't showing or is switched off (userSigWanted), so the trigger can still fire.
// ---------------------------------------------------------------------------
#include "driver/twai.h" // twai_message_t (the CAN frame)

void userSigApply();                       // settings changed: names / units, caches
void userSigTick();                        // ~20 Hz: finds each DBC signal by name, and catches any frame missed below
void userSigOnCanFrame(const twai_message_t &frame); // CAN RX task: decodes the page's DBC signals the moment their frame arrives
// the OBD PIDs a page shows (its gauge signals fed by a PID, and its OBD user signals): fills pids, returns how many
uint8_t userSigPagePids(uint8_t cfgIndex, uint8_t *pids, uint8_t maxPids);
void userSigOnObdPid(uint8_t pid, signalId_t catalogueId, float value); // every decoded OBD PID (ELM or wired)
const UserSigCfg *userSigConfig(signalId_t id); // NULL unless id is a configured user signal
bool userSigPageUses(uint8_t cfgIndex, uint8_t source); // does this page read a user signal from source (userSigSrc_t)?
bool userSigRotationUses(uint8_t source); // 0.15.0: does any page in the rotation (enabled), or (0.35.0) any switched-on trigger, read a user signal from source?
bool userSigInRotation(uint8_t userSigIndex); // 0.17.6: does an enabled page show this user signal (0..USER_SIGS_ALL-1)?
bool userSigOnTrigger(uint8_t userSigIndex);  // 0.35.0: does a switched-on trigger watch this user signal?
bool userSigWanted(uint8_t userSigIndex);     // 0.35.0: on an enabled page or a switched-on trigger: its source keeps reading it
bool userSigDiagValueWanted(uint8_t valueIndex); // 0.37.0: VAG diag value valueIndex + 1 is on an enabled page or a trigger
// 0.35.0: the OBD PIDs the switched-on triggers watch (their OBD user signals), added to pids after `count` (no doubles,
// up to maxPids); returns the new count
uint8_t userSigTriggerPids(uint8_t *pids, uint8_t count, uint8_t maxPids);
// 0.15.0: is this value's source still opening its connection (K-line init, OBD adapter, VAG diag)? For the spinner
bool userSigConnecting(signalId_t id);
// why a user signal has no value yet, for under the gauge ("Waking module 0x01 (5-baud init)", "No DBC on the gauge");
// false for a catalogue signal
bool userSigNote(signalId_t id, char *buf, size_t len);
// decimals a user signal's source says to show it with (an ECU channel's list row); false = no opinion
bool userSigDecimals(signalId_t id, uint8_t &decimals);
