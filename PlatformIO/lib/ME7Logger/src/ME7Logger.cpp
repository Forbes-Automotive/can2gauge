#include "ME7Logger.h"
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <KLine.h>
#include <can2gauge_defs.h>
#include <can2gauge_ME7Fast.h> // 0.19.0: B7 RAM-routine fast logging (install/poll/unhook)

EXT_RAM_BSS_ATTR static Me7Var variables[ME7_MAX_VARS];
static uint8_t varCount = 0;
static Me7EcuInfo ecuInfo;
static uint8_t samplesPerSecond = 10;
EXT_RAM_BSS_ATTR static char extraNames[ME7_MAX_VARS][ME7_NAME_LEN]; // 0.17.6: variables the gauge's pages show (me7AddExtra)
static uint8_t extraCount = 0;                      // how many of them
static me7State_t state = ME7_IDLE;
static uint32_t sampleCount = 0;
static uint32_t rateCount = 0;
static uint32_t rateStamp = 0;
static uint32_t sampleRate = 0;
static bool useDefine = true;    // 0x2C block mode (fallback: 0x23 per variable)
static uint8_t fastMode = ME7_MODE_NORMAL; // 0.19.0: ME7_MODE_NORMAL / TEST_LINK (v0 test routine) / FAST (v1)
static bool fastActive = false;  // 0.19.0: the fast routine installed + hooked OK this session (else normal reads)
static bool fastProvenDead = false; // 0.19.0: set once the routine installs but B7 won't answer - stop re-trying the
                                    // install on every reconnect until the mode is re-armed or the gauge restarts
static uint8_t blockBytes = 0;   // total bytes in the defined local-id block
static File logFile;
static char logName[FILE_LEN] = "";
static uint32_t logStartMs = 0;
static uint8_t missedInARow = 0; // 0.17.2: samples in a row the ECU didn't answer
#define ME7_MAX_MISSES 5         // 0.17.2: this many missed samples in a row = the ECU has gone (~2 s, inside its 5 s P3max)
#define ME7_MISS_PAUSE_MS 100    // 0.17.2: pause after a missed answer before the next request
#define KWP_P3_ISO_MS 55         // 0.17.2: ISO 14230 gap before each request until the ECU agrees new timing
#define KWP_P4_ISO_MS 5          // ISO 14230 gap between our request bytes

// ---------------------------------------------------------------------------
// parsing helpers
// ---------------------------------------------------------------------------
static void trim(char *text)
{
  char *start = text;
  while (*start == ' ' || *start == '\t')
  {
    start++;
  }
  if (start != text)
  {
    memmove(text, start, strlen(start) + 1);
  }
  size_t len = strlen(text);
  while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' || text[len - 1] == '\n'))
  {
    text[--len] = '\0';
  }
}

static void unbrace(char *text)
{ // {text} -> text
  trim(text);
  size_t len = strlen(text);
  if (len >= 2 && text[0] == '{' && text[len - 1] == '}')
  {
    memmove(text, text + 1, len - 2);
    text[len - 2] = '\0';
    trim(text);
  }
}

static bool readLine(File &file, char *buf, size_t len)
{
  size_t count = 0;
  int character;
  bool any = false;
  while ((character = file.read()) >= 0)
  {
    any = true;
    if (character == '\n')
    {
      break;
    }
    if (count < len - 1)
    {
      buf[count++] = (char)character;
    }
  }
  buf[count] = '\0';
  return any;
}

// split a comma separated line into up to maxFields fields (in place). Commas inside {braces} are kept.
static uint8_t splitCsv(char *line, char **fields, uint8_t maxFields)
{
  uint8_t count = 0;
  bool inBrace = false;
  fields[count++] = line;
  for (char *pos = line; *pos && count < maxFields; pos++)
  {
    if (*pos == '{')
    {
      inBrace = true;
    }
    else if (*pos == '}')
    {
      inBrace = false;
    }
    else if (*pos == ',' && !inBrace)
    {
      *pos = '\0';
      fields[count++] = pos + 1;
    }
  }
  return count;
}

static void keyValue(char *line, char **key, char **val)
{
  *key = line;
  *val = NULL;
  char *equals = strchr(line, '=');
  if (!equals)
  {
    return;
  }
  *equals = '\0';
  *val = equals + 1;
  // strip inline comment
  char *semicolon = strchr(*val, ';');
  if (semicolon)
  {
    *semicolon = '\0';
  }
  trim(*key);
  trim(*val);
}

// 0.17.4: .ecu files are Latin-1 text, and their degree sign (byte 0xB0) isn't valid text for the web UI (it showed
// as a broken character). The units are written the way the gauge's own signals are: "°C" -> "C", "°KW" (crank
// degrees) or any other "°" -> "deg". Any other non-ASCII byte is dropped.
static void plainUnit(char *unit)
{
  char plain[ME7_UNIT_LEN];
  size_t out = 0;
  size_t i = 0;
  while (unit[i] != '\0' && out < sizeof(plain) - 1)
  {
    const uint8_t character = (uint8_t)unit[i];
    if (character == 0xB0 && unit[i + 1] == 'C')
    {
      i++; // "°C": just the C, which comes next
    }
    else if (character == 0xB0)
    {
      for (const char *word = "deg"; *word != '\0' && out < sizeof(plain) - 1; word++)
      {
        plain[out] = *word;
        out++;
      }
      i++;
      if (unit[i] == 'K' && unit[i + 1] == 'W')
      {
        i += 2; // "°KW" = crank degrees: "deg" says it
      }
    }
    else if (character < 0x80)
    {
      plain[out] = (char)character;
      out++;
      i++;
    }
    else
    {
      i++; // another non-ASCII byte: left out
    }
  }
  plain[out] = '\0';
  strcpy(unit, plain);
}

