#include "LVGL_Driver.h"
#include "LVGL_Rotate.h" // 0.25.1: Upside Down / Rotation Trim (SCREEN_ROTATION switches it off)
#include <esp_heap_caps.h>
#include <src/display/lv_display_private.h> // 0.24.24: buf_act (finishTransition) - LVGL keeps it private

static lv_display_t *display = NULL;
static lv_indev_t *indev = NULL;
static SemaphoreHandle_t lvMutex = NULL;
static bool touchOk = false;
static bool ready = false;                       // lvglInit got all the way: LVGL may be used
static volatile bool touchRedetectWanted = false; // web UI Diag: look for the touch controller again (volatile: set by the web task)
static uint32_t frames = 0;
static uint32_t fps = 0;
static uint32_t fpsStamp = 0;
// per-second accumulators -> perf (lvglPerf)
static LvglPerf perf = {};
static uint32_t accumulatedFrameUs = 0;
static uint32_t accumulatedFrameMax = 0;
static uint32_t accumulatedFlushUs = 0;
static uint32_t accumulatedPixels = 0;
static uint32_t accumulatedBusyUs = 0;
static uint32_t refrStartUs = 0;
static bool rendered = false;
static volatile bool forceRedraw = false;

// partial mode: two 1/12-screen draw buffers in internal DMA RAM, copied into the RGB peripheral's frame buffer in PSRAM.
// direct mode: LVGL draws into the peripheral's two frame buffers itself and they swap on vsync.
#define DRAW_BUF_LINES 40
// internal RAM partial mode must leave free for what starts after the display (K-line, chain, CAN relay, ELM, WiFi, the
// web server, mDNS, the gauge's tasks). When its strips would leave less, the gauge draws direct instead.
// 2026-10-14: was 100 KB, which let partial mode leave too little - serving index.html then ran out ("failed to create
// mutex", abort). WiFi alone takes ~50 KB, and every task, lock and web connection after it needs internal RAM too.
#define PARTIAL_MODE_RESERVE (160 * 1024)
static lv_color_t *drawBuf1 = NULL;
static lv_color_t *drawBuf2 = NULL;
static bool directMode = false;
static SemaphoreHandle_t vsyncSem = NULL;
// 0.24.4: LVGL silently refuses draw buffers that aren't on an LV_DRAW_BUF_ALIGN (lv_conf.h) boundary, and then never draws
// a single frame (the glass stays black with the backlight on - first board bring-up 2026-10-26). These say what happened.
static uint32_t frameBufAddress = 0;      // where the RGB peripheral's first frame buffer is (boot log)
static uint32_t frameBuf2Address = 0;     // ... and its second (0.24.5)
static bool directRefused = false;        // direct mode was wanted but the frame buffers weren't aligned for LVGL
static bool partialInPsram = false;       // partial mode's strips had to go in PSRAM (no room in internal RAM)

// 0.24.23: page changes played by the driver (lvglSlideTo / lvglDipTo). LVGL's own screen animations redraw both screens
// every frame - ~50 ms a frame on this glass, so a 250 ms slide showed only 4-5 frames (jumpy). Here the new screen is
// drawn ONCE, off the glass, and each frame of the slide is just rows copied from two still pictures (old + new).
#define FRAME_PIXELS (DISPLAY_WIDTH * DISPLAY_HEIGHT)        // one whole screen
#define FRAME_BYTES (FRAME_PIXELS * sizeof(uint16_t))        // 460,800 bytes (RGB565 = 2 bytes a pixel)
static uint16_t *frameBufFirst = NULL;   // the panel's two frame buffers (direct mode)
static uint16_t *frameBufSecond = NULL;
static uint16_t *onGlass = NULL;         // which of the two is being shown now
static uint16_t *pictureOld = NULL;      // PSRAM: the screen as it was when the slide started
static uint16_t *pictureNew = NULL;      // PSRAM: the new screen, drawn once before the slide
static bool captureFrame = false;        // flushCb: keep this frame in pictureNew instead of showing it
static uint32_t lastTransitionFrames = 0; // the last slide: frames shown ...
static uint32_t lastTransitionMs = 0;     // ... in this long (Diag: frames / time = its frame rate)

#if SCREEN_ROTATION
// 0.25.1: Screen rotation (LVGL_Rotate.h). While a rotation is set LVGL draws into the canvas, and flushCb turns it
// onto the glass at the end of each frame.
static uint16_t *canvas = NULL;            // PSRAM: the screen as LVGL draws it, before it's turned (taken the first time a rotation is set)
static bool rotating = false;              // a rotation is set and running: LVGL draws into the canvas
static bool rotatedUpsideDown = false;     // the 180 degree part is set: slides run the other way round on the glass
static uint8_t rotationState = ROTATION_OFF; // what the web UI card shows
static uint16_t *backBuffer();             // (further down) the frame buffer that isn't on the glass
#endif

