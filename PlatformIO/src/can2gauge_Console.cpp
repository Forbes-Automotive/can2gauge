#include <can2gauge_Console.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_API.h> // apiConsoleRequest(): the same handlers the web UI uses
#include <LittleFS.h>      // FILEGET / FILEPUT
#include <mbedtls/base64.h> // files travel as base64 text lines (the serial carries debug text too)
#include <esp_rom_crc.h>    // esp_rom_crc32_le(): a check value for each file sent or received
#include <ME7Read.h>        // me7ReadDump(): the ECU flash read held in memory (0.17.4)

// USB bench console (0.17.3) - see can2gauge_Console.h for the commands.

#define CONSOLE_LINE_MAX 1600     // longest command line (a POST with a settings JSON, or a FILEDATA line)
#define CONSOLE_CHUNK_BYTES 576   // file bytes per "@@B64" line (576 bytes = 768 base64 characters)
#define CONSOLE_TX_TIMEOUT_MS 2000 // while a file is sent: wait this long for the PC rather than drop data
#define CONSOLE_TX_NORMAL_MS 10    // the usual timeout (main.cpp): no PC listening must never hold a task up

static TaskHandle_t consoleTask = NULL;
EXT_RAM_BSS_ATTR static char consoleLine[CONSOLE_LINE_MAX]; // the command being typed in (static: kept off the task's stack)
static uint16_t consoleLineLength = 0;
static File uploadFile;                     // the file FILEPUT opened, until FILEEND
static uint32_t uploadBytes = 0;            // bytes written to it so far
static uint32_t uploadCrc = 0;              // check value of those bytes
EXT_RAM_BSS_ATTR static uint8_t fileBuffer[CONSOLE_CHUNK_BYTES];             // one piece of a file
EXT_RAM_BSS_ATTR static unsigned char base64Buffer[CONSOLE_LINE_MAX + 4];    // the same piece as base64 text (or decoded from it)

// one reply line: "@@C2G <code> <json text>"
static void consoleReply(int code, const String &json)
{
  String line = CONSOLE_MARK " ";
  line += code;
  line += ' ';
  line += json;
  line += '\n';
  Serial.write((const uint8_t *)line.c_str(), line.length()); // one write: debug lines can't land in the middle
}

// 0.17.4: FILEGET /api/me7read/bin - the ECU flash read held in memory, sent the same way as a file
static void consoleDumpGet()
{
  uint32_t size = 0;
  const uint8_t *dump = me7ReadDump(size);
  if (dump == NULL || size == 0)
  {
    consoleReply(404, "{\"error\":\"no ECU read in memory\"}");
    return;
  }
  consoleReply(200, String("{\"size\":") + size + "}");
  Serial.setTxTimeoutMs(CONSOLE_TX_TIMEOUT_MS); // wait for the PC rather than drop any of it
  uint32_t crc = 0;
  uint32_t sent = 0;
  while (sent < size)
  {
    uint32_t count = size - sent;
    if (count > CONSOLE_CHUNK_BYTES)
    {
      count = CONSOLE_CHUNK_BYTES;
    }
    crc = esp_rom_crc32_le(crc, dump + sent, count);
    size_t textLength = 0;
    mbedtls_base64_encode(base64Buffer, sizeof(base64Buffer), &textLength, dump + sent, count);
    String line = "@@B64 ";
    line += (const char *)base64Buffer;
    line += '\n';
    Serial.write((const uint8_t *)line.c_str(), line.length());
    sent += count;
  }
  Serial.setTxTimeoutMs(CONSOLE_TX_NORMAL_MS);
  consoleReply(200, String("{\"size\":") + sent + ",\"crc32\":" + crc + "}");
}