// ---------------------------------------------------------------------------
// file loading
// ---------------------------------------------------------------------------
static bool loadCfg(fs::FS &fs, const char *path, char *err, size_t errLen)
{
  File file = fs.open(path, "r");
  if (!file)
  {
    snprintf(err, errLen, "cfg not found: %s", path);
    return false;
  }
  char line[160];
  char section[32] = "";
  varCount = 0;
  samplesPerSecond = 10;

  while (readLine(file, line, sizeof(line)))
  {
    trim(line);
    if (!line[0] || line[0] == ';' || line[0] == '#')
    {
      continue;
    }
    if (line[0] == '[')
    {
      char *bracket = strchr(line, ']');
      if (bracket)
      {
        *bracket = '\0';
      }
      copyStr(section, sizeof(section), line + 1);
      continue;
    }
    if (strcasecmp(section, "Configuration") == 0)
    {
      char *key;
      char *value;
      keyValue(line, &key, &value);
      if (value && strcasecmp(key, "SamplesPerSecond") == 0)
      {
        samplesPerSecond = constrain(atoi(value), 1, 100);
      }
    }
    else if (strcasecmp(section, "LogVariables") == 0 && varCount < ME7_MAX_VARS)
    {
      // "name ; alias ; comment" or just "name"
      char *semicolon = strchr(line, ';');
      char *alias = NULL;
      if (semicolon)
      {
        *semicolon = '\0';
        alias = semicolon + 1;
        char *secondSemicolon = strchr(alias, ';');
        if (secondSemicolon)
        {
          *secondSemicolon = '\0';
        }
        trim(alias);
      }
      trim(line);
      if (!line[0])
      {
        continue;
      }
      Me7Var &var = variables[varCount++];
      memset(&var, 0, sizeof(var));
      copyStr(var.name, ME7_NAME_LEN, line);
      if (alias)
      {
        copyStr(var.alias, ME7_ALIAS_LEN, alias);
      }
      var.factor = 1;
    }
  }
  file.close();

  // 0.17.6: then the variables the gauge's pages show, if the .cfg doesn't list them already
  for (uint8_t extra = 0; extra < extraCount && varCount < ME7_MAX_VARS; extra++)
  {
    bool listed = false;
    for (uint8_t i = 0; i < varCount; i++)
    {
      if (strcmp(variables[i].name, extraNames[extra]) == 0)
      {
        listed = true;
      }
    }
    if (listed)
    {
      continue;
    }
    Me7Var &var = variables[varCount];
    varCount++;
    memset(&var, 0, sizeof(var));
    copyStr(var.name, ME7_NAME_LEN, extraNames[extra]);
    var.factor = 1;
  }

  if (varCount == 0)
  {
    snprintf(err, errLen, "cfg has no [LogVariables]");
    return false;
  }
  return true;
}

void me7ClearExtras()
{
  extraCount = 0;
}

void me7AddExtra(const char *name)
{
  if (!name || !name[0] || extraCount >= ME7_MAX_VARS)
  {
    return;
  }
  for (uint8_t extra = 0; extra < extraCount; extra++)
  {
    if (strcmp(extraNames[extra], name) == 0)
    {
      return; // two pages show the same variable: logged once
    }
  }
  copyStr(extraNames[extraCount], ME7_NAME_LEN, name);
  extraCount++;
}

int8_t me7FindVar(const char *name)
{
  for (uint8_t i = 0; i < varCount; i++)
  {
    if (strcmp(variables[i].name, name) == 0)
    {
      return (int8_t)i;
    }
  }
  return -1;
}

static bool loadEcu(fs::FS &fs, const char *path, char *err, size_t errLen)
{
  File file = fs.open(path, "r");
  if (!file)
  {
    snprintf(err, errLen, "ecu not found: %s", path);
    return false;
  }
  memset(&ecuInfo, 0, sizeof(ecuInfo));
  ecuInfo.logSpeed = 10400;
  char line[300];
  char section[32] = "";
  uint8_t matched = 0;

  while (readLine(file, line, sizeof(line)) && matched < varCount)
  {
    if (!line[0] || line[0] == ';')
    {
      continue;
    }
    if (line[0] == '[')
    {
      char *bracket = strchr(line, ']');
      if (bracket)
      {
        *bracket = '\0';
      }
      copyStr(section, sizeof(section), line + 1);
      continue;
    }
    if (strcasecmp(section, "Communication") == 0)
    {
      char *key;
      char *value;
      keyValue(line, &key, &value);
      if (!value)
      {
        continue;
      }
      if (strcasecmp(key, "Connect") == 0)
      {
        copyStr(ecuInfo.connect, sizeof(ecuInfo.connect), value);
      }
      else if (strcasecmp(key, "Communicate") == 0)
      {
        copyStr(ecuInfo.communicate, sizeof(ecuInfo.communicate), value);
      }
      else if (strcasecmp(key, "LogSpeed") == 0)
      {
        ecuInfo.logSpeed = atol(value);
      }
    }
    else if (strcasecmp(section, "Identification") == 0)
    {
      char *key;
      char *value;
      keyValue(line, &key, &value);
      if (!value)
      {
        continue;
      }
      unbrace(value);
      if (strcasecmp(key, "HWNumber") == 0)
      {
        copyStr(ecuInfo.hwNumber, sizeof(ecuInfo.hwNumber), value);
      }
      else if (strcasecmp(key, "SWNumber") == 0)
      {
        copyStr(ecuInfo.swNumber, sizeof(ecuInfo.swNumber), value);
      }
      else if (strcasecmp(key, "PartNumber") == 0)
      {
        copyStr(ecuInfo.partNumber, sizeof(ecuInfo.partNumber), value);
      }
      else if (strcasecmp(key, "SWVersion") == 0)
      {
        copyStr(ecuInfo.swVersion, sizeof(ecuInfo.swVersion), value);
      }
      else if (strcasecmp(key, "EngineId") == 0)
      {
        copyStr(ecuInfo.engineId, sizeof(ecuInfo.engineId), value);
      }
    }
    else if (strcasecmp(section, "Measurements") == 0)
    {
      // quick reject before the full split: the name is the first token
      char *fields[12];
      uint8_t fieldCount = splitCsv(line, fields, 12);
      if (fieldCount < 10)
      {
        continue;
      }
      trim(fields[0]);
      for (uint8_t i = 0; i < varCount; i++)
      {
        Me7Var &variable = variables[i];
        if (variable.found || strcmp(variable.name, fields[0]) != 0)
        {
          continue;
        }
        unbrace(fields[1]);
        if (!variable.alias[0])
        {
          copyStr(variable.alias, ME7_ALIAS_LEN, fields[1]);
        }
        trim(fields[2]);
        variable.address = strtoul(fields[2], NULL, 0);
        variable.size = atoi(fields[3]);
        variable.bitmask = strtoul(fields[4], NULL, 0);
        unbrace(fields[5]);
        copyStr(variable.unit, ME7_UNIT_LEN, fields[5]);
        plainUnit(variable.unit);
        variable.isSigned = atoi(fields[6]) != 0;
        variable.inverse = atoi(fields[7]) != 0;
        variable.factor = atof(fields[8]);
        variable.offset = atof(fields[9]);
        if (variable.size != 1 && variable.size != 2)
        {
          variable.size = 1;
        }
        variable.found = true;
        matched++;
        break;
      }
    }
  }
  file.close();

  // drop variables the ecu file does not know (keep order)
  uint8_t kept = 0;
  for (uint8_t i = 0; i < varCount; i++)
  {
    if (variables[i].found)
    {
      variables[kept++] = variables[i];
    }
  }
  if (kept < varCount)
  {
    DEBUG("ME7: %d of %d cfg variables not in ecu file", varCount - kept, varCount);
  }
  varCount = kept;
  if (varCount == 0)
  {
    snprintf(err, errLen, "no cfg variable found in ecu file");
    return false;
  }
  return true;
}

