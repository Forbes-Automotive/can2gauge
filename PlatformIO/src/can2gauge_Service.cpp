#include <can2gauge_Service.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_Labels.h> // the group's title and field names for a basic setting
#include <KLine.h>
#include <can2gauge_ELM.h>    // 0.34.0: elmConnected() - a job through the OBD adapter needs it connected
#include <can2gauge_CarScan.h> // 0.37.0: the car scan's CAN / adapter jobs count as busy too

// ---------------------------------------------------------------------------
// Service (0.16.0) - see can2gauge_Service.h. The job and its results live in "status"; the web server reads a copy,
// the K-line / VAG diag task writes it. statusMux keeps them from reading and writing it at the same moment.
// ---------------------------------------------------------------------------

EXT_RAM_BSS_ATTR static ServiceStatus status;                                   // the job and its results
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;  // guards status
static volatile bool klinePending = false;                     // a K-line job waits for the K-line task
static volatile bool canPending = false;                       // a CAN job waits for the VAG diag task
static volatile bool stopWanted = false;                       // Stop pressed during a basic setting
static volatile uint32_t lastWatchMs = 0;                      // the Service tab last asked for news (millis)

// ---------------------------------------------------------------------------
// reporting (any task)
// ---------------------------------------------------------------------------
void serviceSetState(const char *fmt, ...)
{
  char text[sizeof(status.state)];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  portENTER_CRITICAL(&statusMux);
  copyStr(status.state, sizeof(status.state), text);
  portEXIT_CRITICAL(&statusMux);
  DEBUG("Service: %s", text);
}

void serviceSetProtocol(const char *protocol, const char *moduleId)
{
  portENTER_CRITICAL(&statusMux);
  copyStr(status.protocol, sizeof(status.protocol), protocol);
  copyStr(status.moduleId, sizeof(status.moduleId), moduleId);
  portEXIT_CRITICAL(&statusMux);
}

void serviceClearFaults()
{
  portENTER_CRITICAL(&statusMux);
  status.faultCount = 0;
  portEXIT_CRITICAL(&statusMux);
}

void serviceAddFault(const char *code, uint8_t faultStatus)
{
  portENTER_CRITICAL(&statusMux);
  if (status.faultCount < SERVICE_MAX_FAULTS)
  {
    copyStr(status.faults[status.faultCount].code, sizeof(status.faults[0].code), code);
    status.faults[status.faultCount].status = faultStatus;
    status.faultCount++;
  }
  portEXIT_CRITICAL(&statusMux);
}

// 0.34.0: the scan's progress and the modules it found
void serviceScanProgress(uint8_t done, uint8_t total)
{
  portENTER_CRITICAL(&statusMux);
  status.scanDone = done;
  status.scanTotal = total;
  portEXIT_CRITICAL(&statusMux);
}

void serviceAddModule(const ServiceModule &module)
{
  portENTER_CRITICAL(&statusMux);
  if (status.moduleCount < SERVICE_MAX_MODULES)
  {
    status.modules[status.moduleCount] = module;
    status.moduleCount++;
  }
  portEXIT_CRITICAL(&statusMux);
}

void serviceFinish(bool ok)
{
  portENTER_CRITICAL(&statusMux);
  status.ok = ok;
  status.done = true;
  status.busy = false;
  portEXIT_CRITICAL(&statusMux);
}

// an SAE fault code from its two bytes: top 2 bits = P / C / B / U, then a digit 0-3 and three hex digits ("P0300")
void serviceDtcText(uint8_t high, uint8_t low, char *out, size_t len)
{
  const char letters[4] = {'P', 'C', 'B', 'U'}; // powertrain, chassis, body, network
  const char letter = letters[high >> 6];        // bits 7-6
  const uint8_t firstDigit = (high >> 4) & 0x03; // bits 5-4
  snprintf(out, len, "%c%u%X%X%X", letter, firstDigit, high & 0x0F, low >> 4, low & 0x0F);
}

