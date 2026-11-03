#pragma once
// ---------------------------------------------------------------------------
// GaugeSprite - plays a C2GS sprite pack (include/c2gs.h, docs/SPRITES.md) on an LVGL screen: pages with their Sprite
// switch on, the screensaver and the shutdown / sleep / wake screen. GaugeUI owns the screens; this owns the pack.
//   - frames stream from the pack file (only the tables and palettes stay in RAM); a decode task on core 0 reads and
//     decodes the next frame into a back buffer while LVGL draws the front one, and the UI swaps them (0.11.0); a
//     layered pack is themed with the accent as it is decoded
//   - the clip is picked by the pack's rules: events (once, over the mood) first, then the first mood whose rule
//     holds; a tap plays the pack's tap clip; GaugeUI forces the shutdown / sleep / wake clips
//   - words over a clip ("REDLINE", "zzz") are LVGL labels that pop in and jitter as the pack says
// Call everything with the LVGL lock held (GaugeUI does).
// ---------------------------------------------------------------------------
#include <lvgl.h>
#include <can2gauge_defs.h>

enum spriteMode_t : uint8_t
{
  SPR_PAGE = 0, // a sprite page: moods for the page's signal
  SPR_SAVER,    // the screensaver: the saver loop (moods marked for the saver first, with "react" on)
  SPR_DARK,     // the shutdown / sleep / wake screen: only what spritePlay() starts
};

struct SpriteContext
{
  uint8_t mode;     // spriteMode_t
  uint8_t pageSignal;  // signalId_t the page shows (picks the pack's family of moods)
  bool valid;       // the page's value is fresh
  float value;      // the page's value as the dial shows it (metric)
  float minVal, maxVal, redline;
  bool react;       // SPR_SAVER: moods marked for the saver play over the saver loop
  uint32_t accent;  // the theme's accent 0xRRGGBB (layered packs)
};

struct Sprite;

// the pack to use: `want` if it is on the gauge, else the first in /sprites (by name). false = none at all.
bool spriteResolve(const char *want, char *out);
// a player for pack `name` ("" = the first on the gauge), its canvas centred on parent. NULL = no usable pack.
Sprite *spriteOpen(lv_obj_t *parent, const char *name);
// frees the player; its LVGL objects belong to parent and go with it (call from the screen's delete, or after it)
void spriteClose(Sprite *sprite);
// a sprite that isn't on the glass: its frame buffers go (they come back, and the frame is decoded again, when it updates)
void spriteSleep(Sprite *sprite);
void spriteUpdate(Sprite *sprite, const SpriteContext &ctx); // from uiTick: pick the clip, advance it, draw the frame
bool spritePlay(Sprite *sprite, uint8_t role, uint8_t thenRole = 0xFF); // force a role now (then another); false = none
bool spriteDone(Sprite *sprite);                // the forced clip (and its follow-on, if it plays once) has finished
uint32_t spriteRoleMs(Sprite *sprite, uint8_t role); // how long that role's clip plays once at its speed (0 = none)
bool spriteTap(Sprite *sprite);                 // a short press: the next tap clip (false = the pack has none)

// Rev Runner (lib/GaugeUI/src/RevRunner.cpp): a player decoded at half size (a layered pack; without the pack's words),
// and clips played by hand. spriteUpdate() then just advances the clip (mode SPR_DARK, the accent).
Sprite *spriteOpenEx(lv_obj_t *parent, const char *name, bool half);
int8_t spriteRoleClip(Sprite *sprite, uint8_t role);             // the clip of the first state with that role, -1 = none
int8_t spriteRuleClip(Sprite *sprite, uint8_t sig, float value); // the clip of the first mood whose rule on `sig` holds at value
uint16_t spriteClipFrames(Sprite *sprite, int8_t clip);
// play frames from..to of a clip at fps (0 = hold the frame), looping or once (then holding `to`); asking again for the
// same frames only changes the speed / looping
void spriteClip(Sprite *sprite, int8_t clip, uint16_t from, uint16_t to, uint8_t fps, bool loop);
bool spriteClipDone(Sprite *sprite);
// how the frame goes onto what's behind it: additive = black is see-through (the ground line shows behind Rev), and a
// scale about its centre (256 = 1:1)
void spriteSetLook(Sprite *sprite, bool additive, uint16_t scaleX, uint16_t scaleY);
// decode cost since the last call (frames decoded, total and slowest us, and the unzip part of the total - 0.24.7), for
// the frame-rate figures; resets them
void spriteDecodeStats(uint32_t &count, uint32_t &totalUs, uint32_t &maxUs, uint32_t &unzipUs);
