#include "GaugeSprite.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>
#include <FsUnzip.h> // 0.24.0: version 2 packs, each frame zipped on its own
#include <LVGL_Driver.h> // 0.24.9: lvglPerf() - how long the glass takes to draw a frame, for the pacing

#define SPR_HYST_PM 20              // C2GS_PERMILLE: the current band holds this far past its edges
#define SPR_HYST_REL 0.03f          // C2GS_RANGE: ... and this fraction of its threshold
#define SPR_POP_MS 140              // a word pops in: scale 0 -> 1.25 -> 1 over this long
#define SPR_ZZZ_MS 2200             // one rise of the z's
#define SPR_DROP_WINDOW_MS 1000     // C2GS_DROP: the fall has to happen within this
#define SPR_DROP_COOLDOWN_MS 1500   // ... and not again until this long after the clip

// a word over a clip: one label, or three for the z's
struct SprText
{
  lv_obj_t *labels[3];
  uint16_t fontPx; // the Montserrat size the label uses; scaled to the text's size
  uint32_t popMs;  // when it last popped in
  int32_t lastFrame;
  bool shown;
};

struct Sprite
{
  File file;
  size_t fileSize;
  C2gsHeader header;
  uint8_t *tables; // clips, states, texts, frames (one allocation)
  C2gsClip *clips;
  C2gsState *states;
  C2gsText *texts;
  C2gsFrame *frames;
  uint8_t *palettes; // layered: the palette region of the file
  uint32_t paletteBase;
  uint8_t *blob;   // the frame being decoded, as stored (version 2: once unzipped)
  bool zipped;       // 0.24.0: a version 2 pack - each frame is zipped on its own (C2GS_DEFLATE)
  uint8_t *zip;      // version 2: the frame as read from the file, before it is unzipped into blob
  void *unzipWork;   // version 2: the unzipper's working area (~11 KB), kept while the pack is open
  uint8_t *buf[2]; // decoded frames: buf[front] is the image's pixels, the other is decoded into (NULL while asleep)
  uint8_t front;
  size_t bufBytes;
  // the decode job (the decode task on core 0, 0.11.0): set up here, run there, picked up by stepState()
  volatile uint8_t job; // JOB_*
  volatile bool dead;   // closed while a job was queued or running: the decode task frees it
  int8_t jobClip;
  uint16_t jobFrame;    // within the clip
  uint32_t jobAccent;
  bool jobOk;
  uint32_t shownAccent; // the accent the frame showing was themed with
  uint8_t *glow;   // layered: the frame's glow plane
  uint16_t *glowRow;
  uint8_t *indexRow;
  uint16_t *rowA, *rowB; // scale 2: two composed stored rows
  lv_obj_t *img;
  lv_image_dsc_t dsc;
  int8_t imgClip;       // the clip whose box the image has
  int8_t currentState;  // the state playing (-1 = none yet)
  int8_t forcedState;   // a state spritePlay() started (-1 = the rules pick)
  uint8_t thenRole;
  uint16_t frame;       // within the state's clip
  int32_t shownFrame;   // the pack frame showing (buf[front]), -1 = none
  uint32_t accumulatedMs, lastMs;
  bool done;            // a once clip is on its last frame
  // the shown clip's palette, themed: literal + accent, and the cover
  int8_t paletteClip;
  uint32_t paletteAccent;
  uint16_t palette565[256];
  uint8_t paletteCover[256];
  uint32_t accent;
  // events
  float lastValue[C2GS_MAX_STATES];
  bool lastValid[C2GS_MAX_STATES];
  float peak[C2GS_MAX_STATES];
  uint32_t peakMs[C2GS_MAX_STATES];
  uint32_t coolUntil[C2GS_MAX_STATES];
  uint8_t tapNext;
  SprText *textLabels; // NULL = the pack's words aren't drawn (half size)
  bool half;           // decoded at half size (Rev Runner): layered packs only
  // a clip played by hand (spriteClip), in place of the rules
  bool manual;
  int8_t manualClip;
  uint16_t manualFrom, manualTo;
  uint8_t manualFps;
  bool manualLoop;
  // 0.24.9: the playing clip's frames kept unzipped in PSRAM, so every loop after the first skips the flash read and
  // the unzip (only the decode task touches these, while it has a job for this sprite)
  int8_t cacheClip;   // the clip cached (-1 = none)
  uint8_t *cacheData; // its frames' planes, back to back
  uint32_t *cacheAt;  // where each frame of the clip starts in cacheData
  bool *cacheHave;    // which of them are in yet
  // 0.24.9: steady pacing - when the decode or the drawing can't keep up with the clip's speed, show every 2nd (3rd,
  // 4th) frame at an even rate, rather than whichever frame happens to be due (that made Rev jumpy)
  uint8_t paceStep;           // frames moved on per step (1 = every frame)
  volatile uint32_t decodeUsAvg; // this sprite's decode time, smoothed (written by the decode task)
  uint32_t paceDownSinceMs;   // 0.24.10: since when a smaller step would have fitted (0 = it doesn't)
};

static inline uint16_t storedSide(uint16_t side, uint8_t scale)
{
  return (side + scale - 1) / scale;
}

static inline uint16_t outSide(const Sprite *sprite, uint16_t side);

static uint32_t decodeCount = 0; // spriteDecodeStats
static uint32_t decodeTotalUs = 0;
static uint32_t decodeMaxUs = 0;
static uint32_t unzipTotalUs = 0; // 0.24.7: the unzip part of those decodes
static portMUX_TYPE jobMux = portMUX_INITIALIZER_UNLOCKED; // job / dead / the stats
enum : uint8_t
{
  JOB_IDLE = 0,
  JOB_QUEUED,
  JOB_BUSY,
  JOB_DONE,
};

void spriteDecodeStats(uint32_t &count, uint32_t &totalUs, uint32_t &maxUs, uint32_t &unzipUs)
{
  portENTER_CRITICAL(&jobMux);
  count = decodeCount;
  totalUs = decodeTotalUs;
  maxUs = decodeMaxUs;
  unzipUs = unzipTotalUs;
  decodeCount = 0;
  decodeTotalUs = 0;
  decodeMaxUs = 0;
  unzipTotalUs = 0;
  portEXIT_CRITICAL(&jobMux);
}

// ---------------------------------------------------------------------------
// RLE planes, read as a stream (a row at a time)
// ---------------------------------------------------------------------------
struct Rle
{
  const uint8_t *cursor, *end;
  uint16_t left; // values left in the current packet
  bool repeat;
  uint16_t value;
};

static void rleInit(Rle &rle, const uint8_t *data, size_t len)
{
  rle.cursor = data;
  rle.end = data + len;
  rle.left = 0;
}

static bool rleRead8(Rle &rle, uint8_t *dst, size_t count)
{
  while (count)
  {
    if (!rle.left)
    {
      if (rle.cursor >= rle.end)
      {
        return false;
      }
      const uint8_t packet = *rle.cursor++;
      if (packet & 0x80)
      {
        if (rle.cursor >= rle.end)
        {
          return false;
        }
        rle.repeat = true;
        rle.value = *rle.cursor++;
        rle.left = (packet & 0x7F) + 2;
      }
      else
      {
        rle.repeat = false;
        rle.left = packet + 1;
      }
    }
    const size_t run = rle.left < count ? rle.left : count;
    if (rle.repeat)
    {
      memset(dst, rle.value, run);
    }
    else
    {
      if (rle.cursor + run > rle.end)
      {
        return false;
      }
      memcpy(dst, rle.cursor, run);
      rle.cursor += run;
    }
    dst += run;
    count -= run;
    rle.left -= run;
  }
  return true;
}

static bool rleRead16(Rle &rle, uint16_t *dst, size_t count)
{
  while (count)
  {
    if (!rle.left)
    {
      if (rle.cursor >= rle.end)
      {
        return false;
      }
      const uint8_t packet = *rle.cursor++;
      if (packet & 0x80)
      {
        if (rle.cursor + 2 > rle.end)
        {
          return false;
        }
        rle.repeat = true;
        rle.value = rle.cursor[0] | (rle.cursor[1] << 8);
        rle.cursor += 2;
        rle.left = (packet & 0x7F) + 2;
      }
      else
      {
        rle.repeat = false;
        rle.left = packet + 1;
      }
    }
    size_t run = rle.left < count ? rle.left : count;
    if (rle.repeat)
    {
      for (size_t i = 0; i < run; i++)
      {
        dst[i] = rle.value;
      }
    }
    else
    {
      if (rle.cursor + run * 2 > rle.end)
      {
        return false;
      }
      for (size_t i = 0; i < run; i++, rle.cursor += 2)
      {
        dst[i] = rle.cursor[0] | (rle.cursor[1] << 8);
      }
    }
    dst += run;
    count -= run;
    rle.left -= run;
  }
  return true;
}

