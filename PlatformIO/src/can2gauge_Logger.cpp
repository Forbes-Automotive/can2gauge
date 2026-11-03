#include <can2gauge_Logger.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks

// ---------------------------------------------------------------------------
// Data logger (0.16.0) - see can2gauge_Logger.h. The logger task takes a sample every 1 / rate seconds and puts it in
// the ring in PSRAM; the web server copies pieces of the ring out for the Log tab. A lock (mutex) stops the two from
// touching the ring at the same moment.
// ---------------------------------------------------------------------------

EXT_RAM_BSS_ATTR static LoggerStatus status;                // what the Log tab shows (only changed while holding logLock)
static uint8_t *ring = NULL;               // the recording in PSRAM (NULL = none taken)
static SemaphoreHandle_t logLock = NULL;   // only one of the logger task / the web server uses the ring at a time
static TaskHandle_t loggerTask = NULL;     // the logger task

// the rates the Log tab offers (samples a second)
static const uint8_t allowedRates[6] = {1, 2, 5, 10, 20, 50};

// the rate to use: the one in settings if it's one of the allowed ones, otherwise 10 a second
static uint8_t rateToUse(uint8_t wanted)
{
  for (uint8_t i = 0; i < sizeof(allowedRates); i++)
  {
    if (allowedRates[i] == wanted)
    {
      return wanted;
    }
  }
  return 10;
}

// 0.21.0: the channels when "Active Gauges" is picked - every value the enabled gauge pages show, in gauge (page)
// order: the page's main value always, the second value on a Dual page, the three centre values on a Quad / Custom
// page. Skips empty values and ones already added (two pages showing the same gauge signal log it once), stops at
// LOG_CHANNELS. Fills out[] and returns how many channels it put there.
static uint8_t gaugeChannels(uint8_t *out)
{
  uint8_t count = 0;
  for (uint8_t page = 0; page < MAX_PAGES && count < LOG_CHANNELS; page++)
  {
    if (!settings.pages[page].enabled)
    {
      continue; // page switched off in the web UI: not in the rotation
    }
    // the values this page shows, in the order they appear on the glass
    uint8_t shown[4]; // main + up to 3 more (Quad / Custom centre stack)
    uint8_t shownCount = 0;
    shown[shownCount++] = settings.pages[page].signal; // the main value
    if (settings.pages[page].style == STYLE_DUAL)
    {
      shown[shownCount++] = settings.pages[page].signal2; // the second stacked value
    }
    if (settings.pages[page].style == STYLE_QUAD || settings.pages[page].style == STYLE_TRIPLE ||
        settings.pages[page].style == STYLE_CUSTOM)
    {
      shown[shownCount++] = settings.pages[page].slots[0]; // the three Quad values 2-4 / the Custom centre stack
      shown[shownCount++] = settings.pages[page].slots[1];
      shown[shownCount++] = settings.pages[page].slots[2];
    }
    for (uint8_t i = 0; i < shownCount && count < LOG_CHANNELS; i++)
    {
      const uint8_t signal = shown[i];
      if (signal == SIG_NONE || signal >= SIG_COUNT)
      {
        continue; // empty value
      }
      bool already = false;
      for (uint8_t j = 0; j < count; j++)
      {
        if (out[j] == signal)
        {
          already = true; // this value is already being logged (from an earlier page)
          break;
        }
      }
      if (!already)
      {
        out[count++] = signal;
      }
    }
  }
  return count;
}

// takes PSRAM for the recording, as much as can be spared up to LOG_BUFFER_MAX. Keeps what it already has.
static bool takeMemory()
{
  if (ring != NULL)
  {
    return true; // already have it from the last recording
  }
  const size_t largestFree = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); // biggest single piece of PSRAM free
  if (largestFree < LOG_PSRAM_SPARE + LOG_BUFFER_MIN)
  {
    snprintf(status.error, sizeof(status.error), "not enough free memory (%u KB)", (unsigned)(largestFree / 1024));
    return false;
  }
  size_t size = largestFree - LOG_PSRAM_SPARE; // leave the spare for the gauge itself
  if (size > LOG_BUFFER_MAX)
  {
    size = LOG_BUFFER_MAX;
  }
  ring = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM); // the recording lives in PSRAM
  if (ring == NULL)
  {
    copyStr(status.error, sizeof(status.error), "couldn't take memory for the recording");
    return false;
  }
  status.bufferBytes = size;
  DEBUG("Logger: %u KB of PSRAM for recording", (unsigned)(size / 1024));
  return true;
}