bool me7Load(fs::FS &fs, const char *ecuPath, const char *cfgPath, char *err, size_t errLen)
{
  state = ME7_LOADING;
  err[0] = '\0';
  if (!loadCfg(fs, cfgPath, err, errLen) || !loadEcu(fs, ecuPath, err, errLen))
  {
    state = ME7_ERROR;
    return false;
  }
  // publish names on the gauge signal slots
  for (uint8_t i = 0; i < varCount && i < 8; i++)
  {
    signalSetLabel((signalId_t)(SIG_ME7_1 + i), variables[i].alias[0] ? variables[i].alias : variables[i].name,
                   variables[i].unit);
  }
  state = ME7_IDLE;
  DEBUG("ME7: loaded %d variables, %s, %lu baud, %d sps", varCount, ecuInfo.connect, (unsigned long)ecuInfo.logSpeed,
        samplesPerSecond);
  return true;
}

const Me7EcuInfo &me7EcuInfo()
{
  return ecuInfo;
}

uint8_t me7VarCount()
{
  return varCount;
}

const Me7Var &me7Var(uint8_t index)
{
  return variables[index < varCount ? index : 0];
}

uint8_t me7SamplesPerSecond()
{
  return samplesPerSecond;
}

// ---------------------------------------------------------------------------
// KWP2000 session
// ---------------------------------------------------------------------------
uint8_t me7BaudByte(uint32_t baud)
{
  // ISO 14230-2 baud rate identifier: baud = (mantissa + 32) * 2^exponent * 200 ... closest match
  uint8_t best = 0x14; // 10400
  uint32_t bestErr = 0xFFFFFFFF;
  for (uint8_t exponent = 0; exponent < 8; exponent++)
  {
    for (uint8_t mantissa = 0; mantissa < 32; mantissa++)
    {
      uint32_t candidate = (uint32_t)(mantissa + 32) * (1u << exponent) * 200u;
      uint32_t diff = candidate > baud ? candidate - baud : baud - candidate;
      if (diff < bestErr)
      {
        bestErr = diff;
        best = (exponent << 5) | mantissa;
      }
    }
  }
  return best;
}

// 0.17.6: start one StartDiagnosticSession mode (0x86 or 0x81). Asks for the fast log baud first; if the ECU refuses
// the baud it retries the plain request and stays at 10400. Returns true if the ECU accepted the session; baudTaken
// is set when the ECU switched to the fast baud (the caller then switches the UART to match).
static bool trySession(uint8_t mode, uint32_t speed, bool &baudTaken)
{
  uint8_t response[64];
  uint8_t responseLen;
  baudTaken = false;
  if (speed != 10400)
  {
    uint8_t request[3] = {0x10, mode, me7BaudByte(speed)};
    if (kwp2000Request(request, 3, response, responseLen, 500))
    {
      baudTaken = true;
      return true;
    }
  }
  // no baud change (or the ECU refused it): the plain two-byte session request
  uint8_t plainRequest[2] = {0x10, mode};
  return kwp2000Request(plainRequest, 2, response, responseLen, 500);
}

static bool sessionSetup(char *err, size_t errLen)
{
  (void)err;
  (void)errLen;
  // StartDiagnosticSession 0x10. Mode 0x86 is the development / "VW extended" session; some ECUs refuse it but take
  // mode 0x81 instead, so fall back 0x86 -> 0x81 (the way ME7Logger falls back s86 -> s81). The bench ECUs that
  // connected but never started the logger were stopping here, on a refused 0x86.
  const uint32_t speed = ecuInfo.logSpeed ? ecuInfo.logSpeed : 10400;
  bool baudTaken = false;
  bool started = trySession(0x86, speed, baudTaken);
  if (!started)
  {
    started = trySession(0x81, speed, baudTaken);
  }
  if (!started)
  {
    // 0.17.6: last resort - no extended session at all. Stay in the ECU's default session at 10400 and let the 2C
    // block define / 23 reads try there (some ECUs answer measurements in the default session). If those also fail,
    // defineBlocks()/planReads() report it then, so this is not fatal here.
    DEBUG("ME7: neither 10 86 nor 10 81 session - staying in the default session at 10400");
  }
  else if (baudTaken)
  {
    delay(30); // the ECU switches to the fast baud after its positive response
    klSetBaud(speed);
    delay(50);
  }
  else
  {
    DEBUG("ME7: ECU took the session but refused the baud change, staying at 10400");
  }
  // 0.17.2: AccessTimingParameters done properly: read the ECU's limits (83 00) and set them (83 03). This used to send
  // 83 02, which only reads the current values, so the timing was never set. If the ECU won't give its limits, set
  // the ones an ME7.5 is known to report: P2min 0, P2max 1 s, P3min 0,
  // P3max 5 s, P4min 0. Best effort: a refusal doesn't stop the logger.
  // 0.17.4: our byte gap is now the ECU's P4min (was a fixed 5 ms: ~35 ms of gaps in every 7-byte request, the
  // bench ME7.5 logged 3 samples/s). With 0 the request goes out in one write (klWrite).
  const uint8_t me7TimingLimits[5] = {0x00, 0x28, 0x00, 0x14, 0x00};
  kwp2000AccessTiming(me7TimingLimits, 0);
  return true;
}