// ---------------------------------------------------------------------------
// opening a pack
// ---------------------------------------------------------------------------
bool spriteResolve(const char *want, char *out)
{
  char path[48];
  if (want && want[0])
  {
    snprintf(path, sizeof(path), "/sprites/%s.c2gs", want);
    if (LittleFS.exists(path))
    {
      copyStr(out, SPRITE_NAME_LEN, want);
      return true;
    }
  }
  out[0] = '\0';
  File dir = LittleFS.open("/sprites");
  if (!dir)
  {
    return false;
  }
  for (File file = dir.openNextFile(); file; file = dir.openNextFile())
  {
    const char *baseName = file.name(); // LittleFS gives the base name
    const char *dot = strrchr(baseName, '.');
    if (file.isDirectory() || !dot || strcasecmp(dot, ".c2gs") || dot - baseName >= SPRITE_NAME_LEN)
    {
      continue;
    }
    char key[SPRITE_NAME_LEN];
    memcpy(key, baseName, dot - baseName);
    key[dot - baseName] = '\0';
    if (!out[0] || strcmp(key, out) < 0)
    {
      copyStr(out, SPRITE_NAME_LEN, key);
    }
  }
  return out[0] != '\0';
}

// 0.24.9: the clip cache let go (its PSRAM back)
static void cacheFree(Sprite *sprite)
{
  if (sprite->cacheData)
  {
    heap_caps_free(sprite->cacheData);
  }
  if (sprite->cacheAt)
  {
    heap_caps_free(sprite->cacheAt);
  }
  if (sprite->cacheHave)
  {
    heap_caps_free(sprite->cacheHave);
  }
  sprite->cacheData = NULL;
  sprite->cacheAt = NULL;
  sprite->cacheHave = NULL;
  sprite->cacheClip = -1;
}

static void freeSprite(Sprite *sprite)
{
  cacheFree(sprite);
  if (sprite->file)
  {
    sprite->file.close();
  }
  for (void *block : {(void *)sprite->tables, (void *)sprite->palettes, (void *)sprite->blob, (void *)sprite->buf[0],
                      (void *)sprite->buf[1], (void *)sprite->glow, (void *)sprite->glowRow, (void *)sprite->indexRow,
                      (void *)sprite->rowA, (void *)sprite->rowB, (void *)sprite->textLabels, (void *)sprite->zip,
                      sprite->unzipWork})
  {
    if (block)
    {
      heap_caps_free(block);
    }
  }
  sprite->~Sprite();
  heap_caps_free(sprite);
}

void spriteClose(Sprite *sprite)
{
  if (!sprite)
  {
    return;
  }
  portENTER_CRITICAL(&jobMux);
  const bool inFlight = sprite->job == JOB_QUEUED || sprite->job == JOB_BUSY;
  sprite->dead = true;
  portEXIT_CRITICAL(&jobMux);
  if (!inFlight)
  {
    freeSprite(sprite); // else the decode task frees it when the job ends
  }
}

void spriteSleep(Sprite *sprite)
{
  if (!sprite || (!sprite->buf[0] && !sprite->buf[1]) || sprite->job == JOB_QUEUED || sprite->job == JOB_BUSY)
  {
    return;
  }
  lv_obj_add_flag(sprite->img, LV_OBJ_FLAG_HIDDEN); // nothing to show until it wakes and decodes again
  sprite->dsc.data = NULL;
  for (uint8_t *&buffer : sprite->buf)
  {
    if (buffer)
    {
      heap_caps_free(buffer);
    }
    buffer = NULL;
  }
  sprite->job = JOB_IDLE;
  sprite->shownFrame = -1;
  sprite->imgClip = -1;
  cacheFree(sprite); // 0.24.9: asleep, so its clip cache goes too (no decode job is running: checked above)
}

static const lv_font_t *fontFor(uint8_t px, uint16_t &fontPx)
{
  static const struct
  {
    uint8_t px;
    const lv_font_t *font;
  } fonts[] = {{48, &lv_font_montserrat_48}, {40, &lv_font_montserrat_40}, {32, &lv_font_montserrat_32}, {28, &lv_font_montserrat_28}, {24, &lv_font_montserrat_24}, {20, &lv_font_montserrat_20}, {16, &lv_font_montserrat_16}, {12, &lv_font_montserrat_12}};
  for (auto &entry : fonts)
  {
    if (px >= entry.px)
    {
      fontPx = entry.px;
      return entry.font;
    }
  }
  fontPx = 12;
  return &lv_font_montserrat_12;
}

static void createTextLabels(Sprite *sprite, lv_obj_t *parent)
{
  for (uint8_t textIndex = 0; textIndex < sprite->header.textCount; textIndex++)
  {
    const C2gsText &text = sprite->texts[textIndex];
    SprText &word = sprite->textLabels[textIndex];
    const lv_font_t *font = fontFor(text.size, word.fontPx);
    char wordText[sizeof(text.text) + 1];
    memcpy(wordText, text.text, sizeof(text.text));
    wordText[sizeof(text.text)] = '\0';
    for (int labelIndex = 0; labelIndex < (text.kind == C2GS_ZZZ ? 3 : 1); labelIndex++)
    {
      lv_obj_t *label = lv_label_create(parent);
      lv_obj_set_style_text_font(label, font, 0);
      lv_label_set_text(label, text.kind == C2GS_ZZZ ? "z" : wordText);
      lv_obj_set_style_transform_pivot_x(label, lv_pct(50), 0);
      lv_obj_set_style_transform_pivot_y(label, lv_pct(50), 0);
      lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
      word.labels[labelIndex] = label;
    }
    word.lastFrame = -1;
  }
}

Sprite *spriteOpen(lv_obj_t *parent, const char *name)
{
  return spriteOpenEx(parent, name, false);
}

