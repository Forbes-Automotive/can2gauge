#include <can2gauge_ELM.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_DSG.h>
#include <can2gauge_Link.h>
#include <can2gauge_Vehicle.h> // 0.35.0: the VIN decode (make, model, year) and Match car
#include <CANLine.h> // obdPidLen / obdDecodePid (shared mode 01 decoders)
#include <can2gauge_Tp20.h> // 0.8.0: TP2.0 through the adapter in raw CAN mode
#include <can2gauge_ver.h>
#include <LittleFS.h>       // bench test logs (/logs/elm_*.txt)
#include <stdarg.h>
#include <can2gauge_ElmWiFi.h> // 0.8.0: WiFi adapters
#include <can2gauge_KLine.h>   // 0.8.0: klineApplyGroup() - the engine block map, shared with the gauge's own K-line
#include <KLine.h>             // kwp1281Decode(): VAG KWP2000 measuring blocks use the KWP1281 formula triplets
#include <can2gauge_UserSig.h> // 0.15.0: userSigRotationUses() - a page in the rotation reads an OBD value
#include <can2gauge_Nav.h>     // 0.30.0: navWanted() - the nav link keeps Bluetooth up

// ---------------------------------------------------------------------------
// the adapter link (0.8.0): Bluetooth LE or WiFi, per settings.elmTransport, latched when the link comes up so a
// settings change disconnects the right one
// ---------------------------------------------------------------------------
static uint8_t linkTransport = ELM_TR_BLE;
static bool adapterConnected()
{
  return linkTransport == ELM_TR_WIFI ? elmWifiConnected() : bleConnected();
}

static bool adapterWrite(const uint8_t *data, size_t len)
{
  return linkTransport == ELM_TR_WIFI ? elmWifiWrite(data, len) : bleWrite(data, len);
}

static size_t adapterRead(uint8_t *buf, size_t max, uint32_t timeoutMs)
{
  return linkTransport == ELM_TR_WIFI ? elmWifiRead(buf, max, timeoutMs) : bleRead(buf, max, timeoutMs);
}

static void adapterFlush()
{
  linkTransport == ELM_TR_WIFI ? elmWifiFlush() : bleFlush();
}

static const char *adapterInfo()
{
  return linkTransport == ELM_TR_WIFI ? elmWifiLinkInfo() : bleLinkInfo();
}

static void adapterDisconnect()
{
  if (linkTransport == ELM_TR_WIFI)
  {
    elmWifiDisconnect(); // also hands the WiFi station back (radio returns to the gauge AP's own channel)
  }
  else
  {
    bleDisconnect();
  }
}

static TaskHandle_t elmTask = NULL;
static ElmStatus adapterStatus;
static volatile bool scanRequested = false;
static volatile bool scanning = false;
static volatile bool reidentifyRequested = false;
static volatile bool dropRequested = false;
static volatile bool connectionUp = false; // the adapter link is up (kept by the ELM task for the other tasks)
static volatile uint32_t generation = 0;
EXT_RAM_BSS_ATTR static BleFound foundAdapters[12];
static uint8_t foundAdapterCount = 0;
static QueueHandle_t terminalQueue = NULL;
static portMUX_TYPE adapterStatusMux = portMUX_INITIALIZER_UNLOCKED;

// terminal log: ring of lines
#define TERM_LINES 40
#define TERM_W 72
EXT_RAM_BSS_ATTR static char terminalLines[TERM_LINES][TERM_W];
static uint8_t terminalHead = 0;
static uint8_t terminalCount = 0;
static portMUX_TYPE terminalMux = portMUX_INITIALIZER_UNLOCKED;

static void terminalAdd(const char *prefix, const char *text)
{
  portENTER_CRITICAL(&terminalMux);
  snprintf(terminalLines[terminalHead], TERM_W, "%s%s", prefix, text);
  terminalHead = (terminalHead + 1) % TERM_LINES;
  if (terminalCount < TERM_LINES)
  {
    terminalCount++;
  }
  portEXIT_CRITICAL(&terminalMux);
}

static void setState(const char *text)
{
  portENTER_CRITICAL(&adapterStatusMux);
  copyStr(adapterStatus.state, sizeof(adapterStatus.state), text);
  portEXIT_CRITICAL(&adapterStatusMux);
}

static void setError(const char *message)
{
  copyStr(adapterStatus.lastError, sizeof(adapterStatus.lastError), message);
  terminalAdd("! ", message);
}

// bench test log (0.8.0): open while a test runs; elmCommand writes every exchange to it with its time and round trip
static File testLog;
static uint32_t testStartMs = 0;
static uint32_t lastRoundTripMs = 0; // ms from writing the command to the '>' prompt

