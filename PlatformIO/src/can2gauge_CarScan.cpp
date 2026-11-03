#include <can2gauge_CarScan.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR: a buffer marked with it lives in PSRAM, not internal RAM (kept for WiFi and task stacks)
#include <stdarg.h>   // va_list: carScanSetState takes printf-style arguments
#include <KLine.h>    // kwp1281Connect / kwp1281ReadGroup / kwp2000Connect ... for the K-line jobs
#include <CANLine.h>  // canStats(): a CAN job needs the CAN up
#include <can2gauge_ELM.h>     // elmConnected(): a job through the OBD adapter needs it connected
#include <can2gauge_Service.h> // serviceCanBusy() ...: never at the same time as a Service tab job
#include <can2gauge_Vehicle.h> // 0.38.0: the VIN the klineObd job reads goes to Your Car

// ---------------------------------------------------------------------------
// Car scan (0.37.0): see can2gauge_CarScan.h. This file keeps the job and its results, turns them into JSON for the web
// UI and runs the K-line jobs; the CAN / OBD adapter jobs run in the VAG diag task (can2gauge_VagDiag.cpp), which has
// the UDS and TP2.0 code.
// ---------------------------------------------------------------------------

#define CARSCAN_KLINE_GAP_MS 1200 // a K-line module's session must be closed this long before the next 5-baud wake-up

static CarScanStatus status;                              // the job and its progress
EXT_RAM_BSS_ATTR static CarScanItem items[CARSCAN_MAX_ITEMS]; // its items and their answers (~55 KB, so in PSRAM)
static volatile bool canPending = false;                 // a uds / tp20 job waits for the VAG diag task
static volatile bool klinePending = false;               // a K-line job waits for the K-line task
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED; // guards the state text (the web server reads it)

// the job names the web UI uses, by carScanJob_t
static const char *jobNames[] = {"", "uds", "tp20Modules", "tp20Blocks", "klineModules", "klineBlocks", "klineObd"};

CarScanStatus &carScanStatus()
{
  return status;
}

CarScanItem &carScanItem(uint16_t index)
{
  if (index >= CARSCAN_MAX_ITEMS)
  {
    index = 0;
  }
  return items[index];
}

bool carScanStopWanted()
{
  return status.stopRequested;
}

void carScanSetState(const char *fmt, ...)
{
  char text[sizeof(status.state)];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  portENTER_CRITICAL(&statusMux);
  copyStr(status.state, sizeof(status.state), text);
  portEXIT_CRITICAL(&statusMux);
  DEBUG("CAR SCAN: %s", text);
}

void carScanFinish()
{
  status.done = true;
  status.busy = false;
  status.stopRequested = false;
}

bool carScanCanPending()
{
  return canPending;
}

bool carScanCanBusy()
{
  const bool isCanJob = status.job == CARSCAN_JOB_UDS || status.job == CARSCAN_JOB_TP20_MODULES ||
                        status.job == CARSCAN_JOB_TP20_BLOCKS;
  return status.busy && isCanJob && status.via == VDIAG_LINK_CAN;
}

bool carScanAdapterBusy()
{
  return status.busy && status.job == CARSCAN_JOB_UDS && status.via == VDIAG_LINK_ADAPTER;
}

bool carScanKlinePending()
{
  return klinePending;
}

// the VAG diag task takes the job (it runs it straight away)
void carScanCanTaken()
{
  canPending = false;
}

// two hex digits -> a byte; false if they aren't hex
static bool hexByte(char high, char low, uint8_t &out)
{
  char text[3];
  text[0] = high;
  text[1] = low;
  text[2] = '\0';
  char *end = NULL;
  const unsigned long value = strtoul(text, &end, 16);
  if (end != text + 2)
  {
    return false;
  }
  out = (uint8_t)value;
  return true;
}

// a list of numbers (module addresses or blocks) from the web UI into the items
static uint16_t numbersFromJson(JsonArrayConst list)
{
  uint16_t count = 0;
  for (size_t i = 0; i < list.size() && count < CARSCAN_MAX_ITEMS; i++)
  {
    const int number = list[i].as<int>();
    if (number < 0 || number > 255)
    {
      continue;
    }
    memset(&items[count], 0, sizeof(CarScanItem));
    items[count].number = (uint8_t)number;
    count++;
  }
  return count;
}