Sprite *spriteOpenEx(lv_obj_t *parent, const char *name, bool half)
{
  char key[SPRITE_NAME_LEN];
  char path[48];
  if (!spriteResolve(name, key))
  {
    return NULL;
  }
  snprintf(path, sizeof(path), "/sprites/%s.c2gs", key);
  void *mem = heap_caps_calloc(1, sizeof(Sprite), MALLOC_CAP_SPIRAM);
  if (!mem)
  {
    return NULL;
  }
  Sprite *sprite = new (mem) Sprite();
  sprite->currentState = -1;
  sprite->forcedState = -1;
  sprite->imgClip = -1;
  sprite->paletteClip = -1;
  sprite->shownFrame = -1;
  sprite->thenRole = 0xFF;
  sprite->cacheClip = -1; // 0.24.9: no clip cached yet
  sprite->paceStep = 1;   // 0.24.9: every frame until the timings say otherwise

  sprite->file = LittleFS.open(path, "r");
  bool ok = sprite->file && sprite->file.read((uint8_t *)&sprite->header, sizeof(sprite->header)) == sizeof(sprite->header);
  const C2gsHeader &header = sprite->header;
  sprite->fileSize = ok ? sprite->file.size() : 0;
  // version 1 (plain) or version 2 (each frame zipped, C2GS_DEFLATE - 0.24.0)
  sprite->zipped = ok && header.version == C2GS_VERSION_ZIPPED && (header.flags & C2GS_DEFLATE);
  const bool knownVersion = header.version == C2GS_VERSION || sprite->zipped;
  ok = ok && !memcmp(header.magic, C2GS_MAGIC, 4) && knownVersion && header.w && header.h &&
      header.w <= C2GS_MAX_SIDE && header.h <= C2GS_MAX_SIDE && header.frameCount && header.stateCount &&
      header.stateCount <= C2GS_MAX_STATES && header.textCount <= C2GS_MAX_TEXTS && header.clipCount &&
      header.clipCount <= C2GS_MAX_CLIPS && (header.scale == 1 || header.scale == 2) &&
      ((header.flags & C2GS_LAYERED) || header.scale == 1);
  const size_t tableBytes = ok ? header.clipCount * sizeof(C2gsClip) + header.stateCount * sizeof(C2gsState) +
          header.textCount * sizeof(C2gsText) + header.frameCount * sizeof(C2gsFrame) :
                                 0;
  ok = ok && sizeof(header) + tableBytes <= sprite->fileSize &&
      (sprite->tables = (uint8_t *)heap_caps_malloc(tableBytes, MALLOC_CAP_SPIRAM)) &&
      sprite->file.read(sprite->tables, tableBytes) == tableBytes;
  if (ok)
  {
    sprite->clips = (C2gsClip *)sprite->tables;
    sprite->states = (C2gsState *)(sprite->clips + header.clipCount);
    sprite->texts = (C2gsText *)(sprite->states + header.stateCount);
    sprite->frames = (C2gsFrame *)(sprite->texts + header.textCount);
  }

  // the clips: inside the canvas, their frames there, and what the buffers must hold
  size_t outPixels = 0;
  size_t glowPixels = 0;
  size_t blobMax = 0;
  uint16_t sideMax = 0;
  uint32_t paletteLo = UINT32_MAX;
  uint32_t paletteHi = 0;
  for (uint8_t clipIndex = 0; ok && clipIndex < header.clipCount; clipIndex++)
  {
    const C2gsClip &clip = sprite->clips[clipIndex];
    ok = clip.frameCount && clip.firstFrame + clip.frameCount <= header.frameCount && clip.w && clip.h &&
        clip.x + clip.w <= header.w && clip.y + clip.h <= header.h;
    if (ok && (header.flags & C2GS_LAYERED))
    {
      ok = clip.palCount && clip.palCount <= 256 && clip.palOffset + clip.palCount * sizeof(C2gsPal) <= sprite->fileSize;
      paletteLo = clip.palOffset < paletteLo ? clip.palOffset : paletteLo;
      paletteHi = clip.palOffset + clip.palCount * sizeof(C2gsPal) > paletteHi ? clip.palOffset + clip.palCount * sizeof(C2gsPal) : paletteHi;
    }
    const uint16_t storedW = storedSide(clip.w, header.scale);
    const uint16_t storedH = storedSide(clip.h, header.scale);
    outPixels = (size_t)clip.w * clip.h > outPixels ? (size_t)clip.w * clip.h : outPixels;
    const size_t clipGlowPixels = (size_t)storedSide(storedW, C2GS_GLOW_DIV) * storedSide(storedH, C2GS_GLOW_DIV);
    glowPixels = clipGlowPixels > glowPixels ? clipGlowPixels : glowPixels;
    sideMax = storedW > sideMax ? storedW : sideMax;
  }
  size_t zipMax = 0; // version 2: the biggest zipped frame
  for (uint16_t frameIndex = 0; ok && frameIndex < header.frameCount; frameIndex++)
  {
    const C2gsFrame &frame = sprite->frames[frameIndex];
    if (sprite->zipped)
    {
      // len[0..2]: the planes once unzipped, len[3]: the zipped frame in the file
      const uint64_t len = (uint64_t)frame.len[0] + frame.len[1] + frame.len[2];
      ok = frame.len[3] && frame.offset + (uint64_t)frame.len[3] <= sprite->fileSize;
      if (len > blobMax)
      {
        blobMax = len;
      }
      if (frame.len[3] > zipMax)
      {
        zipMax = frame.len[3];
      }
    }
    else
    {
      const uint64_t len = (uint64_t)frame.len[0] + frame.len[1] + frame.len[2] + frame.len[3];
      ok = frame.offset + len <= sprite->fileSize;
      blobMax = len > blobMax ? len : blobMax;
    }
  }
  if (ok && sprite->zipped) // version 2: room for a zipped frame, and the unzipper's working area
  {
    sprite->zip = (uint8_t *)heap_caps_malloc(zipMax, MALLOC_CAP_SPIRAM);
    sprite->unzipWork = fsUnzipWorkArea();
    ok = sprite->zip && sprite->unzipWork;
  }
  for (uint8_t stateIndex = 0; ok && stateIndex < header.stateCount; stateIndex++)
  {
    ok = sprite->states[stateIndex].clip < header.clipCount && sprite->states[stateIndex].role < c2gsRole_t_MAX;
  }
  for (uint8_t textIndex = 0; ok && textIndex < header.textCount; textIndex++)
  {
    ok = sprite->texts[textIndex].clip < header.clipCount;
  }

  const bool layered = header.flags & C2GS_LAYERED;
  const bool alpha = !layered && (header.flags & C2GS_ALPHA);
  sprite->half = half && layered; // a baked pack has no half-size decode: it shows full size
  if (ok && sprite->half) // ok: a pack that failed to open has no clip table to read
  {
    outPixels = 0;
    for (uint8_t clipIndex = 0; clipIndex < header.clipCount; clipIndex++)
    {
      const size_t pixels = (size_t)((sprite->clips[clipIndex].w + 1) / 2) * ((sprite->clips[clipIndex].h + 1) / 2);
      outPixels = pixels > outPixels ? pixels : outPixels;
    }
  }
  if (ok && layered)
  {
    sprite->paletteBase = paletteLo;
    ok = (sprite->palettes = (uint8_t *)heap_caps_malloc(paletteHi - paletteLo, MALLOC_CAP_SPIRAM)) && sprite->file.seek(paletteLo) &&
        sprite->file.read(sprite->palettes, paletteHi - paletteLo) == paletteHi - paletteLo;
  }
  sprite->bufBytes = outPixels * (alpha ? 3 : 2); // the frame buffers come with the first frame (and go while not shown)
  ok = ok && (sprite->blob = (uint8_t *)heap_caps_malloc(blobMax ? blobMax : 1, MALLOC_CAP_SPIRAM)) &&
      (!layered ||
       ((sprite->glow = (uint8_t *)heap_caps_malloc(glowPixels, MALLOC_CAP_SPIRAM)) &&
        (sprite->glowRow = (uint16_t *)heap_caps_malloc(storedSide(sideMax, C2GS_GLOW_DIV) * 2 + 2, MALLOC_CAP_SPIRAM)) &&
        (sprite->indexRow = (uint8_t *)heap_caps_malloc(sideMax, MALLOC_CAP_SPIRAM)) &&
        ((header.scale == 1 && !sprite->half) ||
         ((sprite->rowA = (uint16_t *)heap_caps_malloc(sideMax * 2, MALLOC_CAP_SPIRAM)) &&
          (sprite->rowB = (uint16_t *)heap_caps_malloc(sideMax * 2, MALLOC_CAP_SPIRAM)))))) &&
      (!header.textCount || sprite->half ||
       (sprite->textLabels = (SprText *)heap_caps_calloc(header.textCount, sizeof(SprText), MALLOC_CAP_SPIRAM)));
  if (!ok)
  {
    DEBUG("sprite %s: not a usable C2GS pack (or out of memory)", path);
    spriteClose(sprite);
    return NULL;
  }

  sprite->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  sprite->dsc.header.cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
  sprite->img = lv_image_create(parent);
  lv_obj_remove_flag(sprite->img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(sprite->img, LV_OBJ_FLAG_HIDDEN); // until a frame is decoded
  if (sprite->textLabels)
  {
    createTextLabels(sprite, parent);
  }
  sprite->lastMs = lv_tick_get();
  return sprite;
}

static inline uint16_t outSide(const Sprite *sprite, uint16_t side)
{
  return sprite->half ? (side + 1) / 2 : side;
}

// ---------------------------------------------------------------------------
// decoding
// ---------------------------------------------------------------------------
static inline uint16_t sat565(int red, int green, int blue)
{
  red = red > 31 ? 31 : red;
  green = green > 63 ? 63 : green;
  blue = blue > 31 ? 31 : blue;
  return (uint16_t)(red << 11 | green << 5 | blue);
}

// the clip's palette as it shows: literal + accent x weight, per entry
static void themePalette(Sprite *sprite, int8_t clipIndex, uint32_t accent)
{
  if (sprite->paletteClip == clipIndex && sprite->paletteAccent == accent)
  {
    return;
  }
  const C2gsClip &clip = sprite->clips[clipIndex];
  const C2gsPal *palette = (const C2gsPal *)(sprite->palettes + (clip.palOffset - sprite->paletteBase));
  const int accentR = (accent >> 16 & 255) * 31 / 255;
  const int accentG = (accent >> 8 & 255) * 63 / 255;
  const int accentB = (accent & 255) * 31 / 255;
  for (uint16_t entry = 0; entry < clip.palCount; entry++)
  {
    const uint16_t literal = palette[entry].lit;
    const int weight = palette[entry].acc;
    sprite->palette565[entry] = sat565((literal >> 11) + accentR * weight / 255, (literal >> 5 & 63) + accentG * weight / 255,
                                       (literal & 31) + accentB * weight / 255);
    sprite->paletteCover[entry] = palette[entry].cover;
  }
  for (uint16_t entry = clip.palCount; entry < 256; entry++) // a damaged index shows black
  {
    sprite->palette565[entry] = 0;
    sprite->paletteCover[entry] = 255;
  }
  sprite->paletteClip = clipIndex;
  sprite->paletteAccent = accent;
}

// 0.24.10: what the layered decode reads for every pixel, in internal RAM (it was all in PSRAM, with the sprite).
// Only the decode task uses them, one frame at a time. ~1.4 KB.
static uint16_t fastPalette[256];                            // the clip's themed palette, copied in for each frame
static uint8_t fastCover[256];                               // ... and how much glow each entry hides
static uint8_t fastIndexRow[C2GS_MAX_SIDE];                  // one stored row of palette indexes
static uint16_t fastGlowRow[C2GS_MAX_SIDE / C2GS_GLOW_DIV + 2]; // the glow at this row's height, across

// one stored row of a layered frame: palette colour + the glow where the body doesn't cover it
static void composeRow(Sprite *sprite, uint16_t storedW, uint16_t glowW, uint16_t glowH, uint16_t storedY,
                       uint16_t *dst, int glowR, int glowG, int glowB, uint8_t gain)
{
  // the glow row at this height (bilinear, 8.8 fixed point; the glow plane is 1/8 of the stored size)
  int yFixed = (2 * storedY + 1) * (256 / (2 * C2GS_GLOW_DIV)) - 128;
  int y0 = yFixed >> 8;
  int fracY = yFixed & 255;
  if (yFixed < 0)
  {
    fracY = 0;
    y0 = 0;
  }
  if (y0 >= glowH - 1)
  {
    y0 = glowH - 1;
    fracY = 0;
  }
  const int y1 = y0 + 1 < glowH ? y0 + 1 : y0;
  const uint8_t *glowAbove = sprite->glow + y0 * glowW;
  const uint8_t *glowBelow = sprite->glow + y1 * glowW;
  bool anyGlow = false;
  for (uint16_t x = 0; x < glowW; x++)
  {
    fastGlowRow[x] = (uint16_t)((glowAbove[x] * (256 - fracY) + glowBelow[x] * fracY) >> 8);
    anyGlow |= fastGlowRow[x] != 0;
  }
  fastGlowRow[glowW] = fastGlowRow[glowW - 1];

  for (uint16_t x = 0; x < storedW; x++)
  {
    const uint8_t index = fastIndexRow[x];
    uint16_t pixel = fastPalette[index];
    const int cover = fastCover[index];
    if (anyGlow && cover < 255)
    {
      int xFixed = (2 * x + 1) * (256 / (2 * C2GS_GLOW_DIV)) - 128;
      int x0 = xFixed >> 8;
      int fracX = xFixed & 255;
      if (xFixed < 0)
      {
        fracX = 0;
        x0 = 0;
      }
      const int glowValue = (fastGlowRow[x0] * (256 - fracX) + fastGlowRow[x0 + 1 < glowW ? x0 + 1 : x0] * fracX) >> 8;
      if (glowValue)
      {
        // 0.24.10: shifts instead of dividing - /255 as >> 8 and / 64 with it as >> 14 (under 0.5 % darker, not visible;
        // the divisions were four per glowing pixel)
        int weight = (glowValue * (255 - cover) * gain) >> 14;
        if (weight > 255)
        {
          weight = 255;
        }
        pixel = sat565((pixel >> 11) + ((glowR * weight) >> 8), (pixel >> 5 & 63) + ((glowG * weight) >> 8), (pixel & 31) + ((glowB * weight) >> 8));
      }
    }
    dst[x] = pixel;
  }
}

static inline uint16_t avg565(uint16_t colourA, uint16_t colourB)
{
  return ((colourA & 0xF7DE) >> 1) + ((colourB & 0xF7DE) >> 1);
}

// a stored row (scale 2) doubled across into an output row
static void upRow(const uint16_t *src, uint16_t storedW, uint16_t *dst, uint16_t w)
{
  for (uint16_t x = 0; x < storedW; x++)
  {
    if (2 * x < w)
    {
      dst[2 * x] = src[x];
    }
    if (2 * x + 1 < w)
    {
      dst[2 * x + 1] = x + 1 < storedW ? avg565(src[x], src[x + 1]) : src[x];
    }
  }
}

static bool decodeLayered(Sprite *sprite, int8_t clipIndex, const uint8_t *planes, const C2gsFrame &frame, uint32_t accent,
                          uint8_t *dst)
{
  const C2gsClip &clip = sprite->clips[clipIndex];
  const uint8_t scale = sprite->header.scale;
  const uint16_t storedW = storedSide(clip.w, scale);
  const uint16_t storedH = storedSide(clip.h, scale);
  const uint16_t glowW = storedSide(storedW, C2GS_GLOW_DIV);
  const uint16_t glowH = storedSide(storedH, C2GS_GLOW_DIV);
  Rle indexRle;
  Rle glowRle;
  rleInit(indexRle, planes, frame.len[0]);
  rleInit(glowRle, planes + frame.len[0], frame.len[1]);
  if (!rleRead8(glowRle, sprite->glow, (size_t)glowW * glowH))
  {
    return false;
  }
  themePalette(sprite, clipIndex, accent);
  memcpy(fastPalette, sprite->palette565, sizeof(fastPalette)); // 0.24.10: into internal RAM for the per-pixel reads
  memcpy(fastCover, sprite->paletteCover, sizeof(fastCover));
  const uint32_t glowColour = (clip.glow & C2GS_GLOW_ACCENT) ? accent : clip.glow;
  const int glowR = (glowColour >> 16 & 255) * 31 / 255;
  const int glowG = (glowColour >> 8 & 255) * 63 / 255;
  const int glowB = (glowColour & 255) * 31 / 255;
  uint16_t *out = (uint16_t *)dst;

  if (sprite->half) // Rev Runner: a quarter of the pixels
  {
    const uint16_t outW = outSide(sprite, clip.w);
    for (uint16_t storedY = 0; storedY < storedH; storedY++)
    {
      if (!rleRead8(indexRle, fastIndexRow, storedW))
      {
        return false;
      }
      if (scale == 2) // stored at half size already: as stored
      {
        composeRow(sprite, storedW, glowW, glowH, storedY, out + (size_t)storedY * outW, glowR, glowG, glowB, clip.glowGain);
        continue;
      }
      composeRow(sprite, storedW, glowW, glowH, storedY, (storedY & 1) ? sprite->rowB : sprite->rowA, glowR, glowG, glowB,
                 clip.glowGain);
      if (!(storedY & 1) && storedY + 1 < storedH)
      {
        continue; // the pair's first row
      }
      const uint16_t *secondRow = (storedY & 1) ? sprite->rowB : sprite->rowA; // the last row of an odd box pairs with itself
      uint16_t *outRow = out + (size_t)(storedY / 2) * outW;
      for (uint16_t x = 0; x < outW; x++)
      {
        const uint16_t x1 = 2 * x + 1 < storedW ? 2 * x + 1 : 2 * x;
        outRow[x] = avg565(avg565(sprite->rowA[2 * x], sprite->rowA[x1]), avg565(secondRow[2 * x], secondRow[x1]));
      }
    }
    return true;
  }

  for (uint16_t storedY = 0; storedY < storedH; storedY++)
  {
    if (!rleRead8(indexRle, fastIndexRow, storedW))
    {
      return false;
    }
    if (scale == 1)
    {
      composeRow(sprite, storedW, glowW, glowH, storedY, out + (size_t)storedY * clip.w, glowR, glowG, glowB, clip.glowGain);
      continue;
    }
    // scale 2: rows 2sy - 1 (between the last stored row and this one) and 2sy
    composeRow(sprite, storedW, glowW, glowH, storedY, sprite->rowB, glowR, glowG, glowB, clip.glowGain);
    if (storedY)
    {
      for (uint16_t x = 0; x < storedW; x++)
      {
        sprite->rowA[x] = avg565(sprite->rowA[x], sprite->rowB[x]);
      }
      upRow(sprite->rowA, storedW, out + (size_t)(2 * storedY - 1) * clip.w, clip.w);
    }
    if (2 * storedY < clip.h)
    {
      upRow(sprite->rowB, storedW, out + (size_t)(2 * storedY) * clip.w, clip.w);
    }
    memcpy(sprite->rowA, sprite->rowB, storedW * 2);
  }
  if (scale == 2 && 2 * storedH - 1 < clip.h) // an odd box: the last row again
  {
    upRow(sprite->rowA, storedW, out + (size_t)(2 * storedH - 1) * clip.w, clip.w);
  }
  return true;
}

static bool decodeBaked(Sprite *sprite, int8_t clipIndex, const uint8_t *planes, const C2gsFrame &frame, uint8_t *dst)
{
  const C2gsClip &clip = sprite->clips[clipIndex];
  const size_t pixels = (size_t)clip.w * clip.h;
  Rle rle;
  rleInit(rle, planes, frame.len[0]);
  if (!rleRead16(rle, (uint16_t *)dst, pixels))
  {
    return false;
  }
  if (!(sprite->header.flags & C2GS_ALPHA))
  {
    return true;
  }
  rleInit(rle, planes + frame.len[0], frame.len[1]);
  return rleRead8(rle, dst + pixels * 2, pixels); // RGB565A8: the A8 plane follows the colour plane
}

// The decode task (core 0, below the radio and bus tasks): reads a frame from flash and decodes it into the back buffer
// while LVGL keeps drawing the front one on core 1. One job per sprite at a time; the UI swaps the buffers in stepState().
static QueueHandle_t jobQueue = NULL;

// 0.24.9: the bytes of a frame's planes once unzipped (version 2: len[3] is the zipped size, not a plane)
static size_t planesBytes(const Sprite *sprite, const C2gsFrame &frame)
{
  size_t bytes = (size_t)frame.len[0] + frame.len[1] + frame.len[2];
  if (!sprite->zipped)
  {
    bytes = bytes + frame.len[3];
  }
  return bytes;
}

// 0.24.9: a cache for a clip's frames, filled as they are decoded the first time round. Only made when PSRAM keeps
// SPR_CACHE_PSRAM_SPARE free after it (Rev's clips need 5 KB to 1.5 MB); without one the clip plays as before.
#define SPR_CACHE_PSRAM_SPARE (1024 * 1024)
static void cacheStart(Sprite *sprite, int8_t clipIndex)
{
  cacheFree(sprite);
  const C2gsClip &clip = sprite->clips[clipIndex];
  size_t total = 0;
  for (uint16_t frameIndex = 0; frameIndex < clip.frameCount; frameIndex++)
  {
    total = total + planesBytes(sprite, sprite->frames[clip.firstFrame + frameIndex]);
  }
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < total + SPR_CACHE_PSRAM_SPARE)
  {
    return;
  }
  sprite->cacheData = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
  sprite->cacheAt = (uint32_t *)heap_caps_malloc(clip.frameCount * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  sprite->cacheHave = (bool *)heap_caps_calloc(clip.frameCount, sizeof(bool), MALLOC_CAP_SPIRAM); // none in yet
  if (!sprite->cacheData || !sprite->cacheAt || !sprite->cacheHave)
  {
    cacheFree(sprite);
    return;
  }
  uint32_t at = 0;
  for (uint16_t frameIndex = 0; frameIndex < clip.frameCount; frameIndex++)
  {
    sprite->cacheAt[frameIndex] = at;
    at = at + planesBytes(sprite, sprite->frames[clip.firstFrame + frameIndex]);
  }
  sprite->cacheClip = clipIndex;
}

static void decodeTask(void *)
{
  for (;;)
  {
    Sprite *sprite = NULL;
    if (xQueueReceive(jobQueue, &sprite, portMAX_DELAY) != pdTRUE || !sprite)
    {
      continue;
    }
    portENTER_CRITICAL(&jobMux);
    const bool dead = sprite->dead;
    if (!dead)
    {
      sprite->job = JOB_BUSY;
    }
    portEXIT_CRITICAL(&jobMux);
    if (dead)
    {
      freeSprite(sprite);
      continue;
    }

    const C2gsClip &clip = sprite->clips[sprite->jobClip];
    const C2gsFrame &frame = sprite->frames[clip.firstFrame + sprite->jobFrame];
    uint8_t *dst = sprite->buf[sprite->front ^ 1];
    const uint32_t startUs = micros();
    bool haveFrame = false; // the frame's planes are in blob, ready to decode
    uint32_t unzipUs = 0;   // 0.24.7: the unzip part on its own (web UI > Diag)
    const uint8_t *planes = sprite->blob; // where the decode reads the planes from: blob, or the clip cache
    // 0.24.9: a new clip gets a cache; a frame already in it skips the flash read and the unzip
    if (sprite->cacheClip != sprite->jobClip)
    {
      cacheStart(sprite, sprite->jobClip);
    }
    const bool caching = sprite->cacheClip == sprite->jobClip;
    if (caching && sprite->cacheHave[sprite->jobFrame])
    {
      planes = sprite->cacheData + sprite->cacheAt[sprite->jobFrame];
      haveFrame = true;
    }
    else if (sprite->zipped)
    {
      // 0.24.0, version 2: read the zipped frame, then unzip it into blob (its planes, as a version 1 pack has them)
      const size_t zipLen = frame.len[3];
      const size_t len = (size_t)frame.len[0] + frame.len[1] + frame.len[2];
      haveFrame = sprite->file.seek(frame.offset) && sprite->file.read(sprite->zip, zipLen) == zipLen;
      if (haveFrame)
      {
        const uint32_t unzipStartUs = micros();
        haveFrame = fsUnzipRaw(sprite->unzipWork, sprite->zip, zipLen, sprite->blob, len);
        unzipUs = micros() - unzipStartUs;
      }
    }
    else
    {
      const size_t len = (size_t)frame.len[0] + frame.len[1] + frame.len[2] + frame.len[3];
      haveFrame = sprite->file.seek(frame.offset) && sprite->file.read(sprite->blob, len) == len;
    }
    if (haveFrame && caching && !sprite->cacheHave[sprite->jobFrame]) // keep it for the next time round the loop
    {
      memcpy(sprite->cacheData + sprite->cacheAt[sprite->jobFrame], sprite->blob, planesBytes(sprite, frame));
      sprite->cacheHave[sprite->jobFrame] = true;
    }
    sprite->jobOk = haveFrame &&
        ((sprite->header.flags & C2GS_LAYERED) ? decodeLayered(sprite, sprite->jobClip, planes, frame, sprite->jobAccent, dst) : decodeBaked(sprite, sprite->jobClip, planes, frame, dst));
    const uint32_t elapsedUs = micros() - startUs; // read from flash + unzip + decode
    sprite->decodeUsAvg = (sprite->decodeUsAvg * 7 + elapsedUs) / 8; // 0.24.9: smoothed, for the pacing (0.24.10: 1/8)

    portENTER_CRITICAL(&jobMux);
    decodeCount++;
    decodeTotalUs += elapsedUs;
    unzipTotalUs += unzipUs;
    decodeMaxUs = elapsedUs > decodeMaxUs ? elapsedUs : decodeMaxUs;
    const bool deadNow = sprite->dead;
    sprite->job = JOB_DONE;
    portEXIT_CRITICAL(&jobMux);
    if (deadNow)
    {
      freeSprite(sprite);
    }
  }
}

static bool ensureDecoder()
{
  if (jobQueue)
  {
    return true;
  }
  jobQueue = xQueueCreate(8, sizeof(Sprite *));
  return jobQueue && xTaskCreatePinnedToCore(decodeTask, "spriteDec", 6144, NULL, 2, NULL, 0) == pdPASS;
}

// a finished decode: the back buffer becomes the image (with the clip's box, if it changed)
static void takeFrame(Sprite *sprite)
{
  if (sprite->job != JOB_DONE)
  {
    return;
  }
  const C2gsClip &clip = sprite->clips[sprite->jobClip];
  const int32_t packFrame = clip.firstFrame + sprite->jobFrame;
  sprite->job = JOB_IDLE;
  sprite->shownFrame = packFrame; // a damaged frame isn't asked for again either
  sprite->shownAccent = sprite->jobAccent;
  if (!sprite->jobOk)
  {
    DEBUG("sprite: frame %ld is damaged", (long)packFrame);
    return;
  }
  sprite->front ^= 1;
  sprite->dsc.data = sprite->buf[sprite->front];
  if (sprite->jobClip != sprite->imgClip || lv_obj_has_flag(sprite->img, LV_OBJ_FLAG_HIDDEN)) // a new box: its size and place
  {
    const bool alpha = sprite->dsc.header.cf == LV_COLOR_FORMAT_RGB565A8;
    const uint16_t w = outSide(sprite, clip.w);
    const uint16_t h = outSide(sprite, clip.h);
    const int shrink = sprite->half ? 2 : 1;
    sprite->dsc.header.w = w;
    sprite->dsc.header.h = h;
    sprite->dsc.header.stride = w * 2;
    sprite->dsc.data_size = (uint32_t)w * h * (alpha ? 3 : 2);
    lv_image_set_src(sprite->img, &sprite->dsc);
    lv_obj_align(sprite->img, LV_ALIGN_CENTER, (clip.x + clip.w / 2 - sprite->header.w / 2) / shrink,
                 (clip.y + clip.h / 2 - sprite->header.h / 2) / shrink);
    lv_obj_remove_flag(sprite->img, LV_OBJ_FLAG_HIDDEN);
    sprite->imgClip = sprite->jobClip;
  }
  lv_obj_invalidate(sprite->img); // new pixels (no image cache: LV_CACHE_DEF_SIZE is 0)
}

// ask for a frame (if no decode is running for this sprite): false = no buffers / no decode task
static bool askFrame(Sprite *sprite, int8_t clip, uint16_t frame)
{
  if (sprite->job != JOB_IDLE)
  {
    return true;
  }
  for (uint8_t *&buffer : sprite->buf)
  {
    if (!buffer && !(buffer = (uint8_t *)heap_caps_calloc(1, sprite->bufBytes, MALLOC_CAP_SPIRAM)))
    {
      return false;
    }
  }
  if (!ensureDecoder())
  {
    return false;
  }
  sprite->jobClip = clip;
  sprite->jobFrame = frame;
  sprite->jobAccent = sprite->accent;
  sprite->job = JOB_QUEUED;
  Sprite *queueItem = sprite;
  if (xQueueSend(jobQueue, &queueItem, 0) != pdTRUE)
  {
    sprite->job = JOB_IDLE; // the queue is full: next tick
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// playing
// ---------------------------------------------------------------------------
static void setState(Sprite *sprite, int8_t state)
{
  if (state < 0 || state == sprite->currentState)
  {
    return;
  }
  sprite->currentState = state;
  sprite->frame = 0;
  sprite->accumulatedMs = 0;
  sprite->done = false;
  for (uint8_t textIndex = 0; sprite->textLabels && textIndex < sprite->header.textCount; textIndex++) // words restart with their clip
  {
    sprite->textLabels[textIndex].lastFrame = -1;
  }
}

static int8_t findRole(const Sprite *sprite, uint8_t role)
{
  for (uint8_t stateIndex = 0; stateIndex < sprite->header.stateCount; stateIndex++)
  {
    if (sprite->states[stateIndex].role == role)
    {
      return stateIndex;
    }
  }
  return -1;
}

uint32_t spriteRoleMs(Sprite *sprite, uint8_t role)
{
  const int8_t stateIndex = sprite ? findRole(sprite, role) : -1;
  if (stateIndex < 0)
  {
    return 0;
  }
  const C2gsState &state = sprite->states[stateIndex];
  return (uint32_t)sprite->clips[state.clip].frameCount * 1000u / (state.fpsLo ? state.fpsLo : 15);
}

// 0.24.9: how many frames to move on per step so the steps come at a rate the gauge can keep up with. The frame takes
// the longer of the decode (core 0) and the drawing (core 1) - they run side by side - plus 1/8 to spare. It only
// comes back down a step when the smaller step fits with a quarter to spare, so it doesn't flick between two.
#define SPR_PACE_MAX_STEP 4
#define SPR_PACE_DOWN_MS 3000 // 0.24.10: how long a smaller step must fit before it's used
static uint8_t paceStepFor(Sprite *sprite, uint8_t fps)
{
  if (fps == 0)
  {
    return 1;
  }
  uint32_t busyUs = sprite->decodeUsAvg;
  const uint32_t drawUs = lvglPerf().frameAvgUs; // the last second's frames on the glass
  if (drawUs > busyUs)
  {
    busyUs = drawUs;
  }
  busyUs = busyUs + busyUs / 8;
  const uint32_t frameUs = 1000000u / fps; // one frame of the clip at its own speed
  uint8_t step = (uint8_t)((busyUs + frameUs - 1) / frameUs); // rounded up
  if (step < 1)
  {
    step = 1;
  }
  if (step > SPR_PACE_MAX_STEP)
  {
    step = SPR_PACE_MAX_STEP;
  }
  if (step < sprite->paceStep)
  {
    // 0.24.10: a smaller step only once it has fitted comfortably (a quarter to spare) for SPR_PACE_DOWN_MS on end -
    // 0.24.9 flicked between 2 and 3 as the frame times swung, and every change of step is a change of rhythm
    const uint32_t now = lv_tick_get();
    if (busyUs * 5 / 4 > (uint32_t)(sprite->paceStep - 1) * frameUs)
    {
      sprite->paceDownSinceMs = 0; // not comfortably faster: start again
      step = sprite->paceStep;
    }
    else if (sprite->paceDownSinceMs == 0)
    {
      sprite->paceDownSinceMs = now; // comfortably faster from now: wait and see
      step = sprite->paceStep;
    }
    else if (now - sprite->paceDownSinceMs < SPR_PACE_DOWN_MS)
    {
      step = sprite->paceStep; // not long enough yet
    }
    else
    {
      sprite->paceDownSinceMs = 0;
      step = sprite->paceStep - 1; // one step down at a time
    }
  }
  else
  {
    sprite->paceDownSinceMs = 0;
  }
  return step;
}

// advance frames from..to of a clip at fps (0 = hold; once: stop on `to`) and show the frame
static void stepClip(Sprite *sprite, int8_t clip, uint16_t from, uint16_t to, uint8_t fps, bool once)
{
  const uint32_t now = lv_tick_get();
  uint32_t elapsedMs = now - sprite->lastMs;
  sprite->lastMs = now;
  if (elapsedMs > 200)
  {
    elapsedMs = 200; // a stall (screen change, page build) doesn't fast-forward
  }
  sprite->accumulatedMs += elapsedMs * fps;
  if (sprite->frame < from || sprite->frame > to)
  {
    sprite->frame = from;
  }

  // 0.24.9: move on `step` frames at a time, every step x 1000 / fps ms - the clip keeps its speed, evenly
  const uint8_t step = paceStepFor(sprite, fps);
  if (step != sprite->paceStep)
  {
    DEBUG("sprite: showing every %u frame(s) (decode %lu us, drawing %lu us)", step, (unsigned long)sprite->decodeUsAvg,
          (unsigned long)lvglPerf().frameAvgUs);
    sprite->paceStep = step;
  }
  const uint32_t stepUnits = 1000u * step;
  while (sprite->accumulatedMs >= stepUnits && !sprite->done)
  {
    sprite->accumulatedMs -= stepUnits;
    for (uint8_t moved = 0; moved < step && !sprite->done; moved++)
    {
      if (sprite->frame < to)
      {
        sprite->frame++;
      }
      else if (once)
      {
        sprite->done = true;
      }
      else
      {
        sprite->frame = from;
      }
    }
  }
  if (sprite->done)
  {
    sprite->accumulatedMs = 0;
  }

  takeFrame(sprite); // a decode that finished since the last tick goes up now
  // the next frame: asked for when it differs from the one showing (one decode in flight; if the clip runs faster than
  // the decode, frames are skipped rather than the animation slowed down)
  if ((int32_t)(sprite->clips[clip].firstFrame + sprite->frame) != sprite->shownFrame || sprite->shownAccent != sprite->accent)
  {
    askFrame(sprite, clip, sprite->frame);
  }
}

// advance the playing state at fps (0 = hold) and show its frame
static void stepState(Sprite *sprite, uint8_t fps)
{
  const C2gsState &state = sprite->states[sprite->currentState];
  stepClip(sprite, state.clip, 0, sprite->clips[state.clip].frameCount - 1, fps, state.flags & C2GS_ONCE);
}

static bool familyMatch(uint8_t family, uint8_t pageSignal)
{
  return !family || family == pageSignal || (family == SIG_BOOST_REL && pageSignal == SIG_BOOST);
}

static bool stateApplies(const C2gsState &state, const SpriteContext &ctx)
{
  if (ctx.mode == SPR_PAGE)
  {
    return (state.flags & C2GS_IN_PAGE) && familyMatch(state.family, ctx.pageSignal);
  }
  return ctx.mode == SPR_SAVER && ctx.react && (state.flags & C2GS_IN_SAVER) && state.sig; // the saver has no page value
}

static int32_t permilleOf(float value, float lo, float hi)
{
  if (hi <= lo)
  {
    return 0;
  }
  const float fraction = (value - lo) / (hi - lo);
  if (fraction < 0)
  {
    return 0;
  }
  else if (fraction > 1)
  {
    return 1000;
  }
  else
  {
    return (int32_t)(fraction * 1000.0f);
  }
}

// does a mood's rule hold? `held`: it is the one playing (it holds a little past its edges)
static bool ruleHolds(const C2gsState &state, const SpriteContext &ctx, bool held)
{
  const bool valid = state.sig ? signalValid((signalId_t)state.sig) : ctx.valid;
  const float value = state.sig ? signalGet((signalId_t)state.sig) : ctx.value;
  switch (state.cond)
  {
  case C2GS_ALWAYS: return true;
  case C2GS_NODATA: return !ctx.valid;
  case C2GS_REDLINE: return ctx.valid && ctx.redline > ctx.minVal && ctx.value >= ctx.redline;
  case C2GS_PERMILLE:
  {
    if (!valid)
    {
      return false;
    }
    const int32_t permille = permilleOf(value, ctx.minVal, ctx.maxVal);
    const int32_t hysteresis = held ? SPR_HYST_PM : 0;
    return permille >= state.lo - hysteresis && (permille < state.hi + hysteresis || (state.hi >= 1000 && permille >= 1000));
  }
  case C2GS_RANGE:
  {
    if (!valid)
    {
      return false;
    }
    float ref = 1.0f;
    if (fabsf(state.lo) < 1e8f && fabsf(state.lo) > ref)
    {
      ref = fabsf(state.lo);
    }
    if (fabsf(state.hi) < 1e8f && fabsf(state.hi) > ref)
    {
      ref = fabsf(state.hi);
    }
    const float hysteresis = held ? ref * SPR_HYST_REL : 0.0f;
    return value >= state.lo - hysteresis && value < state.hi + hysteresis;
  }
  default: return false;
  }
}

// events (once, over the mood): C2GS_RISE / C2GS_DROP fire on the signal's movement. Returns the one that fired.
static int8_t checkEvents(Sprite *sprite, const SpriteContext &ctx)
{
  const uint32_t now = lv_tick_get();
  int8_t fired = -1;
  for (uint8_t stateIndex = 0; stateIndex < sprite->header.stateCount; stateIndex++)
  {
    const C2gsState &state = sprite->states[stateIndex];
    if (state.role != C2GS_EVENT)
    {
      continue;
    }
    const bool valid = state.sig ? signalValid((signalId_t)state.sig) : ctx.valid;
    const float value = state.sig ? signalGet((signalId_t)state.sig) : ctx.value;
    bool fire = false;
    if (valid && state.cond == C2GS_RISE)
    {
      fire = sprite->lastValid[stateIndex] && sprite->lastValue[stateIndex] < state.lo && value >= state.lo;
    }
    else if (valid && state.cond == C2GS_DROP)
    {
      if (!sprite->peakMs[stateIndex] || value > sprite->peak[stateIndex] || now - sprite->peakMs[stateIndex] > SPR_DROP_WINDOW_MS)
      {
        sprite->peak[stateIndex] = value;
        sprite->peakMs[stateIndex] = now | 1;
      }
      if (sprite->peak[stateIndex] >= state.lo && sprite->peak[stateIndex] - value >= state.hi &&
          (int32_t)(now - sprite->coolUntil[stateIndex]) >= 0)
      {
        fire = true;
        sprite->peak[stateIndex] = value;
        sprite->coolUntil[stateIndex] = now + (uint32_t)sprite->clips[state.clip].frameCount * 1000u / (state.fpsLo ? state.fpsLo : 15) +
            SPR_DROP_COOLDOWN_MS;
      }
    }
    sprite->lastValue[stateIndex] = value;
    sprite->lastValid[stateIndex] = valid;
    if (fire && fired < 0 && stateApplies(state, ctx))
    {
      fired = stateIndex;
    }
  }
  return fired;
}

// style a word only where it changed: every set redraws it (the refresh runs 50 times a second)
static void styleWord(lv_obj_t *label, lv_color_t colour, lv_opa_t opa, int32_t scale, int32_t rotation)
{
  if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), colour))
  {
    lv_obj_set_style_text_color(label, colour, 0);
  }
  if (lv_obj_get_style_text_opa(label, LV_PART_MAIN) != opa)
  {
    lv_obj_set_style_text_opa(label, opa, 0);
  }
  if (lv_obj_get_style_transform_scale_x(label, LV_PART_MAIN) != scale)
  {
    lv_obj_set_style_transform_scale(label, scale, 0);
  }
  if (lv_obj_get_style_transform_rotation(label, LV_PART_MAIN) != rotation)
  {
    lv_obj_set_style_transform_rotation(label, rotation, 0);
  }
  if (lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN))
  {
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
  }
}