// ---------------------------------------------------------------------------
// 0.17.4: the variables spread over several dynamically defined local identifiers (F0, F1, F2 ...), each read with
// one 21 <id>. The bench ME7.5 takes at most 3 entries per identifier: the 4th is refused with 7F 2C 12 whatever the
// variable (tried with the order changed), and entry numbers instead of byte positions are refused at entry 2. So
// when an entry is refused, the next identifier is started with it. 11 variables = 4 reads per sample, not 11.
// ---------------------------------------------------------------------------
#define ME7_FIRST_LID 0xF0 // the first local identifier we define
#define ME7_MAX_LIDS 11    // F0 .. FA
static uint8_t lidCount = 0;               // identifiers defined
static uint8_t lidBytes[ME7_MAX_LIDS];     // bytes in each one's record
static uint8_t varLid[ME7_MAX_VARS];       // which identifier each variable is in (0 = F0)
static uint8_t varLidOffset[ME7_MAX_VARS]; // and where it sits in that record

// clears identifier lid (definitionMode 0x04)
static void clearLid(uint8_t lid)
{
  uint8_t response[16];
  uint8_t responseLen;
  uint8_t request[3] = {0x2C, lid, 0x04};
  kwp2000Request(request, 3, response, responseLen, 300, sizeof(response));
}

// the variable numbers in address order (a plain insertion sort: there are at most ME7_MAX_VARS)
static void sortByAddress(uint8_t *order)
{
  for (uint8_t i = 0; i < varCount; i++)
  {
    order[i] = i;
  }
  for (uint8_t i = 1; i < varCount; i++)
  {
    const uint8_t moving = order[i];
    int slot = i - 1;
    while (slot >= 0 && variables[order[slot]].address > variables[moving].address)
    {
      order[slot + 1] = order[slot];
      slot--;
    }
    order[slot + 1] = moving;
  }
}

// ---------------------------------------------------------------------------
// 0.17.6: spans. A 2C entry isn't limited to one variable: the bench ME7.5 took one entry of 3, 4, 6, 8, 16, 32 and 64
// bytes, and 21 sent the whole stretch back. What it limits is 3 entries per identifier, and every identifier is a
// request of its own, one per ~10 ms (the ECU answers each request about 8 ms after it). So the variables are laid out
// as spans - stretches of neighbouring RAM - and each entry covers a span:
//   * variables that touch or overlap always share a span (ps_w + rl_w, tans + tmot);
//   * a gap between two spans is bridged (its bytes read for nothing) when that saves a whole request: for every
//     possible number of requests the smallest gaps are bridged until the spans fit, and the cheapest plan is kept.
// Bench timing (0.17.6, 8N0906018J), one 21 request with an answer of n bytes:
//   56000 baud:  ~9.3 ms + ~0.215 ms per byte (2 bytes 10.0 ms, 8 bytes 11.0, 16 bytes 12.6, 32 bytes 16.1, 64 bytes 23.0)
//   125000 baud: 10 ms flat up to 64 bytes (2-64 bytes 10.0-10.5 ms), then 20 ms (96 and 128 bytes) - the ECU answers
//                on its own 10 ms tick, and at this speed 64 bytes still fit inside one
// ---------------------------------------------------------------------------
#define ME7_ENTRIES_PER_LID 3 // the bench ME7.5 refuses a 4th entry (7F 2C 12)
#define ME7_SPAN_MAX 64       // the longest entry used (the bench ME7.5 took up to 128; 64 is the most that's free at 125000)
#define ME7_REQUEST_US 9300   // the fixed cost of one 21 request at 56000 baud, microseconds
#define ME7_BYTE_US 215       // and of each byte in its answer at 56000 baud (slower bauds scale it up)
#define ME7_TICK_US 10000     // 100000 baud and up: one ECU tick per request ...
#define ME7_BYTES_PER_TICK 64 // ... for every 64 answer bytes
static uint32_t spanStart[ME7_MAX_VARS]; // each span's first address
static uint32_t spanEnd[ME7_MAX_VARS];   // and the address just after it
static uint8_t spanCount = 0;            // spans in the plan

// lays the variables out as at most maxSpans spans (bridging the smallest gaps first). false = it can't be done
// without a span longer than ME7_SPAN_MAX
static bool planSpans(uint8_t maxSpans)
{
  uint8_t order[ME7_MAX_VARS];
  sortByAddress(order);
  spanCount = 0;
  for (uint8_t i = 0; i < varCount; i++)
  {
    const Me7Var &variable = variables[order[i]];
    const uint32_t end = variable.address + variable.size;
    if (spanCount > 0 && variable.address <= spanEnd[spanCount - 1] && end - spanStart[spanCount - 1] <= ME7_SPAN_MAX)
    {
      if (end > spanEnd[spanCount - 1])
      {
        spanEnd[spanCount - 1] = end; // touches or overlaps the span before: it grows to take this one in
      }
      continue;
    }
    spanStart[spanCount] = variable.address; // a span of its own
    spanEnd[spanCount] = end;
    spanCount++;
  }
  while (spanCount > maxSpans)
  {
    // the two neighbouring spans with the smallest gap between them, if joining them stays short enough
    int best = -1;
    uint32_t bestGap = 0xFFFFFFFF;
    for (uint8_t i = 0; i + 1 < spanCount; i++)
    {
      const uint32_t gap = spanStart[i + 1] - spanEnd[i];
      if (spanEnd[i + 1] - spanStart[i] <= ME7_SPAN_MAX && gap < bestGap)
      {
        bestGap = gap;
        best = i;
      }
    }
    if (best < 0)
    {
      return false; // every join would make a span too long
    }
    spanEnd[best] = spanEnd[best + 1]; // span best now reaches over the gap to the end of the next one
    for (uint8_t i = best + 1; i + 1 < spanCount; i++)
    {
      spanStart[i] = spanStart[i + 1]; // the ones after move down a place
      spanEnd[i] = spanEnd[i + 1];
    }
    spanCount--;
  }
  return true;
}