static bool IRAM_ATTR onVsync(void *)
{
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(vsyncSem, &woken);
  return woken == pdTRUE;
}

static void flushCb(lv_display_t *flushDisplay, const lv_area_t *area, uint8_t *pixels)
{
  const uint32_t startUs = micros();
  if (!directMode)
  {
    displayDrawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
  }
  else if (captureFrame)
  {
    // 0.24.23: the slide's new screen. LVGL drew it into the frame buffer that ISN'T on the glass; keep a copy and
    // don't show it - the slide brings it in. (A whole frame: the new screen was all marked for redrawing.)
    if (lv_display_flush_is_last(flushDisplay))
    {
#if SCREEN_ROTATION
      if (rotating)
      {
        rotateFrame((const uint16_t *)pixels, pictureNew); // 0.25.1: kept turned, as it will sit on the glass
      }
      else
      {
        memcpy(pictureNew, pixels, FRAME_BYTES);
      }
#else
      memcpy(pictureNew, pixels, FRAME_BYTES);
#endif
    }
  }
  else if (lv_display_flush_is_last(flushDisplay))
  {
    // pixels is one of the panel's own frame buffers: draw_bitmap just makes it the one scanned out, at the next frame.
    // Wait for that vsync, so LVGL never draws into the buffer still on the glass (the other one is synced by LVGL).
    // 0.24.14: the stale vsync is cleared AFTER draw_bitmap, not before. Before, a vsync landing between the two let the
    // wait below return at once, before the swap - and LVGL drew the next frame into the buffer still on the glass
    // (half-drawn lines that hung about until a later frame covered them). Now the worst case is one frame's extra wait.
    uint16_t *show = (uint16_t *)pixels; // the frame buffer to put on the glass
#if SCREEN_ROTATION
    if (rotating)
    {
      // 0.25.1: pixels is the canvas here, not a frame buffer: turn it into the frame buffer that isn't showing, and
      // show that one instead
      show = backBuffer();
      rotateFrame((const uint16_t *)pixels, show);
    }
#endif
    displayDrawBitmap(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, show);
    xSemaphoreTake(vsyncSem, 0); // a vsync from before the swap was asked for doesn't count
    xSemaphoreTake(vsyncSem, pdMS_TO_TICKS(40));
    onGlass = show; // 0.24.23: the slide starts from what's on the glass
  }
  accumulatedFlushUs += micros() - startUs;
  accumulatedPixels += (uint32_t)lv_area_get_width(area) * lv_area_get_height(area);
  if (lv_display_flush_is_last(flushDisplay)) // a frame is several 40-line flushes: count frames, not flushes
  {
    frames++;
  }
  lv_display_flush_ready(flushDisplay);
}

// 0.38.0: a pretend finger from the web UI (Dashboard > Gauge Control, until the screens have touch), through the same
// input as a real one, so every screen answers as it will to a real finger: a tap, a double-tap or a long press in the
// middle of the screen, or a swipe across it
#define FAKE_TAP_MS 80    // how long the finger stays down for a tap (well short of a long press)
#define FAKE_GAP_MS 150   // up between the two taps of a double-tap (inside the screens' 380 ms double-tap window)
#define FAKE_SWIPE_MS 200 // a swipe's length in time ...
#define FAKE_SWIPE_PX 240 // ... and across the screen (LVGL wants 40 px at 3 px a read or more for a swipe)
static volatile uint8_t fakeWanted = 0;     // LVGL_FAKE_* the web UI asked for, not started yet (0 = none)
static volatile uint16_t fakeWantedHoldMs = 0; // ... and for a long press, how long to hold
static uint8_t fakeKind = 0;                // the one running now (0 = none)
static uint8_t fakePressesLeft = 0;         // presses still to do in it (2 for a double-tap)
static uint16_t fakeHoldMs = 0;             // how long each press lasts
static bool fakeDown = false;               // the pretend finger is down now
static uint32_t fakeStepMs = 0;             // when it last went down or up

void lvglFakeTouch(uint8_t kind, uint16_t longHoldMs)
{
  fakeWantedHoldMs = longHoldMs;
  fakeWanted = kind;
}