static void updateWords(Sprite *sprite)
{
  if (!sprite->header.textCount || !sprite->textLabels)
  {
    return;
  }
  const uint32_t now = lv_tick_get();
  const C2gsState *state = sprite->currentState >= 0 ? &sprite->states[sprite->currentState] : NULL;
  for (uint8_t textIndex = 0; textIndex < sprite->header.textCount; textIndex++)
  {
    const C2gsText &text = sprite->texts[textIndex];
    SprText &word = sprite->textLabels[textIndex];
    const uint16_t last = state ? sprite->clips[state->clip].frameCount - 1 : 0;
    const bool on = state && text.clip == state->clip && sprite->frame >= text.from && sprite->frame <= (text.to < last ? text.to : last);
    const int labelCount = text.kind == C2GS_ZZZ ? 3 : 1;
    if (!on)
    {
      if (word.shown)
      {
        for (int labelIndex = 0; labelIndex < labelCount; labelIndex++)
        {
          lv_obj_add_flag(word.labels[labelIndex], LV_OBJ_FLAG_HIDDEN);
        }
      }
      word.shown = false;
      continue;
    }

    const lv_color_t colour = lv_color_hex((text.colour & C2GS_GLOW_ACCENT) ? sprite->accent : text.colour);
    if (!word.shown || ((int32_t)sprite->frame != word.lastFrame && text.repop && (sprite->frame - text.from) % text.repop == 0 && word.lastFrame >= 0))
    {
      word.popMs = now; // (re)pop
    }
    const bool newFrame = (int32_t)sprite->frame != word.lastFrame;
    word.lastFrame = sprite->frame;
    const int32_t base = (int32_t)text.size * 256 / word.fontPx;
    if (text.kind == C2GS_ZZZ)
    {
      for (int labelIndex = 0; labelIndex < 3; labelIndex++)
      {
        const float phase = fmodf((now - word.popMs) / (float)SPR_ZZZ_MS + labelIndex / 3.0f, 1.0f);
        lv_obj_t *label = word.labels[labelIndex];
        styleWord(label, colour, (lv_opa_t)(sinf(phase * (float)M_PI) * 255.0f), (int32_t)(base * (0.5f + phase)), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, text.x + (int)(phase * 36.0f), text.y - (int)(phase * 70.0f));
      }
    }
    else
    {
      const uint32_t age = now - word.popMs;
      float popScale;
      if (age >= SPR_POP_MS)
      {
        popScale = 1.0f;
      }
      else if (age < SPR_POP_MS / 2)
      {
        popScale = 1.25f * age / (SPR_POP_MS / 2.0f);
      }
      else
      {
        popScale = 1.25f - 0.25f * (age - SPR_POP_MS / 2.0f) / (SPR_POP_MS / 2.0f);
      }
      lv_obj_t *label = word.labels[0];
      styleWord(label, colour, LV_OPA_COVER, (int32_t)(base * popScale), text.rot * 10);
      if (newFrame || !word.shown)
      {
        const int jitter = text.jitter;
        const int dx = jitter ? (int)(esp_random() % (2 * jitter + 1)) - jitter : 0;
        const int dy = jitter ? (int)(esp_random() % (2 * jitter + 1)) - jitter : 0;
        lv_obj_align(label, LV_ALIGN_CENTER, text.x + dx, text.y + dy);
      }
    }
    word.shown = true;
  }
}

