#include <can2gauge_ChainUpdate.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_Link.h>    // linkNodes(), linkEspNowSendTo(): the chain's ESP-NOW stack
#include <can2gauge_signals.h> // RPM / road speed: parked check
#include <GaugeUI.h>           // the progress ring on the glass of the gauge being updated
#include <Display_ST7701.h>    // 0.36.9: displaySetUpdateClock() - a slower screen clock while flash is written
#include <ota_manager.h>       // otaInProgress(): never at the same time as an update from the web page
#include <LittleFS.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h> // esp_image_get_metadata(): how long our firmware is
#include <esp_random.h>
#include <esp_heap_caps.h> // heap_caps_get_free_size(): the internal RAM numbers in the log if the task can't be made
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

// ---------------------------------------------------------------------------
// How a chain update goes (one gauge sends, one gauge receives; every request is answered before the next goes):
//   HELLO      who we are, board, version, firmware length + fingerprint, how many files.     Signed with the chain key.
//              answer: OK = send the firmware, SAME = it already runs this firmware, or why not (busy, driving ...)
//   FW_DATA    1 KB of firmware at an offset.            answer: the next offset it wants (a lost packet is just sent again)
//   FW_DONE    all sent.                                 answer: OK once the image checks out (not switched over yet)
//   FILE       a file's path, size and fingerprint.      Signed. answer: SAME (already has it) or SEND
//   FILE_DATA  1 KB of the file at an offset.            answer: the next offset it wants
//   FILE_DONE  all sent.                                 answer: OK once the file checks out and is in place
//   FINISH     switch to the new firmware and restart (only if anything changed)
//   ABORT      give up: the receiver throws away what it has so far
// The web files go before the restart, so a gauge never starts the new firmware with the old web page.
// ---------------------------------------------------------------------------

#define UPD_MAGIC0 'C' // first byte of every chain-update packet
#define UPD_MAGIC1 'U' // second byte ('C' '2' is the chain's own packets, 'C' 'N' the CAN relay)
#define UPD_CHUNK 1024 // bytes of firmware / file per packet (ESP-NOW v2 takes up to 1470)
#define UPD_PATH_LEN 64 // longest file path sent (the release's longest is ~40)
#define UPD_HMAC_LEN 16 // bytes of the key code on HELLO and FILE
#define UPD_MANIFEST "/fs_manifest.txt" // the list of the release's own files (tools/fs_pack.py)
#define UPD_TEMP_FILE "/chainupd.tmp"   // a file being received; renamed into place once it checks out
#define UPD_MAX_FILES 400 // most files in one release (163 in 0.25.0)
#define UPD_TARGET_TIMEOUT_MS 20000 // receiver: nothing heard from the sender this long = the job is dropped
#define UPD_RESTART_WAIT_MS 90000   // sender: how long to wait for an updated gauge to come back after its restart

// packet types: requests (gauge sending the update -> gauge being updated)
#define UPD_HELLO 1
#define UPD_FW_DATA 2
#define UPD_FW_DONE 3
#define UPD_FILE 4
#define UPD_FILE_DATA 5
#define UPD_FILE_DONE 6
#define UPD_FINISH 7
#define UPD_ABORT 8
// ... and the answer to every one of them
#define UPD_REPLY 20

// answer codes
#define UPD_OK 0           // done / go ahead
#define UPD_SAME 1         // HELLO: already runs this firmware; FILE: already has this file
#define UPD_SEND 2         // FILE: missing or different, send it
#define UPD_BUSY 3         // already in an update (or a web page update)
#define UPD_WRONG_BOARD 4  // firmware built for another board
#define UPD_DRIVING 5      // engine running or the car moving
#define UPD_BAD_KEY 6      // the key code didn't match (0.28.3: only a gauge still on older firmware, with its own key)
#define UPD_NEWER 7        // it has newer firmware than ours
#define UPD_NO_SESSION 8   // a packet for a job it doesn't have (it restarted, or timed out)
#define UPD_WRITE_FAILED 9 // flash / file write failed
#define UPD_BAD_IMAGE 10   // it all arrived but the fingerprint didn't match
#define UPD_CHAIN_OFF 11   // the chain is off on that gauge

// packed: these go over the air byte for byte, so the compiler mustn't add padding between the fields
struct __attribute__((packed)) UpdHeader
{
  uint8_t magic[2]; // 'C' 'U'
  uint8_t type;     // UPD_*
  uint16_t group;   // chain group (settings.chainGroup)
  uint8_t node;     // sender's node id
  uint16_t session; // a random number for this job
  uint16_t request; // request number: the answer carries the same one, a repeat of a lost request keeps it
};

struct __attribute__((packed)) UpdHello
{
  UpdHeader header;
  uint8_t board;              // chainUpdateBoardId(): the firmware only suits the same board
  uint8_t version[3];         // major, minor, patch
  uint32_t imageLength;       // firmware length in bytes
  uint8_t appHash[32];        // the firmware's fingerprint (esp_partition_get_sha256 of our running slot)
  uint16_t fileCount;         // files that will be checked
  uint8_t hmac[UPD_HMAC_LEN]; // key code over everything before it
};

struct __attribute__((packed)) UpdData // FW_DATA and FILE_DATA
{
  UpdHeader header;
  uint32_t offset;           // where these bytes go
  uint16_t length;           // how many (up to UPD_CHUNK)
  uint8_t bytes[UPD_CHUNK];
};

struct __attribute__((packed)) UpdFile
{
  UpdHeader header;
  uint16_t index;             // this file's number (0 = first), for the progress ring
  uint16_t count;             // files in the job
  uint32_t size;              // bytes
  uint8_t hash[32];           // SHA-256 of the file
  char path[UPD_PATH_LEN];    // e.g. "/logos/vw.bin.gz"
  uint8_t hmac[UPD_HMAC_LEN]; // key code over everything before it
};

struct __attribute__((packed)) UpdReply
{
  UpdHeader header;
  uint8_t status; // UPD_OK ... (answer codes above)
  uint32_t value; // HELLO: its version (major << 16 | minor << 8 | patch); data: the next offset it wants
};

// a packet as it arrived (copied out of the WiFi task into the queue)
struct UpdPacket
{
  uint8_t mac[6];  // who sent it
  uint16_t length; // bytes in data
  uint8_t data[sizeof(UpdData)]; // the biggest packet is a data packet
};

// a gauge to update (taken from the chain list when the job starts)
struct UpdTarget
{
  uint8_t node;
  uint8_t mac[6];
  char name[NAME_LEN];
};

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static QueueHandle_t packetQueue = NULL;  // packets from the WiFi task to the update task
static volatile bool taskRunning = false; // the update task exists (0.25.3: made once at boot and kept)
static volatile bool sendRequested = false; // web UI pressed "Update Other Gauges"
static volatile bool stopRequested = false; // web UI pressed "Stop"
static volatile bool sending = false;       // this gauge is updating the others
static volatile bool receiving = false;     // this gauge is being updated
EXT_RAM_BSS_ATTR static ChainUpdateStatus status;            // for the web UI
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED; // the web server reads status while the task writes it