// how long one request with an answer of `bytes` takes at the baud in use, microseconds (bench figures above)
static uint32_t requestCostUs(uint32_t bytes)
{
  const uint32_t baud = klBaud();
  if (baud >= 100000)
  {
    const uint32_t ticks = (bytes + ME7_BYTES_PER_TICK - 1) / ME7_BYTES_PER_TICK; // 1 tick up to 64 bytes, 2 up to 128
    if (ticks == 0)
    {
      return ME7_TICK_US;
    }
    return ticks * ME7_TICK_US;
  }
  uint32_t byteUs = ME7_BYTE_US; // 56000 baud
  if (baud > 0 && baud < 56000)
  {
    byteUs = ME7_BYTE_US * 56000 / baud; // slower: each byte takes longer on the wire
  }
  return ME7_REQUEST_US + bytes * byteUs;
}

// how long one sample would take with the current plan, microseconds: the spans go 3 to a request, in address order
static uint32_t planCostUs()
{
  uint32_t total = 0;
  uint32_t bytesThisRequest = 0;
  for (uint8_t i = 0; i < spanCount; i++)
  {
    bytesThisRequest += spanEnd[i] - spanStart[i];
    if ((i + 1) % ME7_ENTRIES_PER_LID == 0 || i + 1 == spanCount)
    {
      total += requestCostUs(bytesThisRequest); // this request is full (or the last one)
      bytesThisRequest = 0;
    }
  }
  return total;
}

static bool defineBlocks(char *err, size_t errLen)
{
  // 0.17.6: pick the number of requests that gives the quickest sample (fewer requests = bigger gaps bridged)
  planSpans(ME7_MAX_VARS); // touching variables joined, no gaps bridged
  const uint8_t mostRequests = (spanCount + ME7_ENTRIES_PER_LID - 1) / ME7_ENTRIES_PER_LID;
  uint8_t bestRequests = mostRequests;
  uint32_t bestCost = planCostUs();
  for (uint8_t requests = mostRequests; requests >= 1; requests--)
  {
    if (!planSpans(requests * ME7_ENTRIES_PER_LID))
    {
      break; // fewer requests would need spans that are too long
    }
    const uint32_t cost = planCostUs();
    if (cost < bestCost)
    {
      bestCost = cost;
      bestRequests = requests;
    }
  }
  planSpans(bestRequests * ME7_ENTRIES_PER_LID); // the plan used
  DEBUG("ME7: %u variables in %u spans, %u requests a sample (~%lu ms)", varCount, spanCount, bestRequests,
        (unsigned long)(bestCost / 1000));

  uint8_t response[64];
  uint8_t responseLen;
  lidCount = 0;
  blockBytes = 0;
  uint8_t span = 0; // the next span to place
  while (span < spanCount)
  {
    if (lidCount >= ME7_MAX_LIDS)
    {
      snprintf(err, errLen, "0x2C: more than %u identifiers needed", ME7_MAX_LIDS);
      return false;
    }
    const uint8_t lid = ME7_FIRST_LID + lidCount;
    clearLid(lid);
    uint8_t position = 1;   // byte position of the next entry in this record (1 = first)
    uint8_t entriesHere = 0;
    while (span < spanCount)
    {
      const uint32_t address = spanStart[span];
      const uint8_t size = (uint8_t)(spanEnd[span] - spanStart[span]);
      // DynamicallyDefineLocalIdentifier by memory address: [2C][id][03][position][size][addrH][addrM][addrL]
      uint8_t request[8] = {0x2C, lid, 0x03, position, size, (uint8_t)(address >> 16), (uint8_t)(address >> 8),
                            (uint8_t)address};
      if (!kwp2000Request(request, 8, response, responseLen, 300))
      {
        if (entriesHere == 0)
        {
          snprintf(err, errLen, "0x2C refused %u bytes at %06lX as the first entry of %02X", size,
                   (unsigned long)address, lid);
          return false; // not even one entry: the 0x23 reads take over
        }
        break; // this identifier is full: the span starts the next one
      }
      // every variable inside this span: its identifier, and where it sits in that identifier's record
      for (uint8_t i = 0; i < varCount; i++)
      {
        if (variables[i].address >= address && variables[i].address + variables[i].size <= spanEnd[span])
        {
          varLid[i] = lidCount;
          varLidOffset[i] = (position - 1) + (uint8_t)(variables[i].address - address);
        }
      }
      position += size;
      blockBytes += size;
      entriesHere++;
      span++;
    }
    lidBytes[lidCount] = position - 1;
    lidCount++;
  }
  DEBUG("ME7: %u variables in %u local identifiers (F0..%02X)", varCount, lidCount, ME7_FIRST_LID + lidCount - 1);
  return true;
}

