#include <can2gauge_CanMon.h>
#include <CANLine.h>
#include <LittleFS.h>

// ---------------------------------------------------------------------------
// monitor: one entry per id, filled from the CAN RX task
// ---------------------------------------------------------------------------
static CanMonEntry *entries = (CanMonEntry *)psramCalloc(CANMON_MAX_IDS, sizeof(CanMonEntry)); // in PSRAM (4.5 KB off internal RAM)
static uint16_t entryCount = 0;
static uint16_t lastHit = 0;
static portMUX_TYPE monMux = portMUX_INITIALIZER_UNLOCKED;

static void decodeGaugeKeys(const twai_message_t &frame); // 0.28.0, further down (the DBC section)

static void onFrame(const twai_message_t &frame)
{
  const uint32_t id = frame.identifier | (frame.extd ? 0x80000000UL : 0);
  const uint32_t now = millis();
  portENTER_CRITICAL(&monMux);
  CanMonEntry *entry = NULL;
  if (lastHit < entryCount && entries[lastHit].id == id)
  {
    entry = &entries[lastHit];
  }
  else
  {
    for (uint16_t i = 0; i < entryCount; i++)
    {
      if (entries[i].id == id)
      {
        entry = &entries[i];
        lastHit = i;
        break;
      }
    }
  }
  if (!entry && entryCount < CANMON_MAX_IDS)
  {
    entry = &entries[entryCount];
    lastHit = entryCount++;
    entry->id = id;
    entry->count = 0;
    entry->firstMs = now;
  }
  if (entry)
  {
    entry->dlc = frame.data_length_code > 8 ? 8 : frame.data_length_code;
    memcpy(entry->data, frame.data, 8);
    entry->count++;
    entry->lastMs = now;
  }
  portEXIT_CRITICAL(&monMux);
  decodeGaugeKeys(frame); // 0.28.0: the loaded DBCs' keyed signals into the gauge signals
}

uint16_t canMonitorSnapshot(CanMonEntry *out, uint16_t max, uint32_t maxAgeMs)
{
  const uint32_t now = millis();
  uint16_t count = 0;
  portENTER_CRITICAL(&monMux);
  for (uint16_t i = 0; i < entryCount && count < max; i++)
  {
    if (now - entries[i].lastMs <= maxAgeMs)
    {
      out[count++] = entries[i];
    }
  }
  portEXIT_CRITICAL(&monMux);

  for (uint16_t i = 1; i < count; i++) // by id (insertion sort, count is small)
  {
    CanMonEntry tmp = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].id > tmp.id)
    {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = tmp;
  }
  return count;
}

bool canMonitorGet(uint32_t id, CanMonEntry &out)
{
  bool ok = false;
  portENTER_CRITICAL(&monMux);
  for (uint16_t i = 0; i < entryCount; i++)
  {
    if (entries[i].id == id)
    {
      out = entries[i];
      ok = true;
      break;
    }
  }
  portEXIT_CRITICAL(&monMux);
  return ok;
}

float canMonitorRate(const CanMonEntry &entry)
{
  const uint32_t span = entry.lastMs - entry.firstMs;
  return (entry.count > 1 && span) ? (entry.count - 1) * 1000.0f / span : 0.0f;
}

// ---------------------------------------------------------------------------
// DBC: arrays in PSRAM, swapped under a mutex on (re)load
// ---------------------------------------------------------------------------
static CanDbcMessage *messages = NULL;
static CanDbcSignal *signals = NULL;
static CanDbcLabel *labels = NULL; // 0.28.0: value labels of the keyed signals
static uint16_t messageCount = 0, signalCount = 0;
static uint32_t labelCount = 0;
static uint16_t keyedCount = 0;    // 0.28.0: signals that feed a gauge signal (0 = nothing to decode on each frame)
static char dbcName[128] = "";     // the loaded DBCs' names ("maxxecu_default + vw_mlb")
static CanDbcSlotInfo slotInfo[CANDBC_SLOTS]; // 0.28.0: what each slot holds
static SemaphoreHandle_t dbcMutex = NULL;
static volatile uint32_t dbcGeneration = 1;

