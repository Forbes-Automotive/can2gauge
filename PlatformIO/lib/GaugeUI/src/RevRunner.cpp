#include "RevRunner.h"
#include "GaugeSprite.h"
#include <can2gauge_defs.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>

// RUNNER.md: 480 x 480 canvas space, origin at the screen centre, y down
#define GROUND 128
#define REV_X (-118)
#define REV_FEET 71        // half-size Rev: feet this far below its canvas centre (the full-size clips stand on y +142)
#define REV_BOX 240        // its half-size canvas
#define JUMP_V 720.0f      // px/s up
#define GRAV 1600.0f       // px/s^2: 0.9 s in the air, 162 px high
#define BUFFER_S 0.12f     // a tap this soon before landing jumps on touchdown
#define SPEED0 250.0f
#define SPEED_MAX 600.0f
#define SPRINT_SPEED 420.0f
#define H_STAND 150
#define WAKE_MS 1600
#define CRASH_S 0.45f
#define OVER_TAP_S 0.6f
#define OVER_IDLE_S 20.0f
#define NIGHT_EVERY 700
#define MAX_OBS 8
#define OB_IMGS 24
#define OB_M 10 // glow margin around an obstacle bitmap
#define N_TICKS 14
#define N_STARS 26
#define N_LINES 9
#define N_RAYS 10
#define HOT 0xFF4A2A
#define NIGHT_TINT 0x8FB4FF

// the Rev clips: the frames RUNNER.md names in the saver loop
#define F_JUMP_UP 113   // arms up, laughing
#define F_JUMP_DOWN 10  // arms up, content
#define F_OVER_FROM 134 // flat out, kicking ...
#define F_OVER_TO 148   // ... then still

enum runState_t : uint8_t
{
  RS_WAKE = 0, // the wake clip, full size
  RS_READY,    // TAP TO RUN
  RS_RUN,
  RS_CRASH,
  RS_OVER,
};
enum obKind_t : uint8_t
{
  OB_CONE1 = 0,
  OB_CONE2,
  OB_CONE3,
  OB_BUMP,
};
static const struct
{
  uint8_t w, h, cones;
} KIND[] = {{36, 46, 1}, {76, 46, 2}, {116, 46, 3}, {64, 20, 0}};

struct ObstacleImage
{
  uint8_t *pixels; // RGB565A8
  lv_image_dsc_t descriptor;
  uint16_t w, h;
};
struct Word
{
  lv_obj_t *label;
  int32_t baseScale; // transform scale for its size on its font
  uint32_t popMs;
  bool shown;
};
struct Obstacle
{
  uint8_t kind;
  float x; // left edge, canvas space
};

static struct Runner
{
  lv_obj_t *screen, *world, *wakeHolder, *revHolder, *groundLine, *scoreLabel, *hiLabel, *moon, *moonCut;
  lv_obj_t *ticks[N_TICKS], *stars[N_STARS], *lines[N_LINES], *rays[N_RAYS], *obstacleImages[OB_IMGS];
  lv_point_precise_t rayPoints[N_RAYS][2];
  Word tapToRunWord, gameOverWord, tapWord, bangWord;
  Sprite *wakeSprite, *revSprite;
  int8_t clipIdle, clipRun, clipSprint, clipCrash, clipSaver;
  ObstacleImage coneImage, bumpImage;
  uint32_t obstacleAccent;
  uint8_t state;
  float stateSeconds, elapsed; // seconds in the state, seconds since the start
  uint32_t lastMs, wakeEndMs;
  float score, speed, ground, y, velocityY, jumpBuffer, nextGap, nightBlend, shake;
  bool night;
  int lastNightLevel;
  Obstacle obstacles[MAX_OBS];
  uint8_t obstacleCount;
  bool wantsOut;
  int32_t scoreShown;
  bool flashShown;
} runner;

static inline float frand()
{
  return (esp_random() & 0xFFFFFF) / 16777216.0f;
}

static inline float easeOut(float progress)
{
  return 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
}

// ---------------------------------------------------------------------------
// the obstacles: drawn once into small RGB565A8 images (white shape, the accent's glow, black stripes / blocks)
// ---------------------------------------------------------------------------
static bool coneInside(float x, float y) // drawCone's outline, base at y 0
{
  static const float outline[7][2] = {{18, -46}, {32, -6}, {36, -6}, {36, 0}, {0, 0}, {0, -6}, {4, -6}};
  bool inside = false;
  for (int i = 0, j = 6; i < 7; j = i++)
  {
    if ((outline[i][1] > y) != (outline[j][1] > y) &&
        x < (outline[j][0] - outline[i][0]) * (y - outline[i][1]) / (outline[j][1] - outline[i][1]) + outline[i][0])
    {
      inside = !inside;
    }
  }
  return inside;
}