// ---------------------------------------------------------------------------
// 0.17.4: the 0x23 fallback reads neighbouring variables together. The variables are taken in address order and one
// read covers every variable that starts within ME7_READ_GAP bytes of the end of the one before (up to ME7_READ_MAX
// bytes in one read): at 56000 baud a byte costs ~0.2 ms, a request and answer ~10-20 ms. One 0x23 per variable (the
// old way) gave 3 samples/s with 11 variables on the bench ME7.5. nyet's me7-logger reads touching variables together.
// ---------------------------------------------------------------------------
#define ME7_READ_GAP 32  // bytes between two variables that one read still spans
#define ME7_READ_MAX 128 // longest single read (the answer must fit kwp2000Request's buffer)
static uint32_t readAddress[ME7_MAX_VARS]; // where each read starts
static uint8_t readSize[ME7_MAX_VARS];     // its length in bytes
static uint8_t readCount = 0;              // reads per sample
static uint8_t varRead[ME7_MAX_VARS];      // which read each variable is in
static uint8_t varOffset[ME7_MAX_VARS];    // and where it sits in that read

static void planReads()
{
  // the variable numbers in address order (a plain insertion sort: there are at most ME7_MAX_VARS)
  uint8_t order[ME7_MAX_VARS];
  for (uint8_t i = 0; i < varCount; i++)
  {
    order[i] = i;
  }
  for (uint8_t i = 1; i < varCount; i++)
  {
    const uint8_t moving = order[i];
    int slot = i - 1;
    while (slot >= 0 && variables[order[slot]].address > variables[moving].address)
    {
      order[slot + 1] = order[slot];
      slot--;
    }
    order[slot + 1] = moving;
  }
  readCount = 0;
  for (uint8_t i = 0; i < varCount; i++)
  {
    const Me7Var &variable = variables[order[i]];
    bool joined = false;
    if (readCount > 0)
    {
      const uint8_t last = readCount - 1;
      const uint32_t readEnd = readAddress[last] + readSize[last];
      const uint32_t newEnd = variable.address + variable.size;
      if (variable.address <= readEnd + ME7_READ_GAP && newEnd - readAddress[last] <= ME7_READ_MAX)
      {
        if (newEnd > readEnd)
        {
          readSize[last] = (uint8_t)(newEnd - readAddress[last]); // the read grows to take this one in
        }
        varRead[order[i]] = last;
        varOffset[order[i]] = (uint8_t)(variable.address - readAddress[last]);
        joined = true;
      }
    }
    if (!joined)
    {
      readAddress[readCount] = variable.address; // a new read for this one
      readSize[readCount] = variable.size;
      varRead[order[i]] = readCount;
      varOffset[order[i]] = 0;
      readCount++;
    }
  }
  DEBUG("ME7: %u variables in %u memory reads", varCount, readCount);
}

// 0.19.0: set the B7 fast path (takes effect at the next me7Connect): ME7_MODE_NORMAL, ME7_MODE_TEST_LINK (installs
// the v0 test routine so a B7 returns "C2G!" - proves the write/hook/poll chain) or ME7_MODE_FAST (installs v1)
void me7SetFastMode(uint8_t mode)
{
  fastMode = mode;
  fastProvenDead = false; // re-arming the mode gives the install another go
}

// 0.19.0: the fast path mode armed now (ME7_MODE_NORMAL / TEST_LINK / FAST) - the K-line task checks it so the
// saved setting doesn't overwrite a Test link armed from the web UI
uint8_t me7GetFastMode()
{
  return fastMode;
}

// 0.19.0: true while a logging session is actually running on the fast path
bool me7FastRunning()
{
  return fastActive;
}

bool me7Connect(char *err, size_t errLen)
{
  err[0] = '\0';
  if (varCount == 0)
  {
    snprintf(err, errLen, "no variables loaded");
    state = ME7_ERROR;
    return false;
  }
  state = ME7_CONNECTING;
  klSetBaud(10400);
  // 0.17.2: ISO default timing until sessionSetup() agrees new values with the ECU (the logger used to run with no gap
  // before a request at all, inside the ECU's 55 ms P3min)
  kwp2000SetTiming(KWP_P3_ISO_MS, KWP_P4_ISO_MS);
  missedInARow = 0;
  uint8_t keyBytes[2] = {0, 0};
  bool fast = strncasecmp(ecuInfo.connect, "FAST", 4) == 0;
  uint8_t address = 0x01;
  const char *dash = strchr(ecuInfo.connect, '-');
  if (dash)
  {
    address = strtoul(dash + 1, NULL, 0);
  }

  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++)
  {
    ok = kwp2000Connect(address, fast, keyBytes);
    if (!ok)
    {
      delay(1500); // W5 before another init
    }
  }
  if (!ok)
  {
    snprintf(err, errLen, "no answer from ECU (init %s 0x%02X)", fast ? "fast" : "slow", address);
    state = ME7_ERROR;
    return false;
  }
  DEBUG("ME7: connected, key bytes %02X %02X", keyBytes[0], keyBytes[1]);
  // 0.17.4: the .ecu's Communicate= line - how messages are addressed after the wake-up. "HM0" = header mode 0, no
  // address bytes; "HM2-0x10" = address bytes, to 0x10. It used to be ignored and every request went to the wake-up
  // address (0x11 for SLOW-0x11), which the bench ME7.5 never answers - it answers as 0x10.
  if (strncasecmp(ecuInfo.communicate, "HM0", 3) == 0)
  {
    kwp2000SetAddressBytes(false);
  }
  else
  {
    const char *communicateDash = strchr(ecuInfo.communicate, '-');
    if (communicateDash)
    {
      kwp2000SetTarget((uint8_t)strtoul(communicateDash + 1, NULL, 0));
    }
  }
  DEBUG("ME7: communicate %s", ecuInfo.communicate[0] ? ecuInfo.communicate : "(not given)");
  if (!sessionSetup(err, errLen))
  {
    state = ME7_ERROR;
    return false;
  }

  state = ME7_DEFINING;
  useDefine = true; // try the 0x2C blocks first whatever Communicate= says; falls back to grouped 0x23 reads
  if (!defineBlocks(err, errLen))
  {
    DEBUG("ME7: %s - using 0x23 reads", err);
    useDefine = false;
    err[0] = '\0';
  }
  planReads(); // which variables each 0x23 read would cover (to compare, and for the fallback)
  if (useDefine && readCount <= lidCount)
  {
    useDefine = false; // the memory reads need no more requests than the identifiers: simpler, no definitions to lose
    DEBUG("ME7: %u memory reads <= %u identifiers - using 0x23 reads", readCount, lidCount);
  }
  sampleCount = 0;
  rateCount = 0;
  rateStamp = millis();

  // 0.19.0: optional B7 fast path - write our routine into ECU RAM and hook it so one B7 reads the whole block.
  // Test link installs the v0 test routine; Fast installs v1 for the selected variables. If it won't
  // install (RAM write refused, pointer not writable, etc.) we just stay on the normal 0x21/0x23 reads.
  fastActive = false;
  if (fastMode != ME7_MODE_NORMAL && !fastProvenDead)
  {
    if (fastMode == ME7_MODE_TEST_LINK)
    {
      fastActive = me7FastInstall(NULL, 0); // v0 test routine (B7 -> "C2G!")
    }
    else
    {
      uint32_t addrs[ME7_MAX_VARS];
      for (uint8_t i = 0; i < varCount; i++)
      {
        addrs[i] = variables[i].address;
      }
      fastActive = me7FastInstall(addrs, varCount); // v1 fast routine
    }
    if (fastActive)
    {
      // Prove the routine actually answers B7 before we trust the fast path. Test link just needs any answer;
      // Fast needs the full block. If B7 doesn't answer (NRC 0x11 serviceNotSupported, or nothing), un-hook and
      // stay on the normal reads for this whole session - otherwise every sample would miss and the repeated
      // failing polls would eventually trip the K-line task watchdog (bench 2026-10-22, the 8N refuses B7).
      uint8_t first[ME7FAST_MAX_VARS * 2];
      int got = me7FastPoll(first, sizeof(first));
      const int needBytes = (fastMode == ME7_MODE_FAST) ? varCount * 2 : 1;
      if (got >= needBytes)
      {
        DEBUG("ME7: fast path installed (mode %u), first B7 returned %d bytes: %02X %02X %02X %02X", fastMode, got,
              got > 0 ? first[0] : 0, got > 1 ? first[1] : 0, got > 2 ? first[2] : 0, got > 3 ? first[3] : 0);
      }
      else
      {
        DEBUG("ME7: fast routine did not answer B7 (got %d bytes) - un-hooking, staying on normal reads", got);
        me7FastUnhook();
        fastActive = false;
        fastProvenDead = true; // don't keep re-installing on every reconnect; re-arm the mode to try again
      }
    }
    else
    {
      DEBUG("ME7: fast install failed (%s) - staying on normal reads", me7FastStatusGet().message);
    }
  }

  state = ME7_LOGGING;
  return true;
}

