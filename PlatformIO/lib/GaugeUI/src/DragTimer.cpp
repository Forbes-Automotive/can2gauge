#include "DragTimer.h"
#include "GaugeUI.h"          // uiOpenMenu(): a long press goes back to the menu
#include <can2gauge_defs.h>   // settings (the speed row's unit), runtime
#include <can2gauge_signals.h> // the speed (SIG_SPEED), and how long ago it last came
#include <can2gauge_Units.h>  // UNIT_IMPERIAL: the speed row shows mph
#include <can2gauge_Drag.h>   // the scoreboard

// ---------------------------------------------------------------------------
// Drag timer (0.38.0), see DragTimer.h. The screen, 480 x 480 round, from the top:
//   the title ("0-100 KM/H"), the flag, the big number (countdown / clock / time), the speed, a status line and the best
//   time, with the speed ring round the edge. The scoreboard is a second layer on the same screen, shown on a
//   double-tap.
// ---------------------------------------------------------------------------

#define DRAG_TARGET_KMH 100.0f     // 0-100 km/h
#define DRAG_TARGET_MPH_KMH 96.5606f // 0-60 mph, in km/h (the speed signal is always km/h)
#define KMH_TO_MPH 0.621371f       // km/h -> mph, for showing the speed
#define DRAG_COUNT_MS 5000         // the countdown: 5, 4, 3, 2, 1, then the flag drops
#define DRAG_MOVING_KMH 1.5f       // faster than this = the car is moving (the flag drops early, the clock starts)
#define DRAG_STILL_KMH 2.0f        // a run starts only from (nearly) standing still
#define DRAG_MAX_RUN_MS 60000      // a run not finished by then stops by itself
#define DRAG_SPEED_LOST_MS 2000    // no speed reading for this long during a run = it stops ("Lost the speed")
#define DRAG_DOUBLE_MS 380         // a second tap this soon after the first is a double-tap (as on the screensaver)
#define DRAG_CLEAR_CONFIRM_MS 3000 // Clear Scores: the second tap must come within this long
#define DRAG_RING_WIDTH 16         // the speed ring round the edge
#define FLAG_Y (-100)              // where the flag sits (from the glass centre, up is minus)
#define SAMPLE_SAME_MS 3           // two readings timed this close together are the same reading

enum dragState_t : uint8_t
{
  DRAG_READY = 0, // waiting: tap to start
  DRAG_COUNTING,  // the countdown under the raised flag
  DRAG_RUNNING,   // the flag has dropped: the clock runs until the target speed
  DRAG_DONE,      // the target was reached: the time shows under the chequered flag
};

// everything on the screen, and the run
static lv_obj_t *dragScreen = NULL;
static lv_obj_t *ring = NULL;          // the speed ring round the edge
static lv_obj_t *titleLabel = NULL;    // "0-100 KM/H"
static lv_obj_t *startFlag = NULL;     // the plain (green) flag: raised for the countdown, dropped at the start
static lv_obj_t *finishFlag = NULL;    // the chequered flag: lifts at the target speed
static lv_obj_t *bigLabel = NULL;      // the countdown digit, the running clock or the final time
static lv_obj_t *speedLabel = NULL;    // the speed now, or what the big number is
static lv_obj_t *statusLabel = NULL;   // "Tap to Start", "GO!", "New Best!" ...
static lv_obj_t *bestLabel = NULL;     // "Best 4.87 s   Double-tap: Scores"
static lv_obj_t *boardBox = NULL;      // the scoreboard layer (hidden until a double-tap)
static lv_obj_t *boardTitle = NULL;
static lv_obj_t *boardList = NULL;     // the times, one a line
static lv_obj_t *clearButton = NULL;
static lv_obj_t *clearLabel = NULL;
static DragColours shades;             // the theme's colours

