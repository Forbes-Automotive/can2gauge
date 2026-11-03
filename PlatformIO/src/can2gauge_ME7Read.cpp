// can2gauge ME7 flash read - the glue between the ME7Read library, the K-line task, the gauge screen and the web API.
// All the protocol lives in lib/ME7Read; this file only schedules it and reports progress.
//
// To remove the whole feature: delete this file, lib/ME7Read/, the me7read.js include and Read-ECU card in data/, the
// ME7Read block in can2gauge_API.cpp and the one me7ReadService() call in can2gauge_KLine.cpp (there is no setup call).

#include <can2gauge_ME7Read.h>
#include <ME7Read.h>
#include <GaugeUI.h>       // uiProgress: the progress ring on the screen
#include <LittleFS.h>      // the finished read saved as a .bin
#include <can2gauge_API.h> // fsUsage / fsUsageChanged

// ===== Settings for this file =====
#define ME7READ_FS_SPARE (64 * 1024) // room left on the filesystem after saving a read (logs, settings files)

// ===== File variables (static: private to this file) =====
static volatile bool isReadPending = false; // a read is queued (volatile: the web server sets it, the K-line task reads it)
static char lastFile[48] = "";              // the .bin the last read was saved to ("" = not saved, it's only in memory)
static Me7ReadParams pendingParams;         // what the queued read asks for (baud, where to save it ...)

// ===== Functions in this file =====
static void detailLine(char *buf, size_t len);       // the progress ring's one-line detail (KB read, time left)
static bool saveDump(char *path, size_t pathLength); // the finished read saved as a .bin, if the filesystem has room

// queues a read for the K-line task; false if one is already running
bool me7ReadRequest(const Me7ReadParams &params)
{
  if (me7ReadBusy())
  {
    return false;
  }
  pendingParams = params;
  isReadPending = true;
  return true;
}

// a read has been queued but not started yet
bool me7ReadPending()
{
  return isReadPending;
}

// Runs a whole read to completion. Called from the K-line task (which owns the K-line) when a read is pending,
// so no other protocol touches the bus meanwhile. Blocks that task for the length of the read.
void me7ReadService()
{
  if (!isReadPending)
  {
    return;
  }
  isReadPending = false;
  if (!me7ReadStart(pendingParams))
  {
    return;
  }
  lastFile[0] = '\0'; // the old read is replaced by this one

  uiProgress("READ ECU", "connecting", 0);
  char detail[64];
  uint8_t lastPercent = 255;
  me7ReadState_t lastState = M7R_IDLE;
  while (me7ReadPump())
  {
    const Me7ReadStatus &status = me7ReadStatusGet();
    if (status.percent != lastPercent || status.state != lastState)
    {
      detailLine(detail, sizeof(detail));
      // the ring's title: what the read is doing now
      const char *title = "READ ECU";
      if (status.state == M7R_READING)
      {
        title = "READING FLASH";
      }
      uiProgress(title, detail, status.percent);
      lastPercent = status.percent;
      lastState = status.state;
    }
  }

  const Me7ReadStatus &status = me7ReadStatusGet();
  if (status.state == M7R_DONE)
  {
    char doneText[64];
    if (saveDump(lastFile, sizeof(lastFile)))
    {
      snprintf(doneText, sizeof(doneText), "%s\n%lu KB saved", status.message, (unsigned long)(status.bytesRead / 1024));
    }
    else
    {
      snprintf(doneText, sizeof(doneText), "%s\n%lu KB - download it now", status.message,
               (unsigned long)(status.bytesRead / 1024));
    }
    uiProgress("READ COMPLETE", doneText, 100, false, 6000);
  }
  else
  {
    uiProgress("READ FAILED", status.message, status.percent, true, 8000);
  }
}

// the .bin the last read was saved to, "" if none / only in memory
const char *me7ReadLastFile()
{
  return lastFile;
}

// human-readable one-liner for the gauge ring
static void detailLine(char *buf, size_t len)
{
  const Me7ReadStatus &status = me7ReadStatusGet();
  if (status.state == M7R_READING && status.etaMs)
  {
    snprintf(buf, len, "%lu / %lu KB  ~%lus left",
             (unsigned long)(status.bytesRead / 1024), (unsigned long)(status.bytesTotal / 1024),
             (unsigned long)(status.etaMs / 1000));
  }
  else
  {
    snprintf(buf, len, "%s", status.message);
  }
}

// Saves the finished read (in PSRAM) as a .bin if the filesystem has room for it plus ME7READ_FS_SPARE.
// true = saved, path filled in. With the web UI image on the gauge there's usually ~330 KB free - too little for a
// 512 KB / 1 MB dump - so it then stays in memory and the web UI / console download it from there.
static bool saveDump(char *path, size_t pathLength)
{
  path[0] = '\0';
  uint32_t length = 0;
  const uint8_t *dump = me7ReadDump(length);
  if (dump == NULL || length == 0)
  {
    return false;
  }
  size_t totalBytes = 0;
  size_t usedBytes = 0;
  fsUsage(totalBytes, usedBytes); // the house task's numbers (LittleFS.usedBytes() walks the whole filesystem)
  const size_t freeBytes = totalBytes - usedBytes;
  if (length + ME7READ_FS_SPARE > freeBytes)
  {
    DEBUG("ME7 read: %lu KB kept in memory (only %lu KB free on the filesystem)", (unsigned long)(length / 1024),
          (unsigned long)(freeBytes / 1024));
    return false;
  }
  if (!LittleFS.exists(pendingParams.outDir))
  {
    LittleFS.mkdir(pendingParams.outDir);
  }
  snprintf(path, pathLength, "%s/me7_%08lX.bin", pendingParams.outDir, (unsigned long)millis());
  File file = LittleFS.open(path, "w");
  if (!file)
  {
    path[0] = '\0';
    return false;
  }
  const size_t written = file.write(dump, length);
  file.close();
  fsUsageChanged(); // the space used changed
  if (written != length)
  {
    LittleFS.remove(path); // a short file is worse than none
    path[0] = '\0';
    return false;
  }
  return true;
}