// one sample: the time, then every channel's value (NaN when the value has gone stale). Runs in the logger task.
static void takeSample()
{
  uint8_t record[4 + 4 * LOG_CHANNELS]; // built here first, then copied into the ring in one go
  const uint32_t nowMs = millis();
  memcpy(&record[0], &nowMs, 4); // bytes 0-3: the time in ms (little-endian, as the ESP32 stores it)
  for (uint8_t channel = 0; channel < status.channelCount; channel++)
  {
    const signalId_t signal = (signalId_t)status.signals[channel];
    float value = NAN; // "not a number": nothing fresh from this value's source
    if (signalValid(signal))
    {
      value = signalGet(signal);
    }
    memcpy(&record[4 + 4 * channel], &value, 4); // 4 bytes per channel, a float each
  }

  xSemaphoreTake(logLock, portMAX_DELAY);
  if (status.running && ring != NULL && status.capacity > 0)
  {
    const uint32_t slot = status.total % status.capacity; // where this sample goes: the oldest is written over
    memcpy(ring + (size_t)slot * status.recordBytes, record, status.recordBytes);
    status.total++;
  }
  xSemaphoreGive(logLock);
}

// the logger task: a sample every 1 / rate seconds while recording, nothing while the ignition is off
static void loggerTaskFn(void *parameter)
{
  (void)parameter;
  TickType_t lastWake = xTaskGetTickCount();
  while (1)
  {
    uint32_t periodMs = 100; // not recording: look again in 100 ms
    if (status.running && status.rateHz > 0)
    {
      periodMs = 1000 / status.rateHz;
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(periodMs)); // even spacing, however long the sample took
    status.paused = status.running && runtime.standby;
    if (status.running && !runtime.standby)
    {
      takeSample();
    }
  }
}

void setupLogger()
{
  DEBUG("Setting up the data logger...");
  memset(&status, 0, sizeof(status));
  logLock = xSemaphoreCreateMutex();
  // stack in PSRAM (saves internal RAM for WiFi and the web server): this task only reads signals, never flash
  xTaskCreatePinnedToCoreWithCaps(loggerTaskFn,       // task function
                                  "logger",           // name (shows in crash reports)
                                  3072,               // stack size in bytes
                                  NULL,               // no parameter
                                  2,                  // priority (below CAN / K-line / diagnostics)
                                  &loggerTask,        // handle
                                  0,                  // core 0 (LVGL has core 1)
                                  MALLOC_CAP_SPIRAM); // stack in PSRAM
  if (settings.logAutoStart)
  {
    loggerStart(); // Auto Start: recording from power-up
  }
  DEBUG("Set up the data logger!");
}

bool loggerStart()
{
  xSemaphoreTake(logLock, portMAX_DELAY);
  status.running = false;
  status.error[0] = '\0';
  bool ok = takeMemory();
  // the channels to record. 0.21.0: by default the values the active gauge pages show, in gauge order; or, if the Log
  // tab is set to "Chosen Below", every settings slot that has a value picked, in order.
  uint8_t channelCount = 0;
  if (settings.logManualChannels)
  {
    for (uint8_t slot = 0; slot < LOG_CHANNELS; slot++)
    {
      const signalId_t signal = (signalId_t)settings.logSignals[slot];
      if (signal == SIG_NONE || signal >= SIG_COUNT)
      {
        continue; // slot not used
      }
      status.signals[channelCount] = signal;
      channelCount++;
    }
  }
  else
  {
    channelCount = gaugeChannels(status.signals); // the active gauge pages' values, in gauge order
  }
  // each channel's name and unit at Start (a user signal's name is its page label, e.g. "rl_w")
  for (uint8_t channel = 0; channel < channelCount; channel++)
  {
    const signalId_t signal = (signalId_t)status.signals[channel];
    copyStr(status.names[channel], NAME_LEN, signalName(signal));
    copyStr(status.units[channel], UNIT_LEN, signalUnit(signal));
  }
  if (ok && channelCount == 0)
  {
    copyStr(status.error, sizeof(status.error),
            settings.logManualChannels ? "pick at least one value to record" : "no gauge pages are switched on");
    ok = false;
  }
  if (ok)
  {
    status.channelCount = channelCount;
    status.rateHz = rateToUse(settings.logRateHz);
    status.recordBytes = 4 + 4 * channelCount;
    status.capacity = status.bufferBytes / status.recordBytes;
    status.total = 0;
    status.startMs = millis();
    status.session++;
    status.running = true;
    DEBUG("Logger: recording %u values at %u Hz, room for %lu samples", channelCount, status.rateHz,
          (unsigned long)status.capacity);
  }
  xSemaphoreGive(logLock);
  return ok;
}

