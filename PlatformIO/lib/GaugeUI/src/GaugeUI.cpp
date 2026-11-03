#include "GaugeUI.h"
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <stdarg.h>
#include <ctype.h> // 0.31.0: tolower() for the icon match
#include <LVGL_Driver.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <FsUnzip.h> // 0.24.0: the make logos ship zipped (/logos/<key>.bin.gz)
#include <can2gauge_themes.h>
#include <can2gauge_Vehicle.h> // 0.36.2: boot logo 7, My Car's picture and name
#include "GaugeIcons.h"
#include "MenuIcons.h"
#include "GaugeSprite.h" // sprite pages, the screensaver, the shutdown / wake clips
#include "RevRunner.h"   // the screensaver's easter egg (docs/mascot/RUNNER.md)
#include "DragTimer.h"   // 0.38.0: menu > Drag Timer
#include <can2gauge_UserSig.h> // pages from a chosen source: claims + "what is happening" notes
#include <can2gauge_ADC2CAN.h> // 0.36.4: adc2canPageRange - an adc2can sensor's dial range

// ---------------------------------------------------------------------------
// theme
// ---------------------------------------------------------------------------
struct Palette
{
  uint32_t accent, accent2, bg, panel, text, muted, danger, ok;
  uint32_t needle;  // STYLE_NEEDLE pointer
  uint32_t redline; // arc / bar / value colour at or above the page redline
};
// the original can2gauge palettes (theme ids 0-3)
static const Palette palettes[] = {
    {0x24d1c4, 0xffb02e, 0x000000, 0x131a22, 0xe6edf3, 0x7d8ea0, 0xff4d5e, 0x3fd07f, 0x24d1c4, 0xff4d5e}, // 0 teal (Forbes shared theme)
    {0xffb02e, 0x24d1c4, 0x000000, 0x1a1408, 0xf3ead6, 0x9a8b6a, 0xff4d5e, 0x3fd07f, 0xffb02e, 0xff4d5e}, // 1 amber
    {0xff3b4a, 0xffb02e, 0x000000, 0x1a0a0c, 0xf3d6d8, 0xa06a70, 0xffd400, 0x3fd07f, 0xff3b4a, 0xffd400}, // 2 red
    {0xffffff, 0x24d1c4, 0x000000, 0x151515, 0xffffff, 0x8a8a8a, 0xff4d5e, 0x3fd07f, 0xffffff, 0xff4d5e}, // 3 white
};
static Palette palette = palettes[0];
static bool nightMode = false;
static lv_color_t colour(uint32_t hex)
{
  return lv_color_hex(hex);
}

static lv_color_t accentColor()
{
  return nightMode ? lv_color_darken(colour(palette.accent), 60) : colour(palette.accent);
}

static lv_obj_t *plainObject(lv_obj_t *parent); // 0.24.12: further down; the page dots need it earlier

// 0.24.23: every screen change goes through here. Slides and fades are played by the display driver where it can
// (LVGL_Driver lvglSlideTo / lvglDipTo): LVGL's own animations redraw both screens every frame, ~50 ms each on this
// glass, so a 250 ms slide showed 4-5 jumpy frames. Slides: the new screen is drawn once, then moved in as a still
// picture. Fades: a cross-fade (pixels mixed); where that can't run, the backlight dips to dark and back while the
// screen is swapped. (0.24.26: the two looked the same on the glass, so the Fade style setting went.) Same arguments as
// lv_screen_load_anim, which is still used when the driver can't (partial drawing mode, no PSRAM for the pictures).
// The ignition-off fade to the dark screen keeps LVGL's fade (it has its own backlight fade).
//
// 0.30.2: the driver's slides and fades draw whole screens (lv_refr_now) in the task that asks for them, which takes
// several KB of stack. Only the tasks below have the room; any other (io 3 KB: the wake after ignition on; link 4 KB:
// another gauge's "follow page") gets LVGL's own animation instead, which the lvgl task draws. Coredump 2026-10-30: the
// io task ran out of stack in lvglFadeTo at every ignition on, on a gauge with no shutdown sprite.
static bool taskCanDrawTransitions()
{
  const char *taskName = pcTaskGetName(NULL); // the task running this
  if (!strcmp(taskName, "lvgl"))
  {
    return true; // LVGL's own task: touch, swipes, LVGL timers (the boot logo's fade)
  }
  if (!strcmp(taskName, "uiTick"))
  {
    return true; // GaugeUI's refresh task (6 KB): the screensaver, the wake clip ending, Rev Runner
  }
  if (!strcmp(taskName, "async_tcp"))
  {
    return true; // the web server (8 KB): the web UI's page and menu buttons
  }
  return false;
}

static void showScreen(lv_obj_t *screen, lv_screen_load_anim_t anim, uint32_t ms, uint32_t delayMs, bool deleteOld)
{
  bool done = false;
  if (!taskCanDrawTransitions())
  {
    done = false; // straight to LVGL's own animation below
  }
  else if (anim == LV_SCR_LOAD_ANIM_MOVE_LEFT)
  {
    done = lvglSlideTo(screen, LVGL_SLIDE_LEFT, ms, deleteOld);
  }
  else if (anim == LV_SCR_LOAD_ANIM_MOVE_RIGHT)
  {
    done = lvglSlideTo(screen, LVGL_SLIDE_RIGHT, ms, deleteOld);
  }
  else if (anim == LV_SCR_LOAD_ANIM_OVER_TOP)
  {
    done = lvglSlideTo(screen, LVGL_COVER_UP, ms, deleteOld); // the menu comes up from the bottom
  }
  else if (anim == LV_SCR_LOAD_ANIM_OVER_BOTTOM)
  {
    done = lvglSlideTo(screen, LVGL_COVER_DOWN, ms, deleteOld); // and the page comes back down over it
  }
  else if (anim == LV_SCR_LOAD_ANIM_FADE_IN)
  {
    done = lvglFadeTo(screen, ms, deleteOld); // a true cross-fade (pixels mixed)
    if (!done)
    {
      done = lvglDipTo(screen, ms, deleteOld); // partial drawing mode: the backlight dip instead
    }
  }
  if (!done)
  {
    lv_screen_load_anim(screen, anim, ms, delayMs, deleteOld);
  }
}

static lv_color_t needleColor()
{
  return nightMode ? lv_color_darken(colour(palette.needle), 60) : colour(palette.needle);
}

// every channel scaled by pct / 100, which keeps the hue (the night palette dims the same way)
static uint32_t scaleHex(uint32_t hex, uint32_t pct)
{
  return (((hex >> 16 & 0xFF) * pct / 100) << 16) | (((hex >> 8 & 0xFF) * pct / 100) << 8) | ((hex & 0xFF) * pct / 100);
}

// too close to tell apart on the glass (e.g. BMW amber needle on amber, Mazda red on red)
static bool coloursSimilar(uint32_t hexA, uint32_t hexB)
{
  const int dr = (int)(hexA >> 16 & 0xFF) - (int)(hexB >> 16 & 0xFF);
  const int dg = (int)(hexA >> 8 & 0xFF) - (int)(hexB >> 8 & 0xFF);
  const int db = (int)(hexA & 0xFF) - (int)(hexB & 0xFF);
  return dr * dr + dg * dg + db * db < 4800;
}

// manufacturer / Custom themes: the whole palette from the dial and needle colours (docs/Manufacturer themes.pdf).
// Black background; scale, numerals, arcs and labels in the dial colour, secondary text at 55 % of it, tracks at 16 %;
// needle and redline in the needle colour. Where the needle matches the dial, the second accent falls back to the
// warning amber and the redline to red, then white, so neither disappears into the arc.
static Palette paletteFrom(uint32_t dial, uint32_t needle)
{
  Palette result;
  result.accent = dial;
  result.text = dial;
  result.muted = scaleHex(dial, 55);
  result.panel = scaleHex(dial, 16);
  result.bg = 0x000000;
  result.needle = needle;
  if (!coloursSimilar(needle, dial))
  {
    result.accent2 = needle;
  }
  else if (!coloursSimilar(0xFFB000, dial))
  {
    result.accent2 = 0xFFB000;
  }
  else
  {
    result.accent2 = 0xFFFFFF;
  }
  if (!coloursSimilar(needle, dial))
  {
    result.redline = needle;
  }
  else if (!coloursSimilar(0xFF3B3B, dial))
  {
    result.redline = 0xFF3B3B;
  }
  else
  {
    result.redline = 0xFFFFFF;
  }
  result.danger = 0xFF4D5E; // alert banner / reboot button, white text on it
  result.ok = 0x3FD07F;
  return result;
}

// ---------------------------------------------------------------------------
// page objects
// ---------------------------------------------------------------------------
// the DSG view (menu > DSG and STYLE_GEAR pages)
#define GEAR_STRIP 6
struct GearWidgets
{
  lv_obj_t *big;               // "D4" / "M2" / "P"
  lv_obj_t *strip[GEAR_STRIP]; // P R N D S M, the active one lit
  lv_obj_t *speed;
  lv_obj_t *speedUnit;         // "km/h  gearbox"
  lv_obj_t *sub;
  char lastBig[8];
  int8_t lastStrip;
};

// a needle / arc / bar moves like an analog pointer: a damped spring toward the signal (it has mass - it accelerates
// and settles, never steps, damping 0.85: just a hint of follow-through). The response time follows the signal's own
// update interval (CAN at 50 Hz: ~0.12 s, K-line blocks a few times a second: ~0.5 s), so it is quick where the data
// is quick and sweeps where the data is slow.
struct Smooth
{
  float value, velocity, raw; // shown, its speed (units / s), last sample
  uint32_t tickMs, rawMs;
  uint16_t period;            // ms between samples, averaged
  bool init;
};

static float smoothTo(Smooth &smooth, float target, bool valid)
{
  const uint32_t now = lv_tick_get();
  // Gauge feel "digital" (web UI > Display): no spring, every reading shows the moment it arrives. The state is kept
  // up to date, so switching back to analog carries on from where the needle is.
  const bool digital = settings.gaugeFeel == 1;
  if (!valid || !smooth.init || isnan(smooth.value) || digital)
  {
    smooth.raw = target;
    smooth.value = target;
    smooth.velocity = 0.0f;
    smooth.rawMs = now;
    smooth.tickMs = now;
    smooth.period = 100;
    smooth.init = valid;
    return target;
  }
  if (target != smooth.raw) // a new sample: how often they come sets the response time
  {
    const uint32_t gap = now - smooth.rawMs;
    smooth.period = (uint16_t)((smooth.period * 3u + (gap > 1000 ? 1000 : gap)) / 4u);
    smooth.raw = target;
    smooth.rawMs = now;
  }

  float responseMs = smooth.period * 1.6f; // ms to (nearly) arrive
  if (responseMs < 120.0f)
  {
    responseMs = 120.0f;
  }
  else if (responseMs > 600.0f)
  {
    responseMs = 600.0f;
  }
  const float omega = 4000.0f / responseMs; // natural frequency (rad / s)
  const float damping = 0.85f;              // damping
  float dt = (now - smooth.tickMs) / 1000.0f;
  smooth.tickMs = now;
  if (dt > 0.1f)
  {
    dt = 0.1f;
  }
  const int steps = dt > 0.0f ? (int)ceilf(dt / 0.004f) : 0; // small steps keep the spring stable at any frame rate
  const float stepS = steps ? dt / steps : 0.0f;

  for (int i = 0; i < steps; i++)
  {
    const float accel = omega * omega * (target - smooth.value) - 2.0f * damping * omega * smooth.velocity;
    smooth.velocity += accel * stepS;
    smooth.value += smooth.velocity * stepS;
  }
  return smooth.value;
}

#define MAX_TICK_LABELS 12
struct PageWidgets
{
  lv_obj_t *scr;
  lv_obj_t *arc;      // STYLE_ARC / HALDEX (actual)
  lv_obj_t *arc2;     // HALDEX target
  lv_obj_t *scale;    // STYLE_NEEDLE
  lv_obj_t *needle;   // line inside scale
  lv_obj_t *bar;      // STYLE_BAR / DUAL row 1
  lv_obj_t *bar2;     // DUAL row 2
  lv_obj_t *value;    // main value label
  lv_obj_t *value2;   // second value (DUAL / HALDEX mode text)
  lv_obj_t *unit;
  lv_obj_t *name;
  lv_obj_t *sub;      // secondary text (stale / no link / mode)
  lv_obj_t *quad[4];  // QUAD values; CUSTOM: the centre-stack values (0..2)
  lv_obj_t *quadName[4]; // QUAD names; CUSTOM: the row icons (image, or a name label when the signal has no icon)
  lv_obj_t *quadUnit[4];
  lv_obj_t *spinner;  // STYLE_OIL: shown while the source is connecting / waiting
  lv_obj_t *sourceSpinner; // 0.15.0: pages with a K-line / Bluetooth OBD / VAG diag value: spins beside the note while it connects
  lv_obj_t *chart;                // 0.31.0: STYLE_GRAPH: the 30 s line graph
  lv_obj_t *valueIcon[4];         // 0.31.0: Single / Dual / Triple / Quad: an icon per value (NULL = none on this style)
  lv_chart_series_t *graphLine;   // its one line (the page signal)
  uint32_t graphSampleMs;         // when the graph last took a reading
  GearWidgets gear;   // STYLE_GEAR
  uint8_t pageCfgIndex;   // index into settings.pages
  uint8_t builtStyle;     // 0.30.19: the style this page was built as (settings can change before the rebuild)
  bool builtSprite;       // 0.30.19: built as its sprite (the page's spriteOn) rather than its style
  float drawnMin;         // 0.36.4: the range the page is drawn with: its Min / Max, or for an adc2can sensor the
  float drawnMax;         //         adc2can's dial range (pageDrawnRange)
  int32_t scaleMul;   // the scale runs in value x scaleMul (applyTicks)
  bool scaleKilo;     // scale labels in thousands: "x1000" goes in front of the unit
  char tickText[MAX_TICK_LABELS][8];
  const char *tickPtr[MAX_TICK_LABELS + 1];
  float lastValue;
  bool lastValid;
  Smooth smooth[2];                // main / second value as the needle, arcs and bars show them
  lv_point_precise_t needlePts[2]; // STYLE_NEEDLE: the pointer, placed with float angles (setNeedle)
  Sprite *sprite;                  // the page's sprite (the page's spriteOn), GaugeSprite; NULL = its normal style
};

static PageWidgets *pages = (PageWidgets *)psramCalloc(MAX_PAGES, sizeof(PageWidgets)); // in PSRAM (3 KB off internal RAM)
static uint8_t pageCount = 0;
static uint8_t activePage = 0;
static bool menuOpen = false;
static lv_obj_t *menuScr = NULL;
static lv_obj_t *infoScr = NULL;
static lv_obj_t *infoLabel = NULL;
static uiInfoPage_t infoPage = UI_INFO_HALDEX;
static lv_obj_t *settingsScr = NULL;
static lv_obj_t *dsgScr = NULL;
static GearWidgets dsgWidgets;
static lv_obj_t *dragScr = NULL; // 0.38.0: menu > Drag Timer (DragTimer.cpp)
static lv_obj_t *alertBox = NULL;
static lv_obj_t *alertLabel = NULL;
static lv_obj_t *progressBox = NULL;
static lv_obj_t *progressArc = NULL;
static lv_obj_t *progressTitle = NULL;
static lv_obj_t *progressPct = NULL;
static lv_obj_t *progressDetail = NULL;
static lv_timer_t *progressTimer = NULL;
static bool alertLatched = false;
static lv_obj_t *bootScr = NULL;
static lv_timer_t *bootTimer = NULL;
static uint32_t bootStartTick = 0; // 0.30.11: lv_tick_get() when the boot logo went up (moved back when it jumps ahead)
static uint32_t bootLogoMs = 0;    // 0.30.11: how long the logo stays up before the fade to the first page
static lv_obj_t *bootRingArc = NULL; // 0.30.11: the ring round the edge (NULL = this logo has none)
static lv_obj_t *bootDialArc = NULL; // the can2gauge logo's dial sweep (0.30.11: moved up here, bootDone clears it)
static lv_timer_t *infoTimer = NULL;
static uiInfoProvider_t infoProviders[UI_INFO_COUNT] = {};
static UiCallbacks callbacks;
static uint8_t *logoBuf = NULL;
static lv_image_dsc_t logoDsc;
static uint8_t *dialBuf = NULL; // 0.25.2: the can2gauge logo's sweep colours (PSRAM, freed with logoBuf in bootDone)
static lv_image_dsc_t dialDsc;  // dialBuf described for LVGL

static const char *styleNames[gaugeStyle_t_MAX] = {"Arc", "Needle", "Bar", "Single", "Dual", "Haldex", "Quad", "Gear", "Oil (PQ)", "Custom", "Graph", "Triple"};
// 0.31.0: Numeric is shown as "Single" and Oil (OEM) as "Oil (PQ)". The numbers saved in the settings stay as they were;
// the web UI lists them in its own order (STYLE_ORDER in data/app.js).
const char *uiStyleName(uint8_t style)
{
  return style < gaugeStyle_t_MAX ? styleNames[style] : "?";
}

static const char *haldexModeName(int mode)
{
  static const char *names[] = {"STOCK", "FWD", "50:50", "60:40", "75:25", "EXPERT"};
  return (mode >= 0 && mode < 6) ? names[mode] : "--";
}

// ---------------------------------------------------------------------------
// gestures: swipe = page change, long press = menu, tap = acknowledge alert / tickle the sprite
// ---------------------------------------------------------------------------
static void pageEventCb(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_GESTURE)
  {
    lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    if (direction == LV_DIR_LEFT)
    {
      uiNextPage();
    }
    else if (direction == LV_DIR_RIGHT)
    {
      uiPrevPage();
    }
    else if (direction == LV_DIR_TOP || direction == LV_DIR_BOTTOM)
    {
      uiOpenMenu(); // swipe up/down also opens the menu (handy with gloves)
    }
    lv_indev_wait_release(lv_indev_active()); // LVGL would also send a click when the finger lifts: not a tap
  }
  else if (code == LV_EVENT_LONG_PRESSED)
  {
    uiOpenMenu();
    lv_indev_wait_release(lv_indev_active()); // the release must not also count as a tap (or acknowledge an alert)
  }
  else if (code == LV_EVENT_CLICKED)
  {
    if (alertLatched)
    {
      runtime.alertAcks++; // 0.32.0: the driver acknowledged it (the triggers hear this)
      uiClearAlert();
    }
  }
  else if (code == LV_EVENT_SHORT_CLICKED)
  {
    // LVGL sends a short click after a swipe too: only a press that didn't become a gesture is a tap
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_NONE && pageCount &&
        lv_screen_active() == pages[activePage].scr)
    {
      spriteTap(pages[activePage].sprite); // the pack's tap clip (a page without a sprite ignores it)
    }
  }
}

static lv_obj_t *newScreen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, colour(palette.bg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  return scr;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, lv_align_t align, int x, int y, const char *text)
{
  lv_obj_t *labelObj = lv_label_create(parent);
  lv_obj_set_style_text_font(labelObj, font, 0);
  lv_obj_set_style_text_color(labelObj, colour(color), 0);
  lv_label_set_text(labelObj, text);
  lv_obj_align(labelObj, align, x, y);
  return labelObj;
}

// 0.30.2: keeping text inside the round glass. A line of text that changes (a state, a name, a message) could run past
// the round edge ("Bluetooth OBD: not connected, retry 30s" on the menu). These cut it to fit, ending in "...".
#define GLASS_MARGIN 12 // space kept clear at each end of a line, inside the round edge

// how wide the glass is (less GLASS_MARGIN each side) for a line of text whose middle is centreY px from the glass
// centre (minus = above) and lineHeight tall: the line's edge nearer the rim sets it
static int32_t glassWidthAt(int32_t centreY, int32_t lineHeight)
{
  int32_t edge = centreY;
  if (edge < 0)
  {
    edge = -edge; // above or below the centre: the same width
  }
  edge = edge + lineHeight / 2;
  if (edge >= 230)
  {
    return 60; // right at the rim: just a little room
  }
  const int32_t halfWidth = (int32_t)sqrtf((float)(240 * 240 - edge * edge)); // Pythagoras on the 240 px radius
  return 2 * halfWidth - 2 * GLASS_MARGIN;
}

// keep a label to one line no wider than maxWidth: longer text ends in "..."
static void fitOneLine(lv_obj_t *labelObj, int32_t maxWidth)
{
  lv_obj_set_style_max_width(labelObj, maxWidth, 0);
  lv_label_set_long_mode(labelObj, LV_LABEL_LONG_DOT);
  lv_label_set_max_lines(labelObj, 1);
}

// label() for text that changes, centred across the glass (x is always 0 here) on a parent the size of the glass
// (a screen or a 480 x 480 box): one line, as wide as the round glass allows at that height
static lv_obj_t *glassLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color, lv_align_t align, int x, int y,
                            const char *text)
{
  lv_obj_t *labelObj = label(parent, font, color, align, x, y, text);
  const int32_t lineHeight = lv_font_get_line_height(font);
  int32_t centreY = y; // LV_ALIGN_CENTER: y is already from the glass centre
  if (align == LV_ALIGN_TOP_MID)
  {
    centreY = y + lineHeight / 2 - 240; // y is from the top of the glass
  }
  else if (align == LV_ALIGN_BOTTOM_MID)
  {
    centreY = 240 + y - lineHeight / 2; // y is from the bottom of the glass (minus = up)
  }
  fitOneLine(labelObj, glassWidthAt(centreY, lineHeight));
  return labelObj;
}

// ---------------------------------------------------------------------------
// scale ticks (docs/ui-concept: "tick spacing from min / max - 1-2-2.5-5 steps, about six labels, x1000 labels for
// rpm"). A round step that divides the range evenly into 4-10 labels (as near 7 as possible), so 0-8000 rpm reads
// 0 1 2 ... 8 (x1000), boost -1 ... 2 reads -1 -0.5 0 ... 2 and 0-140 C reads 0 20 40 ... 140. lv_scale is integer, so
// the scale runs in value x mul (mul 10 / 100 for 0.5 / 0.25 steps) and gets its own label strings.
// ---------------------------------------------------------------------------
struct TickPlan
{
  float step;     // between labels
  uint8_t majors; // label intervals
  uint8_t sub;    // minor intervals per label
  int32_t mul;    // scale units per value unit
  bool kilo;      // labels in thousands
  uint8_t dp;     // label decimals
};

static TickPlan tickPlan(float lo, float hi)
{
  TickPlan plan = {1, 1, 5, 1, false, 0};
  const float range = hi - lo;
  if (!(range > 0))
  {
    return plan;
  }
  static const float mantissas[] = {1, 2, 2.5f, 5};
  int best = -1;

  for (int exponent = -3; exponent <= 5; exponent++)
  {
    for (float mantissa : mantissas)
    {
      const float step = mantissa * powf(10, exponent);
      const float labelCount = range / step;
      const long labelCountRounded = lroundf(labelCount);
      if (labelCountRounded < 4 || labelCountRounded > 10 || fabsf(labelCount - labelCountRounded) > 1e-3f * labelCount)
      {
        continue;
      }
      const float loSteps = lo / step; // labels must land on round values too
      if (fabsf(loSteps - lroundf(loSteps)) > 1e-3f)
      {
        continue;
      }
      const int score = abs((int)labelCountRounded - 7);
      if (best < 0 || score < best)
      {
        best = score;
        plan.step = step;
        plan.majors = (uint8_t)labelCountRounded;
      }
    }
  }

  if (best < 0) // nothing round fits this range: ten equal steps
  {
    plan.step = range / 10;
    plan.majors = 10;
  }
  const float mantissa = plan.step / powf(10, floorf(log10f(plan.step) + 1e-6f)); // 1 .. 9.99
  plan.sub = fabsf(mantissa - 2) < 0.01f ? 4 : 5;
  plan.kilo = plan.step >= 1000 && fmodf(fabsf(lo), 1000) < 0.5f;
  const float labelStep = plan.kilo ? plan.step / 1000 : plan.step;
  if (fabsf(labelStep - lroundf(labelStep)) < 1e-3f)
  {
    plan.dp = 0;
  }
  else if (fabsf(labelStep * 10 - lroundf(labelStep * 10)) < 1e-3f)
  {
    plan.dp = 1;
  }
  else
  {
    plan.dp = 2;
  }
  const float minor = plan.step / plan.sub;
  while (plan.mul < 1000 &&
         (fabsf(minor * plan.mul - lroundf(minor * plan.mul)) > 1e-3f || fabsf(lo * plan.mul - lroundf(lo * plan.mul)) > 1e-3f))
  {
    plan.mul *= 10;
  }
  return plan;
}

static void applyTicks(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  const TickPlan plan = tickPlan(widgets.drawnMin, widgets.drawnMax);
  widgets.scaleMul = plan.mul;
  widgets.scaleKilo = plan.kilo;
  lv_scale_set_range(widgets.scale, lroundf(widgets.drawnMin * plan.mul), lroundf(widgets.drawnMax * plan.mul));
  lv_scale_set_total_tick_count(widgets.scale, plan.majors * plan.sub + 1);
  lv_scale_set_major_tick_every(widgets.scale, plan.sub);
  const uint8_t labelCount = plan.majors + 1 < MAX_TICK_LABELS ? plan.majors + 1 : MAX_TICK_LABELS;

  for (uint8_t i = 0; i < labelCount; i++)
  {
    float value = widgets.drawnMin + plan.step * i;
    if (plan.kilo)
    {
      value /= 1000;
    }
    if (fabsf(value) < 1e-6f)
    {
      value = 0; // no "-0"
    }
    snprintf(widgets.tickText[i], sizeof(widgets.tickText[i]), "%.*f", plan.dp, value);
    widgets.tickPtr[i] = widgets.tickText[i];
  }
  widgets.tickPtr[labelCount] = NULL;
  lv_scale_set_text_src(widgets.scale, widgets.tickPtr);
}