static uint8_t state = DRAG_READY;
static bool boardShown = false;
static bool tapWaiting = false;        // a tap came: a single one unless a second follows within DRAG_DOUBLE_MS
static uint32_t tapMs = 0;
static uint32_t clearAskedMs = 0;      // Clear Scores tapped once at this time (0 = not asked)
static uint32_t countStartMs = 0;      // the countdown started
static uint32_t runStartMs = 0;        // the clock started (the flag dropped, or the car moved)
static uint8_t runKind = DRAG_KIND_KMH; // what this run times (DRAG_KIND_KMH / MPH)
static float runTargetKmh = DRAG_TARGET_KMH;
// the two latest speed readings and when each arrived (millis), to work out the moment the target was passed
static float lastSpeed = 0;
static uint32_t lastSampleMs = 0;
static float prevSpeed = 0;
static uint32_t prevSampleMs = 0;

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

// a filled box that ignores touches (they go to the screen)
static lv_obj_t *plainBox(lv_obj_t *parent, int32_t width, int32_t height, uint32_t colour)
{
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(box, width, height);
  lv_obj_set_style_bg_color(box, lv_color_hex(colour), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  return box;
}

static lv_obj_t *textLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t colour, int32_t y, const char *text)
{
  lv_obj_t *labelObj = lv_label_create(parent);
  lv_obj_set_style_text_font(labelObj, font, 0);
  lv_obj_set_style_text_color(labelObj, lv_color_hex(colour), 0);
  lv_obj_set_style_text_align(labelObj, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(labelObj, text);
  lv_obj_align(labelObj, LV_ALIGN_CENTER, 0, y);
  return labelObj;
}

// a flag on its pole. chequered = the finish flag (4 x 3 black and white squares), else the plain green start flag
static lv_obj_t *makeFlag(lv_obj_t *parent, bool chequered)
{
  lv_obj_t *flag = lv_obj_create(parent);
  lv_obj_remove_style_all(flag);
  lv_obj_remove_flag(flag, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(flag, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(flag, 80, 76);
  lv_obj_align(flag, LV_ALIGN_CENTER, 0, FLAG_Y);
  lv_obj_t *pole = plainBox(flag, 5, 76, shades.text);
  lv_obj_set_pos(pole, 0, 0);
  if (!chequered)
  {
    lv_obj_t *cloth = plainBox(flag, 72, 48, shades.ok);
    lv_obj_set_pos(cloth, 7, 2);
    return flag;
  }
  for (uint8_t row = 0; row < 3; row++)
  {
    for (uint8_t column = 0; column < 4; column++)
    {
      uint32_t squareColour = shades.text; // white square
      if ((row + column) % 2 == 0)
      {
        squareColour = 0x000000; // black square
      }
      lv_obj_t *square = plainBox(flag, 18, 16, squareColour);
      lv_obj_set_pos(square, 7 + column * 18, 2 + row * 16);
    }
  }
  return flag;
}

// one step of the start flag dropping (progress 0..100): it slides down and fades out
static void flagDropStep(void *object, int32_t progress)
{
  lv_obj_t *flag = (lv_obj_t *)object;
  lv_obj_set_style_translate_y(flag, progress * 60 / 100, 0);
  lv_obj_set_style_opa(flag, (lv_opa_t)(255 - progress * 255 / 100), 0);
}

// one step of the chequered flag lifting (progress 0..100): it rises into place and fades in
static void flagLiftStep(void *object, int32_t progress)
{
  lv_obj_t *flag = (lv_obj_t *)object;
  lv_obj_set_style_translate_y(flag, 40 - progress * 40 / 100, 0);
  lv_obj_set_style_opa(flag, (lv_opa_t)(progress * 255 / 100), 0);
}

static void animateFlag(lv_obj_t *flag, lv_anim_exec_xcb_t step, uint32_t ms)
{
  lv_anim_delete(flag, NULL); // a move still running on it stops first
  lv_obj_remove_flag(flag, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, flag);
  lv_anim_set_values(&anim, 0, 100);
  lv_anim_set_duration(&anim, ms);
  lv_anim_set_exec_cb(&anim, step);
  lv_anim_start(&anim);
}

// a flag straight up in its place (shown = true) or gone
static void setFlag(lv_obj_t *flag, bool shown)
{
  lv_anim_delete(flag, NULL);
  lv_obj_set_style_translate_y(flag, 0, 0);
  lv_obj_set_style_opa(flag, LV_OPA_COVER, 0);
  if (shown)
  {
    lv_obj_remove_flag(flag, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    lv_obj_add_flag(flag, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---------------------------------------------------------------------------
// what the run times, and what the screen says
// ---------------------------------------------------------------------------

// the speed row's unit picks the run: mph -> 0-60 mph, else 0-100 km/h
static uint8_t kindNow()
{
  if (settings.unitSpeed == UNIT_IMPERIAL)
  {
    return DRAG_KIND_MPH;
  }
  return DRAG_KIND_KMH;
}

static float targetKmhOf(uint8_t kind)
{
  if (kind == DRAG_KIND_MPH)
  {
    return DRAG_TARGET_MPH_KMH;
  }
  return DRAG_TARGET_KMH;
}

// a speed in km/h as this kind shows it, with its unit
static float shownSpeed(uint8_t kind, float kmh, const char **unit)
{
  if (kind == DRAG_KIND_MPH)
  {
    *unit = "mph";
    return kmh * KMH_TO_MPH;
  }
  *unit = "km/h";
  return kmh;
}

static const char *kindTitle(uint8_t kind)
{
  if (kind == DRAG_KIND_MPH)
  {
    return "0-60 MPH";
  }
  return "0-100 KM/H";
}

// "4.87" from 4870 ms
static void timeText(uint32_t ms, char *out, size_t outLen)
{
  snprintf(out, outLen, "%lu.%02lu", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 10));
}

static void setStatus(const char *text, uint32_t colour)
{
  lv_label_set_text(statusLabel, text);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(colour), 0);
}

// the bottom line: the best time of this kind
static void showBest(uint8_t kind)
{
  if (dragScoreCount(kind) == 0)
  {
    lv_label_set_text(bestLabel, "No runs yet  -  Double-tap: Scores");
    return;
  }
  char best[12];
  timeText(dragScore(kind, 0).timeMs, best, sizeof(best));
  lv_label_set_text_fmt(bestLabel, "Best %s s  -  Double-tap: Scores", best);
}

// "1st", "2nd", "3rd", "4th" ...
static const char *placeEnding(uint8_t place)
{
  if (place == 1)
  {
    return "st";
  }
  if (place == 2)
  {
    return "nd";
  }
  if (place == 3)
  {
    return "rd";
  }
  return "th";
}

// back to waiting, the screen as it starts (message = the status line, NULL = "Tap to Start")
static void goReady(const char *message)
{
  state = DRAG_READY;
  runKind = kindNow();
  runTargetKmh = targetKmhOf(runKind);
  lv_label_set_text(titleLabel, kindTitle(runKind));
  setFlag(startFlag, false);
  setFlag(finishFlag, false);
  lv_arc_set_value(ring, 0);
  lv_label_set_text(bigLabel, "0.00");
  lv_obj_set_style_text_color(bigLabel, lv_color_hex(shades.text), 0);
  if (message)
  {
    setStatus(message, shades.accent2);
  }
  else
  {
    setStatus("Tap to Start", shades.accent);
  }
  showBest(runKind);
}

// the flag drops: the clock runs from startMs (millis)
static void goRunning(uint32_t startMs)
{
  state = DRAG_RUNNING;
  runStartMs = startMs;
  animateFlag(startFlag, flagDropStep, 300);
  setStatus("GO!", shades.ok);
}

// the target speed was passed at finishMs (millis): the time goes on the scoreboard
static void goDone(uint32_t finishMs)
{
  state = DRAG_DONE;
  uint32_t runMs = 0;
  if (finishMs > runStartMs)
  {
    runMs = finishMs - runStartMs;
  }
  const uint8_t place = dragAddRun(runKind, runMs);
  char text[12];
  timeText(runMs, text, sizeof(text));
  lv_label_set_text(bigLabel, text);
  lv_obj_set_style_text_color(bigLabel, lv_color_hex(shades.accent), 0);
  lv_label_set_text(speedLabel, "seconds");
  lv_arc_set_value(ring, 1000);
  setFlag(startFlag, false);
  lv_obj_set_style_opa(finishFlag, LV_OPA_TRANSP, 0);
  animateFlag(finishFlag, flagLiftStep, 400);
  if (place == 1)
  {
    setStatus("New Best!", shades.ok);
  }
  else if (place > 0)
  {
    char placeText[24];
    snprintf(placeText, sizeof(placeText), "%u%s on the Board", place, placeEnding(place));
    setStatus(placeText, shades.accent);
  }
  else
  {
    setStatus("Not on the Board", shades.muted);
  }
  showBest(runKind);
}

// ---------------------------------------------------------------------------
// the scoreboard layer
// ---------------------------------------------------------------------------
static void boardFill()
{
  const uint8_t kind = kindNow();
  lv_label_set_text_fmt(boardTitle, "BEST %s", kindTitle(kind));
  const uint8_t count = dragScoreCount(kind);
  if (count == 0)
  {
    lv_label_set_text(boardList, "No runs yet.\nTap to go back, then tap to start.");
    lv_obj_add_flag(clearButton, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  char text[DRAG_SCORES * 32];
  size_t used = 0;
  text[0] = '\0';
  for (uint8_t place = 0; place < count && used < sizeof(text); place++)
  {
    const DragRun run = dragScore(kind, place);
    char time[12];
    timeText(run.timeMs, time, sizeof(time));
    used += snprintf(text + used, sizeof(text) - used, "%2u.   %s s     run %lu\n", place + 1, time,
                     (unsigned long)run.runNumber);
  }
  lv_label_set_text(boardList, text);
  lv_obj_remove_flag(clearButton, LV_OBJ_FLAG_HIDDEN);
}

static void boardShow(bool shown)
{
  boardShown = shown;
  clearAskedMs = 0;
  if (shown)
  {
    lv_label_set_text(clearLabel, "Clear Scores");
    boardFill();
    lv_obj_remove_flag(boardBox, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    lv_obj_add_flag(boardBox, LV_OBJ_FLAG_HIDDEN);
  }
}

// Clear Scores: the first tap asks, a second within DRAG_CLEAR_CONFIRM_MS clears this kind's board
static void clearPressed(lv_event_t *event)
{
  (void)event;
  const uint32_t now = millis();
  if (clearAskedMs != 0 && now - clearAskedMs < DRAG_CLEAR_CONFIRM_MS)
  {
    dragClear(kindNow());
    clearAskedMs = 0;
    lv_label_set_text(clearLabel, "Clear Scores");
    boardFill();
    showBest(kindNow());
    return;
  }
  clearAskedMs = now;
  lv_label_set_text(clearLabel, "Tap Again to Clear");
}

// ---------------------------------------------------------------------------
// touches
// ---------------------------------------------------------------------------

// a single tap (once no second one followed)
static void singleTap()
{
  if (boardShown)
  {
    boardShow(false); // back to the timer
    return;
  }
  if (state == DRAG_DONE)
  {
    goReady(NULL);
    return;
  }
  if (state != DRAG_READY)
  {
    return; // counting / running stop on the tap itself (screenEvent)
  }
  if (!signalValid(SIG_SPEED))
  {
    setStatus("No Speed Signal", shades.danger);
    return;
  }
  if (signalGet(SIG_SPEED) > DRAG_STILL_KMH)
  {
    setStatus("Stop the Car First", shades.danger);
    return;
  }
  // the countdown, under the raised start flag
  state = DRAG_COUNTING;
  countStartMs = millis();
  runKind = kindNow();
  runTargetKmh = targetKmhOf(runKind);
  setFlag(finishFlag, false);
  setFlag(startFlag, true);
  lv_obj_set_style_text_color(bigLabel, lv_color_hex(shades.text), 0);
  setStatus("Get Ready", shades.accent2);
}

static void screenEvent(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_LONG_PRESSED)
  {
    if (state == DRAG_COUNTING || state == DRAG_RUNNING)
    {
      goReady("Stopped"); // leaving mid-run saves nothing
    }
    uiOpenMenu();
    return;
  }
  if (code != LV_EVENT_SHORT_CLICKED)
  {
    return;
  }
  if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE)
  {
    return; // the end of a swipe, not a tap
  }
  // counting down or running: a tap stops it straight away (no waiting to see if it's a double-tap)
  if (state == DRAG_COUNTING || state == DRAG_RUNNING)
  {
    tapWaiting = false;
    goReady("Stopped");
    return;
  }
  const uint32_t now = millis();
  if (tapWaiting && now - tapMs < DRAG_DOUBLE_MS)
  {
    tapWaiting = false;
    boardShow(!boardShown); // a double-tap: the scoreboard (or back from it)
    return;
  }
  tapWaiting = true; // a single tap, unless a second one comes (dragTick decides)
  tapMs = now;
}

// ---------------------------------------------------------------------------
// open / tick / close
// ---------------------------------------------------------------------------
lv_obj_t *dragOpen(const DragColours &colours)
{
  shades = colours;
  dragScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(dragScreen, lv_color_hex(shades.bg), 0);
  lv_obj_set_style_bg_opa(dragScreen, LV_OPA_COVER, 0);
  lv_obj_remove_flag(dragScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(dragScreen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(dragScreen, screenEvent, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(dragScreen, screenEvent, LV_EVENT_LONG_PRESSED, NULL);

  // the speed ring round the edge: 0 at the top, full at the target speed
  ring = lv_arc_create(dragScreen);
  lv_obj_set_size(ring, 476, 476);
  lv_obj_center(ring);
  lv_arc_set_rotation(ring, 270);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_range(ring, 0, 1000);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(ring, DRAG_RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, DRAG_RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, lv_color_hex(shades.panel), LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, lv_color_hex(shades.accent), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);

  titleLabel = textLabel(dragScreen, &lv_font_montserrat_24, shades.accent, -170, "0-100 KM/H");
  startFlag = makeFlag(dragScreen, false);
  finishFlag = makeFlag(dragScreen, true);
  bigLabel = textLabel(dragScreen, &lv_font_montserrat_48, shades.text, 0, "0.00");
  lv_obj_set_style_transform_pivot_x(bigLabel, lv_pct(50), 0); // scaled from its middle (LVGL scales from the corner)
  lv_obj_set_style_transform_pivot_y(bigLabel, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(bigLabel, 448, 0); // 1.75x: ~84 px digits
  speedLabel = textLabel(dragScreen, &lv_font_montserrat_24, shades.muted, 72, "");
  statusLabel = textLabel(dragScreen, &lv_font_montserrat_28, shades.accent, 118, "Tap to Start");
  bestLabel = textLabel(dragScreen, &lv_font_montserrat_16, shades.muted, 162, "");

  // the scoreboard: covers the timer while it shows
  boardBox = plainBox(dragScreen, 480, 480, shades.bg);
  lv_obj_center(boardBox);
  lv_obj_set_style_radius(boardBox, LV_RADIUS_CIRCLE, 0);
  boardTitle = textLabel(boardBox, &lv_font_montserrat_24, shades.accent, -165, "BEST");
  boardList = lv_label_create(boardBox);
  lv_obj_set_style_text_font(boardList, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(boardList, lv_color_hex(shades.text), 0);
  lv_obj_set_style_text_align(boardList, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_text_line_space(boardList, 2, 0);
  lv_obj_align(boardList, LV_ALIGN_CENTER, 0, -10);
  clearButton = lv_button_create(boardBox);
  lv_obj_set_size(clearButton, 210, 46);
  lv_obj_align(clearButton, LV_ALIGN_CENTER, 0, 160);
  lv_obj_set_style_bg_color(clearButton, lv_color_hex(shades.panel), 0);
  lv_obj_add_event_cb(clearButton, clearPressed, LV_EVENT_CLICKED, NULL);
  clearLabel = lv_label_create(clearButton);
  lv_obj_set_style_text_color(clearLabel, lv_color_hex(shades.danger), 0);
  lv_label_set_text(clearLabel, "Clear Scores");
  lv_obj_center(clearLabel);
  lv_obj_add_flag(boardBox, LV_OBJ_FLAG_HIDDEN);

  boardShown = false;
  tapWaiting = false;
  lastSampleMs = 0;
  prevSampleMs = 0;
  goReady(NULL);
  return dragScreen;
}

void dragClosed()
{
  dragScreen = NULL; // its objects go with it
  ring = NULL;
  state = DRAG_READY;
}

void dragTick()
{
  if (!dragScreen)
  {
    return;
  }
  const uint32_t now = millis();
  // a tap with no second one after it was a single tap
  if (tapWaiting && now - tapMs >= DRAG_DOUBLE_MS)
  {
    tapWaiting = false;
    singleTap();
  }
  if (clearAskedMs != 0 && now - clearAskedMs >= DRAG_CLEAR_CONFIRM_MS)
  {
    clearAskedMs = 0;
    lv_label_set_text(clearLabel, "Clear Scores"); // the second tap didn't come
  }

  // a new speed reading? (when it came = now less its age)
  const bool speedValid = signalValid(SIG_SPEED);
  const float speedKmh = signalGet(SIG_SPEED);
  bool newReading = false;
  if (speedValid)
  {
    const uint32_t readingMs = now - signalAge(SIG_SPEED);
    const int32_t sinceLast = (int32_t)(readingMs - lastSampleMs);
    if (lastSampleMs == 0 || sinceLast > SAMPLE_SAME_MS || sinceLast < -SAMPLE_SAME_MS)
    {
      prevSpeed = lastSpeed;
      prevSampleMs = lastSampleMs;
      lastSpeed = speedKmh;
      lastSampleMs = readingMs;
      newReading = true;
    }
  }

  const char *unit = "km/h";
  const float speedShown = shownSpeed(runKind, speedKmh, &unit);
  if (state == DRAG_READY)
  {
    if (runKind != kindNow())
    {
      goReady(NULL); // the speed row's unit changed: the other run
    }
    if (speedValid)
    {
      lv_label_set_text_fmt(speedLabel, "%d %s", (int)(speedShown + 0.5f), unit);
    }
    else
    {
      lv_label_set_text(speedLabel, "no speed");
    }
    return;
  }
  if (state == DRAG_COUNTING)
  {
    const uint32_t counted = now - countStartMs;
    if (newReading && lastSpeed > DRAG_MOVING_KMH)
    {
      goRunning(lastSampleMs); // the car moved: the flag drops and the clock starts now
    }
    else if (counted >= DRAG_COUNT_MS)
    {
      goRunning(countStartMs + DRAG_COUNT_MS); // the end of the countdown: the flag drops
    }
    else
    {
      lv_label_set_text_fmt(bigLabel, "%lu", (unsigned long)((DRAG_COUNT_MS - counted + 999) / 1000)); // 5 .. 1
      lv_label_set_text_fmt(speedLabel, "%d %s", (int)(speedShown + 0.5f), unit);
      return;
    }
  }
  if (state == DRAG_RUNNING)
  {
    if (newReading && lastSpeed >= runTargetKmh)
    {
      // the target was passed between the last two readings: the moment it was reached, in proportion
      uint32_t finishMs = lastSampleMs;
      if (prevSampleMs != 0 && prevSpeed < runTargetKmh && lastSpeed > prevSpeed)
      {
        const float share = (runTargetKmh - prevSpeed) / (lastSpeed - prevSpeed); // 0..1 of the way between them
        finishMs = prevSampleMs + (uint32_t)(share * (float)(lastSampleMs - prevSampleMs));
      }
      if (finishMs < runStartMs)
      {
        finishMs = runStartMs; // never before the start
      }
      goDone(finishMs);
      return;
    }
    if (now - runStartMs > DRAG_MAX_RUN_MS)
    {
      goReady("Timed Out");
      return;
    }
    if (!speedValid || signalAge(SIG_SPEED) > DRAG_SPEED_LOST_MS)
    {
      goReady("Lost the Speed");
      return;
    }
    const uint32_t running = now - runStartMs;
    lv_label_set_text_fmt(bigLabel, "%lu.%lu", (unsigned long)(running / 1000), (unsigned long)((running % 1000) / 100));
    lv_label_set_text_fmt(speedLabel, "%d %s", (int)(speedShown + 0.5f), unit);
    int32_t ringValue = (int32_t)(speedKmh * 1000.0f / runTargetKmh); // the ring's share of the way to the target
    if (ringValue < 0)
    {
      ringValue = 0;
    }
    if (ringValue > 1000)
    {
      ringValue = 1000;
    }
    lv_arc_set_value(ring, ringValue);
  }
}