void spriteUpdate(Sprite *sprite, const SpriteContext &ctx)
{
  if (!sprite)
  {
    return;
  }
  sprite->accent = ctx.accent; // a new theme accent: stepState() asks for the frame again in it
  if (sprite->manual) // spriteClip: frames chosen by the caller (Rev Runner)
  {
    stepClip(sprite, sprite->manualClip, sprite->manualFrom, sprite->manualTo, sprite->manualFps, !sprite->manualLoop);
    return;
  }
  if (sprite->forcedState >= 0) // spritePlay: shutdown -> sleep, wake
  {
    if (sprite->done && sprite->thenRole != 0xFF)
    {
      const int8_t next = findRole(sprite, sprite->thenRole);
      sprite->thenRole = 0xFF;
      if (next >= 0)
      {
        sprite->forcedState = next;
        setState(sprite, next);
      }
    }
    stepState(sprite, sprite->states[sprite->currentState].fpsLo);
    updateWords(sprite);
    return;
  }
  if (ctx.mode == SPR_DARK)
  {
    return;
  }

  const bool overlay = sprite->currentState >= 0 &&
      (sprite->states[sprite->currentState].role == C2GS_EVENT || sprite->states[sprite->currentState].role == C2GS_TAP) && !sprite->done;
  const int8_t event = checkEvents(sprite, ctx); // always: keeps the signals' history
  if (!overlay && event >= 0)
  {
    setState(sprite, event);
  }
  else if (!overlay)
  {
    int8_t mood = -1;
    for (uint8_t stateIndex = 0; stateIndex < sprite->header.stateCount && mood < 0; stateIndex++)
    {
      const C2gsState &state = sprite->states[stateIndex];
      if (state.role == C2GS_MOOD && stateApplies(state, ctx) && ruleHolds(state, ctx, stateIndex == sprite->currentState))
      {
        mood = stateIndex;
      }
    }
    if (mood < 0 && ctx.mode == SPR_SAVER)
    {
      mood = findRole(sprite, C2GS_SAVER);
    }
    if (mood < 0)
    {
      mood = sprite->currentState >= 0 && sprite->states[sprite->currentState].role == C2GS_MOOD ? sprite->currentState : findRole(sprite, C2GS_MOOD);
    }
    if (mood < 0)
    {
      mood = 0;
    }
    setState(sprite, mood);
  }

  const C2gsState &state = sprite->states[sprite->currentState];
  uint8_t fps = state.fpsLo;
  if (state.cond == C2GS_PERMILLE && state.role == C2GS_MOOD && ctx.valid) // quicker toward the top of the band
  {
    const int32_t permille = permilleOf(ctx.value, ctx.minVal, ctx.maxVal);
    int32_t span = (int32_t)(state.hi - state.lo);
    if (span < 1)
    {
      span = 1; // a band narrower than 1 (999.5 .. 1000) would divide by zero below
    }
    int32_t bandPos = (permille - (int32_t)state.lo) * 256 / span;
    if (bandPos < 0)
    {
      bandPos = 0;
    }
    else if (bandPos > 256)
    {
      bandPos = 256;
    }
    fps = (uint8_t)(state.fpsLo + ((int32_t)state.fpsHi - state.fpsLo) * bandPos / 256);
  }
  stepState(sprite, fps);
  updateWords(sprite);
}