// the pretend finger, step by step (called at each touch read): true = it is down now, at x / y
static bool fakeTouchStep(int32_t &x, int32_t &y)
{
  const uint32_t now = millis();
  if (fakeKind == 0 && fakeWanted != 0)
  {
    fakeKind = fakeWanted; // a new one starts: the finger goes down
    fakeWanted = 0;
    fakePressesLeft = 1;
    fakeHoldMs = FAKE_TAP_MS;
    if (fakeKind == LVGL_FAKE_DOUBLE_TAP)
    {
      fakePressesLeft = 2;
    }
    else if (fakeKind == LVGL_FAKE_LONG_PRESS)
    {
      fakeHoldMs = fakeWantedHoldMs;
    }
    else if (fakeKind == LVGL_FAKE_SWIPE_LEFT || fakeKind == LVGL_FAKE_SWIPE_RIGHT)
    {
      fakeHoldMs = FAKE_SWIPE_MS;
    }
    fakeDown = true;
    fakeStepMs = now;
  }
  if (fakeKind == 0)
  {
    return false;
  }
  if (fakeDown && now - fakeStepMs >= fakeHoldMs)
  {
    fakeDown = false; // lift it: one press done
    fakeStepMs = now;
    fakePressesLeft--;
    if (fakePressesLeft == 0)
    {
      fakeKind = 0;
    }
  }
  else if (!fakeDown && now - fakeStepMs >= FAKE_GAP_MS)
  {
    fakeDown = true; // the second tap of a double-tap
    fakeStepMs = now;
  }
  const int32_t middleX = lv_display_get_horizontal_resolution(display) / 2;
  x = middleX;
  y = lv_display_get_vertical_resolution(display) / 2;
  if (fakeDown && (fakeKind == LVGL_FAKE_SWIPE_LEFT || fakeKind == LVGL_FAKE_SWIPE_RIGHT))
  {
    // the finger moves across the middle: how far along it is, 0 .. FAKE_SWIPE_PX
    int32_t along = (int32_t)(now - fakeStepMs) * FAKE_SWIPE_PX / FAKE_SWIPE_MS;
    if (along > FAKE_SWIPE_PX)
    {
      along = FAKE_SWIPE_PX;
    }
    if (fakeKind == LVGL_FAKE_SWIPE_LEFT)
    {
      x = middleX + FAKE_SWIPE_PX / 2 - along; // right to left
    }
    else
    {
      x = middleX - FAKE_SWIPE_PX / 2 + along; // left to right
    }
  }
  return fakeDown;
}

static void touchCb(lv_indev_t *driver, lv_indev_data_t *data)
{
  if (touchRedetectWanted)
  {
    touchRedetectWanted = false;
    touchOk = touchRedetect(); // here in the LVGL task, so nothing else is reading the touch controller meanwhile
  }
  TouchPoint point;
  touchRead(point);
  int32_t fakeX = 0;
  int32_t fakeY = 0;
  if (!point.pressed && fakeTouchStep(fakeX, fakeY)) // 0.38.0: the pretend finger, when no real one is down
  {
    point.pressed = true;
    point.x = (uint16_t)fakeX;
    point.y = (uint16_t)fakeY;
  }
  int32_t touchX = point.x;
  int32_t touchY = point.y;
#if SCREEN_ROTATION
  if (rotating)
  {
    rotateTouch(touchX, touchY); // 0.25.1: the picture is turned, so the finger is turned back onto what LVGL drew
  }
#endif
  data->point.x = touchX;
  data->point.y = touchY;
  data->state = point.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tickCb()
{
  return millis();
}

// a frame's cost: LV_EVENT_REFR_START .. REFR_READY, counted when it drew something
static void refrEventCb(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_REFR_START)
  {
    refrStartUs = micros();
    rendered = false;
    if (forceRedraw && lv_display_get_screen_active(display))
    {
      lv_obj_invalidate(lv_display_get_screen_active(display)); // benchmark: the whole screen, every refresh
    }
  }
  else if (code == LV_EVENT_RENDER_START)
  {
    rendered = true;
  }
  else if (code == LV_EVENT_REFR_READY && rendered)
  {
    const uint32_t frameUs = micros() - refrStartUs;
    accumulatedFrameUs += frameUs;
    if (frameUs > accumulatedFrameMax)
    {
      accumulatedFrameMax = frameUs;
    }
  }
}

