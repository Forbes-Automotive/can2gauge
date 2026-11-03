#pragma once
#include <can2gauge_defs.h>

// Boot sync: the gauges in a row power up together but each takes a slightly different time to start, so their boot
// logos would come up one after the other. CAN is up well before the logo (WiFi isn't), so the gauges agree over CAN
// when to show it:
//
//   1. hold     each gauge builds its logo with the screen dark and says READY (its id, and how many gauges it expects)
//               every 10 ms.
//   2. go       a gauge that has heard READY from as many gauges as it expects (itself included) says GO: "start in
//               150 ms", counting down every 10 ms. A gauge that hears GO starts at the same moment. Nobody waits more
//               than BOOT_HOLD_MAX_MS: a missing gauge doesn't keep the others dark.
//   3. start    each gauge sets its logo to start from the beginning at that moment and draws the first frame a
//               little ahead, then the backlight comes on at the moment itself - every gauge together.
//   4. catch-up for the first 3.5 s of the logo each gauge sends its logo time every 20 ms, and one that is behind (it
//               missed the GO, or was too slow for the hold) jumps forward to match.
//
// How many gauges to expect is learnt: the number heard this boot (ids in any boot sync frame), saved with the
// settings for next time. A gauge on its own, or a new gauge, expects 1 and starts at once.
//
// These frames go out even with "link frames on CAN" off (settings.linkCanTx): only at power-up, on the chain's own
// id, which nothing in the car listens to. After that the gauge goes back to the setting (off = nothing more on the
// car's CAN).
//
//   CAN BOOT SYNC  base+0x21  [group lo][group hi][value lo][value hi][type][id lo][id hi][expected]
//       type 0 LOGO TIME  value = how long its logo has been up, ms (gauges on older firmware send only this, with id 0)
//       type 1 READY      value = 0; expected = how many gauges it waits for, itself included
//       type 2 GO         value = ms until every gauge starts its logo
//       type 3 TERM       value = 0: "I've switched my 120R on - the bus is live, leave yours off"
//
// Auto CAN termination (settings.canTermination = CAN_TERM_AUTO): a CAN bus needs 120R at its ends. In a car the car's
// own wiring has them, so a gauge must not add another; on the bench (gauges on their own) nothing does, and no frame
// gets across. So at power-up each gauge listens for CAN_TERM_LISTEN_MS (a little longer or shorter per gauge, so they
// don't all decide together). Any frame heard: the bus is alive and terminated - leave it off. Nothing: switch this
// gauge's 120R on and send TERM, so the others still listening hear a live bus and leave theirs off. If car frames turn
// up later (the car's bus woke after the gauge), it goes off again for the rest of the drive.

// ===== The boot sync frame =====
#define BOOT_SYNC_ID_OFFSET 0x21 // CAN id = settings.linkCanBaseId + this (0x6E1 by default; the link's own frames
                                 // stop at + 0x20)
#define BOOT_SYNC_LOGO_TIME 0    // frame type (byte 4): how long my logo has been up
#define BOOT_SYNC_READY 1        // frame type: my logo is built, I'm waiting for the others
#define BOOT_SYNC_GO 2           // frame type: every gauge starts its logo in this many ms
#define BOOT_SYNC_TERM 3         // frame type: I've switched my 120R on
#define BOOT_SYNC_MAX_GAUGES 8   // gauges remembered from the frames heard

// ===== Boot logo timings =====
#define BOOT_SYNC_HOLD_EVERY_MS 10 // READY / GO this often while holding (Adam: 10-20 ms)
#define BOOT_SYNC_COUNTDOWN_MS 150 // GO: every gauge starts this long after the first GO ...
#define BOOT_SYNC_PREPARE_MS 120   // ... and draws its logo's first frame this long before, so the start itself is only
                                   // the backlight coming on (drawing a frame can wait on the LVGL task)
#define BOOT_HOLD_MAX_MS 1500      // never dark longer than this waiting for the others (one missing / not on CAN)
#define BOOT_LOGO_WAIT_MS 4000     // the logo itself not up this long after setupBootSync: give up, backlight on
#define BOOT_SYNC_EVERY_MS 20      // catch-up: logo time this often ...
#define BOOT_SYNC_WINDOW_MS 3500   // ... for this long from the logo starting (the logo is up 3.7 s)
#define BOOT_SYNC_SLACK_MS 15      // closer than this to the other gauge already: no jump (stops tiny jitters)

// ===== Auto termination timings =====
#define CAN_TERM_LISTEN_MS 1200       // listen this long for any frame ... (over 1 s: an unanswered gauge only gets one
                                      // frame a second out, CANLine's quiet-bus rule)
#define CAN_TERM_STAGGER_MS 40        // ... plus this x (id & 7), so the gauges decide one at a time
#define CAN_TERM_ANNOUNCE_MS 2000     // after switching on: TERM this long (when there's no boot logo handshake to follow)
#define CAN_TERM_ANNOUNCE_EVERY_MS 20 // TERM this often while announcing

// ===== Shared with other files =====
extern uint8_t bootGaugeCount; // gauges on CAN at the last power-up, this one included (saved by writeEEP; 0 = not
                               // known yet, counts as 1)

// ===== Functions =====
void setupBootSync();       // after powerWakeProbe(), BEFORE setupDisplay(): decides whether to hold the screen dark
bool bootSyncHoldingDark(); // true while waiting for the other gauges (backlightTarget() then gives 0)
bool bootSyncAutoTermOn();  // Auto termination has switched this gauge's 120R on
void bootSyncTermTick();    // io task - Auto termination on and car frames turn up: switch it off