bool spritePlay(Sprite *sprite, uint8_t role, uint8_t thenRole)
{
  const int8_t stateIndex = sprite ? findRole(sprite, role) : -1;
  if (stateIndex < 0)
  {
    return false;
  }
  sprite->manual = false;
  sprite->forcedState = stateIndex;
  sprite->thenRole = thenRole;
  sprite->currentState = -1; // restart even if it was playing
  setState(sprite, stateIndex);
  sprite->lastMs = lv_tick_get();
  stepState(sprite, 0);
  updateWords(sprite);
  return true;
}

bool spriteDone(Sprite *sprite)
{
  return !sprite || sprite->forcedState < 0 || (sprite->done && sprite->thenRole == 0xFF);
}

bool spriteTap(Sprite *sprite)
{
  if (!sprite || sprite->forcedState >= 0)
  {
    return false;
  }
  int8_t taps[C2GS_MAX_STATES];
  uint8_t tapCount = 0;
  for (uint8_t stateIndex = 0; stateIndex < sprite->header.stateCount; stateIndex++)
  {
    if (sprite->states[stateIndex].role == C2GS_TAP)
    {
      taps[tapCount++] = stateIndex;
    }
  }
  if (!tapCount)
  {
    return false;
  }
  const int8_t stateIndex = taps[sprite->tapNext++ % tapCount];
  sprite->currentState = -1; // a tap during a tap starts it again
  setState(sprite, stateIndex);
  return true;
}

