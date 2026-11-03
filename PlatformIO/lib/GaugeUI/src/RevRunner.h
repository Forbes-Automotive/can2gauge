#pragma once
// ---------------------------------------------------------------------------
// Rev Runner (docs/mascot/RUNNER.md, prototype docs/mascot/rev_runner.html): the screensaver's easter egg. A double-tap
// on the screensaver wakes Rev, then a tap jumps the cones and speed bumps. Long press (or the game-over screen idling
// for 20 s) leaves. Rev's frames come from the sprite pack, decoded at half size; the world is plain LVGL objects.
// Call everything with the LVGL lock held (GaugeUI does).
// ---------------------------------------------------------------------------
#include <lvgl.h>
#include <stdint.h>

lv_obj_t *runnerOpen(const char *pack, uint32_t accent); // a new screen with the game (NULL = no usable pack); load it yourself
void runnerTick(uint32_t accent, bool night);             // from uiTick while its screen shows
bool runnerWantsOut();                                    // long press, or game over idling for 20 s