// static: one update task at a time, and these are too big for its stack
EXT_RAM_BSS_ATTR static UpdPacket receivedPacket; // the packet being handled
EXT_RAM_BSS_ATTR static UpdPacket incomingPacket; // the WiFi task's copy before it goes in the queue (only the WiFi task uses it)
EXT_RAM_BSS_ATTR static UpdData dataPacket;       // a data packet being built (sender)
EXT_RAM_BSS_ATTR static uint8_t fileBuffer[UPD_CHUNK]; // reading files to fingerprint them

// receiver
static uint8_t senderMac[6];        // the gauge updating us
static uint8_t senderNode = 0;      // its node id (for the glass)
static uint16_t targetSession = 0;  // its job number
static uint32_t lastPacketMs = 0;   // last packet heard from it
static uint16_t lastRequest = 0;    // last request answered ...
static bool haveLastReply = false;
static UpdReply lastReply;          // ... and the answer, sent again if the request comes again
static bool firmwareNeeded = false; // HELLO: our firmware differs from the sender's
static uint32_t firmwareLength = 0; // bytes the firmware will be
static uint32_t firmwareNext = 0;   // next firmware offset we want
static uint8_t firmwareHash[32];    // the fingerprint it must end up with
static bool otaOpen = false;        // writing into the spare slot
static bool firmwareReady = false;  // all in, checked, ready to switch over on FINISH
static esp_ota_handle_t otaHandle = 0;
static const esp_partition_t *otaPartition = NULL; // the spare slot (an ESP-IDF handle, only passed back to ESP-IDF)
static bool fileOpen = false;       // receiving a file into UPD_TEMP_FILE
static File tempFile;
static char filePath[UPD_PATH_LEN]; // where it goes once it checks out
static uint32_t fileSize = 0;
static uint32_t fileNext = 0;       // next file offset we want
static uint8_t fileHash[32];        // the fingerprint it must end up with
static mbedtls_sha256_context fileSha; // fingerprint of what has arrived so far
static uint16_t fileIndex = 0;      // for the progress ring
static uint16_t fileCount = 0;
static bool anythingChanged = false; // a file was replaced (restart at FINISH even with the same firmware)
static int lastPercentShown = -1;

// sender
static uint16_t sendSession = 0;
static uint16_t requestCounter = 0;
static uint32_t imageLength = 0;
static uint8_t appHash[32];
static char *filePaths = NULL;     // the release's files, in PSRAM: UPD_PATH_LEN bytes per name, one after another
static uint8_t *fileHashes = NULL; // their fingerprints, 32 bytes each (worked out once per job)
static uint32_t *fileSizes = NULL; // their sizes
static uint16_t pathCount = 0;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
// file number i's name / fingerprint in the lists above
static char *pathAt(uint16_t i)
{
  return filePaths + i * UPD_PATH_LEN;
}

static uint8_t *hashAt(uint16_t i)
{
  return fileHashes + i * 32;
}

uint8_t chainUpdateBoardId()
{
#ifdef CAN2GAUGE_WAVESHARE
  return 2; // the Waveshare dev board build
#else
  return 1; // the can2gauge board
#endif
}

void chainUpdateVersion(uint8_t version[3])
{
  unsigned versionMajor = 0;
  unsigned versionMinor = 0;
  unsigned versionPatch = 0;
  sscanf(CAN2GAUGE_VERSION, "%u.%u.%u", &versionMajor, &versionMinor, &versionPatch);
  version[0] = (uint8_t)versionMajor;
  version[1] = (uint8_t)versionMinor;
  version[2] = (uint8_t)versionPatch;
}

// a version as one number, so two can be compared: 0.25.1 -> 0x001901
static uint32_t versionNumber(const uint8_t version[3])
{
  return ((uint32_t)version[0] << 16) | ((uint32_t)version[1] << 8) | version[2];
}

// the key code: HMAC-SHA256 with the chain's built-in key (CHAIN_KEY, 0.28.3), first 16 bytes
static void keyCode(const uint8_t *data, size_t length, uint8_t out[UPD_HMAC_LEN])
{
  uint8_t fullHash[32];
  const mbedtls_md_info_t *hashType = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(hashType, (const uint8_t *)CHAIN_KEY, strlen(CHAIN_KEY), data, length, fullHash);
  memcpy(out, fullHash, UPD_HMAC_LEN);
}

static void fillUpdHeader(UpdHeader &header, uint8_t type, uint16_t session, uint16_t request)
{
  header.magic[0] = UPD_MAGIC0;
  header.magic[1] = UPD_MAGIC1;
  header.type = type;
  header.group = settings.chainGroup;
  header.node = linkNodeId();
  header.session = session;
  header.request = request;
}

// engine running or the car moving: no updates
static bool carIsMoving()
{
  if (signalValid(SIG_RPM) && signalGet(SIG_RPM) > 50)
  {
    return true; // engine running
  }
  if (signalValid(SIG_SPEED) && signalGet(SIG_SPEED) > 2)
  {
    return true; // rolling
  }
  return false;
}

// SHA-256 of a file on the gauge. False if it isn't there.
static bool hashFile(const char *path, uint8_t out[32], uint32_t &size)
{
  File file = LittleFS.open(path, "r");
  if (!file)
  {
    return false;
  }
  size = file.size();
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0); // 0 = SHA-256 (1 would be SHA-224)
  uint16_t piecesRead = 0;
  while (1)
  {
    const int got = file.read(fileBuffer, sizeof(fileBuffer));
    if (got <= 0)
    {
      break;
    }
    mbedtls_sha256_update(&sha, fileBuffer, got);
    piecesRead++;
    if (piecesRead % 32 == 0)
    {
      vTaskDelay(1); // a big file takes seconds: let core 0's idle task in, or the watchdog bites
    }
  }
  mbedtls_sha256_finish(&sha, out);
  mbedtls_sha256_free(&sha);
  file.close();
  return true;
}

// make every folder on the way to a file ("/logos/vw.bin.gz" -> "/logos")
static void makeFoldersFor(const char *path)
{
  char folder[UPD_PATH_LEN];
  for (uint8_t i = 1; path[i] != '\0' && i < UPD_PATH_LEN - 1; i++)
  {
    if (path[i] == '/')
    {
      memcpy(folder, path, i);
      folder[i] = '\0';
      if (!LittleFS.exists(folder))
      {
        LittleFS.mkdir(folder);
      }
    }
  }
}