uint32_t canDbcGeneration()
{
  return dbcGeneration;
}

bool canDbcLoaded()
{
  return messageCount > 0;
}

const char *canDbcName()
{
  return dbcName;
}

uint16_t canDbcMessageCount()
{
  return messageCount;
}

uint16_t canDbcSignalCount()
{
  return signalCount;
}

const CanDbcSignal &canDbcSignal(uint16_t index)
{
  return signals[index];
}

const CanDbcMessage *canDbcFind(uint32_t id)
{
  int lo = 0, hi = (int)messageCount - 1; // sorted by id on load
  while (lo <= hi)
  {
    const int mid = (lo + hi) / 2;
    if (messages[mid].id == id)
    {
      return &messages[mid];
    }
    if (messages[mid].id < id)
    {
      lo = mid + 1;
    }
    else
    {
      hi = mid - 1;
    }
  }
  return NULL;
}

void canDbcLock()
{
  if (dbcMutex)
  {
    xSemaphoreTakeRecursive(dbcMutex, portMAX_DELAY);
  }
}

bool canDbcTryLock()
{
  if (!dbcMutex)
  {
    return false;
  }
  return xSemaphoreTakeRecursive(dbcMutex, 0) == pdTRUE; // 0 = don't wait (a DBC being loaded holds it for a while)
}

void canDbcUnlock()
{
  if (dbcMutex)
  {
    xSemaphoreGiveRecursive(dbcMutex);
  }
}

// splits a line on tabs in place; returns the field count
static uint8_t splitTabs(char *line, char **fields, uint8_t max)
{
  uint8_t count = 0;
  char *fieldStart = line;
  while (count < max)
  {
    fields[count++] = fieldStart;
    char *tab = strchr(fieldStart, '\t');
    if (!tab)
    {
      break;
    }
    *tab = '\0';
    fieldStart = tab + 1;
  }
  return count;
}

static size_t readLine(File &file, char *buf, size_t len)
{
  size_t length = file.readBytesUntil('\n', buf, len - 1);
  buf[length] = '\0';
  if (length && buf[length - 1] == '\r')
  {
    buf[--length] = '\0';
  }
  return length;
}

const char *canDbcSlotPath(uint8_t slot)
{
  if (slot == 1)
  {
    return CANDBC_PATH_2;
  }
  if (slot == 2)
  {
    return CANDBC_PATH_3;
  }
  return CANDBC_PATH;
}

const CanDbcSlotInfo &canDbcSlot(uint8_t slot)
{
  if (slot >= CANDBC_SLOTS)
  {
    slot = 0;
  }
  return slotInfo[slot];
}

// 0.28.0: a value label's text -> the gauge's value for that key. A number ("8", "0", "2") is the value itself; for the
// gear, "R" / "N" / "P" are 0; for the selector, the letters P R N D S M E L B. Anything else ("Error", "Init") = no value.
static int16_t labelValue(uint8_t gaugeKey, const char *text)
{
  bool isNumber = text[0] != '\0';
  for (uint8_t i = 0; text[i]; i++)
  {
    if (!isdigit((unsigned char)text[i]) && !(i == 0 && text[i] == '-'))
    {
      isNumber = false;
    }
  }
  if (isNumber)
  {
    return (int16_t)atoi(text);
  }
  if (gaugeKey == SIG_GEAR && text[1] == '\0' && (text[0] == 'R' || text[0] == 'N' || text[0] == 'P'))
  {
    return 0; // no gear engaged (SIG_SELECTOR says which)
  }
  if (gaugeKey == SIG_SELECTOR && text[1] == '\0')
  {
    switch (text[0])
    {
    case 'P':
      return SEL_P;
    case 'R':
      return SEL_R;
    case 'N':
      return SEL_N;
    case 'D':
      return SEL_D;
    case 'S':
      return SEL_S;
    case 'M':
      return SEL_M;
    case 'E':
      return SEL_E;
    case 'L':
      return SEL_L;
    case 'B':
      return SEL_B;
    default:
      break;
    }
  }
  return DBC_LABEL_SKIP;
}