static void decodeVar(Me7Var &variable, const uint8_t *raw)
{
  int32_t rawValue = variable.size == 2 ? (int32_t)((raw[1] << 8) | raw[0]) : raw[0]; // ME7 (C167) RAM is little-endian
  if (variable.bitmask)
  {
    variable.raw = (rawValue & variable.bitmask) ? 1 : 0;
    variable.value = variable.raw;
    return;
  }
  if (variable.isSigned)
  {
    rawValue = variable.size == 2 ? (int16_t)rawValue : (int8_t)rawValue;
  }
  variable.raw = rawValue;
  if (variable.inverse)
  {
    variable.value = (rawValue - variable.offset) != 0 ? variable.factor / (rawValue - variable.offset) : 0;
  }
  else
  {
    variable.value = variable.factor * rawValue - variable.offset;
  }
}

static void publishSample()
{
  sampleCount++;
  rateCount++;
  if (millis() - rateStamp >= 1000)
  {
    sampleRate = rateCount;
    rateCount = 0;
    rateStamp = millis();
  }
  for (uint8_t i = 0; i < varCount && i < 8; i++)
  {
    signalSet((signalId_t)(SIG_ME7_1 + i), variables[i].value);
  }

  // well-known ME7 names also feed the main signals so CAN-less cars get real gauges
  for (uint8_t i = 0; i < varCount; i++)
  {
    const Me7Var &variable = variables[i];
    if (strcmp(variable.name, "nmot") == 0 || strcmp(variable.name, "nmot_w") == 0)
    {
      signalSet(SIG_RPM, variable.value);
    }
    else if (strcmp(variable.name, "tmot") == 0)
    {
      signalSet(SIG_COOLANT, variable.value);
    }
    else if (strcmp(variable.name, "tans") == 0)
    {
      signalSet(SIG_IAT, variable.value);
    }
    else if (strcmp(variable.name, "pvdkds_w") == 0 || strcmp(variable.name, "pvdks_w") == 0)
    {
      signalSet(SIG_BOOST, variable.value);
    }
    else if (strcmp(variable.name, "wped_w") == 0 || strcmp(variable.name, "wped") == 0)
    {
      signalSet(SIG_PEDAL, variable.value);
    }
    else if (strcmp(variable.name, "wdkba") == 0)
    {
      signalSet(SIG_THROTTLE, variable.value);
    }
    else if (strcmp(variable.name, "rl_w") == 0 || strcmp(variable.name, "rl") == 0)
    {
      signalSet(SIG_LOAD, variable.value);
    }
    else if (strcmp(variable.name, "mshfm_w") == 0)
    {
      signalSet(SIG_MAF, variable.value);
    }
    else if (strcmp(variable.name, "lamsoni_w") == 0 || strcmp(variable.name, "lambts_w") == 0)
    {
      signalSet(SIG_LAMBDA, variable.value);
    }
    else if (strcmp(variable.name, "zwout") == 0)
    {
      signalSet(SIG_TIMING, variable.value);
    }
    else if (strcmp(variable.name, "ub") == 0 || strcmp(variable.name, "ubsq") == 0)
    {
      signalSet(SIG_BATTERY, variable.value);
    }
    else if (strcmp(variable.name, "vfzg") == 0 || strcmp(variable.name, "vfzg_w") == 0)
    {
      signalSet(SIG_SPEED, variable.value);
    }
    else if (strcmp(variable.name, "toel") == 0)
    {
      signalSet(SIG_OIL_TEMP, variable.value);
    }
  }

  if (logFile)
  {
    logFile.printf("%.3f", (millis() - logStartMs) / 1000.0f);
    for (uint8_t i = 0; i < varCount; i++)
    {
      logFile.printf(",%.3f", variables[i].value);
    }
    logFile.print('\n');
    if ((sampleCount & 0x1F) == 0)
    {
      logFile.flush();
    }
  }
}