// a path from another gauge: only plain absolute paths, never our temp file, never "..", always ends with a 0
static bool pathIsSafe(char path[UPD_PATH_LEN])
{
  path[UPD_PATH_LEN - 1] = '\0';
  if (path[0] != '/' || path[1] == '\0')
  {
    return false;
  }
  if (strstr(path, "..") != NULL)
  {
    return false;
  }
  if (strcmp(path, UPD_TEMP_FILE) == 0)
  {
    return false;
  }
  return true;
}

static void setStatusText(const char *text)
{
  portENTER_CRITICAL(&statusMux);
  copyStr(status.text, sizeof(status.text), text);
  portEXIT_CRITICAL(&statusMux);
}

static void setStatusPercent(uint8_t percent)
{
  portENTER_CRITICAL(&statusMux);
  status.percent = percent;
  portEXIT_CRITICAL(&statusMux);
}

// ---------------------------------------------------------------------------
// receive (WiFi task - short: copy the packet into the queue and start the update task if it isn't running)
// ---------------------------------------------------------------------------
static void updateTask(void *);

// false = the task couldn't be made at boot (internal RAM short)
// 0.25.3: the task is made once in setupChainUpdate() and kept. It used to be made when the button was pressed (or a
// HELLO came in) and ended when idle, but by then the internal RAM was too broken up to find 8 KB in one piece, even
// with Bluetooth off ("not enough free RAM"). Kept, it costs the same 8 KB all the time but is always there.
static bool startTask()
{
  return taskRunning;
}

void chainUpdateOnEspNow(const uint8_t *mac, const uint8_t *data, int len)
{
  if (mac == NULL || packetQueue == NULL || len < (int)sizeof(UpdHeader) || len > (int)sizeof(incomingPacket.data))
  {
    return;
  }
  if (!settings.chainEnabled)
  {
    return; // chain off: not listening for updates (the sender is told nothing and gives up)
  }
  memcpy(incomingPacket.mac, mac, 6);
  incomingPacket.length = (uint16_t)len;
  memcpy(incomingPacket.data, data, len);
  xQueueSend(packetQueue, &incomingPacket, 0); // queue full = dropped; the sender asks again
  if (data[2] == UPD_HELLO)
  {
    startTask(); // someone wants to update us: the task (made at boot) answers, even if only "busy"
  }
}

// ---------------------------------------------------------------------------
// receiver: being updated
// ---------------------------------------------------------------------------
static void sendReply(const uint8_t mac[6], const UpdHeader &request, uint8_t code, uint32_t value)
{
  UpdReply reply;
  fillUpdHeader(reply.header, UPD_REPLY, request.session, request.request);
  reply.status = code;
  reply.value = value;
  if (request.session == targetSession && memcmp(mac, senderMac, 6) == 0)
  {
    memcpy(&lastReply, &reply, sizeof(reply)); // kept, in case this request comes again
    lastRequest = request.request;
    haveLastReply = true;
  }
  linkEspNowSendTo(mac, &reply, sizeof(reply));
}

// the progress ring on our glass: firmware 0-70 %, files the rest (all 100 % when the firmware is the same)
static void showReceiveProgress()
{
  int percent = 0;
  if (firmwareNeeded)
  {
    if (firmwareLength > 0)
    {
      percent = (int)((uint64_t)firmwareNext * 70 / firmwareLength);
    }
    if (fileCount > 0 && fileIndex > 0)
    {
      percent = 70 + fileIndex * 30 / fileCount;
    }
  }
  else if (fileCount > 0)
  {
    percent = fileIndex * 100 / fileCount;
  }
  if (percent == lastPercentShown)
  {
    return;
  }
  lastPercentShown = percent;
  char detail[40];
  snprintf(detail, sizeof(detail), "from G%u - keep it powered", senderNode);
  uiProgress("UPDATING", detail, percent);
  setStatusPercent((uint8_t)percent);
}

// throw away whatever was half received
static void dropTargetJob(const char *why)
{
  if (otaOpen)
  {
    esp_ota_abort(otaHandle);
    otaOpen = false;
  }
  if (fileOpen)
  {
    tempFile.close();
    fileOpen = false;
    mbedtls_sha256_free(&fileSha);
  }
  if (LittleFS.exists(UPD_TEMP_FILE))
  {
    LittleFS.remove(UPD_TEMP_FILE);
  }
  if (receiving && why != NULL)
  {
    DEBUG("CHAIN UPDATE: dropped - %s", why);
    uiProgress("UPDATE STOPPED", why, 0, true, 5000);
    setStatusText(why);
  }
  if (receiving)
  {
    displaySetUpdateClock(false); // 0.36.9: the screen's normal clock back
  }
  firmwareReady = false;
  receiving = false;
  targetSession = 0;
  haveLastReply = false;
}

// HELLO: someone wants to update us
static void handleHello(const UpdPacket &packet)
{
  UpdHello hello;
  memset(&hello, 0, sizeof(hello));
  memcpy(&hello, packet.data, min((size_t)packet.length, sizeof(hello)));
  uint8_t ourVersion[3];
  chainUpdateVersion(ourVersion);
  const uint32_t ourVersionNumber = versionNumber(ourVersion);

  // the same job asking again (our answer got lost): answer the same
  if (receiving && hello.header.session == targetSession && memcmp(packet.mac, senderMac, 6) == 0)
  {
    if (haveLastReply && hello.header.request == lastRequest)
    {
      linkEspNowSendTo(packet.mac, &lastReply, sizeof(lastReply));
    }
    return;
  }

  if (packet.length < sizeof(UpdHello))
  {
    return; // not a whole HELLO
  }
  uint8_t expected[UPD_HMAC_LEN];
  keyCode(packet.data, offsetof(UpdHello, hmac), expected);
  if (memcmp(expected, hello.hmac, UPD_HMAC_LEN) != 0)
  {
    sendReply(packet.mac, hello.header, UPD_BAD_KEY, ourVersionNumber);
    return;
  }
  if (receiving && millis() - lastPacketMs < UPD_TARGET_TIMEOUT_MS)
  {
    sendReply(packet.mac, hello.header, UPD_BUSY, ourVersionNumber); // already being updated by another job
    return;
  }
  if (sending || otaInProgress())
  {
    sendReply(packet.mac, hello.header, UPD_BUSY, ourVersionNumber); // busy updating others, or a web page update
    return;
  }
  if (hello.board != chainUpdateBoardId())
  {
    sendReply(packet.mac, hello.header, UPD_WRONG_BOARD, ourVersionNumber);
    return;
  }
  if (carIsMoving())
  {
    sendReply(packet.mac, hello.header, UPD_DRIVING, ourVersionNumber);
    return;
  }
  if (versionNumber(hello.version) < ourVersionNumber)
  {
    sendReply(packet.mac, hello.header, UPD_NEWER, ourVersionNumber); // never go back to older firmware this way
    return;
  }

  // take the job
  dropTargetJob(NULL); // anything left from an old job
  memcpy(senderMac, packet.mac, 6);
  senderNode = hello.header.node;
  targetSession = hello.header.session;
  lastPacketMs = millis();
  receiving = true;
  displaySetUpdateClock(true); // 0.36.9: slower screen clock while flash is written (no lines on the screen)
  DEBUG("CHAIN UPDATE: screen clock slowed for the update");
  firmwareLength = hello.imageLength;
  firmwareNext = 0;
  memcpy(firmwareHash, hello.appHash, 32);
  fileIndex = 0;
  fileCount = hello.fileCount;
  anythingChanged = false;
  lastPercentShown = -1;

  // the same firmware already? (the same fingerprint as our running slot)
  uint8_t runningHash[32];
  esp_partition_get_sha256(esp_ota_get_running_partition(), runningHash);
  firmwareNeeded = memcmp(runningHash, hello.appHash, 32) != 0;
  DEBUG("CHAIN UPDATE: G%u is updating us (v%u.%u.%u, firmware %s, %u files)", senderNode, hello.version[0],
        hello.version[1], hello.version[2], firmwareNeeded ? "to send" : "the same", fileCount);
  char text[80];
  snprintf(text, sizeof(text), "Being updated by G%u to v%u.%u.%u", senderNode, hello.version[0], hello.version[1],
           hello.version[2]);
  setStatusText(text);
  showReceiveProgress();

  if (!firmwareNeeded)
  {
    sendReply(packet.mac, hello.header, UPD_SAME, ourVersionNumber);
    return;
  }
  otaPartition = esp_ota_get_next_update_partition(NULL); // the spare slot
  // OTA_WITH_SEQUENTIAL_WRITES: the slot is cleared a piece at a time as the data arrives (clearing all 3 MB first
  // would take seconds, and the sender would think we'd gone)
  if (otaPartition == NULL || esp_ota_begin(otaPartition, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle) != ESP_OK)
  {
    sendReply(packet.mac, hello.header, UPD_WRITE_FAILED, ourVersionNumber);
    dropTargetJob("couldn't open the spare firmware slot");
    return;
  }
  otaOpen = true;
  sendReply(packet.mac, hello.header, UPD_OK, ourVersionNumber);
}