// FILEGET <path>: the whole file as base64 lines, then its size and check value
static void consoleFileGet(const char *path)
{
  if (!strcmp(path, "/api/me7read/bin"))
  {
    consoleDumpGet(); // not a file: the ECU read in memory
    return;
  }
  File file = LittleFS.open(path, "r");
  if (!file || file.isDirectory())
  {
    consoleReply(404, "{\"error\":\"no such file\"}");
    return;
  }
  const uint32_t size = file.size();
  consoleReply(200, String("{\"size\":") + size + "}");
  Serial.setTxTimeoutMs(CONSOLE_TX_TIMEOUT_MS); // a 1 MB flash read is ~1400 lines: wait for the PC, don't drop any
  uint32_t crc = 0;
  uint32_t sent = 0;
  while (sent < size)
  {
    const size_t got = file.read(fileBuffer, CONSOLE_CHUNK_BYTES);
    if (got == 0)
    {
      break; // read error: the size check on the PC catches it
    }
    crc = esp_rom_crc32_le(crc, fileBuffer, got);
    size_t textLength = 0;
    mbedtls_base64_encode(base64Buffer, sizeof(base64Buffer), &textLength, fileBuffer, got);
    String line = "@@B64 ";
    line += (const char *)base64Buffer;
    line += '\n';
    Serial.write((const uint8_t *)line.c_str(), line.length());
    sent += got;
  }
  file.close();
  Serial.setTxTimeoutMs(CONSOLE_TX_NORMAL_MS);
  consoleReply(200, String("{\"size\":") + sent + ",\"crc32\":" + crc + "}");
}

// only these folders take uploads from the console (the .ecu / .cfg files and logs), never the web UI's own files
static bool uploadAllowed(const char *path)
{
  if (!strncmp(path, "/me7/", 5))
  {
    return true;
  }
  if (!strncmp(path, "/logs/", 6))
  {
    return true;
  }
  return false;
}

// FILEPUT <path>: a new, empty file for the FILEDATA lines that follow
static void consoleFilePut(const char *path)
{
  if (uploadFile)
  {
    uploadFile.close();
  }
  if (!uploadAllowed(path))
  {
    consoleReply(403, "{\"error\":\"only /me7/ and /logs/ take uploads\"}");
    return;
  }
  uploadFile = LittleFS.open(path, "w", true); // true = make the folder if it isn't there
  if (!uploadFile)
  {
    consoleReply(500, "{\"error\":\"can't create the file\"}");
    return;
  }
  uploadBytes = 0;
  uploadCrc = 0;
  consoleReply(200, "{\"status\":\"ready\"}");
}

// FILEDATA <base64>: one more piece of the file
static void consoleFileData(const char *text)
{
  if (!uploadFile)
  {
    consoleReply(409, "{\"error\":\"no FILEPUT first\"}");
    return;
  }
  size_t got = 0;
  if (mbedtls_base64_decode(base64Buffer, sizeof(base64Buffer), &got, (const unsigned char *)text, strlen(text)) != 0)
  {
    consoleReply(400, "{\"error\":\"bad base64\"}");
    return;
  }
  if (uploadFile.write(base64Buffer, got) != got)
  {
    consoleReply(500, "{\"error\":\"write failed - filesystem full?\"}");
    return;
  }
  uploadCrc = esp_rom_crc32_le(uploadCrc, base64Buffer, got);
  uploadBytes += got;
  consoleReply(200, String("{\"bytes\":") + uploadBytes + "}");
}

// FILEEND: the file is complete
static void consoleFileEnd()
{
  if (!uploadFile)
  {
    consoleReply(409, "{\"error\":\"no FILEPUT first\"}");
    return;
  }
  uploadFile.close();
  fsUsageChanged(); // 0.17.4: the space used changed
  consoleReply(200, String("{\"size\":") + uploadBytes + ",\"crc32\":" + uploadCrc + "}");
}

// 0.17.4: FILEDEL <path> - removes an uploaded file (only /me7/ and /logs/, the same folders FILEPUT may write to).
// The bench filled the filesystem with .ecu copies (~125 KB each, ~330 KB free with the web UI image).
static void consoleFileDelete(const char *path)
{
  if (!uploadAllowed(path))
  {
    consoleReply(403, "{\"error\":\"only /me7/ and /logs/ files can be deleted\"}");
    return;
  }
  if (!LittleFS.remove(path))
  {
    consoleReply(404, "{\"error\":\"no such file\"}");
    return;
  }
  fsUsageChanged(); // the space used changed
  consoleReply(200, "{\"status\":\"deleted\"}");
}