// ---------------------------------------------------------------------------
// starting / stopping (web server)
// ---------------------------------------------------------------------------
bool serviceStart(uint8_t job, uint8_t via, uint8_t address, uint8_t group, char *why, size_t whyLen)
{
  why[0] = '\0';
  if (status.busy)
  {
    copyStr(why, whyLen, "a job is already running");
    return false;
  }
  if (runtime.standby)
  {
    copyStr(why, whyLen, "the ignition is off");
    return false;
  }
  if (runtime.otherScanners)
  {
    copyStr(why, whyLen, "Enable Other Scanners is on - switch it off first"); // 0.37.0
    return false;
  }
  if (job == SVC_JOB_BASIC_SETTING)
  {
    if (via != SVC_VIA_KLINE)
    {
      copyStr(why, whyLen, "basic settings work over the K-line only for now");
      return false;
    }
    // the module's routine expects the engine off and the car still (a throttle body adaptation moves the throttle)
    if (signalValid(SIG_RPM) && signalGet(SIG_RPM) > 0)
    {
      copyStr(why, whyLen, "the engine is running - switch it off, ignition on");
      return false;
    }
    if (signalValid(SIG_SPEED) && signalGet(SIG_SPEED) > 2)
    {
      copyStr(why, whyLen, "the car is moving");
      return false;
    }
  }
  if (job != SVC_JOB_READ_FAULTS && job != SVC_JOB_CLEAR_FAULTS && job != SVC_JOB_BASIC_SETTING && job != SVC_JOB_SCAN)
  {
    copyStr(why, whyLen, "unknown job");
    return false;
  }
  if (via > SVC_VIA_ADAPTER)
  {
    copyStr(why, whyLen, "unknown connection");
    return false;
  }
  if (job == SVC_JOB_SCAN && via == SVC_VIA_KLINE)
  {
    copyStr(why, whyLen, "the scan runs on CAN or through the OBD adapter");
    return false;
  }
  if (address == KWP2000_OBD_ADDRESS && via != SVC_VIA_KLINE && job != SVC_JOB_SCAN)
  {
    copyStr(why, whyLen, "OBD-II (any make) is for the K-line - on CAN pick the module"); // 0.38.0
    return false;
  }
  if (via == SVC_VIA_ADAPTER && !elmConnected())
  {
    copyStr(why, whyLen, "the OBD adapter isn't connected (Bluetooth OBD tab)");
    return false;
  }

  portENTER_CRITICAL(&statusMux);
  status.job = job;
  status.via = via;
  status.address = address;
  status.group = group;
  status.busy = true;
  status.done = false;
  status.ok = false;
  status.protocol[0] = '\0';
  status.moduleId[0] = '\0';
  status.faultCount = 0;
  status.reads = 0;
  status.fieldCount = 0;
  status.title[0] = '\0';
  status.scanDone = 0;
  status.scanTotal = 0;
  status.moduleCount = 0;
  for (uint8_t field = 0; field < 4; field++)
  {
    status.fieldName[field][0] = '\0';
  }
  copyStr(status.state, sizeof(status.state), "waiting for the line");
  portEXIT_CRITICAL(&statusMux);
  stopWanted = false;
  lastWatchMs = millis();
  if (via == SVC_VIA_KLINE)
  {
    klinePending = true; // the K-line task picks it up (it leaves whatever session it had)
  }
  else
  {
    canPending = true; // the VAG diag task picks it up
  }
  return true;
}

void serviceStop()
{
  stopWanted = true;
}

void serviceStatusCopy(ServiceStatus &out)
{
  lastWatchMs = millis(); // the Service tab is still open
  portENTER_CRITICAL(&statusMux);
  out = status;
  portEXIT_CRITICAL(&statusMux);
}

bool serviceCanBusy()
{
  return (status.busy && status.via == SVC_VIA_CAN) || carScanCanBusy(); // 0.37.0: or a car scan job on the CAN
}

bool serviceAdapterBusy()
{
  return (status.busy && status.via == SVC_VIA_ADAPTER) || carScanAdapterBusy(); // 0.37.0: or a car scan job
}

bool serviceKlinePending()
{
  return klinePending;
}

bool serviceCanPending()
{
  return canPending;
}

void serviceCanTaken()
{
  canPending = false;
}

uint8_t serviceFaultCount()
{
  return status.faultCount;
}

// ---------------------------------------------------------------------------
// K-line jobs (run in the K-line task)
// ---------------------------------------------------------------------------

// KWP1281 read: every stored fault into the list
static bool kwp1281ListFaults()
{
  Kwp1281Fault faults[SERVICE_MAX_FAULTS];
  uint8_t count = 0;
  if (!kwp1281ReadFaults(faults, SERVICE_MAX_FAULTS, count))
  {
    return false; // session lost
  }
  serviceClearFaults();
  for (uint8_t i = 0; i < count; i++)
  {
    char code[12];
    snprintf(code, sizeof(code), "%05u", faults[i].code); // scan tools show VAG fault numbers 5 digits long
    serviceAddFault(code, faults[i].status);
  }
  return true;
}