// 0.17.2: one missed answer no longer ends the session (a new 5-baud wake-up takes seconds). Throw away what's left of
// the answer, give the ECU a moment and carry on with the next sample. Only ME7_MAX_MISSES in a row = the ECU has gone.
static bool sampleMissed()
{
  missedInARow++;
  if (missedInARow >= ME7_MAX_MISSES)
  {
    state = ME7_ERROR;
    return false;
  }
  delay(ME7_MISS_PAUSE_MS);
  klFlush();
  return false;
}

bool me7Sample()
{
  if (state != ME7_LOGGING)
  {
    return false;
  }
  // 0.19.0: fast path - one B7 request runs our RAM routine, which returns the whole block (2 bytes per variable)
  if (fastActive)
  {
    uint8_t block[ME7FAST_MAX_VARS * 2];
    int got = me7FastPoll(block, sizeof(block));
    if (got < 0)
    {
      return sampleMissed();
    }
    if (fastMode == ME7_MODE_FAST) // Fast: decode the block into the variables (Test link just proves the routine answers)
    {
      const int need = varCount * 2; // our routine returns 2 bytes per variable, in the configured order
      if (got < need)
      {
        return sampleMissed();
      }
      // the ECU's B7 service frames our bytes; the value block is the last need bytes of the reply (bench-tune the
      // offset once a stock capture shows the exact framing - see docs/me7_ramroutine_re.md 6g)
      const uint8_t *valueBlock = block + (got - need);
      for (uint8_t i = 0; i < varCount; i++)
      {
        decodeVar(variables[i], valueBlock + i * 2);
      }
    }
    missedInARow = 0;
    publishSample();
    return true;
  }
  uint8_t response[250];
  uint8_t responseLen;
  if (useDefine)
  {
    // 0.17.4: one 21 <id> per defined identifier (defineBlocks)
    for (uint8_t lidIndex = 0; lidIndex < lidCount; lidIndex++)
    {
      uint8_t request[2] = {0x21, (uint8_t)(ME7_FIRST_LID + lidIndex)};
      if (!kwp2000Request(request, 2, response, responseLen, 300))
      {
        return sampleMissed();
      }
      if (responseLen < 2 + lidBytes[lidIndex] || response[0] != 0x61)
      {
        return sampleMissed();
      }
      for (uint8_t i = 0; i < varCount; i++) // every variable in this identifier's record
      {
        if (varLid[i] == lidIndex)
        {
          decodeVar(variables[i], response + 2 + varLidOffset[i]);
        }
      }
    }
  }
  else
  {
    // 0.17.4: one 0x23 per group of neighbouring variables (planReads), not one per variable
    for (uint8_t readIndex = 0; readIndex < readCount; readIndex++)
    {
      const uint32_t address = readAddress[readIndex];
      uint8_t request[5] = {0x23, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address,
                            readSize[readIndex]};
      if (!kwp2000Request(request, 5, response, responseLen, 300) || responseLen < 1 + readSize[readIndex] ||
          response[0] != 0x63)
      {
        return sampleMissed(); // the rest of this sample is skipped
      }
      for (uint8_t i = 0; i < varCount; i++) // every variable in this read
      {
        if (varRead[i] == readIndex)
        {
          decodeVar(variables[i], response + 1 + varOffset[i]);
        }
      }
    }
  }
  missedInARow = 0;
  publishSample();
  return true;
}

void me7Disconnect()
{
  me7LogStop();
  if (fastActive)
  {
    me7FastUnhook(); // put the ECU's mapping-table pointer back before we drop the session (also clears at key-off)
    fastActive = false;
  }
  kwp2000Disconnect();
  klSetBaud(10400);
  kwp2000SetTiming(0, KWP_P4_ISO_MS); // 0.17.2: back to the K-line library's own default for the other K-line jobs
  state = ME7_IDLE;
}

me7State_t me7State()
{
  return state;
}

const char *me7StateName()
{
  static const char *names[] = {"idle", "loading", "connecting", "defining", "logging", "error"};
  return names[state <= ME7_ERROR ? state : ME7_ERROR];
}

uint32_t me7SampleCount()
{
  return sampleCount;
}

uint32_t me7SampleRate()
{
  return sampleRate;
}

uint8_t me7RequestsPerSample()
{
  if (state != ME7_LOGGING)
  {
    return 0;
  }
  if (useDefine)
  {
    return lidCount; // one 21 per defined identifier
  }
  return readCount; // one 0x23 per memory read
}

bool me7LogStart(fs::FS &fs, const char *dir)
{
  me7LogStop();
  if (!fs.exists(dir))
  {
    fs.mkdir(dir);
  }
  bool freeName = false;
  for (int fileNumber = 1; fileNumber < 1000 && !freeName; fileNumber++)
  {
    snprintf(logName, sizeof(logName), "%s/me7_%03d.csv", dir, fileNumber);
    freeName = !fs.exists(logName);
  }
  if (!freeName)
  {
    logName[0] = '\0'; // me7_001 .. me7_999 all exist: don't overwrite one of them
    return false;
  }
  logFile = fs.open(logName, "w");
  if (!logFile)
  {
    logName[0] = '\0';
    return false;
  }

  logFile.print("TIME");
  for (uint8_t i = 0; i < varCount; i++)
  {
    logFile.printf(",%s", variables[i].name);
  }
  logFile.print('\n');
  logFile.print("s");
  for (uint8_t i = 0; i < varCount; i++)
  {
    logFile.printf(",%s", variables[i].unit);
  }
  logFile.print('\n');
  logStartMs = millis();
  return true;
}

void me7LogStop()
{
  if (logFile)
  {
    logFile.flush();
    logFile.close();
  }
}

const char *me7LogFileName()
{
  return logName;
}