bool carScanStart(JsonObjectConst body, char *why, size_t whyLen)
{
  const char *jobName = body["job"] | "";
  uint8_t job = CARSCAN_JOB_NONE;
  for (uint8_t i = 1; i < sizeof(jobNames) / sizeof(jobNames[0]); i++)
  {
    if (!strcmp(jobName, jobNames[i]))
    {
      job = i;
    }
  }
  if (job == CARSCAN_JOB_NONE)
  {
    copyStr(why, whyLen, "unknown scan job");
    return false;
  }
  if (status.busy)
  {
    copyStr(why, whyLen, "a scan job is already running");
    return false;
  }
  if (serviceCanBusy() || serviceAdapterBusy() || serviceKlinePending()) // (cheap checks: no copy of the Service status)
  {
    copyStr(why, whyLen, "a Service tab job is running");
    return false;
  }
  if (runtime.standby)
  {
    copyStr(why, whyLen, "the ignition is off");
    return false;
  }
  if (runtime.otherScanners)
  {
    copyStr(why, whyLen, "Enable Other Scanners is on - switch it off first");
    return false;
  }
  uint8_t via = VDIAG_LINK_CAN;
  if (!strcmp(body["via"] | "", "adapter"))
  {
    via = VDIAG_LINK_ADAPTER;
  }
  const bool isKlineJob = job == CARSCAN_JOB_KLINE_MODULES || job == CARSCAN_JOB_KLINE_BLOCKS ||
                         job == CARSCAN_JOB_KLINE_OBD; // 0.38.0
  if (job != CARSCAN_JOB_UDS)
  {
    via = VDIAG_LINK_CAN; // TP2.0 runs on the gauge's own CAN only, the K-line on its own wire
  }
  if (job == CARSCAN_JOB_UDS && via == VDIAG_LINK_ADAPTER && !elmConnected())
  {
    copyStr(why, whyLen, "the OBD adapter isn't connected");
    return false;
  }
  if (!isKlineJob && via == VDIAG_LINK_CAN && !canStats().up)
  {
    copyStr(why, whyLen, "the CAN is down");
    return false;
  }

  // the items
  uint16_t count = 0;
  if (job == CARSCAN_JOB_UDS || job == CARSCAN_JOB_KLINE_OBD) // 0.38.0: klineObd's requests are read the same way
  {
    JsonArrayConst list = body["items"];
    for (size_t i = 0; i < list.size() && count < CARSCAN_MAX_ITEMS; i++)
    {
      JsonObjectConst itemObj = list[i];
      CarScanItem &item = items[count];
      memset(&item, 0, sizeof(item));
      item.txId = itemObj["tx"] | 0UL;
      item.rxId = itemObj["rx"] | 0UL;
      item.txExt = itemObj["txExt"] | 0;
      item.rxExt = itemObj["rxExt"] | 0;
      const char *requestText = itemObj["req"] | "";
      const size_t textLen = strlen(requestText);
      // a CAN request needs its id; a K-line OBD one goes to 0x33 and has none
      const bool hasTarget = item.txId != 0 || job == CARSCAN_JOB_KLINE_OBD;
      bool good = hasTarget && textLen >= 2 && textLen % 2 == 0 && textLen <= 2 * sizeof(item.request);
      for (size_t at = 0; good && at < textLen; at += 2)
      {
        good = hexByte(requestText[at], requestText[at + 1], item.request[at / 2]);
      }
      if (!good)
      {
        continue; // a request we can't send: left out
      }
      item.requestLen = (uint8_t)(textLen / 2);
      count++;
    }
  }
  else if (job == CARSCAN_JOB_TP20_MODULES || job == CARSCAN_JOB_KLINE_MODULES)
  {
    count = numbersFromJson(body["addrs"]);
  }
  else
  {
    count = numbersFromJson(body["blocks"]);
  }
  if (count == 0)
  {
    copyStr(why, whyLen, "nothing to ask");
    return false;
  }

  memset(&status, 0, sizeof(status));
  status.job = job;
  status.via = via;
  status.address = body["addr"] | 0;
  status.itemCount = count;
  status.busy = true;
  copyStr(status.state, sizeof(status.state), "waiting to start");
  if (isKlineJob)
  {
    klinePending = true; // the K-line task closes whatever it has open and runs it
  }
  else
  {
    canPending = true; // the VAG diag task runs it next
  }
  return true;
}

