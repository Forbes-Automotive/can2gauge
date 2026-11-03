#pragma once
// ---------------------------------------------------------------------------
// Drag timer (0.38.0, menu > Drag Timer): 0-100 km/h, or 0-60 mph when the speed row is set to mph.
//   tap           start: a 5 second countdown under a raised flag, then the flag drops and the clock runs (it drops
//                 early, and the clock starts, the moment the car moves)
//   the run       the ring round the edge fills with the speed; at the target the chequered flag lifts and the time
//                 goes on the scoreboard (src/can2gauge_Drag.cpp, the best 10 of each kind)
//   tap           while counting down or running: stop (nothing is saved)
//   double-tap    the scoreboard, with Clear Scores (tap it twice); a tap goes back
//   long press    the menu
// The finish time is worked out between the two speed readings either side of the target, so it doesn't depend on the
// screen's frame rate; how good it is depends on how often the speed arrives (CAN: many a second, OBD: a few).
// Call everything with the LVGL lock held (GaugeUI does).
// ---------------------------------------------------------------------------
#include <lvgl.h>
#include <stdint.h>

// the theme's colours, from GaugeUI's palette
struct DragColours
{
  uint32_t bg, panel, text, muted, accent, accent2, ok, danger;
};

lv_obj_t *dragOpen(const DragColours &colours); // a new screen with the timer, ready; load it yourself
void dragTick();                                // from uiTick while its screen shows
void dragClosed();                              // its screen is being deleted (a theme change): forget its objects
