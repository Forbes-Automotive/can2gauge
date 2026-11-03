#pragma once
// ---------------------------------------------------------------------------
// C2GS - can2gauge sprite pack (docs/SPRITES.md). One file per pack on LittleFS: /sprites/<name>.c2gs, written by
// tools/sprite_convert.py. Read by GaugeUI (lib/GaugeUI/src/GaugeSprite.cpp: sprite pages, the screensaver, the
// shutdown / sleep / wake screen, the row wave) and the web API (the pack list). All numbers little-endian.
//
//   header      32 bytes                C2gsHeader
//   clips       clipCount x 32 bytes    C2gsClip: a run of frames, its box on the canvas, its palette, its glow
//   states      stateCount x 24 bytes   C2gsState: a clip, when it plays (role + rule) and how fast
//   texts       textCount x 32 bytes    C2gsText: words the firmware draws over a clip ("REDLINE", "zzz")
//   frames      frameCount x 20 bytes   C2gsFrame: where each frame's planes are (identical frames share data)
//   palettes, plane data
//
// The canvas (w x h) sits centred on the glass; a clip covers its box (x, y, w, h) of it. Frames are stored at
// 1 / header.scale of the box size (rounded up) and scaled up smoothly on the gauge. Two encodings:
//   baked    (no C2GS_LAYERED) plane 0 = RGB565, plane 1 = A8 alpha when C2GS_ALPHA (LVGL RGB565A8), else opaque
//   layered  (C2GS_LAYERED) themed at draw time, everything over black:
//            plane 0 = one palette index per pixel; a palette entry (C2gsPal) is a literal colour (premultiplied:
//                      white body, black face, red / ice details), an accent weight (theme-coloured details) and a
//                      cover (how much of the glow the body hides)
//            plane 1 = A8 glow at an eighth of the stored size (ceil(w / 8) x ceil(h / 8)), scaled up bilinearly
//            pixel = literal + accent x ACCENT + glow x (255 - cover) / 255 x gain x GLOW (each channel saturating)
//            ACCENT is the theme's accent, GLOW the clip's glow colour (the accent, or a fixed colour)
// Planes are RLE-packed (C2GS_RLE, always set by the converter) with the C2GL logo packets (GaugeUI loadLogo):
//   c < 0x80: c + 1 literal values follow;  c >= 0x80: the next value repeated (c & 0x7F) + 2 times
//   (2-byte values in an RGB565 plane, 1-byte values otherwise)
//
// Version 2 (0.24.0, C2GS_DEFLATE, written by tools/c2gs_pack.py): every frame is also zipped on its own (raw deflate
// of its planes back to back), so a frame unzips by itself just before it is decoded - the pack is never unpacked as a
// whole. In the frame table len[0..2] stay the planes' RLE sizes (as unzipped) and len[3] is the zipped size stored at
// offset (plane 3 is never used). Rev went from 5.9 MB to 2.8 MB. Version 1 packs (no zip) still play.
// ---------------------------------------------------------------------------
#include <stdint.h>

#define C2GS_MAGIC "C2GS"
#define C2GS_VERSION 1         // a plain pack
#define C2GS_VERSION_ZIPPED 2  // 0.24.0: frames zipped (C2GS_DEFLATE set); firmware before 0.24.0 refuses it
#define C2GS_ALPHA 0x01   // baked: an A8 alpha plane follows the RGB565 plane
#define C2GS_RLE 0x02     // planes are RLE-packed
#define C2GS_LAYERED 0x04 // the themed palette encoding above
#define C2GS_DEFLATE 0x08 // version 2: each frame's planes zipped together (raw deflate), len[3] = the zipped size
#define C2GS_MAX_CLIPS 48
#define C2GS_MAX_STATES 48
#define C2GS_MAX_TEXTS 32
#define C2GS_MAX_SIDE 480
#define C2GS_GLOW_DIV 8   // layered: the glow plane's size divisor (of the stored size)
#define SPRITE_NAME_LEN 16 // a pack is /sprites/<name>.c2gs, name up to 15 characters

// when a state plays
enum c2gsRole_t : uint8_t
{
  C2GS_MOOD = 0,     // a loop, picked by its rule: the first mood (in file order) whose rule holds plays
  C2GS_EVENT = 1,    // played once over the mood when its rule fires (C2GS_RISE / C2GS_DROP), then back to the mood
  C2GS_TAP = 2,      // played once on a short press of a sprite page (several: taken in turn)
  C2GS_SAVER = 3,    // the screensaver's loop (with "react to the car" on, moods marked C2GS_IN_SAVER come first)
  C2GS_SHUTDOWN = 4, // ignition off, played once
  C2GS_SLEEP = 5,    // after the shutdown clip, looped while the backlight fades out
  C2GS_WAKE = 6,     // ignition back on from standby, played once before the gauge page
  C2GS_ROW_LEFT = 7, // the row wave (0.12.0): looks at and waves to the gauge on its left, played once; its last frame
                     // (looking left, arm down) is held while the wave travels along the row to the left
  C2GS_ROW_RIGHT = 8,// the same toward the gauge on its right. Firmware before 0.12.0 refuses a pack with these roles
  c2gsRole_t_MAX
};