void carScanStop()
{
  if (status.busy)
  {
    status.stopRequested = true;
  }
}

// up to `count` bytes as hex text "62F18730..." into out
static void hexText(const uint8_t *data, uint16_t count, char *out, size_t outLen)
{
  size_t used = 0;
  for (uint16_t i = 0; i < count && used + 3 <= outLen; i++)
  {
    snprintf(out + used, outLen - used, "%02X", data[i]);
    used += 2;
  }
  out[used] = '\0';
}

void carScanToJson(JsonDocument &doc, bool withItems)
{
  uint8_t jobIndex = status.job;
  if (jobIndex >= sizeof(jobNames) / sizeof(jobNames[0]))
  {
    jobIndex = 0; // not a job we know: no name
  }
  doc["job"] = jobNames[jobIndex];
  doc["via"] = status.via;
  doc["busy"] = status.busy;
  doc["done"] = status.done;
  portENTER_CRITICAL(&statusMux);
  char state[sizeof(status.state)];
  copyStr(state, sizeof(state), status.state);
  portEXIT_CRITICAL(&statusMux);
  doc["state"] = state;
  doc["total"] = status.itemCount;
  doc["asked"] = status.itemsDone;
  doc["addr"] = status.address;
  doc["moduleId"] = status.moduleId;
  doc["label"] = status.labelFile;
  if (!withItems)
  {
    return; // the progress only: the items come once the job is done
  }
  JsonArray itemsArr = doc["items"].to<JsonArray>();
  for (uint16_t i = 0; i < status.itemCount && i < CARSCAN_MAX_ITEMS; i++)
  {
    const CarScanItem &item = items[i];
    JsonObject itemObj = itemsArr.add<JsonObject>();
    itemObj["s"] = item.state;
    if (item.nrc)
    {
      itemObj["nrc"] = item.nrc;
    }
    if (status.job == CARSCAN_JOB_UDS || status.job == CARSCAN_JOB_KLINE_OBD) // 0.38.0: + the K-line OBD answers
    {
      char answerText[2 * CARSCAN_ANSWER_KEEP + 1];
      uint16_t kept = item.answerLen;
      if (kept > CARSCAN_ANSWER_KEEP)
      {
        kept = CARSCAN_ANSWER_KEEP;
      }
      hexText(item.answer, kept, answerText, sizeof(answerText));
      itemObj["len"] = item.answerLen; // the whole answer's length (a DID's data = this - 3)
      itemObj["a"] = answerText;
      continue;
    }
    itemObj["n"] = item.number;
    if (item.text[0])
    {
      itemObj["id"] = item.text; // a module's identification, or a text block's text
    }
    if (item.labelFile[0])
    {
      itemObj["lbl"] = item.labelFile;
    }
    if (item.fieldCount > 0)
    {
      JsonArray fieldsArr = itemObj["f"].to<JsonArray>();
      for (uint8_t f = 0; f < item.fieldCount && f < 4; f++)
      {
        JsonArray fieldArr = fieldsArr.add<JsonArray>(); // [formula, a, b, value, unit]
        fieldArr.add(item.formula[f]);
        fieldArr.add(item.a[f]);
        fieldArr.add(item.b[f]);
        fieldArr.add(item.value[f]);
        fieldArr.add(item.unit[f]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// the K-line jobs (run in the K-line task, which closed its own session first)
// ---------------------------------------------------------------------------

// wakes a module: KWP1281 first (Mk4-era), else KWP2000 after the same 5-baud wake-up. Fills its identification.
// Returns 1 = KWP1281, 2 = KWP2000, 0 = no answer.
static uint8_t klineWake(uint8_t address, char *ident, size_t identLen)
{
  ident[0] = '\0';
  if (kwp1281Connect(address, ident, identLen))
  {
    return 1;
  }
  vTaskDelay(pdMS_TO_TICKS(2500)); // let it drop back to sleep before waking it for KWP2000
  uint8_t keyBytes[2];
  if (kwp2000Connect(address, false, keyBytes))
  {
    snprintf(ident, identLen, "KWP2000 (key bytes %02X %02X)", keyBytes[0], keyBytes[1]);
    return 2;
  }
  return 0;
}

static void klineSleep(uint8_t protocol)
{
  if (protocol == 1)
  {
    kwp1281Disconnect(); // end-output block: the module closes the session
  }
  else if (protocol == 2)
  {
    kwp2000Disconnect();
  }
}

// the label file that fits a module (the first of the candidates), "" = none
static void klineLabelFor(uint8_t address, const char *ident, char *out, size_t outLen)
{
  out[0] = '\0';
  if (!labelsPrepare())
  {
    return;
  }
  char candidates[LBL_MAX_CANDS][LBL_NAME_LEN];
  if (labelsFind(address, ident, candidates, LBL_MAX_CANDS) > 0)
  {
    copyStr(out, outLen, candidates[0]);
    labelsRemember(address, candidates[0], ident); // the page editor names this module's fields from now on
  }
}

static void klineModulesJob()
{
  uint16_t found = 0;
  for (uint16_t i = 0; i < status.itemCount && !status.stopRequested && !runtime.standby; i++)
  {
    CarScanItem &item = items[i];
    carScanSetState("Waking module %02X on the K-line (%u of %u)", item.number, i + 1, status.itemCount);
    vTaskDelay(pdMS_TO_TICKS(CARSCAN_KLINE_GAP_MS)); // the line idle after the last session
    const uint8_t protocol = klineWake(item.number, item.text, sizeof(item.text));
    if (protocol == 0)
    {
      item.state = CARSCAN_NO_ANSWER;
    }
    else
    {
      item.state = CARSCAN_ANSWERED;
      klineLabelFor(item.number, item.text, item.labelFile, sizeof(item.labelFile));
      klineSleep(protocol);
      found++;
    }
    status.itemsDone = i + 1;
  }
  carScanSetState("K-line: %u module(s) answered", found);
}

static void klineBlocksJob()
{
  carScanSetState("Waking module %02X on the K-line", status.address);
  vTaskDelay(pdMS_TO_TICKS(CARSCAN_KLINE_GAP_MS));
  uint8_t protocol = klineWake(status.address, status.moduleId, sizeof(status.moduleId));
  if (protocol == 0)
  {
    carScanSetState("Module %02X didn't answer on the K-line", status.address);
    return;
  }
  klineLabelFor(status.address, status.moduleId, status.labelFile, sizeof(status.labelFile));
  uint16_t found = 0;
  for (uint16_t i = 0; i < status.itemCount && !status.stopRequested && !runtime.standby; i++)
  {
    CarScanItem &item = items[i];
    carScanSetState("Module %02X: block %u (%u of %u)", status.address, item.number, i + 1, status.itemCount);
    Kwp1281Group block;
    bool got = false;
    if (protocol == 1)
    {
      got = kwp1281ReadGroup(item.number, block);
    }
    else
    {
      got = kwp2000ReadGroup(item.number, block);
    }
    if (got && (block.count > 0 || block.text[0]))
    {
      item.state = CARSCAN_ANSWERED;
      item.fieldCount = block.count;
      for (uint8_t f = 0; f < block.count && f < 4; f++)
      {
        item.formula[f] = block.fields[f].formula;
        item.a[f] = block.fields[f].a;
        item.b[f] = block.fields[f].b;
        item.value[f] = block.fields[f].value;
        copyStr(item.unit[f], sizeof(item.unit[f]), block.fields[f].unit);
      }
      copyStr(item.text, sizeof(item.text), block.text); // a text block (ME7: 81 = VIN)
      found++;
    }
    else
    {
      item.state = CARSCAN_REFUSED; // the module has no such block (or refused it)
      const bool sessionUp = (protocol == 1 && kwp1281Connected()) || (protocol == 2 && kwp2000Connected());
      if (!sessionUp) // some modules drop the session on a block they don't have: wake it again
      {
        vTaskDelay(pdMS_TO_TICKS(CARSCAN_KLINE_GAP_MS));
        char ident[48];
        protocol = klineWake(status.address, ident, sizeof(ident));
        if (protocol == 0)
        {
          carScanSetState("Module %02X stopped answering after block %u", status.address, item.number);
          status.itemsDone = i + 1;
          return;
        }
      }
    }
    status.itemsDone = i + 1;
  }
  klineSleep(protocol);
  carScanSetState("Module %02X: %u block(s) answered", status.address, found);
}

// 0.38.0: OBD-II over the K-line, any make (a car that isn't a VW, or a VW that only answers OBD): one wake-up of the
// OBD address 0x33 (fast init, then the 5-baud one), each item's request and its answer, then the VIN
static void klineObdJob()
{
  carScanSetState("Waking the engine for OBD-II on the K-line");
  vTaskDelay(pdMS_TO_TICKS(CARSCAN_KLINE_GAP_MS)); // the line idle after the last session
  uint8_t keyBytes[2];
  klSetBaud(10400); // ISO 14230-4 / ISO 9141-2 speed
  bool connected = kwp2000Connect(KWP2000_OBD_ADDRESS, true, keyBytes); // fast init
  if (!connected)
  {
    carScanSetState("No answer to the fast init: waking the engine at 5 baud");
    vTaskDelay(pdMS_TO_TICKS(2500)); // let the line settle
    klSetBaud(10400);
    connected = kwp2000Connect(KWP2000_OBD_ADDRESS, false, keyBytes); // 5-baud 0x33
  }
  if (!connected)
  {
    carScanSetState("No OBD-II answer on the K-line");
    return;
  }
  const char *kind = "KWP2000";
  if (kwp2000IsIso9141())
  {
    kind = "ISO 9141-2";
  }
  snprintf(status.moduleId, sizeof(status.moduleId), "%s (key bytes %02X %02X)", kind, keyBytes[0], keyBytes[1]);
  uint16_t found = 0;
  for (uint16_t i = 0; i < status.itemCount && !status.stopRequested && !runtime.standby; i++)
  {
    CarScanItem &item = items[i];
    carScanSetState("OBD-II on the K-line: request %u of %u", i + 1, status.itemCount);
    uint8_t answer[64];
    uint8_t answerLen = 0;
    if (kwp2000Request(item.request, item.requestLen, answer, answerLen, 1000, sizeof(answer)))
    {
      item.state = CARSCAN_ANSWERED;
      item.answerLen = answerLen;
      uint8_t kept = answerLen;
      if (kept > CARSCAN_ANSWER_KEEP)
      {
        kept = CARSCAN_ANSWER_KEEP;
      }
      memcpy(item.answer, answer, kept);
      found++;
    }
    else if (kwp2000LastNrc() != 0)
    {
      item.state = CARSCAN_REFUSED; // the ECU said no (7F)
      item.nrc = kwp2000LastNrc();
    }
    else
    {
      item.state = CARSCAN_NO_ANSWER;
    }
    // a second ECU's answer to the same request (every emissions ECU answers 0x33): read and dropped, so it isn't
    // taken as the answer to the next request
    uint8_t extra[64];
    uint8_t extraLen = 0;
    while (kwp2000ReadNext(extra, extraLen, 60, sizeof(extra)))
    {
    }
    status.itemsDone = i + 1;
  }
  char vin[18];
  if (kwp2000ReadObdVin(vin, sizeof(vin)) && vehicleVinValid(vin))
  {
    vehicleTakeVin(vin, VIN_SRC_KLINE, true); // a K-line car: the older of the two model years a VIN can mean
  }
  kwp2000Disconnect();
  carScanSetState("OBD-II on the K-line: %u of %u answered", found, status.itemCount);
}

void carScanKlineRun()
{
  klinePending = false;
  if (status.job == CARSCAN_JOB_KLINE_MODULES)
  {
    klineModulesJob();
  }
  else if (status.job == CARSCAN_JOB_KLINE_OBD)
  {
    klineObdJob(); // 0.38.0
  }
  else if (status.job == CARSCAN_JOB_KLINE_BLOCKS)
  {
    klineBlocksJob();
  }
  if (status.stopRequested)
  {
    carScanSetState("Stopped");
  }
  carScanFinish();
}