// a basic setting: the group again and again, its fields into the status, until something says stop. Returns how it
// ended for the status line.
static const char *runBasicSetting(uint8_t group)
{
  const uint32_t startMs = millis();
  portENTER_CRITICAL(&statusMux);
  status.startedMs = startMs;
  portEXIT_CRITICAL(&statusMux);
  serviceSetState("Basic setting group %03u running", group);
  while (1)
  {
    if (stopWanted)
    {
      return "stopped";
    }
    if (millis() - startMs > SERVICE_BASIC_MAX_MS)
    {
      return "stopped after 60 s";
    }
    if (millis() - lastWatchMs > SERVICE_WATCH_MS)
    {
      return "stopped: the Service tab was closed";
    }
    if (signalValid(SIG_RPM) && signalGet(SIG_RPM) > 0)
    {
      return "stopped: the engine started";
    }
    if (signalValid(SIG_SPEED) && signalGet(SIG_SPEED) > 2)
    {
      return "stopped: the car moved";
    }
    if (runtime.standby)
    {
      return "stopped: ignition off";
    }
    Kwp1281Group block;
    if (!kwp1281BasicSetting(group, block))
    {
      return "connection lost";
    }
    portENTER_CRITICAL(&statusMux);
    status.reads++;
    status.fieldCount = block.count;
    for (uint8_t field = 0; field < block.count && field < 4; field++)
    {
      status.value[field] = block.fields[field].value;
      copyStr(status.unit[field], sizeof(status.unit[field]), block.fields[field].unit);
      status.formula[field] = block.fields[field].formula;
      status.a[field] = block.fields[field].a;
      status.b[field] = block.fields[field].b;
    }
    portEXIT_CRITICAL(&statusMux);
    if (block.count == 0 && status.reads >= 3)
    {
      return "the module has no basic setting for this group";
    }
    vTaskDelay(pdMS_TO_TICKS(150)); // a few answers a second, as scan tools show them
  }
}

// the group's title and field names from the module's label file
static void loadBasicSettingLabels(uint8_t address, const char *moduleId, uint8_t group)
{
  char candidates[LBL_MAX_CANDS][LBL_NAME_LEN];
  const uint8_t candidateCount = labelsFind(address, moduleId, candidates, LBL_MAX_CANDS);
  if (candidateCount == 0 || !labelsLoad(candidates[0]))
  {
    return; // no label file: the fields keep their numbers
  }
  char title[sizeof(status.title)];
  char names[4][sizeof(status.fieldName[0])];
  labelsField(group, 0, title, sizeof(title), NULL, 0); // field 0 = the group's title
  for (uint8_t field = 0; field < 4; field++)
  {
    names[field][0] = '\0';
    labelsField(group, field + 1, names[field], sizeof(names[field]), NULL, 0);
  }
  portENTER_CRITICAL(&statusMux);
  copyStr(status.title, sizeof(status.title), title);
  for (uint8_t field = 0; field < 4; field++)
  {
    copyStr(status.fieldName[field], sizeof(status.fieldName[field]), names[field]);
  }
  portEXIT_CRITICAL(&statusMux);
}

// the job over KWP1281 (session already open). Returns true if it worked.
static bool kwp1281Job(uint8_t job, uint8_t address, uint8_t group, const char *moduleId)
{
  if (job == SVC_JOB_READ_FAULTS)
  {
    serviceSetState("Reading faults");
    if (!kwp1281ListFaults())
    {
      serviceSetState("Connection lost while reading faults");
      return false;
    }
    serviceSetState("Faults stored: %u", status.faultCount);
    return true;
  }
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Clearing faults");
    bool accepted = false;
    if (!kwp1281ClearFaults(accepted))
    {
      serviceSetState("Connection lost while clearing faults");
      return false;
    }
    if (!accepted)
    {
      serviceSetState("The module refused to clear its faults");
      return false;
    }
    if (!kwp1281ListFaults()) // what's left: a fault that is still there comes straight back
    {
      serviceSetState("Cleared (connection lost before the check read)");
      return true;
    }
    serviceSetState("Cleared - faults still there: %u", status.faultCount);
    return true;
  }
  // basic setting
  loadBasicSettingLabels(address, moduleId, group);
  const char *ending = runBasicSetting(group);
  // back to a normal read of the same group, so the module leaves basic setting mode before the session closes
  Kwp1281Group block;
  kwp1281ReadGroup(group, block);
  serviceSetState("Basic setting group %03u %s after %lu answers", group, ending, (unsigned long)status.reads);
  return strcmp(ending, "connection lost") != 0;
}