// one slot's file read into its own tables (PSRAM), before they're merged with the other slots'
struct SlotTables
{
  CanDbcMessage *messages;
  CanDbcSignal *signals;
  CanDbcLabel *labels;
  uint16_t messageCount;
  uint16_t signalCount;
  uint32_t labelCount;
};

static void freeSlotTables(SlotTables &tables)
{
  free(tables.messages);
  free(tables.signals);
  free(tables.labels);
  memset(&tables, 0, sizeof(tables));
}

// reads a .c2d file (version 1 or 2) into tables and fills in its info. false = unreadable (err says why)
static bool readSlotFile(const char *path, SlotTables &tables, CanDbcSlotInfo &info, char *err, size_t errLen)
{
  memset(&tables, 0, sizeof(tables));
  File file = LittleFS.open(path, "r");
  if (!file)
  {
    snprintf(err, errLen, "%s not found", path);
    return false;
  }

  // pass 1: count
  char line[256];
  char *fields[12];
  uint32_t messageTotal = 0;
  uint32_t signalTotal = 0;
  uint32_t labelTotal = 0;
  bool header = false;
  while (file.available())
  {
    if (!readLine(file, line, sizeof(line)))
    {
      continue;
    }
    if (line[0] == 'M' && line[1] == '\t')
    {
      messageTotal++;
    }
    else if (line[0] == 'S' && line[1] == '\t')
    {
      signalTotal++;
    }
    else if (line[0] == 'V' && line[1] == '\t')
    {
      labelTotal++;
    }
    else if (!strncmp(line, "C2D\t", 4))
    {
      // C2D <version> <name> [<bus speed> <relocatable>]
      const uint8_t fieldCount = splitTabs(line, fields, 5);
      header = true;
      if (fieldCount >= 3)
      {
        copyStr(info.name, sizeof(info.name), fields[2]);
      }
      if (fieldCount >= 5)
      {
        info.busSpeed = strtoul(fields[3], NULL, 10);
        info.relocatable = atoi(fields[4]) == 1;
      }
    }
  }
  if (!header || !messageTotal || messageTotal > 4000 || signalTotal > 40000 || labelTotal > 20000)
  {
    file.close();
    const char *reason = "too big";
    if (!header)
    {
      reason = "not a .c2d file";
    }
    else if (!messageTotal)
    {
      reason = "no messages";
    }
    snprintf(err, errLen, "%s", reason);
    return false;
  }
  tables.messages = (CanDbcMessage *)ps_calloc(messageTotal, sizeof(CanDbcMessage));
  if (signalTotal)
  {
    tables.signals = (CanDbcSignal *)ps_calloc(signalTotal, sizeof(CanDbcSignal));
  }
  if (labelTotal)
  {
    tables.labels = (CanDbcLabel *)ps_calloc(labelTotal, sizeof(CanDbcLabel));
  }
  if (!tables.messages || (signalTotal && !tables.signals) || (labelTotal && !tables.labels))
  {
    freeSlotTables(tables);
    file.close();
    snprintf(err, errLen, "out of memory");
    return false;
  }

  // pass 2: fill
  file.seek(0);
  CanDbcMessage *current = NULL;      // the message the S lines belong to
  CanDbcSignal *currentSignal = NULL; // the signal the V lines belong to
  info.fileBase = 0x7FF;
  while (file.available())
  {
    if (!readLine(file, line, sizeof(line)))
    {
      continue;
    }
    const uint8_t fieldCount = splitTabs(line, fields, 12);
    if (fields[0][0] == 'M' && !fields[0][1] && fieldCount >= 4 && tables.messageCount < messageTotal)
    {
      current = &tables.messages[tables.messageCount++];
      current->id = strtoul(fields[1], NULL, 10);
      current->dlc = (uint8_t)atoi(fields[2]);
      copyStr(current->name, sizeof(current->name), fields[3]);
      current->firstSignal = tables.signalCount;
      current->signalCount = 0;
      currentSignal = NULL;
      if (!(current->id & 0x80000000UL) && current->id < info.fileBase)
      {
        info.fileBase = current->id; // the lowest 11-bit id: the base a relocatable DBC is moved from
      }
    }
    else if (fields[0][0] == 'S' && !fields[0][1] && fieldCount >= 9 && current && tables.signalCount < signalTotal)
    {
      CanDbcSignal &signal = tables.signals[tables.signalCount++];
      copyStr(signal.name, sizeof(signal.name), fields[1]);
      signal.start = (uint16_t)atoi(fields[2]);
      signal.len = (uint8_t)atoi(fields[3]);
      signal.flags = (uint8_t)atoi(fields[4]);
      signal.factor = strtof(fields[5], NULL);
      signal.offset = strtof(fields[6], NULL);
      copyStr(signal.unit, sizeof(signal.unit), fields[7]);
      signal.mux = (int16_t)atoi(fields[8]);
      signal.gaugeKey = SIG_NONE;
      signal.priority = 0;
      signal.firstLabel = tables.labelCount;
      signal.labelCount = 0;
      if (fieldCount >= 11 && fields[9][0]) // version 2: the gauge key and its priority
      {
        signal.gaugeKey = signalFromKey(fields[9]);
        signal.priority = (uint8_t)atoi(fields[10]);
        if (signal.gaugeKey != SIG_NONE)
        {
          info.keyed++;
        }
      }
      current->signalCount++;
      currentSignal = &signal;
    }
    else if (fields[0][0] == 'V' && !fields[0][1] && fieldCount >= 3 && currentSignal && tables.labelCount < labelTotal &&
             currentSignal->labelCount < 255)
    {
      CanDbcLabel &label = tables.labels[tables.labelCount++];
      label.raw = (int32_t)strtol(fields[1], NULL, 10);
      label.value = labelValue(currentSignal->gaugeKey, fields[2]);
      currentSignal->labelCount++;
    }
  }
  file.close();
  if (info.fileBase == 0x7FF)
  {
    info.fileBase = 0; // only extended ids
  }
  info.messages = tables.messageCount;
  info.signals = tables.signalCount;
  return true;
}