// a mood's / event's rule. v = signal `sig` (0 = the page's own value)
enum c2gsCond_t : uint8_t
{
  C2GS_ALWAYS = 0,  // always holds (a fallback: put it last)
  C2GS_RANGE = 1,   // lo <= v < hi, in the signal's metric units
  C2GS_PERMILLE = 2,// lo <= v < hi in permille of the page's min..max (generic packs; hi >= 1000 includes the top)
  C2GS_RISE = 3,    // event: v rises through lo
  C2GS_DROP = 4,    // event: v falls by hi or more within a second, from lo or above (a blow-off valve)
  C2GS_REDLINE = 5, // the page's value is at or above the page's redline
  C2GS_NODATA = 6,  // the page's value is stale / not there yet
  c2gsCond_t_MAX
};
#define C2GS_ONCE 0x01     // state flag: play once and hold the last frame (else loop)
#define C2GS_IN_PAGE 0x02  // the rule applies on a sprite page (only when the page shows signal `family`, if set)
#define C2GS_IN_SAVER 0x04 // the rule applies on the screensaver with "react to the car" on (family ignored)
#define C2GS_GLOW_ACCENT 0x01000000UL // C2gsClip.glow / C2gsText.colour: the theme's accent, not the RGB below

struct __attribute__((packed)) C2gsHeader
{
  char magic[4];      // "C2GS"
  uint8_t version;    // 1
  uint8_t flags;      // C2GS_ALPHA | C2GS_RLE | C2GS_LAYERED
  uint16_t w, h;      // canvas, up to 480 x 480; centred on the glass
  uint16_t frameCount;
  uint8_t stateCount; // 1..C2GS_MAX_STATES
  uint8_t textCount;  // 0..C2GS_MAX_TEXTS
  uint8_t clipCount;  // 1..C2GS_MAX_CLIPS
  uint8_t scale;      // frames are stored at 1 / scale of their box (1 or 2)
  char name[16];      // display name, NUL-padded ("Rev")
};

struct __attribute__((packed)) C2gsClip
{
  uint16_t firstFrame; // index into the frame table
  uint16_t frameCount;
  uint16_t x, y, w, h; // box on the canvas (full size; stored at 1 / scale)
  uint16_t palCount;   // layered: palette entries (1..256)
  uint8_t glowGain;    // layered: glow strength x 64 (64 = 1.0)
  uint8_t reserved1;
  uint32_t palOffset;  // layered: from the start of the file, palCount x C2gsPal
  uint32_t glow;       // layered: glow colour 0xRRGGBB, or C2GS_GLOW_ACCENT
  uint8_t reserved[8];
};

struct __attribute__((packed)) C2gsPal
{
  uint16_t lit;  // RGB565, premultiplied
  uint8_t acc;   // accent weight 0..255
  uint8_t cover; // 0..255: how much of the glow this pixel hides
};

struct __attribute__((packed)) C2gsState
{
  uint8_t clip;     // index into the clips
  uint8_t role;     // c2gsRole_t
  uint8_t flags;    // C2GS_ONCE | C2GS_IN_PAGE | C2GS_IN_SAVER
  uint8_t fpsLo;    // frames per second (C2GS_PERMILLE: at lo)
  uint8_t fpsHi;    // C2GS_PERMILLE: at hi (= fpsLo for a fixed speed)
  uint8_t sig;      // signalId_t the rule reads, 0 = the page's own value
  uint8_t family;   // C2GS_IN_PAGE: only on a page showing this signalId_t (0 = any page)
  uint8_t cond;     // c2gsCond_t
  float lo, hi;     // the rule's numbers
  uint8_t reserved[8];
};

enum c2gsTextKind_t : uint8_t
{
  C2GS_WORD = 0, // a word that pops in (scale 0 -> 1.25 -> 1 over 140 ms), re-pops every `repop` frames, jitters
  C2GS_ZZZ = 1,  // three small z's rising and drifting right from (x, y), staggered, fading, growing (2.2 s cycle)
};

struct __attribute__((packed)) C2gsText
{
  uint8_t clip;         // index into the clips
  uint8_t kind;         // c2gsTextKind_t
  uint16_t from, to;    // frames of that clip it shows on (inclusive; to >= the clip length = to the end)
  char text[12];        // NUL-padded
  int16_t x, y;         // centre, from the canvas centre, y down
  uint8_t size;         // px
  int8_t rot;           // degrees
  uint8_t jitter;       // +- px
  uint8_t repop;        // pop in again every this many frames (0 = once)
  uint32_t colour;      // 0xRRGGBB or C2GS_GLOW_ACCENT
  uint16_t reserved;
};

struct __attribute__((packed)) C2gsFrame
{
  uint32_t offset;  // from the start of the file
  uint32_t len[4];  // bytes of planes 0..3 as stored, back to back from offset (0 = plane absent). Version 2: len[3]
                    // is the zipped size of the whole frame at offset, len[0..2] the planes once unzipped
};

static_assert(sizeof(C2gsHeader) == 32 && sizeof(C2gsClip) == 32 && sizeof(C2gsPal) == 4 && sizeof(C2gsState) == 24 &&
                  sizeof(C2gsText) == 32 && sizeof(C2gsFrame) == 20,
              "C2GS layout");