// the job over KWP2000 on the K-line (session already open). Returns true if it worked.
static bool kwp2000ListFaults()
{
  const uint8_t request[4] = {0x18, 0x02, 0xFF, 0x00}; // ReadDiagnosticTroubleCodesByStatus, every group
  uint8_t response[250];
  uint8_t responseLen = 0;
  if (!kwp2000Request(request, 4, response, responseLen, 2000, sizeof(response)) || responseLen < 2 ||
      response[0] != 0x58)
  {
    return false;
  }
  serviceClearFaults();
  // 58 <count> then [high low status] per fault; the VAG fault number is high * 256 + low
  for (uint16_t pos = 2; pos + 2 < responseLen; pos += 3)
  {
    char code[12];
    snprintf(code, sizeof(code), "%05u", (unsigned)((response[pos] << 8) | response[pos + 1]));
    serviceAddFault(code, response[pos + 2]);
  }
  return true;
}

static bool kwp2000Job(uint8_t job)
{
  // VAG modules want their diagnostic session (10 89) first; a refusal is fine, the reads are tried anyway
  const uint8_t sessionRequest[2] = {0x10, 0x89};
  uint8_t response[16];
  uint8_t responseLen = 0;
  kwp2000Request(sessionRequest, 2, response, responseLen, 1000, sizeof(response));
  if (job == SVC_JOB_BASIC_SETTING)
  {
    serviceSetState("This module speaks KWP2000: basic settings work on KWP1281 modules only for now");
    return false;
  }
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Clearing faults");
    const uint8_t clearRequest[3] = {0x14, 0xFF, 0x00}; // ClearDiagnosticInformation, every group
    if (!kwp2000Request(clearRequest, 3, response, responseLen, 3000, sizeof(response)) || responseLen < 1 ||
        response[0] != 0x54)
    {
      serviceSetState("The module refused to clear its faults (NRC 0x%02X)", kwp2000LastNrc());
      return false;
    }
  }
  if (!kwp2000ListFaults())
  {
    serviceSetState("The module didn't list its faults (NRC 0x%02X)", kwp2000LastNrc());
    return false;
  }
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Cleared - faults still there: %u", status.faultCount);
  }
  else
  {
    serviceSetState("Faults stored: %u", status.faultCount);
  }
  return true;
}

// ---------------------------------------------------------------------------
// 0.38.0: OBD-II fault codes over the K-line, any make (the Service tab's "OBD-II (any make)", address 0x33): one
// wake-up of the OBD address (fast init, then the 5-baud one), then mode 03 (stored), 07 (pending) and 04 (clear), the
// same as a generic scan tool. Every emissions ECU on the line answers, so the list can hold the engine's and the
// gearbox's codes together.
// ---------------------------------------------------------------------------
#define OBD_FAULT_STORED 0x08  // status given to a stored code (the UDS status bit for "confirmed")
#define OBD_FAULT_PENDING 0x04 // ... and to a pending one (the UDS status bit for "pending")

// wakes the OBD address; true = an ECU answered
static bool obdKlineConnect(uint8_t *keyBytes)
{
  klSetBaud(10400); // ISO 14230-4 / ISO 9141-2 speed
  serviceSetState("Waking the engine for OBD-II (fast init)");
  if (kwp2000Connect(KWP2000_OBD_ADDRESS, true, keyBytes))
  {
    return true;
  }
  serviceSetState("Waking the engine for OBD-II (5-baud init)");
  vTaskDelay(pdMS_TO_TICKS(2500)); // let the line settle after the fast init went unanswered
  klSetBaud(10400);                // the 5-baud wake-up always answers at 10400
  return kwp2000Connect(KWP2000_OBD_ADDRESS, false, keyBytes);
}