bool canDbcCheckFile(const char *path, char *err, size_t errLen)
{
  SlotTables tables;
  CanDbcSlotInfo info = {};
  const bool ok = readSlotFile(path, tables, info, err, errLen);
  freeSlotTables(tables);
  return ok;
}

// is this id already taken by a slot loaded before? (two DBCs mustn't claim one frame)
static bool idTaken(uint32_t id, const SlotTables *accepted, const bool *isAccepted, uint8_t beforeSlot, uint8_t &takenBy)
{
  for (uint8_t slot = 0; slot < beforeSlot; slot++)
  {
    if (!isAccepted[slot])
    {
      continue;
    }
    for (uint16_t i = 0; i < accepted[slot].messageCount; i++)
    {
      if (accepted[slot].messages[i].id == id)
      {
        takenBy = slot;
        return true;
      }
    }
  }
  return false;
}

void canDbcLoadAll()
{
  SlotTables slotTables[CANDBC_SLOTS];
  bool isAccepted[CANDBC_SLOTS] = {false, false, false};
  CanDbcSlotInfo newInfo[CANDBC_SLOTS];
  uint32_t messageTotal = 0;
  uint32_t signalTotal = 0;
  uint32_t labelTotal = 0;
  for (uint8_t slot = 0; slot < CANDBC_SLOTS; slot++)
  {
    memset(&slotTables[slot], 0, sizeof(SlotTables));
    memset(&newInfo[slot], 0, sizeof(CanDbcSlotInfo));
    const char *path = canDbcSlotPath(slot);
    if (!LittleFS.exists(path))
    {
      continue; // an empty slot
    }
    char err[48];
    if (!readSlotFile(path, slotTables[slot], newInfo[slot], err, sizeof(err)))
    {
      copyStr(newInfo[slot].error, sizeof(newInfo[slot].error), err);
      continue;
    }
    // a relocatable ECU (its base id moved in its own software): every 11-bit id moves by the same amount
    const uint16_t wantedBase = settings.dbcSlotBase[slot];
    if (newInfo[slot].relocatable && wantedBase != 0 && newInfo[slot].fileBase != 0)
    {
      const int32_t shift = (int32_t)wantedBase - (int32_t)newInfo[slot].fileBase;
      for (uint16_t i = 0; i < slotTables[slot].messageCount; i++)
      {
        CanDbcMessage &message = slotTables[slot].messages[i];
        if (!(message.id & 0x80000000UL))
        {
          message.id = (uint32_t)((int32_t)message.id + shift) & 0x7FF;
        }
      }
    }
    // two DBCs mustn't claim one frame: a slot with an id an earlier slot has is left out, and says which
    bool clash = false;
    for (uint16_t i = 0; i < slotTables[slot].messageCount && !clash; i++)
    {
      uint8_t takenBy = 0;
      if (idTaken(slotTables[slot].messages[i].id, slotTables, isAccepted, slot, takenBy))
      {
        snprintf(newInfo[slot].error, sizeof(newInfo[slot].error), "id 0x%03lX is in slot %u too",
                 (unsigned long)(slotTables[slot].messages[i].id & 0x1FFFFFFF), takenBy + 1);
        clash = true;
      }
    }
    if (clash)
    {
      freeSlotTables(slotTables[slot]);
      continue;
    }
    isAccepted[slot] = true;
    newInfo[slot].loaded = true;
    messageTotal += slotTables[slot].messageCount;
    signalTotal += slotTables[slot].signalCount;
    labelTotal += slotTables[slot].labelCount;
  }

  // one table for all of them, so everything that looks a frame up (canDbcFind) needn't know about slots
  CanDbcMessage *newMessages = NULL;
  CanDbcSignal *newSignals = NULL;
  CanDbcLabel *newLabels = NULL;
  if (messageTotal)
  {
    newMessages = (CanDbcMessage *)ps_calloc(messageTotal, sizeof(CanDbcMessage));
  }
  if (signalTotal)
  {
    newSignals = (CanDbcSignal *)ps_calloc(signalTotal, sizeof(CanDbcSignal));
  }
  if (labelTotal)
  {
    newLabels = (CanDbcLabel *)ps_calloc(labelTotal, sizeof(CanDbcLabel));
  }
  if ((messageTotal && !newMessages) || (signalTotal && !newSignals) || (labelTotal && !newLabels))
  {
    free(newMessages);
    free(newSignals);
    free(newLabels);
    newMessages = NULL;
    newSignals = NULL;
    newLabels = NULL;
    messageTotal = 0;
    signalTotal = 0;
    labelTotal = 0;
    for (uint8_t slot = 0; slot < CANDBC_SLOTS; slot++)
    {
      if (isAccepted[slot])
      {
        newInfo[slot].loaded = false;
        copyStr(newInfo[slot].error, sizeof(newInfo[slot].error), "out of memory");
      }
      isAccepted[slot] = false;
    }
  }
  uint16_t messageIndex = 0;
  uint16_t signalIndex = 0;
  uint32_t labelIndex = 0;
  uint16_t newKeyed = 0;
  char newName[sizeof(dbcName)] = "";
  for (uint8_t slot = 0; slot < CANDBC_SLOTS; slot++)
  {
    if (!isAccepted[slot])
    {
      freeSlotTables(slotTables[slot]);
      continue;
    }
    const uint16_t signalBase = signalIndex; // where this slot's signals start in the merged table
    const uint32_t labelBase = labelIndex;
    for (uint32_t i = 0; i < slotTables[slot].labelCount; i++)
    {
      newLabels[labelIndex++] = slotTables[slot].labels[i];
    }
    for (uint16_t i = 0; i < slotTables[slot].signalCount; i++)
    {
      CanDbcSignal signal = slotTables[slot].signals[i];
      signal.firstLabel = signal.firstLabel + labelBase;
      newSignals[signalIndex++] = signal;
    }
    for (uint16_t i = 0; i < slotTables[slot].messageCount; i++)
    {
      CanDbcMessage message = slotTables[slot].messages[i];
      message.firstSignal = message.firstSignal + signalBase;
      message.slot = slot;
      newMessages[messageIndex++] = message;
    }
    newKeyed += newInfo[slot].keyed;
    if (newName[0])
    {
      strncat(newName, " + ", sizeof(newName) - strlen(newName) - 1);
    }
    strncat(newName, newInfo[slot].name[0] ? newInfo[slot].name : "custom", sizeof(newName) - strlen(newName) - 1);
    freeSlotTables(slotTables[slot]);
  }

  // messages by id for the binary search (signals keep their indices: each message points at its own run)
  for (uint16_t i = 1; i < messageIndex; i++)
  {
    CanDbcMessage tmp = newMessages[i];
    int j = i - 1;
    while (j >= 0 && newMessages[j].id > tmp.id)
    {
      newMessages[j + 1] = newMessages[j];
      j--;
    }
    newMessages[j + 1] = tmp;
  }

  canDbcLock();
  CanDbcMessage *oldMessages = messages;
  CanDbcSignal *oldSignals = signals;
  CanDbcLabel *oldLabels = labels;
  messages = newMessages;
  signals = newSignals;
  labels = newLabels;
  messageCount = messageIndex;
  signalCount = signalIndex;
  labelCount = labelIndex;
  keyedCount = newKeyed;
  copyStr(dbcName, sizeof(dbcName), newName);
  memcpy(slotInfo, newInfo, sizeof(slotInfo));
  dbcGeneration = dbcGeneration + 1; // volatile: an explicit read + write (++ on a volatile is deprecated in C++20)
  canDbcUnlock();
  free(oldMessages);
  free(oldSignals);
  free(oldLabels);
  DEBUG("CAN DBC: %s, %u messages, %u signals, %u feed the gauge", dbcName[0] ? dbcName : "none", messageIndex,
        signalIndex, newKeyed);
}