// FW_DATA: a piece of firmware
static void handleFirmwareData(const UpdPacket &packet)
{
  UpdHeader header;
  memcpy(&header, packet.data, sizeof(header));
  if (!otaOpen)
  {
    sendReply(packet.mac, header, UPD_NO_SESSION, 0);
    return;
  }
  uint32_t offset = 0;
  uint16_t length = 0;
  memcpy(&offset, packet.data + offsetof(UpdData, offset), 4);
  memcpy(&length, packet.data + offsetof(UpdData, length), 2);
  const uint8_t *bytes = packet.data + offsetof(UpdData, bytes);
  if (length > UPD_CHUNK || offsetof(UpdData, bytes) + length > packet.length)
  {
    return; // cut short in the air
  }
  if (offset == firmwareNext && firmwareNext + length <= firmwareLength)
  {
    if (esp_ota_write(otaHandle, bytes, length) != ESP_OK)
    {
      sendReply(packet.mac, header, UPD_WRITE_FAILED, firmwareNext);
      dropTargetJob("firmware write failed");
      return;
    }
    firmwareNext = firmwareNext + length;
    showReceiveProgress();
  }
  sendReply(packet.mac, header, UPD_OK, firmwareNext); // any other offset: tell the sender where we are
}

// FW_DONE: check the firmware (not switched over until FINISH)
static void handleFirmwareDone(const UpdPacket &packet)
{
  UpdHeader header;
  memcpy(&header, packet.data, sizeof(header));
  if (!otaOpen || firmwareNext != firmwareLength)
  {
    sendReply(packet.mac, header, UPD_NO_SESSION, firmwareNext);
    return;
  }
  otaOpen = false;
  if (esp_ota_end(otaHandle) != ESP_OK) // ESP-IDF checks the image is whole and valid
  {
    sendReply(packet.mac, header, UPD_BAD_IMAGE, 0);
    dropTargetJob("the firmware didn't check out");
    return;
  }
  uint8_t writtenHash[32];
  esp_partition_get_sha256(otaPartition, writtenHash);
  if (memcmp(writtenHash, firmwareHash, 32) != 0)
  {
    sendReply(packet.mac, header, UPD_BAD_IMAGE, 0);
    dropTargetJob("the firmware's fingerprint didn't match");
    return;
  }
  firmwareReady = true;
  DEBUG("CHAIN UPDATE: firmware in and checked (%lu bytes)", (unsigned long)firmwareLength);
  sendReply(packet.mac, header, UPD_OK, 0);
}

// a new release's list replaces the old: delete the files the old list had and the new one doesn't
// (a release's own files only - each gauge's own files are on neither list)
static void deleteDroppedFiles(const char *newListPath)
{
  File newList = LittleFS.open(newListPath, "r");
  if (!newList)
  {
    return;
  }
  String newPaths = "\n" + newList.readString() + "\n";
  newList.close();
  newPaths.replace("\r", "");
  File oldList = LittleFS.open(UPD_MANIFEST, "r");
  if (!oldList)
  {
    return; // a gauge from before the list: nothing known to delete
  }
  while (oldList.available())
  {
    String path = oldList.readStringUntil('\n');
    path.trim();
    if (path.length() == 0 || path == UPD_MANIFEST || path == UPD_TEMP_FILE)
    {
      continue;
    }
    if (newPaths.indexOf("\n" + path + "\n") < 0 && LittleFS.exists(path))
    {
      LittleFS.remove(path);
      DEBUG("CHAIN UPDATE: removed %s (not in the new release)", path.c_str());
    }
  }
  oldList.close();
}

// FILE: do we have this one already?
static void handleFile(const UpdPacket &packet)
{
  UpdFile file;
  memset(&file, 0, sizeof(file));
  memcpy(&file, packet.data, min((size_t)packet.length, sizeof(file)));
  if (packet.length < sizeof(UpdFile))
  {
    return;
  }
  uint8_t expected[UPD_HMAC_LEN];
  keyCode(packet.data, offsetof(UpdFile, hmac), expected);
  if (memcmp(expected, file.hmac, UPD_HMAC_LEN) != 0 || !pathIsSafe(file.path))
  {
    sendReply(packet.mac, file.header, UPD_BAD_KEY, 0);
    dropTargetJob("a file came with the wrong key");
    return;
  }
  if (fileOpen) // a file left half done (the sender moved on): drop it
  {
    tempFile.close();
    fileOpen = false;
    mbedtls_sha256_free(&fileSha);
  }
  fileIndex = file.index;
  fileCount = file.count;
  showReceiveProgress();

  // the same size? only then is it worth fingerprinting our copy
  bool haveIt = false;
  uint32_t ourSize = 0;
  File existing = LittleFS.open(file.path, "r");
  if (existing)
  {
    haveIt = true;
    ourSize = existing.size();
    existing.close();
  }
  if (haveIt && ourSize == file.size)
  {
    uint8_t ourHash[32];
    if (hashFile(file.path, ourHash, ourSize) && memcmp(ourHash, file.hash, 32) == 0)
    {
      sendReply(packet.mac, file.header, UPD_SAME, 0);
      return;
    }
  }

  // short of room for the new copy next to the old one: the old one goes first (it is being replaced anyway)
  if (haveIt && LittleFS.totalBytes() - LittleFS.usedBytes() < file.size + 16384)
  {
    LittleFS.remove(file.path);
  }
  tempFile = LittleFS.open(UPD_TEMP_FILE, "w");
  if (!tempFile)
  {
    sendReply(packet.mac, file.header, UPD_WRITE_FAILED, 0);
    dropTargetJob("couldn't write a file");
    return;
  }
  fileOpen = true;
  copyStr(filePath, sizeof(filePath), file.path);
  fileSize = file.size;
  fileNext = 0;
  memcpy(fileHash, file.hash, 32);
  mbedtls_sha256_init(&fileSha);
  mbedtls_sha256_starts(&fileSha, 0); // 0 = SHA-256
  sendReply(packet.mac, file.header, UPD_SEND, 0);
}