void loggerStop()
{
  xSemaphoreTake(logLock, portMAX_DELAY);
  status.running = false;
  status.paused = false;
  xSemaphoreGive(logLock);
  DEBUG("Logger: stopped");
}

void loggerClear()
{
  xSemaphoreTake(logLock, portMAX_DELAY);
  status.running = false;
  status.paused = false;
  status.total = 0;
  status.capacity = 0;
  status.channelCount = 0;
  status.session++;
  if (ring != NULL)
  {
    heap_caps_free(ring); // the PSRAM goes back to the gauge
    ring = NULL;
  }
  status.bufferBytes = 0;
  xSemaphoreGive(logLock);
  DEBUG("Logger: cleared");
}

uint32_t loggerMemoryBytes()
{
  if (ring != NULL)
  {
    return status.bufferBytes; // a recording already holds its memory: that's the room
  }
  // nothing taken yet: what takeMemory() would get if Start were pressed now
  const size_t largestFree = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); // biggest single piece of PSRAM free
  if (largestFree < LOG_PSRAM_SPARE + LOG_BUFFER_MIN)
  {
    return 0; // not enough to record at all
  }
  size_t size = largestFree - LOG_PSRAM_SPARE; // leave the spare for the gauge itself
  if (size > LOG_BUFFER_MAX)
  {
    size = LOG_BUFFER_MAX;
  }
  return size;
}

void loggerStatusCopy(LoggerStatus &out)
{
  xSemaphoreTake(logLock, portMAX_DELAY);
  out = status;
  xSemaphoreGive(logLock);
}

void loggerSend(Print &out, uint32_t from, uint32_t maxCount)
{
  xSemaphoreTake(logLock, portMAX_DELAY);
  // the samples still in memory: from oldestKept up to (not including) total
  uint32_t oldestKept = 0;
  if (status.total > status.capacity)
  {
    oldestKept = status.total - status.capacity; // older ones have been written over
  }
  if (from < oldestKept)
  {
    from = oldestKept;
  }
  if (from > status.total)
  {
    from = status.total;
  }
  uint32_t count = status.total - from;
  if (count > maxCount)
  {
    count = maxCount;
  }
  if (status.recordBytes > 0 && count > LOG_SEND_MAX / status.recordBytes)
  {
    count = LOG_SEND_MAX / status.recordBytes; // keep each answer a sensible size
  }
  if (ring == NULL || status.capacity == 0)
  {
    count = 0;
  }

  // 16-byte header (little-endian, as the ESP32 stores numbers)
  uint8_t header[16];
  const uint16_t recordBytes = status.recordBytes;
  const uint16_t channelCount = status.channelCount;
  memcpy(&header[0], &status.session, 4); // bytes 0-3: session
  memcpy(&header[4], &from, 4);           // bytes 4-7: number of the first sample sent
  memcpy(&header[8], &count, 4);          // bytes 8-11: samples sent
  memcpy(&header[12], &recordBytes, 2);   // bytes 12-13: bytes per sample
  memcpy(&header[14], &channelCount, 2);  // bytes 14-15: channels
  out.write(header, sizeof(header));

  if (count > 0)
  {
    // the samples sit in the ring from slot (from % capacity); they may run off its end and carry on at the start
    const uint32_t firstSlot = from % status.capacity;
    uint32_t firstRun = status.capacity - firstSlot; // samples before the end of the ring
    if (firstRun > count)
    {
      firstRun = count;
    }
    out.write(ring + (size_t)firstSlot * recordBytes, (size_t)firstRun * recordBytes);
    if (count > firstRun)
    {
      out.write(ring, (size_t)(count - firstRun) * recordBytes); // the rest, from the start of the ring
    }
  }
  xSemaphoreGive(logLock);
}