// The RGB panel's interrupt (it copies every line of the picture out of PSRAM into the small bounce buffers, ~2,800
// times a second) runs on the core that sets the panel up: core 1 here, with LVGL. 0.24.8: 0.24.7 set it up from core 0
// instead - LVGL drew ~40 % faster, but the sprite decoder (core 0) got 2.6x slower, so the frame rate didn't move, and
// WiFi's start-up (radio calibration holds core 0's interrupts off) broke up the boot logo. Back on core 1.
bool lvglInit(const DisplayPins &pins, uint8_t touchAddr, int8_t touchInt, uint8_t touchExioReset, bool direct)
{
  lvMutex = xSemaphoreCreateRecursiveMutex();
  touchResetStart(touchExioReset); // 0.24.28: the touch chip's 100 ms reset wait runs during the panel's set-up below
  if (!lvMutex || !displayInit(pins)) // every lvglLock() takes the mutex: no point going on without it
  {
    return false;
  }
  touchOk = touchInit(touchAddr, touchInt, touchExioReset);

  lv_init();
  lv_tick_set_cb(tickCb);
  display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  void *frameBuf0 = NULL;
  void *frameBuf1 = NULL;
  // partial mode's two strips come out of internal RAM, which WiFi and the tasks need too: if taking them would leave less
  // than PARTIAL_MODE_RESERVE, draw direct (the web UI's Diag tab shows the mode it is running in)
  // 2 bytes a pixel (RGB565). Not sizeof(lv_color_t): that is LVGL's 3-byte colour, which made the strips 50 % bigger
  const size_t partialBytes = 2 * DISPLAY_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t);
  if (!direct && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < partialBytes + PARTIAL_MODE_RESERVE)
  {
    direct = true;
  }
  esp_lcd_rgb_panel_get_frame_buffer(displayPanel(), 2, &frameBuf0, &frameBuf1);
  frameBufAddress = (uint32_t)frameBuf0;
  frameBuf2Address = (uint32_t)frameBuf1;
  // 0.24.4: direct mode only when both frame buffers sit on an LV_DRAW_BUF_ALIGN boundary - LVGL refuses them otherwise
  bool framesAligned = false;
  if (frameBuf0 && frameBuf1 && ((uint32_t)frameBuf0 % LV_DRAW_BUF_ALIGN) == 0 && ((uint32_t)frameBuf1 % LV_DRAW_BUF_ALIGN) == 0)
  {
    framesAligned = true;
  }
  if (direct && !framesAligned)
  {
    directRefused = true;
    direct = false; // partial mode below instead (its strips are allocated 64-byte aligned, which covers any LV_DRAW_BUF_ALIGN up to 64)
  }
  if (direct && (vsyncSem = xSemaphoreCreateBinary()))
  {
    directMode = true;
    displaySetVsyncCallback(onVsync, NULL);
    frameBufFirst = (uint16_t *)frameBuf0;
    frameBufSecond = (uint16_t *)frameBuf1;
    // 0.24.23: the slide's two still pictures (~0.9 MB of PSRAM). Taken now, before the sprite cache sizes itself to
    // what's left. Without them the slides fall back to LVGL's own animation.
    pictureOld = (uint16_t *)heap_caps_aligned_alloc(16, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    pictureNew = (uint16_t *)heap_caps_aligned_alloc(16, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(display, frameBuf0, frameBuf1, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), // RGB565
                           LV_DISPLAY_RENDER_MODE_DIRECT);
  }
  else
  {
    if (vsyncSem) // direct mode was asked for but the panel's frame buffers weren't there: partial mode needs no vsync
    {
      vSemaphoreDelete(vsyncSem);
      vsyncSem = NULL;
    }
    const size_t bufBytes = DISPLAY_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t); // RGB565: 2 bytes a pixel
    // internal RAM when there's room for the strips and the reserve, otherwise PSRAM (0.24.4: slower to draw into,
    // but a picture - before, a refused direct mode with no room here meant no drawing at all)
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= partialBytes + PARTIAL_MODE_RESERVE)
    {
      drawBuf1 = (lv_color_t *)heap_caps_aligned_alloc(64, bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
      drawBuf2 = (lv_color_t *)heap_caps_aligned_alloc(64, bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }
    if (!drawBuf1 || !drawBuf2)
    {
      if (drawBuf1)
      {
        heap_caps_free(drawBuf1);
      }
      if (drawBuf2)
      {
        heap_caps_free(drawBuf2);
      }
      drawBuf1 = (lv_color_t *)heap_caps_aligned_alloc(64, bufBytes, MALLOC_CAP_SPIRAM);
      drawBuf2 = (lv_color_t *)heap_caps_aligned_alloc(64, bufBytes, MALLOC_CAP_SPIRAM);
      partialInPsram = true;
    }
    if (!drawBuf1 || !drawBuf2)
    {
      return false;
    }
    lv_display_set_buffers(display, drawBuf1, drawBuf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  }
  lv_display_set_flush_cb(display, flushCb);
  lv_display_add_event_cb(display, refrEventCb, LV_EVENT_REFR_START, NULL);
  lv_display_add_event_cb(display, refrEventCb, LV_EVENT_RENDER_START, NULL);
  lv_display_add_event_cb(display, refrEventCb, LV_EVENT_REFR_READY, NULL);

  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchCb);
  lv_indev_set_display(indev, display);
  ready = true;
  return true;
}