void canDbcClearSlot(uint8_t slot)
{
  if (slot >= CANDBC_SLOTS)
  {
    return;
  }
  LittleFS.remove(canDbcSlotPath(slot));
  canDbcLoadAll(); // the other slots stay
}

static bool extractRaw(const uint8_t *data, uint8_t dlc, uint16_t start, uint8_t len, bool intel, uint64_t &value)
{
  value = 0;
  if (!len || len > 64)
  {
    return false;
  }
  if (intel)
  {
    for (uint8_t i = 0; i < len; i++)
    {
      const uint16_t bit = start + i;
      if (bit / 8 >= dlc)
      {
        return false;
      }
      if ((data[bit / 8] >> (bit % 8)) & 1)
      {
        value |= 1ULL << i;
      }
    }
    return true;
  }
  int bit = start; // Motorola: start = the MSB, DBC "sawtooth" bit numbering
  for (uint8_t i = 0; i < len; i++)
  {
    if (bit < 0 || bit / 8 >= dlc)
    {
      return false;
    }
    value = (value << 1) | ((data[bit / 8] >> (bit % 8)) & 1);
    bit = (bit % 8 == 0) ? bit + 15 : bit - 1;
  }
  return true;
}

static bool rawValue(const CanDbcSignal &signal, const uint8_t *data, uint8_t dlc, int64_t &out)
{
  uint64_t raw;
  if (!extractRaw(data, dlc, signal.start, signal.len, signal.flags & 1, raw))
  {
    return false;
  }
  if ((signal.flags & 2) && signal.len < 64 && (raw >> (signal.len - 1)) & 1)
  {
    out = (int64_t)(raw | (~0ULL << signal.len));
  }
  else
  {
    out = (int64_t)raw;
  }
  return true;
}