static bool coneBlack(float x, float y)
{
  return (x >= 9 && x < 27 && y >= -30 && y < -24) || (x >= 12 && x < 24 && y >= -18 && y < -13);
}

static bool bumpInside(float x, float y) // drawBump: a quadratic arch, peak 20 px
{
  if (x < 0 || x > 64 || y > 0)
  {
    return false;
  }
  const float fraction = x / 64.0f;
  return y >= -80.0f * fraction * (1.0f - fraction);
}

static bool bumpBlack(float x, float y)
{
  return (x >= 14 && x < 23 && y >= -9) || (x >= 28 && x < 37 && y >= -13) || (x >= 42 && x < 51 && y >= -9);
}

static void boxBlur(uint8_t *pixels, uint8_t *scratch, int w, int h, int radius)
{
  for (int y = 0; y < h; y++) // across
  {
    for (int x = 0; x < w; x++)
    {
      int sum = 0;
      int count = 0;
      for (int offset = -radius; offset <= radius; offset++)
      {
        if (x + offset >= 0 && x + offset < w)
        {
          sum += pixels[y * w + x + offset];
          count++;
        }
      }
      scratch[y * w + x] = sum / count;
    }
  }
  for (int y = 0; y < h; y++) // down
  {
    for (int x = 0; x < w; x++)
    {
      int sum = 0;
      int count = 0;
      for (int offset = -radius; offset <= radius; offset++)
      {
        if (y + offset >= 0 && y + offset < h)
        {
          sum += scratch[(y + offset) * w + x];
          count++;
        }
      }
      pixels[y * w + x] = sum / count;
    }
  }
}

static bool renderObstacleImage(ObstacleImage &image, uint8_t shapeW, uint8_t shapeH, bool (*inside)(float, float),
                                bool (*black)(float, float), uint32_t accent)
{
  const int width = shapeW + 2 * OB_M;
  const int height = shapeH + 2 * OB_M;
  const int pixelCount = width * height;
  if (!image.pixels)
  {
    image.pixels = (uint8_t *)heap_caps_malloc(pixelCount * 3, MALLOC_CAP_SPIRAM);
  }
  uint8_t *mask = (uint8_t *)heap_caps_malloc(pixelCount, MALLOC_CAP_SPIRAM);
  uint8_t *glow = (uint8_t *)heap_caps_malloc(pixelCount, MALLOC_CAP_SPIRAM);
  uint8_t *scratch = (uint8_t *)heap_caps_malloc(pixelCount, MALLOC_CAP_SPIRAM);
  if (!image.pixels || !mask || !glow || !scratch)
  {
    heap_caps_free(mask);
    heap_caps_free(glow);
    heap_caps_free(scratch);
    return false;
  }

  for (int y = 0; y < height; y++) // coverage, 4 x 4 samples a pixel
  {
    for (int x = 0; x < width; x++)
    {
      int samples = 0;
      for (int sy = 0; sy < 4; sy++)
      {
        for (int sx = 0; sx < 4; sx++)
        {
          samples += inside(x - OB_M + (sx + 0.5f) / 4.0f, y - OB_M - shapeH + (sy + 0.5f) / 4.0f);
        }
      }
      mask[y * width + x] = samples * 255 / 16;
    }
  }
  memcpy(glow, mask, pixelCount);
  boxBlur(glow, scratch, width, height, 4); // the glow: the shape blurred twice
  boxBlur(glow, scratch, width, height, 4);

  const int accentR = accent >> 16 & 255;
  const int accentG = accent >> 8 & 255;
  const int accentB = accent & 255;
  uint16_t *rgb = (uint16_t *)image.pixels;
  uint8_t *alpha = image.pixels + pixelCount * 2;
  for (int y = 0; y < height; y++)
  {
    for (int x = 0; x < width; x++)
    {
      const int index = y * width + x;
      const int maskValue = mask[index];
      const int glowValue = glow[index] * 140 / 255; // glow at ~55 %
      int red = (255 * maskValue + accentR * (255 - maskValue)) / 255;
      int green = (255 * maskValue + accentG * (255 - maskValue)) / 255;
      int blue = (255 * maskValue + accentB * (255 - maskValue)) / 255;
      if (maskValue > 128 && black(x - OB_M + 0.5f, y - OB_M - shapeH + 0.5f))
      {
        red = 0;
        green = 0;
        blue = 0;
      }
      rgb[index] = (uint16_t)((red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3));
      alpha[index] = maskValue > glowValue ? maskValue : glowValue;
    }
  }
  heap_caps_free(mask);
  heap_caps_free(glow);
  heap_caps_free(scratch);

  image.w = width;
  image.h = height;
  memset(&image.descriptor, 0, sizeof(image.descriptor));
  image.descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  image.descriptor.header.cf = LV_COLOR_FORMAT_RGB565A8;
  image.descriptor.header.w = width;
  image.descriptor.header.h = height;
  image.descriptor.header.stride = width * 2;
  image.descriptor.data_size = pixelCount * 3;
  image.descriptor.data = image.pixels;
  return true;
}