static void lvglTask(void *)
{
  for (;;)
  {
    uint32_t wait = 5;
    if (lvglLock(20))
    {
      const uint32_t startUs = micros();
      wait = lv_timer_handler();
      accumulatedBusyUs += micros() - startUs;
      lvglUnlock();
    }

    const uint32_t now = millis();
    if (now - fpsStamp >= 1000)
    {
      const uint32_t span = now - fpsStamp;
      fps = frames;
      perf.fps = frames;
      perf.frameAvgUs = frames ? accumulatedFrameUs / frames : 0;
      perf.frameMaxUs = accumulatedFrameMax;
      perf.flushUs = accumulatedFlushUs;
      perf.pixels = accumulatedPixels;
      if (accumulatedBusyUs / 10 / (span ? span : 1) > 100)
      {
        perf.busyPct = 100;
      }
      else
      {
        perf.busyPct = (uint8_t)(accumulatedBusyUs / 10 / (span ? span : 1));
      }
      perf.seq++;
      frames = 0;
      accumulatedFrameUs = 0;
      accumulatedFrameMax = 0;
      accumulatedFlushUs = 0;
      accumulatedPixels = 0;
      accumulatedBusyUs = 0;
      fpsStamp = now;
    }

    if (wait > 20)
    {
      wait = 20;
    }
    if (wait < 2)
    {
      wait = 2;
    }
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

void lvglStartTask(UBaseType_t priority, BaseType_t core)
{
  if (!ready)
  {
    return; // the display didn't come up: no LVGL task, the gauge runs without the screen
  }
  // 6 KB (was 12, then 8): measured 2026-10-14 and again 2026-10-26 it never used more than ~1.7 KB, so this still
  // leaves over 3x for the heavier screens, and gives internal RAM back to WiFi / the web UI / Bluetooth
  // 0.38.1: 10 KB. Coredump 2026-11-02: opening Settings from the menu overflowed 6 KB (5.7 KB used): the fade draws the
  // whole new screen at once (takePictures -> lv_refr_now) from inside the menu's tap, ~4.8 KB on its own for Settings'
  // labels. The 1.7 KB measured before was the normal running, not a fade from a tap.
  xTaskCreatePinnedToCore(lvglTask,  // the LVGL task: draws the screen and reads the touch
                          "lvgl",    // its name in a coredump
                          10240,     // stack, bytes
                          NULL,      // nothing passed in
                          priority,  // as the caller asks
                          NULL,      // no handle kept
                          core);     // on the core the caller asks for
}

// LVGL's own screen animation (a page change from a task that can't draw its own slides, or partial drawing) holds
// the screen it is going to (scr_to_load) and the one it came from until it ends. A screen deleted in that time
// (GaugeUI rebuilding its pages, menu or theme) was still moved about by the next screen load: a crash in
// lv_screen_load_anim (bench 2026-11-03, tools/diag/anim_rebuild_test.py: every round). This ends any such animation
// at once - the screen it was going to shows, and LVGL holds on to nothing else. Caller holds the LVGL lock.
void lvglFinishScreenAnimation()
{
  lv_display_t *disp = lvglDisplay();
  if (disp == NULL || disp->scr_to_load == NULL)
  {
    return; // no screen animation running
  }
  lv_obj_t *goingTo = disp->scr_to_load;
  // the screens it was moving: stopped and put back in place (LVGL's own load only stops the ones it knows of)
  lv_obj_t *others[2] = {disp->prev_scr, disp->act_scr};
  for (uint8_t i = 0; i < 2; i++)
  {
    if (others[i] != NULL && others[i] != goingTo)
    {
      lv_anim_delete(others[i], NULL);
      lv_obj_set_pos(others[i], 0, 0);
      lv_obj_remove_local_style_prop(others[i], LV_STYLE_OPA, 0);
    }
  }
  // LVGL's own way out: a screen loaded during an animation finishes that animation first. A blank screen for a
  // moment (never drawn: the lock is held), then the screen the animation was going to, both at once.
  lv_obj_t *placeholder = lv_obj_create(NULL);
  lv_screen_load(placeholder);
  lv_screen_load(goingTo);
  lv_obj_delete(placeholder);
}

bool lvglLock(uint32_t timeoutMs)
{
  if (!ready)
  {
    return false; // LVGL never started (display init failed): every caller skips its drawing
  }
  return xSemaphoreTakeRecursive(lvMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void lvglUnlock()
{
  xSemaphoreGiveRecursive(lvMutex);
}

lv_display_t *lvglDisplay()
{
  return display;
}

lv_indev_t *lvglTouch()
{
  return indev;
}

bool lvglTouchPresent()
{
  return touchOk;
}

uint32_t lvglFps()
{
  return fps;
}

bool lvglReady()
{
  return ready;
}

void lvglTouchRedetect()
{
  touchRedetectWanted = true;
}

const LvglPerf &lvglPerf()
{
  return perf;
}

bool lvglDirect()
{
  return directMode;
}

uint32_t lvglFrameBufferAddress()
{
  return frameBufAddress;
}

uint32_t lvglFrameBuffer2Address()
{
  return frameBuf2Address;
}

bool lvglDirectRefused()
{
  return directRefused;
}

bool lvglPartialInPsram()
{
  return partialInPsram;
}

void lvglForceRedraw(bool on)
{
  forceRedraw = on;
}

// 0.24.23: how far along a transition is, eased out (quick start, gentle stop): 0..1000 for elapsed 0..total ms
static uint32_t easedProgress(uint32_t elapsedMs, uint32_t totalMs)
{
  if (elapsedMs >= totalMs)
  {
    return 1000;
  }
  const uint32_t left = 1000 - elapsedMs * 1000 / totalMs; // 1000 .. 0
  return 1000 - left * left / 1000 * left / 1000;         // 1 - (1 - t) cubed
}

// 0.24.23: the start of a slide / cross-fade: a copy of what's on the glass (pictureOld), then the new screen drawn
// once - into the frame buffer that isn't showing, kept in pictureNew and not shown (flushCb, captureFrame)
static void takePictures(lv_obj_t *screen, bool deleteOld)
{
  memcpy(pictureOld, onGlass, FRAME_BYTES);
  lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, deleteOld);
  lv_obj_invalidate(screen);
  captureFrame = true;
  lv_refr_now(display);
  captureFrame = false;
}

// show one built frame: swap it onto the glass at the next vsync and wait for that
static void showBuiltFrame(uint16_t *frame)
{
  displayDrawBitmap(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, frame);
  xSemaphoreTake(vsyncSem, 0);                 // as flushCb: only a vsync after the swap counts
  xSemaphoreTake(vsyncSem, pdMS_TO_TICKS(40)); // now it's on the glass
  onGlass = frame;
  frames++; // Diag's frame rate counts transition frames too
}

// the frame buffer that isn't on the glass (where the next frame is built)
static uint16_t *backBuffer()
{
  if (onGlass == frameBufFirst)
  {
    return frameBufSecond;
  }
  return frameBufFirst;
}

// the end of a slide / cross-fade: both frame buffers get the new screen, and the one LVGL draws into next must NOT
// be the one on the glass. 0.24.24: it was half the time (it depends on the number of frames), and LVGL's next redraw
// - which clears to the background first - then showed on the glass for a moment: the 'flick' as a slide settled.
static void finishTransition()
{
  memcpy(backBuffer(), pictureNew, FRAME_BYTES);
  // LVGL has no public "which buffer do you draw into next": it's buf_act in its private display record, and the
  // arrow (->) reads a field of a record we only have the address of. The buffer's pixels are its "data" field.
  lv_draw_buf_t *next = display->buf_act;
  if (next && (uint16_t *)next->data == onGlass)
  {
    showBuiltFrame(backBuffer()); // the same picture, from the other buffer: nothing changes on the glass
  }
}

// one frame of a cross-fade: every pixel mixed from the two pictures, amount 0 (all old) .. 32 (all new). RGB565 trick:
// the pixel is spread over 32 bits (green in the top half, red + blue in the bottom, gaps between them) so the three
// colours are mixed by one multiply each, without spilling into each other.
// optimize("O3"): this one loop is compiled for speed (the rest of the gauge is compiled for size) - it runs for every
// pixel of every frame of a fade
__attribute__((optimize("O3"))) static void buildFadeFrame(uint16_t *frame, uint32_t amount)
{
  const uint32_t *oldPixels = (const uint32_t *)pictureOld; // two pixels at a time: half the memory reads
  const uint32_t *newPixels = (const uint32_t *)pictureNew;
  uint32_t *out = (uint32_t *)frame;
  for (uint32_t i = 0; i < FRAME_PIXELS / 2; i++)
  {
    const uint32_t oldPair = oldPixels[i];
    const uint32_t newPair = newPixels[i];
    uint32_t mixed = 0;
    for (uint8_t half = 0; half < 2; half++) // the low pixel, then the high one
    {
      const uint32_t oldPixel = (oldPair >> (half * 16)) & 0xFFFF;
      const uint32_t newPixel = (newPair >> (half * 16)) & 0xFFFF;
      const uint32_t oldSpread = (oldPixel | (oldPixel << 16)) & 0x07E0F81F; // green up top, red + blue below
      const uint32_t newSpread = (newPixel | (newPixel << 16)) & 0x07E0F81F;
      const uint32_t blend = ((((newSpread - oldSpread) * amount) >> 5) + oldSpread) & 0x07E0F81F;
      const uint32_t pixel = (blend | (blend >> 16)) & 0xFFFF; // back to one 16-bit pixel
      mixed |= pixel << (half * 16);
    }
    out[i] = mixed;
  }
}

// one frame of a slide, built into "frame" from the two pictures. offset = how far the new screen has come in (pixels,
// 0 = all old, 480 = all new). Rows are plain block copies - no drawing.
static void buildSlideFrame(uint16_t *frame, uint8_t how, uint32_t offset)
{
  if (how == LVGL_COVER_UP || how == LVGL_COVER_DOWN)
  {
    // the menu: the new screen covers the old one from below (up) or above (down); the old one stays where it is
    const uint32_t newRows = offset;
    const uint32_t oldRows = DISPLAY_HEIGHT - offset;
    if (how == LVGL_COVER_UP)
    {
      memcpy(frame, pictureOld, oldRows * DISPLAY_WIDTH * 2);                                  // the old screen, top part
      memcpy(frame + oldRows * DISPLAY_WIDTH, pictureNew, newRows * DISPLAY_WIDTH * 2);        // the new one's top, at the bottom
    }
    else
    {
      memcpy(frame, pictureNew + oldRows * DISPLAY_WIDTH, newRows * DISPLAY_WIDTH * 2);        // the new one's bottom, at the top
      memcpy(frame + newRows * DISPLAY_WIDTH, pictureOld + newRows * DISPLAY_WIDTH, oldRows * DISPLAY_WIDTH * 2); // the old, below
    }
    return;
  }
  // a page change: both screens move together, sideways
  const uint32_t newColumns = offset;
  const uint32_t oldColumns = DISPLAY_WIDTH - offset;
  for (uint32_t row = 0; row < DISPLAY_HEIGHT; row++)
  {
    uint16_t *line = frame + row * DISPLAY_WIDTH;
    const uint16_t *oldLine = pictureOld + row * DISPLAY_WIDTH;
    const uint16_t *newLine = pictureNew + row * DISPLAY_WIDTH;
    if (how == LVGL_SLIDE_LEFT) // new page comes in from the right
    {
      memcpy(line, oldLine + newColumns, oldColumns * 2);   // the old page's right part, moved left
      memcpy(line + oldColumns, newLine, newColumns * 2);   // the new page's left part, at the right edge
    }
    else // LVGL_SLIDE_RIGHT: new page comes in from the left
    {
      memcpy(line, newLine + oldColumns, newColumns * 2);   // the new page's right part, at the left edge
      memcpy(line + newColumns, oldLine, oldColumns * 2);   // the old page's left part, moved right
    }
  }
}

// 0.24.23: show "screen" with a slide drawn by the driver (see the top of this file). Call with lvglLock held. Blocks
// for the slide (ms). false = can't here (partial mode, no PSRAM for the pictures, nothing shown yet): the caller uses
// LVGL's own animation instead.
bool lvglSlideTo(lv_obj_t *screen, uint8_t how, uint32_t ms, bool deleteOld)
{
  if (!directMode || !pictureOld || !pictureNew || !onGlass || !screen || screen == lv_screen_active())
  {
    return false;
  }
  if (ms < 100)
  {
    ms = 100;
  }
#if SCREEN_ROTATION
  if (rotating && rotatedUpsideDown)
  {
    // 0.25.1: upside down, the glass's left is the gauge's right and its top the gauge's bottom: run the slide the
    // other way so it moves the right way for the driver
    if (how == LVGL_SLIDE_LEFT)
    {
      how = LVGL_SLIDE_RIGHT;
    }
    else if (how == LVGL_SLIDE_RIGHT)
    {
      how = LVGL_SLIDE_LEFT;
    }
    else if (how == LVGL_COVER_UP)
    {
      how = LVGL_COVER_DOWN;
    }
    else if (how == LVGL_COVER_DOWN)
    {
      how = LVGL_COVER_UP;
    }
  }
#endif
  takePictures(screen, deleteOld);

  // the slide: each frame built into the buffer that isn't showing, then swapped onto the glass at the next vsync
  const uint32_t startMs = millis();
  uint32_t shown = 0;
  uint32_t progress = 0;
  while (progress < 1000)
  {
    progress = easedProgress(millis() - startMs, ms);
    uint32_t size = DISPLAY_WIDTH; // the slide's travel: across, or up / down
    if (how == LVGL_COVER_UP || how == LVGL_COVER_DOWN)
    {
      size = DISPLAY_HEIGHT;
    }
    uint16_t *back = backBuffer();
    buildSlideFrame(back, how, size * progress / 1000);
    showBuiltFrame(back);
    shown++;
  }
  lastTransitionFrames = shown;
  lastTransitionMs = millis() - startMs;
  finishTransition();
  return true;
}

// 0.24.24: show "screen" with a true cross-fade: the old screen melts into the new one. Same start and finish as a
// slide; each frame mixes every pixel of the two pictures. Call with lvglLock held; blocks for ~ms. false = can't here.
bool lvglFadeTo(lv_obj_t *screen, uint32_t ms, bool deleteOld)
{
  if (!directMode || !pictureOld || !pictureNew || !onGlass || !screen || screen == lv_screen_active())
  {
    return false;
  }
  if (ms < 150)
  {
    ms = 150; // fewer frames than this reads as a jump
  }
  takePictures(screen, deleteOld);
  const uint32_t startMs = millis();
  uint32_t shown = 0;
  uint32_t progress = 0;
  while (progress < 1000)
  {
    progress = easedProgress(millis() - startMs, ms);
    uint16_t *back = backBuffer();
    buildFadeFrame(back, progress * 32 / 1000);
    showBuiltFrame(back);
    shown++;
  }
  lastTransitionFrames = shown;
  lastTransitionMs = millis() - startMs;
  finishTransition();
  return true;
}

// 0.24.23: show "screen" by dipping the backlight: down to dark, swap while it's dark, back up. The backlight is
// hardware PWM, so the fade is smooth however long the screen takes to draw. Call with lvglLock held; blocks for ~ms.
bool lvglDipTo(lv_obj_t *screen, uint32_t ms, bool deleteOld)
{
  if (!ready || !screen || screen == lv_screen_active())
  {
    return false;
  }
  uint32_t halfMs = ms / 2;
  if (halfMs < 80)
  {
    halfMs = 80; // shorter looks like a flicker, not a fade
  }
  // down
  uint32_t startMs = millis();
  while (millis() - startMs < halfMs)
  {
    displayBacklightDim((uint8_t)(100 - (millis() - startMs) * 100 / halfMs));
    vTaskDelay(pdMS_TO_TICKS(8));
  }
  displayBacklightDim(0);
  // swap and draw it while it's dark
  lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, deleteOld);
  lv_refr_now(display);
  // up
  startMs = millis();
  while (millis() - startMs < halfMs)
  {
    displayBacklightDim((uint8_t)((millis() - startMs) * 100 / halfMs));
    vTaskDelay(pdMS_TO_TICKS(8));
  }
  displayBacklightDim(100); // the set level (and whatever another task set meanwhile)
  return true;
}

uint32_t lvglLastTransitionFrames()
{
  return lastTransitionFrames;
}

uint32_t lvglLastTransitionMs()
{
  return lastTransitionMs;
}

// the bench screen grab (USB console): the frame buffer on the glass, or the other one (direct mode draws into the
// two in turn). NULL = there isn't one (partial mode has only the one; nothing shown yet). Read it with the LVGL lock
// held, so nothing is drawn into it meanwhile.
const uint16_t *lvglScreenBuffer(bool isOnGlass)
{
  if (!directMode)
  {
    if (isOnGlass)
    {
      return (const uint16_t *)frameBufAddress; // partial mode copies its strips into the panel's first frame buffer
    }
    return NULL;
  }
  if (onGlass == NULL)
  {
    return NULL; // no frame shown yet
  }
  if (isOnGlass)
  {
    return onGlass;
  }
  return backBuffer();
}

#if SCREEN_ROTATION
// 0.25.1: set the rotation (LVGL_Rotate.h). Call with lvglLock held. Moves LVGL between drawing straight into the
// glass's two frame buffers (no rotation) and drawing into the canvas (a rotation set, turned in flushCb).
void lvglSetRotation(bool upsideDown, float trimDegrees)
{
  if (trimDegrees > ROTATION_TRIM_MAX)
  {
    trimDegrees = ROTATION_TRIM_MAX;
  }
  if (trimDegrees < -ROTATION_TRIM_MAX)
  {
    trimDegrees = -ROTATION_TRIM_MAX;
  }
  bool wanted = false; // any rotation at all?
  if (upsideDown || trimDegrees > 0.05f || trimDegrees < -0.05f)
  {
    wanted = true;
  }
  if (!ready)
  {
    return; // no display
  }

  if (!wanted)
  {
    rotatedUpsideDown = false;
    rotationState = ROTATION_OFF;
    if (rotating)
    {
      rotating = false;
      // back to drawing straight into the two frame buffers - the one NOT on the glass first, so LVGL's next frame
      // isn't drawn where it can be seen. The canvas is kept for next time (taking it again may find no room).
      uint16_t *drawFirst = backBuffer();
      uint16_t *drawSecond = frameBufFirst;
      if (drawFirst == frameBufFirst)
      {
        drawSecond = frameBufSecond;
      }
      lv_display_set_buffers(display, drawFirst, drawSecond, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
      lv_obj_invalidate(lv_screen_active()); // the whole screen drawn again, straight
    }
    return;
  }

  if (!directMode)
  {
    rotationState = ROTATION_PARTIAL; // partial mode copies strips straight into the glass: nothing to turn
    return;
  }
  if (!canvas)
  {
    canvas = (uint16_t *)heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM); // 64: LVGL's alignment
    if (!canvas)
    {
      rotationState = ROTATION_NO_ROOM; // the sprites have the PSRAM now; at the next boot it's taken before them
      return;
    }
  }
  float angle = trimDegrees;
  if (upsideDown)
  {
    angle = angle + 180.0f;
  }
  rotateSetAngle(angle);
  rotatedUpsideDown = upsideDown;
  rotationState = ROTATION_ON;
  if (!rotating)
  {
    rotating = true;
    lv_display_set_buffers(display, canvas, NULL, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT); // one buffer: it's never on the glass
  }
  lv_obj_invalidate(lv_screen_active()); // a fresh frame, turned the new way
}

uint8_t lvglRotationState()
{
  return rotationState;
}
#endif
