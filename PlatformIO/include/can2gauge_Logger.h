#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// Data logger (0.16.0) - records up to LOG_CHANNELS of the gauge's values (any signal: CAN, K-line, OBD, diagnostics, ME7,
// adc2can, usb2ip ...) side by side, 1 to 50 times a second, into PSRAM. Nothing goes to flash: the web UI's Log tab
// draws the recording as graphs and saves it to the phone as a CSV file. A power cut loses it.
//
// The recording is a ring: when it is full the oldest samples are written over, so it always holds the latest
// stretch (how long depends on the channels and the rate - the Log tab shows it).
// One sample = the time (millis(), 4 bytes) then each channel's value as a float (4 bytes each). A value that has
// gone stale (its source stopped sending) is stored as NaN ("not a number"), which the graphs show as a gap.
// ---------------------------------------------------------------------------

#define LOG_BUFFER_MAX (4 * 1024 * 1024) // most PSRAM a recording takes, in bytes (16 channels at 10 Hz = 1 h 40 min)
#define LOG_PSRAM_SPARE (1536 * 1024)    // PSRAM always left free for the gauge itself (sprite pages, logos, web pages)
#define LOG_BUFFER_MIN (64 * 1024)       // less PSRAM than this to spare = no recording
#define LOG_SEND_MAX 65536               // most bytes of the recording sent to the web UI in one answer (64 KB)

// what the web UI's Log tab shows (copied out by loggerStatusCopy)
struct LoggerStatus
{
  bool running;                         // recording now
  bool paused;                          // recording, but the ignition is off (standby): no samples are taken
  uint8_t channelCount;                 // channels in this recording
  uint8_t signals[LOG_CHANNELS];        // each channel's signal (copied from settings.logSignals at Start)
  char names[LOG_CHANNELS][NAME_LEN];   // each channel's name at Start ("Boost", "ME7 1" renamed by the .cfg ...)
  char units[LOG_CHANNELS][UNIT_LEN];   // each channel's unit at Start
  uint8_t rateHz;                       // samples a second
  uint16_t recordBytes;                 // bytes per sample: 4 for the time + 4 per channel
  uint32_t capacity;                    // samples the memory holds
  uint32_t total;                       // samples taken since Start (the newest is number total - 1)
  uint32_t startMs;                     // millis() when it started
  uint32_t session;                     // goes up at every Start, so the web UI knows to throw away what it fetched
  uint32_t bufferBytes;                 // PSRAM taken for the recording (0 = none)
  char error[64];                       // why the last Start failed ("" = it didn't)
};

void setupLogger();                          // starts the logger task (and the recording, if Auto Start is on)
bool loggerStart();                          // a new recording with the channels / rate in settings; false = see .error
void loggerStop();                           // stops recording (what was recorded stays until the next Start / Clear)
void loggerClear();                          // stops, forgets the recording and gives its PSRAM back
void loggerStatusCopy(LoggerStatus &out);    // a copy of the status, safe to read from the web server
uint32_t loggerMemoryBytes();                // 0.17.0: PSRAM the recording has, or would get at Start now (0 = none)
// writes samples from number "from" onward (at most maxCount, and at most LOG_SEND_MAX bytes) to out, after a 16-byte
// header: session, first sample number, sample count (uint32 each), bytes per sample, channels (uint16 each).
// Samples already written over are skipped, so the first sample number can be later than asked for.
void loggerSend(Print &out, uint32_t from, uint32_t maxCount);