static void buildObstacleImages(uint32_t accent)
{
  const bool coneOk = renderObstacleImage(runner.coneImage, 36, 46, coneInside, coneBlack, accent);
  const bool bumpOk = renderObstacleImage(runner.bumpImage, 64, 20, bumpInside, bumpBlack, accent);
  if (coneOk && bumpOk)
  {
    runner.obstacleAccent = accent; // else (out of PSRAM) runnerTick tries again, as the accent still differs
  }
  for (lv_obj_t *image : runner.obstacleImages) // same descriptors, new pixels
  {
    lv_obj_invalidate(image);
  }
}

// ---------------------------------------------------------------------------
// small helpers: touch an object only when something changed (each set redraws it)
// ---------------------------------------------------------------------------
static void show(lv_obj_t *object, bool on)
{
  if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == on)
  {
    if (on)
    {
      lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
      lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void opa(lv_obj_t *object, lv_opa_t opacity)
{
  if (lv_obj_get_style_opa(object, LV_PART_MAIN) != opacity)
  {
    lv_obj_set_style_opa(object, opacity, 0);
  }
}

static lv_obj_t *box(lv_obj_t *parent, int w, int h, uint32_t colour, lv_opa_t opacity, int radius)
{
  lv_obj_t *object = lv_obj_create(parent);
  lv_obj_remove_style_all(object);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(object, w, h);
  lv_obj_set_style_bg_color(object, lv_color_hex(colour), 0);
  lv_obj_set_style_bg_opa(object, opacity, 0);
  lv_obj_set_style_radius(object, radius, 0);
  return object;
}

static lv_obj_t *holder(lv_obj_t *parent, int side)
{
  lv_obj_t *object = box(parent, side, side, 0, LV_OPA_TRANSP, 0);
  lv_obj_set_style_transform_pivot_x(object, side / 2, 0);
  lv_obj_set_style_transform_pivot_y(object, side / 2, 0);
  return object;
}

static void wordMake(Word &word, lv_obj_t *parent, const char *text, int size, uint32_t colour)
{
  const lv_font_t *font = size >= 48 ? &lv_font_montserrat_48 : &lv_font_montserrat_40;
  word.label = lv_label_create(parent);
  lv_obj_set_style_text_font(word.label, font, 0);
  lv_obj_set_style_text_color(word.label, lv_color_hex(colour), 0);
  lv_label_set_text(word.label, text);
  lv_obj_set_style_transform_pivot_x(word.label, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(word.label, lv_pct(50), 0);
  lv_obj_remove_flag(word.label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(word.label, LV_OBJ_FLAG_HIDDEN);
  word.baseScale = size * 256 / (size >= 48 ? 48 : 40);
  word.shown = false;
}

// a word on (popping in over 140 ms: 0 -> 1.25 -> 1, as the mascot's words) or off; x, y from the screen centre
static void wordSet(Word &word, bool on, int x, int y, uint32_t now)
{
  if (!on)
  {
    if (word.shown)
    {
      show(word.label, false);
    }
    word.shown = false;
    return;
  }
  if (!word.shown)
  {
    word.popMs = now;
    lv_obj_align(word.label, LV_ALIGN_CENTER, x, y);
    show(word.label, true);
    word.shown = true;
  }

  const uint32_t age = now - word.popMs;
  float pop;
  if (age >= 140)
  {
    pop = 1.0f;
  }
  else if (age < 70)
  {
    pop = 1.25f * age / 70.0f;
  }
  else
  {
    pop = 1.25f - 0.25f * (age - 70) / 70.0f;
  }
  const int32_t scale = (int32_t)(word.baseScale * pop);
  if (lv_obj_get_style_transform_scale_x(word.label, LV_PART_MAIN) != scale)
  {
    lv_obj_set_style_transform_scale(word.label, scale, 0);
  }
  lv_obj_align(word.label, LV_ALIGN_CENTER, x, y); // (unchanged: no redraw)
}

// ---------------------------------------------------------------------------
// the game
// ---------------------------------------------------------------------------
static void setState(uint8_t state)
{
  runner.state = state;
  runner.stateSeconds = 0;
}

static void resetRun()
{
  runner.score = 0;
  runner.speed = SPEED0;
  runner.y = 0;
  runner.velocityY = 0;
  runner.jumpBuffer = 0;
  runner.obstacleCount = 0;
  runner.nextGap = 420;
  runner.night = false;
  runner.lastNightLevel = 0;
  runner.shake = 0;
}

static void spawn()
{
  if (runner.obstacleCount >= MAX_OBS)
  {
    return;
  }
  const float kindRoll = frand();
  const float score = runner.score;
  uint8_t kind = OB_CONE1;
  if (kindRoll < 0.42f)
  {
    kind = OB_CONE1;
  }
  else if (kindRoll < 0.66f)
  {
    kind = score > 60 ? OB_CONE2 : OB_CONE1;
  }
  else if (kindRoll < 0.80f)
  {
    if (score > 250)
    {
      kind = OB_CONE3;
    }
    else if (score > 60)
    {
      kind = OB_CONE2;
    }
    else
    {
      kind = OB_CONE1;
    }
  }
  else
  {
    kind = OB_BUMP;
  }
  runner.obstacles[runner.obstacleCount++] = {kind, 300.0f};

  // a floor that leaves room to land and react, then a random skewed short (gapRoll squared): most gaps short, a few long
  const float gapFloor = 110 + runner.speed * 0.55f;
  const float gapRange = 280 + runner.speed * 0.9f;
  const float gapRoll = frand();
  runner.nextGap = gapFloor + gapRange * gapRoll * gapRoll;
}

static bool hit(float ax0, float ax1, float ay0, float ay1, float bx0, float bx1, float by0, float by1)
{
  return ax0 < bx1 && ax1 > bx0 && ay0 < by1 && ay1 > by0;
}

static void crash()
{
  setState(RS_CRASH);
  runner.shake = 1;
  const uint32_t finalScore = (uint32_t)runner.score;
  if (finalScore > settings.runnerBest) // RUNNER.md: saved on the crash, kept through a power cycle
  {
    settings.runnerBest = finalScore;
    settingsDirty = true;
  }
}

static void stepRun(float dt)
{
  runner.speed = SPEED0 * powf(1.08f, floorf(runner.score / 60.0f));
  if (runner.speed > SPEED_MAX)
  {
    runner.speed = SPEED_MAX;
  }
  runner.ground += runner.speed * dt;
  runner.score += runner.speed * dt * 0.04f;

  if (runner.velocityY > 0 || runner.y > 0)
  {
    runner.velocityY -= GRAV * dt;
    runner.y += runner.velocityY * dt;
    if (runner.y <= 0)
    {
      runner.y = 0;
      runner.velocityY = 0;
      if (runner.jumpBuffer > 0) // a tap just before landing
      {
        runner.velocityY = JUMP_V;
        runner.jumpBuffer = 0;
      }
    }
  }
  runner.jumpBuffer = runner.jumpBuffer > dt ? runner.jumpBuffer - dt : 0;

  float lastX = -999;
  uint8_t kept = 0;
  for (uint8_t i = 0; i < runner.obstacleCount; i++)
  {
    runner.obstacles[i].x -= runner.speed * dt;
    if (runner.obstacles[i].x > -330)
    {
      runner.obstacles[kept++] = runner.obstacles[i];
    }
  }
  runner.obstacleCount = kept;
  for (uint8_t i = 0; i < runner.obstacleCount; i++)
  {
    lastX = fmaxf(lastX, runner.obstacles[i].x + KIND[runner.obstacles[i].kind].w);
  }
  if (300 - lastX >= runner.nextGap)
  {
    spawn();
  }

  // two boxes: the legs (narrow) and the body (wide): a cone clipping the head doesn't count
  const float feetY = GROUND - runner.y;
  for (uint8_t i = 0; i < runner.obstacleCount; i++)
  {
    const Obstacle &obstacle = runner.obstacles[i];
    const float x0 = obstacle.x + 3;
    const float x1 = obstacle.x + KIND[obstacle.kind].w - 3;
    const float y0 = GROUND - KIND[obstacle.kind].h + 3;
    const float y1 = GROUND;
    if (hit(REV_X - 20, REV_X + 20, feetY - 44, feetY - 2, x0, x1, y0, y1) ||
        hit(REV_X - 38, REV_X + 38, feetY - H_STAND + 10, feetY - 44, x0, x1, y0, y1))
    {
      crash();
      break;
    }
  }

  const int nightLevel = (int)(runner.score / NIGHT_EVERY);
  if (nightLevel != runner.lastNightLevel)
  {
    runner.lastNightLevel = nightLevel;
    runner.night = nightLevel % 2 == 1;
  }
}

static void onEvent(lv_event_t *event)
{
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_LONG_PRESSED)
  {
    runner.wantsOut = true;
    return;
  }
  if (code != LV_EVENT_PRESSED)
  {
    return;
  }
  switch (runner.state)
  {
  case RS_READY:
    resetRun();
    setState(RS_RUN);
    runner.velocityY = JUMP_V; // the first tap starts the run with a jump, as the prototype
    break;
  case RS_RUN:
    if (runner.y == 0)
    {
      runner.velocityY = JUMP_V;
    }
    else
    {
      runner.jumpBuffer = BUFFER_S;
    }
    break;
  case RS_OVER:
    if (runner.stateSeconds > OVER_TAP_S)
    {
      resetRun();
      setState(RS_RUN); // straight into a run, no second wake-up
    }
    break;
  default:
    break;
  }
}

static void onDelete(lv_event_t *)
{
  spriteClose(runner.revSprite); // the objects went with the screen
  spriteClose(runner.wakeSprite);
  heap_caps_free(runner.coneImage.pixels);
  heap_caps_free(runner.bumpImage.pixels);
  memset(&runner, 0, sizeof(runner));
}

lv_obj_t *runnerOpen(const char *pack, uint32_t accent)
{
  if (runner.screen) // one at a time
  {
    return NULL;
  }
  memset(&runner, 0, sizeof(runner));
  runner.screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(runner.screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(runner.screen, LV_OPA_COVER, 0);
  lv_obj_remove_flag(runner.screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(runner.screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(runner.screen, onEvent, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(runner.screen, onEvent, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(runner.screen, onDelete, LV_EVENT_DELETE, NULL);

  // the world (shaken as one on a crash)
  runner.world = box(runner.screen, 480, 480, 0, LV_OPA_TRANSP, 0);
  for (int i = 0; i < N_STARS; i++)
  {
    runner.stars[i] = box(runner.world, 3 + i % 3, 3 + i % 3, 0xFFFFFF, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    show(runner.stars[i], false);
  }
  runner.moon = box(runner.world, 52, 52, 0xFFFFFF, LV_OPA_COVER, LV_RADIUS_CIRCLE);
  runner.moonCut = box(runner.moon, 44, 44, 0x000000, LV_OPA_COVER, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(runner.moonCut, 26 - 12 - 22, 26 - 6 - 22);
  show(runner.moon, false);
  runner.groundLine = box(runner.world, 480, 3, 0xFFFFFF, 191, 2);
  lv_obj_set_pos(runner.groundLine, 0, 240 + GROUND + 1);
  for (int i = 0; i < N_TICKS; i++)
  {
    runner.ticks[i] = box(runner.world, 6 + (i % 3) * 8, 2, 0xFFFFFF, 89, 1);
    lv_obj_set_y(runner.ticks[i], 240 + GROUND + 14 + (i % 2) * 10);
  }
  buildObstacleImages(accent);
  for (int i = 0; i < OB_IMGS; i++)
  {
    runner.obstacleImages[i] = lv_image_create(runner.world);
    lv_obj_remove_flag(runner.obstacleImages[i], LV_OBJ_FLAG_CLICKABLE);
    show(runner.obstacleImages[i], false);
  }
  for (int i = 0; i < N_LINES; i++)
  {
    runner.lines[i] = box(runner.world, 100, 4, accent, LV_OPA_COVER, 2);
    show(runner.lines[i], false);
  }

  // Rev, half size, on the ground line
  runner.revHolder = holder(runner.world, REV_BOX);
  runner.revSprite = spriteOpenEx(runner.revHolder, pack, true);
  if (!runner.revSprite)
  {
    lv_obj_delete(runner.screen); // onDelete clears runner
    return NULL;
  }
  runner.clipIdle = spriteRuleClip(runner.revSprite, SIG_RPM, 800);
  runner.clipRun = spriteRuleClip(runner.revSprite, SIG_RPM, 3000);
  runner.clipSprint = spriteRuleClip(runner.revSprite, SIG_RPM, 5500);
  runner.clipSaver = spriteRoleClip(runner.revSprite, C2GS_SAVER);
  runner.clipCrash = spriteRoleClip(runner.revSprite, C2GS_TAP);
  if (runner.clipIdle < 0) // a pack without rpm moods: its saver loop stands in
  {
    runner.clipIdle = runner.clipSaver >= 0 ? runner.clipSaver : 0;
  }
  if (runner.clipRun < 0)
  {
    runner.clipRun = runner.clipIdle;
  }
  if (runner.clipSprint < 0)
  {
    runner.clipSprint = runner.clipRun;
  }
  if (runner.clipCrash < 0)
  {
    runner.clipCrash = runner.clipIdle;
  }
  for (int i = 0; i < N_RAYS; i++)
  {
    runner.rays[i] = lv_line_create(runner.world);
    lv_obj_set_style_line_color(runner.rays[i], lv_color_hex(HOT), 0);
    lv_obj_set_style_line_width(runner.rays[i], 6, 0);
    lv_obj_set_style_line_rounded(runner.rays[i], true, 0);
    lv_obj_remove_flag(runner.rays[i], LV_OBJ_FLAG_CLICKABLE);
    show(runner.rays[i], false);
  }

  // the score
  runner.scoreShown = -1;
  runner.scoreLabel = lv_label_create(runner.world);
  lv_obj_set_style_text_font(runner.scoreLabel, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(runner.scoreLabel, lv_color_white(), 0);
  lv_label_set_text(runner.scoreLabel, "00000");
  lv_obj_set_style_transform_pivot_x(runner.scoreLabel, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(runner.scoreLabel, 44 * 256 / 40, 0); // 44 px
  lv_obj_align(runner.scoreLabel, LV_ALIGN_CENTER, 0, -178);
  runner.hiLabel = lv_label_create(runner.world);
  lv_obj_set_style_text_font(runner.hiLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(runner.hiLabel, lv_color_white(), 0);
  lv_obj_set_style_text_opa(runner.hiLabel, 140, 0);
  lv_label_set_text_fmt(runner.hiLabel, "HI %05lu", (unsigned long)settings.runnerBest);
  lv_obj_align(runner.hiLabel, LV_ALIGN_CENTER, 0, -146);
  wordMake(runner.tapToRunWord, runner.world, "TAP TO RUN", 48, accent);
  wordMake(runner.gameOverWord, runner.world, "GAME OVER", 64, HOT);
  wordMake(runner.tapWord, runner.world, "TAP", 40, 0xFFFFFF);
  wordMake(runner.bangWord, runner.world, "!!", 70, HOT);
  show(runner.world, false);

  // the wake-up, full size, before the world shows
  runner.wakeHolder = holder(runner.screen, 480);
  runner.wakeSprite = spriteOpenEx(runner.wakeHolder, pack, false);
  if (runner.wakeSprite && spritePlay(runner.wakeSprite, C2GS_WAKE))
  {
    setState(RS_WAKE);
    runner.wakeEndMs = lv_tick_get() + WAKE_MS;
  }
  else
  {
    lv_obj_delete(runner.wakeHolder);
    runner.wakeHolder = NULL;
    spriteClose(runner.wakeSprite);
    runner.wakeSprite = NULL;
    show(runner.world, true);
    setState(RS_READY);
  }
  resetRun();
  runner.lastMs = lv_tick_get();
  return runner.screen;
}

bool runnerWantsOut()
{
  return runner.wantsOut;
}

static uint32_t mixHex(uint32_t from, uint32_t to, float blend)
{
  uint32_t out = 0;
  for (int shift = 0; shift <= 16; shift += 8)
  {
    const float channel = (from >> shift & 255) + ((int)(to >> shift & 255) - (int)(from >> shift & 255)) * blend;
    out |= (uint32_t)channel << shift;
  }
  return out;
}

void runnerTick(uint32_t accent, bool nightMode)
{
  if (!runner.screen)
  {
    return;
  }
  const uint32_t now = lv_tick_get();
  float dt = (now - runner.lastMs) / 1000.0f;
  runner.lastMs = now;
  dt = dt > 0.05f ? 0.05f : dt;
  runner.elapsed += dt;
  runner.stateSeconds += dt;
  if (accent != runner.obstacleAccent)
  {
    buildObstacleImages(accent);
  }
  SpriteContext ctx = {};
  ctx.mode = SPR_DARK;

  if (runner.state == RS_WAKE) // the wake clip, then the world
  {
    ctx.accent = accent;
    spriteUpdate(runner.wakeSprite, ctx);
    if ((int32_t)(now - runner.wakeEndMs) < 0)
    {
      return;
    }
    lv_obj_delete(runner.wakeHolder); // its image first, then its player
    runner.wakeHolder = NULL;
    spriteClose(runner.wakeSprite);
    runner.wakeSprite = NULL;
    show(runner.world, true);
    setState(RS_READY);
  }

  if (runner.state == RS_RUN)
  {
    stepRun(dt);
  }
  if (runner.state == RS_CRASH && runner.stateSeconds > CRASH_S)
  {
    setState(RS_OVER);
  }
  if (runner.state == RS_OVER && runner.stateSeconds > OVER_IDLE_S)
  {
    runner.wantsOut = true;
  }
  const bool night = runner.night || nightMode; // RUNNER.md: every 700 points, or the dash lights
  runner.nightBlend += ((night ? 1.0f : 0.0f) - runner.nightBlend) * fminf(1.0f, dt * 2.0f);
  runner.shake = fmaxf(0.0f, runner.shake - dt * 2.5f);
  const uint32_t glow = runner.nightBlend > 0.5f ? mixHex(accent, NIGHT_TINT, 0.5f) : accent;

  // shake
  lv_obj_set_pos(runner.world, runner.shake > 0 ? (int)((frand() - 0.5f) * 14 * runner.shake) : 0,
                 runner.shake > 0 ? (int)((frand() - 0.5f) * 10 * runner.shake) : 0);

  // sky at night
  const bool sky = runner.nightBlend > 0.02f;
  for (int i = 0; i < N_STARS; i++)
  {
    show(runner.stars[i], sky);
    if (!sky)
    {
      continue;
    }
    const float x = fmodf(fmodf(i * 97 - runner.ground * 0.15f, 520) + 520, 520) - 260;
    const float y = -230 + (i * 53 % 200);
    lv_obj_set_pos(runner.stars[i], 240 + (int)x, 240 + (int)y);
    opa(runner.stars[i], (lv_opa_t)(255 * runner.nightBlend * (0.4f + 0.6f * (sinf(runner.elapsed * 2 + i) + 1) / 2)));
  }
  show(runner.moon, sky);
  if (sky)
  {
    const float moonX = fmodf(fmodf(-runner.ground * 0.08f, 600) + 600, 600) - 300 + 150;
    lv_obj_set_pos(runner.moon, 240 + (int)moonX - 26, 240 - 120 - 26);
    opa(runner.moon, (lv_opa_t)(255 * runner.nightBlend));
  }

  // ground ticks scroll with the ground
  for (int i = 0; i < N_TICKS; i++)
  {
    lv_obj_set_x(runner.ticks[i], 240 + (int)(fmodf(fmodf(i * 71 - runner.ground, 500) + 500, 500) - 250));
  }

  // obstacles
  int used = 0;
  for (uint8_t i = 0; i < runner.obstacleCount && used < OB_IMGS; i++)
  {
    const Obstacle &obstacle = runner.obstacles[i];
    const int imageCount = KIND[obstacle.kind].cones ? KIND[obstacle.kind].cones : 1;
    const ObstacleImage &image = KIND[obstacle.kind].cones ? runner.coneImage : runner.bumpImage;
    for (int part = 0; part < imageCount && used < OB_IMGS; part++, used++)
    {
      lv_obj_t *imageObj = runner.obstacleImages[used];
      if (lv_image_get_src(imageObj) != &image.descriptor)
      {
        lv_image_set_src(imageObj, &image.descriptor);
      }
      lv_obj_set_pos(imageObj, 240 + (int)obstacle.x + part * 40 - OB_M, 240 + GROUND - KIND[obstacle.kind].h - OB_M);
      show(imageObj, true);
    }
  }
  for (int i = used; i < OB_IMGS; i++)
  {
    show(runner.obstacleImages[i], false);
  }

  // Rev: the clip for the moment, then where it stands
  const uint16_t saverFrames = spriteClipFrames(runner.revSprite, runner.clipSaver);
  switch (runner.state)
  {
  case RS_READY: spriteClip(runner.revSprite, runner.clipIdle, 0, 0xFFFF, 15, true); break;
  case RS_RUN:
    if (runner.y > 0 && runner.clipSaver >= 0)
    {
      const uint16_t frame = runner.velocityY > 0 ? F_JUMP_UP : F_JUMP_DOWN;
      spriteClip(runner.revSprite, runner.clipSaver, frame < saverFrames ? frame : 0, frame < saverFrames ? frame : 0, 0, false);
    }
    else
    {
      spriteClip(runner.revSprite, runner.speed > SPRINT_SPEED ? runner.clipSprint : runner.clipRun, 0, 0xFFFF, 15, true);
    }
    break;
  case RS_CRASH: spriteClip(runner.revSprite, runner.clipCrash, 0, 0xFFFF, 15, false); break;
  case RS_OVER:
    if (runner.clipSaver >= 0 && saverFrames > F_OVER_FROM)
    {
      spriteClip(runner.revSprite, runner.clipSaver, F_OVER_FROM, F_OVER_TO, 15, false);
    }
    else
    {
      spriteClip(runner.revSprite, runner.clipCrash, 0xFFFF, 0xFFFF, 0, false); // the crash's last frame
    }
    break;
  default: break;
  }
  ctx.accent = glow;
  spriteUpdate(runner.revSprite, ctx);
  lv_obj_set_pos(runner.revHolder, 240 + REV_X + (runner.state == RS_OVER ? 60 : 0) - REV_BOX / 2,
                 240 + GROUND - (int)runner.y - REV_FEET - REV_BOX / 2);
  // additive: the frame's black is see-through, so the ground line and the cones show behind Rev; startled on a crash:
  // 110 % wide, 90 % tall
  const bool squash = runner.state == RS_CRASH;
  spriteSetLook(runner.revSprite, true, squash ? 282 : 256, squash ? 230 : 256);

  // speed lines when sprinting
  const bool fast = runner.state == RS_RUN && runner.speed > SPRINT_SPEED;
  for (int i = 0; i < N_LINES; i++)
  {
    show(runner.lines[i], fast);
    if (!fast)
    {
      continue;
    }
    const float seed = (sinf(i * 91.7f) + 1) / 2;
    const float phase = fmodf(runner.elapsed * 2.5f + seed, 1.0f);
    lv_obj_set_pos(runner.lines[i], 240 + (int)(240 - phase * 520), 240 - 170 + i * 40);
    lv_obj_set_size(runner.lines[i], (int)(60 + seed * 90), (int)(3 + seed * 3));
    opa(runner.lines[i], (lv_opa_t)(255 * (0.25f + 0.6f * sinf(phase * (float)M_PI))));
    const lv_color_t colour = lv_color_hex(glow);
    if (!lv_color_eq(lv_obj_get_style_bg_color(runner.lines[i], LV_PART_MAIN), colour))
    {
      lv_obj_set_style_bg_color(runner.lines[i], colour, 0);
    }
  }

  // the crash: a red burst where the feet were
  const float burst = runner.state == RS_CRASH ? runner.stateSeconds * 2.5f : 2.0f;
  for (int i = 0; i < N_RAYS; i++)
  {
    const bool on = burst <= 1.0f;
    show(runner.rays[i], on);
    if (!on)
    {
      continue;
    }
    const float angle = i * 2.0f * (float)M_PI / N_RAYS + 0.3f;
    const float distance = easeOut(burst) * 120;
    const float cx = 240 + REV_X;
    const float cy = 240 + GROUND - 80;
    runner.rayPoints[i][0] = {(lv_value_precise_t)(cx + cosf(angle) * distance * 0.5f),
                              (lv_value_precise_t)(cy + sinf(angle) * distance * 0.5f)};
    runner.rayPoints[i][1] = {(lv_value_precise_t)(cx + cosf(angle) * distance), (lv_value_precise_t)(cy + sinf(angle) * distance)};
    lv_line_set_points(runner.rays[i], runner.rayPoints[i], 2);
    opa(runner.rays[i], (lv_opa_t)(255 * (1.0f - burst)));
  }

  // the score: 5 digits, flashing in the accent for the first 6 points of every hundred
  lv_obj_t *scoreLabel = runner.scoreLabel;
  const int32_t score = (int32_t)runner.score;
  if (score != runner.scoreShown)
  {
    lv_label_set_text_fmt(scoreLabel, "%05ld", (long)score);
    runner.scoreShown = score;
  }
  const bool flash = score > 0 && score % 100 < 6 && runner.state == RS_RUN && fmodf(runner.elapsed * 8, 1.0f) < 0.5f;
  if (flash != runner.flashShown)
  {
    lv_obj_set_style_text_color(scoreLabel, flash ? lv_color_hex(accent) : lv_color_white(), 0);
    runner.flashShown = flash;
  }
  if (runner.state == RS_CRASH && runner.stateSeconds < dt * 1.5f) // the best may have just moved
  {
    lv_label_set_text_fmt(runner.hiLabel, "HI %05lu", (unsigned long)settings.runnerBest);
  }

  // words
  wordSet(runner.tapToRunWord, runner.state == RS_READY && fmodf(runner.elapsed * 1.4f, 1.0f) < 0.7f, 20, -70, now);
  wordSet(runner.bangWord, runner.state == RS_CRASH, REV_X + 70, GROUND - 190, now);
  wordSet(runner.gameOverWord, runner.state == RS_OVER, 0, -60, now);
  wordSet(runner.tapWord, runner.state == RS_OVER && runner.stateSeconds > OVER_TAP_S && fmodf(runner.elapsed * 1.6f, 1.0f) < 0.6f,
          0, 0, now);
}