// ---------------------------------------------------------------------------
// page builders
// ---------------------------------------------------------------------------
static void buildArc(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  widgets.arc = lv_arc_create(widgets.scr);
  lv_obj_set_size(widgets.arc, 440, 440);
  lv_obj_center(widgets.arc);
  lv_arc_set_rotation(widgets.arc, 135);
  lv_arc_set_bg_angles(widgets.arc, 0, 270);
  lv_arc_set_range(widgets.arc, 0, 1000);
  lv_arc_set_value(widgets.arc, 0);
  lv_obj_remove_style(widgets.arc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(widgets.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(widgets.arc, 26, LV_PART_MAIN);
  lv_obj_set_style_arc_width(widgets.arc, 26, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(widgets.arc, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_arc_color(widgets.arc, accentColor(), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(widgets.arc, true, LV_PART_INDICATOR);
  // tick scale just inside the arc
  widgets.scale = lv_scale_create(widgets.scr);
  lv_obj_set_size(widgets.scale, 380, 380);
  lv_obj_center(widgets.scale);
  lv_scale_set_mode(widgets.scale, LV_SCALE_MODE_ROUND_INNER);
  lv_scale_set_rotation(widgets.scale, 135);
  lv_scale_set_angle_range(widgets.scale, 270);
  applyTicks(widgets, pageCfg);
  lv_scale_set_label_show(widgets.scale, true);
  lv_obj_set_style_length(widgets.scale, 8, LV_PART_ITEMS);
  lv_obj_set_style_length(widgets.scale, 16, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(widgets.scale, colour(palette.muted), LV_PART_ITEMS);
  lv_obj_set_style_line_color(widgets.scale, colour(palette.text), LV_PART_INDICATOR);
  lv_obj_set_style_text_color(widgets.scale, colour(palette.muted), LV_PART_INDICATOR);
  lv_obj_set_style_text_font(widgets.scale, &lv_font_montserrat_14, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(widgets.scale, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_remove_flag(widgets.scale, LV_OBJ_FLAG_CLICKABLE);
  widgets.value = label(widgets.scr, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, -10, "--");
  widgets.unit = glassLabel(widgets.scr, &lv_font_montserrat_20, palette.muted, LV_ALIGN_CENTER, 0, 40, "");
  widgets.name = glassLabel(widgets.scr, &lv_font_montserrat_24, palette.accent, LV_ALIGN_BOTTOM_MID, 0, -34, pageCfg.label);
  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, 0, 70, "");
}

// the needle: a bar from a short counterweight tail through the centre to the tip, rotated about the centre. LVGL draws
// a transformed object anti-aliased at 0.1 degree steps, so the tip glides between pixels (a line's ends snap to them,
// and lv_scale_set_line_needle_value works in whole degrees - 3 px at the tip)
#define NEEDLE_LEN 170
#define NEEDLE_TAIL 26
#define NEEDLE_W 7
static void setNeedle(PageWidgets &widgets, const GaugePageCfg &pageCfg, float value)
{
  float fraction = widgets.drawnMax > widgets.drawnMin ? (value - widgets.drawnMin) / (widgets.drawnMax - widgets.drawnMin) : 0.0f;
  if (fraction < -0.01f) // the pointer may just touch past the stops, like a real one
  {
    fraction = -0.01f;
  }
  else if (fraction > 1.01f)
  {
    fraction = 1.01f;
  }
  const int32_t rotation = (int32_t)lroundf((135.0f + 270.0f * fraction) * 10.0f); // the scale: rotation 135, 270 degrees
  if (rotation != lv_obj_get_style_transform_rotation(widgets.needle, LV_PART_MAIN))
  {
    lv_obj_set_style_transform_rotation(widgets.needle, rotation, 0);
  }
}

static void buildNeedle(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  widgets.scale = lv_scale_create(widgets.scr);
  lv_obj_set_size(widgets.scale, 430, 430);
  lv_obj_center(widgets.scale);
  lv_scale_set_mode(widgets.scale, LV_SCALE_MODE_ROUND_INNER);
  lv_scale_set_rotation(widgets.scale, 135);
  lv_scale_set_angle_range(widgets.scale, 270);
  applyTicks(widgets, pageCfg);
  lv_scale_set_label_show(widgets.scale, true);
  lv_obj_set_style_length(widgets.scale, 10, LV_PART_ITEMS);
  lv_obj_set_style_length(widgets.scale, 22, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(widgets.scale, 2, LV_PART_ITEMS);
  lv_obj_set_style_line_width(widgets.scale, 4, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(widgets.scale, colour(palette.muted), LV_PART_ITEMS);
  lv_obj_set_style_line_color(widgets.scale, colour(palette.text), LV_PART_INDICATOR);
  lv_obj_set_style_text_color(widgets.scale, colour(palette.text), LV_PART_INDICATOR);
  lv_obj_set_style_text_font(widgets.scale, &lv_font_montserrat_18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(widgets.scale, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(widgets.scale, colour(palette.panel), LV_PART_MAIN);
  lv_obj_remove_flag(widgets.scale, LV_OBJ_FLAG_CLICKABLE);
  // 0.25.3: the name, number and unit first: the needle and its hub are drawn on top of them, like a real dial's
  // printed legend (LVGL draws in the order things are made, so the newest is on top)
  widgets.value = label(widgets.scr, &lv_font_montserrat_40, palette.text, LV_ALIGN_CENTER, 0, 90, "--");
  widgets.unit = glassLabel(widgets.scr, &lv_font_montserrat_18, palette.muted, LV_ALIGN_CENTER, 0, 125, "");
  widgets.name = glassLabel(widgets.scr, &lv_font_montserrat_24, palette.accent, LV_ALIGN_CENTER, 0, -80, pageCfg.label);
  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_BOTTOM_MID, 0, -24, "");
  widgets.needle = lv_obj_create(widgets.scr); // above the scale and the labels, below the hub
  lv_obj_remove_style_all(widgets.needle);
  lv_obj_remove_flag(widgets.needle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(widgets.needle, NEEDLE_LEN + NEEDLE_TAIL, NEEDLE_W);
  lv_obj_set_pos(widgets.needle, 240 - NEEDLE_TAIL, 240 - NEEDLE_W / 2); // the scale is centred on the 480 x 480 glass
  lv_obj_set_style_radius(widgets.needle, NEEDLE_W / 2, 0);
  lv_obj_set_style_bg_color(widgets.needle, needleColor(), 0);
  lv_obj_set_style_bg_opa(widgets.needle, LV_OPA_COVER, 0);
  lv_obj_set_style_transform_pivot_x(widgets.needle, NEEDLE_TAIL, 0);
  lv_obj_set_style_transform_pivot_y(widgets.needle, NEEDLE_W / 2, 0);
  setNeedle(widgets, pageCfg, widgets.drawnMin);
  // hub cap: dark centre ringed in the needle colour, like an OEM pointer cap
  lv_obj_t *hub = lv_obj_create(widgets.scr);
  lv_obj_set_size(hub, 28, 28);
  lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(hub, colour(palette.panel), 0);
  lv_obj_set_style_border_width(hub, 3, 0);
  lv_obj_set_style_border_color(hub, needleColor(), 0);
  lv_obj_center(hub);
  lv_obj_remove_flag(hub, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *newBar(lv_obj_t *parent, int y, int w, int h)
{
  lv_obj_t *bar = lv_bar_create(parent);
  lv_obj_set_size(bar, w, h);
  lv_obj_align(bar, LV_ALIGN_CENTER, 0, y);
  lv_bar_set_range(bar, 0, 1000);
  lv_obj_set_style_bg_color(bar, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, accentColor(), LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  return bar;
}

static uint8_t signalIcon(signalId_t id); // further down: which icon a value gets
static void setHidden(lv_obj_t *obj, bool hidden);   // further down

// 0.31.0: an icon for one value on Single / Dual / Triple / Quad, centred on x, y. It starts hidden; showValueIcon puts
// the right one in (the theme's accent colour, like the Custom page's row icons).
static lv_obj_t *makeValueIcon(lv_obj_t *parent, int x, int y)
{
  lv_obj_t *image = lv_image_create(parent);
  lv_obj_set_style_image_recolor(image, accentColor(), 0);
  lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
  lv_obj_align(image, LV_ALIGN_CENTER, x, y);
  lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE); // taps and swipes go to the page
  lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
  return image;
}

// 0.31.0: the icon for a value, checked on every refresh: a page source or an ADC sensor gets its name a few seconds
// after start-up, and its icon follows. Only touched when it changes. No icon fits = the space stays empty.
static void showValueIcon(lv_obj_t *image, signalId_t id)
{
  if (image == NULL)
  {
    return; // this style has no icon here
  }
  const lv_image_dsc_t *picture = gaugeIcon(signalIcon(id)); // NULL for ICON_NONE
  if (picture == NULL)
  {
    setHidden(image, true);
    return;
  }
  if (lv_image_get_src(image) != picture)
  {
    lv_image_set_src(image, picture);
  }
  setHidden(image, false);
}

static void buildBar(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  widgets.name = glassLabel(widgets.scr, &lv_font_montserrat_28, palette.accent, LV_ALIGN_CENTER, 0, -120, pageCfg.label);
  widgets.value = label(widgets.scr, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, -40, "--");
  widgets.unit = glassLabel(widgets.scr, &lv_font_montserrat_20, palette.muted, LV_ALIGN_CENTER, 0, 10, "");
  widgets.bar = newBar(widgets.scr, 70, 340, 34);
  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, 0, 120, "");
}

static void buildNumeric(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  widgets.valueIcon[0] = makeValueIcon(widgets.scr, 0, -158); // 0.31.0: above the name
  widgets.name = glassLabel(widgets.scr, &lv_font_montserrat_28, palette.accent, LV_ALIGN_CENTER, 0, -110, pageCfg.label);
  widgets.value = label(widgets.scr, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, 0, "--");
  // 0.24.27: scaled from its middle - LVGL scales from the top-left corner unless told, so the doubled value sat below
  // and right of centre (the battery page's "--", 2026-10-27)
  lv_obj_set_style_transform_pivot_x(widgets.value, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(widgets.value, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(widgets.value, 512, 0); // 2x -> ~96 px digits
  widgets.unit = glassLabel(widgets.scr, &lv_font_montserrat_24, palette.muted, LV_ALIGN_CENTER, 0, 100, "");
  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, 0, 150, "");
}

// 0.31.0: Graph page - the last 30 s of the page signal as a rolling line, the value and unit above it. The graph
// runs from the page's Min (bottom) to Max (top), with the Redline as a thin red line across it.
#define GRAPH_POINTS 150     // readings across the graph
#define GRAPH_SAMPLE_MS 200  // one reading every 200 ms: 150 x 200 ms = the last 30 s
#define GRAPH_WIDTH 340      // px
#define GRAPH_HEIGHT 180     // px
#define GRAPH_Y 40           // its middle, px below the glass centre
#define GRAPH_LABEL_GAP 4    // 0.36.4: px between the graph's left edge and its Max / Min (which sit outside it)

static int32_t permille(float value, float lo, float hi);                  // further down
static void formatValue(char *buf, size_t len, float value, uint8_t decimals); // further down
static uint8_t pageDecimals(const GaugePageCfg &pageCfg, signalId_t id);   // further down

static void buildGraph(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  widgets.name = glassLabel(widgets.scr, &lv_font_montserrat_20, palette.accent, LV_ALIGN_CENTER, 0, -175, pageCfg.label);
  widgets.value = label(widgets.scr, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, -118, "--");
  widgets.unit = glassLabel(widgets.scr, &lv_font_montserrat_18, palette.muted, LV_ALIGN_CENTER, 0, -76, "");

  // the graph itself: a dark panel with faint grid lines, values 0-1000 = the page's Min to Max
  widgets.chart = lv_chart_create(widgets.scr);
  lv_obj_set_size(widgets.chart, GRAPH_WIDTH, GRAPH_HEIGHT);
  lv_obj_align(widgets.chart, LV_ALIGN_CENTER, 0, GRAPH_Y);
  lv_chart_set_type(widgets.chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(widgets.chart, GRAPH_POINTS);
  lv_chart_set_update_mode(widgets.chart, LV_CHART_UPDATE_MODE_SHIFT); // a new reading goes on the right, the rest move left
  lv_chart_set_axis_range(widgets.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
  lv_chart_set_div_line_count(widgets.chart, 5, 7); // 5 lines across (quarters), 7 down (every 5 s)
  lv_obj_set_style_bg_color(widgets.chart, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(widgets.chart, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(widgets.chart, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(widgets.chart, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_all(widgets.chart, 0, LV_PART_MAIN); // the line uses the whole panel, so the redline sits true
  lv_obj_set_style_line_color(widgets.chart, lv_color_lighten(colour(palette.panel), 25), LV_PART_MAIN); // grid
  lv_obj_set_style_line_width(widgets.chart, 1, LV_PART_MAIN);
  lv_obj_set_style_line_width(widgets.chart, 3, LV_PART_ITEMS); // the reading's line
  lv_obj_set_style_width(widgets.chart, 0, LV_PART_INDICATOR);  // no dot on each reading
  lv_obj_set_style_height(widgets.chart, 0, LV_PART_INDICATOR);
  lv_obj_remove_flag(widgets.chart, LV_OBJ_FLAG_CLICKABLE);     // swipes and taps go to the page, as on the others
  lv_obj_remove_flag(widgets.chart, LV_OBJ_FLAG_SCROLLABLE);
  widgets.graphLine = lv_chart_add_series(widgets.chart, accentColor(), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_values(widgets.chart, widgets.graphLine, LV_CHART_POINT_NONE); // empty until readings come in
  widgets.graphSampleMs = lv_tick_get();

  // the redline, a thin line across the graph at its height (only when the page has one)
  if (pageCfg.redline > widgets.drawnMin && pageCfg.redline < widgets.drawnMax)
  {
    const int32_t redHeight = permille(pageCfg.redline, widgets.drawnMin, widgets.drawnMax); // 0-1000 up the graph
    const int32_t redFromTop = (GRAPH_HEIGHT - 1) - (redHeight * (GRAPH_HEIGHT - 1)) / 1000;
    lv_obj_t *redLine = plainObject(widgets.scr);
    lv_obj_set_size(redLine, GRAPH_WIDTH, 2);
    lv_obj_set_style_bg_color(redLine, colour(palette.redline), 0);
    lv_obj_set_style_bg_opa(redLine, LV_OPA_70, 0);
    lv_obj_align_to(redLine, widgets.chart, LV_ALIGN_TOP_MID, 0, redFromTop - 1);
  }

  // Max / Min just outside the graph, against its left edge (towards the screen's edge), the time along the bottom.
  // 0.36.4: they sat inside the graph's top and bottom left (on a solid box from 0.33.1, as the line scrolled under
  // them); outside it the line never reaches them, so no box. OUT_LEFT_TOP / OUT_LEFT_BOTTOM = to the left of the
  // graph, level with its top / bottom, the label's right-hand end GRAPH_LABEL_GAP px from the graph.
  char buf[24];
  formatValue(buf, sizeof(buf), widgets.drawnMax, pageDecimals(pageCfg, (signalId_t)pageCfg.signal));
  lv_obj_t *maxLabel = label(widgets.scr, &lv_font_montserrat_12, palette.muted, LV_ALIGN_CENTER, 0, 0, buf);
  lv_obj_align_to(maxLabel, widgets.chart, LV_ALIGN_OUT_LEFT_TOP, -GRAPH_LABEL_GAP, 0);
  formatValue(buf, sizeof(buf), widgets.drawnMin, pageDecimals(pageCfg, (signalId_t)pageCfg.signal));
  lv_obj_t *minLabel = label(widgets.scr, &lv_font_montserrat_12, palette.muted, LV_ALIGN_CENTER, 0, 0, buf);
  lv_obj_align_to(minLabel, widgets.chart, LV_ALIGN_OUT_LEFT_BOTTOM, -GRAPH_LABEL_GAP, 0);
  lv_obj_t *startLabel = label(widgets.scr, &lv_font_montserrat_12, palette.muted, LV_ALIGN_CENTER, 0, 0, "-30 s");
  lv_obj_align_to(startLabel, widgets.chart, LV_ALIGN_OUT_BOTTOM_LEFT, 4, 4);
  lv_obj_t *nowLabel = label(widgets.scr, &lv_font_montserrat_12, palette.muted, LV_ALIGN_CENTER, 0, 0, "now");
  lv_obj_align_to(nowLabel, widgets.chart, LV_ALIGN_OUT_BOTTOM_RIGHT, -4, 4);

  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, 0, 176, "");
}

// 0.31.0: every Graph page takes a reading every GRAPH_SAMPLE_MS, shown or not, so its last 30 s are there the moment
// you swipe to it. No reading (stale / no data) leaves a gap in the line. Caller holds the LVGL lock.
static void sampleGraphs()
{
  const uint32_t now = lv_tick_get();
  for (uint8_t pageIndex = 0; pageIndex < pageCount; pageIndex++)
  {
    PageWidgets &widgets = pages[pageIndex];
    if (widgets.chart == NULL || widgets.builtStyle != STYLE_GRAPH || widgets.builtSprite)
    {
      continue; // not a Graph page
    }
    if (now - widgets.graphSampleMs < GRAPH_SAMPLE_MS)
    {
      continue; // not time for its next reading yet
    }
    widgets.graphSampleMs = now;
    const GaugePageCfg &pageCfg = settings.pages[widgets.pageCfgIndex];
    const signalId_t id = (signalId_t)pageCfg.signal;
    int32_t point = LV_CHART_POINT_NONE; // a gap in the line
    if (signalValid(id))
    {
      const float shown = signalDisplayValue(id, signalGet(id), NULL); // in the shown unit, like the page's Min / Max
      point = permille(shown, widgets.drawnMin, widgets.drawnMax);       // off the top / bottom = at the top / bottom
    }
    lv_chart_set_next_value(widgets.chart, widgets.graphLine, point);
  }
}

static void buildDual(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  widgets.name = glassLabel(widgets.scr, &lv_font_montserrat_20, palette.accent, LV_ALIGN_CENTER, 0, -150, pageCfg.label);
  widgets.value = label(widgets.scr, &lv_font_montserrat_40, palette.text, LV_ALIGN_CENTER, 0, -95, "--");
  widgets.bar = newBar(widgets.scr, -50, 300, 22);
  widgets.valueIcon[0] = makeValueIcon(widgets.scr, -174, -50); // 0.31.0: left of each bar
  widgets.unit = glassLabel(widgets.scr, &lv_font_montserrat_20, palette.accent2, LV_ALIGN_CENTER, 0, 20,
                       signalName((signalId_t)pageCfg.signal2));
  widgets.value2 = label(widgets.scr, &lv_font_montserrat_40, palette.text, LV_ALIGN_CENTER, 0, 70, "--");
  widgets.bar2 = newBar(widgets.scr, 115, 300, 22);
  widgets.valueIcon[1] = makeValueIcon(widgets.scr, -174, 115);
  lv_obj_set_style_bg_color(widgets.bar2, colour(palette.accent2), LV_PART_INDICATOR);
  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, 0, 160, "");
}

static void buildHaldex(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  // outer thin arc = lock target, inner thick arc = actual engagement
  widgets.arc2 = lv_arc_create(widgets.scr);
  lv_obj_set_size(widgets.arc2, 460, 460);
  lv_obj_center(widgets.arc2);
  lv_arc_set_rotation(widgets.arc2, 135);
  lv_arc_set_bg_angles(widgets.arc2, 0, 270);
  lv_arc_set_range(widgets.arc2, 0, 1000);
  lv_obj_remove_style(widgets.arc2, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(widgets.arc2, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(widgets.arc2, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_width(widgets.arc2, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(widgets.arc2, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_arc_color(widgets.arc2, colour(palette.accent2), LV_PART_INDICATOR);
  widgets.arc = lv_arc_create(widgets.scr);
  lv_obj_set_size(widgets.arc, 420, 420);
  lv_obj_center(widgets.arc);
  lv_arc_set_rotation(widgets.arc, 135);
  lv_arc_set_bg_angles(widgets.arc, 0, 270);
  lv_arc_set_range(widgets.arc, 0, 1000);
  lv_obj_remove_style(widgets.arc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(widgets.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(widgets.arc, 30, LV_PART_MAIN);
  lv_obj_set_style_arc_width(widgets.arc, 30, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(widgets.arc, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_arc_color(widgets.arc, accentColor(), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(widgets.arc, true, LV_PART_INDICATOR);
  widgets.name = label(widgets.scr, &lv_font_montserrat_20, palette.muted, LV_ALIGN_CENTER, 0, -95, "HALDEX LOCK");
  widgets.value = label(widgets.scr, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, -30, "--");
  // scale around the label's own centre: the default pivot is its top-left corner, which pushed the number right
  // and down off the dial's centre line (more so the wider the number)
  lv_obj_set_style_transform_pivot_x(widgets.value, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(widgets.value, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(widgets.value, 400, 0);
  widgets.unit = label(widgets.scr, &lv_font_montserrat_20, palette.muted, LV_ALIGN_CENTER, 0, 40, "% actual");
  widgets.value2 = label(widgets.scr, &lv_font_montserrat_28, palette.accent2, LV_ALIGN_CENTER, 0, 85, "--");
  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_16, palette.muted, LV_ALIGN_BOTTOM_MID, 0, -40,
                      "target -- %   speed -- km/h");
  if (pageCfg.signal2 != SIG_NONE) // the page's second signal, under the mode
  {
    widgets.quad[0] = glassLabel(widgets.scr, &lv_font_montserrat_20, palette.text, LV_ALIGN_CENTER, 0, 124, "");
  }
}

// quad page: the page's value and its three slots (the page's slots - catalogue signals or the page source's values)
static signalId_t quadSignal(const GaugePageCfg &pageCfg, int slot)
{
  if (slot == 0)
  {
    return (signalId_t)pageCfg.signal;
  }
  const int pageIndex = &pageCfg - settings.pages;
  return (signalId_t)(pageIndex >= 0 && pageIndex < MAX_PAGES ? settings.pages[pageIndex].slots[slot - 1] : SIG_NONE);
}

static void buildQuad(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  static const int qx[4] = {-105, 105, -105, 105};
  static const int qy[4] = {-85, -85, 75, 75};
  widgets.name = glassLabel(widgets.scr, &lv_font_montserrat_18, palette.accent, LV_ALIGN_CENTER, 0, -190,
                       pageCfg.label[0] ? pageCfg.label : "QUAD");
  for (int slot = 0; slot < 4; slot++)
  {
    widgets.valueIcon[slot] = makeValueIcon(widgets.scr, qx[slot], qy[slot] - 77); // 0.31.0: above its name
    widgets.quadName[slot] = label(widgets.scr, &lv_font_montserrat_16, palette.muted, LV_ALIGN_CENTER, qx[slot], qy[slot] - 45,
                                   signalName(quadSignal(pageCfg, slot)));
    widgets.quad[slot] = label(widgets.scr, &lv_font_montserrat_40, palette.text, LV_ALIGN_CENTER, qx[slot], qy[slot], "--");
    widgets.quadUnit[slot] = label(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, qx[slot], qy[slot] + 38,
                                   signalUnit(quadSignal(pageCfg, slot)));
    fitOneLine(widgets.quadName[slot], 150); // 0.30.2: a long signal name stays in its corner
    fitOneLine(widgets.quadUnit[slot], 150);
  }
  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_BOTTOM_MID, 0, -30, "");
}

// 0.31.0: Triple - Quad with one fewer: the page signal big on top, slots 1 and 2 side by side below. It uses the same
// quad[] / quadName[] / quadUnit[] labels (0-2), so refreshPage's Quad code fills it in.
static void buildTriple(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  static const int tripleX[3] = {0, -105, 105};  // top middle, bottom left, bottom right
  static const int tripleY[3] = {-80, 80, 80};
  widgets.name = glassLabel(widgets.scr, &lv_font_montserrat_18, palette.accent, LV_ALIGN_CENTER, 0, -190,
                            pageCfg.label[0] ? pageCfg.label : "TRIPLE");
  for (int slot = 0; slot < 3; slot++)
  {
    const lv_font_t *valueFont = &lv_font_montserrat_40;
    if (slot == 0)
    {
      valueFont = &lv_font_montserrat_48; // the top one is the page's main value: a size up
    }
    widgets.valueIcon[slot] = makeValueIcon(widgets.scr, tripleX[slot], tripleY[slot] - 80); // 0.31.0: above its name
    widgets.quadName[slot] = label(widgets.scr, &lv_font_montserrat_16, palette.muted, LV_ALIGN_CENTER, tripleX[slot],
                                   tripleY[slot] - 48, signalName(quadSignal(pageCfg, slot)));
    widgets.quad[slot] = label(widgets.scr, valueFont, palette.text, LV_ALIGN_CENTER, tripleX[slot], tripleY[slot], "--");
    widgets.quadUnit[slot] = label(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, tripleX[slot],
                                   tripleY[slot] + 40, signalUnit(quadSignal(pageCfg, slot)));
    int nameWidth = 150; // the two bottom ones share the width
    if (slot == 0)
    {
      nameWidth = 300; // the top one has the glass to itself
    }
    fitOneLine(widgets.quadName[slot], nameWidth);
    fitOneLine(widgets.quadUnit[slot], nameWidth);
  }
  widgets.sub = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_BOTTOM_MID, 0, -30, "");
}

// OEM oil page: temperature arc + value, level OK / LOW, and the source's connection status (a K-line cluster login takes
// a few seconds, so the page says what it is doing instead of just showing "--")
static void buildOil(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  buildArc(widgets, pageCfg); // arc, ticks, value, unit, name (bottom), sub
  lv_obj_align(widgets.value, LV_ALIGN_CENTER, 0, -40);
  lv_obj_align(widgets.unit, LV_ALIGN_CENTER, 0, 8);
  lv_label_set_text(widgets.name, pageCfg.label[0] ? pageCfg.label : "OIL");
  lv_obj_align(widgets.name, LV_ALIGN_CENTER, 0, -100);
  fitOneLine(widgets.name, glassWidthAt(-100, lv_font_get_line_height(&lv_font_montserrat_24))); // wider up here
  widgets.value2 = label(widgets.scr, &lv_font_montserrat_24, palette.muted, LV_ALIGN_CENTER, 0, 50, "LEVEL  --"); // oil level OK / LOW
  lv_obj_set_width(widgets.sub, 300);
  lv_obj_set_style_text_align(widgets.sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(widgets.sub, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(widgets.sub, &lv_font_montserrat_16, 0);
  lv_obj_align(widgets.sub, LV_ALIGN_CENTER, 0, 92);
  fitOneLine(widgets.sub, glassWidthAt(92, lv_font_get_line_height(&lv_font_montserrat_16))); // 0.30.2: its new place
  widgets.spinner = lv_spinner_create(widgets.scr);
  lv_spinner_set_anim_params(widgets.spinner, 1000, 90);
  lv_obj_set_size(widgets.spinner, 28, 28);
  lv_obj_align(widgets.spinner, LV_ALIGN_CENTER, 0, 130);
  lv_obj_set_style_arc_width(widgets.spinner, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(widgets.spinner, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(widgets.spinner, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_arc_color(widgets.spinner, accentColor(), LV_PART_INDICATOR);
  lv_obj_remove_flag(widgets.spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(widgets.spinner, LV_OBJ_FLAG_HIDDEN);
  // source line where the arc page has its name
  widgets.quadName[0] = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_BOTTOM_MID, 0, -34, "");
}

// ---------------------------------------------------------------------------
// Custom page (approved layout "A - centre stack"): the arc is the main gauge (page signal, min / max / redline), its
// value sits high in the dial, and three small icon readouts are stacked in the centre (the page's slots: top,
// middle, bottom). Values that are a state rather than a number (oil pressure / level) read OK / LOW.
// ---------------------------------------------------------------------------
// 0.31.0: does this (already lower-case) text contain this word?
static bool textHas(const char *lowerText, const char *word)
{
  return strstr(lowerText, word) != NULL;
}

// 0.31.0: an icon for any value from its name and unit - a DBC signal, an OBD PID, a K-line field, an ECU channel, an
// ME7 variable or an ADC sensor named on the adc2can ("Oil temp" in C gets the oil can). ICON_NONE when nothing fits.
static uint8_t iconFromNameAndUnit(signalId_t id)
{
  char name[NAME_LEN + 1];
  copyStr(name, sizeof(name), signalName(id));
  for (uint8_t i = 0; name[i] != 0; i++)
  {
    name[i] = (char)tolower((unsigned char)name[i]); // "Oil Temp" and "OIL_TEMP" read the same
  }
  const char *unit = signalUnit(id);
  const bool isTemperature = !strcmp(unit, "C") || !strcmp(unit, "F");
  const bool isPressure = !strcmp(unit, "bar") || !strcmp(unit, "mbar") || !strcmp(unit, "kPa") || !strcmp(unit, "psi") ||
                          !strcmp(unit, "hPa") || textHas(name, "press");

  // 0.37.6: the engine warning light (EML / MIL): "Distance MIL on", "Time MIL on", "... since codes cleared"
  if (textHas(name, "mil on") || textHas(name, "eml") || textHas(name, "malfunction") || textHas(name, "check engine") ||
      textHas(name, "codes cleared"))
  {
    return ICON_EML;
  }
  if (textHas(name, "oil"))
  {
    if (isPressure)
    {
      return ICON_OIL_PRESS;
    }
    if (textHas(name, "level"))
    {
      return ICON_OIL_LEVEL;
    }
    return ICON_OIL_TEMP;
  }
  if (textHas(name, "coolant") || textHas(name, "water") || textHas(name, "radiator") || textHas(name, "clt"))
  {
    return ICON_COOLANT;
  }
  if (isTemperature && (textHas(name, "gearbox") || textHas(name, "trans") || textHas(name, "atf") || textHas(name, "dsg") ||
                        textHas(name, "diff") || textHas(name, "clutch")))
  {
    return ICON_GEARBOX_TEMP;
  }
  if (textHas(name, "ambient") || textHas(name, "outside") || textHas(name, "outdoor"))
  {
    return ICON_OUTSIDE;
  }
  if (isTemperature && (textHas(name, "intake") || textHas(name, "iat") || textHas(name, "charge") || textHas(name, "inlet") ||
                        textHas(name, "air")))
  {
    return ICON_INTAKE;
  }
  if (textHas(name, "boost") || textHas(name, "map") || textHas(name, "manifold") || textHas(name, "turbo"))
  {
    return ICON_BOOST;
  }
  if (textHas(name, "fuel") || textHas(name, "ethanol") || textHas(name, "flex"))
  {
    if (isPressure)
    {
      return ICON_PRESSURE; // fuel pressure
    }
    return ICON_FUEL;
  }
  if (textHas(name, "lambda") || textHas(name, "afr") || textHas(name, "o2") || textHas(name, "wideband") ||
      !strcmp(unit, "lambda") || !strcmp(unit, "AFR"))
  {
    return ICON_LAMBDA;
  }
  if (textHas(name, "knock") || textHas(name, "timing") || textHas(name, "ignition") || textHas(name, "advance") ||
      textHas(name, "spark"))
  {
    return ICON_IGNITION;
  }
  if (textHas(name, "inj") || textHas(name, "duty"))
  {
    return ICON_INJECTOR;
  }
  if (textHas(name, "brake"))
  {
    return ICON_BRAKE;
  }
  if (textHas(name, "throttle") || textHas(name, "pedal") || textHas(name, "tps") || textHas(name, "load") ||
      textHas(name, "accel"))
  {
    return ICON_THROTTLE;
  }
  if (textHas(name, "rpm") || textHas(name, "engine speed") || textHas(name, "revs") || !strcmp(unit, "rpm"))
  {
    return ICON_RPM;
  }
  if (textHas(name, "speed") || !strcmp(unit, "km/h") || !strcmp(unit, "mph"))
  {
    return ICON_SPEED;
  }
  if (textHas(name, "haldex"))
  {
    return ICON_HALDEX;
  }
  if (textHas(name, "gear"))
  {
    return ICON_GEAR;
  }
  if (textHas(name, "batt"))
  {
    return ICON_BATTERY;
  }
  if (isTemperature)
  {
    return ICON_TEMP; // any other temperature (EGT, a sensor named "Temp 1" ...)
  }
  if (isPressure)
  {
    return ICON_PRESSURE;
  }
  if (!strcmp(unit, "V") || textHas(name, "volt"))
  {
    return ICON_VOLTS; // a voltage that isn't the battery (a raw ADC sensor ...)
  }
  return ICON_NONE;
}

// the icon for a value: the gauge's own signals by what they are, anything else by its name and unit
static uint8_t signalIcon(signalId_t id)
{
  switch (id)
  {
  case SIG_NONE: return ICON_NONE;
  case SIG_COOLANT: return ICON_COOLANT;
  case SIG_OIL_TEMP: return ICON_OIL_TEMP;
  case SIG_OIL_PRESS:
  case SIG_OIL_PRESSURE: return ICON_OIL_PRESS;
  case SIG_OIL_LEVEL: return ICON_OIL_LEVEL;
  case SIG_BATTERY:
  case SIG_HALDEX_VOLTS: return ICON_BATTERY;
  case SIG_BOOST:
  case SIG_BOOST_REL: return ICON_BOOST;
  case SIG_IAT:
  case SIG_MAF: return ICON_INTAKE;
  case SIG_AMBIENT: return ICON_OUTSIDE;
  case SIG_FUEL_LEVEL:
  case SIG_FUEL_LITRES:
  case SIG_ETHANOL: return ICON_FUEL;
  case SIG_FUEL_PRESSURE:
  case SIG_BARO: return ICON_PRESSURE;
  case SIG_SPEED:
  case SIG_TCU_SPEED:
  case SIG_HALDEX_SPEED: return ICON_SPEED;
  case SIG_RPM:
  case SIG_INPUT_RPM: return ICON_RPM;
  case SIG_LAMBDA:
  case SIG_LAMBDA_TARGET: return ICON_LAMBDA;
  case SIG_GEARBOX_TEMP: return ICON_GEARBOX_TEMP;
  case SIG_EGT:
  case SIG_BOARD_TEMP: return ICON_TEMP;
  case SIG_TIMING:
  case SIG_KNOCK_RETARD: return ICON_IGNITION;
  case SIG_INJ_DUTY: return ICON_INJECTOR;
  case SIG_HALDEX_TARGET:
  case SIG_HALDEX_ACTUAL:
  case SIG_HALDEX_MODE:
  case SIG_HALDEX_CLUTCH_TEMP:
  case SIG_HALDEX_OIL_TEMP:
    return ICON_HALDEX;
  case SIG_THROTTLE:
  case SIG_PEDAL:
  case SIG_HALDEX_PEDAL:
  case SIG_LOAD: return ICON_THROTTLE;
  case SIG_GEAR:
  case SIG_SELECTOR:
  case SIG_SHIFTING:
  case SIG_LOCKUP:
  case SIG_CONVERTER_SLIP: return ICON_GEAR;
  default:
    return iconFromNameAndUnit(id); // 0.31.0: page sources, ADC sensors, ME7 slots ...
  }
}

#define SLOT_ROW_H 40
static const int slotRowY[3] = {-6, 34, 74}; // row centres relative to the dial centre

static void buildCustom(PageWidgets &widgets, const GaugePageCfg &pageCfg, uint8_t pageCfgIndex)
{
  buildArc(widgets, pageCfg); // arc, ticks, value, unit, name, sub
  // main value high in the dial: 48 px font scaled 1.25x around its centre
  lv_obj_set_style_transform_pivot_x(widgets.value, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(widgets.value, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(widgets.value, 320, 0);
  lv_obj_align(widgets.value, LV_ALIGN_CENTER, 0, -101);
  lv_obj_set_style_text_font(widgets.unit, &lv_font_montserrat_18, 0);
  lv_obj_align(widgets.unit, LV_ALIGN_CENTER, 0, -58);
  lv_obj_align(widgets.name, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_align(widgets.sub, LV_ALIGN_CENTER, 0, 112);
  // 0.30.2: their widths for their new places (the name sits higher than the arc page's, the line lower)
  fitOneLine(widgets.name, glassWidthAt(240 - 40 - 12, lv_font_get_line_height(&lv_font_montserrat_24)));
  fitOneLine(widgets.sub, glassWidthAt(112, lv_font_get_line_height(&lv_font_montserrat_14)));
  // separators above, between and below the three rows
  for (int i = 0; i <= 3; i++)
  {
    lv_obj_t *line = lv_obj_create(widgets.scr);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 200, 1);
    lv_obj_set_style_bg_color(line, lv_color_lighten(colour(palette.panel), 20), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, slotRowY[0] - SLOT_ROW_H / 2 + i * SLOT_ROW_H);
  }
  for (int slot = 0; slot < 3; slot++)
  {
    const signalId_t id = (signalId_t)settings.pages[pageCfgIndex].slots[slot];
    if (id == SIG_NONE)
    {
      continue; // empty slot: the row stays blank
    }
    uint8_t icon = signalIcon(id);
    if (icon == ICON_TEMP || icon == ICON_PRESSURE || icon == ICON_VOLTS)
    {
      icon = ICON_NONE; // 0.31.0: a generic icon here would hide WHICH value it is ("ADC 2" / "ADC 3"): its name instead
    }
    if (icon != ICON_NONE)
    {
      lv_obj_t *image = lv_image_create(widgets.scr);
      lv_image_set_src(image, gaugeIcon(icon));
      lv_obj_set_style_image_recolor(image, accentColor(), 0);
      lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
      lv_obj_align(image, LV_ALIGN_CENTER, -83, slotRowY[slot]);
      widgets.quadName[slot] = image;
    }
    else
    { // no icon for this one (ME7 slots, board values ...): its short name instead
      widgets.quadName[slot] = label(widgets.scr, &lv_font_montserrat_12, palette.accent, LV_ALIGN_CENTER, -83, slotRowY[slot],
                                     signalName(id));
      lv_obj_set_width(widgets.quadName[slot], 44);
      lv_label_set_long_mode(widgets.quadName[slot], LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_align(widgets.quadName[slot], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(widgets.quadName[slot], LV_ALIGN_CENTER, -83, slotRowY[slot]);
    }
    // value right-aligned to x +62, unit left-aligned from x +70 (the approved grid: 34 | value | 30, gap 8)
    widgets.quad[slot] = label(widgets.scr, &lv_font_montserrat_28, palette.text, LV_ALIGN_CENTER, 0, slotRowY[slot], "--");
    lv_obj_set_width(widgets.quad[slot], 120);
    lv_obj_set_style_text_align(widgets.quad[slot], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(widgets.quad[slot], LV_ALIGN_CENTER, 2, slotRowY[slot]);
    widgets.quadUnit[slot] = label(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, 0, slotRowY[slot], "");
    lv_obj_set_width(widgets.quadUnit[slot], 30);
    lv_obj_align(widgets.quadUnit[slot], LV_ALIGN_CENTER, 85, slotRowY[slot] + 3);
  }
}

static void formatValue(char *buf, size_t len, float value, uint8_t decimals); // value formatting / refresh, below

// a value's decimals on the glass. 0.31.0: the shared rule (signalDecimals, src/can2gauge_signals.cpp), so every
// display shows a value the same way - before, an ADC sensor came out as whole numbers here
static uint8_t slotDecimals(signalId_t id)
{
  return signalDecimals(id);
}

// 0.36.14: the decimals of a page's own value (and its Min / Max): the page's Decimals, but never fewer than the value
// has (bench: lambda showed "1" on a page left at Decimals 0, while its Graph showed 1.00). The page's Decimals can
// add places, not take them away.
static uint8_t pageDecimals(const GaugePageCfg &pageCfg, signalId_t id)
{
  uint8_t decimals = pageCfg.decimals; // the page's Decimals box
  const uint8_t ownDecimals = slotDecimals(id); // what the value itself has (lambda 2, volts 1 ...)
  if (ownDecimals > decimals)
  {
    decimals = ownDecimals;
  }
  if (decimals > 2)
  {
    decimals = 2; // formatValue shows at most two
  }
  return decimals;
}

// The refresh runs 50 times a second, and LVGL invalidates (redraws) an object on every text or style set, even an
// unchanged one: these touch it only when something changed (0.11.0 - a still page used to redraw its ring and labels
// at 50 Hz).
static void labelSet(lv_obj_t *label, const char *text)
{
  if (label && strcmp(lv_label_get_text(label), text))
  {
    lv_label_set_text(label, text);
  }
}

static void labelFormat(lv_obj_t *label, const char *fmt, ...)
{
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  labelSet(label, buf);
}

static void textColor(lv_obj_t *obj, lv_color_t color)
{
  if (obj && !lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color))
  {
    lv_obj_set_style_text_color(obj, color, 0);
  }
}

static void arcColor(lv_obj_t *obj, lv_color_t color) // the indicator
{
  if (obj && !lv_color_eq(lv_obj_get_style_arc_color(obj, LV_PART_INDICATOR), color))
  {
    lv_obj_set_style_arc_color(obj, color, LV_PART_INDICATOR);
  }
}

static void bgColor(lv_obj_t *obj, lv_color_t color, lv_part_t part)
{
  if (obj && !lv_color_eq(lv_obj_get_style_bg_color(obj, part), color))
  {
    lv_obj_set_style_bg_color(obj, color, part);
  }
}

static void refreshSlots(PageWidgets &widgets)
{
  char buf[24];
  for (int slot = 0; slot < 3; slot++)
  {
    if (!widgets.quad[slot])
    {
      continue;
    }
    const signalId_t id = (signalId_t)settings.pages[widgets.pageCfgIndex].slots[slot];
    const bool stateSignal = id == SIG_OIL_PRESS || id == SIG_OIL_LEVEL; // OK / LOW, no unit
    const char *unit = "";
    const float value = signalDisplayValue(id, signalGet(id), &unit);
    uint32_t color = palette.text;
    if (stateSignal)
    {
      unit = "";
    }
    if (!signalValid(id))
    {
      strcpy(buf, "--");
      color = palette.muted;
    }
    else if (stateSignal)
    {
      const bool ok = value >= 0.5f;
      strcpy(buf, ok ? "OK" : "LOW");
      color = ok ? palette.ok : palette.danger;
    }
    else if (id == SIG_SELECTOR)
    {
      copyStr(buf, sizeof(buf), selectorLetter((uint8_t)value));
    }
    else if (id == SIG_HALDEX_MODE)
    {
      copyStr(buf, sizeof(buf), haldexModeName((int)value));
    }
    else
    {
      formatValue(buf, sizeof(buf), value, slotDecimals(id));
    }
    labelSet(widgets.quad[slot], buf);
    textColor(widgets.quad[slot], colour(color));
    labelSet(widgets.quadUnit[slot], unit);
  }
}

// DSG view: title, P R N D S M strip, the big selector + gear, speed. Shared by menu > DSG and STYLE_GEAR pages.
static void buildGear(lv_obj_t *scr, GearWidgets &gear, const char *title)
{
  memset(&gear, 0, sizeof(gear));
  gear.lastStrip = -2;
  label(scr, &lv_font_montserrat_20, palette.accent, LV_ALIGN_CENTER, 0, -185, title);
  static const char *letters[GEAR_STRIP] = {"P", "R", "N", "D", "S", "M"};
  for (int i = 0; i < GEAR_STRIP; i++)
  {
    gear.strip[i] = label(scr, &lv_font_montserrat_28, palette.muted, LV_ALIGN_CENTER, -125 + i * 50, -135, letters[i]);
  }
  gear.big = label(scr, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, -15, "--");
  // 48 px font scaled 3x around its own centre -> ~140 px glyphs
  lv_obj_set_style_transform_pivot_x(gear.big, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(gear.big, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(gear.big, 768, 0);
  gear.speed = label(scr, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, 110, "--");
  gear.speedUnit = glassLabel(scr, &lv_font_montserrat_16, palette.muted, LV_ALIGN_CENTER, 0, 150, "");
  gear.sub = glassLabel(scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, 0, 178, "");
}

static void refreshGear(GearWidgets &gear)
{
  const bool selectorOk = signalValid(SIG_SELECTOR);
  const bool gearOk = signalValid(SIG_GEAR);
  const uint8_t selector = selectorOk ? (uint8_t)signalGet(SIG_SELECTOR) : SEL_NONE;
  const int gearNumber = gearOk ? (int)lroundf(signalGet(SIG_GEAR)) : -1;
  char bigText[8];
  if (selector == SEL_P || selector == SEL_R || selector == SEL_N)
  {
    snprintf(bigText, sizeof(bigText), "%s", selectorLetter(selector));
  }
  else if (selector != SEL_NONE && gearNumber > 0)
  {
    snprintf(bigText, sizeof(bigText), "%s%d", selectorLetter(selector), gearNumber);
  }
  else if (selector != SEL_NONE)
  {
    snprintf(bigText, sizeof(bigText), "%s", selectorLetter(selector));
  }
  else if (gearNumber > 0)
  {
    snprintf(bigText, sizeof(bigText), "%d", gearNumber);
  }
  else
  {
    snprintf(bigText, sizeof(bigText), "--");
  }
  if (strcmp(bigText, gear.lastBig) != 0)
  {
    copyStr(gear.lastBig, sizeof(gear.lastBig), bigText);
    labelSet(gear.big, bigText);
    lv_color_t bigColor;
    if (selector == SEL_R)
    {
      bigColor = colour(palette.accent2);
    }
    else if (selector == SEL_M || selector == SEL_S)
    {
      bigColor = accentColor();
    }
    else
    {
      bigColor = colour(palette.text);
    }
    textColor(gear.big, bigColor);
  }

  // strip: P R N D S M (E and L light D, B lights D)
  static const uint8_t stripSel[GEAR_STRIP] = {SEL_P, SEL_R, SEL_N, SEL_D, SEL_S, SEL_M};
  int8_t litStrip = -1;
  for (int i = 0; i < GEAR_STRIP; i++)
  {
    if (stripSel[i] == selector || (i == 3 && (selector == SEL_E || selector == SEL_L || selector == SEL_B)))
    {
      litStrip = i;
    }
  }
  if (litStrip != gear.lastStrip)
  {
    gear.lastStrip = litStrip;
    for (int i = 0; i < GEAR_STRIP; i++)
    {
      textColor(gear.strip[i], i == litStrip ? accentColor() : colour(palette.muted));
    }
  }

  // speed: gearbox (UDS / TP2.0 / ELM) or vehicle (CAN / OBD / K-line) per settings.dsgSpeedPref
  const bool tcuOk = signalValid(SIG_TCU_SPEED) && settings.dsgSpeedPref != 2;
  const bool vehicleOk = signalValid(SIG_SPEED) && settings.dsgSpeedPref != 1;
  const signalId_t speedSignal = tcuOk ? SIG_TCU_SPEED : SIG_SPEED;
  const char *unit = "km/h";
  if (tcuOk || vehicleOk)
  {
    const float speed = signalDisplayValue(speedSignal, signalGet(speedSignal), &unit);
    labelFormat(gear.speed, "%d", (int)lroundf(speed));
    labelFormat(gear.speedUnit, "%s  %s", unit, tcuOk ? "gearbox" : "vehicle");
  }
  else
  {
    labelSet(gear.speed, "--");
    labelSet(gear.speedUnit, "no speed");
  }
  // 0.26.0: the torque converter lock-up (8HP) / clutch (DSG), when the gearbox gives it: "locked" or "slipping"
  const char *lockupWord = ""; // "" = not known, or open
  if (signalValid(SIG_LOCKUP))
  {
    const int lockup = (int)lroundf(signalGet(SIG_LOCKUP));
    if (lockup == 2)
    {
      lockupWord = "locked";
    }
    else if (lockup == 1)
    {
      lockupWord = "slipping";
    }
  }
  const bool isGearboxProfile = settings.canProfile == CAN_PROFILE_VAG_PQ || settings.canProfile == CAN_PROFILE_VAG_MQB ||
                                settings.canProfile == CAN_PROFILE_VAG_MLB;
  if (!selectorOk && !gearOk) // why there's nothing: the wrong CAN profile, or no automatic gearbox talking on the bus
  {
    if (isGearboxProfile || settings.dsgSource != DSG_SRC_CAN_ONLY)
    {
      labelSet(gear.sub, "No gearbox heard (manual car?)");
    }
    else
    {
      labelSet(gear.sub, "Pick VAG PQ, MQB or MLB on the CAN tab");
    }
  }
  else if (signalValid(SIG_GEARBOX_TEMP))
  {
    const float gearboxTemp = signalDisplayValue(SIG_GEARBOX_TEMP, signalGet(SIG_GEARBOX_TEMP), &unit);
    if (lockupWord[0])
    {
      labelFormat(gear.sub, "gearbox %d %s  %s", (int)lroundf(gearboxTemp), unit, lockupWord);
    }
    else
    {
      labelFormat(gear.sub, "gearbox %d %s", (int)lroundf(gearboxTemp), unit);
    }
  }
  else
  {
    labelSet(gear.sub, lockupWord);
  }
}

// a page with its Sprite switch on (docs/mascot/MASCOT.md): the pack's canvas centred on the glass, the firmware's ring over it (centreline
// radius 218, 12 px, 270 degrees, the red zone from the redline) and the reading under the figure
#define SPRITE_RING 448
static lv_obj_t *ringArc(lv_obj_t *parent, lv_color_t color, lv_opa_t opa, int32_t from, int32_t to)
{
  lv_obj_t *arc = lv_arc_create(parent);
  lv_obj_set_size(arc, SPRITE_RING, SPRITE_RING);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, from, to);
  lv_arc_set_range(arc, 0, 1000);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
  return arc;
}

static void buildSprite(PageWidgets &widgets, const GaugePageCfg &pageCfg, uint8_t pageCfgIndex)
{
  widgets.sprite = spriteOpen(widgets.scr, settings.pages[pageCfgIndex].sprite);
  if (!widgets.sprite)
  {
    lv_obj_t *noteLabel = label(widgets.scr, &lv_font_montserrat_18, palette.muted, LV_ALIGN_CENTER, 0, -24,
                                "No sprite pack on the gauge\nWeb UI > Settings > Sprites");
    lv_obj_set_style_text_align(noteLabel, LV_TEXT_ALIGN_CENTER, 0);
  }
  ringArc(widgets.scr, colour(palette.panel), LV_OPA_COVER, 0, 270); // the track
  if (pageCfg.redline > widgets.drawnMin && pageCfg.redline < widgets.drawnMax)  // the red zone
  {
    ringArc(widgets.scr, colour(palette.redline), LV_OPA_50,
            (int32_t)lroundf(270.0f * (pageCfg.redline - widgets.drawnMin) / (widgets.drawnMax - widgets.drawnMin)), 270);
  }
  widgets.arc = ringArc(widgets.scr, colour(palette.panel), LV_OPA_TRANSP, 0, 270); // the reading
  lv_obj_set_style_arc_opa(widgets.arc, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(widgets.arc, accentColor(), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(widgets.arc, true, LV_PART_INDICATOR);
  widgets.value = label(widgets.scr, &lv_font_montserrat_40, palette.text, LV_ALIGN_CENTER, 0, 176, "--");
  widgets.unit = glassLabel(widgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_CENTER, 0, 207, "");
}

// 0.15.0: a small spinner for a page with a value from a source that has to connect first (a K-line module's 5-baud
// init, the Bluetooth OBD adapter, VAG diag). Hidden until refreshSourceSpinner shows it beside the note line.
static void buildSourceSpinner(PageWidgets &widgets)
{
  widgets.sourceSpinner = lv_spinner_create(widgets.scr);
  lv_spinner_set_anim_params(widgets.sourceSpinner, 1000, 90); // one turn a second, 90 degree arc (like the oil page's)
  lv_obj_set_size(widgets.sourceSpinner, 16, 16);
  lv_obj_set_style_arc_width(widgets.sourceSpinner, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(widgets.sourceSpinner, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(widgets.sourceSpinner, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_arc_color(widgets.sourceSpinner, accentColor(), LV_PART_INDICATOR);
  lv_obj_remove_flag(widgets.sourceSpinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(widgets.sourceSpinner, LV_OBJ_FLAG_HIDDEN);
}

// does this page need the connecting spinner? Only page styles with a note line, and only when a value comes from a
// source that connects (the others never wait, so they don't get the extra object)
static bool pageNeedsSourceSpinner(const PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  if (widgets.sub == NULL || pageCfg.style == STYLE_OIL || pageCfg.style == STYLE_HALDEX || pageCfg.style == STYLE_GEAR)
  {
    return false; // no note line, or a page that shows its own connection state
  }
  if (userSigPageUses(widgets.pageCfgIndex, USRC_KLINE))
  {
    return true;
  }
  if (userSigPageUses(widgets.pageCfgIndex, USRC_OBD))
  {
    return true;
  }
  if (userSigPageUses(widgets.pageCfgIndex, USRC_DIAG))
  {
    return true;
  }
  return false;
}

// 0.36.4: the range a page is drawn with. A page showing an adc2can sensor scaled on the adc2can takes the adc2can's dial
// range (its Dial Min / Max, or Auto from its scaling - adc2can 1.4+), in the units on show, so the page and the
// adc2can always agree; the page's own Min / Max only count for anything else (or an older adc2can).
static void pageDrawnRange(const GaugePageCfg &pageCfg, float &low, float &high)
{
  low = pageCfg.minVal;
  high = pageCfg.maxVal;
  float adcLow = 0;
  float adcHigh = 0;
  if (adc2canPageRange((signalId_t)pageCfg.signal, adcLow, adcHigh))
  {
    low = adcLow;
    high = adcHigh;
  }
}

static volatile bool pageRangesPending = false; // 0.36.4: an adc2can's dial range changed: rebuild the pages (uiTick)

void uiPageRangesChanged()
{
  pageRangesPending = true;
}

static void buildPage(uint8_t pageIndex, uint8_t pageCfgIndex)
{
  PageWidgets &widgets = pages[pageIndex];
  memset(&widgets, 0, sizeof(widgets));
  widgets.pageCfgIndex = pageCfgIndex;
  widgets.lastValue = -12345.0f;
  const GaugePageCfg &pageCfg = settings.pages[pageCfgIndex];
  widgets.builtStyle = pageCfg.style;                       // 0.30.19: what this page's widgets are (see pageOutOfDate)
  widgets.builtSprite = settings.pages[pageCfgIndex].spriteOn; // and whether it's the sprite in place of the style
  pageDrawnRange(pageCfg, widgets.drawnMin, widgets.drawnMax); // 0.36.4: before anything is built with it
  widgets.scr = newScreen();
  lv_obj_add_event_cb(widgets.scr, pageEventCb, LV_EVENT_GESTURE, NULL);
  lv_obj_add_event_cb(widgets.scr, pageEventCb, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(widgets.scr, pageEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(widgets.scr, pageEventCb, LV_EVENT_SHORT_CLICKED, NULL);
  if (settings.pages[pageCfgIndex].spriteOn)
  {
    buildSprite(widgets, pageCfg, pageCfgIndex); // the page's sprite reacting to its reading, in place of its style
  }
  else
  {
    switch (pageCfg.style)
    {
    case STYLE_NEEDLE: buildNeedle(widgets, pageCfg); break;
    case STYLE_BAR: buildBar(widgets, pageCfg); break;
    case STYLE_NUMERIC: buildNumeric(widgets, pageCfg); break;
    case STYLE_DUAL: buildDual(widgets, pageCfg); break;
    case STYLE_HALDEX: buildHaldex(widgets, pageCfg); break;
    case STYLE_QUAD: buildQuad(widgets, pageCfg); break;
    case STYLE_TRIPLE: buildTriple(widgets, pageCfg); break; // 0.31.0
    case STYLE_GEAR: buildGear(widgets.scr, widgets.gear, pageCfg.label[0] ? pageCfg.label : "DSG"); break;
    case STYLE_OIL: buildOil(widgets, pageCfg); break;
    case STYLE_CUSTOM: buildCustom(widgets, pageCfg, pageCfgIndex); break;
    case STYLE_GRAPH: buildGraph(widgets, pageCfg); break; // 0.31.0
    default: buildArc(widgets, pageCfg); break;
    }
    if (pageNeedsSourceSpinner(widgets, pageCfg))
    {
      buildSourceSpinner(widgets); // 0.15.0: spins while a K-line / OBD adapter / VAG diag value connects
    }
  }
  // page dots: one per page, this page's larger and in the accent colour (the menu's dots, drawn the same way).
  // 0.24.12: they were text ("●" / "·"), which the built-in Montserrat font doesn't have - on the glass each one drew
  // as an empty box along the bottom.
  const int32_t dotsWidth = (pageCount - 1) * 14;
  for (uint8_t dotIndex = 0; dotIndex < pageCount; dotIndex++)
  {
    lv_obj_t *dot = plainObject(widgets.scr);
    int32_t size = 6;
    lv_color_t dotColour = colour(palette.muted);
    if (dotIndex == pageIndex)
    {
      size = 9;
      dotColour = accentColor();
    }
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, dotColour, 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, -dotsWidth / 2 + dotIndex * 14, -12);
  }
}

// ---------------------------------------------------------------------------
// value formatting / refresh
// ---------------------------------------------------------------------------
static void formatValue(char *buf, size_t len, float value, uint8_t decimals)
{
  value = valueNoMinusZero(value, decimals); // 0.00, never -0.00
  if (decimals == 0)
  {
    snprintf(buf, len, "%ld", lroundf(value));
  }
  else if (decimals == 1)
  {
    snprintf(buf, len, "%.1f", value);
  }
  else
  {
    snprintf(buf, len, "%.2f", value);
  }
}

static int32_t permille(float value, float lo, float hi)
{
  if (hi <= lo)
  {
    return 0;
  }
  float fraction = (value - lo) / (hi - lo);
  if (fraction < 0)
  {
    fraction = 0;
  }
  if (fraction > 1)
  {
    fraction = 1;
  }
  return (int32_t)(fraction * 1000.0f);
}

static void setHidden(lv_obj_t *obj, bool hidden)
{
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != hidden)
  {
    if (hidden)
    {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
      lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void refreshOil(PageWidgets &widgets, const GaugePageCfg &pageCfg)
{
  runtime.oilPageSeenMs = millis() | 1; // on screen: with oil source = K-line this is what opens (and keeps) the cluster session
  const OilStatus &oil = runtime.oil;
  const bool kline = settings.oilSource == OIL_SRC_KLINE;
  const bool valid = signalValid(SIG_OIL_TEMP);
  const char *unit = "";
  char buf[64];
  const float value = signalDisplayValue(SIG_OIL_TEMP, signalGet(SIG_OIL_TEMP), &unit);
  if (valid)
  {
    formatValue(buf, sizeof(buf), value, pageDecimals(pageCfg, SIG_OIL_TEMP));
  }
  else
  {
    strcpy(buf, "--");
  }
  labelSet(widgets.value, buf);
  labelSet(widgets.unit, unit);
  // 0.27.0: Min / Max / Redline are in the shown unit (C or F), so the arc and the redline use the shown value
  const bool red = (pageCfg.redline > widgets.drawnMin) && valid && value >= pageCfg.redline;
  const float smoothedShown = signalDisplayValue(SIG_OIL_TEMP, smoothTo(widgets.smooth[0], signalGet(SIG_OIL_TEMP), valid), NULL);
  lv_arc_set_value(widgets.arc, valid ? permille(smoothedShown, widgets.drawnMin, widgets.drawnMax) : 0);
  arcColor(widgets.arc, red ? colour(palette.redline) : accentColor());
  textColor(widgets.value, red ? colour(palette.redline) : colour(palette.text));

  // level: only the cluster's measuring block has it (Kombi_2 carries the temperature alone)
  if (kline && signalValid(SIG_OIL_LEVEL))
  {
    const bool ok = signalGet(SIG_OIL_LEVEL) >= 0.5f;
    labelSet(widgets.value2, ok ? "LEVEL  OK" : "LEVEL  LOW");
    textColor(widgets.value2, ok ? colour(palette.ok) : colour(palette.danger));
  }
  else
  {
    const char *levelText;
    if (kline && oil.connected && oil.levelField == 0xFF)
    {
      levelText = "LEVEL  n/a";
    }
    else if (kline)
    {
      levelText = "LEVEL  --";
    }
    else
    {
      levelText = "LEVEL  K-line only";
    }
    labelSet(widgets.value2, levelText);
    textColor(widgets.value2, colour(palette.muted));
  }

  // connection status
  bool busy = false;
  static const char *dots[3] = {".", "..", "..."};
  const char *dotText = dots[(millis() / 400) % 3];
  if (kline)
  {
    if (oil.connecting)
    {
      snprintf(buf, sizeof(buf), "%s%s", oil.state, dotText);
      busy = true;
    }
    else if (oil.connected)
    {
      // "1J0920926C  group 3" once values flow, the state ("Reading group 3") until then
      char clusterPart[16];
      size_t partLen = 0;
      while (oil.clusterId[partLen] && oil.clusterId[partLen] != ' ' && partLen < sizeof(clusterPart) - 1)
      {
        clusterPart[partLen] = oil.clusterId[partLen];
        partLen++;
      }
      clusterPart[partLen] = '\0';
      if (valid)
      {
        snprintf(buf, sizeof(buf), "%s  group %u", partLen ? clusterPart : "cluster", oil.group);
      }
      else
      {
        snprintf(buf, sizeof(buf), "%s%s", oil.state, dotText);
      }
      busy = !valid;
    }
    else if (!strcmp(runtime.klineState, "ECU read"))
    {
      snprintf(buf, sizeof(buf), "K-line busy: ECU flash read");
    }
    else
    { // the previous session is closing / the task has not picked the page up yet / retry pause
      snprintf(buf, sizeof(buf), "%s%s", oil.state[0] && strcmp(oil.state, "Closed") ? oil.state : "Opening the K-line", dotText);
      busy = true;
    }
  }
  else if (!settings.canEnabled)
  {
    snprintf(buf, sizeof(buf), "CAN is off (web UI > CAN)");
  }
  else if (settings.canProfile != CAN_PROFILE_VAG_PQ)
  {
    snprintf(buf, sizeof(buf), "Needs the VAG PQ CAN profile");
  }
  else if (!oil.kombi2Ms)
  {
    snprintf(buf, sizeof(buf), "Waiting for Kombi_2 (0x420)%s", dotText);
    busy = true;
  }
  else if (millis() - oil.kombi2Ms > kombi2Timeout)
  {
    snprintf(buf, sizeof(buf), "Kombi_2 (0x420) lost");
  }
  else if (oil.kombi2OilErr || oil.kombi2OilRaw == 0xFF)
  {
    snprintf(buf, sizeof(buf), "Cluster reports no oil sensor");
  }
  else
  {
    snprintf(buf, sizeof(buf), "Kombi_2 0x420");
  }
  labelSet(widgets.sub, buf);
  textColor(widgets.sub, busy || valid ? colour(palette.muted) : colour(palette.accent2));
  setHidden(widgets.spinner, !busy);
  if (kline)
  {
    snprintf(buf, sizeof(buf), "K-line  cluster 0x%02X", settings.oilClusterAddr);
  }
  else
  {
    snprintf(buf, sizeof(buf), "CAN  Kombi_2");
  }
  labelSet(widgets.quadName[0], buf);
}

static void refreshHaldex(PageWidgets &widgets) // Haldex pages and the Haldex mode screen
{
  const char *unit = "";
  {
    const bool link = signalValid(SIG_HALDEX_ACTUAL);
    float actual = signalGet(SIG_HALDEX_ACTUAL);
    float target = signalGet(SIG_HALDEX_TARGET);
    const float smoothActual = smoothTo(widgets.smooth[0], actual, link);
    const float smoothTarget = smoothTo(widgets.smooth[1], target, link);
    lv_arc_set_value(widgets.arc, link ? (int32_t)(smoothActual * 10.0f) : 0);
    lv_arc_set_value(widgets.arc2, link ? (int32_t)(smoothTarget * 10.0f) : 0);
    if (link)
    {
      labelFormat(widgets.value, "%d", (int)actual);
    }
    else
    {
      labelSet(widgets.value, "--");
    }
    // OpenHaldex heard over WiFi but its Haldex CAN is down: say so rather than "no link"
    const bool ohxHeard = runtime.ohxRxMs && millis() - runtime.ohxRxMs < haldexTimeout;
    const char *modeText;
    if (link)
    {
      modeText = haldexModeName((int)signalGet(SIG_HALDEX_MODE));
    }
    else if (ohxHeard)
    {
      modeText = "NO HALDEX";
    }
    else
    {
      modeText = "NO LINK";
    }
    labelSet(widgets.value2, modeText);
    float speed = signalDisplayValue(SIG_HALDEX_SPEED, signalGet(SIG_HALDEX_SPEED), &unit);
    labelFormat(widgets.sub, "target %d %%   speed %d %s", (int)target, (int)speed, unit);
  }
  if (widgets.quad[0]) // a Haldex page's second signal
  {
    const GaugePageCfg &pageCfg = settings.pages[widgets.pageCfgIndex];
    const signalId_t id2 = (signalId_t)pageCfg.signal2;
    char valueText[24] = "--";
    const char *unit2 = "";
    const float value2 = signalDisplayValue(id2, signalGet(id2), &unit2);
    if (signalValid(id2))
    {
      formatValue(valueText, sizeof(valueText), value2, slotDecimals(id2)); // 0.31.0: the second value's own decimals
    }
    labelFormat(widgets.quad[0], "%s  %s %s", signalName(id2), valueText, signalValid(id2) ? unit2 : "");
  }
}

// the sources a page on the glass keeps open. Also stamped under the screensaver, so the page's values (and the
// speed the screensaver watches) keep coming.
static void pageClaims(const PageWidgets &widgets)
{
  runtime.pageCfgIndex = widgets.pageCfgIndex; // the OBD polling asks for this page's PIDs in its fast group
  runtime.pageSeenMs = millis() | 1;
  if (userSigPageUses(widgets.pageCfgIndex, USRC_KLINE)) // on screen = the K-line task opens the page's module
  {
    runtime.klinePageCfgIndex = widgets.pageCfgIndex;
    runtime.klinePageSeenMs = millis() | 1;
  }
  if (userSigPageUses(widgets.pageCfgIndex, USRC_OBD)) // ... and the Bluetooth OBD adapter connects
  {
    runtime.elmModeSeenMs = millis() | 1;
  }
}

// a sprite page: the pack picks its clip from the page's reading (and the other signals its rules name)
static void refreshSprite(PageWidgets &widgets, const GaugePageCfg &pageCfg, float smoothedValue, bool valid)
{
  if (!widgets.sprite)
  {
    return;
  }
  SpriteContext context = {};
  context.mode = SPR_PAGE;
  context.pageSignal = pageCfg.signal;
  context.valid = valid;
  context.value = smoothedValue;
  context.minVal = widgets.drawnMin;
  context.maxVal = widgets.drawnMax;
  context.redline = pageCfg.redline;
  context.accent = palette.accent;
  spriteUpdate(widgets.sprite, context);
}

// 0.30.19: true when the page's settings no longer match what it was built as. The web UI saves the new style first
// and rebuilds the page just after, while this task keeps refreshing on the other core. In that gap a Custom page
// switched to Quad was refreshed as Quad: its icons (images) were read as text labels and the gauge crashed. Going the
// other way, a Quad page refreshed as a ring style had no ring, LVGL's null check stopped the screen task for good
// (screen frozen) and the rebuild never got in. Until the rebuild comes, the page is just left as it is.
static bool pageOutOfDate(const PageWidgets &widgets)
{
  const GaugePageCfg &pageCfg = settings.pages[widgets.pageCfgIndex];
  if (pageCfg.style != widgets.builtStyle)
  {
    return true; // the style was changed: the rebuild is on its way
  }
  const bool spriteOn = settings.pages[widgets.pageCfgIndex].spriteOn != 0;
  if (spriteOn != widgets.builtSprite)
  {
    return true; // Sprite On was switched: the same
  }
  return false;
}

// 0.15.0: the page's spinner shows while one of its values is still waiting for its source to connect (K-line 5-baud
// init, the Bluetooth OBD adapter, VAG diag), just left of the note line that says what it's doing
static void refreshSourceSpinner(PageWidgets &widgets)
{
  if (widgets.sourceSpinner == NULL || widgets.sub == NULL)
  {
    return; // this page has no value that needs connecting
  }
  if (pageOutOfDate(widgets))
  {
    return; // 0.30.19: built as another style - wait for the rebuild
  }
  const GaugePageCfg &pageCfg = settings.pages[widgets.pageCfgIndex];
  // the page's values: main, second, and for Quad / Custom the three slots
  signalId_t ids[5];
  ids[0] = (signalId_t)pageCfg.signal;
  ids[1] = (signalId_t)pageCfg.signal2;
  ids[2] = SIG_NONE;
  ids[3] = SIG_NONE;
  ids[4] = SIG_NONE;
  if (pageCfg.style == STYLE_QUAD || pageCfg.style == STYLE_TRIPLE || pageCfg.style == STYLE_CUSTOM)
  {
    ids[2] = (signalId_t)settings.pages[widgets.pageCfgIndex].slots[0];
    ids[3] = (signalId_t)settings.pages[widgets.pageCfgIndex].slots[1];
    ids[4] = (signalId_t)settings.pages[widgets.pageCfgIndex].slots[2];
  }
  signalId_t connectingId = SIG_NONE; // the first value with no reading whose source is still connecting
  for (uint8_t i = 0; i < 5; i++)
  {
    if (connectingId == SIG_NONE && ids[i] != SIG_NONE && !signalValid(ids[i]) && userSigConnecting(ids[i]))
    {
      connectingId = ids[i];
    }
  }
  const bool isConnecting = connectingId != SIG_NONE;
  if (pageCfg.style == STYLE_QUAD || pageCfg.style == STYLE_TRIPLE)
  {
    // Quad / Triple have no note of their own on that line: say what the connecting value is waiting for
    char note[48] = "";
    if (isConnecting)
    {
      userSigNote(connectingId, note, sizeof(note));
    }
    labelSet(widgets.sub, note);
  }
  if (isConnecting)
  {
    lv_obj_align_to(widgets.sourceSpinner, widgets.sub, LV_ALIGN_OUT_LEFT_MID, -6, 0); // just left of the note text
  }
  setHidden(widgets.sourceSpinner, !isConnecting);
}

static void refreshPage(PageWidgets &widgets)
{
  const GaugePageCfg &pageCfg = settings.pages[widgets.pageCfgIndex];
  char buf[24];
  const char *unit = "";
  if (pageOutOfDate(widgets))
  {
    return; // 0.30.19: built as another style - wait for the rebuild (see pageOutOfDate)
  }
  pageClaims(widgets);

  if (settings.pages[widgets.pageCfgIndex].spriteOn) // the sprite in place of the page's style: ring, reading, the pack's mood
  {
    if (pageCfg.style == STYLE_OIL)
    {
      runtime.oilPageSeenMs = millis() | 1; // the oil page's K-line cluster session stays open
    }
    const signalId_t id = (signalId_t)pageCfg.signal;
    const bool valid = signalValid(id);
    const float smoothedValue = smoothTo(widgets.smooth[0], signalGet(id), valid);
    const float value = signalDisplayValue(id, smoothedValue, &unit);
    if (valid)
    {
      formatValue(buf, sizeof(buf), value, pageDecimals(pageCfg, id));
    }
    else
    {
      strcpy(buf, "--");
    }
    labelSet(widgets.value, buf);
    if (strcmp(lv_label_get_text(widgets.unit), unit))
    {
      labelSet(widgets.unit, unit);
    }
    // 0.27.0: Min / Max / Redline are in the shown unit, so the ring and the redline work on the shown value too
    const float shownNow = signalDisplayValue(id, signalGet(id), NULL); // unsmoothed, for the redline
    const bool red = (pageCfg.redline > widgets.drawnMin) && valid && (shownNow >= pageCfg.redline);
    lv_arc_set_value(widgets.arc, valid ? permille(value, widgets.drawnMin, widgets.drawnMax) : 0);
    arcColor(widgets.arc, red ? colour(palette.redline) : accentColor());
    textColor(widgets.value, red ? colour(palette.redline) : colour(palette.text));
    refreshSprite(widgets, pageCfg, value, valid);
    return;
  }

  if (pageCfg.style == STYLE_HALDEX)
  {
    refreshHaldex(widgets);
    return;
  }
  if (pageCfg.style == STYLE_GEAR)
  {
    refreshGear(widgets.gear);
    return;
  }
  if (pageCfg.style == STYLE_OIL)
  {
    refreshOil(widgets, pageCfg);
    return;
  }
  if (pageCfg.style == STYLE_QUAD || pageCfg.style == STYLE_TRIPLE)
  {
    int slotCount = 4; // Quad: the page signal and slots 1-3
    if (pageCfg.style == STYLE_TRIPLE)
    {
      slotCount = 3; // 0.31.0 Triple: the page signal and slots 1-2
    }
    for (int slot = 0; slot < slotCount; slot++)
    {
      signalId_t id = quadSignal(pageCfg, slot);
      const float quadValue = signalDisplayValue(id, signalGet(id), &unit);
      if (signalValid(id))
      {
        uint8_t decimals = pageDecimals(pageCfg, id); // the first value: the page's Decimals (0.36.14: never fewer than its own)
        if (slot > 0)
        {
          decimals = slotDecimals(id); // 0.31.0: values 2-4 their own (an ADC sensor's two places ...)
        }
        formatValue(buf, sizeof(buf), quadValue, decimals);
      }
      else
      {
        copyStr(buf, sizeof(buf), id == SIG_NONE ? "" : "--");
      }
      labelSet(widgets.quad[slot], buf);
      showValueIcon(widgets.valueIcon[slot], id); // 0.31.0
      if (strcmp(lv_label_get_text(widgets.quadName[slot]), id == SIG_NONE ? "" : signalName(id))) // page sources name theirs later
      {
        labelSet(widgets.quadName[slot], id == SIG_NONE ? "" : signalName(id));
      }
      if (strcmp(lv_label_get_text(widgets.quadUnit[slot]), unit))
      {
        labelSet(widgets.quadUnit[slot], unit);
      }
    }
    return;
  }

  signalId_t id = (signalId_t)pageCfg.signal;
  const bool valid = signalValid(id);
  const float smoothedValue = smoothTo(widgets.smooth[0], signalGet(id), valid); // what the needle / arc / bar and the number show
  float value = signalDisplayValue(id, smoothedValue, &unit);
  if (valid)
  {
    formatValue(buf, sizeof(buf), value, pageDecimals(pageCfg, id));
  }
  else
  {
    strcpy(buf, "--");
  }
  labelSet(widgets.value, buf);
  showValueIcon(widgets.valueIcon[0], id); // 0.31.0: Single / Dual (the other styles have none here)
  if (widgets.unit && pageCfg.style != STYLE_DUAL)
  {
    if (widgets.scaleKilo) // the scale reads 0 1 2 ...: say so under the value
    {
      labelFormat(widgets.unit, "x1000 %s", unit);
    }
    else
    {
      labelSet(widgets.unit, unit);
    }
  }
  if (widgets.sub)
  {
    char note[48] = "";
    if (!valid && !userSigNote(id, note, sizeof(note))) // a page source says what it is doing ("Waking module 0x01 ...")
    {
      copyStr(note, sizeof(note), signalAge(id) == 0xFFFFFFFF ? "no data" : "stale");
    }
    if (strcmp(lv_label_get_text(widgets.sub), note))
    {
      labelSet(widgets.sub, note);
    }
  }
  // 0.27.0: Min / Max / Redline are in the shown unit (mph, psi, F ...), so the needle, arc, bar and redline all work
  // on the shown value - before, they used the stored metric one, and a 0-175 mph dial pointed at km/h
  const float shownNow = signalDisplayValue(id, signalGet(id), NULL); // unsmoothed, for the redline
  const bool red = (pageCfg.redline > widgets.drawnMin) && valid && (shownNow >= pageCfg.redline);
  lv_color_t color = red ? colour(palette.redline) : accentColor();

  switch (pageCfg.style)
  {
  case STYLE_CUSTOM:
    refreshSlots(widgets);
    [[fallthrough]]; // the main gauge is the arc
  case STYLE_ARC:
    lv_arc_set_value(widgets.arc, valid ? permille(value, widgets.drawnMin, widgets.drawnMax) : 0);
    arcColor(widgets.arc, color);
    textColor(widgets.value, red ? colour(palette.redline) : colour(palette.text));
    break;
  case STYLE_NEEDLE:
  {
    setNeedle(widgets, pageCfg, valid ? value : widgets.drawnMin);
    bgColor(widgets.needle, red ? colour(palette.redline) : needleColor(), LV_PART_MAIN);
    break;
  }
  case STYLE_BAR:
    lv_bar_set_value(widgets.bar, valid ? permille(value, widgets.drawnMin, widgets.drawnMax) : 0, LV_ANIM_OFF);
    bgColor(widgets.bar, color, LV_PART_INDICATOR);
    break;
  case STYLE_NUMERIC:
    textColor(widgets.value, red ? colour(palette.redline) : colour(palette.text));
    break;
  case STYLE_GRAPH: // 0.31.0: the line itself is drawn by sampleGraphs; here just the value turns red over the redline
    textColor(widgets.value, red ? colour(palette.redline) : colour(palette.text));
    break;
  case STYLE_DUAL:
  {
    lv_bar_set_value(widgets.bar, valid ? permille(value, widgets.drawnMin, widgets.drawnMax) : 0, LV_ANIM_OFF); // shown unit (0.27.0)
    signalId_t id2 = (signalId_t)pageCfg.signal2; // its bar keeps the catalogue's stored-unit range (defMin / defMax)
    const SignalInfo &info2 = signalInfo(id2);
    showValueIcon(widgets.valueIcon[1], id2); // 0.31.0
    if (strcmp(lv_label_get_text(widgets.unit), signalName(id2))) // a page source names it once connected (K-line label file)
    {
      labelSet(widgets.unit, signalName(id2));
    }
    const char *unit2;
    const float smoothedValue2 = smoothTo(widgets.smooth[1], signalGet(id2), signalValid(id2));
    float value2 = signalDisplayValue(id2, smoothedValue2, &unit2);
    if (signalValid(id2))
    {
      formatValue(buf, sizeof(buf), value2, slotDecimals(id2)); // 0.31.0: the second value's own decimals
      labelFormat(widgets.value2, "%s %s", buf, unit2);
      lv_bar_set_value(widgets.bar2, permille(smoothedValue2, info2.defMin, info2.defMax), LV_ANIM_OFF);
    }
    else
    {
      labelSet(widgets.value2, "--");
      lv_bar_set_value(widgets.bar2, 0, LV_ANIM_OFF);
    }
    break;
  }
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// menu / info / settings screens
// ---------------------------------------------------------------------------
#define MENU_SETTINGS 100

static void dsgScreenCb(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED)
  {
    uiOpenMenu(); // tap anywhere: back to the menu
    lv_indev_wait_release(lv_indev_active()); // a long press's release must not click the menu too
  }
}

// caller holds the LVGL lock
static void showDsg()
{
  if (!dsgScr)
  {
    dsgScr = newScreen();
    buildGear(dsgScr, dsgWidgets, "GEARBOX"); // 0.26.0: was "DSG" (the 8HP uses this screen too)
    lv_obj_add_event_cb(dsgScr, pageEventCb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(dsgScr, dsgScreenCb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(dsgScr, dsgScreenCb, LV_EVENT_LONG_PRESSED, NULL);
  }
  refreshGear(dsgWidgets);
  showScreen(dsgScr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

// 0.38.0: menu > Drag Timer, built the first time (DragTimer.cpp) in the theme's colours. Caller holds the LVGL lock.
static void showDrag()
{
  if (!dragScr)
  {
    DragColours colours;
    colours.bg = palette.bg;
    colours.panel = palette.panel;
    colours.text = palette.text;
    colours.muted = palette.muted;
    colours.accent = palette.accent;
    colours.accent2 = palette.accent2;
    colours.ok = palette.ok;
    colours.danger = palette.danger;
    dragScr = dragOpen(colours);
  }
  showScreen(dragScr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

void uiOpenDsg()
{
  if (!lvglLock(50))
  {
    return;
  }
  menuOpen = true;
  showDsg();
  lvglUnlock();
}

static void showModeScreen(uint8_t item); // mode screens, below the menu
static void showHaldexMode();

// open a menu item: Haldex has its own screen, Gearbox and the drag timer theirs, the rest are mode screens the firmware
// fills (Display.cpp modeView), and Settings
static void openMenuItem(int item)
{
  if (item == MENU_HALDEX)
  {
    showHaldexMode();
    return;
  }
  if (item == MENU_CAN || item == MENU_KLINE || item == MENU_BLE || item == MENU_OUTPUT || item == MENU_CHAIN ||
      item == MENU_ADC || item == MENU_NAV || item == MENU_ECU || item == MENU_OIL || item == MENU_TRIGGERS)
  {
    showModeScreen((uint8_t)item);
    return;
  }
  if (item == MENU_GAUGES)
  {
    menuOpen = false;
    showScreen(pages[activePage].scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    return;
  }
  // menu item -> info page (DSG has its own screen)
  static const int8_t infoOf[MENU_ITEM_COUNT] = {UI_INFO_HALDEX, UI_INFO_CAN, UI_INFO_KLINE, UI_INFO_ADC, -1, UI_INFO_BLE, UI_INFO_CHAIN};
  if (item == MENU_DSG)
  {
    showDsg();
    return;
  }
  if (item == MENU_DRAG)
  {
    showDrag(); // 0.38.0
    return;
  }
  int infoIndex = item < MENU_ITEM_COUNT ? infoOf[item] : item;
  if (infoIndex >= 0 && infoIndex < UI_INFO_COUNT)
  {
    infoPage = (uiInfoPage_t)infoIndex;
    if (!infoScr)
    {
      infoScr = newScreen();
      lv_obj_t *title = glassLabel(infoScr, &lv_font_montserrat_24, palette.accent, LV_ALIGN_TOP_MID, 0, 40, "");
      lv_obj_set_user_data(infoScr, title);
      infoLabel = lv_label_create(infoScr);
      lv_obj_set_style_text_font(infoLabel, &lv_font_montserrat_18, 0);
      lv_obj_set_style_text_color(infoLabel, colour(palette.text), 0);
      lv_obj_set_style_text_align(infoLabel, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_width(infoLabel, 400);
      lv_label_set_long_mode(infoLabel, LV_LABEL_LONG_WRAP);
      lv_obj_align(infoLabel, LV_ALIGN_CENTER, 0, 10);
      lv_obj_t *back = lv_button_create(infoScr);
      lv_obj_set_size(back, 160, 44);
      lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -30);
      lv_obj_set_style_bg_color(back, colour(palette.panel), 0);
      lv_obj_add_event_cb(back, [](lv_event_t *)
                          { uiOpenMenu(); }, LV_EVENT_CLICKED, NULL);
      lv_obj_t *backLabel = lv_label_create(back);
      lv_label_set_text(backLabel, LV_SYMBOL_LEFT "  Menu");
      lv_obj_center(backLabel);
      lv_obj_add_event_cb(infoScr, pageEventCb, LV_EVENT_GESTURE, NULL);
    }
    static const char *titles[UI_INFO_COUNT] = {"HALDEX", "CAN BUS", "K-LINE", "OUTPUT / LIGHTS", "BLUETOOTH OBD", "GAUGE CHAIN"};
    lv_label_set_text((lv_obj_t *)lv_obj_get_user_data(infoScr), titles[infoIndex]);
    lv_label_set_text(infoLabel, "...");
    showScreen(infoScr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
  }
  else if (item == MENU_SETTINGS)
  {
    showScreen(settingsScr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
  }
}

// ---------------------------------------------------------------------------
// mode menu (docs/ui-concept/can2gauge_ui_concept.html "Mode Menu"): one item per screen - the big two-colour icon,
// the name, a line of text and a live status line, with arrows and dots. Swipe left / right steps through the items
// (wraps), tap opens the item, long press goes back to the gauge, and so do 10 s without a touch. Items switched off in
// the web UI (settings.menuMask) are skipped; Settings is always there.
// ---------------------------------------------------------------------------
#define MENU_IDLE_MS 10000
struct MenuItemDef
{
  uint8_t id;   // uiMenuItem_t or MENU_SETTINGS
  uint8_t icon; // menuIcon_t
  const char *title;
  const char *desc;
};
// in the order of the web UI's Settings > Device > Menu and Tabs switches (data/app.js menuItems), then Settings
static const MenuItemDef menuDefs[] = {
    {MENU_GAUGES, MI_GAUGES, "Gauges", "Your gauge pages."},
    {MENU_NAV, MI_NAV, "Navigation", "The phone's next turn: Off, Pop Up or the whole Route."},
    {MENU_CAN, MI_CAN, "CAN Bus", "Every message on the bus, decoded with the car's DBC."},
    {MENU_HALDEX, MI_HALDEX, "Haldex", "OpenHaldex lock, target and mode."},
    {MENU_ADC, MI_ADC, "ADC", "The 0-5 V sensors on the adc2can expander."},
    {MENU_ECU, MI_ECU, "usb2ip", "The aftermarket ECU's live values, over WiFi."},
    {MENU_KLINE, MI_KLINE, "K-Line", "Measuring blocks from the module picked in the web UI."},
    {MENU_OIL, MI_OIL, "Oil", "The cluster's oil temperature and level."},
    {MENU_DSG, MI_DSG, "Gearbox", "Selector, gear, speed and lock-up, full screen."},
    {MENU_BLE, MI_BLE, "Bluetooth OBD", "Live PIDs through the paired ELM327 adapter."},
    {MENU_TRIGGERS, MI_TRIGGERS, "Triggers", "Your alert and output limits, and which have fired."},
    {MENU_CHAIN, MI_CHAIN, "Daisy Chain", "The gauges linked together, who leads and what they share."},
    {MENU_OUTPUT, MI_OUTPUT, "Output", "The output, the illumination input and the brightness."}, // short: fits the glass
    {MENU_DRAG, MI_DRAG, "Drag Timer", "0-100 km/h (0-60 mph) from a countdown, with a scoreboard."},
    {MENU_SETTINGS, MI_SETTINGS, "Settings", "Brightness, units, theme, 120 ohm. The rest is in the web UI."},
};
#define MENU_DEF_COUNT (sizeof(menuDefs) / sizeof(menuDefs[0]))
static uint8_t menuVisible[MENU_DEF_COUNT]; // visible items (indices into menuDefs)
static uint8_t menuVisibleCount = 0;
static uint8_t menuIndex = 0;             // the item showing, index into menuVisible
static lv_obj_t *menuBox = NULL;        // the item's content (slides on a swipe)
#define MENU_SLIDE_MS 250 // 0.24.26: the menu's content slide - quick, and no longer the page slide's time (now 800 ms)
static lv_obj_t *menuStatusRow = NULL;
static lv_obj_t *menuStatusDot = NULL;
static lv_obj_t *menuStatusLabel = NULL;
static char menuStatusLast[48] = "";
static uint32_t menuTouchMs = 0;
static lv_obj_t *menuFrom = NULL; // the screen the menu was opened from (idle / long press go back to it)
static bool menuClosePending = false; // the 10 s idle close is waiting for the LVGL task (menuIdleClose)
static uiMenuStatus_t menuStatusProvider = NULL;

void uiSetMenuStatus(uiMenuStatus_t provider)
{
  menuStatusProvider = provider;
}

static lv_color_t lineColor()
{
  return lv_color_lighten(colour(palette.panel), 20);
} // the concept's track grey

static lv_color_t roleColor(uint8_t role)
{
  switch (role)
  {
  case MIR_ACC2: return colour(palette.accent2);
  case MIR_MUTED: return colour(palette.muted);
  case MIR_LINE: return lineColor();
  default: return accentColor();
  }
}

static lv_obj_t *plainObject(lv_obj_t *parent)
{
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE); // touches go to the screen (swipe / tap / long press)
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static void menuStatusRefresh()
{
  if (!menuStatusLabel || !menuVisibleCount)
  {
    return;
  }
  char buf[48] = "";
  int8_t ok = -1;
  if (menuStatusProvider)
  {
    ok = menuStatusProvider(menuDefs[menuVisible[menuIndex]].id, buf, sizeof(buf));
  }
  char key[52];
  snprintf(key, sizeof(key), "%d%s", ok, buf);
  if (!strcmp(key, menuStatusLast))
  {
    return;
  }
  copyStr(menuStatusLast, sizeof(menuStatusLast), key);
  labelSet(menuStatusLabel, buf);
  textColor(menuStatusLabel, ok == 1 ? colour(palette.text) : colour(palette.muted));
  if (ok < 0)
  {
    lv_obj_add_flag(menuStatusDot, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    lv_obj_remove_flag(menuStatusDot, LV_OBJ_FLAG_HIDDEN);
    bgColor(menuStatusDot, ok ? colour(palette.ok) : colour(palette.muted), LV_PART_MAIN);
  }
}

static void menuSlideCb(void *obj, int32_t value) // value 0..256: slide in from the swipe side and fade in
{
  const int32_t direction = (int32_t)(intptr_t)lv_obj_get_user_data((lv_obj_t *)obj);
  lv_obj_set_style_translate_x((lv_obj_t *)obj, direction * (256 - value) * 60 / 256, 0);
  lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)(value < 256 ? value : 255), 0);
}

// draws the item at menuIndex into menuBox; direction = +1 / -1 slides it in from the right / left, 0 = no animation
static void menuFill(int direction)
{
  if (!menuBox || !menuVisibleCount)
  {
    return;
  }
  if (menuIndex >= menuVisibleCount)
  {
    menuIndex = 0;
  }
  const MenuItemDef &item = menuDefs[menuVisible[menuIndex]];
  lv_obj_clean(menuBox);
  const MenuIcon &icon = menuIcon(item.icon); // layers back to front, centred on (240, 168) like the concept
  for (uint8_t layer = 0; layer < icon.count; layer++)
  {
    lv_obj_t *img = lv_image_create(menuBox);
    lv_image_set_src(img, icon.layers[layer].img);
    lv_obj_set_style_image_recolor(img, roleColor(icon.layers[layer].role), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_align(img, LV_ALIGN_CENTER, icon.layers[layer].dx, -72 + icon.layers[layer].dy);
  }
  glassLabel(menuBox, &lv_font_montserrat_40, palette.text, LV_ALIGN_CENTER, 0, 50, item.title);
  lv_obj_t *descLabel = label(menuBox, &lv_font_montserrat_16, palette.muted, LV_ALIGN_TOP_MID, 0, 324, item.desc);
  lv_obj_set_width(descLabel, 320);
  lv_label_set_long_mode(descLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(descLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(descLabel, 6, 0);
  lv_obj_align(descLabel, LV_ALIGN_TOP_MID, 0, 324);
  // status row: dot + text, centred at y 400
  menuStatusRow = plainObject(menuBox);
  lv_obj_set_size(menuStatusRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(menuStatusRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(menuStatusRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(menuStatusRow, 8, 0);
  lv_obj_align(menuStatusRow, LV_ALIGN_CENTER, 0, 160);
  menuStatusDot = plainObject(menuStatusRow);
  lv_obj_set_size(menuStatusDot, 10, 10);
  lv_obj_set_style_radius(menuStatusDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(menuStatusDot, LV_OPA_COVER, 0);
  menuStatusLabel = lv_label_create(menuStatusRow);
  lv_obj_set_style_text_font(menuStatusLabel, &lv_font_montserrat_16, 0);
  // 0.30.2: one line inside the glass at this height, less the dot and its gap ("not connected, retry 30s" ran off it)
  fitOneLine(menuStatusLabel, glassWidthAt(160, lv_font_get_line_height(&lv_font_montserrat_16)) - 18);
  menuStatusLast[0] = '\0';
  menuStatusRefresh();
  // dots: one per item, the current one larger and in the accent colour
  const int32_t w = (menuVisibleCount - 1) * 16;
  for (uint8_t dotIndex = 0; dotIndex < menuVisibleCount; dotIndex++)
  {
    lv_obj_t *dot = plainObject(menuBox);
    const int32_t size = dotIndex == menuIndex ? 10 : 7;
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, dotIndex == menuIndex ? accentColor() : lineColor(), 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, -w / 2 + dotIndex * 16, 206);
  }
  if (direction)
  {
    lv_obj_set_user_data(menuBox, (void *)(intptr_t)direction);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, menuBox);
    lv_anim_set_values(&anim, 0, 256);
    lv_anim_set_duration(&anim, MENU_SLIDE_MS);
    lv_anim_set_exec_cb(&anim, menuSlideCb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
  }
  else
  {
    lv_obj_set_style_translate_x(menuBox, 0, 0);
    lv_obj_set_style_opa(menuBox, LV_OPA_COVER, 0);
  }
}

static void menuStep(int direction)
{
  if (!menuVisibleCount)
  {
    return;
  }
  menuIndex = (menuIndex + menuVisibleCount + direction) % menuVisibleCount;
  menuFill(direction);
}

static void menuEventCb(lv_event_t *event)
{
  menuTouchMs = lv_tick_get(); // any touch keeps the menu up
  switch (lv_event_get_code(event))
  {
  case LV_EVENT_GESTURE:
  {
    const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    if (direction == LV_DIR_LEFT)
    {
      menuStep(1); // swipe left: the next item comes in from the right
    }
    else if (direction == LV_DIR_RIGHT)
    {
      menuStep(-1);
    }
    lv_indev_wait_release(lv_indev_active()); // the release must not also count as a tap
    break;
  }
  case LV_EVENT_SHORT_CLICKED:
    if (menuVisibleCount)
    {
      openMenuItem(menuDefs[menuVisible[menuIndex]].id);
    }
    break;
  case LV_EVENT_LONG_PRESSED:
    uiCloseMenu();
    break;
  default:
    break;
  }
}

// The menu's 10 s idle close, run by the LVGL task (uiTick asks for it with lv_async_call, LVGL's "run this function on
// your next pass"). Going back slides in the screen the menu came from, drawn whole in the task doing the slide. The
// LVGL task has the stack for that (10 KB); uiTick (6 KB) hasn't: a menu screen (rows of text, five layers deep)
// overflowed it (coredump 2026-11-03, 0.39.2, after the menu timed out back to a menu screen).
static void menuIdleClose(void *unused)
{
  (void)unused;
  menuClosePending = false;
  if (lv_screen_active() == menuScr && lv_tick_elaps(menuTouchMs) > MENU_IDLE_MS) // still idle, still on the menu
  {
    uiCloseMenu();
  }
}

static void buildMenu()
{
  menuVisibleCount = 0;
  for (uint8_t defIndex = 0; defIndex < MENU_DEF_COUNT; defIndex++)
  {
    if (menuDefs[defIndex].id >= MENU_ITEM_COUNT || ((settings.menuMask >> menuDefs[defIndex].id) & 1))
    {
      menuVisible[menuVisibleCount++] = defIndex;
    }
  }
  if (menuIndex >= menuVisibleCount)
  {
    menuIndex = 0;
  }
  menuScr = newScreen();
  lv_obj_t *ring = plainObject(menuScr); // thin ring at r 233
  lv_obj_set_size(ring, 468, 468);
  lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, 2, 0);
  lv_obj_set_style_border_color(ring, lineColor(), 0);

  for (int side = -1; side <= 1; side += 2) // swipe hints
  {
    lv_obj_t *arrow = label(menuScr, &lv_font_montserrat_28, palette.muted, LV_ALIGN_CENTER, side * 206, -4,
                            side < 0 ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
    lv_obj_set_style_opa(arrow, LV_OPA_50, 0);
  }

  menuBox = plainObject(menuScr);
  lv_obj_set_size(menuBox, 480, 480);
  lv_obj_center(menuBox);
  menuFill(0);
  lv_obj_add_event_cb(menuScr, menuEventCb, LV_EVENT_GESTURE, NULL);
  lv_obj_add_event_cb(menuScr, menuEventCb, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(menuScr, menuEventCb, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(menuScr, menuEventCb, LV_EVENT_PRESSED, NULL);
}

void uiRebuildMenu()
{
  if (!lvglLock(500))
  {
    return;
  }
  lvglFinishScreenAnimation(); // LVGL mustn't be sliding the menu that is about to go
  lv_obj_t *oldMenu = menuScr;
  buildMenu();
  if (lv_screen_active() == oldMenu)
  {
    lv_screen_load(menuScr);
  }
  if (oldMenu)
  {
    lv_obj_delete_async(oldMenu);
  }
  lvglUnlock();
}

// ---------------------------------------------------------------------------
// mode screens (menu > CAN / K-line / Bluetooth OBD): title, a status line with a dot or a spinner (connecting), a big
// header (block / CAN ID / page), a line under it, then up to UI_MODE_ROWS name / value rows or the concept's eight-byte
// grid (a changed byte lights up). The firmware's provider fills it; a swipe asks the firmware for the next / previous
// item and slides the content in from that side. Long press or swipe up / down: the menu.
// ---------------------------------------------------------------------------
#define MODE_REFRESH_MS 100
#define MODE_HOT_MS 700    // a changed byte stays lit this long
#define MODE_CHOICE_W 104  // an option button (menu > Navigation: Off / Pop Up / Route), wide ...
#define MODE_CHOICE_H 44   // ... and high
#define MODE_CHOICE_GAP 10 // ... and the gap between two
struct ModeWidgets
{
  lv_obj_t *scr, *title, *dot, *spinner, *status, *body, *header, *sub, *rows, *grid, *hint;
  lv_obj_t *row[UI_MODE_ROWS], *rowName[UI_MODE_ROWS], *rowValue[UI_MODE_ROWS];
  lv_obj_t *cell[8], *cellHex[8], *cellDec[8];
  lv_obj_t *nodeRow, *nodeLine, *node[UI_MODE_NODES], *nodeLabel[UI_MODE_NODES];
  char nodeKey[64];
  lv_obj_t *choiceRow;                    // the option buttons, in place of the header
  lv_obj_t *choice[UI_MODE_CHOICES];      // ... each button
  lv_obj_t *choiceLabel[UI_MODE_CHOICES]; // ... and its words
  char choiceKey[48];                     // the buttons as last drawn (the lit one + the names): redrawn only on a change
  uint8_t lastData[8];
  uint32_t hotMs[8];
  bool lit[8];
  int8_t state, tone;
};
static ModeWidgets modeWidgets = {};
static uint8_t modeItem = 0;
static uiModeProvider_t modeProvider = NULL;
static uint32_t modeRefreshMs = 0;

void uiSetModeProvider(uiModeProvider_t provider)
{
  modeProvider = provider;
}

static void setText(lv_obj_t *label, const char *text) // touches the label only when the text changed (no needless redraw)
{
  if (strcmp(lv_label_get_text(label), text))
  {
    lv_label_set_text(label, text);
  }
}

static void slideIn(lv_obj_t *obj, int direction) // the menu's slide: in from the swipe side, fading in
{
  lv_obj_set_user_data(obj, (void *)(intptr_t)direction);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, obj);
  lv_anim_set_values(&anim, 0, 256);
  lv_anim_set_duration(&anim, MENU_SLIDE_MS);
  lv_anim_set_exec_cb(&anim, menuSlideCb);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
  lv_anim_start(&anim);
}

static void modeRefresh(bool force)
{
  if (!modeWidgets.scr || (!force && lv_tick_elaps(modeRefreshMs) < MODE_REFRESH_MS))
  {
    return;
  }
  modeRefreshMs = lv_tick_get();
  EXT_RAM_BSS_ATTR static UiModeView view;
  memset(&view, 0, sizeof(view));
  view.state = UI_MS_NONE;
  if (modeProvider)
  {
    modeProvider(modeItem, view);
  }
  else
  {
    copyStr(view.status, sizeof(view.status), "no data source registered");
  }
  setText(modeWidgets.title, view.title);
  setText(modeWidgets.status, view.status);
  setText(modeWidgets.header, view.header);
  setText(modeWidgets.sub, view.sub);
  setText(modeWidgets.hint, view.hint);
  if (view.state != modeWidgets.state)
  {
    modeWidgets.state = view.state;
    setHidden(modeWidgets.spinner, view.state != UI_MS_BUSY);
    setHidden(modeWidgets.dot, view.state != UI_MS_OK && view.state != UI_MS_BAD && view.state != UI_MS_ERR);
    lv_color_t dotColor;
    if (view.state == UI_MS_OK)
    {
      dotColor = colour(palette.ok);
    }
    else if (view.state == UI_MS_ERR)
    {
      dotColor = colour(palette.danger);
    }
    else
    {
      dotColor = colour(palette.muted);
    }
    bgColor(modeWidgets.dot, dotColor, LV_PART_MAIN);
    textColor(modeWidgets.status,
              view.state == UI_MS_BAD || view.state == UI_MS_BUSY ? colour(palette.muted) : colour(palette.text));
  }
  if (view.headerTone != modeWidgets.tone)
  {
    modeWidgets.tone = view.headerTone;
    lv_color_t headerColor;
    if (view.headerTone == 1)
    {
      headerColor = colour(palette.ok);
    }
    else if (view.headerTone == 2)
    {
      headerColor = colour(palette.danger);
    }
    else
    {
      headerColor = colour(palette.text);
    }
    textColor(modeWidgets.header, headerColor);
  }

  setHidden(modeWidgets.rows, view.bytes > 0);
  setHidden(modeWidgets.grid, view.bytes == 0);
  setHidden(modeWidgets.header, view.nodes > 0 || view.choices > 0);
  setHidden(modeWidgets.nodeRow, view.nodes == 0);
  setHidden(modeWidgets.choiceRow, view.choices == 0);
  if (view.choices) // Navigation: redraw the buttons only when they change
  {
    uint8_t choiceCount = view.choices;
    if (choiceCount > UI_MODE_CHOICES)
    {
      choiceCount = UI_MODE_CHOICES;
    }
    char key[48] = "";
    size_t keyLength = 0;
    appendf(key, sizeof(key), keyLength, "%d", view.choiceOn);
    for (uint8_t i = 0; i < choiceCount; i++)
    {
      appendf(key, sizeof(key), keyLength, ",%s", view.choiceName[i]);
    }
    if (strcmp(key, modeWidgets.choiceKey))
    {
      copyStr(modeWidgets.choiceKey, sizeof(modeWidgets.choiceKey), key);
      // centred as a group: fewer buttons start further in
      const int32_t firstX = (UI_MODE_CHOICES - choiceCount) * (MODE_CHOICE_W + MODE_CHOICE_GAP) / 2;
      for (uint8_t i = 0; i < UI_MODE_CHOICES; i++)
      {
        setHidden(modeWidgets.choice[i], i >= choiceCount);
        if (i >= choiceCount)
        {
          continue;
        }
        lv_obj_set_pos(modeWidgets.choice[i], firstX + i * (MODE_CHOICE_W + MODE_CHOICE_GAP), 0);
        // the one in use is filled in the accent colour, its words in the background colour; the others are outlined
        if (i == view.choiceOn)
        {
          bgColor(modeWidgets.choice[i], accentColor(), LV_PART_MAIN);
          lv_obj_set_style_border_color(modeWidgets.choice[i], accentColor(), 0);
          textColor(modeWidgets.choiceLabel[i], colour(palette.bg));
        }
        else
        {
          bgColor(modeWidgets.choice[i], colour(palette.panel), LV_PART_MAIN);
          lv_obj_set_style_border_color(modeWidgets.choice[i], lineColor(), 0);
          textColor(modeWidgets.choiceLabel[i], colour(palette.text));
        }
        labelSet(modeWidgets.choiceLabel[i], view.choiceName[i]);
      }
    }
  }
  if (view.nodes) // Daisy Chain: redraw the circles only when the chain changes
  {
    char key[64] = "";
    size_t keyLen = 0;
    for (uint8_t i = 0; i < view.nodes && i < UI_MODE_NODES; i++)
    {
      keyLen += snprintf(key + keyLen, sizeof(key) - keyLen, "%s%u,", view.nodeName[i], view.nodeFlags[i]);
    }
    if (strcmp(key, modeWidgets.nodeKey))
    {
      copyStr(modeWidgets.nodeKey, sizeof(modeWidgets.nodeKey), key);
      const uint8_t nodeCount = view.nodes < UI_MODE_NODES ? view.nodes : UI_MODE_NODES;
      const int32_t pitch = 62;
      const int32_t x0 = (6 * 62 - (nodeCount - 1) * pitch) / 2 - 25;
      lv_obj_set_width(modeWidgets.nodeLine, (nodeCount - 1) * pitch);
      lv_obj_set_pos(modeWidgets.nodeLine, x0 + 25, 26);
      setHidden(modeWidgets.nodeLine, nodeCount < 2);
      for (uint8_t i = 0; i < UI_MODE_NODES; i++)
      {
        setHidden(modeWidgets.node[i], i >= nodeCount);
        if (i >= nodeCount)
        {
          continue;
        }
        const uint8_t flags = view.nodeFlags[i];
        lv_obj_set_pos(modeWidgets.node[i], x0 + i * pitch, 3);
        bgColor(modeWidgets.node[i], (flags & UI_NODE_SELF) ? accentColor() : colour(palette.panel), LV_PART_MAIN);
        lv_color_t borderColor;
        if (flags & UI_NODE_MASTER)
        {
          borderColor = colour(palette.accent2);
        }
        else if (flags & UI_NODE_HEARD)
        {
          borderColor = accentColor();
        }
        else
        {
          borderColor = lineColor();
        }
        lv_obj_set_style_border_color(modeWidgets.node[i], borderColor, 0);
        lv_color_t nameColor;
        if (flags & UI_NODE_SELF)
        {
          nameColor = colour(palette.bg);
        }
        else if (flags & UI_NODE_HEARD)
        {
          nameColor = colour(palette.text);
        }
        else
        {
          nameColor = colour(palette.muted);
        }
        textColor(modeWidgets.nodeLabel[i], nameColor);
        labelSet(modeWidgets.nodeLabel[i], view.nodeName[i]);
      }
    }
  }

  for (uint8_t i = 0; i < UI_MODE_ROWS; i++)
  {
    const bool on = i < view.rows && !view.bytes;
    setHidden(modeWidgets.row[i], !on);
    if (on)
    {
      setText(modeWidgets.rowName[i], view.rowName[i]);
      setText(modeWidgets.rowValue[i], view.rowValue[i]);
    }
  }

  if (!view.bytes)
  {
    return;
  }
  const uint32_t now = lv_tick_get();
  char text[8];
  for (uint8_t i = 0; i < 8; i++)
  {
    const bool on = i < view.bytes;
    if (on && view.data[i] != modeWidgets.lastData[i])
    {
      modeWidgets.lastData[i] = view.data[i];
      modeWidgets.hotMs[i] = now ? now : 1;
    }
    snprintf(text, sizeof(text), on ? "%02X" : "--", view.data[i]);
    setText(modeWidgets.cellHex[i], text);
    snprintf(text, sizeof(text), on ? "%u" : "", view.data[i]);
    setText(modeWidgets.cellDec[i], text);
    const bool hot = on && modeWidgets.hotMs[i] && now - modeWidgets.hotMs[i] < MODE_HOT_MS;
    if (hot != modeWidgets.lit[i])
    {
      modeWidgets.lit[i] = hot;
      lv_obj_set_style_border_color(modeWidgets.cell[i], hot ? accentColor() : lineColor(), 0);
      textColor(modeWidgets.cellHex[i], hot ? accentColor() : colour(palette.text));
    }
  }
}

static void modeEventCb(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_LONG_PRESSED)
  {
    uiOpenMenu();
    return;
  }
  if (code != LV_EVENT_GESTURE)
  {
    return;
  }
  const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
  if (direction == LV_DIR_TOP || direction == LV_DIR_BOTTOM)
  {
    uiOpenMenu();
    return;
  }
  int8_t step = 0;
  if (direction == LV_DIR_LEFT)
  {
    step = 1;
  }
  else if (direction == LV_DIR_RIGHT)
  {
    step = -1;
  }
  if (!step)
  {
    return;
  }
  lv_indev_wait_release(lv_indev_active()); // the release mustn't also tap an option button the swipe started on
  if (callbacks.onModeSwipe)
  {
    callbacks.onModeSwipe(modeItem, step);
  }
  memset(modeWidgets.hotMs, 0, sizeof(modeWidgets.hotMs)); // a new message: no "changed" flashes from the old one
  slideIn(modeWidgets.body, step);
  modeRefresh(true);
}

// an option button tapped (menu > Navigation): the firmware takes it, and the screen shows it straight away
static void modeChoiceCb(lv_event_t *event)
{
  // the button's number, handed to the event when the button was made (a number carried in the "pointer" slot)
  const uint8_t choiceIndex = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
  if (callbacks.onModeChoice)
  {
    callbacks.onModeChoice(modeItem, choiceIndex);
  }
  modeRefresh(true);
}

static void buildModeScreen()
{
  memset(&modeWidgets, 0, sizeof(modeWidgets));
  modeWidgets.state = -2; // forces the first status styling
  modeWidgets.tone = -1;
  modeWidgets.scr = newScreen();
  modeWidgets.title = glassLabel(modeWidgets.scr, &lv_font_montserrat_20, palette.accent, LV_ALIGN_TOP_MID, 0, 40, "");
  lv_obj_set_style_text_letter_space(modeWidgets.title, 2, 0);
  // status: spinner or dot + text, centred as one
  lv_obj_t *statusRow = plainObject(modeWidgets.scr);
  lv_obj_set_size(statusRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(statusRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(statusRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(statusRow, 8, 0);
  lv_obj_align(statusRow, LV_ALIGN_CENTER, 0, -150);
  modeWidgets.spinner = lv_spinner_create(statusRow);
  lv_obj_set_size(modeWidgets.spinner, 18, 18);
  lv_obj_remove_flag(modeWidgets.spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(modeWidgets.spinner, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(modeWidgets.spinner, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(modeWidgets.spinner, lineColor(), LV_PART_MAIN);
  lv_obj_set_style_arc_color(modeWidgets.spinner, accentColor(), LV_PART_INDICATOR);
  modeWidgets.dot = plainObject(statusRow);
  lv_obj_set_size(modeWidgets.dot, 10, 10);
  lv_obj_set_style_radius(modeWidgets.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(modeWidgets.dot, LV_OPA_COVER, 0);
  modeWidgets.status = lv_label_create(statusRow);
  lv_obj_set_style_text_font(modeWidgets.status, &lv_font_montserrat_16, 0);
  // up to two lines; 0.30.2: as wide as the glass is there, less the spinner and its gap (was 330, a little too wide)
  lv_obj_set_style_max_width(modeWidgets.status, glassWidthAt(-150, 2 * lv_font_get_line_height(&lv_font_montserrat_16)) - 26, 0);
  lv_obj_set_style_text_align(modeWidgets.status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(modeWidgets.status, "");
  // the content that slides on a swipe
  modeWidgets.body = plainObject(modeWidgets.scr);
  lv_obj_set_size(modeWidgets.body, 480, 480);
  lv_obj_center(modeWidgets.body);
  modeWidgets.header = glassLabel(modeWidgets.body, &lv_font_montserrat_40, palette.text, LV_ALIGN_CENTER, 0, -96, "");
  modeWidgets.sub = glassLabel(modeWidgets.body, &lv_font_montserrat_16, palette.accent2, LV_ALIGN_CENTER, 0, -56, "");
  lv_obj_set_width(modeWidgets.sub, 340);
  lv_label_set_long_mode(modeWidgets.sub, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(modeWidgets.sub, LV_TEXT_ALIGN_CENTER, 0);
  // rows: name left, value right, a faint rule under each
  modeWidgets.rows = plainObject(modeWidgets.body);
  lv_obj_set_size(modeWidgets.rows, 340, UI_MODE_ROWS * 38);
  lv_obj_align(modeWidgets.rows, LV_ALIGN_CENTER, 0, -33 + UI_MODE_ROWS * 19);
  for (uint8_t rowIndex = 0; rowIndex < UI_MODE_ROWS; rowIndex++)
  {
    lv_obj_t *row = plainObject(modeWidgets.rows);
    modeWidgets.row[rowIndex] = row;
    lv_obj_set_size(row, 340, 36);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, rowIndex * 38);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lineColor(), 0);
    modeWidgets.rowName[rowIndex] = label(row, &lv_font_montserrat_18, palette.muted, LV_ALIGN_LEFT_MID, 4, 0, "");
    lv_obj_set_width(modeWidgets.rowName[rowIndex], 210); // label names ("Intake Air Temperature") get the room; values are short
    lv_label_set_long_mode(modeWidgets.rowName[rowIndex], LV_LABEL_LONG_DOT);
    modeWidgets.rowValue[rowIndex] = label(row, &lv_font_montserrat_24, palette.text, LV_ALIGN_RIGHT_MID, -4, 0, "");
    fitOneLine(modeWidgets.rowValue[rowIndex], 340 - 210 - 12); // 0.30.2: the room the name leaves in the row
  }
  // byte grid: 4 x 2 cells (B0..B7: hex big, decimal small)
  modeWidgets.grid = plainObject(modeWidgets.body);
  lv_obj_set_size(modeWidgets.grid, 4 * 80 + 3 * 8, 2 * 80 + 8);
  lv_obj_align(modeWidgets.grid, LV_ALIGN_CENTER, 0, 60);
  for (uint8_t byteIndex = 0; byteIndex < 8; byteIndex++)
  {
    lv_obj_t *cell = plainObject(modeWidgets.grid);
    modeWidgets.cell[byteIndex] = cell;
    lv_obj_set_size(cell, 80, 80);
    lv_obj_set_pos(cell, (byteIndex % 4) * 88, (byteIndex / 4) * 88);
    lv_obj_set_style_radius(cell, 12, 0);
    lv_obj_set_style_bg_color(cell, colour(palette.panel), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cell, 2, 0);
    lv_obj_set_style_border_color(cell, lineColor(), 0);
    char byteText[4];
    snprintf(byteText, sizeof(byteText), "B%u", byteIndex);
    label(cell, &lv_font_montserrat_12, palette.muted, LV_ALIGN_TOP_LEFT, 8, 5, byteText);
    modeWidgets.cellHex[byteIndex] = label(cell, &lv_font_montserrat_28, palette.text, LV_ALIGN_CENTER, 0, 0, "--");
    modeWidgets.cellDec[byteIndex] = label(cell, &lv_font_montserrat_12, palette.muted, LV_ALIGN_BOTTOM_MID, 0, -5, "");
  }
  lv_obj_add_flag(modeWidgets.grid, LV_OBJ_FLAG_HIDDEN);
  // Daisy Chain: node circles joined by a line, in place of the header
  modeWidgets.nodeRow = plainObject(modeWidgets.body);
  lv_obj_set_size(modeWidgets.nodeRow, 6 * 62, 56);
  lv_obj_align(modeWidgets.nodeRow, LV_ALIGN_CENTER, 0, -96);
  modeWidgets.nodeLine = plainObject(modeWidgets.nodeRow);
  lv_obj_set_height(modeWidgets.nodeLine, 3);
  lv_obj_set_style_bg_color(modeWidgets.nodeLine, lineColor(), 0);
  lv_obj_set_style_bg_opa(modeWidgets.nodeLine, LV_OPA_COVER, 0);
  for (uint8_t nodeIndex = 0; nodeIndex < UI_MODE_NODES; nodeIndex++)
  {
    lv_obj_t *node = plainObject(modeWidgets.nodeRow);
    modeWidgets.node[nodeIndex] = node;
    lv_obj_set_size(node, 50, 50);
    lv_obj_set_style_radius(node, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(node, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(node, 3, 0);
    modeWidgets.nodeLabel[nodeIndex] = label(node, &lv_font_montserrat_16, palette.text, LV_ALIGN_CENTER, 0, 0, "");
    fitOneLine(modeWidgets.nodeLabel[nodeIndex], 44); // 0.30.2: inside its 50 px circle
  }
  lv_obj_add_flag(modeWidgets.nodeRow, LV_OBJ_FLAG_HIDDEN);
  // option buttons (menu > Navigation), in place of the header. A tap picks one (onModeChoice); a swipe anywhere steps
  // between them (onModeSwipe), so they work from the web UI's Gauge Control too, which can only swipe or tap the middle
  modeWidgets.choiceRow = plainObject(modeWidgets.body);
  lv_obj_set_size(modeWidgets.choiceRow, UI_MODE_CHOICES * MODE_CHOICE_W + (UI_MODE_CHOICES - 1) * MODE_CHOICE_GAP, MODE_CHOICE_H);
  lv_obj_align(modeWidgets.choiceRow, LV_ALIGN_CENTER, 0, -96);
  for (uint8_t choiceIndex = 0; choiceIndex < UI_MODE_CHOICES; choiceIndex++)
  {
    lv_obj_t *button = lv_obj_create(modeWidgets.choiceRow);
    lv_obj_remove_style_all(button);
    modeWidgets.choice[choiceIndex] = button;
    lv_obj_set_size(button, MODE_CHOICE_W, MODE_CHOICE_H);
    lv_obj_set_style_radius(button, MODE_CHOICE_H / 2, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, colour(palette.panel), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, lineColor(), 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE); // a long press on a button still reaches the screen (the menu)
    // the button's number goes in the event's "pointer" slot (a plain number, not an address), read back in modeChoiceCb
    lv_obj_add_event_cb(button, modeChoiceCb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)choiceIndex);
    modeWidgets.choiceLabel[choiceIndex] = label(button, &lv_font_montserrat_18, palette.text, LV_ALIGN_CENTER, 0, 0, "");
  }
  lv_obj_add_flag(modeWidgets.choiceRow, LV_OBJ_FLAG_HIDDEN);
  modeWidgets.hint = glassLabel(modeWidgets.scr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_BOTTOM_MID, 0, -40, "");
  lv_obj_add_event_cb(modeWidgets.scr, modeEventCb, LV_EVENT_GESTURE, NULL);
  lv_obj_add_event_cb(modeWidgets.scr, modeEventCb, LV_EVENT_LONG_PRESSED, NULL);
}

// caller holds the LVGL lock
static void showModeScreen(uint8_t item)
{
  if (!modeWidgets.scr)
  {
    buildModeScreen();
  }
  modeItem = item;
  memset(modeWidgets.hotMs, 0, sizeof(modeWidgets.hotMs));
  lv_obj_set_style_translate_x(modeWidgets.body, 0, 0);
  lv_obj_set_style_opa(modeWidgets.body, LV_OPA_COVER, 0);
  modeRefresh(true); // the provider claims the source (K-line session, ELM link) on this first call
  showScreen(modeWidgets.scr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

// ---------------------------------------------------------------------------
// Haldex mode screen (menu > Haldex, docs/ui-concept "Haldex"): the Haldex gauge, the mode under it with a dot per
// mode, and a swipe left / right requests the next / previous mode (0x6A0, or via the chain master). The requested
// mode shows straight away and stays until the OpenHaldex broadcast confirms it (or HDX_PENDING_MS passes).
// ---------------------------------------------------------------------------
#define HDX_MODES 6
#define HDX_PENDING_MS 2500
static PageWidgets haldexWidgets = {};
static lv_obj_t *haldexModeDots[HDX_MODES] = {};
static int8_t haldexPendingMode = -1;
static int8_t haldexDotShown = -2;
static uint32_t haldexPendingMs = 0;

static void refreshHaldexMode()
{
  refreshHaldex(haldexWidgets);
  const bool link = signalValid(SIG_HALDEX_ACTUAL);
  const int mode = (int)signalGet(SIG_HALDEX_MODE);
  if (haldexPendingMode >= 0 && ((link && mode == haldexPendingMode) || lv_tick_elaps(haldexPendingMs) > HDX_PENDING_MS))
  {
    haldexPendingMode = -1;
  }
  if (haldexPendingMode >= 0)
  {
    setText(haldexWidgets.value2, haldexModeName(haldexPendingMode));
  }
  if (!link)
  {
    char waitText[48];
    if (runtime.ohxRxMs && millis() - runtime.ohxRxMs < haldexTimeout)
    {
      copyStr(waitText, sizeof(waitText), "OpenHaldex: no Haldex CAN data");
    }
    else if (settings.ohxWireless)
    {
      snprintf(waitText, sizeof(waitText), "waiting for OpenHaldex (0x%03X / WiFi)", settings.haldexBroadcastId);
    }
    else
    {
      snprintf(waitText, sizeof(waitText), "waiting for OpenHaldex (0x%03X)", settings.haldexBroadcastId);
    }
    setText(haldexWidgets.sub, waitText);
  }
  int8_t shown = -1;
  if (haldexPendingMode >= 0)
  {
    shown = haldexPendingMode;
  }
  else if (link && mode >= 0 && mode < HDX_MODES)
  {
    shown = mode;
  }
  if (shown == haldexDotShown)
  {
    return;
  }
  haldexDotShown = shown;
  for (int i = 0; i < HDX_MODES; i++)
  {
    const int32_t size = i == shown ? 10 : 7;
    lv_obj_set_size(haldexModeDots[i], size, size);
    bgColor(haldexModeDots[i], i == shown ? colour(palette.accent2) : lineColor(), LV_PART_MAIN);
  }
}

static void haldexModeEventCb(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_LONG_PRESSED)
  {
    uiOpenMenu();
    return;
  }
  if (code != LV_EVENT_GESTURE)
  {
    return;
  }
  const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
  if (direction == LV_DIR_TOP || direction == LV_DIR_BOTTOM)
  {
    uiOpenMenu();
    return;
  }
  int step = 0;
  if (direction == LV_DIR_LEFT)
  {
    step = 1;
  }
  else if (direction == LV_DIR_RIGHT)
  {
    step = -1;
  }
  if (!step)
  {
    return;
  }
  const bool link = signalValid(SIG_HALDEX_ACTUAL);
  const int mode = (int)signalGet(SIG_HALDEX_MODE);
  int base = 0;
  if (haldexPendingMode >= 0)
  {
    base = haldexPendingMode;
  }
  else if (link && mode >= 0 && mode < HDX_MODES)
  {
    base = mode;
  }
  const uint8_t nextMode = (uint8_t)((base + step + HDX_MODES) % HDX_MODES);
  if (callbacks.onHaldexMode && callbacks.onHaldexMode(nextMode))
  {
    haldexPendingMode = nextMode;
    haldexPendingMs = lv_tick_get();
    slideIn(haldexWidgets.value2, step);
  }
  refreshHaldexMode();
}

// caller holds the LVGL lock
static void showHaldexMode()
{
  if (!haldexWidgets.scr)
  {
    memset(&haldexWidgets, 0, sizeof(haldexWidgets));
    haldexWidgets.scr = newScreen();
    GaugePageCfg plain = {}; // no second signal on the mode screen
    buildHaldex(haldexWidgets, plain);
    lv_obj_align(haldexWidgets.sub, LV_ALIGN_CENTER, 0, 150);
    lv_obj_t *row = plainObject(haldexWidgets.scr);
    lv_obj_set_size(row, HDX_MODES * 18, 12);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 118);
    for (int i = 0; i < HDX_MODES; i++)
    {
      haldexModeDots[i] = plainObject(row);
      lv_obj_set_style_radius(haldexModeDots[i], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(haldexModeDots[i], LV_OPA_COVER, 0);
    }
    lv_obj_add_event_cb(haldexWidgets.scr, haldexModeEventCb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(haldexWidgets.scr, haldexModeEventCb, LV_EVENT_LONG_PRESSED, NULL);
    haldexDotShown = -2;
  }
  refreshHaldexMode();
  showScreen(haldexWidgets.scr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

static lv_obj_t *brightnessLabel = NULL;

static void brightnessCb(lv_event_t *event) // the slider runs 1..10 = 10..100 %
{
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(event);
  uint8_t percent = (uint8_t)(lv_slider_get_value(slider) * 10);
  if (brightnessLabel)
  {
    lv_label_set_text_fmt(brightnessLabel, "Brightness  %u %%", percent);
  }
  settings.brightness = percent;
  settingsDirty = true;
  if (callbacks.onBrightness)
  {
    callbacks.onBrightness(percent);
  }
}

static void terminationCb(lv_event_t *event)
{
  lv_obj_t *switchObj = (lv_obj_t *)lv_event_get_target(event);
  bool on = lv_obj_has_state(switchObj, LV_STATE_CHECKED);
  settings.canTermination = on;
  settingsDirty = true;
  if (callbacks.onCanTermination)
  {
    callbacks.onCanTermination(on);
  }
}

static const char *unitsText()
{
  if (settings.units == 1)
  {
    return "Units: Imperial (mph, \xC2\xB0"
           "F, psi)";
  }
  if (settings.units == 2)
  {
    return "Units: Custom (set in the web UI)"; // 0.27.0: a unit picked per quantity on the Display tab
  }
  return "Units: Metric (km/h, \xC2\xB0"
         "C, bar)";
}

// tap: Metric <-> Imperial (Custom goes to Metric). The firmware's callback sets every unit, turns the pages' Min / Max /
// Redline over into the new units and rebuilds the pages (0.27.0: src/can2gauge_Display.cpp onUnits)
static void unitsCb(lv_event_t *event)
{
  bool imperial = false;
  if (settings.units == 0)
  {
    imperial = true; // Metric -> Imperial; Imperial or Custom -> Metric
  }
  if (callbacks.onUnits)
  {
    callbacks.onUnits(imperial);
  }
  settingsDirty = true;
  lv_label_set_text((lv_obj_t *)lv_event_get_user_data(event), unitsText());
}

static void settingsLongPressCb(lv_event_t *event); // 0.38.0: below the theme picker (it closes it)

// a full-width settings button (Units, Theme): text centred, tap = callback (user data = the label)
static lv_obj_t *settingButton(lv_obj_t *parent, int y, const char *text, lv_event_cb_t callback)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_size(button, 320, 42);
  lv_obj_align(button, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_bg_color(button, colour(palette.panel), 0);
  lv_obj_t *buttonLabel = lv_label_create(button);
  lv_obj_set_style_text_font(buttonLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(buttonLabel, colour(palette.text), 0);
  lv_obj_set_width(buttonLabel, 296);
  lv_label_set_long_mode(buttonLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(buttonLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(buttonLabel, text);
  lv_obj_center(buttonLabel);
  // 0.38.0: a short press only (LVGL calls a long press "clicked" too when it's let go), and a long press on it goes
  // back a level like anywhere else on the screen
  lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, buttonLabel);
  lv_obj_add_event_cb(button, settingsLongPressCb, LV_EVENT_LONG_PRESSED, NULL);
  return button;
}

static lv_obj_t *settingRow(lv_obj_t *parent, int y, const char *text)
{
  lv_obj_t *rowLabel = label(parent, &lv_font_montserrat_18, palette.text, LV_ALIGN_TOP_LEFT, 70, y, text);
  return rowLabel;
}

// ---------------------------------------------------------------------------
// theme picker (Settings > Theme): roller over every theme, the selected row drawn in its dial colour, dial / needle
// colour chips and the era below. Built on open, deleted on close, so it always uses the current palette.
// ---------------------------------------------------------------------------
static lv_obj_t *themeScr = NULL;
static lv_obj_t *themeRoller = NULL;
static lv_obj_t *themeEra = NULL;
static lv_obj_t *themeChipDial = NULL;
static lv_obj_t *themeChipNeedle = NULL;
static uint8_t themePickerIds[96];

static void closeThemePicker(bool rethemed)
{
  if (rethemed)
  {
    uiApplyTheme(); // builds a new settings screen in the new colours
  }
  showScreen(settingsScr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, true); // true: deletes the picker afterwards
  themeChipNeedle = NULL;
  themeChipDial = NULL;
  themeEra = NULL;
  themeRoller = NULL;
  themeScr = NULL;
}

// 0.38.0: a long press on the settings screens goes back a level, wherever it lands (the screen, a button, the theme
// list): the theme picker back to Settings (as Cancel), Settings back to the menu (saved first, as its Menu button)
static void settingsLongPressCb(lv_event_t *event)
{
  (void)event;
  if (themeScr && lv_screen_active() == themeScr)
  {
    closeThemePicker(false);
    return;
  }
  if (callbacks.onSettingsChanged)
  {
    callbacks.onSettingsChanged();
  }
  uiOpenMenu();
}

static void themePickerChanged(lv_event_t *)
{
  const uint8_t id = themePickerIds[lv_roller_get_selected(themeRoller)];
  uint32_t dial, needle;
  themeColours(id, dial, needle);
  lv_obj_set_style_text_color(themeRoller, colour(dial), LV_PART_SELECTED);
  lv_obj_set_style_bg_color(themeChipDial, colour(dial), 0);
  lv_obj_set_style_bg_color(themeChipNeedle, colour(needle), 0);
  if (id == THEME_MATCH)
  {
    lv_label_set_text_fmt(themeEra, "from the VIN (Bluetooth OBD)\nnow: %s",
                          settings.themeMatched != THEME_NONE ? themeName(settings.themeMatched) : "no VIN yet - Custom colours");
  }
  else
  {
    const ThemeDef *theme = themeById(id);
    lv_label_set_text(themeEra, theme ? theme->era : "");
  }
}

static lv_obj_t *colourChip(lv_obj_t *parent, int x, int y)
{
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_set_size(obj, 26, 26);
  lv_obj_align(obj, LV_ALIGN_TOP_MID, x, y);
  lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, colour(palette.muted), 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

static lv_obj_t *smallButton(lv_obj_t *parent, int x, uint32_t color, const char *text, lv_event_cb_t callback)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_size(button, 130, 44);
  lv_obj_align(button, LV_ALIGN_BOTTOM_MID, x, -30);
  lv_obj_set_style_bg_color(button, colour(color), 0);
  lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, NULL);              // 0.38.0: short press only
  lv_obj_add_event_cb(button, settingsLongPressCb, LV_EVENT_LONG_PRESSED, NULL); // ... a long press goes back
  lv_obj_t *buttonLabel = lv_label_create(button);
  lv_label_set_text(buttonLabel, text);
  lv_obj_center(buttonLabel);
  return button;
}

static void showThemePicker()
{
  // one left behind (the picker was left by the web UI's page buttons or Follow Page, not Cancel / Apply): delete it
  if (themeScr && themeScr != lv_screen_active())
  {
    lv_obj_delete(themeScr);
  }
  themeScr = newScreen();
  label(themeScr, &lv_font_montserrat_24, palette.accent, LV_ALIGN_TOP_MID, 0, 36, "THEME");
  // options: every theme in table order; Custom only once it has been set up in the web UI (it needs colour pickers)
  EXT_RAM_BSS_ATTR static char options[2600];
  size_t optionsLen = 0;
  uint8_t count = 0;
  uint8_t selected = 0;
  for (uint8_t themeIndex = 0; themeIndex < themeCount() && count < sizeof(themePickerIds); themeIndex++)
  {
    const ThemeDef &theme = themeAt(themeIndex);
    if (theme.id == THEME_CUSTOM && settings.theme != THEME_CUSTOM)
    {
      continue;
    }
    if (theme.id == settings.theme)
    {
      selected = count;
    }
    themePickerIds[count++] = theme.id;
    optionsLen += snprintf(options + optionsLen, sizeof(options) - optionsLen, "%s%s", optionsLen ? "\n" : "", theme.name);
    if (optionsLen >= sizeof(options))
    {
      break;
    }
  }

  themeRoller = lv_roller_create(themeScr);
  lv_roller_set_options(themeRoller, options, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(themeRoller, 5);
  lv_obj_set_width(themeRoller, 350);
  lv_obj_align(themeRoller, LV_ALIGN_TOP_MID, 0, 80);
  lv_obj_set_style_text_font(themeRoller, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_font(themeRoller, &lv_font_montserrat_20, LV_PART_SELECTED);
  lv_obj_set_style_bg_color(themeRoller, colour(palette.bg), 0);
  lv_obj_set_style_text_color(themeRoller, colour(palette.muted), 0);
  lv_obj_set_style_border_width(themeRoller, 0, 0);
  lv_obj_set_style_bg_color(themeRoller, colour(palette.panel), LV_PART_SELECTED);
  lv_obj_set_style_bg_opa(themeRoller, LV_OPA_COVER, LV_PART_SELECTED);
  lv_roller_set_selected(themeRoller, selected, LV_ANIM_OFF);
  lv_obj_add_event_cb(themeRoller, themePickerChanged, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(themeRoller, settingsLongPressCb, LV_EVENT_LONG_PRESSED, NULL); // 0.38.0: back to Settings
  lv_obj_add_event_cb(themeScr, settingsLongPressCb, LV_EVENT_LONG_PRESSED, NULL);
  // the theme's two colours: the dial's and the needle's (0.38.0: named - two same-coloured dots meant nothing)
  themeChipDial = colourChip(themeScr, -20, 292);
  themeChipNeedle = colourChip(themeScr, 20, 292);
  label(themeScr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_TOP_MID, -66, 296, "Dial");
  label(themeScr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_TOP_MID, 72, 296, "Needle");
  themeEra = label(themeScr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_TOP_MID, 0, 326, "");
  lv_obj_set_width(themeEra, 330);
  lv_obj_set_style_text_align(themeEra, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(themeEra, LV_LABEL_LONG_WRAP);
  smallButton(themeScr, -90, palette.panel, LV_SYMBOL_LEFT "  Cancel", [](lv_event_t *)
              { closeThemePicker(false); });
  smallButton(themeScr, 90, palette.panel, LV_SYMBOL_OK "  Apply", [](lv_event_t *)
              {
    const uint8_t id = themePickerIds[lv_roller_get_selected(themeRoller)];
    const bool changed = id != settings.theme;
    if (changed)
    {
      settings.theme = id;
      settingsDirty = true;
    }
    closeThemePicker(changed); });
  themePickerChanged(NULL);
  showScreen(themeScr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

static void buildSettings()
{
  settingsScr = newScreen();
  label(settingsScr, &lv_font_montserrat_24, palette.accent, LV_ALIGN_TOP_MID, 0, 36, "SETTINGS");
  brightnessLabel = settingRow(settingsScr, 78, "");
  lv_label_set_text_fmt(brightnessLabel, "Brightness  %u %%", settings.brightness);
  lv_obj_t *slider = lv_slider_create(settingsScr);
  lv_obj_set_size(slider, 300, 16);
  lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 108);
  lv_slider_set_range(slider, 1, 10); // 10 % steps; a tap on the bar jumps there
  lv_slider_set_value(slider, (settings.brightness + 5) / 10 < 1 ? 1 : (settings.brightness + 5) / 10, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, accentColor(), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, accentColor(), LV_PART_KNOB);
  lv_obj_add_event_cb(slider, brightnessCb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(slider, settingsLongPressCb, LV_EVENT_LONG_PRESSED, NULL); // 0.38.0: a long press goes back
  lv_obj_add_event_cb(settingsScr, settingsLongPressCb, LV_EVENT_LONG_PRESSED, NULL); // ... to the menu

  settingRow(settingsScr, 146, "CAN 120R termination");
  lv_obj_t *terminationSwitch = lv_switch_create(settingsScr);
  lv_obj_align(terminationSwitch, LV_ALIGN_TOP_RIGHT, -70, 140);
  if (settings.canTermination == 1) // 0.30.15: 1 = On (2 = Auto shows as off here; the web UI sets Auto)
  {
    lv_obj_add_state(terminationSwitch, LV_STATE_CHECKED);
  }
  lv_obj_set_style_bg_color(terminationSwitch, accentColor(), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(terminationSwitch, terminationCb, LV_EVENT_VALUE_CHANGED, NULL);

  // units and theme: two buttons the same size; units toggles, theme opens the picker
  settingButton(settingsScr, 186, unitsText(), unitsCb);
  char themeText[64];
  snprintf(themeText, sizeof(themeText), "Theme: %s  " LV_SYMBOL_RIGHT, themeName(settings.theme));
  settingButton(settingsScr, 238, themeText, [](lv_event_t *)
                { showThemePicker(); });

  lv_obj_t *wifi = label(settingsScr, &lv_font_montserrat_14, palette.muted, LV_ALIGN_TOP_MID, 0, 292, "");
  lv_obj_set_user_data(settingsScr, wifi);
  lv_obj_set_style_text_align(wifi, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(wifi, 300); // 0.30.2: a long AP name wraps inside the glass, not past it
  lv_label_set_long_mode(wifi, LV_LABEL_LONG_WRAP);
  lv_obj_align(wifi, LV_ALIGN_TOP_MID, 0, 292);

  // Menu / Reboot: 120 wide, 66 px apart from the centre line and 70 up, so both sit well inside the round glass
  lv_obj_t *reboot = lv_button_create(settingsScr);
  lv_obj_set_size(reboot, 120, 44);
  lv_obj_align(reboot, LV_ALIGN_BOTTOM_MID, 66, -70);
  lv_obj_set_style_bg_color(reboot, colour(palette.danger), 0);
  lv_obj_add_event_cb(reboot, [](lv_event_t *)
                      { if (callbacks.onReboot){ callbacks.onReboot();
} }, LV_EVENT_SHORT_CLICKED, NULL); // 0.38.0: short press only - a long press let go here must not reboot
  lv_obj_add_event_cb(reboot, settingsLongPressCb, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_t *rebootLabel = lv_label_create(reboot);
  lv_label_set_text(rebootLabel, "Reboot");
  lv_obj_center(rebootLabel);
  lv_obj_t *back = lv_button_create(settingsScr);
  lv_obj_set_size(back, 120, 44);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, -66, -70);
  lv_obj_set_style_bg_color(back, colour(palette.panel), 0);
  lv_obj_add_event_cb(back, [](lv_event_t *)
                      { if (callbacks.onSettingsChanged){ callbacks.onSettingsChanged();
} uiOpenMenu(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(back, settingsLongPressCb, LV_EVENT_LONG_PRESSED, NULL); // 0.38.0
  lv_obj_t *backLabel = lv_label_create(back);
  lv_label_set_text(backLabel, LV_SYMBOL_LEFT "  Menu");
  lv_obj_center(backLabel);
}

// alert: the whole round glass pulses red (a thick red rim and a red wash, ~1.2 s a cycle) behind a red card with a
// warning sign and the message. A 50 ms timer drives the pulse, so a full-screen redraw never starves the gauges.
static lv_obj_t *alertGlow = NULL;
static lv_obj_t *alertHint = NULL;
static lv_timer_t *alertTimer = NULL;
// 0.24.13: a clear that couldn't get the LVGL lock, done at uiTick's next pass instead. The alert's own full-screen pulse
// held the lock ~165 ms a frame, so uiClearAlert's 50 ms wait nearly always ran out and Clear Alerts did nothing.
static volatile bool alertClearPending = false;
static void clearAlertLocked(); // further down, with uiClearAlert
static uint8_t alertPhase = 0;
#define ALERT_STEPS 24 // x 50 ms = one pulse

// 0.24.15: only the alert box pulses, between dark and bright red, solid. Before, the full-screen glow's ring and red
// wash faded in and out too: every step re-blended all 230,400 pixels plus the gauge under them (~177 ms a step on the
// glass, so the 50 ms pulse showed as jumps). The glow now stays steady while the alert is up (set in uiAlert).
#define ALERT_GLOW_RING_OPA 200 // the steady red ring round the edge
#define ALERT_GLOW_WASH_OPA 40  // the steady red wash over the gauge
static void alertPulse(lv_timer_t *)
{
  alertPhase = (alertPhase + 1) % ALERT_STEPS;
  const float pulse = 0.5f - 0.5f * cosf(alertPhase * 6.2831853f / ALERT_STEPS); // 0 .. 1 .. 0
  const lv_color_t bright = colour(palette.danger);
  const lv_color_t dark = lv_color_darken(bright, 90);
  lv_obj_set_style_bg_color(alertBox, lv_color_mix(bright, dark, (uint8_t)(255 * pulse)), 0);
}

// 0.32.0: a tap on the alert box: acknowledged (the triggers hear it through runtime.alertAcks), then away
static void alertTapped(lv_event_t *event)
{
  (void)event;
  runtime.alertAcks++;
  uiClearAlert();
}

static void buildAlert()
{
  alertGlow = lv_obj_create(lv_layer_top());
  lv_obj_set_size(alertGlow, 480, 480);
  lv_obj_center(alertGlow);
  lv_obj_set_style_radius(alertGlow, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(alertGlow, colour(palette.danger), 0);
  lv_obj_set_style_bg_opa(alertGlow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(alertGlow, colour(palette.danger), 0);
  lv_obj_set_style_border_width(alertGlow, 34, 0);
  lv_obj_set_style_border_opa(alertGlow, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(alertGlow, LV_OBJ_FLAG_CLICKABLE); // a tap goes through to the screen (acknowledges a latched one)
  lv_obj_remove_flag(alertGlow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(alertGlow, LV_OBJ_FLAG_HIDDEN);

  alertBox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(alertBox, 360, 190);
  lv_obj_center(alertBox);
  lv_obj_set_style_bg_color(alertBox, lv_color_darken(colour(palette.danger), 40), 0);
  lv_obj_set_style_bg_opa(alertBox, LV_OPA_COVER, 0); // 0.24.15: solid, so a pulse step doesn't re-blend the gauge behind
  lv_obj_set_style_border_color(alertBox, lv_color_white(), 0);
  lv_obj_set_style_border_width(alertBox, 2, 0);
  lv_obj_set_style_border_opa(alertBox, LV_OPA_40, 0);
  lv_obj_set_style_radius(alertBox, 26, 0);
  lv_obj_set_style_pad_all(alertBox, 0, 0);
  lv_obj_remove_flag(alertBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(alertBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(alertBox, alertTapped, LV_EVENT_CLICKED, NULL);
  lv_obj_t *icon = lv_label_create(alertBox);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(icon, lv_color_white(), 0);
  lv_label_set_text(icon, LV_SYMBOL_WARNING);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 16);
  alertLabel = lv_label_create(alertBox);
  lv_obj_set_style_text_font(alertLabel, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(alertLabel, lv_color_white(), 0);
  lv_obj_set_style_text_align(alertLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(alertLabel, 330);
  lv_label_set_long_mode(alertLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(alertLabel, LV_ALIGN_CENTER, 0, 10);
  alertHint = lv_label_create(alertBox);
  lv_obj_set_style_text_font(alertHint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(alertHint, lv_color_white(), 0);
  lv_obj_set_style_text_opa(alertHint, LV_OPA_70, 0);
  lv_label_set_text(alertHint, "Tap to acknowledge");
  lv_obj_align(alertHint, LV_ALIGN_BOTTOM_MID, 0, -14);
}

static void buildProgress()
{
  progressBox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(progressBox, 480, 480);
  lv_obj_center(progressBox);
  lv_obj_set_style_bg_color(progressBox, colour(palette.bg), 0);
  lv_obj_set_style_bg_opa(progressBox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(progressBox, 0, 0);
  lv_obj_set_style_radius(progressBox, 0, 0);
  lv_obj_set_style_pad_all(progressBox, 0, 0);
  lv_obj_remove_flag(progressBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(progressBox, LV_OBJ_FLAG_CLICKABLE); // swallow gestures so a stray swipe cannot change page mid-job
  lv_obj_add_flag(progressBox, LV_OBJ_FLAG_HIDDEN);
  progressArc = lv_arc_create(progressBox);
  lv_obj_set_size(progressArc, 440, 440);
  lv_obj_center(progressArc);
  lv_arc_set_rotation(progressArc, 270);
  lv_arc_set_bg_angles(progressArc, 0, 360);
  lv_arc_set_range(progressArc, 0, 100);
  lv_obj_remove_style(progressArc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(progressArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(progressArc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(progressArc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(progressArc, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(progressArc, true, LV_PART_INDICATOR);
  progressTitle = label(progressBox, &lv_font_montserrat_20, palette.accent, LV_ALIGN_CENTER, 0, -95, "");
  progressPct = label(progressBox, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, -20, "");
  lv_obj_set_style_transform_pivot_x(progressPct, lv_pct(50), 0); // 0.24.27: scaled from its middle, as the pages'
  lv_obj_set_style_transform_pivot_y(progressPct, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(progressPct, 400, 0);
  progressDetail = lv_label_create(progressBox);
  lv_obj_set_style_text_font(progressDetail, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(progressDetail, colour(palette.muted), 0);
  lv_obj_set_style_text_align(progressDetail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(progressDetail, 320);
  lv_label_set_long_mode(progressDetail, LV_LABEL_LONG_WRAP);
  lv_obj_align(progressDetail, LV_ALIGN_CENTER, 0, 85);
}

// 0.37.4: the ring fades out when it goes (after SET UP / an update's last word, or Skip), instead of just vanishing
#define PROGRESS_FADE_MS 500 // how long the fade takes

// one step of the fade: the whole card's opacity, 255 (solid) down to 0
static void progressFadeCb(void *obj, int32_t value)
{
  lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

// the fade is over: hide the card and make it solid again for the next time it shows
static void progressFadeDone(lv_anim_t *anim)
{
  (void)anim;
  lv_obj_add_flag(progressBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(progressBox, LV_OPA_COVER, 0);
}

// fades the ring out (nothing to do if it isn't showing)
static void progressFadeOut()
{
  if (!progressBox || lv_obj_has_flag(progressBox, LV_OBJ_FLAG_HIDDEN))
  {
    return;
  }
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, progressBox);
  lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP); // solid -> see-through
  lv_anim_set_duration(&anim, PROGRESS_FADE_MS);
  lv_anim_set_exec_cb(&anim, progressFadeCb);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
  lv_anim_set_completed_cb(&anim, progressFadeDone);
  lv_anim_start(&anim);
}

// 0.38.0: the ring moves to a new value over this long instead of jumping there
#define PROGRESS_GLIDE_MS 600

// one step of the glide: the ring and the number at value (0..100)
static void progressGlideCb(void *obj, int32_t value)
{
  (void)obj;
  lv_arc_set_value(progressArc, value);
  lv_label_set_text_fmt(progressPct, "%d%%", (int)value);
}

static void progressHideCb(lv_timer_t *)
{
  progressTimer = NULL; // repeat count 1: LVGL deletes it
  progressFadeOut(); // 0.37.4: was hidden at once
}

void uiProgress(const char *title, const char *detail, int pct, bool failed, uint32_t autoHideMs)
{
  if (!lvglLock(50))
  {
    return;
  }
  if (!progressBox)
  {
    buildProgress();
  }
  if (progressTimer)
  {
    lv_timer_delete(progressTimer);
    progressTimer = NULL;
  }
  if (pct < 0)
  {
    progressFadeOut(); // 0.37.4: fades out (was hidden at once)
    lvglUnlock();
    return;
  }
  // 0.37.4: showing it again stops a fade that's running and makes it solid
  lv_anim_delete(progressBox, progressFadeCb);
  lv_obj_set_style_opa(progressBox, LV_OPA_COVER, 0);
  lv_color_t ringColor;
  if (failed)
  {
    ringColor = colour(palette.danger);
  }
  else if (pct >= 100)
  {
    ringColor = colour(palette.ok);
  }
  else
  {
    ringColor = accentColor();
  }
  // 0.38.0: the ring glides to its new value (it jumped: First Boot went 0 -> 40 % and 40 -> 99 % in one step)
  int32_t target = pct;
  if (target > 100)
  {
    target = 100;
  }
  const bool wasHidden = lv_obj_has_flag(progressBox, LV_OBJ_FLAG_HIDDEN);
  lv_anim_delete(progressArc, progressGlideCb); // a glide still running stops where it is
  const int32_t from = lv_arc_get_value(progressArc);
  if (wasHidden || target <= from)
  {
    progressGlideCb(progressArc, target); // a fresh ring, or a step back (a new job): straight there
  }
  else
  {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, progressArc);
    lv_anim_set_values(&anim, from, target);
    lv_anim_set_duration(&anim, PROGRESS_GLIDE_MS);
    lv_anim_set_exec_cb(&anim, progressGlideCb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out); // quick at first, settling into place
    lv_anim_start(&anim);
  }
  lv_obj_set_style_arc_color(progressArc, ringColor, LV_PART_INDICATOR);
  lv_label_set_text(progressTitle, title ? title : "");
  lv_obj_set_style_text_color(progressTitle, failed ? colour(palette.danger) : colour(palette.accent), 0);
  lv_label_set_text(progressDetail, detail ? detail : "");
  lv_obj_remove_flag(progressBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(progressBox);
  if (autoHideMs)
  {
    progressTimer = lv_timer_create(progressHideCb, autoHideMs, NULL);
    lv_timer_set_repeat_count(progressTimer, 1);
  }
  lvglUnlock();
}

// ---------------------------------------------------------------------------
// 0.29.0: Show Numbers (web UI > Daisy Chain) - the gauge's number in the row, big, over everything, so you can see
// at a glance which gauge is which and number them left to right from any gauge
// ---------------------------------------------------------------------------
static lv_obj_t *numberBox = NULL;      // the full-screen card (on the top layer, over every screen)
static lv_obj_t *numberBig = NULL;      // the number itself ("2", or "?" when the gauge has no place in the row)
static lv_obj_t *numberTitle = NULL;    // small line above it: "GAUGE"
static lv_obj_t *numberDetail = NULL;   // two lines under it: name, master / follower
static lv_timer_t *numberTimer = NULL;  // hides the card after a while

static void leaveSaverForNumber(); // 0.29.1: the screensaver / Rev Runner goes so the number shows (further down, by the saver code)

// is the number card up right now?
static bool numberShowing()
{
  return numberBox != NULL && !lv_obj_has_flag(numberBox, LV_OBJ_FLAG_HIDDEN);
}

// a tap on the card hides it
static void numberTapped(lv_event_t *event)
{
  (void)event;
  if (numberTimer)
  {
    lv_timer_delete(numberTimer);
    numberTimer = NULL;
  }
  lv_obj_add_flag(numberBox, LV_OBJ_FLAG_HIDDEN);
}

// the timer ran out: hide the card
static void numberHideCb(lv_timer_t *timer)
{
  (void)timer;
  numberTimer = NULL; // repeat count 1: LVGL deletes the timer itself
  if (numberBox)
  {
    lv_obj_add_flag(numberBox, LV_OBJ_FLAG_HIDDEN);
  }
}

// make the card the first time it is needed
static void buildNumber()
{
  numberBox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(numberBox, 480, 480);
  lv_obj_center(numberBox);
  lv_obj_set_style_bg_color(numberBox, colour(palette.bg), 0);
  lv_obj_set_style_bg_opa(numberBox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(numberBox, accentColor(), 0);
  lv_obj_set_style_border_width(numberBox, 14, 0); // a ring in the accent colour round the edge of the glass
  lv_obj_set_style_radius(numberBox, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_all(numberBox, 0, 0);
  lv_obj_remove_flag(numberBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(numberBox, LV_OBJ_FLAG_CLICKABLE); // a tap hides it (and doesn't reach the page underneath)
  lv_obj_add_event_cb(numberBox, numberTapped, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(numberBox, LV_OBJ_FLAG_HIDDEN);
  numberTitle = label(numberBox, &lv_font_montserrat_24, palette.muted, LV_ALIGN_CENTER, 0, -150, "GAUGE");
  numberBig = label(numberBox, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, -20, "");
  lv_obj_set_style_transform_pivot_x(numberBig, lv_pct(50), 0); // grown from its middle, like the progress ring's %
  lv_obj_set_style_transform_pivot_y(numberBig, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(numberBig, 1024, 0); // 256 = normal size, so 1024 = four times as big
  numberDetail = lv_label_create(numberBox);
  lv_obj_set_style_text_font(numberDetail, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(numberDetail, colour(palette.accent), 0);
  lv_obj_set_style_text_align(numberDetail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(numberDetail, 340);
  lv_label_set_long_mode(numberDetail, LV_LABEL_LONG_WRAP);
  lv_obj_align(numberDetail, LV_ALIGN_CENTER, 0, 140);
}

void uiShowNumber(const char *number, const char *detail, uint32_t showMs)
{
  if (!lvglLock(200))
  {
    return;
  }
  if (!numberBox)
  {
    buildNumber();
  }
  if (numberTimer)
  {
    lv_timer_delete(numberTimer);
    numberTimer = NULL;
  }
  if (number == NULL)
  {
    lv_obj_add_flag(numberBox, LV_OBJ_FLAG_HIDDEN); // NULL = hide it now
    lvglUnlock();
    return;
  }
  leaveSaverForNumber(); // 0.29.1: off the screensaver (or Rev Runner) first, back to the gauge page
  lv_label_set_text(numberBig, number);
  if (detail)
  {
    lv_label_set_text(numberDetail, detail);
  }
  else
  {
    lv_label_set_text(numberDetail, "");
  }
  lv_obj_remove_flag(numberBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(numberBox);
  if (showMs)
  {
    numberTimer = lv_timer_create(numberHideCb, showMs, NULL);
    lv_timer_set_repeat_count(numberTimer, 1);
  }
  lvglUnlock();
}

// ---------------------------------------------------------------------------
// 0.30.0: turn-by-turn navigation card (src/can2gauge_Nav.cpp, docs/nav-link/NAV_LINK.md). Google Maps' next turn from
// the can2gauge Link phone app, laid out like Android Auto's: the arrow and the distance on a band across the top in the
// accent colour, the road under it, the arrival time at the bottom.
// ---------------------------------------------------------------------------
#define NAV_ARROW_SHOWN_PX 144 // the turn arrow is drawn this big (the app's 48 x 48 arrow, three times over)
#define NAV_BAND_HEIGHT 272    // the accent band across the top of the glass

static lv_obj_t *navBox = NULL;         // the full-screen card (on the top layer, over every screen)
static lv_obj_t *navBand = NULL;        // the accent band across the top
static lv_obj_t *navArrow = NULL;       // the turn arrow (an image)
static lv_obj_t *navDistance = NULL;    // "250 m", big, under the arrow ("Rerouting" / "Arrived" in its place)
static lv_obj_t *navInstruction = NULL; // "Turn right onto High Street", two lines at most
static lv_obj_t *navEta = NULL;         // "12 min - 5.2 km - 22:41", small, at the bottom
// 0.30.7 / 0.30.8: the journey ring round the edge - a 3/4 circle (open at the bottom), filling from the start dot
// (bottom left, where the route began) over the top to the chequered flag (bottom right, the destination)
static lv_obj_t *navRingTrack = NULL;   // dark outline under the ring, so it shows on any band colour
static lv_obj_t *navRing = NULL;        // the ring: grey still to go, the second accent colour covered so far
static lv_obj_t *navRingStart = NULL;   // the start dot
static lv_obj_t *navRingFlag = NULL;    // the finish flag (a box with the chequer image in it)
static lv_obj_t *navRingFlagImage = NULL;
#define NAV_RING_SIZE 468               // the ring's box (its outer edge, px)
#define NAV_RING_WIDTH 10               // the ring's thickness
#define NAV_RING_OUTLINE 4              // the dark outline each side of it
#define NAV_FLAG_PX 18                  // the chequered flag, each way
#define NAV_FLAG_SQUARE 6               // ... in squares this big (3 x 3)
static uint8_t navFlagPixels[NAV_FLAG_PX * NAV_FLAG_PX]; // the chequer, one byte a dot (A8, coloured by recolour)
static lv_image_dsc_t navFlagImage;
static bool navDismissed = false;       // tapped away: stays hidden until the next turn
static lv_obj_t *navCoveredPage = NULL; // 0.30.9: the page hidden under the card while it's up (NULL = none)
static bool navWaitingForBoot = false;  // 0.30.17: a turn came in during the boot logo: the card joins the logo's fade

// the arrow's dots as LVGL wants them: one byte a dot, 0 = see-through, 255 = solid ("A8"). The image is coloured by
// its recolour style, the same way the gauge icons are. In PSRAM (EXT_RAM_BSS_ATTR): 4 KB internal RAM can't spare.
EXT_RAM_BSS_ATTR static uint8_t navArrowPixels[UI_NAV_ARROW_MAX * UI_NAV_ARROW_MAX];
static lv_image_dsc_t navArrowImage; // tells LVGL the size and format of navArrowPixels

// 0.30.1: the stored arrows (/nav/<name>.bin, tools/nav_arrows.py), kept on the filesystem like the boot logos and
// read when a turn needs one. "C2GA": a 12-byte header ("C2GA", version 1, flags, width, height, spare), then one
// byte a dot (A8) at the size it's shown (144 x 144). The file is read in uiTick (the UI task: its stack is in internal
// RAM, which reading flash needs; uiNavShow runs in the logic task, whose stack is in PSRAM).
#define NAV_STORED_MAX_PX 160 // biggest stored arrow taken, each way
EXT_RAM_BSS_ATTR static uint8_t navStoredPixels[NAV_STORED_MAX_PX * NAV_STORED_MAX_PX]; // 25 KB of PSRAM
static lv_image_dsc_t navStoredImage;  // tells LVGL the size and format of navStoredPixels
static char navWantedFile[24] = "";    // the stored arrow the card wants ("" = it shows dots or nothing)
static char navLoadedFile[24] = "";    // the one last read into navStoredPixels (tried, whether it worked or not)
static bool navStoredOk = false;       // navLoadedFile was read and is in navStoredPixels
#define NAV_SRC_NONE 0   // navArrow shows nothing yet
#define NAV_SRC_DOTS 1   // navArrow shows navArrowImage (Maps' own arrow, from the phone)
#define NAV_SRC_STORED 2 // navArrow shows navStoredImage
static uint8_t navArrowSource = NAV_SRC_NONE;

// 0.30.9: the page under the card was deleted (pages are rebuilt after a settings change): forget it, so it isn't
// touched after it's gone
static void navCoveredPageDeleted(lv_event_t *event)
{
  (void)event;
  navCoveredPage = NULL;
}

// 0.30.9: the page under the card is hidden while the card is up. On the gauge, a needle dial's tick marks were drawn
// on top of the card (faint horizontal lines down the left of the ring, more after each time the card came and went).
// With the page hidden nothing under the card is drawn at all, so nothing can show through (and it's less drawing).
// 0.30.17: the page to hide is passed in (the boot fade hides the first page before it's on the glass)
static void navHidePageUnder(lv_obj_t *page)
{
  if (page == navCoveredPage)
  {
    return; // hidden already
  }
  if (navCoveredPage) // the page changed while the card was up: the old one back, the new one hidden
  {
    lv_obj_remove_event_cb(navCoveredPage, navCoveredPageDeleted);
    lv_obj_remove_flag(navCoveredPage, LV_OBJ_FLAG_HIDDEN);
  }
  navCoveredPage = page;
  if (page)
  {
    lv_obj_add_event_cb(page, navCoveredPageDeleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  }
}

// 0.30.9: the card has gone: the page underneath back
static void navShowPageUnder()
{
  if (!navCoveredPage)
  {
    return;
  }
  lv_obj_remove_event_cb(navCoveredPage, navCoveredPageDeleted);
  lv_obj_remove_flag(navCoveredPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(navCoveredPage); // drawn again in full
  navCoveredPage = NULL;
}

// A tap or a long press on the card: it goes away and the gauges come back (it doesn't reach the page underneath). The
// firmware is told (onNavDismissed): in Pop Up the card comes back for the next turn, in Route not for the rest of
// the route.
static void navTapped(lv_event_t *event)
{
  (void)event;
  if (lv_obj_has_flag(navBox, LV_OBJ_FLAG_HIDDEN))
  {
    return; // gone already
  }
  navDismissed = true;
  lv_obj_add_flag(navBox, LV_OBJ_FLAG_HIDDEN);
  navShowPageUnder(); // 0.30.9
  if (callbacks.onNavDismissed)
  {
    callbacks.onNavDismissed();
  }
  // back to the gauges: the card may have come up over the menu or one of its screens (Navigation's own, say)
  if (pageCount && lv_screen_active() != pages[activePage].scr)
  {
    menuOpen = false;
    showScreen(pages[activePage].scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
  }
}

// the card's colours from the theme. Done at every show, so a theme change or night dimming reaches it too.
static void navColours()
{
  lv_obj_set_style_bg_color(navBox, colour(palette.bg), 0);
  lv_obj_set_style_bg_color(navBand, accentColor(), 0);
  // the arrow and the distance sit on the accent band, so they're drawn in the background colour: dark on a bright
  // accent reads on every theme (white on a yellow accent wouldn't)
  lv_obj_set_style_image_recolor(navArrow, colour(palette.bg), 0);
  lv_obj_set_style_text_color(navDistance, colour(palette.bg), 0);
  lv_obj_set_style_text_color(navInstruction, colour(palette.text), 0);
  lv_obj_set_style_text_color(navEta, colour(palette.muted), 0);
  // 0.30.8: the ring in the theme's second accent (picked to differ from the main accent, so it shows on the band),
  // on a dark outline; the part still to go in grey
  lv_obj_set_style_arc_color(navRingTrack, colour(palette.bg), LV_PART_MAIN);
  lv_obj_set_style_arc_color(navRing, colour(palette.muted), LV_PART_MAIN);
  lv_obj_set_style_arc_color(navRing, colour(palette.accent2), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(navRingStart, colour(palette.accent2), 0);
  lv_obj_set_style_border_color(navRingStart, colour(palette.bg), 0);
  lv_obj_set_style_bg_color(navRingFlag, colour(palette.bg), 0);
  lv_obj_set_style_border_color(navRingFlag, colour(palette.text), 0);
  lv_obj_set_style_image_recolor(navRingFlagImage, colour(palette.text), 0);
}

// 0.30.8: a point on the ring's centre line, angle in LVGL's degrees (0 = 3 o'clock, going clockwise)
static void navRingPoint(int angle, int *x, int *y)
{
  const float radius = NAV_RING_SIZE / 2 - NAV_RING_WIDTH / 2;
  const float radians = angle * 3.14159265f / 180.0f;
  *x = 240 + (int)(radius * cosf(radians));
  *y = 240 + (int)(radius * sinf(radians));
}

// 0.30.8: one arc of the ring: a 3/4 circle from 135 degrees (bottom left) round the top to 45 (bottom right)
static lv_obj_t *navRingArc(int size, int width)
{
  lv_obj_t *arc = lv_arc_create(navBox);
  lv_obj_set_size(arc, size, size);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 135);     // starts bottom left
  lv_arc_set_bg_angles(arc, 0, 270); // three quarters round, the gap at the bottom
  lv_arc_set_range(arc, 0, 1000);    // tenths of a percent, so it moves smoothly
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE); // taps go through to the card
  lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
  return arc;
}

// 0.30.8: the ring, its outline, start dot and flag shown or hidden together
static void navRingVisible(bool isVisible)
{
  lv_obj_t *parts[4] = {navRingTrack, navRing, navRingStart, navRingFlag};
  for (uint8_t i = 0; i < 4; i++)
  {
    if (isVisible)
    {
      lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
      lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// make the card the first time it is needed
static void buildNav()
{
  navBox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(navBox, 480, 480);
  lv_obj_center(navBox);
  lv_obj_set_style_bg_opa(navBox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(navBox, 0, 0);
  // 0.30.7: square, not cut round. The glass is round, so the corners are never seen anyway. Cutting it round
  // (radius + clip_corner) made LVGL draw the whole card into a separate 480 x 480 picture first, and on the gauge that
  // didn't work: a stray line for the arrow and no text (fine in the simulator, which draws in a browser).
  lv_obj_set_style_radius(navBox, 0, 0);
  lv_obj_set_style_pad_all(navBox, 0, 0);
  lv_obj_remove_flag(navBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(navBox, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(navBox, navTapped, LV_EVENT_SHORT_CLICKED, NULL); // a tap ...
  lv_obj_add_event_cb(navBox, navTapped, LV_EVENT_LONG_PRESSED, NULL);  // ... or a long press: back to the gauges
  lv_obj_add_flag(navBox, LV_OBJ_FLAG_HIDDEN);

  // the accent band across the top
  navBand = lv_obj_create(navBox);
  lv_obj_set_size(navBand, 480, NAV_BAND_HEIGHT);
  lv_obj_align(navBand, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_opa(navBand, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(navBand, 0, 0);
  lv_obj_set_style_radius(navBand, 0, 0);
  lv_obj_set_style_pad_all(navBand, 0, 0);
  lv_obj_remove_flag(navBand, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(navBand, LV_OBJ_FLAG_CLICKABLE); // taps go through to the card

  // the turn arrow: the image is the arrow's own size (48 x 48), scaled up round its middle to NAV_ARROW_SHOWN_PX
  navArrow = lv_image_create(navBand);
  lv_obj_align(navArrow, LV_ALIGN_TOP_MID, 0, 110 - 24); // its middle 110 px down the glass
  lv_obj_set_style_image_recolor_opa(navArrow, LV_OPA_COVER, 0);
  lv_image_set_antialias(navArrow, true); // smooth edges when it's scaled up (the dots are 1 bit)

  navDistance = label(navBand, &lv_font_montserrat_48, palette.bg, LV_ALIGN_TOP_MID, 0, 196, "");

  navInstruction = lv_label_create(navBox);
  lv_obj_set_style_text_font(navInstruction, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_align(navInstruction, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(navInstruction, 380);
  lv_obj_set_height(navInstruction, 72);                 // two lines of the 28 px font ...
  lv_label_set_long_mode(navInstruction, LV_LABEL_LONG_DOT); // ... and "..." when the road name is longer
  lv_obj_align(navInstruction, LV_ALIGN_TOP_MID, 0, NAV_BAND_HEIGHT + 22);

  navEta = lv_label_create(navBox);
  lv_obj_set_style_text_font(navEta, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(navEta, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(navEta, 260); // the glass is narrower down here (0.30.8: 300 -> 260, clear of the ring's ends)
  lv_label_set_long_mode(navEta, LV_LABEL_LONG_DOT);
  lv_obj_align(navEta, LV_ALIGN_TOP_MID, 0, 404);

  // 0.30.8: the distance ring (see navRingArc): a dark outline, then the ring itself just inside it
  navRingTrack = navRingArc(NAV_RING_SIZE + 2 * NAV_RING_OUTLINE, NAV_RING_WIDTH + 2 * NAV_RING_OUTLINE);
  lv_obj_set_style_arc_opa(navRingTrack, LV_OPA_TRANSP, LV_PART_INDICATOR); // the outline has no "filled" part
  navRing = navRingArc(NAV_RING_SIZE, NAV_RING_WIDTH);

  // the start dot, on the ring's start (bottom left)
  int x = 0;
  int y = 0;
  navRingPoint(135, &x, &y);
  navRingStart = lv_obj_create(navBox);
  lv_obj_set_size(navRingStart, 18, 18);
  lv_obj_set_pos(navRingStart, x - 9, y - 9);
  lv_obj_set_style_radius(navRingStart, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(navRingStart, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(navRingStart, 3, 0);
  lv_obj_remove_flag(navRingStart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(navRingStart, LV_OBJ_FLAG_SCROLLABLE);

  // the finish flag, on the ring's end (bottom right): a small chequer in a box
  for (uint8_t row = 0; row < NAV_FLAG_PX; row++)
  {
    for (uint8_t column = 0; column < NAV_FLAG_PX; column++)
    {
      const uint8_t squareRow = row / NAV_FLAG_SQUARE;
      const uint8_t squareColumn = column / NAV_FLAG_SQUARE;
      if ((squareRow + squareColumn) % 2 == 0)
      {
        navFlagPixels[row * NAV_FLAG_PX + column] = 255; // a solid square
      }
      else
      {
        navFlagPixels[row * NAV_FLAG_PX + column] = 0; // a see-through square (the box's dark colour shows)
      }
    }
  }
  navFlagImage.header.magic = LV_IMAGE_HEADER_MAGIC;
  navFlagImage.header.cf = LV_COLOR_FORMAT_A8;
  navFlagImage.header.flags = 0;
  navFlagImage.header.w = NAV_FLAG_PX;
  navFlagImage.header.h = NAV_FLAG_PX;
  navFlagImage.header.stride = NAV_FLAG_PX;
  navFlagImage.data = navFlagPixels;
  navFlagImage.data_size = NAV_FLAG_PX * NAV_FLAG_PX;
  navRingPoint(45, &x, &y);
  navRingFlag = lv_obj_create(navBox);
  lv_obj_set_size(navRingFlag, NAV_FLAG_PX + 4, NAV_FLAG_PX + 4); // the chequer plus a 2 px border
  lv_obj_set_pos(navRingFlag, x - (NAV_FLAG_PX + 4) / 2, y - (NAV_FLAG_PX + 4) / 2);
  lv_obj_set_style_radius(navRingFlag, 3, 0);
  lv_obj_set_style_bg_opa(navRingFlag, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(navRingFlag, 2, 0);
  lv_obj_set_style_pad_all(navRingFlag, 0, 0);
  lv_obj_remove_flag(navRingFlag, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(navRingFlag, LV_OBJ_FLAG_SCROLLABLE);
  navRingFlagImage = lv_image_create(navRingFlag);
  lv_image_set_src(navRingFlagImage, &navFlagImage);
  lv_obj_center(navRingFlagImage);
  lv_obj_set_style_image_recolor_opa(navRingFlagImage, LV_OPA_COVER, 0);
  navRingVisible(false);
}

// the phone's 1-bit arrow -> navArrowPixels (one byte a dot) and onto the image
static void navSetArrow(const UiNavView &view)
{
  const uint8_t width = view.arrowWidth;
  const uint8_t height = view.arrowHeight;
  const uint16_t bytesPerRow = (width + 7) / 8; // each row is padded to whole bytes
  for (uint8_t y = 0; y < height; y++)
  {
    for (uint8_t x = 0; x < width; x++)
    {
      const uint8_t rowByte = view.arrowBits[y * bytesPerRow + x / 8];
      const uint8_t bitMask = 0x80 >> (x % 8); // the leftmost dot is the top bit
      if (rowByte & bitMask)
      {
        navArrowPixels[y * width + x] = 255; // solid
      }
      else
      {
        navArrowPixels[y * width + x] = 0; // see-through
      }
    }
  }
  navArrowImage.header.magic = LV_IMAGE_HEADER_MAGIC;
  navArrowImage.header.cf = LV_COLOR_FORMAT_A8;
  navArrowImage.header.flags = 0;
  navArrowImage.header.w = width;
  navArrowImage.header.h = height;
  navArrowImage.header.stride = width;
  navArrowImage.data = navArrowPixels;
  navArrowImage.data_size = (uint32_t)width * height;
  // same buffer, new dots: clear the source and set it again so LVGL reads the new size and dots (LVGL's image cache is
  // off in lv_conf.h, and a plain A8 image is drawn straight from the buffer, so no old copy is kept anywhere)
  lv_image_set_src(navArrow, NULL);
  lv_image_set_src(navArrow, &navArrowImage);
  lv_image_set_scale(navArrow, 256 * NAV_ARROW_SHOWN_PX / width); // 256 = actual size, so 768 = three times as big
  lv_obj_align(navArrow, LV_ALIGN_TOP_MID, 0, 110 - height / 2);
  lv_obj_invalidate(navArrow);
}

// the distance (or Rerouting / Arrived) under the arrow, or up in the middle of the band when there's no arrow
static void navPlaceDistance()
{
  if (lv_obj_has_flag(navArrow, LV_OBJ_FLAG_HIDDEN))
  {
    lv_obj_align(navDistance, LV_ALIGN_TOP_MID, 0, 130);
  }
  else
  {
    lv_obj_align(navDistance, LV_ALIGN_TOP_MID, 0, 196);
  }
}

// read a stored arrow file into navStoredPixels. false = missing (an older filesystem image) or damaged.
static bool navLoadStored(const char *path)
{
  size_t size = 0;
  uint8_t *file = fsReadAll(path, &size); // the whole file in PSRAM, unzipped (nav/*.bin ship as .bin.gz)
  if (!file)
  {
    DEBUG("Nav: no arrow file %s (filesystem image older than 0.30.1?)", path);
    return false;
  }
  bool ok = false;
  if (size >= 12 && !memcmp(file, "C2GA", 4) && file[4] == 1) // "C2GA", version 1
  {
    const uint16_t width = file[6] | (file[7] << 8);  // bytes 6-7: width, low byte first
    const uint16_t height = file[8] | (file[9] << 8); // bytes 8-9: height
    if (width > 0 && height > 0 && width <= NAV_STORED_MAX_PX && height <= NAV_STORED_MAX_PX &&
        size == 12 + (size_t)width * height)
    {
      memcpy(navStoredPixels, file + 12, (size_t)width * height);
      navStoredImage.header.magic = LV_IMAGE_HEADER_MAGIC;
      navStoredImage.header.cf = LV_COLOR_FORMAT_A8;
      navStoredImage.header.flags = 0;
      navStoredImage.header.w = width;
      navStoredImage.header.h = height;
      navStoredImage.header.stride = width;
      navStoredImage.data = navStoredPixels;
      navStoredImage.data_size = (uint32_t)width * height;
      ok = true;
    }
  }
  if (!ok)
  {
    DEBUG("Nav: arrow file %s unreadable (%u bytes)", path, (unsigned)size);
  }
  heap_caps_free(file);
  return ok;
}

// put the stored arrow on the card (already read into navStoredPixels)
static void navUseStored()
{
  if (navArrowSource != NAV_SRC_STORED)
  {
    lv_image_set_src(navArrow, NULL); // set it again so LVGL reads the new size and dots
    lv_image_set_src(navArrow, &navStoredImage);
    lv_image_set_scale(navArrow, 256); // drawn at its own size (256 = 1:1)
    lv_obj_align(navArrow, LV_ALIGN_TOP_MID, 0, 110 - navStoredImage.header.h / 2); // its middle 110 px down
    navArrowSource = NAV_SRC_STORED;
  }
  lv_obj_remove_flag(navArrow, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(navArrow);
}

// from uiTick (LVGL lock held): read the stored arrow the card asked for, then show it
static void navArrowTick()
{
  if (!navBox || navWantedFile[0] == '\0' || strcmp(navWantedFile, navLoadedFile) == 0)
  {
    return; // nothing asked for, or read already
  }
  copyStr(navLoadedFile, sizeof(navLoadedFile), navWantedFile); // tried now, worked or not (no retry every tick)
  navStoredOk = navLoadStored(navLoadedFile);
  if (navArrowSource == NAV_SRC_STORED)
  {
    navArrowSource = NAV_SRC_NONE; // new dots in the same buffer: navUseStored sets the image again
  }
  if (navStoredOk)
  {
    navUseStored();
    navPlaceDistance();
  }
}

bool uiNavShow(const UiNavView &view, bool newTurn)
{
  if (!lvglLock(200))
  {
    return false; // 0.30.5: the screen was busy (a fade, a sprite): navTick tries again next time
  }
  if (!navBox)
  {
    buildNav();
  }
  if (newTurn)
  {
    navDismissed = false; // a new turn brings a tapped-away card back
  }
  if (navDismissed)
  {
    navShowPageUnder(); // 0.30.9: (already back after the tap; this just makes sure)
    lvglUnlock();
    return true; // tapped away: done as asked, it stays hidden until the next turn
  }
  navColours();

  // the big words on the band
  if (view.rerouting)
  {
    lv_label_set_text(navDistance, "Rerouting");
  }
  else if (view.arrived)
  {
    lv_label_set_text(navDistance, "Arrived");
  }
  else
  {
    lv_label_set_text(navDistance, view.distText);
  }

  // the arrow: a stored one (0.30.1), Maps' own dots, or none
  if (view.rerouting)
  {
    navWantedFile[0] = '\0';
    lv_obj_add_flag(navArrow, LV_OBJ_FLAG_HIDDEN); // no arrow while Maps works out the new route
  }
  else if (view.arrowFile[0] != '\0')
  {
    copyStr(navWantedFile, sizeof(navWantedFile), view.arrowFile);
    if (navStoredOk && strcmp(navLoadedFile, navWantedFile) == 0)
    {
      navUseStored(); // read already
    }
    else
    {
      lv_obj_add_flag(navArrow, LV_OBJ_FLAG_HIDDEN); // uiTick reads it (navArrowTick) and shows it, ~30 ms later
    }
  }
  else if (view.arrowWidth > 0 && view.arrowHeight > 0)
  {
    navWantedFile[0] = '\0';
    if (newTurn || navArrowSource != NAV_SRC_DOTS)
    {
      navSetArrow(view); // only when the turn changes: the dots are the same between updates
      navArrowSource = NAV_SRC_DOTS;
    }
    lv_obj_remove_flag(navArrow, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    navWantedFile[0] = '\0';
    lv_obj_add_flag(navArrow, LV_OBJ_FLAG_HIDDEN);
  }
  navPlaceDistance(); // no arrow: the words move up to the middle of the band

  // 0.30.8: the journey ring - how much of the whole route is done (the app works it out from Maps' time or
  // distance left), from the start dot to the chequered flag. Full on arrival; hidden while rerouting or not known.
  if (view.arrived)
  {
    lv_arc_set_value(navRing, 1000);
    navRingVisible(true);
  }
  else if (view.hasProgress && !view.rerouting)
  {
    uint16_t progress = view.routeProgress;
    if (progress > 1000)
    {
      progress = 1000;
    }
    lv_arc_set_value(navRing, progress);
    navRingVisible(true);
  }
  else
  {
    navRingVisible(false);
  }
  lv_label_set_text(navInstruction, view.instruction);
  lv_label_set_text(navEta, view.etaText);

  // 0.30.17: the boot logo is still up (the phone links within a second or two of power on): the card is filled in
  // but stays hidden, and bootDone shows it as part of the fade from the logo, like the gauge pages
  if (bootScr)
  {
    navWaitingForBoot = true;
    lvglUnlock();
    return true;
  }

  lv_obj_remove_flag(navBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(navBox);
  navHidePageUnder(lv_screen_active()); // 0.30.9: nothing under the card is drawn (it leaked through on the gauge)
  lvglUnlock();
  return true;
}

bool uiNavHide()
{
  if (!navBox)
  {
    return true; // never shown
  }
  if (!lvglLock(200))
  {
    return false; // 0.30.5: the screen was busy - before, the card was then left up for good
  }
  lv_obj_add_flag(navBox, LV_OBJ_FLAG_HIDDEN);
  navShowPageUnder();         // 0.30.9: the page back
  navWaitingForBoot = false;  // 0.30.17: route ended before the boot logo did: nothing to fade in
  navWantedFile[0] = '\0';    // nothing to read for a card that's away
  lvglUnlock();
  return true;
}

// ---------------------------------------------------------------------------
// boot logo
// ---------------------------------------------------------------------------
// 0.25.3: slower sweep and a longer fade (the sweep was 1.4 s: too quick for a boot logo)
#define BOOT_SWEEP_MS 3000        // the ring round the edge fills 0 -> 100 % in this long (was 1400)
#define BOOT_DIAL_UP_MS 1700      // the can2gauge logo's own dial: up to full in this long (was 800) ...
#define BOOT_DIAL_DOWN_MS 1300    // ... then back to 72 % in this long (was 600): 3 s in all, same as the ring
#define BOOT_HOLD_MS 700          // after a sweep, the logo stays up this long before the fade starts
#define BOOT_NO_SWEEP_MS 2200     // a logo picture with no sweep (a make logo) stays up this long, as before

static void bootDone(lv_timer_t *timer)
{
  bootTimer = NULL; // repeat count 1: LVGL deletes the timer itself after this callback
  if (pageCount)
  {
    uint32_t fadeMs = settings.bootFadeMs; // 0.24.28: adjustable (web UI > Display), was a fixed 400 ms
    // 0.30.17: a nav card that came in while the logo was up is put in place now, before the fade, so the logo fades
    // into the card (with the page under it hidden, as when it's up) instead of the card popping up over the logo
    if (navWaitingForBoot && navBox && !navDismissed)
    {
      lv_obj_remove_flag(navBox, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(navBox);
      navHidePageUnder(pages[activePage].scr);
    }
    navWaitingForBoot = false;
    showScreen(pages[activePage].scr, LV_SCR_LOAD_ANIM_FADE_IN, fadeMs, 0, true);
  }
  bootScr = NULL;
  bootRingArc = NULL; // 0.30.11: deleted with the boot screen
  bootDialArc = NULL;
  if (logoBuf)
  {
    heap_caps_free(logoBuf);
    logoBuf = NULL;
  }
  if (dialBuf)
  {
    heap_caps_free(dialBuf);
    dialBuf = NULL;
  }
}

static void arcAnimCb(void *obj, int32_t value)
{
  lv_arc_set_value((lv_obj_t *)obj, value);
}

// 0.30.11: start the ring's fill, alreadyMs into it (0 = from the start; more when the logo jumps ahead to line up
// with another gauge's; 0.30.14: below 0 = wait that long first)
static void bootRingAnim(int32_t alreadyMs)
{
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, bootRingArc);
  lv_anim_set_values(&anim, 0, 100);
  lv_anim_set_duration(&anim, BOOT_SWEEP_MS);
  lv_anim_set_exec_cb(&anim, arcAnimCb);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
  anim.act_time = alreadyMs; // LVGL's own "time into the animation" (a public field): start this far in
  lv_anim_start(&anim);
}

// the boot screen's sweep: a ring round the edge of the glass that fills 0 -> 100 % in 3 s, like a cluster's needles
// at ignition on. The text logos (OpenHaldex, Forbes text, your own text) have always had it; 0.24.29 the can2gauge logo
// too, drawn over the picture (the ring sits outside the logo's own dial and wordmark).
static void bootSweepRing()
{
  lv_obj_t *arc = lv_arc_create(bootScr);
  lv_obj_set_size(arc, 440, 440);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, colour(palette.panel), LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, accentColor(), LV_PART_INDICATOR);
  bootRingArc = arc;
  bootRingAnim(0);
}

// 0.25.2: the can2gauge logo's own dial sweeps instead of a ring round the edge. The logo file has the dial empty (track
// and unlit ticks, tools/logo_can2gauge.py, drawn 1:1 with logo_convert.py --exact); these numbers must match its
// dial(240, 200, 122).
#define BOOT_DIAL_X 240       // the dial's centre on the glass, px
#define BOOT_DIAL_Y 200       // (40 px above the middle: the wordmark sits underneath)
#define BOOT_DIAL_R 122       // radius to the middle of the sweep's band, px
#define BOOT_DIAL_WIDTH 16    // width of the band, px
#define BOOT_DIAL_TICKS 11    // tick marks round the dial
#define BOOT_DIAL_FULL 1000   // the sweep's range (0-1000 rather than 0-100 so it moves smoothly)
#define BOOT_DIAL_REST 720    // where it settles after the sweep: 72 %, as the logo was drawn before 0.25.2
#define BOOT_DIAL_TEAL 0x24D1C4  // the logo's colours: teal at the start of the sweep ...
#define BOOT_DIAL_AMBER 0xFFB02E // ... into amber at the top end
static lv_obj_t *bootDialTicks[BOOT_DIAL_TICKS];             // the lit (white) tick marks, shown as the sweep passes
static lv_point_precise_t bootDialTickPoints[BOOT_DIAL_TICKS][2]; // each tick's two ends (LVGL keeps a pointer to them)

// the sweep's colours, as a square picture centred on the dial that LVGL paints the band with: teal for the first
// 35 % of the sweep, blending into amber by 72 % (where it settles, so it rests looking like the old logo), amber past
// that like a rev counter's red line. Coloured by the angle round the dial, so it stays amber right to the top end.
static bool makeDialColours()
{
  const int size = 2 * (BOOT_DIAL_R + BOOT_DIAL_WIDTH / 2); // just covers the band
  dialBuf = (uint8_t *)heap_caps_malloc(size * size * 2, MALLOC_CAP_SPIRAM);
  if (dialBuf == NULL)
  {
    return false;
  }
  uint16_t *pixels = (uint16_t *)dialBuf; // the buffer read as RGB565 pixels
  const float centre = size / 2.0f;
  const lv_color_t teal = lv_color_hex(BOOT_DIAL_TEAL);
  const lv_color_t amber = lv_color_hex(BOOT_DIAL_AMBER);
  for (int y = 0; y < size; y++)
  {
    for (int x = 0; x < size; x++)
    {
      // the pixel's angle round the dial, in degrees, turned into how far round the sweep it is (0 = start, 1 = end)
      float angle = atan2f(y - centre, x - centre) * 180.0f / (float)M_PI; // -180 .. 180, 0 = 3 o'clock
      float sweepPart = fmodf(angle - 135.0f + 720.0f, 360.0f) / 270.0f;    // the sweep starts at 135 (bottom left)
      if (sweepPart > 1.0f)
      {
        // in the gap at the bottom (only the round end caps reach it): the start's half counts as 0, the end's as 1
        if (sweepPart > 1.0f + 45.0f / 270.0f)
        {
          sweepPart = 0.0f;
        }
        else
        {
          sweepPart = 1.0f;
        }
      }
      float amberPart = (sweepPart - 0.35f) / 0.37f; // 0 at 35 % round, 1 at 72 %
      if (amberPart < 0.0f)
      {
        amberPart = 0.0f;
      }
      if (amberPart > 1.0f)
      {
        amberPart = 1.0f;
      }
      const lv_color_t pixelColour = lv_color_mix(amber, teal, (uint8_t)(amberPart * 255.0f)); // 255 = all amber
      pixels[y * size + x] = lv_color_to_u16(pixelColour);
    }
  }
  memset(&dialDsc, 0, sizeof(dialDsc));
  dialDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dialDsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dialDsc.header.w = size;
  dialDsc.header.h = size;
  dialDsc.header.stride = size * 2;
  dialDsc.data_size = size * size * 2;
  dialDsc.data = dialBuf;
  return true;
}

// runs every frame of the sweep: move the band, and light each tick once the band has reached it
static void bootDialAnimCb(void *obj, int32_t value)
{
  lv_arc_set_value((lv_obj_t *)obj, value);
  for (uint8_t i = 0; i < BOOT_DIAL_TICKS; i++)
  {
    const int32_t tickAt = (int32_t)i * BOOT_DIAL_FULL / (BOOT_DIAL_TICKS - 1); // where tick i sits on the 0-1000 scale
    if (value >= tickAt)
    {
      lv_obj_remove_flag(bootDialTicks[i], LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
      lv_obj_add_flag(bootDialTicks[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// 0.30.11: start the dial's sweep up and back down, alreadyMs into it (0 = from the start; more when the logo jumps
// ahead to line up with another gauge's; 0.30.14: below 0 = wait that long first)
static void bootDialAnims(int32_t alreadyMs)
{
  // up to full in 1.7 s ...
  lv_anim_t sweepUp;
  lv_anim_init(&sweepUp);
  lv_anim_set_var(&sweepUp, bootDialArc);
  lv_anim_set_values(&sweepUp, 0, BOOT_DIAL_FULL);
  lv_anim_set_duration(&sweepUp, BOOT_DIAL_UP_MS);
  lv_anim_set_exec_cb(&sweepUp, bootDialAnimCb);
  lv_anim_set_path_cb(&sweepUp, lv_anim_path_ease_in_out);
  sweepUp.act_time = alreadyMs; // LVGL's own "time into the animation": start this far in (below 0 = a wait)
  lv_anim_start(&sweepUp);
  // ... then back down to 72 % in 1.3 s (waits for the first; early apply off so it doesn't jump to full at the start)
  lv_anim_t sweepDown;
  lv_anim_init(&sweepDown);
  lv_anim_set_var(&sweepDown, bootDialArc);
  lv_anim_set_values(&sweepDown, BOOT_DIAL_FULL, BOOT_DIAL_REST);
  lv_anim_set_duration(&sweepDown, BOOT_DIAL_DOWN_MS);
  lv_anim_set_early_apply(&sweepDown, false);
  lv_anim_set_exec_cb(&sweepDown, bootDialAnimCb);
  lv_anim_set_path_cb(&sweepDown, lv_anim_path_ease_out);
  sweepDown.act_time = alreadyMs - BOOT_DIAL_UP_MS; // below 0 = still waiting for the first (a delay)
  lv_anim_start(&sweepDown);
}

// the can2gauge logo's dial: sweeps up to full like a cluster's needles at ignition on, then drops back to 72 %
static void bootSweepDial()
{
  if (!makeDialColours())
  {
    bootSweepRing(); // no room for the colours: the old ring round the edge instead
    return;
  }
  const int size = 2 * (BOOT_DIAL_R + BOOT_DIAL_WIDTH / 2);
  bootDialArc = lv_arc_create(bootScr);
  lv_obj_set_size(bootDialArc, size, size);
  lv_obj_set_pos(bootDialArc, BOOT_DIAL_X - size / 2, BOOT_DIAL_Y - size / 2);
  lv_obj_set_style_pad_all(bootDialArc, 0, LV_PART_MAIN); // no padding, so the band lands exactly on the logo's track
  lv_obj_set_style_pad_all(bootDialArc, 0, LV_PART_INDICATOR);
  lv_arc_set_rotation(bootDialArc, 135); // starts bottom-left, like the logo's track
  lv_arc_set_bg_angles(bootDialArc, 0, 270);
  lv_arc_set_range(bootDialArc, 0, BOOT_DIAL_FULL);
  lv_arc_set_value(bootDialArc, 0);
  lv_obj_remove_style(bootDialArc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(bootDialArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_opa(bootDialArc, LV_OPA_TRANSP, LV_PART_MAIN); // the track is in the logo picture already
  lv_obj_set_style_arc_width(bootDialArc, BOOT_DIAL_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(bootDialArc, true, LV_PART_INDICATOR); // round ends, as in the logo
  lv_obj_set_style_arc_color(bootDialArc, lv_color_hex(BOOT_DIAL_TEAL), LV_PART_INDICATOR); // if the picture can't be used
  lv_obj_set_style_arc_image_src(bootDialArc, &dialDsc, LV_PART_INDICATOR); // painted with the teal-to-amber picture

  // the lit ticks, white over the logo's grey ones (same places and sizes as tools/logo_can2gauge.py)
  const float innerRadius = BOOT_DIAL_R - BOOT_DIAL_WIDTH * 1.55f;
  const float outerRadius = BOOT_DIAL_R - BOOT_DIAL_WIDTH * 0.95f;
  for (uint8_t i = 0; i < BOOT_DIAL_TICKS; i++)
  {
    const float angle = (135.0f + 270.0f * i / (BOOT_DIAL_TICKS - 1)) * (float)M_PI / 180.0f; // radians
    bootDialTickPoints[i][0].x = BOOT_DIAL_X + innerRadius * cosf(angle);
    bootDialTickPoints[i][0].y = BOOT_DIAL_Y + innerRadius * sinf(angle);
    bootDialTickPoints[i][1].x = BOOT_DIAL_X + outerRadius * cosf(angle);
    bootDialTickPoints[i][1].y = BOOT_DIAL_Y + outerRadius * sinf(angle);
    bootDialTicks[i] = lv_line_create(bootScr);
    lv_line_set_points(bootDialTicks[i], bootDialTickPoints[i], 2);
    lv_obj_set_pos(bootDialTicks[i], 0, 0); // the points are glass coordinates
    uint8_t tickWidth = 3;
    if (i % 5 == 0)
    {
      tickWidth = 4; // every fifth tick (start, middle, end) is thicker
    }
    lv_obj_set_style_line_width(bootDialTicks[i], tickWidth, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(bootDialTicks[i], true, LV_PART_MAIN);
    lv_obj_set_style_line_color(bootDialTicks[i], lv_color_hex(0xF2F4F8), LV_PART_MAIN); // the logo's white
    lv_obj_add_flag(bootDialTicks[i], LV_OBJ_FLAG_HIDDEN); // lit by bootDialAnimCb as the sweep passes
  }

  bootDialAnims(0);
}

// "C2GL" logo packets: c < 0x80 -> c + 1 literal pixels follow (2 bytes each, LE); c >= 0x80 -> the next pixel
// repeated (c & 0x7F) + 2 times. Written by tools/logo_convert.py and the web UI converter.
static bool rleUnpack(const uint8_t *src, size_t len, uint16_t *dst, size_t pixelCount)
{
  size_t in = 0;
  size_t out = 0;
  while (in < len && out < pixelCount)
  {
    const uint8_t packet = src[in++];
    if (packet & 0x80)
    {
      if (in + 2 > len)
      {
        return false;
      }
      const uint16_t pixel = src[in] | (src[in + 1] << 8);
      in += 2;
      for (size_t count = (packet & 0x7F) + 2; count && out < pixelCount; count--)
      {
        dst[out++] = pixel;
      }
    }
    else
    {
      size_t count = (size_t)packet + 1;
      if (in + count * 2 > len)
      {
        return false;
      }
      for (; count && out < pixelCount; count--, in += 2)
      {
        dst[out++] = src[in] | (src[in + 1] << 8);
      }
    }
  }
  return out == pixelCount;
}

// draws a logo file centred on the boot screen. Two formats:
//   raw   exactly 460800 bytes = 480x480 RGB565 LE (the original /logo.bin)
//   C2GL  16-byte header "C2GL" ver(1) flags(bit0 RLE) w(u16) h(u16) reserved(u16) dataLen(u32), then RGB565 LE
//         pixels (RLE-packed when flags bit0), up to 480x480; logos are drawn on black so the file has no alpha
// The pixels are unpacked once into PSRAM (freed in bootDone): LVGL's own heap is too small and has no PNG decoder.
// 0.24.0: the make logos ship zipped (/logos/<key>.bin.gz, tools/fs_pack.py). fsReadAll() unzips one into PSRAM as it
// is read - a few milliseconds - so no unpacked copy is kept on the filesystem; a plain file (/logo.bin, an older image)
// still reads as it is.
static bool loadLogo(const char *path)
{
  size_t size = 0;
  uint8_t *file = fsReadAll(path, &size); // the whole file, unzipped if need be
  if (!file)
  {
    return false;
  }
  uint16_t w = 480;
  uint16_t h = 480;
  bool ok = false;
  const size_t headerBytes = 16;
  if (size == 480 * 480 * 2)
  {
    logoBuf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    ok = logoBuf != NULL;
    if (ok)
    {
      memcpy(logoBuf, file, size);
    }
  }
  else if (size > headerBytes && !memcmp(file, "C2GL", 4) && file[4] == 1)
  {
    const uint8_t *header = file; // the 16-byte header at the start of the file
    w = header[6] | (header[7] << 8);
    h = header[8] | (header[9] << 8);
    const size_t dataLen = header[12] | (header[13] << 8) | ((uint32_t)header[14] << 16) | ((uint32_t)header[15] << 24);
    const size_t pixelCount = (size_t)w * h;
    if (w && h && w <= 480 && h <= 480 && dataLen == size - headerBytes)
    {
      logoBuf = (uint8_t *)heap_caps_malloc(pixelCount * 2, MALLOC_CAP_SPIRAM);
      if (!(header[5] & 1))
      {
        ok = logoBuf && dataLen == pixelCount * 2;
        if (ok)
        {
          memcpy(logoBuf, file + headerBytes, dataLen);
        }
      }
      else
      {
        ok = logoBuf && rleUnpack(file + headerBytes, dataLen, (uint16_t *)logoBuf, pixelCount);
      }
    }
  }

  heap_caps_free(file);
  if (!ok)
  {
    DEBUG("logo %s: unreadable (%u bytes)", path, (unsigned)size);
    if (logoBuf)
    {
      heap_caps_free(logoBuf);
    }
    logoBuf = NULL;
    return false;
  }
  memset(&logoDsc, 0, sizeof(logoDsc));
  logoDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  logoDsc.header.cf = LV_COLOR_FORMAT_RGB565;
  logoDsc.header.w = w;
  logoDsc.header.h = h;
  logoDsc.header.stride = w * 2;
  logoDsc.data_size = (uint32_t)w * h * 2;
  logoDsc.data = logoBuf;
  lv_obj_t *image = lv_image_create(bootScr);
  lv_image_set_src(image, &logoDsc);
  lv_obj_center(image);
  return true;
}

void uiShowBootLogo()
{
  if (settings.bootLogo == 0)
  {
    return;
  }
  if (!lvglLock(200))
  {
    return;
  }
  bootScr = newScreen();
  bool drawn = false;
  bool hasSweep = false; // 0.25.3: a ring or dial sweep was started (the logo then stays up until it has finished)
  CarChoice myCar; // 0.36.2: My Car, for its name when boot logo 7 has no picture yet
  const char *makeKey = settings.bootLogo == 5 ? themeLogoKey() : NULL; // chosen make / VIN make / theme's make
  if (settings.bootLogo == 1)
  {
    drawn = loadLogo("/logos/forbes.bin"); // the text version below if it isn't on the gauge
  }
  else if (settings.bootLogo == 6)
  {
    drawn = loadLogo("/logos/can2gauge.bin"); // tools/logo_can2gauge.py
  }
  else if (settings.bootLogo == 4)
  {
    drawn = loadLogo("/logo.bin");
  }
  else if (settings.bootLogo == 7)
  {
    drawn = loadLogo(CAR_LOGO_PATH); // 0.36.2: My Car in its paint, drawn by the web UI
  }
  else if (makeKey)
  {
    char path[32];
    snprintf(path, sizeof(path), "/logos/%s.bin", makeKey);
    drawn = loadLogo(path);
  }

  if (drawn && settings.bootLogo == 6)
  {
    bootSweepDial(); // 0.25.2: the logo's own dial sweeps (0.24.29 had a ring round the edge of the glass)
    hasSweep = true;
  }

  if (!drawn)
  {
    bootSweepRing();
    hasSweep = true;
    const char *line1 = "FORBES";
    const char *line2 = "AUTOMOTIVE";
    const char *line3 = "can2gauge";
    if (settings.bootLogo == 2)
    {
      line1 = "OPEN";
      line2 = "HALDEX";
      line3 = "for enthusiasts - by enthusiasts";
    }
    else if (settings.bootLogo == 6)
    {
      line1 = "can2gauge";
      line2 = "";
      line3 = "by Forbes Automotive";
    }
    else if (settings.bootLogo == 3)
    {
      line1 = settings.bootText;
      line2 = "";
      line3 = "can2gauge";
    }
    else if (settings.bootLogo == 7)
    { // 0.36.2: My Car's picture isn't on the gauge yet (the web UI draws it when it's next open): the car's name
      vehicleCarCopy(myCar);
      if (myCar.confirmed != CAR_NOT_SET)
      {
        line1 = myCar.make;  // big: "SEAT"
        line2 = myCar.model; // under it: "Ibiza IV" ("" when the model isn't known)
      }
      else
      {
        line1 = "can2gauge"; // no car saved yet
        line2 = "";
      }
      line3 = "can2gauge";
    }
    else if (makeKey && logoName(makeKey))
    { // make logo chosen but /logos/<key>.bin not uploaded yet: the make's name in the theme colours
      line1 = logoName(makeKey);
      line2 = "";
      line3 = "can2gauge";
    }
    label(bootScr, &lv_font_montserrat_48, palette.text, LV_ALIGN_CENTER, 0, -40, line1);
    label(bootScr, &lv_font_montserrat_32, palette.accent, LV_ALIGN_CENTER, 0, 15, line2);
    label(bootScr, &lv_font_montserrat_16, palette.muted, LV_ALIGN_CENTER, 0, 70, line3);
    label(bootScr, &lv_font_montserrat_12, palette.muted, LV_ALIGN_BOTTOM_MID, 0, -50, "v" CAN2GAUGE_VERSION);
  }
  lv_screen_load(bootScr);
  // how long the logo stays up before the fade to the first page
  uint32_t logoMs = BOOT_NO_SWEEP_MS;
  if (hasSweep)
  {
    logoMs = BOOT_SWEEP_MS + BOOT_HOLD_MS; // the sweep, then the full ring / dial held a moment
  }
  bootTimer = lv_timer_create(bootDone, logoMs, NULL);
  lv_timer_set_repeat_count(bootTimer, 1);
  bootStartTick = lv_tick_get(); // 0.30.11: the boot sync counts from here
  bootLogoMs = logoMs;
  lvglUnlock();
}

static void bootLogoMoveTo(int32_t targetMs); // 0.30.14: below (written out after its first use)

// 0.30.11: how far into the boot logo this gauge is, in ms. false = no boot logo up (gone, or none chosen).
// 0.30.12: no LVGL lock here. A gauge still starting up can hold the lock a while, and a missed lock used to read as
// "the logo has gone", which ended the boot sync on the slowest gauge - the one that needed it most. Only reads.
bool uiBootLogoClock(uint32_t &elapsedMs)
{
  elapsedMs = 0;
  const bool isUp = bootScr != NULL && bootTimer != NULL;
  if (isUp)
  {
    // 0.30.14: the start can be set a moment ahead (the handshake): read as a signed number so "not yet" is 0
    const int32_t sinceStartMs = (int32_t)(lv_tick_get() - bootStartTick);
    if (sinceStartMs > 0)
    {
      elapsedMs = (uint32_t)sinceStartMs;
    }
  }
  return isUp;
}

// 0.30.11: another gauge's logo went up earlier: jump this one forward to the same point (sweep and fade timer), so
// the row sweeps together. Only ever forward, so the first gauge up sets the time for the rest.
// 0.30.13: given WHEN the other gauge's logo went up (millis() on this gauge), not where to jump to. The point is
// worked out only once the lock is ours: waiting for the lock (the LVGL task holds it while it draws a frame) used to
// make every jump land that much short - Adam: the slow gauge stayed a steady ~100 ms behind.
void uiBootLogoCatchUp(uint32_t otherStartMs)
{
  if (!lvglLock(200))
  {
    return; // busy: the boot sync tries again 20 ms later
  }
  if (bootScr == NULL || bootTimer == NULL)
  {
    lvglUnlock();
    return; // the logo has gone already
  }
  const int32_t targetMs = (int32_t)(millis() - otherStartMs);      // how long the other gauge's logo has been up, now
  const int32_t elapsedMs = (int32_t)(lv_tick_get() - bootStartTick); // ... and ours
  if (targetMs <= elapsedMs)
  {
    lvglUnlock();
    return; // already there (or ahead)
  }
  bootLogoMoveTo(targetMs);
  lvglUnlock();
}

// 0.30.14: the boot sync's handshake - the logo was built with the glass dark while the gauges waited for each other.
// Set it to start again from the beginning at startAtMs (millis(), a moment from now, agreed with the other gauges) and
// draw that first frame now, so the backlight only has to come on at startAtMs. Worked out once the lock is ours, so
// waiting for it doesn't make this gauge late.
void uiBootLogoRestart(uint32_t startAtMs)
{
  if (!lvglLock(500))
  {
    return; // the logo just carries on where it is (the catch-up still lines it up)
  }
  if (bootScr == NULL || bootTimer == NULL)
  {
    lvglUnlock();
    return; // the logo has gone already
  }
  int32_t untilStartMs = (int32_t)(startAtMs - millis());
  if (untilStartMs < 0)
  {
    untilStartMs = 0; // already late: start now
  }
  bootLogoMoveTo(-untilStartMs); // a minus point = wait that long, then start from the beginning
  lv_refr_now(NULL); // NULL = every display: draws the first frame now, so the backlight shows it
  lvglUnlock();
}

// 0.30.14: put the boot logo's sweep and fade timer at targetMs into the logo (forward or back; below 0 = start that
// long from now). The LVGL lock must be held by the caller.
static void bootLogoMoveTo(int32_t targetMs)
{
  if (targetMs >= (int32_t)bootLogoMs)
  {
    targetMs = bootLogoMs - 1; // never past the end: the fade still happens through bootDone
  }
  // the sweep, restarted at the new point
  if (bootRingArc)
  {
    lv_anim_delete(bootRingArc, NULL); // NULL = every animation on it
    bootRingAnim(targetMs);
  }
  if (bootDialArc)
  {
    lv_anim_delete(bootDialArc, NULL);
    bootDialAnims(targetMs);
  }
  // the fade to the first page, moved to match
  lv_timer_delete(bootTimer);
  bootTimer = lv_timer_create(bootDone, (uint32_t)((int32_t)bootLogoMs - targetMs), NULL);
  lv_timer_set_repeat_count(bootTimer, 1);
  bootStartTick = lv_tick_get() - targetMs; // so the boot logo clock reads the new point (minus a minus = later)
}

// ---------------------------------------------------------------------------
// screensaver, ignition off (shutdown -> sleep -> dark standby) and wake (src/can2gauge_Power.cpp), docs/mascot/MASCOT.md
// ---------------------------------------------------------------------------
// The screensaver comes up over a gauge page once the car has been still (speed under 2 km/h, or no speed signal) and
// the glass untouched for settings.saverDelayS. It goes at the first touch, moving off, an alert, a page change, or the
// rpm / Haldex lock moving (MASCOT.md: more than 300 rpm / 10 %).
// These screens own their Sprite (the screen's user data), freed when LVGL deletes the screen. A screen that is replaced
// is "retired" and deleted only once it is neither showing nor fading out, since until then it still draws from the
// sprite's frame buffer.
#define SAVER_MOVING_KMH 2.0f
#define SAVER_TOUCH_MS 300
#define SAVER_RPM_JUMP 300.0f
#define SAVER_LOCK_JUMP 10.0f
#define SHUTDOWN_MAX_MS 12000
static lv_obj_t *saverScr = NULL; // the screensaver
static lv_obj_t *saverFrom = NULL; // the screen it came up over (a gauge page, a menu screen ...): it goes back there
static bool saverLeavePending = false; // leaving the screensaver / Rev Runner is waiting for the LVGL task (leaveSaver)
static lv_obj_t *darkScr = NULL;  // shutdown + sleep, the dark standby screen, the wake clip
static lv_obj_t *retiredScreens[4] = {};
static bool standbyOn = false;    // ignition off: the glass is dark and nothing refreshes
static bool waking = false;       // the wake clip is playing on darkScr
// The row wave (0.12.0, docs/SPRITES.md "A row of gauges"): before the shutdown clip, the gauges in a row greet each
// other in turns from the left. Turn 0 is the first and second gauge, turn 1 the second and third, and so on. In its
// turn a gauge plays the pack's row clip toward its neighbour (looks at it and waves); the rest of the time it watches,
// holding the last frame of the clip that looks toward the turn being played. Every gauge counts the turns on its own
// clock from its own ignition off, which all the gauges in the car see at the same moment.
#define ROW_MAX_TURN_MS 4000      // a pack's row clip longer than this is cut to it (keeps a long row's goodbye sensible)
#define ROW_WAVE_LEFT 0           // what this gauge is doing in the row wave: waving to the gauge on its left
#define ROW_WAVE_RIGHT 1          // waving to the gauge on its right
#define ROW_WATCH_LEFT 2          // watching the wave further along to its left
#define ROW_WATCH_RIGHT 3         // watching the wave further along to its right
static bool rowWaveOn = false;    // the row wave is playing on darkScr
static uint8_t rowPlace = 0;      // this gauge's place in the row, 0 = leftmost
static uint8_t rowGauges = 0;     // gauges in the row, this one included
static uint32_t rowStartMs = 0;   // lv_tick_get() when the row wave started
static uint32_t rowTurnMs = 0;    // how long one turn lasts (the longer of the two row clips)
static int8_t rowDoing = -1;      // ROW_WAVE_* / ROW_WATCH_* now (-1 = nothing yet), so each clip is started only once
static uint32_t activityMs = 0;   // last touch / movement / page change / alert
static float saverRpm = 0;  // the readings when the screensaver came up
static float saverLock = 0;
// RUNNER.md: a double-tap on the screensaver starts Rev Runner. A single tap still leaves it, but only once no second
// tap can come (it holds the exit 400 ms), and Rev answers it with a "?" meanwhile.
#define SAVER_DOUBLE_MS 380
#define SAVER_EXIT_HOLD_MS 400
static lv_obj_t *gameScr = NULL; // Rev Runner
static lv_obj_t *saverQuestion = NULL;  // the "?"
static bool saverTapped = false;
static uint32_t saverTapMs = 0;

static bool carMoving()
{
  return signalValid(SIG_SPEED) && signalGet(SIG_SPEED) >= SAVER_MOVING_KMH;
}

static Sprite *spriteOf(lv_obj_t *scr)
{
  return scr ? (Sprite *)lv_obj_get_user_data(scr) : NULL;
}

static void spriteScreenDeleted(lv_event_t *event)
{
  lv_obj_t *scr = (lv_obj_t *)lv_event_get_current_target(event);
  spriteClose(spriteOf(scr));
  lv_obj_set_user_data(scr, NULL);
  if (scr == saverScr)
  {
    saverScr = NULL;
  }
  if (scr == darkScr)
  {
    darkScr = NULL;
  }
}

// a black screen with pack `pack` on it (NULL = none; "" = the first pack on the gauge)
static lv_obj_t *spriteScreen(const char *pack)
{
  lv_obj_t *scr = newScreen();
  lv_obj_set_user_data(scr, pack ? spriteOpen(scr, pack) : NULL);
  lv_obj_add_event_cb(scr, spriteScreenDeleted, LV_EVENT_DELETE, NULL);
  return scr;
}

static void retireScreen(lv_obj_t *&scr)
{
  if (!scr)
  {
    return;
  }
  for (lv_obj_t *&slot : retiredScreens)
  {
    if (!slot)
    {
      slot = scr;
      scr = NULL;
      return;
    }
  }
  lv_obj_delete_async(scr); // never expected: four transitions inside one fade
  scr = NULL;
}

static void sweepRetiredScreens()
{
  lv_obj_t *active = lv_screen_active();
  lv_obj_t *previous = lv_display_get_screen_prev(lv_display_get_default());
  for (lv_obj_t *&slot : retiredScreens)
  {
    if (slot && slot != active && slot != previous)
    {
      lv_obj_delete(slot);
      slot = NULL;
    }
  }
  for (uint8_t pageIndex = 0; pageIndex < pageCount; pageIndex++) // a sprite page off the glass gives its frame buffers back (~900 kB for Rev)
  {
    if (pages[pageIndex].sprite && pages[pageIndex].scr != active && pages[pageIndex].scr != previous)
    {
      spriteSleep(pages[pageIndex].sprite);
    }
  }
}

static SpriteContext makeSpriteContext(uint8_t mode)
{
  SpriteContext context = {};
  context.mode = mode;
  context.react = settings.saverReact != 0;
  context.accent = palette.accent;
  return context;
}

static void gameOpen()
{
  lv_obj_t *scr = runnerOpen(settings.saverSprite, palette.accent);
  if (!scr)
  {
    return; // no pack: the screensaver carries on
  }
  retireScreen(saverScr);
  gameScr = scr;
  showScreen(gameScr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
}

static void saverTapCb(lv_event_t *)
{
  const uint32_t now = lv_tick_get();
  if (saverTapped && now - saverTapMs < SAVER_DOUBLE_MS)
  {
    saverTapped = false;
    gameOpen();
    return;
  }
  saverTapped = true; // one tap: leaves once no second one comes
  saverTapMs = now;
  if (saverQuestion)
  {
    lv_obj_remove_flag(saverQuestion, LV_OBJ_FLAG_HIDDEN);
  }
}

// 0.39.5: may the screensaver come up over this screen? A gauge page, a menu screen (CAN, Navigation ...), Haldex,
// Gearbox, Settings, and the drag timer when it isn't timing. Not the menu itself: it goes back by itself after 10 s.
static bool saverCanCover(lv_obj_t *screen)
{
  if (screen == NULL)
  {
    return false;
  }
  if (pageCount && screen == pages[activePage].scr)
  {
    return true;
  }
  if (screen == modeWidgets.scr || screen == haldexWidgets.scr || screen == dsgScr || screen == settingsScr ||
      screen == infoScr)
  {
    return true;
  }
  if (dragScr && screen == dragScr && !dragIsTiming())
  {
    return true;
  }
  return false;
}

// where the screensaver (or Rev Runner) goes back to: the screen it came up over if that's still there, else the gauge
// page (NULL = no pages)
static lv_obj_t *saverBackScreen()
{
  if (saverFrom != NULL && (saverFrom == modeWidgets.scr || saverFrom == haldexWidgets.scr || saverFrom == dsgScr ||
                            saverFrom == dragScr || saverFrom == settingsScr || saverFrom == infoScr))
  {
    return saverFrom;
  }
  if (pageCount)
  {
    return pages[activePage].scr;
  }
  return NULL;
}

// Leaving the screensaver or Rev Runner, done by the LVGL task (uiTick asks with lv_async_call): going back slides in
// a whole screen, and a menu screen needs more stack than uiTick has (see menuIdleClose).
static void leaveSaver(void *unused)
{
  (void)unused;
  saverLeavePending = false;
  lv_obj_t *active = lv_screen_active();
  const bool onSaver = saverScr != NULL && active == saverScr;
  const bool onGame = gameScr != NULL && active == gameScr;
  if (!onSaver && !onGame)
  {
    return; // gone already (a page change, standby, a number to show ...)
  }
  activityMs = lv_tick_get();
  saverTapped = false;
  saverQuestion = NULL;
  lv_obj_t *back = saverBackScreen();
  saverFrom = NULL;
  if (back)
  {
    showScreen(back, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
  }
  retireScreen(saverScr);
  retireScreen(gameScr);
}

// from uiTick: ask the LVGL task to leave the screensaver / Rev Runner (once)
static void askLeaveSaver()
{
  if (!saverLeavePending)
  {
    saverLeavePending = true;
    lv_async_call(leaveSaver, NULL);
  }
}

// 0.29.1: the Screensaver switch when the saver came up. Before, a saver shown by hand ignored the switch completely,
// so switching it off from on did nothing; now switching it OFF while it's up always takes it away.
static bool saverWasOnAtOpen = false;

static void saverOpen()
{
  lv_obj_t *scr = spriteScreen(settings.saverSprite);
  Sprite *sprite = spriteOf(scr);
  if (!sprite)
  { // no pack on the gauge: nothing to show
    lv_obj_delete(scr);
    return;
  }
  spriteUpdate(sprite, makeSpriteContext(SPR_SAVER)); // the first frame, before it fades in
  lv_obj_add_event_cb(scr, saverTapCb, LV_EVENT_PRESSED, NULL);
  saverQuestion = label(scr, &lv_font_montserrat_48, palette.accent, LV_ALIGN_CENTER, 90, -60, "?");
  lv_obj_set_style_transform_pivot_x(saverQuestion, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(saverQuestion, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(saverQuestion, 60 * 256 / 48, 0);
  lv_obj_add_flag(saverQuestion, LV_OBJ_FLAG_HIDDEN);
  saverTapped = false;
  retireScreen(saverScr);
  saverScr = scr;
  saverRpm = signalGet(SIG_RPM);
  saverLock = signalGet(SIG_HALDEX_ACTUAL);
  saverWasOnAtOpen = settings.saverEnabled != 0; // 0.29.1: switching it off later takes the saver away
  showScreen(saverScr, LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, false);
}

// 0.24.12: the saver was put up from the web UI (Show the Screensaver Now), so it stays even with the screensaver off
static bool saverShownByHand = false;

static void saverRefresh()
{
  const bool rpmMoved = signalValid(SIG_RPM) && fabsf(signalGet(SIG_RPM) - saverRpm) > SAVER_RPM_JUMP;
  const bool lockMoved = signalValid(SIG_HALDEX_ACTUAL) && fabsf(signalGet(SIG_HALDEX_ACTUAL) - saverLock) > SAVER_LOCK_JUMP;
  const bool tapped = saverTapped && lv_tick_elaps(saverTapMs) > SAVER_EXIT_HOLD_MS; // a single tap, no second one
  bool switchedOff = false; // turned off in the web UI: leave now
  if (!settings.saverEnabled && !saverShownByHand)
  {
    switchedOff = true; // 0.24.12: the saver's own timer put it up
  }
  if (!settings.saverEnabled && saverWasOnAtOpen)
  {
    switchedOff = true; // 0.29.1: it was on when the saver came up and has been switched off since
  }
  if (tapped || carMoving() || runtime.alertActive || rpmMoved || lockMoved || switchedOff || numberShowing())
  {
    askLeaveSaver(); // back to the screen it came up over (the LVGL task does it)
    return;
  }
  if (saverFrom != NULL && saverFrom == modeWidgets.scr)
  {
    modeRefresh(false); // a menu screen underneath keeps its source (the K-line session, the adapter) open
  }
  if (pageCount)
  {
    pageClaims(pages[activePage]); // the page underneath keeps its sources (and the speed) coming
    if (settings.pages[pages[activePage].pageCfgIndex].style == STYLE_OIL)
    {
      runtime.oilPageSeenMs = millis() | 1;
    }
  }
  spriteUpdate(spriteOf(saverScr), makeSpriteContext(SPR_SAVER));
}

// a fresh dark screen on the glass, playing the shutdown pack's `role` (then `thenRole`): how long one pass takes
static uint32_t darkOpen(int role, uint8_t thenRole, uint32_t fadeMs)
{
  lv_obj_t *scr = spriteScreen(role >= 0 && settings.shutdownSprite[0] ? settings.shutdownSprite : NULL);
  Sprite *sprite = spriteOf(scr);
  uint32_t ms = 0;
  if (sprite && spritePlay(sprite, (uint8_t)role, thenRole))
  {
    ms = spriteRoleMs(sprite, (uint8_t)role) + (thenRole != 0xFF ? spriteRoleMs(sprite, thenRole) : 0);
  }
  retireScreen(saverScr);
  retireScreen(gameScr);
  retireScreen(darkScr);
  darkScr = scr;
  waking = false;
  rowWaveOn = false; // a new dark screen (standby, the wake clip) ends any row wave
  lv_screen_load_anim(darkScr, LV_SCR_LOAD_ANIM_FADE_IN, fadeMs, 0, false);
  return ms;
}

// start one of the pack's row clips: played once now (wave), or just its last frame held (watch)
static void rowStartClip(Sprite *sprite, uint8_t role, bool wave)
{
  if (wave)
  {
    spritePlay(sprite, role); // looks at the neighbour and waves, then holds its last frame
    return;
  }
  const int8_t clip = spriteRoleClip(sprite, role);
  const uint16_t lastFrame = spriteClipFrames(sprite, clip) - 1;
  spriteClip(sprite, clip, lastFrame, lastFrame, 0, false); // looking that way, arm down, held (speed 0)
}

// the row wave, from darkRefresh: which turn is playing, and what this gauge does in it
static void rowWaveStep(Sprite *sprite)
{
  const uint32_t turn = lv_tick_elaps(rowStartMs) / rowTurnMs; // turn n is between the gauges at places n and n + 1
  const uint32_t turnCount = rowGauges - 1;                    // one turn for each pair of neighbours
  if (turn >= turnCount)
  {
    // every pair has waved: all the gauges play the normal goodbye together (it starts with a wave to the driver)
    rowWaveOn = false;
    spritePlay(sprite, C2GS_SHUTDOWN, C2GS_SLEEP);
    return;
  }

  int8_t doing;
  if (turn + 1 == rowPlace)
  {
    doing = ROW_WAVE_LEFT; // our turn with the gauge on our left
  }
  else if (turn == rowPlace)
  {
    doing = ROW_WAVE_RIGHT; // our turn with the gauge on our right
  }
  else if (turn + 1 < rowPlace)
  {
    doing = ROW_WATCH_LEFT; // the wave hasn't reached us yet: look left toward it
  }
  else
  {
    doing = ROW_WATCH_RIGHT; // the wave has gone past us: look right after it
  }
  if (doing == rowDoing)
  {
    return; // already doing it
  }
  rowDoing = doing;

  switch (doing)
  {
  case ROW_WAVE_LEFT:
    rowStartClip(sprite, C2GS_ROW_LEFT, true);
    break;
  case ROW_WAVE_RIGHT:
    rowStartClip(sprite, C2GS_ROW_RIGHT, true);
    break;
  case ROW_WATCH_LEFT:
    rowStartClip(sprite, C2GS_ROW_LEFT, false);
    break;
  default:
    rowStartClip(sprite, C2GS_ROW_RIGHT, false);
    break;
  }
}

// Rev Runner: leaves on a long press or the game-over screen idling 20 s (the game says), moving off or an alert
static void gameRefresh()
{
  if (runnerWantsOut() || carMoving() || runtime.alertActive)
  {
    askLeaveSaver(); // back to the screen the screensaver came up over (the LVGL task does it)
    return;
  }
  if (pageCount)
  {
    pageClaims(pages[activePage]);
  }
  runnerTick(palette.accent, nightMode);
}

// 0.29.1: Show Numbers came in while the screensaver or Rev Runner was up: back to the gauge page so the number shows
// over it (the shutdown / standby screens are left alone). The caller holds the LVGL lock.
static void leaveSaverForNumber()
{
  lv_obj_t *active = lv_screen_active();
  const bool onSaver = saverScr != NULL && active == saverScr;
  const bool onGame = gameScr != NULL && active == gameScr;
  if (!onSaver && !onGame)
  {
    return; // on a page, the menu or a dark screen: nothing to leave
  }
  activityMs = lv_tick_get(); // the screensaver's clock starts again from now
  saverTapped = false;
  saverQuestion = NULL;
  saverFrom = NULL; // the number shows over the gauge page, whatever the saver came up over
  if (pageCount)
  {
    showScreen(pages[activePage].scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, false); // straight to the page, under the number
  }
  retireScreen(saverScr);
  retireScreen(gameScr);
}

static void darkRefresh()
{
  Sprite *sprite = spriteOf(darkScr);
  if (rowWaveOn)
  {
    rowWaveStep(sprite); // the row wave picks the clip; spriteUpdate below plays it
  }
  spriteUpdate(sprite, makeSpriteContext(SPR_DARK));
  if (waking && spriteDone(sprite)) // the wake clip has played: the gauge
  {
    waking = false;
    menuOpen = false; // back on the gauge page, whatever screen the ignition went off on
    if (pageCount)
    {
      showScreen(pages[activePage].scr, LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, false);
    }
    retireScreen(darkScr);
  }
}

uint32_t uiShutdown(uint8_t placeFromLeft, uint8_t gaugesInRow)
{
  if (!lvglLock(200))
  {
    return 0;
  }
  // MASCOT.md: the shutdown clip once, then one pass of the sleep loop; the caller fades the backlight while it loops
  uint32_t ms = darkOpen(C2GS_SHUTDOWN, C2GS_SLEEP, 300) + 300;
  if (ms > SHUTDOWN_MAX_MS)
  {
    ms = SHUTDOWN_MAX_MS;
  }

  // the row wave first, when this gauge is in a row and its pack has both row clips
  Sprite *sprite = spriteOf(darkScr);
  const bool hasRowClips = spriteRoleClip(sprite, C2GS_ROW_LEFT) >= 0 && spriteRoleClip(sprite, C2GS_ROW_RIGHT) >= 0;
  if (sprite && gaugesInRow >= 2 && hasRowClips)
  {
    rowTurnMs = spriteRoleMs(sprite, C2GS_ROW_LEFT);
    if (spriteRoleMs(sprite, C2GS_ROW_RIGHT) > rowTurnMs)
    {
      rowTurnMs = spriteRoleMs(sprite, C2GS_ROW_RIGHT);
    }
    if (rowTurnMs > ROW_MAX_TURN_MS)
    {
      rowTurnMs = ROW_MAX_TURN_MS;
    }
    if (rowTurnMs < 500)
    {
      rowTurnMs = 500; // never a zero-length turn (it divides the time)
    }
    rowPlace = placeFromLeft;
    rowGauges = gaugesInRow;
    rowStartMs = lv_tick_get();
    rowDoing = -1;
    rowWaveOn = true;
    rowWaveStep(sprite);                      // the first pose now, while the screen fades in
    ms = ms + rowTurnMs * (gaugesInRow - 1);  // the turns come before the shutdown clip
  }
  lvglUnlock();
  return ms < 700 ? 700 : ms; // no sprite: the fade to black
}

void uiStandby(bool on)
{
  if (!lvglLock(200))
  {
    return;
  }
  if (on)
  {
    darkOpen(-1, 0xFF, 0); // plain black: nothing left to draw, and the pack's memory goes with the shutdown screen
    standbyOn = true;
  }
  else
  {
    standbyOn = false;
    activityMs = lv_tick_get();
    // the wake clip (if the pack has one), then the gauge - always from darkRefresh, in the uiTick task. 0.30.2: with no
    // shutdown sprite this used to fade straight to the page here, in the caller's task (the io task, powerTick). The
    // cross-fade draws whole screens in the task that asks for it, and the io task's 3 KB stack ran out: a crash at
    // every ignition on (coredump 2026-10-30: io task, uiStandby -> showScreen -> lvglFadeTo -> lv_refr_now). With no
    // clip, spriteDone() is true at once, so darkRefresh fades to the page on its next pass (~30 ms).
    darkOpen(C2GS_WAKE, 0xFF, 0);
    waking = true;
  }
  lvglUnlock();
}

void uiSaverShow()
{
  if (!lvglLock(200))
  {
    return;
  }
  lv_obj_t *active = lv_screen_active();
  if (!standbyOn && saverCanCover(active)) // 0.39.5: over any screen it may cover, not only a gauge page
  {
    saverFrom = active;
    saverOpen();
    saverShownByHand = true; // 0.24.12: stays up even with the screensaver switched off
  }
  lvglUnlock();
}

bool uiSaverActive()
{
  return saverScr != NULL;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
static void destroyPages()
{
  lvglFinishScreenAnimation(); // LVGL mustn't be sliding a page that is about to go
  for (uint8_t pageIndex = 0; pageIndex < pageCount; pageIndex++)
  {
    if (pages[pageIndex].scr)
    {
      lv_obj_delete(pages[pageIndex].scr);
    }
    pages[pageIndex].scr = NULL;
    spriteClose(pages[pageIndex].sprite); // after its screen: the image went with it
    pages[pageIndex].sprite = NULL;
  }
  pageCount = 0;
}

static void createPages()
{
  destroyPages();
  for (uint8_t pageCfgIndex = 0; pageCfgIndex < MAX_PAGES; pageCfgIndex++)
  {
    if (settings.pages[pageCfgIndex].enabled)
    {
      pageCount++;
    }
  }
  if (pageCount == 0)
  { // never leave the user with nothing: force page 0 on
    settings.pages[0].enabled = 1;
    pageCount = 1;
  }
  uint8_t built = 0;
  for (uint8_t pageCfgIndex = 0; pageCfgIndex < MAX_PAGES && built < pageCount; pageCfgIndex++)
  {
    if (settings.pages[pageCfgIndex].enabled)
    {
      buildPage(built++, pageCfgIndex);
    }
  }
  if (activePage >= pageCount)
  {
    activePage = 0;
  }
}

bool uiInit(const UiCallbacks &uiCallbacks)
{
  callbacks = uiCallbacks;
  uiSetTheme(settings.theme);
  if (!lvglLock(500))
  {
    return false;
  }
  lv_indev_set_long_press_time(lvglTouch(), settings.longPressMs ? settings.longPressMs : 600);
  buildAlert();
  buildMenu();
  buildSettings();
  createPages();
  activePage = settings.startPage < pageCount ? settings.startPage : 0;
  lv_screen_load(pages[activePage].scr);
  activityMs = lv_tick_get();
  lvglUnlock();
  return true;
}

void uiRebuildPages()
{
  if (!lvglLock(500))
  {
    return;
  }
  lvglFinishScreenAnimation(); // first: a page change LVGL is sliding ends now, so the screen showing is settled
  // the page itself is on the screen (asked of LVGL: a flag could be left over from before a standby)
  const bool wasVisible = pageCount > 0 && activePage < pageCount && lv_screen_active() == pages[activePage].scr;
  createPages();
  if (wasVisible)
  {
    lv_screen_load(pages[activePage].scr);
  }
  lvglUnlock();
}

static void infoRefresh()
{
  if (!infoScr || lv_screen_active() != infoScr)
  {
    return;
  }
  EXT_RAM_BSS_ATTR static char buf[512];
  buf[0] = '\0';
  if (infoProviders[infoPage])
  {
    infoProviders[infoPage](buf, sizeof(buf));
  }
  else
  {
    strcpy(buf, "no data source registered");
  }
  labelSet(infoLabel, buf);
}

// ---------------------------------------------------------------------------
// frame-rate figures (uiPerf) and the benchmark
// ---------------------------------------------------------------------------
static UiPerf perfStats = {};
static uint32_t tickSumUs = 0;
static uint32_t tickCount = 0;
static uint32_t tickPeakUs = 0;
static uint32_t perfStampMs = 0;
static uint32_t benchStartMs = 0;
static uint32_t benchLastSeq = 0;
static uint32_t benchSamples = 0;
static uint32_t benchFpsSum = 0;
static uint32_t benchFrameSum = 0;
static uint32_t benchFramePeakUs = 0;
static uint32_t benchFlushSum = 0;
static uint32_t benchBusySum = 0;
static uint32_t benchDecodeSum = 0;

const UiPerf &uiPerf()
{
  return perfStats;
}

void uiBenchmark()
{
  if (perfStats.benchRunning)
  {
    return;
  }
  uint8_t item = 0;
  const uint8_t kind = uiScreenKind(&item);
  static const char *kinds[] = {"boot", "page", "menu", "mode", "haldex", "dsg", "settings", "other", "saver", "dark"};
  if (kind == UI_SCR_PAGE)
  {
    snprintf(perfStats.benchScreen, sizeof(perfStats.benchScreen), "page %u%s", item + 1,
             settings.pages[item].spriteOn ? " sprite" : "");
  }
  else
  {
    copyStr(perfStats.benchScreen, sizeof(perfStats.benchScreen), kinds[kind < 10 ? kind : 7]);
  }
  benchDecodeSum = 0;
  benchBusySum = 0;
  benchFlushSum = 0;
  benchFramePeakUs = 0;
  benchFrameSum = 0;
  benchFpsSum = 0;
  benchSamples = 0;
  benchStartMs = millis();
  benchLastSeq = lvglPerf().seq;
  perfStats.benchRunning = true;
  lvglForceRedraw(true);
  DEBUG("perf: benchmark on %s", perfStats.benchScreen);
}

static void perfTick(uint32_t tickUs)
{
  tickSumUs += tickUs;
  tickCount++;
  tickPeakUs = tickUs > tickPeakUs ? tickUs : tickPeakUs;
  const uint32_t now = millis();
  if (now - perfStampMs >= 1000)
  {
    perfStats.tickAvgUs = tickCount ? tickSumUs / tickCount : 0;
    perfStats.tickMaxUs = tickPeakUs;
    uint32_t decodeCount;
    uint32_t decodeTotalUs;
    uint32_t decodeMaxUs;
    uint32_t unzipTotalUs;
    spriteDecodeStats(decodeCount, decodeTotalUs, decodeMaxUs, unzipTotalUs);
    perfStats.decodes = decodeCount;
    perfStats.decodeAvgUs = decodeCount ? decodeTotalUs / decodeCount : 0;
    perfStats.unzipAvgUs = 0; // 0.24.7: the unzip part of one sprite frame
    if (decodeCount)
    {
      perfStats.unzipAvgUs = unzipTotalUs / decodeCount;
    }
    perfStats.decodeMaxUs = decodeMaxUs;
    tickPeakUs = 0;
    tickCount = 0;
    tickSumUs = 0;
    perfStampMs = now;
  }

  if (!perfStats.benchRunning)
  {
    return;
  }
  const LvglPerf &lvglStats = lvglPerf();
  if (lvglStats.seq != benchLastSeq) // a new whole second of the driver's figures
  {
    benchLastSeq = lvglStats.seq;
    if (now - benchStartMs > 1500) // skip the first second (the redraw ramping up)
    {
      benchSamples++;
      benchFpsSum += lvglStats.fps;
      benchFrameSum += lvglStats.frameAvgUs;
      benchFramePeakUs = lvglStats.frameMaxUs > benchFramePeakUs ? lvglStats.frameMaxUs : benchFramePeakUs;
      benchFlushSum += lvglStats.flushUs;
      benchBusySum += lvglStats.busyPct;
      benchDecodeSum += perfStats.decodeAvgUs;
    }
  }
  if (now - benchStartMs >= UI_BENCH_MS)
  {
    lvglForceRedraw(false);
    const uint32_t samples = benchSamples ? benchSamples : 1;
    perfStats.benchFps = benchFpsSum / samples;
    perfStats.benchFrameAvgUs = benchFrameSum / samples;
    perfStats.benchFrameMaxUs = benchFramePeakUs;
    perfStats.benchFlushUs = benchFlushSum / samples;
    perfStats.benchBusyPct = (uint8_t)(benchBusySum / samples);
    perfStats.benchDecodeAvgUs = benchDecodeSum / samples;
    perfStats.benchMs = now | 1;
    perfStats.benchRunning = false;
    DEBUG("perf: benchmark %s: %lu fps, frame %lu us avg / %lu max, copy %lu us/s, LVGL %u%% busy, sprite decode %lu us",
          perfStats.benchScreen, (unsigned long)perfStats.benchFps, (unsigned long)perfStats.benchFrameAvgUs,
          (unsigned long)perfStats.benchFrameMaxUs, (unsigned long)perfStats.benchFlushUs, perfStats.benchBusyPct,
          (unsigned long)perfStats.benchDecodeAvgUs);
  }
}

void uiTick()
{
  const uint32_t tickStartUs = micros();
  if (!lvglLock(10))
  {
    return;
  }
  sweepRetiredScreens();
  navArrowTick(); // 0.30.1: read a stored turn arrow the nav card asked for
  if (alertClearPending) // 0.24.13: a Clear Alerts that couldn't get the lock earlier
  {
    clearAlertLocked();
  }
  if (pageRangesPending) // 0.36.4: an adc2can's dial range changed: the pages again, here where the lock is already held
  {
    pageRangesPending = false;
    lvglFinishScreenAnimation(); // as uiRebuildPages: a page change LVGL is sliding ends first
    const bool wasVisible = pageCount > 0 && activePage < pageCount && lv_screen_active() == pages[activePage].scr; // as uiRebuildPages
    createPages();
    if (wasVisible)
    {
      lv_screen_load(pages[activePage].scr);
    }
  }
  if (bootScr || standbyOn)
  {
    lvglUnlock();
    perfTick(micros() - tickStartUs); // the benchmark still ends
    return;
  }
  sampleGraphs(); // 0.31.0: Graph pages keep their last 30 s, shown or not
  lv_obj_t *active = lv_screen_active();
  if (lv_display_get_inactive_time(NULL) < SAVER_TOUCH_MS || carMoving() || runtime.alertActive)
  {
    activityMs = lv_tick_get();
  }
  if (saverScr && active == saverScr)
  {
    saverRefresh();
  }
  else if (gameScr && active == gameScr)
  {
    gameRefresh();
  }
  else if (darkScr && active == darkScr)
  {
    darkRefresh();
  }
  else if (settings.saverEnabled && lv_tick_elaps(activityMs) > settings.saverDelayS * 1000UL &&
           !lv_display_get_screen_prev(lv_display_get_default()) && !numberShowing() && // 0.29.1: not over a number
           saverCanCover(active)) // 0.39.5: over any screen it may cover (it was a gauge page only), and back there after
  {
    saverShownByHand = false; // 0.24.12: the screensaver's own timer - it leaves if switched off
    saverFrom = active;
    saverOpen(); // the car is still and nobody has touched the glass
  }
  else if (pageCount && active == pages[activePage].scr)
  {
    refreshPage(pages[activePage]);
    refreshSourceSpinner(pages[activePage]); // 0.15.0: after the page has written its note line
  }
  else if (active == menuScr)
  {
    if (lv_tick_elaps(menuTouchMs) > MENU_IDLE_MS)
    {
      // 10 s idle: back to the gauge (or the screen the menu came from), done by the LVGL task (menuIdleClose)
      if (!menuClosePending)
      {
        menuClosePending = true;
        lv_async_call(menuIdleClose, NULL);
      }
    }
    else
    {
      menuStatusRefresh();
    }
  }
  else if (active == infoScr)
  {
    infoRefresh();
  }
  else if (dsgScr && active == dsgScr)
  {
    refreshGear(dsgWidgets);
  }
  else if (dragScr && active == dragScr)
  {
    dragTick(); // 0.38.0: the drag timer's countdown, clock and ring
  }
  else if (modeWidgets.scr && active == modeWidgets.scr)
  {
    modeRefresh(false);
  }
  else if (haldexWidgets.scr && active == haldexWidgets.scr)
  {
    refreshHaldexMode();
  }
  else if (active == settingsScr)
  {
    lv_obj_t *wifi = (lv_obj_t *)lv_obj_get_user_data(settingsScr);
    if (wifi)
    {
      labelFormat(wifi, "WiFi AP: %s\n%s\nv%s  %d fps", settings.apSsid,
                  runtime.wifiStaConnected ? runtime.wifiStaIP : "http://192.168.1.1", CAN2GAUGE_VERSION, (int)lvglFps());
    }
  }
  runtime.activePage = activePage;
  lvglUnlock();
  perfTick(micros() - tickStartUs);
}

static void loadPage(uint8_t pageIndex, lv_screen_load_anim_t anim)
{
  if (pageIndex >= pageCount)
  {
    return;
  }
  activePage = pageIndex;
  activityMs = lv_tick_get();
  if (standbyOn || darkScr) // ignition off: the page shows on wake
  {
    return;
  }
  retireScreen(saverScr); // it fades out under the page
  retireScreen(gameScr);
  menuOpen = false;
  if (settings.startPage != pageIndex) // the gauge comes back on the last page shown (saved by the EEP task, rate-limited)
  {
    settings.startPage = pageIndex;
    settingsDirty = true;
  }
  uint32_t ms = settings.swipeAnimMs ? settings.swipeAnimMs : 800;
  showScreen(pages[pageIndex].scr, anim, ms, 0, false);
  if (callbacks.onPageChanged)
  {
    callbacks.onPageChanged(pageIndex);
  }
}

void uiNextPage()
{
  if (!lvglLock(50))
  {
    return;
  }
  loadPage((activePage + 1) % pageCount, LV_SCR_LOAD_ANIM_MOVE_LEFT);
  lvglUnlock();
}

void uiPrevPage()
{
  if (!lvglLock(50))
  {
    return;
  }
  loadPage((activePage + pageCount - 1) % pageCount, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
  lvglUnlock();
}

void uiGotoPage(uint8_t index)
{
  if (!lvglLock(50))
  {
    return;
  }
  loadPage(index, LV_SCR_LOAD_ANIM_FADE_IN);
  lvglUnlock();
}

uint8_t uiActivePage()
{
  return activePage;
}

void uiOpenMenu()
{
  if (!lvglLock(50))
  {
    return;
  }
  lv_obj_t *from = lv_screen_active();
  if (standbyOn || (darkScr && from == darkScr))
  {
    lvglUnlock();
    return;
  }
  if ((saverScr && from == saverScr) || (gameScr && from == gameScr))
  {
    retireScreen(saverScr);
    retireScreen(gameScr);
    from = pages[activePage].scr;
  }
  if (from != menuScr && from != settingsScr && from != infoScr && from != bootScr)
  {
    menuFrom = from;
  }
  menuOpen = true;
  menuTouchMs = lv_tick_get();
  menuFill(0); // fresh status, no slide
  showScreen(menuScr, LV_SCR_LOAD_ANIM_OVER_TOP, 250, 0, false);
  lvglUnlock();
}

void uiCloseMenu()
{
  if (!lvglLock(50))
  {
    return;
  }
  // back to where the menu was opened from, if that screen still exists; otherwise the gauge page
  lv_obj_t *to = pages[activePage].scr;
  bool isPage = true;
  if (menuFrom && (menuFrom == modeWidgets.scr || menuFrom == haldexWidgets.scr || menuFrom == dsgScr ||
                   menuFrom == dragScr)) // 0.38.0: + the drag timer
  {
    to = menuFrom;
    isPage = false;
  }
  menuOpen = !isPage;
  showScreen(to, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 250, 0, false);
  lvglUnlock();
}

uint8_t uiScreenKind(uint8_t *item)
{
  if (!lvglReady())
  {
    if (item)
    {
      *item = 0;
    }
    return UI_SCR_OTHER; // the display didn't come up: no screens to ask about
  }
  lv_obj_t *active = lv_screen_active();
  uint8_t kind = UI_SCR_OTHER;
  uint8_t shownItem = 0;
  if (bootScr && active == bootScr)
  {
    kind = UI_SCR_BOOT;
  }
  else if (saverScr && active == saverScr)
  {
    kind = UI_SCR_SAVER;
  }
  else if (gameScr && active == gameScr)
  {
    kind = UI_SCR_GAME;
  }
  else if (standbyOn || (darkScr && active == darkScr))
  {
    kind = UI_SCR_DARK;
  }
  else if (active == menuScr)
  {
    kind = UI_SCR_MENU;
  }
  else if (modeWidgets.scr && active == modeWidgets.scr)
  {
    kind = UI_SCR_MODE;
    shownItem = modeItem;
  }
  else if (haldexWidgets.scr && active == haldexWidgets.scr)
  {
    kind = UI_SCR_HALDEX;
  }
  else if (dsgScr && active == dsgScr)
  {
    kind = UI_SCR_DSG;
  }
  else if (dragScr && active == dragScr)
  {
    kind = UI_SCR_DRAG; // 0.38.0
  }
  else if (active == settingsScr)
  {
    kind = UI_SCR_SETTINGS;
  }
  else if (pageCount && active == pages[activePage].scr)
  {
    kind = UI_SCR_PAGE;
    shownItem = pages[activePage].pageCfgIndex;
  }
  if (item)
  {
    *item = shownItem;
  }
  return kind;
}

bool uiMenuOpen()
{
  return menuOpen;
}

bool uiAlert(const char *text, bool latch)
{
  if (!lvglLock(50))
  {
    return false; // 0.32.0: the screen was busy - the caller tries again
  }
  lv_label_set_text(alertLabel, text);
  lv_obj_remove_flag(alertBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(alertGlow, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_border_opa(alertGlow, ALERT_GLOW_RING_OPA, 0); // 0.24.15: steady while the alert is up
  lv_obj_set_style_bg_opa(alertGlow, ALERT_GLOW_WASH_OPA, 0);
  if (latch)
  {
    lv_obj_remove_flag(alertHint, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    lv_obj_add_flag(alertHint, LV_OBJ_FLAG_HIDDEN);
  }
  if (!alertTimer)
  {
    alertPhase = 0;
    alertTimer = lv_timer_create(alertPulse, 50, NULL);
  }
  alertLatched = latch;
  alertClearPending = false; // 0.24.13: a new alert wins over a clear still waiting
  runtime.alertActive = true;
  copyStr(runtime.alertText, sizeof(runtime.alertText), text);
  lvglUnlock();
  return true;
}

// the alert taken off the glass (caller holds the LVGL lock)
static void clearAlertLocked()
{
  alertClearPending = false;
  lv_obj_add_flag(alertBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(alertGlow, LV_OBJ_FLAG_HIDDEN);
  if (alertTimer)
  {
    lv_timer_delete(alertTimer);
    alertTimer = NULL;
  }
  alertLatched = false;
  runtime.alertActive = false;
  runtime.alertText[0] = '\0';
}

void uiClearAlert()
{
  if (!lvglLock(50))
  {
    alertClearPending = true; // LVGL is busy drawing: uiTick clears it on its next pass
    return;
  }
  clearAlertLocked();
  lvglUnlock();
}

void uiSetInfoProvider(uiInfoPage_t page, uiInfoProvider_t provider)
{
  if (page < UI_INFO_COUNT)
  {
    infoProviders[page] = provider;
  }
}

void uiSetTheme(uint8_t theme)
{
  const uint8_t effective = themeEffective(theme); // Match car -> matched theme / Custom
  if (effective < THEME_CUSTOM)
  {
    palette = palettes[effective];
    return;
  }
  uint32_t dial, needle;
  themeColours(effective, dial, needle);
  palette = paletteFrom(dial, needle);
}

// colours are baked into the LVGL objects when a screen is built, so a new theme means rebuilding them: gauge pages,
// menu and settings now; the info / DSG screens are recreated next time they open (if one is showing, the menu is
// shown instead). Safe from any task and from LVGL callbacks (the lock is recursive).
void uiApplyTheme()
{
  if (!lvglLock(500))
  {
    return;
  }
  lvglFinishScreenAnimation(); // every screen is built again: LVGL mustn't be sliding one of the old ones
  uiSetTheme(settings.theme);
  lv_obj_t *active = lv_screen_active();
  uiRebuildPages();
  uiRebuildMenu();
  lv_obj_t *oldSettings = settingsScr;
  buildSettings();
  if (active == oldSettings)
  {
    lv_screen_load(settingsScr);
  }
  if (oldSettings)
  {
    lv_obj_delete_async(oldSettings);
  }
  if (dragScr) // 0.38.0: the drag timer is built again in the new colours when it's next opened
  {
    if (active == dragScr)
    {
      lv_screen_load(menuScr);
      menuOpen = true;
      menuTouchMs = lv_tick_get(); // a fresh menu: its idle time-out starts now, not from the last touch
    }
    lv_obj_delete_async(dragScr);
    dragScr = NULL;
    dragClosed();
  }
  for (lv_obj_t **screenPtr : {&infoScr, &dsgScr, &modeWidgets.scr, &haldexWidgets.scr})
  {
    if (*screenPtr)
    {
      if (active == *screenPtr)
      {
        lv_screen_load(menuScr);
        menuOpen = true;
        menuTouchMs = lv_tick_get(); // a fresh menu: its idle time-out starts now, not from the last touch
      }
      lv_obj_delete_async(*screenPtr);
      *screenPtr = NULL;
    }
  }
  infoLabel = NULL;
  if (alertBox)
  {
    lv_obj_set_style_bg_color(alertBox, lv_color_darken(colour(palette.danger), 40), 0);
  }
  if (alertGlow)
  {
    lv_obj_set_style_bg_color(alertGlow, colour(palette.danger), 0);
    lv_obj_set_style_border_color(alertGlow, colour(palette.danger), 0);
  }
  if (progressArc)
  {
    lv_obj_set_style_arc_color(progressArc, colour(palette.panel), LV_PART_MAIN);
  }
  lvglUnlock();
}

void uiSetNight(bool night)
{
  nightMode = night;
}