// one whole command line
static void consoleCommand(char *line)
{
  if (!strcmp(line, "PING"))
  {
    consoleReply(200, String("{\"version\":\"") + CAN2GAUGE_VERSION + "\",\"uptime\":" + (millis() / 1000) + "}");
    return;
  }
  if (!strncmp(line, "GET ", 4))
  {
    const char *path = line + 4;
    if (!apiConsoleRequest("GET", path, String()))
    {
      consoleReply(404, "{\"error\":\"not a console route\"}");
    }
    return;
  }
  if (!strncmp(line, "POST ", 5))
  {
    // POST <path> <json>: the path ends at the first space, the JSON is the rest (may be empty)
    char *path = line + 5;
    char *body = strchr(path, ' ');
    String bodyText;
    if (body != NULL)
    {
      *body = '\0'; // ends the path
      bodyText = body + 1;
    }
    if (!apiConsoleRequest("POST", path, bodyText))
    {
      consoleReply(404, "{\"error\":\"not a console route\"}");
    }
    return;
  }
  if (!strncmp(line, "FILEGET ", 8))
  {
    consoleFileGet(line + 8);
    return;
  }
  if (!strncmp(line, "FILEPUT ", 8))
  {
    consoleFilePut(line + 8);
    return;
  }
  if (!strncmp(line, "FILEDATA ", 9))
  {
    consoleFileData(line + 9);
    return;
  }
  if (!strcmp(line, "FILEEND"))
  {
    consoleFileEnd();
    return;
  }
  if (!strncmp(line, "FILEDEL ", 8))
  {
    consoleFileDelete(line + 8);
    return;
  }
  consoleReply(400, "{\"error\":\"unknown command\"}");
}

// collects characters from the USB serial into lines and runs each one
static void consoleTaskFn(void *parameter)
{
  (void)parameter;
  bool lineTooLong = false; // the current line ran past CONSOLE_LINE_MAX: it's dropped at its end
  while (1)
  {
    if (!Serial.available())
    {
      vTaskDelay(pdMS_TO_TICKS(5)); // nothing typed: check again shortly
      continue;
    }
    const int received = Serial.read();
    if (received < 0 || received == '\r')
    {
      continue;
    }
    if (received == '\n')
    {
      if (lineTooLong)
      {
        consoleReply(413, "{\"error\":\"line too long\"}");
      }
      else if (consoleLineLength > 0)
      {
        consoleLine[consoleLineLength] = '\0';
        consoleCommand(consoleLine);
      }
      consoleLineLength = 0;
      lineTooLong = false;
      continue;
    }
    if (consoleLineLength < CONSOLE_LINE_MAX - 1)
    {
      consoleLine[consoleLineLength] = (char)received;
      consoleLineLength++;
    }
    else
    {
      lineTooLong = true;
    }
  }
}

void setupConsole()
{
#if enableConsole
  // 0.24.18: only with a PC on the USB port at boot (Serial is true then). In the car nothing is plugged in, and the
  // console's 8 KB stack is internal RAM that Bluetooth and the web UI need. On the bench the PC is there from power-up.
  if (!Serial)
  {
    DEBUG("USB bench console: no PC on the USB port - not started");
    return;
  }
  DEBUG("Setting up the USB bench console...");
  xTaskCreatePinnedToCore(consoleTaskFn, // the function the task runs
                          "console",     // its name (shows in crash reports)
                          8192,          // stack bytes: the API handlers build their JSON replies here. 0.24.10: back to
                                         // 8 KB - 0.24.6's 6 KB overflowed on a POST /api/settings (crash, 2026-10-26)
                          NULL,          // no parameter
                          1,             // low priority: the gauge's own work comes first
                          &consoleTask,  // handle, kept for debugging
                          0);            // core 0, with the other communication tasks
  DEBUG("Set up the USB bench console!");
#endif
}
