#pragma once
#include <stdint.h>

// 0.25.1: Screen rotation - the whole picture turned round the middle of the glass, for a gauge that sits slightly
// crooked in its pod (Rotation Trim, a few degrees either way) or one fitted upside down (Upside Down, 180 degrees).
//
// How it works: with a rotation set, LVGL draws into one extra screen-sized picture in PSRAM (the "canvas") instead of
// the glass's own frame buffers. At the end of every frame the canvas is copied into the frame buffer that isn't
// showing, turned by the set angle, and that buffer is swapped onto the glass. Touches are turned back the same way,
// so a tap lands on what's drawn under the finger. With no rotation set nothing of this runs (no cost at all).
//
// The cost with a rotation set: one turned copy of the round picture every frame (shows in Diag as copy time), and the
// canvas's 450 KB of PSRAM. Direct drawing mode only (web UI Diag tab > Drawing); in partial mode it is ignored.
//
// To take the feature out: set SCREEN_ROTATION to 0. Every piece of it (this file, its hooks in LVGL_Driver.cpp, the
// setting, the web card) is inside "#if SCREEN_ROTATION" or skipped when the gauge doesn't report it.
#define SCREEN_ROTATION 1 // ** CAN CHANGE THIS ** 1 = Upside Down + Rotation Trim available, 0 = left out of the build

#if SCREEN_ROTATION

// what lvglRotationState() reports (web UI card)
#define ROTATION_OFF 0     // no rotation set: drawn straight onto the glass as normal
#define ROTATION_ON 1      // turning every frame
#define ROTATION_NO_ROOM 2 // a rotation is set but there was no PSRAM for the canvas: restart the gauge to apply it
#define ROTATION_PARTIAL 3 // a rotation is set but the gauge draws in partial mode, which can't turn the picture

#define ROTATION_TRIM_MAX 15.0f // the most Rotation Trim allowed either way (degrees)

// set the rotation: upsideDown = turned 180 degrees, trimDegrees = extra turn (+ = clockwise as you look at the glass).
// Call with lvglLock held (or before the LVGL task starts). Applies from the next frame.
void lvglSetRotation(bool upsideDown, float trimDegrees);
uint8_t lvglRotationState(); // ROTATION_OFF / ON / NO_ROOM / PARTIAL

// ---- used by LVGL_Driver.cpp only -----------------------------------------------------------------------------------
void rotateSetAngle(float degrees); // work out the turn's numbers once (rotateFrame / rotateTouch use them)
void rotateFrame(const uint16_t *canvas, uint16_t *glass); // the canvas turned into a frame buffer (the round part only)
void rotateTouch(int32_t &x, int32_t &y);                  // a touch on the glass -> where it is on the canvas

#endif
