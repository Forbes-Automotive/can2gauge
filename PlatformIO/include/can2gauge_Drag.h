#pragma once
#include <can2gauge_defs.h>

// Drag timer: the scoreboard. The screen (menu > Drag Timer, lib/GaugeUI/src/DragTimer.cpp) times 0-100 km/h, or
// 0-60 mph when the speed row shows mph, and hands each finished run to dragAddRun(). The best DRAG_SCORES times of
// each kind are kept here, in NVS under their own name ("c2gdrag"), not in the settings blob: a change to the settings
// layout resets every setting, so the scores stay out of it (and a settings reset leaves them).
// The web UI shows and clears them (GET /api/drag, actions dragClear / dragDelete).

// ===== Settings =====
#define DRAG_KINDS 2    // the two runs timed
#define DRAG_KIND_KMH 0 // 0-100 km/h
#define DRAG_KIND_MPH 1 // 0-60 mph
#define DRAG_SCORES 10  // best times kept of each kind

// one run on the scoreboard (saved to NVS as raw bytes inside the board, so the fields stay in this order)
struct DragRun
{
  uint32_t timeMs;    // 0 to the target speed, ms
  uint32_t runNumber; // which run of this kind it was (1 = the first ever timed)
};

// ===== Set-up =====
void setupDrag(); // loads the scoreboard from NVS (in setup)

// ===== The screen: a finished run =====
uint8_t dragAddRun(uint8_t kind, uint32_t timeMs); // a finished run: its place on the board 1..DRAG_SCORES, 0 = not on it

// ===== Reading the board (screen and web UI) =====
uint8_t dragScoreCount(uint8_t kind);           // times on the board for this kind
DragRun dragScore(uint8_t kind, uint8_t place); // place 0 = the best
uint32_t dragRunCount(uint8_t kind);            // runs of this kind finished so far

// ===== Clearing it (web UI) =====
void dragClear(uint8_t kind);                 // empties this kind's board (the run count stays)
void dragDelete(uint8_t kind, uint8_t place); // takes one time off the board (place 0 = the best)