// FILE_DATA: a piece of the file
static void handleFileData(const UpdPacket &packet)
{
  UpdHeader header;
  memcpy(&header, packet.data, sizeof(header));
  if (!fileOpen)
  {
    sendReply(packet.mac, header, UPD_NO_SESSION, 0);
    return;
  }
  uint32_t offset = 0;
  uint16_t length = 0;
  memcpy(&offset, packet.data + offsetof(UpdData, offset), 4);
  memcpy(&length, packet.data + offsetof(UpdData, length), 2);
  const uint8_t *bytes = packet.data + offsetof(UpdData, bytes);
  if (length > UPD_CHUNK || offsetof(UpdData, bytes) + length > packet.length)
  {
    return; // cut short in the air
  }
  if (offset == fileNext && fileNext + length <= fileSize)
  {
    if (tempFile.write(bytes, length) != length)
    {
      sendReply(packet.mac, header, UPD_WRITE_FAILED, fileNext);
      dropTargetJob("the filesystem is full");
      return;
    }
    mbedtls_sha256_update(&fileSha, bytes, length);
    fileNext = fileNext + length;
  }
  sendReply(packet.mac, header, UPD_OK, fileNext);
}

// FILE_DONE: check it and put it in place
static void handleFileDone(const UpdPacket &packet)
{
  UpdHeader header;
  memcpy(&header, packet.data, sizeof(header));
  if (!fileOpen || fileNext != fileSize)
  {
    sendReply(packet.mac, header, UPD_NO_SESSION, fileNext);
    return;
  }
  tempFile.close();
  fileOpen = false;
  uint8_t gotHash[32];
  mbedtls_sha256_finish(&fileSha, gotHash);
  mbedtls_sha256_free(&fileSha);
  if (memcmp(gotHash, fileHash, 32) != 0)
  {
    LittleFS.remove(UPD_TEMP_FILE);
    sendReply(packet.mac, header, UPD_BAD_IMAGE, 0);
    return; // the sender sends it again
  }
  if (strcmp(filePath, UPD_MANIFEST) == 0)
  {
    deleteDroppedFiles(UPD_TEMP_FILE); // the list comes last: tidy up before it replaces the old one
  }
  if (LittleFS.exists(filePath))
  {
    LittleFS.remove(filePath);
  }
  makeFoldersFor(filePath);
  if (!LittleFS.rename(UPD_TEMP_FILE, filePath))
  {
    sendReply(packet.mac, header, UPD_WRITE_FAILED, 0);
    dropTargetJob("couldn't put a file in place");
    return;
  }
  anythingChanged = true;
  sendReply(packet.mac, header, UPD_OK, 0);
}