// ---------------------------------------------------------------------------
// clips by hand (Rev Runner)
// ---------------------------------------------------------------------------
int8_t spriteRoleClip(Sprite *sprite, uint8_t role)
{
  const int8_t stateIndex = sprite ? findRole(sprite, role) : -1;
  return stateIndex < 0 ? -1 : (int8_t)sprite->states[stateIndex].clip;
}

int8_t spriteRuleClip(Sprite *sprite, uint8_t sig, float value)
{
  for (uint8_t stateIndex = 0; sprite && stateIndex < sprite->header.stateCount; stateIndex++)
  {
    const C2gsState &state = sprite->states[stateIndex];
    if (state.role == C2GS_MOOD && state.sig == sig && state.cond == C2GS_RANGE && value >= state.lo && value < state.hi)
    {
      return state.clip;
    }
  }
  return -1;
}

uint16_t spriteClipFrames(Sprite *sprite, int8_t clip)
{
  return sprite && clip >= 0 && clip < sprite->header.clipCount ? sprite->clips[clip].frameCount : 0;
}

void spriteClip(Sprite *sprite, int8_t clip, uint16_t from, uint16_t to, uint8_t fps, bool loop)
{
  if (!sprite || clip < 0 || clip >= sprite->header.clipCount)
  {
    return;
  }
  const uint16_t frameCount = sprite->clips[clip].frameCount;
  to = to >= frameCount ? frameCount - 1 : to;
  from = from > to ? to : from;
  if (sprite->manual && sprite->manualClip == clip && sprite->manualFrom == from && sprite->manualTo == to)
  { // the same frames: just the speed / looping (the caller asks every tick)
    sprite->manualFps = fps;
    sprite->manualLoop = loop;
    return;
  }
  sprite->manual = true;
  sprite->forcedState = -1;
  sprite->manualClip = clip;
  sprite->manualFrom = from;
  sprite->manualTo = to;
  sprite->manualFps = fps;
  sprite->manualLoop = loop;
  sprite->frame = from;
  sprite->accumulatedMs = 0;
  sprite->done = false;
  sprite->lastMs = lv_tick_get();
}

bool spriteClipDone(Sprite *sprite)
{
  return !sprite || (sprite->manual && sprite->done);
}

void spriteSetLook(Sprite *sprite, bool additive, uint16_t scaleX, uint16_t scaleY)
{
  if (!sprite)
  {
    return;
  }
  const lv_blend_mode_t blendMode = additive ? LV_BLEND_MODE_ADDITIVE : LV_BLEND_MODE_NORMAL;
  if (lv_obj_get_style_blend_mode(sprite->img, LV_PART_MAIN) != blendMode)
  {
    lv_obj_set_style_blend_mode(sprite->img, blendMode, 0);
  }
  if (lv_image_get_scale_x(sprite->img) != scaleX || lv_image_get_scale_y(sprite->img) != scaleY)
  {
    lv_image_set_pivot(sprite->img, lv_pct(50), lv_pct(50));
    lv_image_set_scale_x(sprite->img, scaleX);
    lv_image_set_scale_y(sprite->img, scaleY);
  }
}
