#pragma once
#include <lvgl.h>
#include "Display_ST7701.h"
#include "CST8xx_Touch.h"

// LVGL owns one core (task on core 1). Anything that touches LVGL objects from another task MUST hold the lock:
//   if (lvglLock(50)) { ...; lvglUnlock(); }
// direct = draw straight into the RGB panel's two PSRAM frame buffers and swap them on vsync (no copy, no tearing);
// else partial: draw 40-line strips in internal RAM and copy each into the panel's frame buffer (0.11.0: either, set
// at boot, compared with the Diag benchmark)
bool lvglInit(const DisplayPins &pins, uint8_t touchAddr, int8_t touchInt, uint8_t touchExioReset, bool direct = false);
bool lvglDirect(); // the mode it is running in
// 0.24.4 (boot log): where the RGB frame buffers are, whether direct mode was refused because they aren't on an
// LV_DRAW_BUF_ALIGN boundary (LVGL won't draw into them), and whether partial mode's strips had to go in PSRAM
uint32_t lvglFrameBufferAddress();
uint32_t lvglFrameBuffer2Address();
bool lvglDirectRefused();
bool lvglPartialInPsram();
void lvglStartTask(UBaseType_t priority = 4, BaseType_t core = 1);
bool lvglLock(uint32_t timeoutMs);
void lvglUnlock();
lv_display_t *lvglDisplay();
lv_indev_t *lvglTouch();
bool lvglTouchPresent();
uint32_t lvglFps();
bool lvglReady();         // false = the display didn't come up; the gauge runs without the screen (lvglLock refuses)
void lvglTouchRedetect(); // look for the touch controller again (done in the LVGL task at its next touch read)
// 0.38.0: a pretend finger (Dashboard > Gauge Control, until the screens have touch), through the touch input: a tap,
// double-tap or long press (held longHoldMs) in the middle of the screen, or a swipe across it
#define LVGL_FAKE_TAP 1
#define LVGL_FAKE_DOUBLE_TAP 2
#define LVGL_FAKE_LONG_PRESS 3
#define LVGL_FAKE_SWIPE_LEFT 4  // right to left: the next page / menu item
#define LVGL_FAKE_SWIPE_RIGHT 5 // left to right: the previous one
void lvglFakeTouch(uint8_t kind, uint16_t longHoldMs);

// what drawing costs, over the last whole second (0.11.0 frame-rate work; web UI > Diag)
struct LvglPerf
{
  uint32_t seq;        // bumped every second (a new sample)
  uint32_t fps;        // frames drawn (LVGL redraws only what changed: a still screen reads ~0)
  uint32_t frameAvgUs; // one frame: render + copy to the panel, average
  uint32_t frameMaxUs; // ... the slowest
  uint32_t flushUs;    // time spent copying to the panel, in the second
  uint32_t pixels;     // pixels redrawn in the second (480 x 480 = 230400 per full frame)
  uint8_t busyPct;     // the LVGL task inside lv_timer_handler, share of the second
};
const LvglPerf &lvglPerf();
// benchmark: redraw the whole screen every refresh (worst case: how fast a full frame can go)
void lvglForceRedraw(bool on);
// 0.24.23: screen changes played by the driver - the new screen is drawn once, then the slide is plain copies of two
// still pictures (LVGL's own animations redraw both screens every frame: 4-5 jumpy frames). Call with lvglLock held;
// they block for the transition. false = not possible here: use lv_screen_load_anim instead.
#define LVGL_SLIDE_LEFT 0  // the new screen comes in from the right, the old one goes out to the left
#define LVGL_SLIDE_RIGHT 1 // the new screen comes in from the left
#define LVGL_COVER_UP 2    // the new screen comes up from the bottom, over the old one
#define LVGL_COVER_DOWN 3  // the new screen comes down from the top, over the old one
bool lvglSlideTo(lv_obj_t *screen, uint8_t how, uint32_t ms, bool deleteOld);
bool lvglDipTo(lv_obj_t *screen, uint32_t ms, bool deleteOld); // backlight down, swap, backlight up
bool lvglFadeTo(lv_obj_t *screen, uint32_t ms, bool deleteOld); // 0.24.24: a true cross-fade (pixels mixed)
uint32_t lvglLastTransitionFrames(); // the last slide's (or cross-fade's) frames ...
uint32_t lvglLastTransitionMs();     // ... and how long it took