// a signal's raw number in this frame: false when it doesn't apply (multiplexed for another value, past the DLC)
static bool decodeRaw(const CanDbcMessage &message, const CanDbcSignal &signal, const uint8_t *data, uint8_t dlc,
                      int64_t &raw)
{
  if (signal.mux >= 0) // multiplexed: only when the multiplexer holds this signal's value
  {
    bool ok = false;
    for (uint16_t i = 0; i < message.signalCount && !ok; i++)
    {
      const CanDbcSignal &muxSignal = signals[message.firstSignal + i];
      int64_t muxValue;
      if (muxSignal.mux == -2 && rawValue(muxSignal, data, dlc, muxValue))
      {
        ok = muxValue == signal.mux;
      }
    }
    if (!ok)
    {
      return false;
    }
  }
  return rawValue(signal, data, dlc, raw);
}

bool canDbcDecode(const CanDbcMessage &message, const CanDbcSignal &signal, const uint8_t *data, uint8_t dlc,
                  float &value)
{
  int64_t raw;
  if (!decodeRaw(message, signal, data, dlc, raw))
  {
    return false;
  }
  value = (float)((double)raw * signal.factor + signal.offset);
  return true;
}

// ---------------------------------------------------------------------------
// 0.28.0: DBC signals that feed gauge signals (GaugeKey), straight from each frame
// ---------------------------------------------------------------------------
// a DBC value in the gauge's own unit for that signal (the catalogue's: mbar for MAP, bar for pressures, C, km/h ...).
// DBCs keep the maker's units (kPa, F ...) so they read like the maker's document.
static float toGaugeUnit(uint8_t gaugeKey, float value, const char *dbcUnit)
{
  const char *gaugeUnit = signalInfo((signalId_t)gaugeKey).unit;
  // the DBC's unit without a degree sign in front ("°C" -> "C"; the sign is 2 bytes in UTF-8)
  const char *unit = dbcUnit;
  if ((unsigned char)unit[0] == 0xC2 && (unsigned char)unit[1] == 0xB0)
  {
    unit = dbcUnit + 2;
  }
  if (!strcmp(gaugeUnit, "mbar"))
  {
    if (!strcmp(unit, "kPa"))
    {
      return value * 10.0f;
    }
    if (!strcmp(unit, "bar"))
    {
      return value * 1000.0f;
    }
    if (!strcmp(unit, "psi"))
    {
      return value * 68.9476f;
    }
  }
  else if (!strcmp(gaugeUnit, "bar"))
  {
    if (!strcmp(unit, "kPa"))
    {
      return value / 100.0f;
    }
    if (!strcmp(unit, "mbar") || !strcmp(unit, "hPa"))
    {
      return value / 1000.0f;
    }
    if (!strcmp(unit, "psi"))
    {
      return value / 14.5038f;
    }
  }
  else if (!strcmp(gaugeUnit, "C"))
  {
    if (!strcmp(unit, "F") || !strcmp(unit, "degF"))
    {
      return (value - 32.0f) * 5.0f / 9.0f;
    }
    if (!strcmp(unit, "K"))
    {
      return value - 273.15f;
    }
  }
  else if (!strcmp(gaugeUnit, "km/h"))
  {
    if (!strcmp(unit, "mph"))
    {
      return value * 1.60934f;
    }
    if (!strcmp(unit, "m/s"))
    {
      return value * 3.6f;
    }
  }
  else if (gaugeKey == SIG_LAMBDA || gaugeKey == SIG_LAMBDA_TARGET)
  {
    if (!strcmp(unit, "AFR"))
    {
      return value / 14.7f; // petrol
    }
  }
  return value; // already the gauge's unit (or one it doesn't know: taken as it is)
}