// stored codes, then pending ones (a code that is both shows once, as stored). false = no ECU answered mode 03
static bool obdKlineListFaults()
{
  uint16_t stored[SERVICE_MAX_FAULTS];
  uint16_t pending[SERVICE_MAX_FAULTS];
  bool storedAnswered = false;
  bool pendingAnswered = false;
  const uint8_t storedCount = kwp2000ReadObdCodes(0x03, stored, SERVICE_MAX_FAULTS, storedAnswered);
  if (!storedAnswered)
  {
    return false;
  }
  const uint8_t pendingCount = kwp2000ReadObdCodes(0x07, pending, SERVICE_MAX_FAULTS, pendingAnswered);
  serviceClearFaults();
  for (uint8_t i = 0; i < storedCount; i++)
  {
    char code[12];
    serviceDtcText(stored[i] >> 8, stored[i] & 0xFF, code, sizeof(code)); // "P0300"
    serviceAddFault(code, OBD_FAULT_STORED);
  }
  for (uint8_t i = 0; i < pendingCount && serviceFaultCount() < SERVICE_MAX_FAULTS; i++)
  {
    bool alsoStored = false;
    for (uint8_t j = 0; j < storedCount; j++)
    {
      if (stored[j] == pending[i])
      {
        alsoStored = true;
      }
    }
    if (alsoStored)
    {
      continue;
    }
    char code[12];
    serviceDtcText(pending[i] >> 8, pending[i] & 0xFF, code, sizeof(code));
    serviceAddFault(code, OBD_FAULT_PENDING);
  }
  return true;
}

// the job on an open OBD-II session. Returns true if it worked.
static bool obdKlineJob(uint8_t job)
{
  if (job == SVC_JOB_BASIC_SETTING)
  {
    serviceSetState("Basic settings need a VW module's own address, not OBD-II");
    return false;
  }
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Clearing faults");
    if (!kwp2000ClearObdCodes())
    {
      serviceSetState("The engine refused to clear its faults (NRC 0x%02X)", kwp2000LastNrc());
      return false;
    }
  }
  if (!obdKlineListFaults())
  {
    serviceSetState("The engine didn't list its faults (NRC 0x%02X)", kwp2000LastNrc());
    return false;
  }
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Cleared - faults still there: %u", status.faultCount);
  }
  else
  {
    serviceSetState("Faults: %u", status.faultCount);
  }
  return true;
}

// the whole OBD-II job: wake-up, job, end of the session
static void obdKlineRun(uint8_t job)
{
  vTaskDelay(pdMS_TO_TICKS(300)); // W5: the line idle after whatever session had it
  uint8_t keyBytes[2];
  if (!obdKlineConnect(keyBytes))
  {
    serviceSetState("No OBD-II answer on the K-line (fast init or 5-baud to 0x33)");
    serviceFinish(false);
    return;
  }
  char keyText[24];
  snprintf(keyText, sizeof(keyText), "key bytes %02X %02X", keyBytes[0], keyBytes[1]);
  if (kwp2000IsIso9141())
  {
    serviceSetProtocol("OBD-II (ISO 9141-2)", keyText);
  }
  else
  {
    serviceSetProtocol("OBD-II (KWP2000)", keyText);
  }
  const bool ok = obdKlineJob(job);
  kwp2000Disconnect();
  serviceFinish(ok);
}

void serviceKlineRun()
{
  klinePending = false;
  const uint8_t job = status.job;
  const uint8_t address = status.address;
  const uint8_t group = status.group;
  bool ok = false;

  if (address == KWP2000_OBD_ADDRESS)
  {
    obdKlineRun(job); // 0.38.0: OBD-II, any make
    return;
  }

  serviceSetState("Waking module 0x%02X (5-baud init, KWP1281)", address);
  vTaskDelay(pdMS_TO_TICKS(300)); // W5: the line idle after whatever session had it
  char moduleId[48];
  if (kwp1281Connect(address, moduleId, sizeof(moduleId)))
  {
    serviceSetProtocol("KWP1281", moduleId);
    ok = kwp1281Job(job, address, group, moduleId);
    kwp1281Disconnect(); // end-output block: the module closes the session
  }
  else
  {
    // not a KWP1281 module (or no answer): wait for it to drop back to sleep, then wake it for KWP2000
    serviceSetState("Waking module 0x%02X again for KWP2000", address);
    vTaskDelay(pdMS_TO_TICKS(2500));
    uint8_t keyBytes[2];
    if (kwp2000Connect(address, false, keyBytes))
    {
      char keyText[24];
      snprintf(keyText, sizeof(keyText), "key bytes %02X %02X", keyBytes[0], keyBytes[1]);
      serviceSetProtocol("KWP2000 (K-line)", keyText);
      ok = kwp2000Job(job);
      kwp2000Disconnect();
    }
    else
    {
      serviceSetState("No answer from module 0x%02X on the K-line (KWP1281 or KWP2000)", address);
    }
  }
  serviceFinish(ok);
}