// FINISH: switch to the new firmware and restart (or just say "up to date")
static void handleFinish(const UpdPacket &packet)
{
  UpdHeader header;
  memcpy(&header, packet.data, sizeof(header));
  if (firmwareReady && esp_ota_set_boot_partition(otaPartition) != ESP_OK)
  {
    sendReply(packet.mac, header, UPD_WRITE_FAILED, 0);
    dropTargetJob("couldn't switch to the new firmware");
    return;
  }
  bool restart = false;
  if (firmwareReady || anythingChanged)
  {
    restart = true;
  }
  sendReply(packet.mac, header, UPD_OK, restart);
  vTaskDelay(pdMS_TO_TICKS(50));
  linkEspNowSendTo(packet.mac, &lastReply, sizeof(lastReply)); // the answer twice: we may be gone before a repeat
  receiving = false;
  targetSession = 0;
  firmwareReady = false;
  displaySetUpdateClock(false); // 0.36.9: the screen's normal clock back (a restart sets it anyway)
  if (restart)
  {
    DEBUG("CHAIN UPDATE: done - restarting");
    uiProgress("UPDATED", "restarting", 100);
    setStatusText("Updated - restarting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
  }
  DEBUG("CHAIN UPDATE: already up to date");
  uiProgress("UP TO DATE", "nothing to change", 100, false, 3000);
  setStatusText("Already up to date");
}

// one packet for the gauge being updated
static void handleAsTarget(const UpdPacket &packet)
{
  UpdHeader header;
  memcpy(&header, packet.data, sizeof(header));
  if (header.magic[0] != UPD_MAGIC0 || header.magic[1] != UPD_MAGIC1 || header.group != settings.chainGroup)
  {
    return;
  }
  if (header.type == UPD_REPLY)
  {
    return; // a late answer for a job we were sending
  }
  if (header.type == UPD_HELLO)
  {
    handleHello(packet);
    return;
  }
  // everything else belongs to the job we've taken
  if (!receiving || header.session != targetSession || memcmp(packet.mac, senderMac, 6) != 0)
  {
    if (header.type != UPD_ABORT)
    {
      sendReply(packet.mac, header, UPD_NO_SESSION, 0);
    }
    return;
  }
  lastPacketMs = millis();
  if (haveLastReply && header.request == lastRequest)
  {
    linkEspNowSendTo(packet.mac, &lastReply, sizeof(lastReply)); // a repeat: our answer got lost, send it again
    return;
  }
  switch (header.type)
  {
  case UPD_FW_DATA:
    handleFirmwareData(packet);
    break;
  case UPD_FW_DONE:
    handleFirmwareDone(packet);
    break;
  case UPD_FILE:
    handleFile(packet);
    break;
  case UPD_FILE_DATA:
    handleFileData(packet);
    break;
  case UPD_FILE_DONE:
    handleFileDone(packet);
    break;
  case UPD_FINISH:
    handleFinish(packet);
    break;
  case UPD_ABORT:
    dropTargetJob("stopped by the other gauge");
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// sender: updating the others
// ---------------------------------------------------------------------------
// send a request and wait for its answer, sending it again if the answer doesn't come. False = no answer / stopped.
// While waiting, a HELLO from another gauge is told we're busy.
static bool askTarget(const UpdTarget &target, uint8_t *packet, size_t length, uint32_t waitMs, uint8_t tries,
                      UpdReply &reply)
{
  UpdHeader header;
  memcpy(&header, packet, sizeof(header));
  for (uint8_t attempt = 0; attempt < tries; attempt++)
  {
    if (stopRequested)
    {
      return false;
    }
    linkEspNowSendTo(target.mac, packet, length);
    const uint32_t sentMs = millis();
    while (millis() - sentMs < waitMs)
    {
      if (xQueueReceive(packetQueue, &receivedPacket, pdMS_TO_TICKS(20)) != pdTRUE)
      {
        if (stopRequested)
        {
          return false;
        }
        continue;
      }
      UpdHeader got;
      memcpy(&got, receivedPacket.data, sizeof(got));
      if (got.type == UPD_HELLO)
      {
        handleHello(receivedPacket); // someone wants to update us: answers "busy" (sending is set)
        continue;
      }
      if (got.type != UPD_REPLY || got.session != header.session || got.request != header.request ||
          memcmp(receivedPacket.mac, target.mac, 6) != 0 || receivedPacket.length < sizeof(UpdReply))
      {
        continue; // not the answer we're waiting for (a late one for an earlier request)
      }
      memcpy(&reply, receivedPacket.data, sizeof(reply));
      return true;
    }
  }
  return false;
}

// what an answer code means, for the result list
static const char *replyText(uint8_t code)
{
  switch (code)
  {
  case UPD_BUSY:
    return "busy (another update running)";
  case UPD_WRONG_BOARD:
    return "different board - needs its own firmware";
  case UPD_DRIVING:
    return "engine running or moving";
  case UPD_BAD_KEY:
    return "key doesn't match - it's on older firmware with its own key: update it once from its own page"; // 0.28.3
  case UPD_NEWER:
    return "has newer firmware than this gauge";
  case UPD_NO_SESSION:
    return "lost track of the update (restarted?)";
  case UPD_WRITE_FAILED:
    return "couldn't write its flash";
  case UPD_BAD_IMAGE:
    return "firmware didn't check out";
  case UPD_CHAIN_OFF:
    return "chain is off on it";
  default:
    return "error";
  }
}

// a new request number for every new request (repeats keep theirs)
static uint16_t nextRequest()
{
  requestCounter = requestCounter + 1;
  return requestCounter;
}

// the release's file list, with each file's size and fingerprint. False = no list (an image from before 0.25.0).
static bool loadFileList()
{
  pathCount = 0;
  File list = LittleFS.open(UPD_MANIFEST, "r");
  if (!list)
  {
    return false;
  }
  while (list.available() && pathCount < UPD_MAX_FILES - 1) // one place kept for the list itself
  {
    String path = list.readStringUntil('\n');
    path.trim();
    if (path.length() == 0 || path.length() >= UPD_PATH_LEN || path == UPD_MANIFEST || !LittleFS.exists(path))
    {
      continue;
    }
    copyStr(pathAt(pathCount), UPD_PATH_LEN, path.c_str());
    pathCount++;
  }
  list.close();
  copyStr(pathAt(pathCount), UPD_PATH_LEN, UPD_MANIFEST); // the list itself goes last (see deleteDroppedFiles)
  pathCount++;
  for (uint16_t i = 0; i < pathCount; i++)
  {
    if (!hashFile(pathAt(i), hashAt(i), fileSizes[i]))
    {
      return false;
    }
    vTaskDelay(1); // let the other tasks in between files
  }
  return true;
}

// send a whole stream piece by piece, following the offsets the other gauge asks for.
// type UPD_FW_DATA = from our running firmware slot, UPD_FILE_DATA = from the open file
static bool sendStream(const UpdTarget &target, uint8_t type, uint32_t totalLength, File &file, const char *what,
                       String &failText)
{
  const esp_partition_t *runningPartition = esp_ota_get_running_partition();
  uint32_t offset = 0;
  uint8_t lastPercent = 255;
  while (offset < totalLength)
  {
    uint32_t pieceLength = totalLength - offset;
    if (pieceLength > UPD_CHUNK)
    {
      pieceLength = UPD_CHUNK;
    }
    if (type == UPD_FW_DATA)
    {
      esp_partition_read(runningPartition, offset, dataPacket.bytes, pieceLength);
    }
    else
    {
      file.seek(offset);
      file.read(dataPacket.bytes, pieceLength);
    }
    fillUpdHeader(dataPacket.header, type, sendSession, nextRequest());
    dataPacket.offset = offset;
    dataPacket.length = (uint16_t)pieceLength;
    UpdReply reply;
    // 500 ms each, 20 tries: a flash page being cleared can hold the other gauge up for ~50 ms
    if (!askTarget(target, (uint8_t *)&dataPacket, offsetof(UpdData, bytes) + pieceLength, 500, 20, reply))
    {
      failText = String("no answer during the ") + what;
      return false;
    }
    if (reply.status != UPD_OK)
    {
      failText = replyText(reply.status);
      return false;
    }
    if (reply.value > totalLength)
    {
      failText = "lost its place";
      return false;
    }
    offset = reply.value; // normally offset + pieceLength; after a lost packet, wherever it got to
    if (type == UPD_FW_DATA)
    {
      const uint8_t percent = (uint8_t)((uint64_t)offset * 100 / totalLength);
      if (percent != lastPercent)
      {
        lastPercent = percent;
        setStatusPercent(percent);
      }
    }
  }
  return true;
}

// update one gauge. True = updated (or already up to date). failText = why not.
static bool updateOneGauge(const UpdTarget &target, bool haveFiles, bool &restarting, String &failText)
{
  restarting = false;
  sendSession = (uint16_t)(esp_random() & 0xFFFF);
  if (sendSession == 0)
  {
    sendSession = 1; // 0 means "no job" on the other side
  }
  char text[80];

  // HELLO
  UpdHello hello;
  memset(&hello, 0, sizeof(hello));
  fillUpdHeader(hello.header, UPD_HELLO, sendSession, nextRequest());
  hello.board = chainUpdateBoardId();
  chainUpdateVersion(hello.version);
  hello.imageLength = imageLength;
  memcpy(hello.appHash, appHash, 32);
  hello.fileCount = haveFiles ? pathCount : 0;
  keyCode((const uint8_t *)&hello, offsetof(UpdHello, hmac), hello.hmac);
  UpdReply reply;
  if (!askTarget(target, (uint8_t *)&hello, sizeof(hello), 8000, 3, reply))
  {
    failText = "no answer (chain on, same group, 0.25.0 or later?)";
    return false;
  }
  if (reply.status != UPD_OK && reply.status != UPD_SAME)
  {
    failText = replyText(reply.status);
    return false;
  }

  // firmware
  File noFile;
  if (reply.status == UPD_OK)
  {
    snprintf(text, sizeof(text), "G%u %s: sending firmware", target.node, target.name);
    setStatusText(text);
    if (!sendStream(target, UPD_FW_DATA, imageLength, noFile, "firmware", failText))
    {
      return false;
    }
    UpdHeader done;
    fillUpdHeader(done, UPD_FW_DONE, sendSession, nextRequest());
    if (!askTarget(target, (uint8_t *)&done, sizeof(done), 15000, 2, reply)) // it checks the whole image: a few s
    {
      failText = "no answer after the firmware";
      return false;
    }
    if (reply.status != UPD_OK)
    {
      failText = replyText(reply.status);
      return false;
    }
  }

  // web UI files
  uint16_t filesSent = 0;
  if (haveFiles)
  {
    for (uint16_t i = 0; i < pathCount; i++)
    {
      snprintf(text, sizeof(text), "G%u %s: files %u of %u (%u sent)", target.node, target.name, i + 1, pathCount,
               filesSent);
      setStatusText(text);
      setStatusPercent((uint8_t)((i + 1) * 100 / pathCount));
      UpdFile filePacket;
      memset(&filePacket, 0, sizeof(filePacket));
      fillUpdHeader(filePacket.header, UPD_FILE, sendSession, nextRequest());
      filePacket.index = i;
      filePacket.count = pathCount;
      filePacket.size = fileSizes[i];
      memcpy(filePacket.hash, hashAt(i), 32);
      copyStr(filePacket.path, UPD_PATH_LEN, pathAt(i));
      keyCode((const uint8_t *)&filePacket, offsetof(UpdFile, hmac), filePacket.hmac);
      // it fingerprints its own copy first: up to a few s for the biggest file
      if (!askTarget(target, (uint8_t *)&filePacket, sizeof(filePacket), 10000, 3, reply))
      {
        failText = "no answer while checking files";
        return false;
      }
      if (reply.status == UPD_SAME)
      {
        continue;
      }
      if (reply.status != UPD_SEND)
      {
        failText = replyText(reply.status);
        return false;
      }
      File file = LittleFS.open(pathAt(i), "r");
      if (!file)
      {
        failText = String("couldn't read ") + pathAt(i);
        return false;
      }
      const bool sent = sendStream(target, UPD_FILE_DATA, fileSizes[i], file, "files", failText);
      file.close();
      if (!sent)
      {
        return false;
      }
      UpdHeader done;
      fillUpdHeader(done, UPD_FILE_DONE, sendSession, nextRequest());
      if (!askTarget(target, (uint8_t *)&done, sizeof(done), 5000, 3, reply))
      {
        failText = "no answer after a file";
        return false;
      }
      if (reply.status != UPD_OK)
      {
        failText = String(pathAt(i)) + ": " + replyText(reply.status);
        return false;
      }
      filesSent++;
    }
  }

  // FINISH
  UpdHeader finish;
  fillUpdHeader(finish, UPD_FINISH, sendSession, nextRequest());
  if (!askTarget(target, (uint8_t *)&finish, sizeof(finish), 5000, 3, reply) || reply.status != UPD_OK)
  {
    failText = "no answer at the end";
    return false;
  }
  restarting = reply.value != 0;
  return true;
}

// tell a gauge to throw away a half-done job (one try: it times out by itself anyway)
static void sendAbort(const UpdTarget &target)
{
  UpdHeader abortHeader;
  fillUpdHeader(abortHeader, UPD_ABORT, sendSession, nextRequest());
  linkEspNowSendTo(target.mac, &abortHeader, sizeof(abortHeader));
}

// wait for an updated gauge to come back after its restart with our version
static bool waitForRestart(const UpdTarget &target)
{
  uint8_t ourVersion[3];
  chainUpdateVersion(ourVersion);
  vTaskDelay(pdMS_TO_TICKS(5000)); // it's still restarting: its old heartbeats are still in the list
  const uint32_t startMs = millis();
  LinkNode nodes[16];
  while (millis() - startMs < UPD_RESTART_WAIT_MS && !stopRequested)
  {
    const uint8_t count = linkNodes(nodes, 16);
    for (uint8_t i = 0; i < count; i++)
    {
      LinkNode node = nodes[i];
      const bool sameGauge = memcmp(node.mac, target.mac, 6) == 0;
      const bool heardJustNow = millis() - node.lastSeen < 2000;
      const bool hasRestarted = node.uptimeS < 120; // not its last heartbeat from before the restart
      if (sameGauge && heardJustNow && hasRestarted && memcmp(node.version, ourVersion, 3) == 0)
      {
        return true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  return false;
}

static void addResult(uint8_t node, bool finished, bool worked, const char *text)
{
  portENTER_CRITICAL(&statusMux);
  int slot = -1;
  for (uint8_t i = 0; i < status.resultCount; i++)
  {
    if (status.results[i].node == node)
    {
      slot = i;
    }
  }
  if (slot < 0 && status.resultCount < CHAIN_UPDATE_RESULTS)
  {
    slot = status.resultCount;
    status.resultCount++;
  }
  if (slot >= 0)
  {
    status.results[slot].node = node;
    status.results[slot].finished = finished;
    status.results[slot].worked = worked;
    copyStr(status.results[slot].text, sizeof(status.results[slot].text), text);
  }
  portEXIT_CRITICAL(&statusMux);
}

// the whole job: every other gauge that can take a chain update, one at a time
static void runSendJob()
{
  sending = true;
  // 0.36.10: reading our firmware and files back out of flash to send them also starves the screen feed (bench:
  // lines on the sending gauge), so the sender slows its screen clock too
  displaySetUpdateClock(true);
  stopRequested = false;
  portENTER_CRITICAL(&statusMux);
  status.resultCount = 0;
  status.percent = 0;
  portEXIT_CRITICAL(&statusMux);
  setStatusText("Getting ready: checking this gauge's firmware and files");

  // our firmware: its length and fingerprint
  const esp_partition_t *runningPartition = esp_ota_get_running_partition();
  esp_partition_t running; // a copy of ESP-IDF's description of our slot, to read with dots
  memcpy(&running, runningPartition, sizeof(running));
  esp_partition_pos_t position;
  position.offset = running.address;
  position.size = running.size;
  esp_image_metadata_t metadata;
  memset(&metadata, 0, sizeof(metadata));
  bool ready = true;
  if (esp_image_get_metadata(&position, &metadata) != ESP_OK)
  {
    setStatusText("Couldn't read this gauge's own firmware");
    ready = false;
  }
  imageLength = metadata.image_len;
  esp_partition_get_sha256(runningPartition, appHash);

  // the release's files
  bool haveFiles = false;
  if (ready)
  {
    filePaths = (char *)ps_malloc(UPD_MAX_FILES * UPD_PATH_LEN); // PSRAM: 25 KB of names
    fileHashes = (uint8_t *)ps_malloc(UPD_MAX_FILES * 32);
    fileSizes = (uint32_t *)ps_malloc(UPD_MAX_FILES * sizeof(uint32_t));
    if (filePaths != NULL && fileHashes != NULL && fileSizes != NULL)
    {
      haveFiles = loadFileList();
    }
    if (!haveFiles)
    {
      DEBUG("CHAIN UPDATE: no file list on this gauge - firmware only");
    }
  }

  // who to update: every gauge heard over ESP-NOW that can take a chain update
  UpdTarget targets[16];
  uint8_t targetCount = 0;
  if (ready)
  {
    LinkNode nodes[16];
    const uint8_t count = linkNodes(nodes, 16);
    for (uint8_t i = 0; i < count; i++)
    {
      LinkNode node = nodes[i];
      const bool heardOnEspNow = (node.via & 2) != 0; // bit 1 = heard on ESP-NOW
      if (!heardOnEspNow)
      {
        addResult(node.node, true, false, "only heard on CAN - chain updates go over ESP-NOW");
        continue;
      }
      if (node.updateProto < 1)
      {
        addResult(node.node, true, false, "older than 0.25.0 - update it from its own page");
        continue;
      }
      targets[targetCount].node = node.node;
      memcpy(targets[targetCount].mac, node.mac, 6);
      copyStr(targets[targetCount].name, NAME_LEN, node.name);
      addResult(node.node, false, false, "waiting");
      targetCount++;
    }
  }
  if (ready && targetCount == 0)
  {
    setStatusText("No other gauges to update");
  }

  uint8_t updatedCount = 0;
  for (uint8_t i = 0; i < targetCount && ready && !stopRequested; i++)
  {
    const UpdTarget target = targets[i];
    char text[80];
    snprintf(text, sizeof(text), "G%u %s: starting", target.node, target.name);
    setStatusText(text);
    setStatusPercent(0);
    addResult(target.node, false, false, "updating");
    bool restarting = false;
    String failText;
    const bool worked = updateOneGauge(target, haveFiles, restarting, failText);
    if (!worked)
    {
      sendAbort(target);
      if (stopRequested)
      {
        failText = "stopped";
      }
      DEBUG("CHAIN UPDATE: G%u failed - %s", target.node, failText.c_str());
      addResult(target.node, true, false, failText.c_str());
      continue;
    }
    if (!restarting)
    {
      addResult(target.node, true, true, "already up to date");
      continue;
    }
    addResult(target.node, false, true, "updated - restarting");
    snprintf(text, sizeof(text), "G%u %s: restarting", target.node, target.name);
    setStatusText(text);
    if (waitForRestart(target))
    {
      addResult(target.node, true, true, "updated, back up on " CAN2GAUGE_VERSION);
      updatedCount++;
    }
    else
    {
      // one gauge that doesn't come back stops the job: better than spreading a bad build round the whole row
      addResult(target.node, true, false, "not heard back after its restart - stopped");
      stopRequested = true;
    }
  }

  if (ready && targetCount > 0)
  {
    char text[80];
    if (stopRequested)
    {
      snprintf(text, sizeof(text), "Stopped (%u updated)", updatedCount);
    }
    else
    {
      snprintf(text, sizeof(text), "Finished (%u updated)", updatedCount);
    }
    setStatusText(text);
  }
  free(filePaths);
  free(fileHashes);
  free(fileSizes);
  filePaths = NULL;
  fileHashes = NULL;
  fileSizes = NULL;
  displaySetUpdateClock(false); // 0.36.10: the screen's normal clock back
  sending = false;
  stopRequested = false;
}

// ---------------------------------------------------------------------------
// the task: made at boot and kept (0.25.3), it waits on the packet queue when there is nothing to do
// ---------------------------------------------------------------------------
static void updateTask(void *)
{
  while (1)
  {
    if (sendRequested)
    {
      sendRequested = false;
      runSendJob();
    }
    if (xQueueReceive(packetQueue, &receivedPacket, pdMS_TO_TICKS(200)) == pdTRUE)
    {
      handleAsTarget(receivedPacket);
    }
    if (receiving && millis() - lastPacketMs > UPD_TARGET_TIMEOUT_MS)
    {
      dropTargetJob("the other gauge went quiet");
    }
  }
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------
void setupChainUpdate()
{
  DEBUG("Setting up chain update...");
  packetQueue = xQueueCreate(4, sizeof(UpdPacket)); // 4 KB, in PSRAM (anything over 1 KB goes there)
  memset(&status, 0, sizeof(status));
  // 0.25.3: made now, while the internal RAM is still in big pieces, and kept (see startTask)
  // 8 KB of internal RAM: it writes flash, so its stack can't go in PSRAM
  const BaseType_t made = xTaskCreatePinnedToCore(
      updateTask,   // the task's function
      "chainupd",   // name (task list / crash reports)
      8192,         // stack, bytes (file system + fingerprints + the gauge lists)
      NULL,         // nothing passed in
      3,            // priority: below the link task (5), so the gauges keep talking meanwhile
      NULL,         // handle not needed
      0);           // core 0, with the radio (the glass is on core 1)
  if (made == pdPASS)
  {
    taskRunning = true;
  }
  else
  {
    DEBUG("CHAIN UPDATE: no RAM for the task (%u bytes internal free, largest piece %u)",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  DEBUG("Set up chain update!");
}

bool chainUpdateStart(String &reason)
{
  if (!settings.chainEnabled)
  {
    reason = "turn the chain on first";
    return false;
  }
  if (!linkStats().espNowUp)
  {
    reason = "ESP-NOW is off (Transport: Auto or ESP-NOW)";
    return false;
  }
  if (sending || receiving || otaInProgress())
  {
    reason = "an update is already running";
    return false;
  }
  if (carIsMoving())
  {
    reason = "engine running or moving - park first";
    return false;
  }
  esp_ota_img_states_t imageState;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &imageState) == ESP_OK &&
      imageState == ESP_OTA_IMG_PENDING_VERIFY)
  {
    reason = "this gauge's new firmware is still being checked - try again in a minute";
    return false;
  }
  sendRequested = true;
  if (!startTask())
  {
    sendRequested = false;
    reason = "the update task couldn't start at boot (not enough internal RAM) - restart the gauge";
    return false;
  }
  reason = "updating the other gauges";
  return true;
}

void chainUpdateStop()
{
  if (sending)
  {
    stopRequested = true;
  }
}

bool chainUpdateBusy()
{
  return sending || receiving || sendRequested;
}

void chainUpdateGetStatus(ChainUpdateStatus &out)
{
  portENTER_CRITICAL(&statusMux);
  memcpy(&out, &status, sizeof(out));
  portEXIT_CRITICAL(&statusMux);
  out.sending = sending || sendRequested;
  out.receiving = receiving;
}