// two DBC signals for one gauge signal (GaugePriority): the higher one wins while it was heard in the last second
#define KEY_PRIORITY_HOLD_MS 1000 // how long a better source keeps a worse one out after it was last heard
static uint8_t *keyPriority = (uint8_t *)psramCalloc(SIG_COUNT, sizeof(uint8_t));      // priority last used per gauge signal
static uint32_t *keyPriorityMs = (uint32_t *)psramCalloc(SIG_COUNT, sizeof(uint32_t)); // when (0 = never)

static bool priorityAllows(uint8_t gaugeKey, uint8_t priority, uint32_t now)
{
  if (!keyPriority || !keyPriorityMs)
  {
    return true;
  }
  const bool better = priority >= keyPriority[gaugeKey];
  const bool stale = keyPriorityMs[gaugeKey] == 0 || now - keyPriorityMs[gaugeKey] > KEY_PRIORITY_HOLD_MS;
  if (better || stale)
  {
    keyPriority[gaugeKey] = priority;
    keyPriorityMs[gaugeKey] = now;
    return true;
  }
  return false;
}

// the gauge's value for a keyed signal's raw number: a value label first (gear / selector / "no value"), else
// raw * factor + offset in the gauge's unit. false = nothing to set
static bool keyedValue(const CanDbcSignal &signal, int64_t raw, float &value)
{
  for (uint8_t i = 0; i < signal.labelCount; i++)
  {
    const CanDbcLabel &label = labels[signal.firstLabel + i];
    if (label.raw == raw)
    {
      if (label.value == DBC_LABEL_SKIP)
      {
        return false; // Error / Init / Undefined
      }
      value = label.value;
      return true;
    }
  }
  if (signal.gaugeKey == SIG_SELECTOR)
  {
    return false; // a selector needs its label (P R N D ...): a bare number means nothing to the gauge
  }
  value = (float)((double)raw * signal.factor + signal.offset);
  value = toGaugeUnit(signal.gaugeKey, value, signal.unit);
  return true;
}