static void testLine(const char *fmt, ...)
{
  if (!testLog)
  {
    return;
  }
  char line[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  testLog.printf("%7lu %s\n", (unsigned long)(millis() - testStartMs), line);
}

// ---------------------------------------------------------------------------
// command / response
// ---------------------------------------------------------------------------
// sends cmd + CR and collects everything up to the '>' prompt. out holds the answer lines separated by '\r'.
// log: 0 = not logged, 1 = logged to the terminal
static bool elmCommand(const char *cmd, char *out, size_t outLen, uint32_t timeoutMs, bool log = true)
{
  if (!adapterConnected() || !cmd[0])
  {
    return false; // never an empty line: the iCar Pro (fw 4.1.16+) repeats the last command on a bare CR
  }
  adapterFlush();
  const uint32_t t0 = millis();
  uint8_t line[48];
  size_t len = strlen(cmd);
  if (len > sizeof(line) - 1)
  {
    len = sizeof(line) - 1;
  }
  memcpy(line, cmd, len);
  line[len++] = '\r';
  if (log)
  {
    terminalAdd("> ", cmd);
  }
  if (!adapterWrite(line, len))
  {
    return false;
  }

  size_t count = 0;
  bool prompt = false;
  const uint32_t deadline = millis() + timeoutMs;
  while (!prompt && (int32_t)(deadline - millis()) > 0)
  {
    uint8_t buf[64];
    const size_t readCount = adapterRead(buf, sizeof(buf), 20);
    for (size_t i = 0; i < readCount; i++)
    {
      const char character = (char)buf[i];
      if (character == '>')
      {
        prompt = true;
        break;
      }
      if (character == '\n' || character == 0)
      {
        continue;
      }
      if (count < outLen - 1)
      {
        out[count++] = character;
      }
    }
  }
  out[count] = '\0';

  adapterStatus.requests++;
  lastRoundTripMs = millis() - t0;
  if (testLog)
  {
    testLine("> %s  [%lu ms%s]", cmd, (unsigned long)lastRoundTripMs, prompt ? "" : ", no prompt");
    char tmp[120];
    size_t tmpLen = 0;
    for (size_t i = 0; i <= count; i++)
    {
      if (i == count || out[i] == '\r')
      {
        tmp[tmpLen] = '\0';
        if (tmpLen)
        {
          testLine("    %s", tmp);
        }
        tmpLen = 0;
      }
      else if (tmpLen < sizeof(tmp) - 1)
      {
        tmp[tmpLen++] = out[i];
      }
    }
  }
  if (log)
  {
    // one terminal line per answer line
    char tmp[TERM_W];
    size_t tmpLen = 0;
    for (size_t i = 0; i <= count; i++)
    {
      if (i == count || out[i] == '\r')
      {
        tmp[tmpLen] = '\0';
        if (tmpLen)
        {
          terminalAdd("  ", tmp);
        }
        tmpLen = 0;
      }
      else if (tmpLen < sizeof(tmp) - 1)
      {
        tmp[tmpLen++] = out[i];
      }
    }
    if (!prompt)
    {
      terminalAdd("  ", "(timeout)");
    }
  }
#if detailedDebugELM
  DEBUG("ELM %s -> %s", cmd, out);
#endif
  return prompt;
}

static bool containsOk(const char *text)
{
  return strstr(text, "OK") != NULL;
}

static bool isHex(char character)
{
  return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F') ||
      (character >= 'a' && character <= 'f');
}

static uint8_t hexValue(char character)
{
  return character <= '9' ? character - '0' : (character | 0x20) - 'a' + 10;
}

static bool protocolIsCan(uint8_t protocol)
{
  return protocol >= 6 && protocol <= 12 && protocol != 10;
}

static bool protocolIs29Bit(uint8_t protocol)
{
  return protocol == 7 || protocol == 9;
}

// one ECU's reassembled answer
struct ElmMsg
{
  uint32_t header; // CAN id (11 / 29-bit) or the source byte of a K-line / J1850 header
  uint16_t len;
  uint8_t data[64];
};

// the protocol the adapter is running when a K-line session (0.8.0) has replaced the detected OBD one (0 = adapterStatus.protocol)
static uint8_t protocolOverride = 0;
static bool klineSessionUp = false; // a VAG KWP2000 session on the adapter's K-line is open (see "VAG KWP2000 over the K-line")
static bool canMonArmed = false;    // ELM_EXTRA_CAN_MONITOR: ATMA is running on the adapter (see "CAN broadcast source")
// 0.26.0: BMW extended addressing through the adapter (ATCEA, pollDsg): every answer line has the tester's address
// (0xF1) between the CAN header and the ISO-TP bytes, which parseAnswer drops. 0 = normal addressing.
static uint8_t elmRxExtAddr = 0;
static bool elmBmwFlowControlSet = false; // ATFCSH / ATFCSD sent this session (ATZ forgets them)

// hex characters of the header on one answer line: 8 on 29-bit CAN, 3 on 11-bit CAN, none on K-line / J1850
static uint8_t headerCharsFor(uint8_t protocol)
{
  if (!protocolIsCan(protocol))
  {
    return 0;
  }
  if (protocolIs29Bit(protocol))
  {
    return 8;
  }
  return 3;
}

// parses the answer lines (ATH1, ATS0) into messages. CAN: [header][PCI][data] per line, ISO-TP reassembled per
// header. K-line / J1850: [prio][target][source][data][checksum] per line, one message per line; a KWP2000 format byte
// with length bits 0 has the length in a fourth header byte.
static uint8_t parseAnswer(const char *text, ElmMsg *msgs, uint8_t max)
{
  uint8_t count = 0;
  uint16_t expect[4] = {0}; // ISO-TP total per message slot
  const uint8_t proto = protocolOverride ? protocolOverride : adapterStatus.protocol;
  const char *pos = text;
  while (*pos)
  {
    // take one line
    char line[200];
    size_t lineLen = 0;
    while (*pos && *pos != '\r')
    {
      if (*pos != ' ' && lineLen < sizeof(line) - 1)
      {
        line[lineLen++] = *pos;
      }
      pos++;
    }
    if (*pos == '\r')
    {
      pos++;
    }
    line[lineLen] = '\0';
    if (lineLen < 4)
    {
      continue;
    }
    bool allHex = true;
    for (size_t i = 0; i < lineLen && allHex; i++)
    {
      allHex = isHex(line[i]);
    }
    if (!allHex)
    {
      continue; // SEARCHING..., NO DATA, BUS INIT, errors
    }
    const uint8_t headerChars = headerCharsFor(proto);
    if (lineLen <= headerChars || ((lineLen - headerChars) & 1))
    {
      continue;
    }
    uint32_t header = 0;
    for (uint8_t i = 0; i < headerChars; i++)
    {
      header = (header << 4) | hexValue(line[i]);
    }
    uint8_t bytes[100];
    uint8_t byteCount = 0;
    for (size_t i = headerChars; i + 1 < lineLen && byteCount < sizeof(bytes); i += 2)
    {
      bytes[byteCount++] = (hexValue(line[i]) << 4) | hexValue(line[i + 1]);
    }
    // 0.26.0: BMW extended addressing - drop the tester's address byte (F1) in front of the ISO-TP bytes. Only when it's
    // there: an ISO-TP frame never starts with F1 (no such frame type), and a clone may already leave it out.
    if (headerChars && elmRxExtAddr != 0 && byteCount >= 2 && bytes[0] == elmRxExtAddr)
    {
      for (uint8_t i = 0; i + 1 < byteCount; i++)
      {
        bytes[i] = bytes[i + 1]; // each byte moves one place to the front
      }
      byteCount--;
    }
    if (!headerChars)
    {
      // K-line / J1850: 3 header bytes (4 for KWP2000 with the length in its own byte) + data + checksum
      const uint8_t headerLen = ((proto == 4 || proto == 5) && (bytes[0] & 0x80) && !(bytes[0] & 0x3F)) ? 4 : 3;
      if (byteCount < headerLen + 2 || count >= max)
      {
        continue;
      }
      ElmMsg &message = msgs[count++];
      message.header = bytes[2];
      message.len = byteCount - headerLen - 1;
      if (message.len > sizeof(message.data))
      {
        message.len = sizeof(message.data);
      }
      memcpy(message.data, &bytes[headerLen], message.len);
      continue;
    }
    // CAN: find this header's message slot
    int slot = -1;
    for (uint8_t i = 0; i < count; i++)
    {
      if (msgs[i].header == header && expect[i] && msgs[i].len < expect[i])
      {
        slot = i;
      }
    }
    const uint8_t pci = bytes[0] >> 4;
    if (pci == 0 && count < max)
    {
      ElmMsg &message = msgs[count++];
      message.header = header;
      message.len = bytes[0] & 0x0F;
      if (message.len > byteCount - 1)
      {
        message.len = byteCount - 1;
      }
      memcpy(message.data, &bytes[1], message.len);
    }
    else if (pci == 1 && count < max && count < 4)
    {
      ElmMsg &message = msgs[count];
      expect[count] = ((bytes[0] & 0x0F) << 8) | bytes[1];
      if (expect[count] > sizeof(message.data))
      {
        expect[count] = sizeof(message.data);
      }
      count++;
      message.header = header;
      message.len = byteCount > 2 ? byteCount - 2 : 0;
      memcpy(message.data, &bytes[2], message.len);
    }
    else if (pci == 2 && slot >= 0)
    {
      ElmMsg &message = msgs[slot];
      for (uint8_t i = 1; i < byteCount && message.len < expect[slot]; i++)
      {
        message.data[message.len++] = bytes[i];
      }
    }
  }
  return count;
}

// sends a hex request and returns the first answer that starts with the positive response byte (or 7F)
static bool elmRequest(const char *hexReq, ElmMsg &out, uint32_t timeoutMs, bool log = false)
{
  EXT_RAM_BSS_ATTR static char text[768];
  if (!elmCommand(hexReq, text, sizeof(text), timeoutMs, log))
  {
    adapterStatus.errors++;
    return false;
  }
  if (strstr(text, "NO DATA"))
  {
    adapterStatus.noData++;
    return false;
  }
  ElmMsg msgs[6];
  const uint8_t messageCount = parseAnswer(text, msgs, 6);
  const uint8_t service = (hexValue(hexReq[0]) << 4) | hexValue(hexReq[1]);
  // the positive answer first: a "response pending" (7F xx 78) can come ahead of it in the same output
  for (int pass = 0; pass < 2; pass++)
  {
    for (uint8_t i = 0; i < messageCount; i++)
    {
      const ElmMsg &candidate = msgs[i];
      const bool hit = pass == 0 ? candidate.len && candidate.data[0] == (uint8_t)(service + 0x40) : candidate.len >= 2 && candidate.data[0] == 0x7F && !(candidate.len >= 3 && candidate.data[2] == 0x78);
      if (hit)
      {
        out = candidate;
        return true;
      }
    }
  }
  adapterStatus.errors++;
  return false;
}

// 0.25.3: the engine's address on a 29-bit CAN car, learned from its answer to 01 00 (2025 Tiguan: 18DAF101 -> 0x01)
static uint8_t engineAddress29Bit = 0x01;
// 0.25.3: the gearbox's address on a 29-bit CAN car (VW: 0x02), learned from the other ECU answering 01 00 (Tiguan:
// 18DAF102 -> 0x02). The DSG tab's request / answer ids are 11-bit only, so pollDsg() uses this instead there.
static uint8_t gearboxAddress29Bit = 0x02;

// points the adapter's requests at the engine ECU only (its physical address), on 11-bit or 29-bit CAN.
// 0.25.3: it was always ATSH7E0 (the 11-bit engine), which nothing listens to on a 29-bit car (Tiguan: NO DATA)
static void selectEngineHeader(bool log)
{
  static char text[16]; // the adapter's "OK"
  char command[16];
  if (protocolIs29Bit(adapterStatus.protocol))
  {
    // 29-bit: ATSH sets the last 3 bytes, the adapter puts the 18 priority byte in front -> 18 DA <engine> F1
    snprintf(command, sizeof(command), "ATSHDA%02XF1", engineAddress29Bit);
  }
  else
  {
    copyStr(command, sizeof(command), "ATSH7E0"); // 11-bit: 0x7E0 = the engine ECU
  }
  elmCommand(command, text, sizeof(text), 1000, log);
}

// asks every ECU for its PID list (01 00) and keeps the engine's answer in "engine". Several ECUs answer (Tiguan:
// engine 18DAF101 and gearbox 18DAF102) and the engine isn't always first in the adapter's output.
// 11-bit: the engine answers on 7E8. 29-bit: its address isn't fixed between makes, so the answer with the most PIDs.
// "text" is the caller's buffer for the adapter's answer (saves another buffer in internal RAM).
static bool requestEngineSupported(ElmMsg &engine, char *text, size_t textLen)
{
  if (!elmCommand("0100", text, textLen, 3000, true) || strstr(text, "NO DATA"))
  {
    return false;
  }
  ElmMsg msgs[6];
  const uint8_t messageCount = parseAnswer(text, msgs, 6);
  int bestMessage = -1;      // which answer is the engine's (-1 = none yet)
  uint8_t bestPidCount = 0; // how many PIDs that answer lists
  for (uint8_t i = 0; i < messageCount; i++)
  {
    // a good answer is [41][00][4 bitmap bytes]
    if (msgs[i].len < 6 || msgs[i].data[0] != 0x41 || msgs[i].data[1] != 0x00)
    {
      continue;
    }
    if (!protocolIs29Bit(adapterStatus.protocol) && msgs[i].header == 0x7E8)
    {
      bestMessage = i; // 11-bit: 7E8 is the engine, no need to look further
      break;
    }
    // count the PIDs this ECU says it has (one bit each in the 4 bitmap bytes)
    uint8_t pidCount = 0;
    for (uint8_t byteIndex = 2; byteIndex < 6; byteIndex++)
    {
      for (uint8_t bit = 0; bit < 8; bit++)
      {
        if (msgs[i].data[byteIndex] & (1 << bit))
        {
          pidCount++;
        }
      }
    }
    if (bestMessage < 0 || pidCount > bestPidCount)
    {
      bestMessage = i;
      bestPidCount = pidCount;
    }
  }
  if (bestMessage < 0)
  {
    return false;
  }
  engine = msgs[bestMessage];

  // 29-bit: another ECU that answered is taken as the gearbox. VW's gearbox address 0x02 wins if it answered (a car with
  // more ECUs, e.g. a hybrid, could list something else first); nobody else answered = 0x02 stays
  if (protocolIs29Bit(adapterStatus.protocol))
  {
    bool hasVwGearbox = false; // an ECU at 0x02 answered
    int otherMessage = -1;     // the first other ECU that answered (-1 = none)
    for (uint8_t i = 0; i < messageCount; i++)
    {
      if (i == bestMessage || msgs[i].len < 6 || msgs[i].data[0] != 0x41 || msgs[i].data[1] != 0x00)
      {
        continue;
      }
      if ((msgs[i].header & 0xFF) == 0x02)
      {
        hasVwGearbox = true;
      }
      else if (otherMessage < 0)
      {
        otherMessage = i;
      }
    }
    gearboxAddress29Bit = 0x02;
    if (!hasVwGearbox && otherMessage >= 0)
    {
      gearboxAddress29Bit = msgs[otherMessage].header & 0xFF; // the last byte of 18DAF1xx
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// raw CAN mode (0.8.0) - DiagLink over the adapter, for TP2.0 (which is not ISO-TP, so the ELM's own framing can't
// carry it):
//   ATCAF0  - send our bytes as they are, show received frames whole (no PCI handling)
//   ATCFC0  - don't answer frames that look like ISO-TP first frames with a flow control of its own (TP2.0 data frames
//             starting 0x1x would trigger it)
//   ATV1    - variable DLC: a frame goes out at the length we give, as in the capture (refused -> padded to 8, logged)
//   ATCRA / ATSH are sent only when the id changes. ATH1 ATS0 from the session init stay: answers read "300A10F8AFF4AFF"
// ---------------------------------------------------------------------------
class ElmRawLink : public DiagLink
{
public:
  bool enter();
  void leave();
  void forget() // the adapter was reset (ATZ): back in normal mode
  {
    rawMode = false;
    invalidate();
    flush();
  }
  void invalidate()
  {
    headerId = 0xFFFF;
    filterId = 0xFFFF;
  } // a terminal command may have changed ATSH / ATCRA
  bool active() const
  {
    return rawMode;
  }
  bool varDlc = false; // ATV1 accepted
  bool send(uint16_t id, const uint8_t *data, uint8_t len) override;
  bool recv(DiagFrame &frame, uint32_t waitMs) override
  {
    if (queueHead == queueTail)
    {
      // nothing more will come: the adapter only listens while a command runs. Waiting keeps the client's timers honest
      if (waitMs)
      {
        vTaskDelay(pdMS_TO_TICKS(waitMs));
      }
      return false;
    }
    frame = queue[queueTail];
    queueTail = (queueTail + 1) % QUEUE_LEN;
    return true;
  }
  void listen(uint16_t id) override
  {
    wantedId = id;
  }
  void flush() override
  {
    queueHead = 0;
    queueTail = 0;
  }

private:
  static const uint8_t QUEUE_LEN = 24;
  bool rawMode = false;
  uint16_t headerId = 0xFFFF;
  uint16_t filterId = 0xFFFF;
  uint16_t wantedId = 0;
  DiagFrame queue[QUEUE_LEN];
  uint8_t queueHead = 0;
  uint8_t queueTail = 0;
  void push(const DiagFrame &frame)
  {
    const uint8_t next = (queueHead + 1) % QUEUE_LEN;
    if (next == queueTail)
    {
      return; // full: drop (the client repeats)
    }
    queue[queueHead] = frame;
    queueHead = next;
  }
};

bool ElmRawLink::enter()
{
  static char text[64];
  if (!elmCommand("ATCAF0", text, sizeof(text), 1000, false) || !containsOk(text))
  {
    return false;
  }
  elmCommand("ATCFC0", text, sizeof(text), 1000, false);
  varDlc = elmCommand("ATV1", text, sizeof(text), 1000, false) && containsOk(text);
  rawMode = true;
  invalidate();
  flush();
  return true;
}

void ElmRawLink::leave()
{
  static char text[64];
  elmCommand("ATCAF1", text, sizeof(text), 1000, false);
  elmCommand("ATCFC1", text, sizeof(text), 1000, false);
  if (varDlc)
  {
    elmCommand("ATV0", text, sizeof(text), 1000, false);
  }
  elmCommand("ATCRA", text, sizeof(text), 1000, false); // no argument = filter back to the default
  selectEngineHeader(false);                            // back to the engine (11-bit 7E0, or 29-bit 18 DA xx F1)
  rawMode = false;
  invalidate();
  flush();
}

bool ElmRawLink::send(uint16_t id, const uint8_t *data, uint8_t len)
{
  if (!rawMode || !len || len > 8)
  {
    return false;
  }
  EXT_RAM_BSS_ATTR static char text[512];
  char cmd[20];
  if (wantedId != filterId)
  {
    if (wantedId)
    {
      snprintf(cmd, sizeof(cmd), "ATCRA%03X", wantedId);
    }
    else
    {
      copyStr(cmd, sizeof(cmd), "ATCRA");
    }
    if (!elmCommand(cmd, text, sizeof(text), 1000, false))
    {
      return false;
    }
    filterId = wantedId;
  }
  if (id != headerId)
  {
    snprintf(cmd, sizeof(cmd), "ATSH%03X", id);
    if (!elmCommand(cmd, text, sizeof(text), 1000, false))
    {
      return false;
    }
    headerId = id;
  }
  for (uint8_t i = 0; i < len; i++)
  {
    snprintf(cmd + i * 2, 3, "%02X", data[i]);
  }
  if (!elmCommand(cmd, text, sizeof(text), 2000, false))
  {
    return false;
  }
  // every frame heard in the ATST window: [3 hex id][data], one per line. NO DATA / CAN ERROR / ... aren't hex
  const char *pos = text;
  while (*pos)
  {
    char line[40];
    size_t lineLen = 0;
    while (*pos && *pos != '\r')
    {
      if (*pos != ' ' && lineLen < sizeof(line) - 1)
      {
        line[lineLen++] = *pos;
      }
      pos++;
    }
    if (*pos == '\r')
    {
      pos++;
    }
    if (lineLen < 5 || ((lineLen - 3) & 1) || lineLen > 3 + 16)
    {
      continue;
    }
    bool allHex = true;
    for (size_t i = 0; i < lineLen && allHex; i++)
    {
      allHex = isHex(line[i]);
    }
    if (!allHex)
    {
      continue;
    }
    DiagFrame frame;
    frame.id = (hexValue(line[0]) << 8) | (hexValue(line[1]) << 4) | hexValue(line[2]);
    frame.len = (lineLen - 3) / 2;
    for (uint8_t i = 0; i < frame.len; i++)
    {
      frame.data[i] = (hexValue(line[3 + i * 2]) << 4) | hexValue(line[4 + i * 2]);
    }
    if (!wantedId || frame.id == wantedId)
    {
      push(frame);
    }
  }
  return true;
}

static ElmRawLink rawLink;
static Tp20Client tp20Client(rawLink);

static void leaveRawMode()
{
  if (rawLink.active())
  {
    rawLink.leave();
  }
}

// ---------------------------------------------------------------------------
// 0.34.0: UDS requests for other tasks (elmDiagTransact). The Diagnostics tab and the Service tab run in the VAG diag task,
// which can't talk to the adapter itself (only this task does, one command at a time). It leaves a job here and waits;
// this task runs it between its own OBD requests. To reach a module the adapter is pointed at it: ATSH (request id),
// ATCRA (only that module's answers), ATCP (a 29-bit id's first byte), and when the module is on the other CAN id
// length than the car's OBD (an MQB Evo: OBD on 29-bit, most modules on 11-bit) the protocol is switched (ATSP6 / 7)
// for it. It stays pointed there while jobs keep coming; before any OBD / DSG / test / terminal work it is put back
// (diagHeadersBack). The ELM327 assembles long answers and sends the flow control itself.
// ---------------------------------------------------------------------------
#define DIAG_ANSWER_MAX 256              // longest answer kept (a long fault list)
static SemaphoreHandle_t diagMutex = NULL; // one caller at a time
static SemaphoreHandle_t diagDone = NULL;  // given by this task when a job is done
static volatile bool diagPending = false;  // a job waits for this task
static volatile uint32_t diagJobNumber = 0;  // counts jobs, so a late answer to a job given up on isn't taken for the next
static volatile uint32_t diagDoneNumber = 0; // the number of the job last finished
static DiagTarget diagJobTarget;             // the job: where to
static uint8_t diagJobRequest[8];            // ... what to send
static uint8_t diagJobRequestLen = 0;
static uint32_t diagJobWaitMs = 0;
EXT_RAM_BSS_ATTR static uint8_t diagJobAnswer[DIAG_ANSWER_MAX]; // ... and what came back
static uint16_t diagJobAnswerLen = 0;
static bool diagJobOk = false;
static bool diagJustRan = false;          // the last loop ran a job: the next one does the task's own work first
static bool diagHeadersSet = false;       // the adapter is pointed at a module (not the engine / OBD as usual)
static DiagTarget diagHeadersFor;         // ... this one
static uint8_t diagProtocolSwitched = 0;  // the protocol switched to for a module (0 = the car's own)

// the CAN protocol for a module: 11-bit or 29-bit at the car's own speed (500k: 6 / 7, 250k: 8 / 9)
static uint8_t diagProtocolFor(const DiagTarget &target)
{
  const uint8_t carProtocol = adapterStatus.protocol;
  const bool fast = carProtocol == 6 || carProtocol == 7; // 500 kbit/s
  if (target.is29Bit)
  {
    if (fast)
    {
      return 7;
    }
    return 9;
  }
  if (fast)
  {
    return 6;
  }
  return 8;
}

// two targets are the same module, addressed the same way
static bool diagSameTarget(const DiagTarget &first, const DiagTarget &second)
{
  return first.requestId == second.requestId && first.answerId == second.answerId &&
         first.is29Bit == second.is29Bit && first.txExt == second.txExt && first.rxExt == second.rxExt;
}

// puts the adapter back the way OBD polling expects it: no receive filter, priority 18, the car's own protocol, no
// extended addressing, the engine's header
static void diagHeadersBack()
{
  if (!diagHeadersSet && diagProtocolSwitched == 0)
  {
    return;
  }
  static char text[32];
  if (diagHeadersSet && diagHeadersFor.txExt)
  {
    elmRxExtAddr = 0;
    elmCommand("ATCEA", text, sizeof(text), 1000, false);   // extended addressing off
    elmCommand("ATFCSM0", text, sizeof(text), 1000, false); // the adapter's own flow control again
    elmBmwFlowControlSet = false;                           // the next ext job sets its own again
  }
  elmCommand("ATCRA", text, sizeof(text), 1000, false); // every answer again
  if (diagHeadersSet && diagHeadersFor.is29Bit)
  {
    elmCommand("ATCP18", text, sizeof(text), 1000, false); // the usual 29-bit priority
  }
  if (diagProtocolSwitched != 0)
  {
    char command[8];
    snprintf(command, sizeof(command), "ATSP%X", adapterStatus.protocol); // the car's own protocol
    elmCommand(command, text, sizeof(text), 1000, false);
    protocolOverride = 0;
    diagProtocolSwitched = 0;
  }
  diagHeadersSet = false;
  selectEngineHeader(false);
}

// points the adapter at the job's module (only what differs from last time is sent)
static bool diagPointAt(const DiagTarget &target)
{
  static char text[32];
  char command[24];
  const uint8_t wanted = diagProtocolFor(target);
  uint8_t running = adapterStatus.protocol;
  if (diagProtocolSwitched != 0)
  {
    running = diagProtocolSwitched;
  }
  if (wanted != running)
  {
    if (diagHeadersSet)
    {
      diagHeadersBack(); // filters and priority back first, then switch
    }
    snprintf(command, sizeof(command), "ATSP%X", wanted);
    if (!elmCommand(command, text, sizeof(text), 1000, false) || !containsOk(text))
    {
      return false;
    }
    if (wanted == adapterStatus.protocol)
    {
      diagProtocolSwitched = 0;
      protocolOverride = 0;
    }
    else
    {
      diagProtocolSwitched = wanted;
      protocolOverride = wanted; // parseAnswer reads 3 or 8 header characters by the protocol running
    }
    diagHeadersSet = false;
  }
  if (diagHeadersSet && diagSameTarget(diagHeadersFor, target))
  {
    return true; // still pointed there
  }
  if (target.is29Bit)
  {
    // 29-bit: ATCP is the id's first byte (the priority: 18 or VW's 17), ATSH the other three
    snprintf(command, sizeof(command), "ATCP%02X", (unsigned)((target.requestId >> 24) & 0x1F));
    elmCommand(command, text, sizeof(text), 1000, false);
    snprintf(command, sizeof(command), "ATSH%06lX", (unsigned long)(target.requestId & 0xFFFFFF));
    elmCommand(command, text, sizeof(text), 1000, false);
    snprintf(command, sizeof(command), "ATCRA%08lX", (unsigned long)target.answerId);
  }
  else
  {
    snprintf(command, sizeof(command), "ATSH%03lX", (unsigned long)target.requestId);
    elmCommand(command, text, sizeof(text), 1000, false);
    snprintf(command, sizeof(command), "ATCRA%03lX", (unsigned long)target.answerId);
  }
  if (!elmCommand(command, text, sizeof(text), 1000, false) || !containsOk(text))
  {
    return false;
  }
  if (target.txExt)
  {
    // BMW-style: the module's address goes in front of every frame (ATCEA), and the flow control for a long answer
    // is our own so it carries the address too (as pollDsg does for the 8HP)
    snprintf(command, sizeof(command), "ATCEA%02X", target.txExt);
    if (!elmCommand(command, text, sizeof(text), 1000, false) || !containsOk(text))
    {
      return false; // the adapter can't do extended addressing
    }
    snprintf(command, sizeof(command), "ATFCSH%03lX", (unsigned long)target.requestId);
    elmCommand(command, text, sizeof(text), 1000, false);
    snprintf(command, sizeof(command), "ATFCSD%02X300000", target.txExt); // [address][30 = carry on][00][00]
    elmCommand(command, text, sizeof(text), 1000, false);
    elmCommand("ATFCSM1", text, sizeof(text), 1000, false); // use that flow control
    elmRxExtAddr = target.rxExt;
  }
  diagHeadersFor = target;
  diagHeadersSet = true;
  return true;
}

// the module's answer out of the adapter's lines: single frames, or a first frame and its consecutive frames.
// A "response pending" (7F xx 78) is skipped: the real answer follows it. Returns the payload length (0 = none).
static uint16_t diagParseAnswer(const char *text, uint8_t headerChars, uint8_t rxExt, uint8_t *out, uint16_t outMax)
{
  uint16_t total = 0;   // a long answer's length (0 = none being collected)
  uint16_t got = 0;     // bytes collected
  const char *pos = text;
  while (*pos)
  {
    // one line, spaces left out
    char line[64];
    uint8_t lineLen = 0;
    while (*pos && *pos != '\r')
    {
      if (*pos != ' ' && lineLen < sizeof(line) - 1)
      {
        line[lineLen] = *pos;
        lineLen++;
      }
      pos++;
    }
    if (*pos == '\r')
    {
      pos++;
    }
    line[lineLen] = '\0';
    bool allHex = lineLen > headerChars + 1;
    for (uint8_t i = 0; i < lineLen && allHex; i++)
    {
      allHex = isHex(line[i]);
    }
    if (!allHex || ((lineLen - headerChars) & 1))
    {
      continue; // NO DATA, CAN ERROR, a half line
    }
    uint8_t bytes[8];
    uint8_t byteCount = 0;
    for (uint8_t i = headerChars; i + 1 < lineLen && byteCount < 8; i += 2)
    {
      bytes[byteCount] = (hexValue(line[i]) << 4) | hexValue(line[i + 1]);
      byteCount++;
    }
    uint8_t first = 0; // where the ISO-TP bytes start (after an extended address byte)
    if (rxExt != 0 && byteCount >= 2 && bytes[0] == rxExt)
    {
      first = 1;
    }
    if (byteCount <= first)
    {
      continue;
    }
    const uint8_t frameType = bytes[first] >> 4; // 0 single, 1 first, 2 consecutive
    if (frameType == 0 && total == 0)
    {
      uint8_t length = bytes[first] & 0x0F;
      if (length > byteCount - first - 1)
      {
        length = byteCount - first - 1;
      }
      if (length >= 3 && bytes[first + 1] == 0x7F && bytes[first + 3] == 0x78)
      {
        continue; // response pending: the answer comes later
      }
      if (length > outMax)
      {
        length = outMax;
      }
      memcpy(out, &bytes[first + 1], length);
      return length;
    }
    if (frameType == 1 && total == 0 && byteCount >= first + 2)
    {
      total = ((bytes[first] & 0x0F) << 8) | bytes[first + 1];
      if (total > outMax)
      {
        total = outMax;
      }
      got = 0;
      for (uint8_t i = first + 2; i < byteCount && got < total; i++)
      {
        out[got] = bytes[i];
        got++;
      }
      continue;
    }
    if (frameType == 2 && total > 0)
    {
      for (uint8_t i = first + 1; i < byteCount && got < total; i++)
      {
        out[got] = bytes[i];
        got++;
      }
      if (got >= total)
      {
        return total;
      }
    }
  }
  if (total > 0 && got >= total)
  {
    return total;
  }
  return 0;
}

// runs the waiting job (in this task)
static void runDiagJob()
{
  EXT_RAM_BSS_ATTR static char text[2048]; // a 256-byte answer is ~37 lines of hex
  const uint32_t jobNumber = diagJobNumber;
  const DiagTarget target = diagJobTarget;
  diagPending = false;
  diagJobOk = false;
  diagJobAnswerLen = 0;
  leaveRawMode();
  if (protocolIsCan(adapterStatus.protocol) && settings.elmExtraSource == ELM_EXTRA_OFF && diagPointAt(target))
  {
    char hexRequest[2 * 8 + 1];
    for (uint8_t i = 0; i < diagJobRequestLen; i++)
    {
      snprintf(hexRequest + i * 2, 3, "%02X", diagJobRequest[i]);
    }
    if (elmCommand(hexRequest, text, sizeof(text), diagJobWaitMs, false) && !strstr(text, "NO DATA"))
    {
      uint8_t headerChars = 3;
      if (target.is29Bit)
      {
        headerChars = 8;
      }
      diagJobAnswerLen = diagParseAnswer(text, headerChars, target.rxExt, diagJobAnswer, DIAG_ANSWER_MAX);
      diagJobOk = diagJobAnswerLen > 0;
    }
    else
    {
      adapterStatus.noData++;
    }
  }
  diagDoneNumber = jobNumber;
  xSemaphoreGive(diagDone);
}

bool elmDiagTransact(const DiagTarget &target, const uint8_t *request, uint8_t requestLen, uint8_t *answer,
                     uint16_t answerMax, uint16_t &answerLen, uint32_t waitMs)
{
  answerLen = 0;
  uint8_t room = 7; // a single CAN frame's payload
  if (target.txExt)
  {
    room = 6; // the address byte takes one
  }
  if (!connectionUp || !elmTask || !diagMutex || requestLen == 0 || requestLen > room)
  {
    return false;
  }
  if (xSemaphoreTake(diagMutex, pdMS_TO_TICKS(waitMs + 5000)) != pdTRUE)
  {
    return false;
  }
  xSemaphoreTake(diagDone, 0); // a give left over from a job given up on
  diagJobTarget = target;
  memcpy(diagJobRequest, request, requestLen);
  diagJobRequestLen = requestLen;
  diagJobWaitMs = waitMs;
  diagJobNumber = diagJobNumber + 1; // volatile: an explicit read + write
  const uint32_t myJob = diagJobNumber;
  diagPending = true;
  xTaskNotifyGive(elmTask); // wakes the ELM task if it is idling
  bool ok = false;
  // the ELM task gets to it after its current OBD request; then the job's own commands and answer
  const uint32_t deadline = millis() + waitMs + 4000;
  while ((int32_t)(deadline - millis()) > 0)
  {
    if (xSemaphoreTake(diagDone, pdMS_TO_TICKS(100)) == pdTRUE && diagDoneNumber == myJob)
    {
      ok = diagJobOk;
      break;
    }
    if (!connectionUp)
    {
      break;
    }
  }
  diagPending = false;
  if (ok)
  {
    answerLen = diagJobAnswerLen;
    if (answerLen > answerMax)
    {
      answerLen = answerMax;
    }
    memcpy(answer, diagJobAnswer, answerLen);
  }
  xSemaphoreGive(diagMutex);
  return ok;
}

// ---------------------------------------------------------------------------
// vehicle identification
// ---------------------------------------------------------------------------
// 0.35.0: the WMI and VW model tables and the VIN decode moved to src/can2gauge_Vehicle.cpp (every VIN source uses
// them); identifyVehicle() below hands the VIN over and copies the result into adapterStatus

static void identifyVehicle()
{
  setState("identifying");
  char vin[18] = "";
  EXT_RAM_BSS_ATTR static char text[768];
  // 1) mode 09 PID 02
  if (elmCommand("0902", text, sizeof(text), 5000))
  {
    ElmMsg msgs[8];
    const uint8_t messageCount = parseAnswer(text, msgs, 8);
    uint8_t rawVin[40];
    uint8_t rawLen = 0;
    if (protocolIsCan(adapterStatus.protocol))
    {
      for (uint8_t i = 0; i < messageCount; i++)
      {
        if (msgs[i].len >= 20 && msgs[i].data[0] == 0x49 && msgs[i].data[1] == 0x02)
        {
          // 0.36.1: the last 17 bytes, so an ECU that pads in front of the VIN (some non-VW ones do) still reads right
          memcpy(rawVin, &msgs[i].data[msgs[i].len - 17], 17);
          rawLen = 17;
          break;
        }
      }
    }
    else
    {
      // K-line / J1850: up to 5 lines [49][02][n][4 bytes]
      for (uint8_t seq = 1; seq <= 5; seq++)
      {
        for (uint8_t i = 0; i < messageCount; i++)
        {
          if (msgs[i].len >= 7 && msgs[i].data[0] == 0x49 && msgs[i].data[1] == 0x02 && msgs[i].data[2] == seq)
          {
            memcpy(rawVin + rawLen, &msgs[i].data[3], 4);
            rawLen += 4;
          }
        }
      }
    }
    if (rawLen >= 17)
    {
      memcpy(vin, rawVin + rawLen - 17, 17);
      vin[17] = '\0';
    }
  }
  // 2) UDS 22 F190 on the engine (MQB and later answer this even when 0902 is missing)
  if (!vehicleVinValid(vin) && protocolIsCan(adapterStatus.protocol))
  {
    ElmMsg response;
    selectEngineHeader(true);
    if (elmRequest("22F190", response, 3000, true) && response.len >= 20 && response.data[0] == 0x62)
    {
      memcpy(vin, &response.data[3], 17);
      vin[17] = '\0';
    }
  }
  if (vehicleVinValid(vin))
  {
    // 0.35.0: decoded by the vehicle module (shared with the CAN and K-line sources), then copied here for the tab
    vehicleTakeVin(vin, VIN_SRC_ADAPTER, adapterStatus.protocol && !protocolIsCan(adapterStatus.protocol));
    VehicleInfo detected;
    vehicleDetectedCopy(detected);
    copyStr(adapterStatus.vin, sizeof(adapterStatus.vin), detected.vin);
    copyStr(adapterStatus.make, sizeof(adapterStatus.make), detected.make);
    copyStr(adapterStatus.makeKey, sizeof(adapterStatus.makeKey), detected.makeKey);
    copyStr(adapterStatus.model, sizeof(adapterStatus.model), detected.model);
    copyStr(adapterStatus.modelCode, sizeof(adapterStatus.modelCode), detected.modelCode);
    adapterStatus.yearNum = detected.year;
    adapterStatus.year[0] = '\0';
    if (detected.year)
    {
      snprintf(adapterStatus.year, sizeof(adapterStatus.year), "%u?", detected.year);
    }
    char message[TERM_W];
    snprintf(message, sizeof(message), "Detected: %s %s%s%s%s", adapterStatus.make, adapterStatus.model,
             adapterStatus.modelCode[0] ? " (" : "", adapterStatus.modelCode, adapterStatus.modelCode[0] ? ")" : "");
    terminalAdd("# ", message);
  }
  else
  {
    adapterStatus.vin[0] = '\0';
    copyStr(adapterStatus.make, sizeof(adapterStatus.make), "unknown (no VIN)");
    terminalAdd("# ", "no VIN from 0902 / 22F190 - generic OBD only");
  }
}

// ---------------------------------------------------------------------------
// session setup
// ---------------------------------------------------------------------------
static uint8_t readProtocol()
{
  static char text[64];
  if (!elmCommand("ATDPN", text, sizeof(text), 1000))
  {
    return 0;
  }
  for (const char *pos = text; *pos; pos++)
  {
    if (*pos == 'A' && isHex(pos[1]))
    {
      continue; // "A6" = automatic, protocol 6
    }
    if (isHex(*pos))
    {
      return hexValue(*pos);
    }
  }
  return 0;
}

static ObdSchedule elmObdSchedule;    // the car's PID lists (from readSupported) and where the fast / slow rotation is
static uint8_t elmAdaptiveTiming = 1; // the ATAT setting the adapter has now (sessionInit sends ATAT1)
static bool elmCountDigitOff = false; // Fast OBD's "one answer" digit made answers come back short: off this session
static uint8_t elmShortAnswers = 0;   // short answers in a row with that digit
// 0.37.6: Fast OBD is on by default; when its quicker timing (ATAT2) loses answers it drops back to standard timing
#define ELM_LOST_ANSWERS_MAX 3        // requests in a row with no answer before Fast OBD's timing goes back to standard
static bool elmFastTimingOff = false; // Fast OBD's timing lost answers: standard timing this session
static uint8_t elmLostAnswers = 0;    // requests in a row with no answer at Fast OBD's timing
#define ELM_SHORT_ANSWERS_MAX 3       // ... before it's turned off

static void readSupported(const ElmMsg &first)
{
  // bitmap after [41][00]
  memset(adapterStatus.supported, 0, sizeof(adapterStatus.supported));
  if (first.len >= 6)
  {
    adapterStatus.supported[0] = ((uint32_t)first.data[2] << 24) | (first.data[3] << 16) | (first.data[4] << 8) | first.data[5];
  }
  for (uint8_t block = 1; block < 3; block++)
  {
    if (!(adapterStatus.supported[block - 1] & 1))
    {
      break; // "next block supported" bit
    }
    char request[8];
    snprintf(request, sizeof(request), "01%02X", block * 0x20);
    ElmMsg response;
    if (elmRequest(request, response, 2000, true) && response.len >= 6 && response.data[0] == 0x41)
    {
      adapterStatus.supported[block] =
          ((uint32_t)response.data[2] << 24) | (response.data[3] << 16) | (response.data[4] << 8) | response.data[5];
    }
  }
  // the shared PID picker (CANLine) works from the same lists
  obdScheduleReset(elmObdSchedule);
  for (uint8_t block = 0; block < 3; block++)
  {
    elmObdSchedule.supported[block] = adapterStatus.supported[block];
  }
}

static bool sessionInit()
{
  EXT_RAM_BSS_ATTR static char text[256];
  setState("init");
  rawLink.forget(); // ATZ puts the adapter back in normal mode, and a TP2.0 channel doesn't survive the gap
  tp20Client.drop();
  klineSessionUp = false; // same for a K-line session
  canMonArmed = false;    // ATZ stops any ATMA monitor: pollCanMonitor() re-arms it
  protocolOverride = 0;
  elmRxExtAddr = 0;             // 0.26.0: and forgets BMW's extended addressing
  elmBmwFlowControlSet = false;
  diagHeadersSet = false;       // 0.34.0: ATZ forgets a module the Diagnostics / Service tab pointed it at
  diagProtocolSwitched = 0;
  elmCommand("ATZ", text, sizeof(text), 2500);
  vTaskDelay(pdMS_TO_TICKS(300));
  const char *setupCommands[] = {"ATE0", "ATL0", "ATS0", "ATH1", "ATAT1"};
  for (const char *command : setupCommands)
  {
    if (!elmCommand(command, text, sizeof(text), 1000) || !containsOk(text))
    {
      char error[48];
      snprintf(error, sizeof(error), "%s refused", command);
      setError(error);
      return false;
    }
  }
  elmAdaptiveTiming = 1;    // ATAT1 above; pollObd moves it to ATAT2 when Fast OBD is on
  elmCountDigitOff = false; // a new session gives Fast OBD's digit another go
  elmShortAnswers = 0;
  elmFastTimingOff = false; // 0.37.6: ... and its timing
  elmLostAnswers = 0;
  elmCommand("ATI", text, sizeof(text), 1000);
  char adapterId[48];
  copyStr(adapterId, sizeof(adapterId), text[0] == '\r' ? text + 1 : text);
  for (char *pos = adapterId; *pos; pos++)
  {
    if (*pos == '\r')
    {
      *pos = ' ';
    }
  }
  adapterStatus.stn = elmCommand("STI", text, sizeof(text), 1000) && text[0] && !strchr(text, '?');
  for (char *pos = text; *pos; pos++)
  {
    if (*pos == '\r')
    {
      *pos = ' ';
    }
  }
  if (adapterStatus.stn)
  {
    snprintf(adapterStatus.adapter, sizeof(adapterStatus.adapter), "%.20s / %.24s", adapterId, text);
  }
  else
  {
    copyStr(adapterStatus.adapter, sizeof(adapterStatus.adapter), adapterId);
  }
  if (elmCommand("ATRV", text, sizeof(text), 1000))
  {
    adapterStatus.volts = atof(text);
  }

  // CAN broadcast source (ELM_EXTRA_CAN_MONITOR): skip OBD protocol detection - pollCanMonitor() puts the adapter straight into
  // CAN monitor. This also lets it work on a car that broadcasts on CAN but answers no OBD (0100 would get no reply).
  if (settings.elmExtraSource == ELM_EXTRA_CAN_MONITOR)
  {
    adapterStatus.protocol = 6; // ISO 15765-4 CAN 11-bit / 500k, what pollCanMonitor forces
    return true;
  }

  // protocol: cached one first (skips the 5-10 s search), auto search as the fallback
  setState("protocol search");
  ElmMsg response;
  bool ok = false;
  // 0.24.1: three tries - the cached protocol, the same again after a 5.5 s wait if it is a K-line one, then the auto
  // search. A K-line ECU (ISO 9141-2 / KWP2000) keeps its last session for up to 5 s (P3max) and ignores a new wake-up
  // until then, so after a reset mid-session the first 0100 gets UNABLE TO CONNECT; waiting it out lets the second
  // one through instead of dropping the adapter (bench: Read DTCs reconnected the adapter every time)
  for (int attempt = 0; attempt < 3 && !ok; attempt++)
  {
    uint8_t cached = settings.elmProtocol;
    if (attempt == 0 && cached == 0)
    {
      cached = 6; // 0.37.6: no protocol known yet: ISO 15765-4 CAN (11-bit, 500k) first - most cars since 2008 (the X5) -
                  // a quick yes or no, before the adapter's slow search through every protocol
    }
    if (attempt == 2)
    {
      cached = 0; // the last try: the adapter's own search
    }
    if (attempt == 1)
    {
      const bool kLine = cached >= 3 && cached <= 5;
      if (!kLine)
      {
        continue; // CAN / J1850: nothing to wait for, straight to the search
      }
      setState("waiting for the K-line");
      terminalAdd("# ", "K-line busy (the ECU still holds the last session): trying again in 5.5 s");
      vTaskDelay(pdMS_TO_TICKS(5500));
    }
    char selectCommand[8];
    snprintf(selectCommand, sizeof(selectCommand), "ATSP%X", cached);
    elmCommand(selectCommand, text, sizeof(text), 1000);
    // header width isn't known until ATDPN, so just look for the 41 00 answer in the text here
    ok = elmCommand("0100", text, sizeof(text), 15000) && (strstr(text, "4100") || strstr(text, "41 00"));
    if (!ok && cached == 0)
    {
      break;
    }
  }
  if (!ok)
  {
    setError("no answer to 0100 (ignition on?)");
    return false;
  }
  adapterStatus.protocol = readProtocol();
  if (adapterStatus.protocol && adapterStatus.protocol != settings.elmProtocol)
  {
    settings.elmProtocol = adapterStatus.protocol;
    settingsDirty = true;
  }
  // with the real protocol known, re-parse the 0100 answer for the bitmap (header width may have been guessed wrong).
  // 0.25.3: the engine's answer, not just the first one (on 29-bit the gearbox can come first)
  if (!requestEngineSupported(response, text, sizeof(text)))
  {
    setError("0100 unreadable after protocol detect");
    return false;
  }
  if (protocolIs29Bit(adapterStatus.protocol))
  {
    engineAddress29Bit = response.header & 0xFF; // the last byte of 18DAF1xx = the ECU that answered (the engine)
  }
  readSupported(response);
  identifyVehicle();
  if (protocolIsCan(adapterStatus.protocol))
  {
    selectEngineHeader(true); // physical engine address: only the ECU answers, no waiting on others
  }
  return true;
}

// ---------------------------------------------------------------------------
// polling
// ---------------------------------------------------------------------------
// decodes a mode 01 answer; returns how many PIDs it held
static uint8_t decodeMode01(const ElmMsg &message)
{
  if (message.len < 2 || message.data[0] != 0x41)
  {
    return 0;
  }
  // one or several [pid][data] pairs after the 41
  uint8_t decoded = 0;
  uint16_t pos = 1;
  while (pos < message.len)
  {
    const uint8_t pid = message.data[pos];
    const uint8_t len = obdPidLen(pid);
    if (!len || pos + 1 + len > message.len)
    {
      break;
    }
    obdDecodePid(pid, &message.data[pos + 1], len, signalSet);
    decoded++;
    pos += 1 + len;
  }
  return decoded;
}

// one OBD request through the adapter: the PIDs come from the shared picker (CANLine) - on CAN up to 6 per request
// (5 fast + 1 slow), on K-line / J1850 one per request (a slow one every 4th)
static void pollObd()
{
  leaveRawMode(); // OBD requests need the adapter's own ISO-TP framing (ATCAF1)

  // Fast OBD: the adapter's aggressive adaptive timing (ATAT2: it shortens its wait for the ECU harder) instead of ATAT1
  uint8_t wantedTiming = 1;
  if (settings.elmFastObd && !elmFastTimingOff) // 0.37.6: unless it lost answers this session
  {
    wantedTiming = 2;
  }
  if (wantedTiming != elmAdaptiveTiming)
  {
    char text[16];
    char command[8];
    snprintf(command, sizeof(command), "ATAT%u", wantedTiming);
    elmCommand(command, text, sizeof(text), 1000, false);
    elmAdaptiveTiming = wantedTiming;
  }

  const bool isCan = protocolIsCan(adapterStatus.protocol);
  uint8_t maxPids = 1; // K-line / J1850: one PID per request
  if (isCan)
  {
    maxPids = OBD_MAX_PIDS; // CAN: up to 6
  }
  uint8_t pids[OBD_MAX_PIDS];
  const uint8_t pidCount = obdSchedulePick(elmObdSchedule, pids, maxPids);
  if (pidCount == 0)
  {
    return;
  }
  char request[24];
  size_t written = 0;
  appendf(request, sizeof(request), written, "01"); // service 01, then the PIDs
  for (uint8_t i = 0; i < pidCount; i++)
  {
    appendf(request, sizeof(request), written, "%02X", pids[i]);
  }

  // The "one answer" digit on the end of a request: the adapter hands the answer back as soon as it's in, instead of
  // waiting in case another ECU answers too. Always for one PID on CAN (the engine header, 7E0 or 18DAxxF1, asks only the engine ECU). With Fast
  // OBD also for several PIDs and on K-line / J1850 - turned off for the session if answers then come back short (an
  // adapter that counts each CAN frame of a long answer as "an answer" stops after the first one).
  // 0.24.1: on K-line / J1850 always (one PID per request there): the bench speed test (8N0906018J, ISO 9141-2, an
  // ELM327 v2.3 BLE clone) had every answer back in 184 ms with the digit and 254 ms without - 5.4 instead of 3.9
  // requests a second - whatever the adaptive timing or answer window was set to.
  bool fastDigit = false;
  if (isCan && pidCount == 1)
  {
    appendf(request, sizeof(request), written, "1");
  }
  else if ((settings.elmFastObd || !isCan) && !elmCountDigitOff)
  {
    appendf(request, sizeof(request), written, "1");
    fastDigit = true;
  }

  ElmMsg response;
  uint32_t timeoutMs = 1500; // K-line / J1850 answer slower
  if (isCan)
  {
    timeoutMs = 1000;
  }
  uint8_t decoded = 0;
  if (elmRequest(request, response, timeoutMs))
  {
    decoded = decodeMode01(response);
  }
  // 0.37.6: Fast OBD first, with a fall-back: answers lost at its quicker timing put the standard timing back
  if (elmAdaptiveTiming == 2)
  {
    if (decoded == 0)
    {
      elmLostAnswers++;
      if (elmLostAnswers >= ELM_LOST_ANSWERS_MAX)
      {
        elmFastTimingOff = true;
        terminalAdd("# ", "Fast OBD: answers went missing - standard timing until reconnect");
      }
    }
    else
    {
      elmLostAnswers = 0;
    }
  }

  if (fastDigit)
  {
    if (decoded < pidCount)
    {
      elmShortAnswers++;
      if (elmShortAnswers >= ELM_SHORT_ANSWERS_MAX)
      {
        elmCountDigitOff = true;
        terminalAdd("# ", "Fast OBD: answers came back short - one-answer digit off until reconnect");
      }
    }
    else
    {
      elmShortAnswers = 0;
    }
  }
}

static void pollDsg()
{
  if (!protocolIsCan(adapterStatus.protocol))
  {
    return;
  }
  leaveRawMode();
  static char text[64];
  static uint32_t lastSession = 0;
  char cmd[16];
  uint32_t answerId = settings.dsgRxId; // the id the gearbox answers on (DSG tab, 11-bit)
  if (protocolIs29Bit(adapterStatus.protocol))
  {
    // 0.25.3: a 29-bit car: 18 DA <gearbox> F1 out, 18 DA F1 <gearbox> back (Tiguan: 18DA02F1 -> 18DAF102). ATSH sets
    // the last 3 bytes, the adapter adds the 18
    snprintf(cmd, sizeof(cmd), "ATSHDA%02XF1", gearboxAddress29Bit);
    answerId = 0x18DAF100 | gearboxAddress29Bit;
  }
  else
  {
    snprintf(cmd, sizeof(cmd), "ATSH%03X", settings.dsgTxId);
  }
  elmCommand(cmd, text, sizeof(text), 1000, false);

  // 0.26.0: BMW extended addressing (Gearbox type ZF BMW: EGS 0x18 on 0x6F1, answers on 0x618 starting F1). The adapter
  // does it: ATCEA puts the EGS's address in front of every frame, ATCRA listens to the EGS only, and the flow control
  // for a long answer is our own (ATFCSM1) so it carries the address byte too. All put back after the reads, because
  // the engine polling in between must not have them.
  const bool useExtAddr = !protocolIs29Bit(adapterStatus.protocol) && settings.dsgTxExtAddr != 0;
  if (useExtAddr)
  {
    snprintf(cmd, sizeof(cmd), "ATCEA%02X", settings.dsgTxExtAddr);
    if (!elmCommand(cmd, text, sizeof(text), 1000, false) || !containsOk(text))
    {
      dsgSetState("not supported", "the adapter refused ATCEA (BMW extended addressing)");
      selectEngineHeader(false);
      return;
    }
    snprintf(cmd, sizeof(cmd), "ATCRA%03X", settings.dsgRxId); // only the gearbox's answers (0x618)
    elmCommand(cmd, text, sizeof(text), 1000, false);
    if (!elmBmwFlowControlSet)
    {
      // once a session (ATZ forgets them): the flow control goes out on our id, starting with the EGS's address
      snprintf(cmd, sizeof(cmd), "ATFCSH%03X", settings.dsgTxId);
      elmCommand(cmd, text, sizeof(text), 1000, false);
      snprintf(cmd, sizeof(cmd), "ATFCSD%02X300000", settings.dsgTxExtAddr); // [18][30 = continue][00][00]
      elmCommand(cmd, text, sizeof(text), 1000, false);
      elmBmwFlowControlSet = true;
    }
    elmCommand("ATFCSM1", text, sizeof(text), 1000, false); // use that flow control
    elmRxExtAddr = settings.dsgRxExtAddr; // parseAnswer drops the F1 in front of each answer
  }

  if (settings.dsgUdsSession == 0x03 && millis() - lastSession > 2000)
  {
    ElmMsg sessionResponse;
    elmRequest("1003", sessionResponse, 1000); // re-entering the session also restarts the gearbox's S3 timer
    lastSession = millis();
  }
  static uint32_t pass = 0; // 0.26.0: counts the passes, so the slow reads (temperature) are asked for every 10th
  for (uint8_t which = 0; which < DSG_READ_COUNT; which++)
  {
    if (!dsgReadDue(which, pass))
    {
      continue;
    }
    uint8_t request[4];
    const uint8_t requestLen = dsgBuildRequest(dsgReadSpec(which), request);
    char hexRequest[12];
    for (uint8_t i = 0; i < requestLen; i++)
    {
      snprintf(hexRequest + i * 2, 3, "%02X", request[i]);
    }
    ElmMsg response;
    if (elmRequest(hexRequest, response, 1000) && response.header == answerId)
    {
      dsgHandleResponse(which, response.data, response.len);
    }
  }
  pass++;

  if (useExtAddr)
  {
    elmRxExtAddr = 0;
    elmCommand("ATCEA", text, sizeof(text), 1000, false);   // no value = extended addressing off
    elmCommand("ATFCSM0", text, sizeof(text), 1000, false); // the adapter's own flow control again
    elmCommand("ATCRA", text, sizeof(text), 1000, false);   // listen to the usual answers again
  }
  selectEngineHeader(false); // back to the engine (on a 29-bit car 7E0 would break engine polling after every pass)
}

// DSG_SRC_ELM_TP20 (0.8.0): the TP2.0 gearbox read through the adapter in raw mode. The channel stays open across
// cycles; with OBD polling on, the adapter switches modes in between (5-8 extra commands), and if the gap outlasts the
// ECU's channel timeout the channel is simply opened again [U: bench - see the TP2.0 test's idle section].
static void pollDsgTp20()
{
  static uint32_t seenGeneration = 0;
  static uint32_t retryAt = 0;
  static uint8_t fails = 0;
  static bool gearTurn = false;
  char detail[64];
  if (adapterStatus.protocol != 6)
  {
    dsgSetState("not available", "TP2.0 needs CAN 11-bit 500k (ELM protocol 6)");
    return;
  }
  if (seenGeneration != dsgGeneration())
  {
    seenGeneration = dsgGeneration(); // DSG settings changed: start again
    if (tp20Client.isOpen() && rawLink.active())
    {
      tp20Client.close();
    }
    tp20Client.drop();
    fails = 0;
    retryAt = 0;
  }
  if (!tp20Client.isOpen() && (int32_t)(millis() - retryAt) < 0)
  {
    return;
  }
  static uint32_t lastRead = 0;
  if (tp20Client.isOpen() && rawLink.active() && millis() - lastRead < settings.dsgPollMs)
  {
    tp20Client.keepAlive(); // between reads: A3 once the channel has been idle 500 ms
    return;
  }
  lastRead = millis();
  if (!rawLink.active() && !rawLink.enter())
  {
    dsgSetState("ELM refused", "ATCAF0 not accepted - this adapter can't send raw CAN");
    retryAt = millis() + 10000;
    return;
  }
  if (!tp20Client.isOpen())
  {
    dsgSetState("connecting", "TP2.0 channel setup on 0x200 (through the ELM)");
    if (!tp20Client.open(settings.dsgTp20Module))
    {
      snprintf(detail, sizeof(detail), "module 0x%02X did not open a TP2.0 channel", settings.dsgTp20Module);
      dsgSetState("no channel", detail);
      retryAt = millis() + 3000;
      return;
    }
    dsgTp20Channel(tp20Client.txId(), tp20Client.rxId());
    const uint8_t sessionRequest[2] = {0x10, 0x89};
    uint8_t response[16];
    uint16_t responseLen = 0;
    if (!tp20Client.transact(sessionRequest, 2, response, sizeof(response), responseLen, 1000) || responseLen < 1 ||
        response[0] != 0x50)
    {
      dsgSetState("no session", "10 89 refused or no answer");
      tp20Client.close();
      retryAt = millis() + 3000;
      return;
    }
    snprintf(detail, sizeof(detail), "TP2.0 via ELM tx %03X rx %03X, group %u", tp20Client.txId(), tp20Client.rxId(),
             settings.dsgTp20Group);
    dsgSetState("polling", detail);
    fails = 0;
  }
  tp20Client.keepAlive();
  const bool gear = gearTurn && settings.dsgTp20GearGroup;
  DsgField fields[4];
  uint8_t fieldCount = 0;
  uint8_t nrc = 0;
  const bool ok = tp20Client.isOpen() &&
      tp20Client.readGroup(gear ? settings.dsgTp20GearGroup : settings.dsgTp20Group, fields, fieldCount, nrc);
  dsgTp20Result(gear, ok, fields, fieldCount, nrc);
  gearTurn = !gearTurn;
  fails = ok ? 0 : fails + 1;
  if (fails >= 5)
  {
    dsgSetState("lost", "5 block reads in a row failed");
    tp20Client.close();
    retryAt = millis() + 1000;
  }
}

// ---------------------------------------------------------------------------
// VAG KWP2000 over the adapter's K-line (0.8.0). An ELM-type adapter can't run KWP1281 (the tester has to echo every
// byte inverted; the adapter only sends / receives whole messages). KWP1281 modules stay on the gauge's own K-line.
//   ATSP4 + ATIIA <module>  5-baud init to the module      | ATSP5  fast init (ISO 14230)
//   ATKW0                   accept VAG key bytes (not just the ISO ones)
//   ATSH 80 <module> F1     physical header [U: the adapter puts the length into the format byte - bench check]
//   ATWM 81 <module> F1 3E  its idle keep-alive = TesterPresent to our module (default is C1 33 F1 3E, the OBD one)
//   ATSI / ATFI             the init now, then ATKW for the key bytes (01 8A = a KWP1281 module: refused)
//   10 89                   VAG diagnostic session for measuring blocks (some modules answer in the default session)
// A K-line session replaces the OBD one on the line, so with settings.elmExtraSource on, the adapter does only this.
// ---------------------------------------------------------------------------
static uint8_t klineKeyBytes[2] = {0, 0};

static void klineParseKeyBytes(const char *text)
{
  klineKeyBytes[0] = 0;
  klineKeyBytes[1] = 0;
  const char *key1Text = strstr(text, "1:"); // ATKW -> "1:8F 2:EF"
  const char *key2Text = strstr(text, "2:");
  if (key1Text && isHex(key1Text[2]) && isHex(key1Text[3]))
  {
    klineKeyBytes[0] = (hexValue(key1Text[2]) << 4) | hexValue(key1Text[3]);
  }
  if (key2Text && isHex(key2Text[2]) && isHex(key2Text[3]))
  {
    klineKeyBytes[1] = (hexValue(key2Text[2]) << 4) | hexValue(key2Text[3]);
  }
}

static bool klineConnect(uint8_t module, bool fast, char *why, size_t whyLen)
{
  EXT_RAM_BSS_ATTR static char text[256];
  char cmd[24];
  leaveRawMode();
  klineSessionUp = false;
  klineKeyBytes[0] = 0;
  klineKeyBytes[1] = 0;
  elmCommand(fast ? "ATSP5" : "ATSP4", text, sizeof(text), 1000, false);
  protocolOverride = fast ? 5 : 4;
  if (!fast)
  {
    snprintf(cmd, sizeof(cmd), "ATIIA%02X", module);
    elmCommand(cmd, text, sizeof(text), 1000, false);
  }
  elmCommand("ATKW0", text, sizeof(text), 1000, false);
  snprintf(cmd, sizeof(cmd), "ATSH80%02XF1", module);
  elmCommand(cmd, text, sizeof(text), 1000, false);
  snprintf(cmd, sizeof(cmd), "ATWM81%02XF13E", module);
  elmCommand(cmd, text, sizeof(text), 1000, false);
  elmCommand(fast ? "ATFI" : "ATSI", text, sizeof(text), 8000, false); // 5-baud: ~2 s for the address byte alone
  const bool initOk = strstr(text, "OK") && !strstr(text, "ERROR");
  if (elmCommand("ATKW", text, sizeof(text), 1000, false))
  {
    klineParseKeyBytes(text);
  }
  if (!initOk)
  {
    snprintf(why, whyLen, "%s init to 0x%02X: no answer", fast ? "fast" : "5-baud", module);
    return false;
  }
  if (klineKeyBytes[0] == 0x01 && klineKeyBytes[1] == 0x8A)
  {
    snprintf(why, whyLen, "0x%02X is a KWP1281 module (key bytes 01 8A) - use the gauge's own K-line", module);
    return false;
  }
  klineSessionUp = true;
  ElmMsg response;
  elmRequest("1089", response, 1500); // not fatal: some modules give measuring blocks in the default session
  return true;
}

static void klineClose()
{
  if (klineSessionUp)
  {
    ElmMsg response;
    elmRequest("82", response, 500); // StopCommunication
  }
  klineSessionUp = false;
  protocolOverride = 0;
}

// measuring block: [formula a b] x n after 61 <group>, decoded like KWP1281 / TP2.0
static uint8_t decodeBlock(const uint8_t *data, uint16_t len, DsgField *fields)
{
  uint8_t count = 0;
  for (uint16_t i = 0; i + 2 < len && count < 4; i += 3, count++)
  {
    fields[count].formula = data[i];
    fields[count].a = data[i + 1];
    fields[count].b = data[i + 2];
    fields[count].value = kwp1281Decode(fields[count].formula, fields[count].a, fields[count].b, fields[count].unit,
                                        sizeof(fields[count].unit));
  }
  return count;
}

static bool klineReadBlock(uint8_t group, DsgField *fields, uint8_t &count, uint8_t &nrc)
{
  char request[8];
  snprintf(request, sizeof(request), "21%02X", group);
  ElmMsg response;
  count = 0;
  nrc = 0;
  if (!elmRequest(request, response, 1500))
  {
    return false;
  }
  if (response.data[0] == 0x7F)
  {
    nrc = response.len >= 3 ? response.data[2] : 0;
    return false;
  }
  if (response.len < 2 || response.data[0] != 0x61 || response.data[1] != group)
  {
    return false;
  }
  count = decodeBlock(response.data + 2, response.len - 2, fields);
  return true;
}

// settings.elmExtraSource ELM_EXTRA_KLINE: the engine blocks the gauge's own K-line would read (klineEngineGroups), into the same signals
static void pollKlineVag()
{
  static uint32_t retryAt = 0;
  static uint8_t groupIndex = 0;
  static uint8_t fails = 0;
  if (!klineSessionUp)
  {
    if ((int32_t)(millis() - retryAt) < 0)
    {
      return;
    }
    setState("K-line init");
    char why[96];
    if (!klineConnect(settings.elmKlAddr, settings.elmKlFast, why, sizeof(why)))
    {
      setError(why);
      setState("K-line: no module");
      retryAt = millis() + 10000;
      return;
    }
    char message[48];
    snprintf(message, sizeof(message), "K-line 0x%02X up, key bytes %02X %02X", settings.elmKlAddr, klineKeyBytes[0],
             klineKeyBytes[1]);
    terminalAdd("# ", message);
    setState("K-line polling");
    fails = 0;
  }
  const uint8_t *groups;
  const uint8_t groupCount = klineEngineGroups(&groups);
  if (!groupCount)
  {
    return;
  }
  const uint8_t group = groups[groupIndex++ % groupCount];
  DsgField fields[4];
  uint8_t fieldCount = 0;
  uint8_t nrc = 0;
  if (klineReadBlock(group, fields, fieldCount, nrc))
  {
    float values[4];
    for (uint8_t i = 0; i < fieldCount; i++)
    {
      values[i] = fields[i].value;
    }
    klineApplyGroup(group, values, fieldCount);
    fails = 0;
  }
  else if (++fails >= 5)
  {
    setError("K-line: 5 block reads in a row failed");
    klineSessionUp = false; // re-init (W5: wait a little before waking it again)
    retryAt = millis() + 3000;
  }
}

static void runTerminal()
{
  char cmd[40];
  EXT_RAM_BSS_ATTR static char text[768];
  while (terminalQueue && xQueueReceive(terminalQueue, cmd, 0) == pdTRUE)
  {
    elmCommand(cmd, text, sizeof(text), 5000, true);
    rawLink.invalidate(); // it may have changed ATSH / ATCRA under the raw link
  }
}

// ---------------------------------------------------------------------------
// bench tests (0.8.0) - one sitting with a new adapter. Each writes /logs/elm_<kind>_NNN.txt with every command, its
// answer and round trip, then a RESULT line; the web UI (Bluetooth OBD > Bench tests) and tools/elm_bench.py start them
// and fetch the log. All of them only READ (no coding, no adaptation) except "clear", which clears the fault codes and
// is never part of "all". Afterwards the adapter is reset and set up again (the normal session init), so whatever a
// test changed on the adapter is gone.
//   probe - ATI / AT@1 / VTVERS / STI / ATDP / ATRV / ATCS / ATPPS, which of the commands this firmware uses (or may
//           use) the adapter accepts, and the BLE round trip (20 x ATRV)
//   raw   - raw CAN mode: OBD 01 00 sent by hand to 0x7E0, answer on 0x7E8, at 8 and at 3 bytes (ATV1 check)
//   dtc   - OBD 03 / 07 / 0A on 0x7DF, UDS 19 02 FF on the engine and on the gearbox ids, KWP2000 18 02 FF 00 over
//           TP2.0 on the module given
//   tp20  - channel open / A0 / 10 89 on the module given, then block reads at five ATST / ATAT settings, then how long
//           the channel survives idle (400 / 800 / 1500 ms with no traffic)
//   kline - VAG KWP2000 on the adapter's K-line: 5-baud and fast init to the module, key bytes, ECU id, block reads,
//           DTCs; then a KWP1281-style wake-up (ISO 9141 5-baud init) to show what such a module answers
//   all   - probe, raw, dtc (engine 0x01), tp20 on the engine (block 1) and tp20 on the DSG module / block from settings,
//           and kline (engine) when OBD was found on K-line
//   clear - (0.14.0) CLEARS the fault codes the dtc test reads: OBD 04 (every emissions ECU - also resets the readiness
//           monitors), UDS 14 FF FF FF on the engine and the gearbox ids, KWP2000 14 FF 00 over TP2.0 on the module given
//   speed - (0.24.1) read only: 15 x 010C (engine speed) with each of the adapter's timing choices (adaptive timing,
//           answer window, the "one answer" digit), the round trip and how many came back right
// ---------------------------------------------------------------------------
enum : uint8_t
{
  TEST_NONE = 0,
  TEST_ALL,
  TEST_PROBE,
  TEST_RAW,
  TEST_DTC,
  TEST_TP20,
  TEST_KLINE,
  TEST_CANMON, // 0.22.0: force CAN and ATMA-monitor the broadcast frames (read raw CAN through the adapter)
  TEST_SPEED,  // 0.24.1: OBD answer speed with each adapter timing choice
  TEST_CLEAR // the last one: elmStartTest() and runTest() check up to here
};
static const char *testNames[] = {"", "all", "probe", "raw", "dtc", "tp20", "kline", "canmon", "speed", "clear"};
static volatile uint8_t testRequested = TEST_NONE;
static uint8_t testModule = 0x01;
static uint8_t testGroup = 1;
EXT_RAM_BSS_ATTR static ElmTestStatus testStatus;
static portMUX_TYPE testStatusMux = portMUX_INITIALIZER_UNLOCKED;

static void testStep(const char *text)
{
  portENTER_CRITICAL(&testStatusMux);
  copyStr(testStatus.step, sizeof(testStatus.step), text);
  portEXIT_CRITICAL(&testStatusMux);
  testLine("%s", ""); // a blank line before each section
  testLine("==== %s", text);
}

static void tp20Trace(char direction, uint16_t, const uint8_t *, uint8_t, const char *note)
{
  if (direction == '#' && note) // the frames themselves are in the log already (elmCommand), the client adds its reasoning
  {
    testLine("# tp20: %s", note);
  }
}

static void obdDtcText(uint8_t hi, uint8_t lo, char *out, size_t len)
{
  snprintf(out, len, "%c%u%X%X%X", "PCBU"[hi >> 6], (hi >> 4) & 3, hi & 0x0F, lo >> 4, lo & 0x0F);
}

// KWP2000 18 02 FF 00 answer: 58 <count> then [hi lo status] x count; the VAG fault number is hi * 256 + lo (scan
// tools show it 5-digit). Returns how many were listed.
static uint16_t logVagDtcs(const uint8_t *response, uint16_t len)
{
  uint16_t count = 0;
  testLine("  %u fault(s)", len >= 2 ? response[1] : 0);
  for (uint16_t pos = 2; pos + 2 < len; pos += 3, count++)
  {
    testLine("  %05u  status 0x%02X", (unsigned)((response[pos] << 8) | response[pos + 1]), response[pos + 2]);
  }
  return count;
}

static void testProbe(char *result, size_t resultLen)
{
  EXT_RAM_BSS_ATTR static char text[1024];
  testStep("identity");
  const char *infoCommands[] = {"ATI", "AT@1", "VTVERS", "STI", "ATDP", "ATDPN", "ATRV", "ATCS", "ATPPS"};
  for (const char *command : infoCommands)
  {
    elmCommand(command, text, sizeof(text), 3000, false);
  }
  // VTD (added in 4.1.16) is left out on purpose: the changelog doesn't say what it does

  testStep("command support (all undone by the reset afterwards)");
  const char *checks[] = {"ATCAF0", "ATCFC0", "ATV1", "ATV0", "ATCFC1", "ATCAF1", "ATCRA7E8", "ATCF7E8", "ATCM7FF",
                          "ATCRA", "ATFCSM0", "ATCEA", "ATKW0", "ATKW1", "ATIIA33", "ATAT2", "ATAT1", "ATST19",
                          "ATST32", "ATR1", "ATD0", "ATAL"};
  uint8_t okCount = 0;
  uint8_t total = 0;
  char refused[96] = "";
  for (const char *command : checks)
  {
    const bool good = elmCommand(command, text, sizeof(text), 1000, false) && containsOk(text);
    total++;
    if (good)
    {
      okCount++;
    }
    else if (strlen(refused) + strlen(command) + 2 < sizeof(refused))
    {
      strcat(refused, " ");
      strcat(refused, command);
    }
    testLine("  %-10s %s", command, good ? "OK" : "REFUSED");
  }

  testStep("BLE round trip (20 x ATRV)");
  uint32_t minRtt = 0xFFFFFFFF;
  uint32_t maxRtt = 0;
  uint32_t sum = 0;
  uint8_t answered = 0;
  for (int i = 0; i < 20; i++)
  {
    if (elmCommand("ATRV", text, sizeof(text), 1000, false))
    {
      minRtt = lastRoundTripMs < minRtt ? lastRoundTripMs : minRtt;
      maxRtt = lastRoundTripMs > maxRtt ? lastRoundTripMs : maxRtt;
      sum += lastRoundTripMs;
      answered++;
    }
  }
  testLine("  rtt min %lu avg %lu max %lu ms (%u/20 answered), %s", (unsigned long)(answered ? minRtt : 0),
           (unsigned long)(answered ? sum / answered : 0), (unsigned long)maxRtt, answered, adapterInfo());
  snprintf(result, resultLen, "probe: %u/%u commands OK%s%s, rtt avg %lu ms", okCount, total,
           refused[0] ? ", refused:" : "", refused, (unsigned long)(answered ? sum / answered : 0));
}

static void testRaw(char *result, size_t resultLen)
{
  testStep("raw CAN mode");
  if (adapterStatus.protocol != 6 && adapterStatus.protocol != 8)
  {
    snprintf(result, resultLen, "raw: skipped (protocol %u is not 11-bit CAN)", adapterStatus.protocol);
    testLine("%s", result);
    return;
  }
  if (!rawLink.enter())
  {
    snprintf(result, resultLen, "raw: ATCAF0 refused - no raw CAN on this adapter");
    testLine("%s", result);
    return;
  }
  testLine("  ATV1 (variable DLC) %s", rawLink.varDlc ? "accepted" : "refused - frames go out padded to 8");
  rawLink.listen(0x7E8);
  bool answered[2] = {false, false};
  const uint8_t req8[8] = {0x02, 0x01, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55};
  for (int pass = 0; pass < 2; pass++)
  {
    testStep(pass == 0 ? "01 00 to 0x7E0 as 8 bytes" : "01 00 to 0x7E0 as 3 bytes (does the ECU take a short frame?)");
    rawLink.flush();
    rawLink.send(0x7E0, req8, pass == 0 ? 8 : 3);
    DiagFrame frame;
    while (rawLink.recv(frame, 0))
    {
      if (frame.id == 0x7E8 && frame.len >= 3 && frame.data[1] == 0x41 && frame.data[2] == 0x00)
      {
        answered[pass] = true;
      }
    }
    testLine("  answer on 0x7E8: %s", answered[pass] ? "yes" : "no");
  }
  rawLink.leave();
  snprintf(result, resultLen, "raw: ATV1 %s, 8-byte request %s, 3-byte request %s", rawLink.varDlc ? "ok" : "refused",
           answered[0] ? "answered" : "NOT answered", answered[1] ? "answered" : "not answered");
}

// TP2.0 open + KWP2000 10 89 on the module; false = no channel / session (logged)
static bool tp20OpenSession(uint8_t module)
{
  if (!tp20Client.open(module))
  {
    testLine("  module 0x%02X: no TP2.0 channel", module);
    return false;
  }
  testLine("  channel: we send on 0x%03X, the module on 0x%03X", tp20Client.txId(), tp20Client.rxId());
  const uint8_t sessionRequest[2] = {0x10, 0x89};
  uint8_t response[16];
  uint16_t responseLen = 0;
  const bool ok = tp20Client.transact(sessionRequest, 2, response, sizeof(response), responseLen, 1000) && responseLen &&
      response[0] == 0x50;
  const char *sessionText;
  if (ok)
  {
    sessionText = "50 (accepted)";
  }
  else if (responseLen && response[0] == 0x7F)
  {
    sessionText = "refused (7F)";
  }
  else
  {
    sessionText = "no answer";
  }
  testLine("  10 89: %s", sessionText);
  if (!ok)
  {
    tp20Client.close();
  }
  return ok;
}

static void testDtc(uint8_t module, char *result, size_t resultLen)
{
  EXT_RAM_BSS_ATTR static char text[1024];
  leaveRawMode();
  const bool can = protocolIsCan(adapterStatus.protocol);
  uint16_t obdCount = 0;
  uint16_t udsCount = 0;
  uint16_t kwpCount = 0;
  char line[160];

  testStep("OBD 03 / 07 / 0A (stored / pending / permanent), every emissions ECU");
  if (can)
  {
    elmCommand(protocolIs29Bit(adapterStatus.protocol) ? "ATSH18DB33F1" : "ATSH7DF", text, sizeof(text), 1000, false);
  }
  const char *services[] = {"03", "07", "0A"};
  for (const char *service : services)
  {
    if (!elmCommand(service, text, sizeof(text), 5000, false))
    {
      continue;
    }
    ElmMsg msgs[6];
    const uint8_t messageCount = parseAnswer(text, msgs, 6);
    const uint8_t positive = hexValue(service[0]) * 16 + hexValue(service[1]) + 0x40;
    for (uint8_t i = 0; i < messageCount; i++)
    {
      if (!msgs[i].len || msgs[i].data[0] != positive)
      {
        continue;
      }
      // CAN: [43][count][hi lo]..., K-line / J1850: [43][hi lo] x 3 (00 00 = empty slot)
      uint16_t pos = can ? 2 : 1;
      size_t lineLen = snprintf(line, sizeof(line), "  %s from %03lX:", service, (unsigned long)msgs[i].header);
      for (; pos + 1 < msgs[i].len && lineLen < sizeof(line) - 8; pos += 2)
      {
        if (!msgs[i].data[pos] && !msgs[i].data[pos + 1])
        {
          continue;
        }
        char code[8];
        obdDtcText(msgs[i].data[pos], msgs[i].data[pos + 1], code, sizeof(code));
        lineLen += snprintf(line + lineLen, sizeof(line) - lineLen, " %s", code);
        obdCount++;
      }
      testLine("%s", line);
    }
  }

  if (can)
  {
    // 0.34.0: on a 29-bit car too (it was skipped there: a 2025 Tiguan only showed its OBD codes). 11-bit: the engine
    // 0x7E0 and the DSG tab's gearbox ids; 29-bit: 18 DA <engine> F1 and 18 DA <gearbox> F1, the addresses the car
    // answered 01 00 from
    const bool is29Bit = protocolIs29Bit(adapterStatus.protocol);
    char headerCommands[2][16]; // the ATSH for each module
    char moduleNames[2][24];    // and how the log names it
    if (is29Bit)
    {
      snprintf(headerCommands[0], sizeof(headerCommands[0]), "ATSHDA%02XF1", engineAddress29Bit);
      snprintf(moduleNames[0], sizeof(moduleNames[0]), "18DA%02XF1 (engine)", engineAddress29Bit);
      snprintf(headerCommands[1], sizeof(headerCommands[1]), "ATSHDA%02XF1", gearboxAddress29Bit);
      snprintf(moduleNames[1], sizeof(moduleNames[1]), "18DA%02XF1 (gearbox)", gearboxAddress29Bit);
    }
    else
    {
      copyStr(headerCommands[0], sizeof(headerCommands[0]), "ATSH7E0");
      copyStr(moduleNames[0], sizeof(moduleNames[0]), "0x7E0 (engine)");
      snprintf(headerCommands[1], sizeof(headerCommands[1]), "ATSH%03X", settings.dsgTxId);
      snprintf(moduleNames[1], sizeof(moduleNames[1]), "0x%03X (gearbox)", settings.dsgTxId);
    }
    for (uint8_t moduleIndex = 0; moduleIndex < 2; moduleIndex++)
    {
      snprintf(line, sizeof(line), "UDS 19 02 FF on %s", moduleNames[moduleIndex]);
      testStep(line);
      elmCommand(headerCommands[moduleIndex], text, sizeof(text), 1000, false);
      ElmMsg response;
      if (!elmRequest("1902FF", response, 5000))
      {
        testLine("  no answer");
        continue;
      }
      if (response.data[0] == 0x7F)
      {
        testLine("  refused, NRC 0x%02X (0x11 = the module doesn't speak UDS, e.g. KWP2000 on PQ35)",
                 response.len >= 3 ? response.data[2] : 0);
        continue;
      }
      // 59 02 <availability mask> then [DTC hi mid][failure type][status] x n
      for (uint16_t pos = 3; pos + 3 < response.len; pos += 4)
      {
        char code[8];
        obdDtcText(response.data[pos], response.data[pos + 1], code, sizeof(code));
        testLine("  %s-%02X  status 0x%02X", code, response.data[pos + 2], response.data[pos + 3]);
        udsCount++;
      }
      if (response.len >= 64)
      {
        testLine("  (answer cut at 64 bytes - more DTCs than shown)");
      }
    }
    selectEngineHeader(false); // back to the engine (7E0, or 18 DA <engine> F1 on a 29-bit car)
  }
  if (can && !protocolIs29Bit(adapterStatus.protocol))
  {
    snprintf(line, sizeof(line), "KWP2000 18 02 FF 00 over TP2.0, module 0x%02X", module);
    testStep(line);
    if (adapterStatus.protocol == 6 && rawLink.enter())
    {
      if (tp20OpenSession(module))
      {
        const uint8_t request[4] = {0x18, 0x02, 0xFF, 0x00};
        uint8_t response[128];
        uint16_t responseLen = 0;
        if (tp20Client.transact(request, 4, response, sizeof(response), responseLen, 2000) && responseLen >= 2 &&
            response[0] == 0x58)
        {
          kwpCount += logVagDtcs(response, responseLen);
        }
        else
        {
          testLine("  %s", responseLen && response[0] == 0x7F ? "refused (7F)" : "no answer");
        }
        tp20Client.close();
      }
      leaveRawMode();
    }
    else
    {
      testLine("  skipped (needs CAN 11-bit 500k and raw mode)");
    }
  }
  if (can && protocolIs29Bit(adapterStatus.protocol))
  {
    selectEngineHeader(false); // 0.25.3: off the functional 18DB33F1 and back to the engine (only 11-bit did this)
  }
  snprintf(result, resultLen, "dtc: OBD %u, UDS %u, TP2.0 module %02X %u (read only, nothing cleared)", obdCount, udsCount,
           module, kwpCount);
}

// 0.24.1: the "speed" test. How long an OBD answer takes with each of the adapter's timing choices, on whatever
// protocol the car uses: 15 x 010C (engine speed) per choice, its round trip (command out to '>' back) and how many
// came back as a proper answer. Read only. The adapter is put back to the session's settings afterwards.
//   ATAT0/1/2  adaptive timing off / normal / aggressive: how much the adapter shortens its wait after an answer
//   ATSTxx     the longest it waits for an answer, in 4 ms steps (32 = 200 ms, the default)
//   "1" digit  "expect one answer": the adapter hands it back the moment it is in instead of waiting for others
struct SpeedChoice
{
  const char *timing;  // ATAT0 / ATAT1 / ATAT2
  const char *timeout; // ATSTxx
  bool oneAnswer;      // the "1" digit on the end of the request
};
static const SpeedChoice speedChoices[] = {
    {"ATAT1", "ATST32", false}, // the session's default (Fast OBD off)
    {"ATAT2", "ATST32", false},
    {"ATAT1", "ATST32", true},
    {"ATAT2", "ATST32", true},  // Fast OBD on
    {"ATAT0", "ATST19", true},  // a fixed 100 ms window
    {"ATAT0", "ATST0F", true},  // 60 ms
    {"ATAT0", "ATST0C", true},  // 48 ms
    {"ATAT0", "ATST0C", false}, // 48 ms without the digit: is the window what it waits for?
};
#define SPEED_TRIES 15 // requests per choice

static void testSpeed(char *result, size_t resultLen)
{
  EXT_RAM_BSS_ATTR static char text[256];
  leaveRawMode();
  const uint8_t choiceCount = sizeof(speedChoices) / sizeof(speedChoices[0]);
  int8_t bestChoice = -1;
  uint32_t bestAverage = 0xFFFFFFFF;
  uint32_t firstAverage = 0;
  for (uint8_t choice = 0; choice < choiceCount; choice++)
  {
    const SpeedChoice &settingsNow = speedChoices[choice];
    char label[48];
    snprintf(label, sizeof(label), "%s %s%s", settingsNow.timing, settingsNow.timeout,
             settingsNow.oneAnswer ? " + one-answer digit" : "");
    testStep(label);
    elmCommand(settingsNow.timing, text, sizeof(text), 1000, false);
    elmCommand(settingsNow.timeout, text, sizeof(text), 1000, false);
    const char *request = "010C";
    if (settingsNow.oneAnswer)
    {
      request = "010C1";
    }
    uint8_t good = 0;
    uint32_t total = 0;
    uint32_t fastest = 0xFFFFFFFF;
    uint32_t slowest = 0;
    for (uint8_t attempt = 0; attempt < SPEED_TRIES; attempt++)
    {
      const bool answered = elmCommand(request, text, sizeof(text), 2000, false);
      const uint32_t roundTrip = lastRoundTripMs;
      total = total + roundTrip;
      if (roundTrip < fastest)
      {
        fastest = roundTrip;
      }
      if (roundTrip > slowest)
      {
        slowest = roundTrip;
      }
      if (answered && strstr(text, "410C") != NULL) // a proper engine speed answer (headers on: "486B10410C....")
      {
        good++;
      }
      if (!adapterConnected())
      {
        break;
      }
    }
    const uint32_t average = total / SPEED_TRIES;
    if (choice == 0)
    {
      firstAverage = average;
    }
    testLine("  SPEED %s: %u/%u ok, avg %lu ms (min %lu, max %lu) = %.1f requests/s", label, good, SPEED_TRIES,
             (unsigned long)average, (unsigned long)fastest, (unsigned long)slowest,
             average ? 1000.0f / average : 0.0f);
    if (good == SPEED_TRIES && average < bestAverage)
    {
      bestAverage = average;
      bestChoice = choice;
    }
    if (!adapterConnected())
    {
      break;
    }
  }
  // back to the session's settings; pollObd() sends its own ATAT again (elmAdaptiveTiming no longer matches)
  elmCommand("ATAT1", text, sizeof(text), 1000, false);
  elmCommand("ATST32", text, sizeof(text), 1000, false);
  elmAdaptiveTiming = 0xFF;
  if (bestChoice < 0)
  {
    snprintf(result, resultLen, "speed: no choice got all %u answers back (default avg %lu ms)", SPEED_TRIES,
             (unsigned long)firstAverage);
    return;
  }
  const SpeedChoice &best = speedChoices[bestChoice];
  snprintf(result, resultLen, "speed: best %s %s%s avg %lu ms (%.1f/s), default %lu ms (%.1f/s)", best.timing,
           best.timeout, best.oneAnswer ? " +1" : "", (unsigned long)bestAverage, 1000.0f / bestAverage,
           (unsigned long)firstAverage, firstAverage ? 1000.0f / firstAverage : 0.0f);
}

// Clear DTCs (0.14.0): the same three routes as testDtc() above, clearing instead of reading.
//   OBD 04 on 0x7DF       - every emissions ECU clears its codes (and its readiness monitors, as any scan tool's clear)
//   UDS 14 FF FF FF       - engine 0x7E0 and the gearbox ids from the DSG tab, in the extended session (10 03) as in the capture
//   KWP2000 14 FF 00      - over TP2.0 to the module given (PQ35-era modules)
// Each answer is logged: 44 / 54 = cleared, 7F = refused (the NRC says why), nothing = that route isn't there.
static void testClear(uint8_t module, char *result, size_t resultLen)
{
  EXT_RAM_BSS_ATTR static char text[1024];
  leaveRawMode();
  const bool can = protocolIsCan(adapterStatus.protocol);
  char line[96];
  bool obdCleared = false;   // an emissions ECU answered 44
  uint8_t udsCleared = 0;    // how many of the engine / gearbox answered 54
  bool kwpCleared = false;   // the TP2.0 module answered 54

  // --- OBD mode 04: every emissions ECU
  testStep("OBD 04 (clear emissions codes), every emissions ECU");
  if (can)
  {
    if (protocolIs29Bit(adapterStatus.protocol))
    {
      elmCommand("ATSH18DB33F1", text, sizeof(text), 1000, false); // 29-bit functional address
    }
    else
    {
      elmCommand("ATSH7DF", text, sizeof(text), 1000, false); // 11-bit functional address (every ECU)
    }
  }
  if (elmCommand("04", text, sizeof(text), 5000, false))
  {
    ElmMsg msgs[6];
    const uint8_t messageCount = parseAnswer(text, msgs, 6);
    for (uint8_t i = 0; i < messageCount; i++)
    {
      if (msgs[i].len && msgs[i].data[0] == 0x44) // 44 = codes cleared
      {
        testLine("  cleared by %03lX", (unsigned long)msgs[i].header);
        obdCleared = true;
      }
      else if (msgs[i].len >= 3 && msgs[i].data[0] == 0x7F) // 7F 04 <NRC> = refused
      {
        testLine("  refused by %03lX, NRC 0x%02X (0x22 = conditions not correct: engine running?)",
                 (unsigned long)msgs[i].header, msgs[i].data[2]);
      }
    }
  }
  if (!obdCleared)
  {
    testLine("  no ECU confirmed the clear");
  }

  // --- UDS 14 FF FF FF: the engine and the gearbox (MQB era)
  if (can)
  {
    // 0.34.0: on a 29-bit car too, to 18 DA <engine> F1 and 18 DA <gearbox> F1 (as testDtc)
    const bool is29Bit = protocolIs29Bit(adapterStatus.protocol);
    char headerCommands[2][16]; // the ATSH for each module
    char moduleNames[2][24];    // and how the log names it
    if (is29Bit)
    {
      snprintf(headerCommands[0], sizeof(headerCommands[0]), "ATSHDA%02XF1", engineAddress29Bit);
      snprintf(moduleNames[0], sizeof(moduleNames[0]), "18DA%02XF1 (engine)", engineAddress29Bit);
      snprintf(headerCommands[1], sizeof(headerCommands[1]), "ATSHDA%02XF1", gearboxAddress29Bit);
      snprintf(moduleNames[1], sizeof(moduleNames[1]), "18DA%02XF1 (gearbox)", gearboxAddress29Bit);
    }
    else
    {
      copyStr(headerCommands[0], sizeof(headerCommands[0]), "ATSH7E0");
      copyStr(moduleNames[0], sizeof(moduleNames[0]), "0x7E0 (engine)");
      snprintf(headerCommands[1], sizeof(headerCommands[1]), "ATSH%03X", settings.dsgTxId);
      snprintf(moduleNames[1], sizeof(moduleNames[1]), "0x%03X (gearbox)", settings.dsgTxId);
    }
    for (uint8_t i = 0; i < 2; i++)
    {
      snprintf(line, sizeof(line), "UDS 14 FF FF FF on %s", moduleNames[i]);
      testStep(line);
      elmCommand(headerCommands[i], text, sizeof(text), 1000, false);
      ElmMsg response;
      elmRequest("1003", response, 2000); // extended session first, as the capture does (a refusal here is fine - try anyway)
      if (!elmRequest("14FFFFFF", response, 5000))
      {
        testLine("  no answer");
        continue;
      }
      if (response.data[0] == 0x54) // 54 = cleared
      {
        testLine("  cleared");
        udsCleared++;
      }
      else if (response.data[0] == 0x7F)
      {
        uint8_t nrc = 0;
        if (response.len >= 3)
        {
          nrc = response.data[2];
        }
        testLine("  refused, NRC 0x%02X (0x11 = no UDS here, 0x22 = conditions not correct)", nrc);
      }
      elmRequest("1001", response, 2000); // back to the default session
    }
    selectEngineHeader(false); // back to the engine
  }
  if (can && !protocolIs29Bit(adapterStatus.protocol))
  {
    // --- KWP2000 14 FF 00 over TP2.0: the module given (PQ35 era)
    snprintf(line, sizeof(line), "KWP2000 14 FF 00 over TP2.0, module 0x%02X", module);
    testStep(line);
    if (adapterStatus.protocol == 6 && rawLink.enter())
    {
      if (tp20OpenSession(module))
      {
        const uint8_t request[3] = {0x14, 0xFF, 0x00}; // ClearDiagnosticInformation, every fault group
        uint8_t response[16];
        uint16_t responseLen = 0;
        const bool answered = tp20Client.transact(request, 3, response, sizeof(response), responseLen, 3000);
        if (answered && responseLen >= 1 && response[0] == 0x54) // 54 = cleared
        {
          testLine("  cleared");
          kwpCleared = true;
        }
        else if (responseLen >= 1 && response[0] == 0x7F)
        {
          testLine("  refused (7F)");
        }
        else
        {
          testLine("  no answer");
        }
        tp20Client.close();
      }
      leaveRawMode();
    }
    else
    {
      testLine("  skipped (needs CAN 11-bit 500k and raw mode)");
    }
  }
  if (can && protocolIs29Bit(adapterStatus.protocol))
  {
    selectEngineHeader(false); // 0.25.3: off the functional 18DB33F1 and back to the engine (only 11-bit did this)
  }

  // the one-line result for the web UI
  const char *obdText = "no";
  if (obdCleared)
  {
    obdText = "cleared";
  }
  const char *kwpText = "no";
  if (kwpCleared)
  {
    kwpText = "cleared";
  }
  snprintf(result, resultLen, "clear: OBD %s, UDS %u cleared, TP2.0 module %02X %s - read DTCs again to check", obdText,
           udsCleared, module, kwpText);
}

static void testTp20(uint8_t module, uint8_t group, char *result, size_t resultLen)
{
  static char text[64];
  char line[96];
  snprintf(line, sizeof(line), "TP2.0 module 0x%02X, block %u", module, group);
  testStep(line);
  if (adapterStatus.protocol != 6)
  {
    snprintf(result, resultLen, "tp20 %02X: skipped (protocol %u, needs CAN 11-bit 500k)", module, adapterStatus.protocol);
    testLine("%s", result);
    return;
  }
  if (!rawLink.enter())
  {
    snprintf(result, resultLen, "tp20 %02X: ATCAF0 refused", module);
    testLine("%s", result);
    return;
  }
  elmCommand("ATST32", text, sizeof(text), 1000, false); // start slow and safe: 200 ms answer window, no adaptive timing
  elmCommand("ATAT0", text, sizeof(text), 1000, false);
  tp20Client.stats = {};
  if (!tp20OpenSession(module))
  {
    snprintf(result, resultLen, "tp20 %02X: no channel / session (an MQB car, or the module isn't fitted)", module);
    leaveRawMode();
    return;
  }
  // the sweep: how short can the adapter's answer window be before the ECU's frames / ACK timing suffers?
  struct
  {
    uint8_t timeoutSetting, adaptiveTiming;
  } sweep[] = {{0x32, 0}, {0x19, 0}, {0x0C, 0}, {0x19, 1}, {0x19, 2}};
  uint8_t bestOk = 0;
  uint8_t bestTimeout = 0;
  uint8_t bestAdaptive = 0;
  uint32_t bestAvg = 0;
  bool shownFields = false;
  for (auto &step : sweep)
  {
    snprintf(line, sizeof(line), "block %u x 6 at ATST%02X (%u ms) ATAT%u", group, step.timeoutSetting,
             step.timeoutSetting * 4, step.adaptiveTiming);
    testStep(line);
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "ATST%02X", step.timeoutSetting);
    elmCommand(cmd, text, sizeof(text), 1000, false);
    snprintf(cmd, sizeof(cmd), "ATAT%u", step.adaptiveTiming);
    elmCommand(cmd, text, sizeof(text), 1000, false);
    if (!tp20Client.isOpen() && !tp20OpenSession(module))
    {
      break;
    }
    tp20Client.keepAlive();
    const uint32_t repeatsBefore = tp20Client.stats.repeats;
    uint8_t okCount = 0;
    uint32_t sum = 0;
    for (int i = 0; i < 6 && tp20Client.isOpen(); i++)
    {
      DsgField fields[4];
      uint8_t fieldCount = 0;
      uint8_t nrc = 0;
      const uint32_t startMs = millis();
      const bool good = tp20Client.readGroup(group, fields, fieldCount, nrc);
      const uint32_t elapsedMs = millis() - startMs;
      char nrcTxt[16] = "";
      if (nrc)
      {
        snprintf(nrcTxt, sizeof(nrcTxt), ", NRC 0x%02X", nrc);
      }
      testLine("  read %d: %s, %lu ms%s", i + 1, good ? "ok" : "FAILED", (unsigned long)elapsedMs, nrcTxt);
      if (!good)
      {
        continue;
      }
      okCount++;
      sum += elapsedMs;
      if (!shownFields)
      {
        shownFields = true;
        for (uint8_t fieldIndex = 0; fieldIndex < fieldCount; fieldIndex++)
        {
          testLine("    field %u: formula 0x%02X a %u b %u = %.2f %s", fieldIndex + 1, fields[fieldIndex].formula,
                   fields[fieldIndex].a, fields[fieldIndex].b, fields[fieldIndex].value, fields[fieldIndex].unit);
        }
      }
    }
    const uint32_t avg = okCount ? sum / okCount : 0;
    testLine("  SWEEP ATST%02X ATAT%u: %u/6 ok, avg %lu ms, %lu repeat(s)%s", step.timeoutSetting, step.adaptiveTiming,
             okCount, (unsigned long)avg, (unsigned long)(tp20Client.stats.repeats - repeatsBefore),
             tp20Client.isOpen() ? "" : ", channel lost");
    if (okCount > bestOk || (okCount == bestOk && okCount && avg < bestAvg))
    {
      bestOk = okCount;
      bestAvg = avg;
      bestTimeout = step.timeoutSetting;
      bestAdaptive = step.adaptiveTiming;
    }
  }
  // idle: how long may the channel go without traffic (e.g. while OBD PIDs are polled between gearbox reads)?
  uint16_t idleOk = 0;
  if (bestOk)
  {
    snprintf(line, sizeof(line), "ATST%02X", bestTimeout);
    elmCommand(line, text, sizeof(text), 1000, false);
    snprintf(line, sizeof(line), "ATAT%u", bestAdaptive);
    elmCommand(line, text, sizeof(text), 1000, false);
    const uint16_t idles[] = {400, 800, 1500};
    for (uint16_t idle : idles)
    {
      snprintf(line, sizeof(line), "idle %u ms, then a block read", idle);
      testStep(line);
      if (!tp20Client.isOpen() && !tp20OpenSession(module))
      {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(idle));
      DsgField fields[4];
      uint8_t fieldCount = 0;
      uint8_t nrc = 0;
      const bool good = tp20Client.readGroup(group, fields, fieldCount, nrc);
      testLine("  after %u ms idle: %s", idle, good ? "channel alive" : "FAILED / channel gone");
      if (!good)
      {
        break;
      }
      idleOk = idle;
    }
  }
  tp20Client.close();
  leaveRawMode();
  testLine("  totals: %lu frames sent, %lu received, %lu repeats, %lu channel tests, %lu ACKs",
           (unsigned long)tp20Client.stats.sent, (unsigned long)tp20Client.stats.received,
           (unsigned long)tp20Client.stats.repeats, (unsigned long)tp20Client.stats.channelTests,
           (unsigned long)tp20Client.stats.acks);
  if (bestOk)
  {
    snprintf(result, resultLen, "tp20 %02X: ch %03X/%03X, best ATST%02X ATAT%u %u/6 avg %lu ms, idle ok to %u ms", module,
             tp20Client.txId(), tp20Client.rxId(), bestTimeout, bestAdaptive, bestOk, (unsigned long)bestAvg, idleOk);
  }
  else
  {
    snprintf(result, resultLen, "tp20 %02X: channel opened but no block %u read worked", module, group);
  }
}

// K-line through the adapter: VAG KWP2000 with a 5-baud and a fast init to the module (key bytes, ECU id 1A 9B, a few
// block reads with timing, DTCs 18 02 FF 00, StopCommunication), then a KWP1281-style wake-up (ISO 9141 5-baud init,
// ATSP3) to show what such a module answers - the adapter can wake it but not run the block protocol.
static void testKline(uint8_t module, uint8_t group, char *result, size_t resultLen)
{
  EXT_RAM_BSS_ATTR static char text[512];
  char line[112];
  char why[96];
  bool sessionUp[2] = {false, false};
  uint8_t blocksOk[2] = {0, 0};
  uint16_t dtcs = 0;
  // OBD polling (ISO 9141-2) ran right up to this test, and an ISO 9141-2 ECU holds its session ~5 s after the last
  // request (there is no StopCommunication), ignoring any fresh 5-baud wake-up until then - so the first init would
  // always get BUS INIT ERROR. Quiet the K-line first, as the gauge's own K-line path does before a wake-up (bench
  // 0.22.0: all three inits to 0x01 / 0x11 failed to sync because the OBD session was still open).
  testStep("quiet the K-line for 5.2 s (let any ISO 9141-2 OBD session lapse)");
  vTaskDelay(pdMS_TO_TICKS(5200));
  for (int fast = 0; fast < 2; fast++)
  {
    snprintf(line, sizeof(line), "KWP2000 %s init to module 0x%02X", fast ? "fast" : "5-baud", module);
    testStep(line);
    sessionUp[fast] = klineConnect(module, fast, why, sizeof(why));
    testLine("  key bytes %02X %02X", klineKeyBytes[0], klineKeyBytes[1]);
    if (!sessionUp[fast])
    {
      testLine("  %s", why);
      protocolOverride = 0;
      continue;
    }
    ElmMsg response;
    if (elmRequest("1A9B", response, 2000) && response.data[0] == 0x5A)
    {
      char ecuId[64];
      uint8_t idLen = 0;
      for (uint16_t i = 2; i < response.len && idLen < sizeof(ecuId) - 1; i++)
      {
        ecuId[idLen++] = response.data[i] >= 0x20 && response.data[i] < 0x7F ? (char)response.data[i] : '.';
      }
      ecuId[idLen] = '\0';
      testLine("  ECU id: %s", ecuId);
    }
    else
    {
      testLine("  ECU id (1A 9B): no answer / refused");
    }
    for (int i = 0; i < 3; i++)
    {
      DsgField fields[4];
      uint8_t fieldCount = 0;
      uint8_t nrc = 0;
      const uint32_t startMs = millis();
      const bool ok = klineReadBlock(group, fields, fieldCount, nrc);
      testLine("  block %u read %d: %s, %lu ms", group, i + 1, ok ? "ok" : "FAILED", (unsigned long)(millis() - startMs));
      if (nrc)
      {
        testLine("    NRC 0x%02X", nrc);
      }
      if (!ok)
      {
        continue;
      }
      blocksOk[fast]++;
      if (i == 0)
      {
        for (uint8_t fieldIndex = 0; fieldIndex < fieldCount; fieldIndex++)
        {
          testLine("    field %u: formula 0x%02X a %u b %u = %.2f %s", fieldIndex + 1, fields[fieldIndex].formula,
                   fields[fieldIndex].a, fields[fieldIndex].b, fields[fieldIndex].value, fields[fieldIndex].unit);
        }
      }
    }
    if (elmRequest("1802FF00", response, 3000) && response.data[0] == 0x58)
    {
      dtcs = logVagDtcs(response.data, response.len);
    }
    else
    {
      testLine("  DTCs (18 02 FF 00): no answer / refused");
    }
    klineClose();
    vTaskDelay(pdMS_TO_TICKS(3000)); // let the module drop back to idle before the next wake-up
  }

  testStep("KWP1281-style wake-up: ISO 9141 5-baud init (ATSP3)");
  elmCommand("ATSP3", text, sizeof(text), 1000, false);
  snprintf(line, sizeof(line), "ATIIA%02X", module);
  elmCommand(line, text, sizeof(text), 1000, false);
  elmCommand("ATKW0", text, sizeof(text), 1000, false);
  klineKeyBytes[0] = 0;
  klineKeyBytes[1] = 0;
  elmCommand("ATSI", text, sizeof(text), 8000, false);
  const bool woke = strstr(text, "OK") && !strstr(text, "ERROR");
  if (elmCommand("ATKW", text, sizeof(text), 1000, false))
  {
    klineParseKeyBytes(text);
  }
  const bool is1281 = klineKeyBytes[0] == 0x01 && klineKeyBytes[1] == 0x8A;
  testLine("  init %s, key bytes %02X %02X%s", woke ? "OK" : "failed", klineKeyBytes[0], klineKeyBytes[1],
           is1281 ? " = KWP1281: the module woke up, but the adapter can't echo each byte inverted, so the block"
                    " protocol can't run - the gauge's own K-line does it" :
                    "");
  protocolOverride = 0;
  const char *wakeText;
  if (is1281)
  {
    wakeText = "= KWP1281 module (gauge K-line only)";
  }
  else if (woke)
  {
    wakeText = "answered (not KWP1281 key bytes)";
  }
  else
  {
    wakeText = "no answer";
  }

  // Auto-init attempt (0.22.0): some adapters fail the explicit ATFI / ATSI to a VAG address but still auto-init on
  // the first request - which is how OBD connects to 0x33. Try ISO 14230 fast init by setting the protocol / header
  // and just sending a request (ReadEcuId 1A 9B), letting the adapter do the init itself. If this works where the
  // explicit init didn't, klineConnect() can be switched to it so VAG blocks run over the adapter's K-line.
  testStep("KWP2000 auto-init: fast init on the first request (no explicit ATFI)");
  elmCommand("ATSP5", text, sizeof(text), 1000, false);
  elmCommand("ATKW0", text, sizeof(text), 1000, false);
  snprintf(line, sizeof(line), "ATSH80%02XF1", module);
  elmCommand(line, text, sizeof(text), 1000, false);
  snprintf(line, sizeof(line), "ATWM81%02XF13E", module);
  elmCommand(line, text, sizeof(text), 1000, false);
  protocolOverride = 5;
  ElmMsg autoResp;
  const bool autoGot = elmRequest("1A9B", autoResp, 8000) && autoResp.len > 0;
  const bool autoInit = autoGot && (autoResp.data[0] == 0x5A || autoResp.data[0] == 0x7F);
  if (autoGot && autoResp.data[0] == 0x5A)
  {
    char ecuId[48];
    uint8_t idLen = 0;
    for (uint16_t i = 2; i < autoResp.len && idLen < sizeof(ecuId) - 1; i++)
    {
      ecuId[idLen++] = autoResp.data[i] >= 0x20 && autoResp.data[i] < 0x7F ? (char)autoResp.data[i] : '.';
    }
    ecuId[idLen] = '\0';
    testLine("  auto-init WORKED: ECU id %s", ecuId);
  }
  else if (autoInit)
  {
    testLine("  auto-init reached the ECU (7F refusal to 1A 9B) - the init itself works");
  }
  else
  {
    testLine("  auto-init: no answer (this adapter can't init a VAG module address at all)");
  }
  protocolOverride = 0;

  snprintf(result, resultLen,
           "kline %02X: KWP2000 5-baud %s (%u/3 blocks), fast %s (%u/3), %u DTC(s); KWP1281 wake-up %s; auto-init %s", module,
           sessionUp[0] ? "up" : "no", blocksOk[0], sessionUp[1] ? "up" : "no", blocksOk[1], dtcs, wakeText,
           autoInit ? "reached the ECU" : "no answer");
}

// CAN broadcast source (0.22.0, settings.elmExtraSource ELM_EXTRA_CAN_MONITOR): hold the adapter in ATMA monitor and inject every CAN frame
// it reads into the gauge's CAN pipeline (canInject), exactly like a frame relayed from an OpenHaldex / adc2can - so
// gauge pages reading a DBC / profile signal get the car's CAN wirelessly through the adapter, even on a car that
// broadcasts on CAN but answers no OBD-on-CAN (a PQ34 ME7). The CAN decoding (profile or uploaded DBC) is the gauge's
// existing one; this only feeds it frames.
#define ELM_CAN_SOURCE CAN_AIR_SOURCES // canInject source id for adapter-monitored frames (the top air slot)

// stop ATMA (any byte stops it; NOT a bare CR - this clone repeats the last command on one) and drain to the prompt
static void canMonStop()
{
  if (!canMonArmed)
  {
    return;
  }
  const uint8_t space[1] = {' '};
  adapterWrite(space, sizeof(space));
  uint8_t buf[64];
  adapterRead(buf, sizeof(buf), 150);
  canMonArmed = false;
}

static void pollCanMonitor()
{
  static char line[40];
  static size_t lineLen = 0;
  static uint32_t lastFrameMs = 0;
  leaveRawMode();
  // if ATMA has gone quiet (the clone stops streaming on a noisy / marginal bus without always printing the prompt),
  // stop and re-arm it so the monitor recovers instead of sitting dead
  if (canMonArmed && (uint32_t)(millis() - lastFrameMs) > 1500)
  {
    canMonStop();
  }
  if (!canMonArmed)
  {
    // set the adapter to 11-bit 500k CAN, raw frames, headers on, no receive filter, and start monitoring everything
    static char text[64];
    elmCommand("ATSP6", text, sizeof(text), 1000, false);  // ISO 15765-4 CAN 11-bit / 500k - the VAG powertrain bus
    elmCommand("ATCAF0", text, sizeof(text), 1000, false); // raw frames, no ISO-TP reassembly
    elmCommand("ATH1", text, sizeof(text), 1000, false);   // show the CAN id on every line
    elmCommand("ATS0", text, sizeof(text), 1000, false);   // no spaces
    elmCommand("ATCRA", text, sizeof(text), 1000, false);  // clear any receive filter - hear every id
    protocolOverride = 6;
    adapterStatus.protocol = 6;
    adapterFlush();
    const uint8_t startCmd[5] = {'A', 'T', 'M', 'A', '\r'};
    adapterWrite(startCmd, sizeof(startCmd));
    lineLen = 0;
    canMonArmed = true;
    lastFrameMs = millis();
    setState("CAN monitor");
  }
  // drain whatever has streamed in since the last call and inject each complete frame line
  uint8_t buf[128];
  const size_t got = adapterRead(buf, sizeof(buf), 100);
  for (size_t i = 0; i < got; i++)
  {
    const char character = (char)buf[i];
    if (character == '\r' || character == '\n' || character == '>')
    {
      if (character == '>')
      {
        canMonArmed = false; // ATMA stopped by itself (adapter reset / error): re-arm on the next call
      }
      line[lineLen] = '\0';
      if (lineLen >= 5 && ((lineLen - 3) % 2) == 0 && lineLen <= 3 + 16)
      {
        bool allHex = true;
        for (size_t j = 0; j < lineLen && allHex; j++)
        {
          allHex = isHex(line[j]);
        }
        if (allHex)
        {
          twai_message_t frame = {};
          frame.identifier = (hexValue(line[0]) << 8) | (hexValue(line[1]) << 4) | hexValue(line[2]);
          frame.extd = 0;
          frame.data_length_code = (lineLen - 3) / 2;
          for (uint8_t b = 0; b < frame.data_length_code && b < 8; b++)
          {
            frame.data[b] = (hexValue(line[3 + b * 2]) << 4) | hexValue(line[4 + b * 2]);
          }
          canInject(frame, ELM_CAN_SOURCE); // into the same pipeline as a relayed frame: DBC / profile + the CAN tab
          adapterStatus.requests++;          // so the Requests/s tile shows the frame rate
          lastFrameMs = millis();
        }
      }
      lineLen = 0;
    }
    else if (character != ' ' && lineLen < sizeof(line) - 1)
    {
      line[lineLen++] = character;
    }
  }
}

// CAN broadcast monitor (0.22.0): force the adapter to 11-bit 500k CAN and ATMA-monitor the bus for a few seconds, to
// read a car's broadcast frames through the adapter even when it has no OBD-on-CAN responder (a PQ34 ME7 broadcasts on
// CAN but answers diagnostics only on K-line). Also the quickest check that the OBD port's CAN (pins 6 / 14) is the
// right way round: frames = right, nothing = reversed, no 120R termination, or the ECU isn't transmitting.
static void testCanMonitor(char *result, size_t resultLen)
{
  static char text[128];
  static struct
  {
    uint16_t id;
    uint8_t len;
    uint8_t data[8];
    uint32_t count;
  } seen[16];
  uint8_t seenCount = 0;
  uint32_t frames = 0;
  leaveRawMode();
  testStep("force CAN 11-bit 500k, monitor all frames (ATMA) for 3 s");
  elmCommand("ATSP6", text, sizeof(text), 1000, false);  // ISO 15765-4 CAN 11-bit / 500k - the VAG powertrain bus
  elmCommand("ATCAF0", text, sizeof(text), 1000, false); // raw frames, no ISO-TP reassembly
  elmCommand("ATH1", text, sizeof(text), 1000, false);   // show the CAN id on every line
  elmCommand("ATS0", text, sizeof(text), 1000, false);   // no spaces in the output
  elmCommand("ATCRA", text, sizeof(text), 1000, false);  // clear any receive filter - hear every id
  protocolOverride = 6;

  // ATMA streams frames and never prints the '>' prompt until a byte is sent, so it can't go through elmCommand:
  // drive it by hand - start it, read lines for the window, then send one byte to stop it and drain to the prompt.
  adapterFlush();
  const uint8_t startCmd[5] = {'A', 'T', 'M', 'A', '\r'};
  adapterWrite(startCmd, sizeof(startCmd));
  char lineBuf[40];
  size_t lineLen = 0;
  const uint32_t deadline = millis() + 3000;
  while ((int32_t)(deadline - millis()) > 0)
  {
    uint8_t buf[64];
    const size_t got = adapterRead(buf, sizeof(buf), 50);
    for (size_t i = 0; i < got; i++)
    {
      const char character = (char)buf[i];
      if (character == '\r' || character == '\n' || character == '>')
      {
        // end of a line: parse it as [3 hex id][data byte pairs] (spaces are already off)
        lineBuf[lineLen] = '\0';
        if (lineLen >= 5 && ((lineLen - 3) % 2) == 0 && lineLen <= 3 + 16)
        {
          bool allHex = true;
          for (size_t j = 0; j < lineLen && allHex; j++)
          {
            allHex = isHex(lineBuf[j]);
          }
          if (allHex)
          {
            frames++;
            const uint16_t id = (hexValue(lineBuf[0]) << 8) | (hexValue(lineBuf[1]) << 4) | hexValue(lineBuf[2]);
            int slot = -1;
            for (uint8_t s = 0; s < seenCount; s++)
            {
              if (seen[s].id == id)
              {
                slot = s;
                break;
              }
            }
            if (slot < 0 && seenCount < 16)
            {
              slot = seenCount++;
              seen[slot].id = id;
              seen[slot].count = 0;
            }
            if (slot >= 0)
            {
              seen[slot].count++;
              seen[slot].len = (lineLen - 3) / 2;
              for (uint8_t b = 0; b < seen[slot].len; b++)
              {
                seen[slot].data[b] = (hexValue(lineBuf[3 + b * 2]) << 4) | hexValue(lineBuf[4 + b * 2]);
              }
            }
          }
        }
        lineLen = 0;
      }
      else if (character != ' ' && lineLen < sizeof(lineBuf) - 1)
      {
        lineBuf[lineLen++] = character;
      }
    }
  }
  // stop ATMA (any byte stops it) with a space, NOT a bare CR - this clone repeats the last command on a bare CR,
  // which would restart ATMA. Then drain whatever is left up to the prompt.
  const uint8_t stopCmd[1] = {' '};
  adapterWrite(stopCmd, sizeof(stopCmd));
  const uint32_t drainDeadline = millis() + 600;
  while ((int32_t)(drainDeadline - millis()) > 0)
  {
    uint8_t buf[64];
    if (adapterRead(buf, sizeof(buf), 50) == 0)
    {
      break;
    }
  }
  protocolOverride = 0;

  testLine("  %lu frames, %u distinct id(s)", (unsigned long)frames, seenCount);
  for (uint8_t s = 0; s < seenCount; s++)
  {
    char bytes[20] = "";
    for (uint8_t b = 0; b < seen[s].len; b++)
    {
      snprintf(bytes + b * 2, 3, "%02X", seen[s].data[b]);
    }
    testLine("  %03X  x%-5lu %s", seen[s].id, (unsigned long)seen[s].count, bytes);
  }
  if (frames == 0)
  {
    snprintf(result, resultLen,
             "canmon: no frames - CAN pins 6/14 reversed, no 120R termination, or the ECU isn't transmitting");
  }
  else
  {
    snprintf(result, resultLen, "canmon: %lu frames, %u ids - the adapter reads this CAN bus", (unsigned long)frames,
             seenCount);
  }
}

static bool openTestLog(const char *kind)
{
  if (!LittleFS.exists("/logs"))
  {
    LittleFS.mkdir("/logs");
  }
  char path[40];
  for (int number = 1; number < 1000; number++)
  {
    snprintf(path, sizeof(path), "/logs/elm_%s_%03d.txt", kind, number);
    if (!LittleFS.exists(path))
    {
      break;
    }
  }
  testLog = LittleFS.open(path, "w");
  if (!testLog)
  {
    return false;
  }
  testStartMs = millis();
  portENTER_CRITICAL(&testStatusMux);
  copyStr(testStatus.file, sizeof(testStatus.file), path);
  portEXIT_CRITICAL(&testStatusMux);
  testLog.printf("can2gauge %s ELM bench test '%s'\n", CAN2GAUGE_VERSION, kind);
  testLog.printf("adapter: %s%s\nlink: %s\nprotocol: %u (%s)\nvehicle: %s %s %s\n", adapterStatus.adapter,
                 adapterStatus.stn ? " (STN)" : "", adapterInfo(), adapterStatus.protocol,
                 elmProtocolName(adapterStatus.protocol), adapterStatus.vin, adapterStatus.make, adapterStatus.model);
  testLog.printf("dsg: tx %03X rx %03X, tp20 module %02X block %u\n", settings.dsgTxId, settings.dsgRxId,
                 settings.dsgTp20Module, settings.dsgTp20Group);
  testLog.printf("columns: ms since start, '>' command [round trip], answer lines indented, '#' client notes\n");
  return true;
}

static void runTest()
{
  const uint8_t kind = testRequested;
  testRequested = TEST_NONE;
  if (kind == TEST_NONE || kind > TEST_CLEAR)
  {
    return;
  }
  portENTER_CRITICAL(&testStatusMux);
  testStatus.running = true;
  copyStr(testStatus.kind, sizeof(testStatus.kind), testNames[kind]);
  testStatus.result[0] = '\0';
  testStatus.step[0] = '\0';
  portEXIT_CRITICAL(&testStatusMux);
  setState("bench test");
  terminalAdd("# test ", testNames[kind]);
  // a gearbox channel from DSG polling would collide with the test's: close it
  if (tp20Client.isOpen())
  {
    rawLink.active() ? tp20Client.close() : tp20Client.drop();
  }
  leaveRawMode();
  if (klineSessionUp)
  {
    klineClose(); // a K-line session from elmExtraSource: the tests set up their own
  }
  char result[ELM_TEST_RESULT_LEN] = "";
  if (!openTestLog(testNames[kind]))
  {
    copyStr(result, sizeof(result), "could not create the log file (LittleFS full?)");
  }
  else
  {
    tp20Client.trace = tp20Trace;
    char testResult[160];
    size_t resultLen = 0;
    auto add = [&](const char *text)
    {
      testLine("RESULT %s", text);
      if (resultLen + 4 >= sizeof(result))
      {
        return; // full (the log has every RESULT line anyway)
      }
      resultLen += snprintf(result + resultLen, sizeof(result) - resultLen, "%s%s", resultLen ? " | " : "", text);
      if (resultLen >= sizeof(result))
      {
        resultLen = sizeof(result) - 1;
      }
    };
    if (kind == TEST_ALL || kind == TEST_PROBE)
    {
      testProbe(testResult, sizeof(testResult));
      add(testResult);
    }
    if (kind == TEST_ALL || kind == TEST_RAW)
    {
      testRaw(testResult, sizeof(testResult));
      add(testResult);
    }
    if (kind == TEST_ALL || kind == TEST_DTC)
    {
      testDtc(kind == TEST_ALL ? 0x01 : testModule, testResult, sizeof(testResult));
      add(testResult);
    }
    if (kind == TEST_TP20)
    {
      testTp20(testModule, testGroup, testResult, sizeof(testResult));
      add(testResult);
    }
    if (kind == TEST_KLINE)
    {
      testKline(testModule, testGroup, testResult, sizeof(testResult));
      add(testResult);
    }
    if (kind == TEST_SPEED)
    {
      testSpeed(testResult, sizeof(testResult));
      add(testResult);
    }
    if (kind == TEST_CANMON)
    {
      testCanMonitor(testResult, sizeof(testResult));
      add(testResult);
    }
    if (kind == TEST_CLEAR) // only ever on its own - never part of "all"
    {
      testClear(testModule, testResult, sizeof(testResult));
      add(testResult);
    }
    if (kind == TEST_ALL)
    {
      testTp20(0x01, 1, testResult, sizeof(testResult)); // engine, block 1 - answers on any PQ35 with the ignition on
      add(testResult);
      if (settings.dsgTp20Module != 0x01)
      {
        testTp20(settings.dsgTp20Module, settings.dsgTp20Group, testResult, sizeof(testResult));
        add(testResult);
      }
      // K-line only on a K-line car (OBD found on ISO 9141 / KWP2000): elsewhere it would just wait out three inits
      if (adapterStatus.protocol >= 3 && adapterStatus.protocol <= 5)
      {
        testKline(0x01, 1, testResult, sizeof(testResult));
        add(testResult);
      }
      else
      {
        add("kline: skipped (OBD is on CAN here) - run 'kline' on its own to try the K-line anyway");
      }
      testCanMonitor(testResult, sizeof(testResult)); // last: it forces CAN, undone by the reset after the test
      add(testResult);
    }
    tp20Client.trace = nullptr;
    testLine("%s", ""); // a blank line before the summary
    testLine("==== summary");
    testLine("%s", result);
    testLog.close();
  }
  portENTER_CRITICAL(&testStatusMux);
  copyStr(testStatus.result, sizeof(testStatus.result), result);
  copyStr(testStatus.step, sizeof(testStatus.step), "done");
  testStatus.running = false;
  testStatus.runs++;
  portEXIT_CRITICAL(&testStatusMux);
  terminalAdd("# test done: ", testStatus.file);
  // reset the adapter and set it up again, so nothing a test changed survives. 0.24.1: not after reading / clearing
  // codes or the speed test on a K-line car - those only send OBD requests (the speed test puts its ATAT / ATST
  // back), and the reset made the adapter wake the ECU again while it still held the session (up to 5 s): UNABLE TO
  // CONNECT, so the gauge dropped the Bluetooth link and started over (bench: every Read DTCs did it)
  const bool kLine = adapterStatus.protocol >= 3 && adapterStatus.protocol <= 5; // ISO 9141-2, KWP2000 slow / fast
  const bool onlyObdRequests = kind == TEST_DTC || kind == TEST_CLEAR || kind == TEST_SPEED;
  if (!(kLine && onlyObdRequests))
  {
    reidentifyRequested = true; // the re-identify sets "polling" again once the adapter is set up
  }
  else
  {
    setState("polling"); // straight back to polling: runTest() set "bench test" at the start
  }
}

static bool modeWanted() // menu > Bluetooth OBD is showing (stamped by its screen, released MODE_RELEASE_MS after)
{
  const uint32_t seen = runtime.elmModeSeenMs;
  return seen && millis() - seen < MODE_RELEASE_MS;
}

// 0.15.0: a page in the rotation reads an OBD value and only the adapter can give it: keep the adapter connected and
// polling from power-up (like Auto-connect), so the value is already there when that page is swiped to, rather than
// connecting only once it shows. Not when OBD-II comes over the wire (CAN tab), and not when no adapter is set up
// (a Bluetooth one paired, or a WiFi one's network saved) - it must not start scanning just for a page.
static bool rotationWanted()
{
  if (settings.canProfile == CAN_PROFILE_OBD2 || settings.obdOverCan)
  {
    return false; // the wired OBD polling feeds the page
  }
  if (settings.elmTransport == ELM_TR_WIFI && !settings.elmWifiSsid[0])
  {
    return false; // no WiFi adapter saved
  }
  if (settings.elmTransport != ELM_TR_WIFI && !settings.elmMac[0])
  {
    return false; // no Bluetooth adapter paired
  }
  return userSigRotationUses(USRC_OBD);
}

// 0.34.0: the Diagnostics tab or a Service job runs through the adapter (only once one is paired / saved: it must not start
// scanning for adapters just for that)
static bool diagWanted()
{
  if (settings.elmTransport == ELM_TR_WIFI && !settings.elmWifiSsid[0])
  {
    return false; // no WiFi adapter saved
  }
  if (settings.elmTransport != ELM_TR_WIFI && !settings.elmMac[0])
  {
    return false; // no Bluetooth adapter paired
  }
  return vdiagWantsAdapter();
}

// in standby (ignition off on permanent power) the adapter is let go: its requests would keep the car awake
static bool elmActive()
{
  return (settings.elmEnabled || modeWanted() || rotationWanted() || diagWanted()) && !runtime.standby;
}

static bool keepRunning(uint32_t myGeneration)
{
  return generation == myGeneration && elmActive() && linkIsMaster() && adapterConnected() && !scanRequested && !dropRequested;
}

static void scanForAdapters()
{
  scanning = true;
  setState("scanning");
  BleFound results[12];
  const uint8_t count = bleScan(5000, results, 12);
  portENTER_CRITICAL(&adapterStatusMux);
  memcpy(foundAdapters, results, sizeof(results));
  foundAdapterCount = count;
  portEXIT_CRITICAL(&adapterStatusMux);
  char message[80];
  if (count == 0 && bleStartError()[0] != '\0')
  {
    snprintf(message, sizeof(message), "scan: Bluetooth %s", bleStartError()); // didn't start: say why, not "0 devices"
  }
  else
  {
    snprintf(message, sizeof(message), "scan: %u device(s)", count);
  }
  terminalAdd("# ", message);
  // 0.24.12: a scan with the OBD adapter switched off (First Boot's check, or looking before pairing): Bluetooth stops
  // again and its internal RAM goes back to WiFi and the web UI. Pairing starts it again.
  // 0.30.0: not while the nav link is on: it needs Bluetooth all the time for the phone
  if (!settings.elmEnabled && !navWanted())
  {
    bleShutdown();
  }
  scanning = false;
}

// WiFi: connect attempts back off 30 s -> 60 s -> 2 min. Each attempt can move the radio (and so the gauge's AP, which
// carries the web UI) to the adapter's channel; an adapter that's off must not make the AP hop every few seconds.
static uint32_t wifiBackoffMs = 0;
// 0.24.21: Bluetooth connects missed in a row. Each try uses the radio for up to 5 s, which WiFi shares: after
// BLE_MISSES_BEFORE_SLOW misses (adapter unplugged / out of range) only try every BLE_SLOW_RETRY_MS.
#define BLE_MISSES_BEFORE_SLOW 3  // tries at the normal 5 s gap first (an adapter waking with the ignition)
#define BLE_SLOW_RETRY_MS 30000   // then one try every 30 s
static uint8_t bleMissedConnects = 0;
// 0.37.1: an adapter that isn't there stops being looked for. After ELM_TRIES_BEFORE_STOP misses in a row (no adapter in
// a scan, a Bluetooth connect that failed, a WiFi join that failed) the gauge stops trying ("not found - stopped") until
// the settings change (pairing, Forget, the adapter switch), Scan is pressed, the ignition goes off and on, or the
// OBD screen is opened on the gauge.
#define ELM_TRIES_BEFORE_STOP 5    // misses in a row (Bluetooth: 3 at 5 s + 2 at 30 s, about a minute and a quarter)
static uint8_t adapterMisses = 0;  // misses in a row
static bool adapterGivenUp = false; // stopped looking
static bool wasModeWanted = false; // the OBD screen was open last time round (to see it being opened)

// one more miss; true = that was the last one: stop looking
static bool adapterMissed()
{
  if (adapterMisses < 255)
  {
    adapterMisses++;
  }
  if (adapterMisses >= ELM_TRIES_BEFORE_STOP)
  {
    adapterGivenUp = true;
    setState("not found - stopped");
    DEBUG("ELM: no adapter after %u tries - stopped (Scan, a settings change or ignition off / on tries again)",
          (unsigned)adapterMisses);
    return true;
  }
  return false;
}

// a fresh start: look for the adapter again
static void adapterTryAgain()
{
  adapterMisses = 0;
  adapterGivenUp = false;
}

static void elmTaskFn(void *)
{
  uint32_t nextTryMs = 0;
  for (;;)
  {
    const uint32_t myGeneration = generation;
    connectionUp = adapterConnected(); // for the other tasks (web UI, DSG, display): they don't touch the socket / BLE link
    // a test that was requested or is running can't survive the adapter going away: cancel it here so a dropped
    // link never leaves testStatus.running stuck true (which would block every later elmStartTest) (bench 0.22.0)
    if (!connectionUp && (testRequested || testStatus.running))
    {
      testRequested = TEST_NONE;
      portENTER_CRITICAL(&testStatusMux);
      testStatus.running = false;
      copyStr(testStatus.step, sizeof(testStatus.step), "adapter gone");
      portEXIT_CRITICAL(&testStatusMux);
    }
    if (dropRequested)
    {
      dropRequested = false; // settings changed (elmRestart): drop the link on the transport it was opened with
      adapterDisconnect();
      nextTryMs = 0;
      wifiBackoffMs = 0;
      adapterTryAgain(); // 0.37.1: paired, forgotten or switched: look again
    }
    if (modeWanted() && !wasModeWanted)
    {
      adapterTryAgain(); // 0.37.1: the OBD screen was just opened on the gauge: look again
      nextTryMs = 0;
    }
    wasModeWanted = modeWanted();
    if (scanRequested)
    {
      adapterTryAgain(); // 0.37.1: the web UI's Scan: look again
      adapterDisconnect();
      scanForAdapters();
      scanRequested = false;
      continue;
    }
    if (!elmActive() || !linkIsMaster())
    {
      if (adapterConnected())
      {
        adapterDisconnect();
      }
      setState(elmActive() ? "follower (master has it)" : "off");
      if (runtime.standby)
      {
        adapterTryAgain(); // 0.37.1: ignition off: the next ignition on is a fresh start
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!adapterConnected() && adapterGivenUp)
    {
      vTaskDelay(pdMS_TO_TICKS(300)); // 0.37.1: not found: stopped looking (the state says so)
      continue;
    }
    if (!adapterConnected())
    {
      if ((int32_t)(millis() - nextTryMs) < 0)
      {
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      nextTryMs = millis() + 5000; // reconnect backoff (adapters sleep when the ignition is off)
      linkTransport = settings.elmTransport;
      if (linkTransport == ELM_TR_WIFI)
      {
        setState("connecting (WiFi)");
        char error[72];
        if (!elmWifiConnect(error, sizeof(error)))
        {
          setError(error);
          elmWifiDisconnect(); // give the station back: the radio returns to the AP's channel while we wait
          if (!wifiBackoffMs)
          {
            wifiBackoffMs = 30000;
          }
          else if (wifiBackoffMs * 2 > 120000)
          {
            wifiBackoffMs = 120000;
          }
          else
          {
            wifiBackoffMs = wifiBackoffMs * 2;
          }
          nextTryMs = millis() + wifiBackoffMs;
          if (adapterMissed()) // 0.37.1: not there that many times: stop
          {
            continue;
          }
          char stateText[24];
          snprintf(stateText, sizeof(stateText), "no adapter, retry %lus", (unsigned long)(wifiBackoffMs / 1000));
          setState(stateText);
          continue;
        }
        wifiBackoffMs = 0;
        adapterTryAgain(); // 0.37.1: found it: the count starts again if it goes away
        terminalAdd("# ", adapterInfo());
        if (!sessionInit())
        {
          setState("init failed");
          adapterDisconnect();
          nextTryMs = millis() + 30000; // adapter there but no car answer (ignition off?): don't keep re-joining
          continue;
        }
        setState("polling");
      }
      else if (!settings.elmMac[0] && !settings.elmEnabled)
      {
        // opened from the menu with nothing paired: say so (the screen asks for pairing in the web UI), no scan
        setState("no adapter paired");
        nextTryMs = millis() + 1000;
        continue;
      }
    }
    if (!adapterConnected() && linkTransport == ELM_TR_BLE) // Bluetooth: bind (first likely adapter) and connect
    {
      if (!settings.elmMac[0])
      {
        // nothing bound yet: take the first likely adapter (strongest)
        scanForAdapters();
        if (foundAdapterCount && foundAdapters[0].likely)
        {
          copyStr(settings.elmMac, sizeof(settings.elmMac), foundAdapters[0].mac);
          settings.elmAddrType = foundAdapters[0].addressType;
          settingsDirty = true;
          terminalAdd("# bound ", foundAdapters[0].mac);
        }
        else
        {
          nextTryMs = millis() + 10000;
          if (!adapterMissed()) // 0.37.1: nothing in that many scans: stop (adapterMissed says so)
          {
            setState("no adapter found");
          }
          continue;
        }
      }
      setState("connecting");
      char error[64];
      if (!bleConnect(settings.elmMac, settings.elmAddrType, error, sizeof(error)))
      {
        setError(error);
        // 0.24.22: switched on after boot, Bluetooth can't start: WiFi, the display and the web server already hold the
        // internal RAM it needs (at boot it starts first, with plenty free). The web UI offers Restart Now.
        if (bleNeedsRestart())
        {
          setState("restart needed");
          nextTryMs = millis() + BLE_SLOW_RETRY_MS; // it may still fit later (RAM freed): look again now and then
          continue;
        }
        if (bleMissedConnects < 255)
        {
          bleMissedConnects++;
        }
        if (adapterMissed()) // 0.37.1: not there that many times: stop
        {
          continue;
        }
        if (bleMissedConnects >= BLE_MISSES_BEFORE_SLOW)
        {
          nextTryMs = millis() + BLE_SLOW_RETRY_MS; // adapter not around: leave the radio to WiFi for a while
          setState("not connected, retry 30s");
        }
        else
        {
          setState("not connected");
        }
        continue;
      }
      bleMissedConnects = 0; // found it: back to quick retries if it drops
      adapterTryAgain();     // 0.37.1: and the stop count starts again
      terminalAdd("# ", adapterInfo());
      if (!sessionInit())
      {
        setState("init failed");
        adapterDisconnect();
        continue;
      }
      setState("polling");
    }
    if (reidentifyRequested)
    {
      reidentifyRequested = false;
      if (!sessionInit())
      {
        adapterDisconnect();
        continue;
      }
      setState("polling");
    }
    // one poll cycle
    static uint8_t cycle = 0;
    static uint32_t rateStamp = 0;
    static uint32_t rateRequests = 0;
    const bool pollObdWanted = settings.elmPollObd || modeWanted() || rotationWanted(); // 0.15.0: or a page in the rotation reads OBD
    // 0.34.0: anything of this task's own that uses the adapter's headers this loop (the Diagnostics / Service jobs point
    // them elsewhere and diagHeadersBack puts them back first)
    const bool terminalWaiting = terminalQueue && uxQueueMessagesWaiting(terminalQueue) > 0;
    const bool ownWork = pollObdWanted || settings.elmExtraSource != ELM_EXTRA_OFF || settings.dsgSource == DSG_SRC_ELM ||
                         settings.dsgSource == DSG_SRC_ELM_TP20 || testRequested || terminalWaiting;
    if (terminalWaiting)
    {
      diagHeadersBack();
    }
    runTerminal();
    if (!keepRunning(myGeneration))
    {
      continue;
    }
    // 0.34.0: a Diagnostics / Service job waiting: run it, but take turns with the own work (OBD keeps its rate up)
    if (diagPending && (!diagJustRan || !ownWork))
    {
      runDiagJob();
      diagJustRan = true;
      continue;
    }
    diagJustRan = false;
    if (ownWork)
    {
      diagHeadersBack();
    }
    if (testRequested)
    {
      runTest();
      continue;
    }
    if (settings.elmExtraSource == ELM_EXTRA_KLINE)
    {
      pollKlineVag(); // the K-line session owns the line: no OBD / DSG polling beside it
    }
    else if (settings.elmExtraSource == ELM_EXTRA_CAN_MONITOR)
    {
      pollCanMonitor(); // the adapter monitors the car's CAN broadcast (ATMA): no OBD / DSG / K-line beside it
    }
    else
    {
      canMonStop(); // in case we just left CAN monitor mode: stop ATMA so OBD commands aren't lost in its stream
      if (pollObdWanted)
      {
        pollObd();
      }
      if (settings.dsgSource == DSG_SRC_ELM && (++cycle % 3) == 0)
      {
        pollDsg();
      }
      if (settings.dsgSource == DSG_SRC_ELM_TP20 && (!pollObdWanted || (++cycle % 2) == 0)) // with OBD on: every other cycle
      {
        pollDsgTp20();
      }
    }
    if (millis() - rateStamp >= 2000)
    {
      adapterStatus.reqPerSec = (adapterStatus.requests - rateRequests) * 1000.0f / (millis() - rateStamp);
      rateRequests = adapterStatus.requests;
      rateStamp = millis();
    }
    // battery voltage (ATRV) every 10 s: each read costs one round trip that the PIDs would otherwise get. Not in CAN
    // monitor mode (ELM_EXTRA_CAN_MONITOR): sending ATRV mid-ATMA stops the monitor and lands in the frame stream (volts = inf).
    static uint32_t voltsStamp = 0;
    if ((voltsStamp == 0 || millis() - voltsStamp >= 10000) && settings.elmExtraSource != ELM_EXTRA_CAN_MONITOR)
    {
      voltsStamp = millis();
      static char text[32];
      if (elmCommand("ATRV", text, sizeof(text), 500, false))
      {
        adapterStatus.volts = atof(text);
      }
    }
    if (!ownWork && !diagPending)
    {
      // nothing to poll: wait up to 200 ms, or until a Diagnostics / Service job arrives (elmDiagTransact wakes this task with
      // a "task notification", FreeRTOS's lightest way for one task to nudge another)
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------
void setupELM()
{
  memset(&adapterStatus, 0, sizeof(adapterStatus));
  copyStr(adapterStatus.state, sizeof(adapterStatus.state), "off");
  memset(&testStatus, 0, sizeof(testStatus));
  // over the ELM the ECU's reaction arrives with the send itself (the ATST window), so "no reaction" is known at once:
  // repeat after 30 ms instead of the CAN path's 200 ms
  tp20Client.config.repeatMs = 30;
  terminalQueue = xQueueCreate(6, 40);
  diagMutex = xSemaphoreCreateMutex();  // 0.34.0: one Diagnostics / Service job at a time
  diagDone = xSemaphoreCreateBinary();  // 0.34.0: "your job is done"
  xTaskCreatePinnedToCore(elmTaskFn, "elm", 8192, NULL, 3, &elmTask, 0); // 0.8.0: 6 -> 8 KB for the bench tests
}

void elmRestart()
{
  generation = generation + 1; // volatile: an explicit read + write (++ on a volatile is deprecated in C++20)
  dropRequested = true; // the ELM task disconnects (a WiFi disconnect blocks; this runs in the web server)
}

bool elmConnected()
{
  return connectionUp;
}

const char *elmLinkInfo()
{
  return adapterInfo();
}

void elmRequestScan()
{
  scanRequested = true;
}

bool elmScanning()
{
  return scanning || scanRequested;
}

uint8_t elmFound(BleFound *out, uint8_t max)
{
  portENTER_CRITICAL(&adapterStatusMux);
  const uint8_t count = foundAdapterCount < max ? foundAdapterCount : max;
  memcpy(out, foundAdapters, count * sizeof(BleFound));
  portEXIT_CRITICAL(&adapterStatusMux);
  return count;
}

void elmPair(const char *mac, uint8_t type)
{
  copyStr(settings.elmMac, sizeof(settings.elmMac), mac);
  settings.elmAddrType = type;
  settings.elmProtocol = 0; // a different car / adapter: search again
  // pairing an adapter means "use it": auto-connect on. Before 2026-10-14 it stayed off, so a gauge with no OBD page on
  // screen (or no screen at all) never connected - the web UI's Diag just said "off - off"
  settings.elmEnabled = 1;
  settingsDirty = true;
  elmRestart();
}

void elmForget()
{
  settings.elmMac[0] = '\0';
  settings.elmProtocol = 0;
  settingsDirty = true;
  elmRestart();
}

void elmReidentify()
{
  reidentifyRequested = true;
}

bool elmQueueCommand(const char *cmd)
{
  char command[40];
  copyStr(command, sizeof(command), cmd);
  for (char *pos = command; *pos; pos++)
  {
    *pos = toupper((unsigned char)*pos);
  }
  return terminalQueue && xQueueSend(terminalQueue, command, 0) == pdTRUE;
}

void elmTerminalText(char *buf, size_t len)
{
  size_t written = 0;
  buf[0] = '\0';
  portENTER_CRITICAL(&terminalMux);
  const uint8_t start = (terminalHead + TERM_LINES - terminalCount) % TERM_LINES;
  for (uint8_t i = 0; i < terminalCount && written < len; i++)
  {
    appendf(buf, len, written, "%s\n", terminalLines[(start + i) % TERM_LINES]);
  }
  portEXIT_CRITICAL(&terminalMux);
}

const ElmStatus &elmStatus()
{
  return adapterStatus;
}

bool elmCar29Bit()
{
  return protocolIs29Bit(adapterStatus.protocol);
}

uint8_t elmEngineAddress29()
{
  return engineAddress29Bit;
}

uint8_t elmGearboxAddress29()
{
  return gearboxAddress29Bit;
}

bool elmStartTest(const char *kind, uint8_t module, uint8_t group)
{
  if (!connectionUp || testStatus.running || testRequested)
  {
    return false;
  }
  for (uint8_t testKind = TEST_ALL; testKind <= TEST_CLEAR; testKind++)
  {
    if (!strcmp(kind, testNames[testKind]))
    {
      testModule = module ? module : 0x01;
      testGroup = group ? group : 1;
      // mark it running now, before the ELM task picks up testRequested on its next loop: without this a client
      // (the web UI's Run All, the bench console) polling straight after the request would still see the PREVIOUS
      // test's result in the gap and think this one had already finished (bench 0.22.0)
      portENTER_CRITICAL(&testStatusMux);
      testStatus.running = true;
      testStatus.result[0] = '\0';
      copyStr(testStatus.kind, sizeof(testStatus.kind), testNames[testKind]);
      copyStr(testStatus.step, sizeof(testStatus.step), "starting");
      portEXIT_CRITICAL(&testStatusMux);
      testRequested = testKind;
      return true;
    }
  }
  return false;
}

const ElmTestStatus &elmTestStatus()
{
  return testStatus;
}

const char *elmProtocolName(uint8_t protocol)
{
  static const char *names[] = {"-", "SAE J1850 PWM", "SAE J1850 VPW", "ISO 9141-2", "ISO 14230-4 KWP (5 baud)",
                                "ISO 14230-4 KWP (fast)", "ISO 15765-4 CAN 11/500", "ISO 15765-4 CAN 29/500",
                                "ISO 15765-4 CAN 11/250", "ISO 15765-4 CAN 29/250", "SAE J1939", "USER1 CAN", "USER2 CAN"};
  return protocol < 13 ? names[protocol] : "?";
}

void elmInfoProvider(char *buf, size_t len)
{
  if (!settings.elmEnabled)
  {
    snprintf(buf, len, "Bluetooth OBD off\n\nenable it in the web UI\n(Bluetooth OBD tab)");
    return;
  }
  const char *who;
  if (settings.elmTransport == ELM_TR_WIFI)
  {
    who = settings.elmWifiSsid[0] ? settings.elmWifiSsid : "WiFi: first OBD network";
  }
  else
  {
    who = settings.elmMac[0] ? settings.elmMac : "no adapter bound";
  }
  snprintf(buf, len, "%s\n%s\n%s\n\n%s %s\n%s\n%.1f req/s  %.1f V", adapterStatus.state, who, adapterStatus.adapter,
           adapterStatus.make, adapterStatus.model, elmProtocolName(adapterStatus.protocol), adapterStatus.reqPerSec,
           adapterStatus.volts);
}