// the CAN RX task, every frame: the keyed signals of its message into the gauge signals
static void decodeGaugeKeys(const twai_message_t &frame)
{
  if (keyedCount == 0)
  {
    return; // no loaded DBC feeds the gauge (an opendbc file without an overlay, or none at all)
  }
  if (!canDbcTryLock())
  {
    return; // a DBC is being loaded: skip this frame rather than hold the RX task up
  }
  const uint32_t id = frame.identifier | (frame.extd ? 0x80000000UL : 0);
  const CanDbcMessage *message = canDbcFind(id);
  if (message)
  {
    uint8_t dlc = frame.data_length_code;
    if (dlc > 8)
    {
      dlc = 8;
    }
    const uint32_t now = millis();
    for (uint16_t i = 0; i < message->signalCount; i++)
    {
      const CanDbcSignal &signal = signals[message->firstSignal + i];
      if (signal.gaugeKey == SIG_NONE)
      {
        continue;
      }
      int64_t raw;
      float value;
      if (!decodeRaw(*message, signal, frame.data, dlc, raw) || !keyedValue(signal, raw, value))
      {
        continue;
      }
      if (priorityAllows(signal.gaugeKey, signal.priority, now))
      {
        signalSetBus((signalId_t)signal.gaugeKey, value);
      }
    }
  }
  canDbcUnlock();
}

bool canDbcIsHousekeeping(const CanDbcSignal &signal)
{
  char lowerName[sizeof(signal.name)];
  size_t i = 0;
  for (; signal.name[i] && i < sizeof(lowerName) - 1; i++)
  {
    lowerName[i] = (char)tolower((unsigned char)signal.name[i]);
  }
  lowerName[i] = '\0';
  static const char *words[] = {"checksum", "chksum", "crc", "counter", "zaehler", "alive", "parity", "rolling"};
  for (const char *word : words)
  {
    if (strstr(lowerName, word))
    {
      return true;
    }
  }
  const size_t length = strlen(lowerName);
  return (length >= 3 && (!strcmp(lowerName + length - 3, "_bz") || !strcmp(lowerName + length - 3, "_cs"))) ||
      (length >= 4 && !strcmp(lowerName + length - 4, "_chk"));
}

void canDbcFormat(const CanDbcSignal &signal, float value, char *buf, size_t len)
{
  const float factor = fabsf(signal.factor);
  const bool whole = factor >= 1.0f && fabsf(signal.factor - lroundf(signal.factor)) < 1e-6f &&
      fabsf(signal.offset - lroundf(signal.offset)) < 1e-6f;
  int decimals;
  if (whole)
  {
    decimals = 0;
  }
  else if (factor >= 0.1f)
  {
    decimals = 1;
  }
  else if (factor >= 0.01f)
  {
    decimals = 2;
  }
  else
  {
    decimals = 3;
  }
  snprintf(buf, len, signal.unit[0] ? "%.*f %s" : "%.*f", decimals, valueNoMinusZero(value, decimals), signal.unit);
}

void setupCanMonitor()
{
  dbcMutex = xSemaphoreCreateRecursiveMutex();
  canOnFrame(onFrame, true); // relayed frames too (OpenHaldex / adc2can over WiFi): the DBC and user signals read them
  canDbcLoadAll();           // 0.28.0: every DBC slot's file (slot 1 is the /candbc.c2d there has always been)
}
