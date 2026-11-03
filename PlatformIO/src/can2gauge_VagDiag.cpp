#include <can2gauge_VagDiag.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_Link.h>    // linkIsMaster(): only the chain master sends requests
#include <can2gauge_UserSig.h> // userSigConfig / userSigPageUses: which gauge pages show a VAG diag value
#include <can2gauge_Tp20.h>    // the TP2.0 client (shared with the DSG and the BLE ELM)
#include <CANLine.h>           // canSend, canOnFrame, canStats
#include <can2gauge_Service.h> // 0.16.0: the Service tab's fault read / clear on CAN runs in this task
#include <can2gauge_DSG.h>     // 0.17.2: dsgCanSessionOpen() - a Service job waits for the DSG to close its channel
#include <can2gauge_ELM.h>     // 0.34.0: elmDiagTransact() - the same requests through the OBD adapter
#include <can2gauge_Vehicle.h> // 0.35.0: the VIN read over the gauge's CAN (First Boot / Your Car ask for it)
#include <can2gauge_CarScan.h> // 0.37.0: Scan My Car's CAN / adapter jobs run in this task

// VAG diag (0.13.0). What a dealer scan tool sends, from two bench CAN captures of a diagnostic session (see the capture notes in docs/):
//  UDS, Gen5 Haldex (address 22, request 0x70F, answer 0x779):
//    70F: 02 10 03 55 55 55 55 55     extended session         -> 779: 06 50 03 00 32 01 F4 (P2 50 ms, P2* 5 s)
//    70F: 03 22 F1 9E 55 55 55 55     ASAM dataset name        -> 779: 10 1C 62 F1 9E "EV_AllWheelContrHA1VW37X"
//    70F: 30 00 01 55 55 55 55 55     the tool's flow control: send it all, 1 ms between frames, padded with 0x55
//    then F1A2 (dataset version), F187 / F189 / F191 / F1A3 / F197 (part number, software, hardware, name)
//    measuring values: one 22 <DID> per request, ~315 ms apart, each answered in 13-20 ms (no Turbo in that log)
//    TesterPresent 3E 80 to 0x700 every ~400 ms. A refused value (7F 22 22 / 7F 22 31) is simply asked again.
//  TP2.0, Mk5 DSG (module 02): 200: 02 C0 00 10 00 03 01 -> 202: 00 D0 00 03 60 07 01, A0 / A1 parameters, 10 89,
//    1A 9B (identification), 31 B8, 1A 91, then 21 01 every ~140 ms, each answered in ~30 ms.
// Turbo asks again as soon as an answer is in; "Group UDS requests" packs several DIDs into
// one 22 request, split into groups of N. Neither is in the captures: here they follow the UDS standard (ISO 14229:
// a 22 request may list several DIDs; the answer lists each DID it supports followed by its data).

#define VDIAG_PAD 0x55             // the capture pads its ISO-TP frames with 0x55
#define VDIAG_ANSWER_MS 150        // no answer this long = lost (the captured answers took 13-60 ms)
#define VDIAG_PENDING_MS 5000      // a 7F xx 78 "response pending" extends the wait (P2* from the 50 03 answer)
#define VDIAG_TESTER_MS 2000       // TesterPresent after this long with no request (the session times out after 5 s)
#define VDIAG_FAILS_MAX 8          // this many lost answers in a row = the session is gone, start again
#define VDIAG_BUFFER 256           // biggest answer kept (a grouped answer of 8 DIDs fits easily)
#define VDIAG_TRIES_MAX 3          // 0.36.8: connects in a row with no answer before the tab stops trying
#define VDIAG_QUIET_MS 2000        // 0.36.8: nothing heard on the gauge's CAN this long = no car (ECU) on the wire

static TaskHandle_t vdiagTask = NULL;          // the VAG diag task
static QueueHandle_t rxQueue = NULL;           // CAN frames from the module, filled by canHook
static volatile uint32_t listenId = 0;         // the CAN id canHook passes on (0 = none), 11 or 29-bit
static volatile bool listenExtended = false;   // 0.34.0: listenId is a 29-bit (extended) id
static bool useAdapter = false;                // 0.34.0: this session / job goes through the OBD adapter, not our CAN
static volatile uint32_t generation = 0;       // bumped by vdiagRestart(): the task starts its session again
static volatile uint16_t probeRequest = 0;     // the tab's "Try a DID" (0 = none waiting)
static volatile bool probeWaiting = false;     // probeRequest holds a DID to try
EXT_RAM_BSS_ATTR static VdiagStatus status;                     // what the web UI shows
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED; // guards the status text (the web server reads it)
static uint8_t answer[VDIAG_BUFFER];           // the last answer (static: too big for the task's stack)
static uint32_t rateWindowMs = 0;              // start of the current one-second sample count
static uint32_t rateCount = 0;                 // answers in the current second
static uint32_t lastRequestMs = 0;             // when the last request went out (TesterPresent timing)
// 0.38.0: Turbo stepping back when the module or the gauge can't keep up, and forward again once it can
#define VDIAG_GROW_AFTER 50                    // grouped answers in a row before the group size steps back up
static bool turboGapNeeded = false;            // a long answer lost a frame with no gap asked for: 1 ms gaps this session
static bool turboGapJustSet = false;           // ... and that happened on the request just made (that loss isn't a group too big)
static uint8_t groupSizeWanted = 4;            // the group size set on the tab (where stepping back up stops)
static uint8_t goodGroups = 0;                 // grouped answers in a row since the group size last changed
static uint8_t failedConnects = 0;             // 0.36.8: connects in a row nobody answered
static bool failedOnQuietBus = false;          // 0.36.8: the last one failed with nothing else heard on the CAN
static uint32_t failedGeneration = 0;          // 0.36.8: the settings / Restart Session the count is for

// ---------------------------------------------------------------------------
// address tables: which CAN ids a module answers on. VW gives each diagnostic address fixed UDS ids; the answer id is
// normally the request id + 0x6A. The engine and gearbox use the OBD-II ids instead. Sources: the captures (0x70F,
// 0x710, 0x7E0), OBDb's VW profiles (0x70E, 0x710, 0x713, 0x714, 0x746, 0x767; 0.34.0: 0x752, 0x773, 0x7E5, 0x7E6,
// 0x744, which OBDb lists with what they carry - the address is the usual one for that kind of module) and the
// OBD-II standard (0x7E0 / 0x7E1). Anything else: type the ids on the tab.
// ---------------------------------------------------------------------------
#define UDS_TABLE_SIZE 16
static const uint8_t udsAddress[UDS_TABLE_SIZE] = {0x01, 0x02, 0x03, 0x08, 0x09, 0x15, 0x17, 0x19,
                                                   0x22, 0x44, 0x75, 0x5F, 0x53, 0x8C, 0x51, 0xC6};
static const uint16_t udsRequestId[UDS_TABLE_SIZE] = {0x7E0, 0x7E1, 0x713, 0x746, 0x70E, 0x715, 0x714, 0x710,
                                                      0x70F, 0x712, 0x767, 0x773, 0x752, 0x7E5, 0x7E6, 0x744};
static const uint16_t udsAnswerId[UDS_TABLE_SIZE] = {0x7E8, 0x7E9, 0x77D, 0x7B0, 0x778, 0x77F, 0x77E, 0x77A,
                                                     0x779, 0x77C, 0x7D1, 0x7DD, 0x7BC, 0x7ED, 0x7EE, 0x7AE};
static const char *udsModuleName[UDS_TABLE_SIZE] = {"Engine", "Gearbox", "Brakes", "Climate", "Central electrics",
                                                    "Airbag", "Instruments", "Gateway", "AWD", "Steering assist",
                                                    "Emergency call", "Infotainment", "Parking brake",
                                                    "Hybrid battery", "Electric drive", "Charger"};

// TP2.0 module ids that differ from the address (capture: the tool opened the gateway as 0x1F, and AWD 22 as 0x0A)
#define TP20_TABLE_SIZE 2
static const uint8_t tp20Address[TP20_TABLE_SIZE] = {0x19, 0x22};
static const uint8_t tp20Module[TP20_TABLE_SIZE] = {0x1F, 0x0A};

// 0.34.0: the answer id that goes with a request id when none is typed in:
//   11-bit  0x7E0..0x7E7 (OBD-II) + 8, VW's other 0x7xx modules + 0x6A
//   29-bit  18 DA <module> F1 (OBD-II) -> 18 DA F1 <module>; VW's 17 FC 00 xx -> 17 FE 00 xx
static uint32_t answerIdFor(uint32_t requestId)
{
  if (requestId <= 0x7FF)
  {
    if (requestId >= 0x7E0 && requestId <= 0x7E7)
    {
      return requestId + 8;
    }
    return requestId + 0x6A; // VW's rule for the 0x7xx diagnostic ids
  }
  if (((requestId >> 16) & 0xFF) == 0xDA)
  {
    const uint32_t priority = requestId & 0x1F000000UL; // the first byte (18)
    const uint32_t module = (requestId >> 8) & 0xFF;    // the third byte: who it's for
    const uint32_t tester = requestId & 0xFF;           // the last byte: who it's from (F1)
    return priority | 0x00DA0000UL | (tester << 8) | module;
  }
  if (((requestId >> 8) & 0xFFFF) == 0xFC00)
  {
    return requestId + 0x00020000UL; // 17 FC 00 xx -> 17 FE 00 xx
  }
  return requestId;
}

// a target from a request / answer id (answer 0 = worked out), with the tab's extended address bytes
static DiagTarget makeTarget(uint32_t requestId, uint32_t answerId, uint8_t txExt, uint8_t rxExt)
{
  DiagTarget target;
  target.requestId = requestId;
  target.answerId = answerId;
  if (answerId == 0)
  {
    target.answerId = answerIdFor(requestId);
  }
  target.is29Bit = requestId > 0x7FF;
  target.txExt = txExt;
  target.rxExt = rxExt;
  return target;
}

// a module address's target from the table: VW's 11-bit ids. Through the adapter on a car that answers OBD on 29-bit
// (a 2025 Tiguan), the engine and gearbox are asked at 18 DA <address> F1, the addresses they answered 01 00 from.
// Not in the table: 0x700 + the address, a guess the tab's help text warns about.
static DiagTarget tableTarget(uint8_t address)
{
  if (useAdapter && elmCar29Bit() && address == 0x01)
  {
    return makeTarget(0x18DA00F1UL | ((uint32_t)elmEngineAddress29() << 8), 0, 0, 0);
  }
  if (useAdapter && elmCar29Bit() && address == 0x02)
  {
    return makeTarget(0x18DA00F1UL | ((uint32_t)elmGearboxAddress29() << 8), 0, 0, 0);
  }
  for (uint8_t i = 0; i < UDS_TABLE_SIZE; i++)
  {
    if (udsAddress[i] == address)
    {
      return makeTarget(udsRequestId[i], udsAnswerId[i], 0, 0);
    }
  }
  return makeTarget(0x700 + address, 0, 0, 0);
}

// the target for the configured module: the tab's own ids first, then the table
static DiagTarget tabTarget()
{
  if (settings.vdiagTxId != 0)
  {
    return makeTarget(settings.vdiagTxId, settings.vdiagRxId, settings.vdiagTxExt, settings.vdiagRxExt);
  }
  return tableTarget(settings.vdiagAddr);
}

static uint8_t tp20ModuleFor(uint8_t address)
{
  if (settings.vdiagTp20Module)
  {
    return settings.vdiagTp20Module;
  }
  for (uint8_t i = 0; i < TP20_TABLE_SIZE; i++)
  {
    if (tp20Address[i] == address)
    {
      return tp20Module[i];
    }
  }
  return address;
}

// ---------------------------------------------------------------------------
// status helpers
// ---------------------------------------------------------------------------
static void setState(const char *state, const char *detail)
{
  portENTER_CRITICAL(&statusMux);
  copyStr(status.state, sizeof(status.state), state);
  copyStr(status.detail, sizeof(status.detail), detail);
  portEXIT_CRITICAL(&statusMux);
}

// one answer arrived: counts towards the samples per second the tab shows
static void countSample()
{
  status.okCount++;
  rateCount++;
  if (millis() - rateWindowMs >= 1000)
  {
    status.samplesPerSec = rateCount;
    rateCount = 0;
    rateWindowMs = millis();
  }
}

// a text identification answer (the DID's data) into a string: printable characters only, spaces trimmed
static void identText(const uint8_t *data, uint16_t len, char *out, size_t outLen)
{
  size_t used = 0;
  for (uint16_t i = 0; i < len && used + 1 < outLen; i++)
  {
    if (data[i] >= 0x20 && data[i] < 0x7F)
    {
      out[used] = (char)data[i];
      used++;
    }
  }
  while (used > 0 && out[used - 1] == ' ')
  {
    used--;
  }
  out[used] = '\0';
}

// ---------------------------------------------------------------------------
// CAN receive: runs in the CAN RX task, so it only copies the module's frames into our queue
// ---------------------------------------------------------------------------
static void canHook(const twai_message_t &message)
{
  if (!rxQueue)
  {
    return;
  }
  const uint32_t wanted = listenId;
  if (wanted && message.identifier == wanted && (bool)message.extd == listenExtended) // 0.34.0: 11 or 29-bit
  {
    xQueueSend(rxQueue, &message, 0);
  }
}

// should the task be talking to the module right now?
static bool wanted()
{
  if (settings.vdiagRun == VDIAG_RUN_ALWAYS)
  {
    return true;
  }
  if (settings.vdiagRun != VDIAG_RUN_SHOWN)
  {
    return false;
  }
  if (runtime.vdiagSeenMs && millis() - runtime.vdiagSeenMs < MODE_RELEASE_MS)
  {
    return true; // the web UI's VAG diag tab is open
  }
  if (runtime.pageSeenMs && millis() - runtime.pageSeenMs < MODE_RELEASE_MS &&
      userSigPageUses(runtime.pageCfgIndex, USRC_DIAG))
  {
    return true; // the page on the glass shows a VAG diag value
  }
  if (userSigRotationUses(USRC_DIAG))
  {
    return true; // 0.15.0: a page in the rotation shows one: keep reading, so it's there when that page is swiped to
  }
  return false;
}

// 0.34.0: the way to the module is there: our CAN is up, or the OBD adapter is connected
static bool linkUp()
{
  if (useAdapter)
  {
    return elmConnected();
  }
  return canStats().up;
}

// 0.36.8: is the gauge's CAN quiet? Nothing heard for 2 s = no car, or no ECU awake, on the wire.
// (An MQB car's OBD port is quiet until a tester asks, so a quiet bus still gets one try.)
static bool canQuiet()
{
  const CanStats &stats = canStats();
  if (stats.lastRxMs == 0)
  {
    return true; // nothing heard since power-up
  }
  return millis() - stats.lastRxMs > VDIAG_QUIET_MS;
}

// 0.36.8: the same ISO 15765 test as the ELM's: ATDPN 6-9, 11, 12 are CAN (0 = not known yet, 1-5 = J1850 / K-line)
static bool adapterProtocolIsCan(uint8_t protocol)
{
  return protocol >= 6 && protocol <= 12 && protocol != 10;
}

// 0.36.8: has the tab given up on this module? After VDIAG_TRIES_MAX connects with no answer, or one on a quiet CAN,
// it stops sending until Restart Session, a settings save, the ignition going off and on, or (quiet CAN) the car
// showing up on the wire. detail gets the line the tab shows.
static bool gaveUp(char *detail, size_t detailLen)
{
  if (failedGeneration != generation)
  {
    failedConnects = 0; // settings saved or Restart Session: start counting again
    failedOnQuietBus = false;
    failedGeneration = generation;
  }
  if (failedConnects == 0)
  {
    return false;
  }
  if (failedOnQuietBus && !useAdapter)
  {
    if (!canQuiet())
    {
      failedConnects = 0; // the car is talking now: try again
      failedOnQuietBus = false;
      return false;
    }
    snprintf(detail, detailLen, "nothing heard on the CAN - waiting for the car");
    return true;
  }
  if (failedConnects >= VDIAG_TRIES_MAX)
  {
    snprintf(detail, detailLen, "no module answered %u times - Restart Session to retry", (unsigned)VDIAG_TRIES_MAX);
    return true;
  }
  return false;
}

static bool running(uint32_t myGeneration)
{
  // a waiting Service tab job, Scan My Car job or VIN read closes the session and takes over
  return generation == myGeneration && linkUp() && linkIsMaster() && !runtime.standby && !runtime.otherScanners && wanted() &&
         !serviceCanPending() && !carScanCanPending() && !vehicleCanReadWanted();
}

// ---------------------------------------------------------------------------
// ISO-TP (ISO 15765-2): requests of any length (a grouped 22 request is longer than one frame), answers of any
// length. 8-byte frames padded with 0x55 as in the capture. 0.34.0: 11-bit or 29-bit ids, and extended addressing (BMW): the
// module's address byte in front of every frame we send, the tester's in front of every frame it sends, so each
// frame carries one byte less.
// ---------------------------------------------------------------------------
static bool sendFrame(const DiagTarget &target, const uint8_t *data, uint8_t len)
{
  uint8_t frame[8];
  memset(frame, VDIAG_PAD, sizeof(frame));
  uint8_t first = 0; // where the ISO-TP bytes start
  if (target.txExt)
  {
    frame[0] = target.txExt; // the module's address
    first = 1;
  }
  if (len > 8 - first)
  {
    len = 8 - first;
  }
  memcpy(&frame[first], data, len);
  return canSend(target.requestId, frame, 8, target.is29Bit);
}

// the ISO-TP bytes a CAN frame carries for this target (7 with extended addressing)
static uint8_t frameRoom(const DiagTarget &target)
{
  if (target.txExt)
  {
    return 7;
  }
  return 8;
}

// waits for the next frame from the module; false after waitMs. The ISO-TP bytes are copied into data (the extended
// address byte left out), their count into dataLen (0 = a frame for someone else, skip it).
static bool nextFrame(const DiagTarget &target, uint8_t *data, uint8_t &dataLen, uint32_t waitMs)
{
  twai_message_t message;
  dataLen = 0;
  if (xQueueReceive(rxQueue, &message, pdMS_TO_TICKS(waitMs)) != pdTRUE)
  {
    return false;
  }
  uint8_t first = 0;
  if (target.rxExt)
  {
    if (message.data_length_code < 2 || message.data[0] != target.rxExt)
    {
      return true; // not for us
    }
    first = 1;
  }
  for (uint8_t i = first; i < message.data_length_code && i < 8; i++)
  {
    data[dataLen] = message.data[i];
    dataLen++;
  }
  return true;
}

// sends a request longer than one frame: first frame, the module's flow control, then the consecutive frames
static bool sendLong(const DiagTarget &target, const uint8_t *request, uint16_t requestLen)
{
  const uint8_t room = frameRoom(target);
  uint8_t frame[8];
  frame[0] = 0x10 | ((requestLen >> 8) & 0x0F); // first frame + length high nibble
  frame[1] = requestLen & 0xFF;                 // length low byte
  memcpy(&frame[2], request, room - 2);         // the first bytes of the request
  if (!sendFrame(target, frame, room))
  {
    return false;
  }
  uint16_t sent = room - 2;
  uint8_t sequence = 1;
  while (sent < requestLen)
  {
    // the module's flow control: 30 = carry on, 31 = wait, 32 = too long for it
    uint8_t data[8];
    uint8_t dataLen = 0;
    uint8_t blockSize = 0;
    uint8_t gapMs = 0;
    bool clear = false;
    uint32_t deadline = millis() + VDIAG_ANSWER_MS * 2;
    while (!clear)
    {
      if ((int32_t)(deadline - millis()) <= 0)
      {
        return false;
      }
      if (!nextFrame(target, data, dataLen, 10) || dataLen < 3)
      {
        continue;
      }
      if (data[0] == 0x30)
      {
        blockSize = data[1]; // frames before the next flow control (0 = all)
        gapMs = data[2];     // gap between frames (STmin, ms; 0xF1..F9 = under 1 ms)
        if (gapMs > 0x7F)
        {
          gapMs = 1;
        }
        clear = true;
      }
      else if (data[0] == 0x31)
      {
        deadline = millis() + VDIAG_ANSWER_MS * 2; // the module asked us to wait
      }
      else if (data[0] == 0x32)
      {
        return false; // overflow: the request is too long for this module
      }
    }
    uint8_t inBlock = 0;
    while (sent < requestLen && (blockSize == 0 || inBlock < blockSize))
    {
      uint16_t chunk = requestLen - sent;
      if (chunk > room - 1)
      {
        chunk = room - 1;
      }
      frame[0] = 0x20 | (sequence & 0x0F); // consecutive frame + sequence number
      memset(&frame[1], VDIAG_PAD, 7);
      memcpy(&frame[1], request + sent, chunk);
      if (gapMs)
      {
        vTaskDelay(pdMS_TO_TICKS(gapMs));
      }
      if (!sendFrame(target, frame, room))
      {
        return false;
      }
      sent += chunk;
      sequence++;
      inBlock++;
    }
  }
  return true;
}

// 0.38.0: a long answer lost frames. In Turbo with no gap asked for, that's the first thing to step back: 1 ms gaps
// for the rest of the session (before the group size is halved for it)
static void turboFramesLost()
{
  if (settings.vdiagTurbo && !turboGapNeeded)
  {
    turboGapNeeded = true;
    turboGapJustSet = true; // udsValues: don't halve the group size for this one
    status.turboGap = true; // the Diagnostics tab shows it
    DEBUG("VAG diag: frames lost with no gap - 1 ms gaps from now on");
  }
}

// one UDS request and its answer over the gauge's own CAN. Fills answer[] with the answer's payload (positive or 7F)
// and answerLen. false = no answer (or a broken one).
static bool udsRequestCanOnTurn(const DiagTarget &target, const uint8_t *request, uint16_t requestLen, uint16_t &answerLen);

// 0.37.9: one request on the gauge's CAN, on our turn (CANLine.h): the OBD polling and the DSG wait meanwhile, so an
// answer from the engine or gearbox is never taken by the wrong asker
static bool udsRequestCan(const DiagTarget &target, const uint8_t *request, uint16_t requestLen, uint16_t &answerLen)
{
  answerLen = 0;
  if (!canDiagWait(CAN_DIAG_VDIAG, 6000, 1000))
  {
    return false; // another asker held the CAN too long: counted as no answer, asked again next round
  }
  const bool ok = udsRequestCanOnTurn(target, request, requestLen, answerLen);
  canDiagRelease(CAN_DIAG_VDIAG);
  return ok;
}

static bool udsRequestCanOnTurn(const DiagTarget &target, const uint8_t *request, uint16_t requestLen, uint16_t &answerLen)
{
  const uint8_t room = frameRoom(target);
  answerLen = 0;
  listenExtended = target.is29Bit;
  listenId = target.answerId;
  xQueueReset(rxQueue);
  lastRequestMs = millis();
  if (requestLen <= room - 1)
  {
    uint8_t frame[8];
    frame[0] = requestLen; // single frame: the length
    memcpy(&frame[1], request, requestLen);
    if (!sendFrame(target, frame, requestLen + 1))
    {
      return false;
    }
  }
  else if (!sendLong(target, request, requestLen))
  {
    return false;
  }
#if detailedDebugVagDiag
  DEBUG("VAG diag -> %lX %02X %02X %02X (%u bytes)", (unsigned long)target.requestId, request[0],
        requestLen > 1 ? request[1] : 0, requestLen > 2 ? request[2] : 0, requestLen);
#endif

  uint32_t deadline = millis() + VDIAG_ANSWER_MS;
  bool longAnswer = false;   // a first frame came: collecting consecutive frames
  uint16_t total = 0;        // the long answer's length
  uint16_t received = 0;     // how much of it has come so far (answerLen stops at the buffer's size)
  uint8_t nextSequence = 1;  // the consecutive frame expected next
  uint8_t data[8];           // one frame's ISO-TP bytes
  uint8_t dataLen = 0;
  while ((int32_t)(deadline - millis()) > 0)
  {
    if (!nextFrame(target, data, dataLen, 5) || dataLen == 0)
    {
      continue;
    }
    const uint8_t frameType = data[0] >> 4; // 0 single, 1 first, 2 consecutive, 3 flow control
    if (frameType == 0 && !longAnswer)
    {
      uint8_t payloadLen = data[0] & 0x0F;
      if (payloadLen == 0 || payloadLen > dataLen - 1)
      {
        continue;
      }
      if (payloadLen >= 3 && data[1] == 0x7F && data[3] == 0x78)
      {
        deadline = millis() + VDIAG_PENDING_MS; // "response pending": the module needs longer
        continue;
      }
      memcpy(answer, &data[1], payloadLen);
      answerLen = payloadLen;
      return true;
    }
    if (frameType == 1 && !longAnswer && dataLen >= 2)
    {
      total = ((data[0] & 0x0F) << 8) | data[1];
      if (total < 7)
      {
        continue; // a first frame always carries more than one frame's worth
      }
      answerLen = dataLen - 2;
      memcpy(answer, &data[2], answerLen);
      received = answerLen;
      longAnswer = true;
      nextSequence = 1;
      // the flow control: carry on, no block limit, and the gap the module leaves between its frames
      uint8_t flowControl[3];
      flowControl[0] = 0x30; // carry on
      flowControl[1] = 0x00; // no block limit: send the rest in one go
      flowControl[2] = 0x01; // 1 ms between frames (as in the capture)
      if (settings.vdiagTurbo && !turboGapNeeded)
      {
        // 0.38.0: Turbo asks for no gap, so a grouped answer (8 DIDs = 5 frames) streams straight through. The
        // RokketGauge (RokketUds, from Kile at Rokketek) reads Simos 18 engines this way on cars. If a frame then goes
        // missing, the rest of the session goes back to 1 ms (turboGapNeeded).
        flowControl[2] = 0x00;
      }
      sendFrame(target, flowControl, 3);
      deadline = millis() + VDIAG_ANSWER_MS;
      continue;
    }
    if (frameType == 2 && longAnswer)
    {
      if ((data[0] & 0x0F) != (nextSequence & 0x0F))
      {
        turboFramesLost(); // 0.38.0: maybe too fast with no gap
        return false;      // a frame went missing
      }
      nextSequence++;
      uint16_t chunk = total - received;
      if (chunk > dataLen - 1)
      {
        chunk = dataLen - 1;
      }
      // keep what fits: a longer answer (a long fault list) is cut short rather than thrown away
      uint16_t kept = chunk;
      if (answerLen + kept > VDIAG_BUFFER)
      {
        kept = VDIAG_BUFFER - answerLen;
      }
      memcpy(answer + answerLen, &data[1], kept);
      answerLen += kept;
      received += chunk;
      if (received >= total)
      {
        return true;
      }
      deadline = millis() + VDIAG_ANSWER_MS;
    }
  }
  if (longAnswer)
  {
    turboFramesLost(); // 0.38.0: the frames stopped half way through
  }
  return false;
}

// one UDS request and its answer, on the gauge's CAN or (0.34.0) through the OBD adapter
static bool udsRequest(const DiagTarget &target, const uint8_t *request, uint16_t requestLen, uint16_t &answerLen)
{
  if (!useAdapter)
  {
    return udsRequestCan(target, request, requestLen, answerLen);
  }
  answerLen = 0;
  lastRequestMs = millis();
  if (requestLen > 7)
  {
    return false; // an ELM327 sends one frame only (the Turbo groups are kept small enough on the adapter)
  }
  // the adapter's own wait for an answer (ATST) decides "no answer"; this is the most the whole job may take
  return elmDiagTransact(target, request, (uint8_t)requestLen, answer, VDIAG_BUFFER, answerLen, 2500);
}

// how many DIDs fit one grouped request: through the adapter one frame (22 + 3 DIDs = 7 bytes, 2 with extended
// addressing); on our own CAN up to 8 (longer requests go as several frames)
static uint8_t groupLimit(const DiagTarget &target)
{
  if (!useAdapter)
  {
    return 8;
  }
  if (target.txExt)
  {
    return 2;
  }
  return 3;
}

// ---------------------------------------------------------------------------
// values
// ---------------------------------------------------------------------------

// every gauge page value that shows VAG diag value number valueIndex + 1 gets the new value
static void publish(uint8_t valueIndex, float value)
{
  for (uint8_t userIndex = 0; userIndex < USER_SIGS_ALL; userIndex++)
  {
    const signalId_t id = (signalId_t)(SIG_USER_1 + userIndex);
    const UserSigCfg *userSig = userSigConfig(id);
    if (userSig && userSig->source == USRC_DIAG && userSig->diagValue == valueIndex + 1) // 0.37.0: was ->pid (never matched)
    {
      signalSet(id, value);
    }
  }
}

// a KWP value with no unit typed on the tab: its gauge pages show the unit of the block's formula byte
static void publishUnit(uint8_t valueIndex, const char *unit)
{
  for (uint8_t userIndex = 0; userIndex < USER_SIGS_ALL; userIndex++)
  {
    const signalId_t id = (signalId_t)(SIG_USER_1 + userIndex);
    const UserSigCfg *userSig = userSigConfig(id);
    if (userSig && userSig->source == USRC_DIAG && userSig->diagValue == valueIndex + 1) // 0.37.0: was ->pid
    {
      signalSetLabel(id, NULL, unit); // NULL name = keep the name
    }
  }
}

// a UDS value out of its DID's data, then x scale + offset. 0.34.0: any run of 1..32 bits, the way OBDb describes its
// values: bitPos counts from the top bit of the first data byte (bit 0 = 0x80 of byte 0, bit 8 = 0x80 of byte 1), and
// the bits are read from there, most significant first. With VDIAG_FLAG_LSB_FIRST a value longer than a byte has its
// bytes the other way round (least significant first, "Intel" order), so they are turned round before reading.
static bool udsValue(const VdiagValue &config, const uint8_t *data, uint16_t dataLen, float &out)
{
  uint8_t bitLen = config.bitLen;
  if (bitLen < 1 || bitLen > 32)
  {
    bitLen = 8;
  }
  // worked out in 32 bits: a bit position near 65535 must not wrap round past the length check below
  const uint32_t firstBit = config.bitPos;        // the value's first bit
  const uint32_t lastBit = firstBit + bitLen - 1; // the value's last bit
  if (lastBit / 8 >= dataLen)
  {
    return false; // the DID is shorter than the setting says
  }
  const uint16_t firstByte = firstBit / 8;                // the byte the value starts in
  const uint16_t byteCount = lastBit / 8 - firstByte + 1; // the bytes it touches (1..5)
  // the bytes the value is in, turned round for an LSB-first value of more than a byte
  uint8_t bytes[5];
  for (uint16_t i = 0; i < byteCount; i++)
  {
    if ((config.flags & VDIAG_FLAG_LSB_FIRST) && bitLen > 8)
    {
      bytes[i] = data[firstByte + byteCount - 1 - i]; // last byte first
    }
    else
    {
      bytes[i] = data[firstByte + i];
    }
  }
  // the bits one by one, most significant first
  uint32_t raw = 0;
  for (uint32_t bit = firstBit; bit <= lastBit; bit++)
  {
    const uint16_t byteIndex = bit / 8 - firstByte;
    const uint8_t bitInByte = 7 - (bit % 8); // bit 0 of a byte is its top bit (0x80)
    raw = raw << 1;
    if (bytes[byteIndex] & (1 << bitInByte))
    {
      raw = raw | 1;
    }
  }
  float number = (float)raw;
  if (config.flags & VDIAG_FLAG_SIGNED)
  {
    const uint32_t topBit = 1UL << (bitLen - 1); // the sign bit of a value this many bits long
    if (raw & topBit)
    {
      number = (float)raw - (float)topBit * 2.0f; // two's complement: subtract 2^bits
    }
  }
  out = number * config.scale + config.offset;
  return true;
}

// the DID's answer data for value valueIndex arrived (UDS)
static void udsGotData(uint8_t valueIndex, const uint8_t *data, uint16_t dataLen)
{
  const VdiagValue &config = settings.vdiagValues[valueIndex];
  VdiagValueStatus &valueStatus = status.values[valueIndex];
  valueStatus.didLen = (uint8_t)dataLen;
  if (dataLen > sizeof(valueStatus.raw))
  {
    valueStatus.rawLen = sizeof(valueStatus.raw); // the tab shows the first 8 bytes
  }
  else
  {
    valueStatus.rawLen = (uint8_t)dataLen;
  }
  memcpy(valueStatus.raw, data, valueStatus.rawLen);
  float value;
  if (udsValue(config, data, dataLen, value))
  {
    valueStatus.value = value;
    valueStatus.valid = true;
    valueStatus.lastNrc = 0;
    publish(valueIndex, value);
  }
}

// a refused read: NRC 0x31 (out of range) / 0x11 / 0x12 / 0x33 never change, so that value stops being asked for;
// 0x22 (conditions not correct, e.g. engine not running) is asked again, as the capture does
static void udsRefused(uint8_t valueIndex, uint8_t nrc)
{
  status.values[valueIndex].lastNrc = nrc;
  status.errCount++;
  if (nrc == 0x31 || nrc == 0x11 || nrc == 0x12 || nrc == 0x33)
  {
    status.values[valueIndex].supported = false;
  }
}

// reads one value's DID on its own (normal mode, and to learn the DID's length before grouping it)
static bool udsReadOne(const DiagTarget &target, uint8_t valueIndex)
{
  const uint16_t did = settings.vdiagValues[valueIndex].id;
  uint8_t request[3];
  request[0] = 0x22;       // ReadDataByIdentifier
  request[1] = did >> 8;   // DID high byte
  request[2] = did & 0xFF; // DID low byte
  uint16_t answerLen = 0;
  if (!udsRequest(target, request, 3, answerLen))
  {
    status.errCount++;
    return false;
  }
  if (answerLen >= 3 && answer[0] == 0x7F)
  {
    udsRefused(valueIndex, answer[2]);
    return true; // it answered: the session is fine
  }
  if (answerLen < 3 || answer[0] != 0x62 || answer[1] != request[1] || answer[2] != request[2])
  {
    status.errCount++;
    return false;
  }
  countSample();
  udsGotData(valueIndex, &answer[3], answerLen - 3);
  return true;
}

// 0.37.6: one OBD PID asked of the value's module (01 <pid> -> 41 <pid> <data>), decoded with the gauge's OBD table -
// the gearbox's (or any ECU's) own PIDs, which the engine's OBD polling never asks
static bool obdReadOne(const DiagTarget &target, uint8_t valueIndex)
{
  const uint8_t pid = (uint8_t)settings.vdiagValues[valueIndex].id;
  uint8_t request[2];
  request[0] = 0x01; // OBD mode 01: current data
  request[1] = pid;  // the PID
  uint16_t answerLen = 0;
  if (!udsRequest(target, request, 2, answerLen))
  {
    status.errCount++;
    return false;
  }
  if (answerLen >= 3 && answer[0] == 0x7F)
  {
    udsRefused(valueIndex, answer[2]);
    return true; // it answered: the module is fine
  }
  if (answerLen < 3 || answer[0] != 0x41 || answer[1] != pid)
  {
    status.errCount++;
    return false;
  }
  countSample();
  VdiagValueStatus &valueStatus = status.values[valueIndex];
  uint16_t kept = answerLen - 2;
  if (kept > sizeof(valueStatus.raw))
  {
    kept = sizeof(valueStatus.raw); // the tab shows the first 8 bytes
  }
  memcpy(valueStatus.raw, &answer[2], kept);
  valueStatus.rawLen = (uint8_t)kept;
  float value;
  if (obdPidValueOf(pid, &answer[2], answerLen - 2, value))
  {
    valueStatus.value = value;
    valueStatus.valid = true;
    valueStatus.lastNrc = 0;
    publish(valueIndex, value);
  }
  else
  {
    valueStatus.supported = false; // a PID the gauge can't decode: stop asking
  }
  return true;
}

// reads several values in one request: 22 D1hi D1lo D2hi D2lo ... -> 62 D1hi D1lo <data1> D2hi D2lo <data2> ...
// Every DID must have been read once on its own (its length is how the answer is split). A DID the module doesn't
// support is left out of its answer, so each DID is looked up by its echo. False = refused / lost (caller halves the
// group size).
static bool udsReadGroup(const DiagTarget &target, const uint8_t *valueIndexes, uint8_t count)
{
  uint8_t request[1 + 2 * 8];
  request[0] = 0x22; // ReadDataByIdentifier, several DIDs
  uint8_t didCount = 0; // DIDs in the request: two values from the same DID ask for it once
  for (uint8_t i = 0; i < count && didCount < 8; i++)
  {
    const uint16_t did = settings.vdiagValues[valueIndexes[i]].id;
    bool already = false;
    for (uint8_t j = 0; j < i; j++)
    {
      if (settings.vdiagValues[valueIndexes[j]].id == did)
      {
        already = true;
      }
    }
    if (!already)
    {
      request[1 + didCount * 2] = did >> 8;   // DID high byte
      request[2 + didCount * 2] = did & 0xFF; // DID low byte
      didCount++;
    }
  }
  uint16_t answerLen = 0;
  if (!udsRequest(target, request, 1 + didCount * 2, answerLen))
  {
    status.errCount++;
    return false;
  }
  if (answerLen >= 3 && answer[0] == 0x7F && answer[2] == 0x22)
  {
    // 0.38.0: 7F 22 22 "conditions not correct" (engine not running ...) says nothing about the group's size: keep
    // it (before, every one halved it, and a Simos 18 with the key on and the engine off ended up asked one DID at a time)
    status.errCount++;
    return true; // it answered: the session is fine
  }
  if (answerLen < 1 || answer[0] != 0x62)
  {
    status.errCount++;
    return false; // refused as a group (too long for it, or one DID it won't mix): the caller tries smaller groups
  }
  countSample();
  uint16_t at = 1; // the first DID echo
  while (at + 2 <= answerLen)
  {
    const uint16_t did = (answer[at] << 8) | answer[at + 1];
    int8_t found = -1;
    for (uint8_t i = 0; i < count && found < 0; i++)
    {
      if (settings.vdiagValues[valueIndexes[i]].id == did)
      {
        found = (int8_t)i;
      }
    }
    if (found < 0)
    {
      return true; // a DID we didn't ask for: stop splitting (the values so far are good)
    }
    const uint8_t valueIndex = valueIndexes[found];
    const uint8_t dataLen = status.values[valueIndex].didLen;
    if (at + 2 + dataLen > answerLen)
    {
      return true; // shorter than learnt: stop here
    }
    // every value that reads this same DID takes its bytes from this one piece of data
    for (uint8_t i = 0; i < count; i++)
    {
      if (settings.vdiagValues[valueIndexes[i]].id == did)
      {
        udsGotData(valueIndexes[i], &answer[at + 2], dataLen);
      }
    }
    at += 2 + dataLen;
  }
  return true;
}

// the identification the capture reads after 10 03 (capture order: F19E, F1A2, F187, F189, F191, ..., F197)
static void udsIdentify(const DiagTarget &target)
{
  const uint16_t dids[5] = {0xF19E, 0xF1A2, 0xF187, 0xF189, 0xF197};
  for (uint8_t i = 0; i < 5; i++)
  {
    uint8_t request[3];
    request[0] = 0x22;            // ReadDataByIdentifier
    request[1] = dids[i] >> 8;    // DID high byte
    request[2] = dids[i] & 0xFF;  // DID low byte
    uint16_t answerLen = 0;
    if (!udsRequest(target, request, 3, answerLen) || answerLen < 4 || answer[0] != 0x62)
    {
      continue; // not every module has every one
    }
    portENTER_CRITICAL(&statusMux);
    if (dids[i] == 0xF19E)
    {
      identText(&answer[3], answerLen - 3, status.asamDataset, sizeof(status.asamDataset));
    }
    else if (dids[i] == 0xF1A2)
    {
      identText(&answer[3], answerLen - 3, status.asamVersion, sizeof(status.asamVersion));
    }
    else if (dids[i] == 0xF187)
    {
      identText(&answer[3], answerLen - 3, status.partNumber, sizeof(status.partNumber));
    }
    else if (dids[i] == 0xF189)
    {
      identText(&answer[3], answerLen - 3, status.swVersion, sizeof(status.swVersion));
    }
    else
    {
      identText(&answer[3], answerLen - 3, status.component, sizeof(status.component));
    }
    portEXIT_CRITICAL(&statusMux);
  }
}

// the tab's "Try a DID": one read, the raw answer kept for the tab
static void udsProbe(const DiagTarget &target, uint16_t did)
{
  uint8_t request[3];
  request[0] = 0x22;       // ReadDataByIdentifier
  request[1] = did >> 8;   // DID high byte
  request[2] = did & 0xFF; // DID low byte
  uint16_t answerLen = 0;
  const bool got = udsRequest(target, request, 3, answerLen);
  portENTER_CRITICAL(&statusMux);
  status.probeId = did;
  status.probeLen = 0;
  if (!got)
  {
    copyStr(status.probeText, sizeof(status.probeText), "no answer");
  }
  else if (answerLen >= 3 && answer[0] == 0x7F)
  {
    snprintf(status.probeText, sizeof(status.probeText), "refused: NRC 0x%02X", answer[2]);
  }
  else if (answerLen >= 3 && answer[0] == 0x62)
  {
    uint16_t dataLen = answerLen - 3;
    if (dataLen > sizeof(status.probe))
    {
      dataLen = sizeof(status.probe);
    }
    memcpy(status.probe, &answer[3], dataLen);
    status.probeLen = (uint8_t)dataLen;
    identText(&answer[3], answerLen - 3, status.probeText, sizeof(status.probeText)); // in case it is text
  }
  else
  {
    copyStr(status.probeText, sizeof(status.probeText), "unexpected answer");
  }
  portEXIT_CRITICAL(&statusMux);
}

// ---------------------------------------------------------------------------
// the UDS session: 10 03 (or 10 01), identification, then the values until something changes
// ---------------------------------------------------------------------------
static bool udsOpen(const DiagTarget &target)
{
  uint8_t request[2];
  request[0] = 0x10;                  // DiagnosticSessionControl
  request[1] = settings.vdiagSession; // 0x03 extended (as the capture) or 0x01 default
  uint16_t answerLen = 0;
  if (!udsRequest(target, request, 2, answerLen))
  {
    return false;
  }
  return answerLen >= 2 && answer[0] == 0x50;
}

// ---------------------------------------------------------------------------
// TP2.0 (KWP2000): the shared Tp20Client, on the gauge's own CAN
// ---------------------------------------------------------------------------
// Tp20Client (can2gauge_Tp20.cpp) talks through a "DiagLink": an object with send / recv / listen / flush. It was written
// that way so the same client also runs through the Bluetooth ELM327. This small class is that link on the gauge's own
// CAN. (A class is needed here only because Tp20Client takes one; `override` just marks the four functions it supplies.)
class VdiagCanLink : public DiagLink
{
public:
  bool send(uint16_t id, const uint8_t *data, uint8_t len) override
  {
    return canSend(id, data, len); // TP2.0 frames go out at their real length, as in the capture
  }

  bool recv(DiagFrame &frame, uint32_t waitMs) override
  {
    twai_message_t message;
    if (xQueueReceive(rxQueue, &message, pdMS_TO_TICKS(waitMs)) != pdTRUE)
    {
      return false;
    }
    frame.id = message.identifier;
    frame.len = message.data_length_code;
    if (frame.len > 8)
    {
      frame.len = 8;
    }
    memcpy(frame.data, message.data, frame.len);
    return true;
  }

  void listen(uint16_t id) override
  {
    listenExtended = false; // TP2.0 runs on 11-bit ids
    listenId = id;
  }

  void flush() override
  {
    xQueueReset(rxQueue);
  }
};
static VdiagCanLink canLink;     // the link on the gauge's CAN
static Tp20Client tp20(canLink); // a second TP2.0 client (the DSG has its own): only one channel per module, so don't
                                 // point both at the same module

static bool tp20Open(uint8_t module)
{
  if (!tp20.open(module))
  {
    return false;
  }
  const uint8_t sessionRequest[2] = {0x10, 0x89}; // StartDiagnosticSession 0x89, as in the capture
  uint16_t answerLen = 0;
  if (!tp20.transact(sessionRequest, 2, answer, VDIAG_BUFFER, answerLen, 1000) || answerLen < 1)
  {
    tp20.close(); // no answer at all: the channel isn't usable
    return false;
  }
  // 0.17.2: a refusal (7F 10 xx) is not the end: many modules (MED9, EDC17, MED17 ...) read their blocks and faults in
  // the session they're already in, with no session request at all. Before, the Service tab then
  // said "answered neither UDS nor TP2.0" although the channel was open.
  return true;
}

// 1A 9B -> 5A 9B "02E300050H  1401" ... "GSG DSG   081": the part number, then the longest text in the rest
static void tp20Identify()
{
  // forget the last module's identification first, so a module that won't say carries no one else's
  portENTER_CRITICAL(&statusMux);
  status.partNumber[0] = '\0';
  status.component[0] = '\0';
  portEXIT_CRITICAL(&statusMux);
  const uint8_t identRequest[2] = {0x1A, 0x9B}; // ReadEcuIdentification 0x9B
  uint16_t answerLen = 0;
  if (!tp20.transact(identRequest, 2, answer, VDIAG_BUFFER, answerLen, 1000) || answerLen < 14 || answer[0] != 0x5A)
  {
    return;
  }
  portENTER_CRITICAL(&statusMux);
  identText(&answer[2], 12, status.partNumber, sizeof(status.partNumber));
  uint16_t bestStart = 0;
  uint16_t bestLen = 0;
  uint16_t runStart = 14;
  for (uint16_t i = 14; i <= answerLen; i++)
  {
    const bool printable = i < answerLen && answer[i] >= 0x20 && answer[i] < 0x7F;
    if (!printable)
    {
      if (i - runStart > bestLen)
      {
        bestStart = runStart;
        bestLen = i - runStart;
      }
      runStart = i + 1;
    }
  }
  if (bestLen > 0)
  {
    identText(&answer[bestStart], bestLen, status.component, sizeof(status.component));
  }
  portEXIT_CRITICAL(&statusMux);
}

static void clearSession(); // below: forgets the session's identification and values

// ---------------------------------------------------------------------------
// 0.37.0: reading the values. They can come from several modules now (each value's own ids or address, else the
// Diagnostics tab's module), and only the values something uses are asked for. One module works as before: its
// session stays open and the values are read round and round. Several modules are visited in turn, each asked for its
// values once per round.
// ---------------------------------------------------------------------------
#define VDIAG_MODULES_MAX 8     // different modules read at once
#define VDIAG_SESSION_MS 4000   // a UDS module not asked for this long gets its session opened again (it ends after 5 s)

// one module the values come from, and how talking to it is going
struct PollModule
{
  bool isKwp;            // KWP2000 measuring blocks over TP2.0 (false = UDS)
  bool isTab;            // the Diagnostics tab's own module: its identification and Try a DID run here
  bool tryBoth;          // the tab's module with Protocol Auto and no values yet: UDS first, then TP2.0
  bool hasUds;           // 0.37.6: it has UDS values (or is the tab's module): opened with 10 03 first. A module with
                         // only OBD PIDs is asked straight away (mode 01 works in the default session)
  DiagTarget target;     // UDS: its ids
  uint8_t tp20Module;    // TP2.0: its module id for the channel setup on 0x200
  bool viaDsg;           // 0.37.10: TP2.0 module the DSG already reads (the gearbox): its blocks come through the DSG's
                         // channel (dsgTp20Read), as a module allows only one channel
  bool open;             // UDS: the session is open; TP2.0: the channel is open
  bool identified;       // the tab's module: its identification has been read
  uint8_t failedOpens;   // opens in a row that got no answer (VDIAG_TRIES_MAX = left alone until the list changes)
  uint8_t lostAnswers;   // answers lost in a row (VDIAG_FAILS_MAX = the session is opened again)
  uint32_t lastAskedMs;  // UDS: when it was last asked
};

static PollModule modules[VDIAG_MODULES_MAX]; // the modules in use now
static uint8_t moduleCount = 0;               // how many of them
static int8_t valueModule[VDIAG_VALUES];      // which of modules[] each value comes from, -1 = not read now

// the TP2.0 module id for a VAG address (the table's exceptions, else the address itself)
static uint8_t tp20TableModule(uint8_t address)
{
  for (uint8_t i = 0; i < TP20_TABLE_SIZE; i++)
  {
    if (tp20Address[i] == address)
    {
      return tp20Module[i];
    }
  }
  return address;
}

// the UDS ids a value is asked on: its own ids, else its address's ids, else the tab's module
static DiagTarget valueTarget(const VdiagValue &config)
{
  if (config.txId != 0)
  {
    return makeTarget(config.txId, config.rxId, config.txExt, config.rxExt);
  }
  if (config.addr != 0)
  {
    return tableTarget(config.addr);
  }
  return tabTarget();
}

// the TP2.0 module a KWP value's block is read from: its address's, else the tab's module
static uint8_t valueTp20Module(const VdiagValue &config)
{
  if (config.addr != 0)
  {
    return tp20TableModule(config.addr);
  }
  return tp20ModuleFor(settings.vdiagAddr);
}

// is the web UI's Diagnostics tab open? (then every value is read, and the tab's module is talked to)
static bool tabOpen()
{
  return runtime.vdiagSeenMs && millis() - runtime.vdiagSeenMs < MODE_RELEASE_MS;
}

// the values to read now, as bits (bit n = value n): set up, and used by a page / trigger (or the tab is open, or
// Requests = Always). Refused values stay in: they are skipped when read, so a refusal doesn't start the list again.
static uint32_t wantedValues()
{
  uint32_t mask = 0;
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    if (settings.vdiagValues[i].kind == VDIAG_KIND_NONE)
    {
      continue;
    }
    if (settings.vdiagRun == VDIAG_RUN_ALWAYS || tabOpen() || userSigDiagValueWanted(i))
    {
      mask = mask | (1UL << i);
    }
  }
  return mask;
}

static bool sameUdsTarget(const DiagTarget &first, const DiagTarget &second)
{
  return first.requestId == second.requestId && first.answerId == second.answerId && first.txExt == second.txExt;
}

// adds a module (or finds it already there); returns its index, -1 = no room
static int8_t addModule(bool isKwp, const DiagTarget &target, uint8_t tp20Module)
{
  for (uint8_t m = 0; m < moduleCount; m++)
  {
    if (modules[m].isKwp != isKwp)
    {
      continue;
    }
    if (isKwp && modules[m].tp20Module == tp20Module)
    {
      return (int8_t)m;
    }
    if (!isKwp && sameUdsTarget(modules[m].target, target))
    {
      return (int8_t)m;
    }
  }
  if (moduleCount >= VDIAG_MODULES_MAX)
  {
    return -1;
  }
  PollModule &module = modules[moduleCount];
  memset(&module, 0, sizeof(module));
  module.isKwp = isKwp;
  module.target = target;
  module.tp20Module = tp20Module;
  moduleCount++;
  return (int8_t)(moduleCount - 1);
}

// the list of modules for these values (and the tab's module while the tab is open)
static void buildModules(uint32_t mask)
{
  moduleCount = 0;
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    valueModule[i] = -1;
    if (!(mask & (1UL << i)))
    {
      continue;
    }
    const VdiagValue &config = settings.vdiagValues[i];
    const bool isKwp = config.kind == VDIAG_KIND_KWP;
    if (isKwp && useAdapter)
    {
      continue; // TP2.0 runs on the gauge's own CAN only
    }
    valueModule[i] = addModule(isKwp, valueTarget(config), valueTp20Module(config));
  }
  // the tab's own module: identification and Try a DID. While the tab is open it is talked to even with no values.
  const DiagTarget tab = tabTarget();
  const uint8_t tabTp20 = tp20ModuleFor(settings.vdiagAddr);
  bool tabFound = false;
  for (uint8_t m = 0; m < moduleCount; m++)
  {
    if ((!modules[m].isKwp && sameUdsTarget(modules[m].target, tab)) ||
        (modules[m].isKwp && modules[m].tp20Module == tabTp20))
    {
      modules[m].isTab = true;
      modules[m].hasUds = true;
      tabFound = true;
    }
  }
  // 0.37.6: which modules need a UDS session (a gearbox with only OBD PIDs doesn't)
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    if (valueModule[i] >= 0 && settings.vdiagValues[i].kind == VDIAG_KIND_UDS)
    {
      modules[valueModule[i]].hasUds = true;
    }
  }
  if (!tabFound && tabOpen())
  {
    const bool isKwp = settings.vdiagProtocol == VDIAG_PROTO_TP20 && !useAdapter;
    const int8_t m = addModule(isKwp, tab, tabTp20);
    if (m >= 0)
    {
      modules[m].isTab = true;
      modules[m].hasUds = true; // the tab's module: identification and Try a DID need the session
      modules[m].tryBoth = settings.vdiagProtocol == VDIAG_PROTO_AUTO && !useAdapter;
    }
  }
  // 0.37.10: a TP2.0 module the DSG reads too borrows the DSG's channel
  for (uint8_t m = 0; m < moduleCount; m++)
  {
    if (modules[m].isKwp && dsgTp20Shares(modules[m].tp20Module))
    {
      modules[m].viaDsg = true;
    }
  }
}

// opens a module's UDS session or TP2.0 channel; false = no answer
static bool openModule(PollModule &module)
{
  if (!module.isKwp && !module.hasUds)
  {
    module.open = true; // 0.37.6: only OBD PIDs: no session to open
    module.lastAskedMs = millis();
    return true;
  }
  if (!module.isKwp)
  {
    if (udsOpen(module.target))
    {
      module.open = true;
      module.lastAskedMs = millis();
      return true;
    }
    if (!module.tryBoth)
    {
      return false;
    }
    module.isKwp = true; // Auto: no UDS answer, so TP2.0 (PQ era)
  }
  if (dsgTp20Shares(module.tp20Module))
  {
    module.viaDsg = true; // 0.37.10: the DSG has this module's channel: nothing to open, its blocks come through it
    module.open = true;
    return true;
  }
  if (tp20.isOpen())
  {
    tp20.close(); // one TP2.0 channel at a time
    // whichever module had it is closed now too, so it opens its own channel again on its next visit
    for (uint8_t m = 0; m < moduleCount; m++)
    {
      if (&modules[m] != &module && modules[m].isKwp && !modules[m].viaDsg)
      {
        modules[m].open = false;
      }
    }
  }
  if (tp20Open(module.tp20Module))
  {
    module.open = true;
    return true;
  }
  if (module.tryBoth)
  {
    module.isKwp = false; // Auto: next time UDS first again
  }
  return false;
}

// 0.38.0: Turbo, a good answer: after VDIAG_GROW_AFTER in a row the group size steps back up (1, 2, 4, 8), up to the
// size set on the tab, so one bad moment (cranking, a busy CAN) doesn't leave it slow until the next session
static void turboAnswered()
{
  if (status.groupSize >= groupSizeWanted)
  {
    return; // already as big as it's allowed
  }
  goodGroups++;
  if (goodGroups >= VDIAG_GROW_AFTER)
  {
    status.groupSize = status.groupSize * 2;
    if (status.groupSize > groupSizeWanted)
    {
      status.groupSize = groupSizeWanted;
    }
    goodGroups = 0;
    DEBUG("VAG diag: %u good answers - groups of %u again", VDIAG_GROW_AFTER, status.groupSize);
  }
}

// 0.38.0: Turbo, one grouped request and what its answer means for the group size. Refused or lost: smaller groups
// from now on, unless the loss was frames with no gap (turboFramesLost has stepped back the gap instead)
static void turboReadGroup(PollModule &module, const uint8_t *group, uint8_t groupCount)
{
  turboGapJustSet = false;
  if (udsReadGroup(module.target, group, groupCount))
  {
    module.lostAnswers = 0;
    turboAnswered();
    return;
  }
  if (status.groupSize > 1 && !turboGapJustSet)
  {
    status.groupSize = status.groupSize / 2;
  }
  goodGroups = 0;
  module.lostAnswers++;
}

// a UDS module's values, once round: one DID per request (normal mode) or grouped (Turbo)
static void udsValues(PollModule &module, const uint8_t *allRead, uint8_t allCount)
{
  // 0.37.6: OBD PIDs one per request; the UDS DIDs (below) as before
  uint8_t toRead[VDIAG_VALUES];
  uint8_t toReadCount = 0;
  for (uint8_t i = 0; i < allCount; i++)
  {
    const uint8_t valueIndex = allRead[i];
    if (settings.vdiagValues[valueIndex].kind != VDIAG_KIND_OBD)
    {
      toRead[toReadCount] = valueIndex;
      toReadCount++;
      continue;
    }
    if (obdReadOne(module.target, valueIndex))
    {
      module.lostAnswers = 0;
    }
    else
    {
      module.lostAnswers++;
    }
    if (!settings.vdiagTurbo)
    {
      vTaskDelay(pdMS_TO_TICKS(settings.vdiagPollMs));
    }
  }
  uint8_t groupSize = status.groupSize;
  if (groupSize < 1)
  {
    groupSize = 1;
  }
  if (groupSize > groupLimit(module.target))
  {
    groupSize = groupLimit(module.target); // through the adapter a grouped request must fit one frame
  }
  if (!settings.vdiagTurbo)
  {
    // normal mode: one DID per request, a gap after each (the capture without its fast mode: ~315 ms per value)
    for (uint8_t i = 0; i < toReadCount && module.open; i++)
    {
      if (udsReadOne(module.target, toRead[i]))
      {
        module.lostAnswers = 0;
      }
      else
      {
        module.lostAnswers++;
      }
      vTaskDelay(pdMS_TO_TICKS(settings.vdiagPollMs));
    }
    return;
  }
  // Turbo: DIDs not read on their own yet go singly first (that is how their length is learnt), the rest in groups
  // of groupSize, each request straight after the last answer
  uint8_t group[8];
  uint8_t groupCount = 0;
  for (uint8_t i = 0; i < toReadCount; i++)
  {
    const uint8_t valueIndex = toRead[i];
    if (status.values[valueIndex].didLen == 0 || groupSize == 1)
    {
      if (udsReadOne(module.target, valueIndex))
      {
        module.lostAnswers = 0;
        turboAnswered(); // 0.38.0: good answers at a group size of 1 count too, or it could never grow again
      }
      else
      {
        module.lostAnswers++;
        goodGroups = 0;
      }
      continue;
    }
    bool sameDid = false; // two values from one DID ride in one place of the request
    for (uint8_t j = 0; j < groupCount; j++)
    {
      if (settings.vdiagValues[group[j]].id == settings.vdiagValues[valueIndex].id)
      {
        sameDid = true;
      }
    }
    group[groupCount] = valueIndex;
    groupCount++;
    if (sameDid)
    {
      groupSize++; // it costs no room in the request
    }
    if (groupCount >= groupSize || groupCount >= 8)
    {
      turboReadGroup(module, group, groupCount); // 0.38.0: halves or grows the group size as it goes
      groupCount = 0;
      groupSize = status.groupSize;
      if (groupSize > groupLimit(module.target))
      {
        groupSize = groupLimit(module.target); // 0.38.0: through the adapter still one frame (was lost here)
      }
    }
  }
  if (groupCount > 0)
  {
    turboReadGroup(module, group, groupCount); // 0.38.0: the last, part-full group counts too (it didn't halve before)
  }
}

// a TP2.0 module's values, once round: each block once, however many values come from it
static void tp20Values(PollModule &module, const uint8_t *toRead, uint8_t toReadCount)
{
  uint8_t blocks[VDIAG_VALUES];
  uint8_t blockCount = 0;
  for (uint8_t i = 0; i < toReadCount; i++)
  {
    const VdiagValue &config = settings.vdiagValues[toRead[i]];
    if (config.id < 1 || config.id > 255)
    {
      continue;
    }
    bool seen = false;
    for (uint8_t j = 0; j < blockCount; j++)
    {
      if (blocks[j] == config.id)
      {
        seen = true;
      }
    }
    if (!seen)
    {
      blocks[blockCount] = (uint8_t)config.id;
      blockCount++;
    }
  }
  // 0.37.10: an answer the DSG got for the same block within this long is used as it is (no second request)
  const uint32_t dsgAnswerMaxAgeMs = 2UL * settings.dsgPollMs + 300;
  for (uint8_t b = 0; b < blockCount; b++)
  {
    if (!module.viaDsg && !tp20.isOpen())
    {
      break; // our channel dropped
    }
    DsgField fields[4];
    uint8_t fieldCount = 0;
    uint8_t nrc = 0;
    bool ok = false;
    if (module.viaDsg)
    {
      ok = dsgTp20Read(blocks[b], fields, fieldCount, nrc, dsgAnswerMaxAgeMs); // through the DSG's channel
    }
    else
    {
      ok = tp20.readGroup(blocks[b], fields, fieldCount, nrc);
    }
    if (!ok)
    {
      status.errCount++;
      if (nrc)
      {
        for (uint8_t i = 0; i < toReadCount; i++) // the block was refused: its values stop being asked for
        {
          if (settings.vdiagValues[toRead[i]].id == blocks[b])
          {
            status.values[toRead[i]].lastNrc = nrc;
            status.values[toRead[i]].supported = nrc != 0x31 && nrc != 0x12;
          }
        }
      }
      else
      {
        module.lostAnswers++;
      }
      continue;
    }
    module.lostAnswers = 0;
    countSample();
    for (uint8_t i = 0; i < toReadCount; i++) // every value in this block
    {
      const uint8_t valueIndex = toRead[i];
      const VdiagValue &config = settings.vdiagValues[valueIndex];
      if (config.id != blocks[b] || config.field < 1 || config.field > fieldCount)
      {
        continue;
      }
      const DsgField &field = fields[config.field - 1];
      VdiagValueStatus &valueStatus = status.values[valueIndex];
      valueStatus.raw[0] = field.formula;
      valueStatus.raw[1] = field.a;
      valueStatus.raw[2] = field.b;
      valueStatus.rawLen = 3;
      valueStatus.value = field.value;
      valueStatus.valid = true;
      valueStatus.lastNrc = 0;
      if (!config.unit[0] && strcmp(valueStatus.unit, field.unit) != 0)
      {
        copyStr(valueStatus.unit, sizeof(valueStatus.unit), field.unit);
        publishUnit(valueIndex, field.unit); // only when it changes: a label set redraws the gauge
      }
      publish(valueIndex, field.value);
    }
    if (!settings.vdiagTurbo)
    {
      vTaskDelay(pdMS_TO_TICKS(settings.vdiagPollMs)); // normal mode: the capture read a block every ~140 ms
    }
  }
  if (!module.viaDsg && !tp20.isOpen())
  {
    module.open = false; // the channel dropped: opened again next time
  }
}

// one visit to a module: open it if it isn't, read the tab's identification once, then its values once
static void visitModule(uint8_t m)
{
  PollModule &module = modules[m];
  if (!module.isKwp && module.open && moduleCount > 1 && millis() - module.lastAskedMs > VDIAG_SESSION_MS)
  {
    module.open = false; // not asked for a while: its session may have ended
  }
  if (!module.open)
  {
    if (!openModule(module))
    {
      module.failedOpens++;
      return;
    }
    module.failedOpens = 0;
    module.lostAnswers = 0;
  }
  if (module.isTab && !module.identified)
  {
    if (module.isKwp)
    {
      status.protocol = VDIAG_PROTO_TP20;
    }
    else
    {
      status.protocol = VDIAG_PROTO_UDS;
    }
    if (module.isKwp && module.viaDsg)
    {
      status.txId = 0; // 0.37.10: the DSG's channel: its ids show on the DSG card, no identification read here
      status.rxId = 0;
    }
    else if (module.isKwp)
    {
      status.txId = tp20.txId();
      status.rxId = tp20.rxId();
      tp20Identify();
    }
    else
    {
      status.txId = module.target.requestId;
      status.rxId = module.target.answerId;
      udsIdentify(module.target);
    }
    module.identified = true;
  }
  bool probeAsked = false; // a "Try a DID" went to this module's UDS session just now
  if (module.isTab && probeWaiting) // the tab's "Try a DID"
  {
    if (module.isKwp)
    {
      portENTER_CRITICAL(&statusMux);
      copyStr(status.probeText, sizeof(status.probeText), "this module talks KWP2000 - try a block instead");
      portEXIT_CRITICAL(&statusMux);
    }
    else
    {
      udsProbe(module.target, probeRequest);
      probeAsked = true;
    }
    probeWaiting = false;
  }
  // this module's values, leaving out the ones it refused for good
  uint8_t toRead[VDIAG_VALUES];
  uint8_t toReadCount = 0;
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    if (valueModule[i] == (int8_t)m && status.values[i].supported)
    {
      toRead[toReadCount] = i;
      toReadCount++;
    }
  }
  if (module.isKwp)
  {
    tp20Values(module, toRead, toReadCount);
  }
  else
  {
    udsValues(module, toRead, toReadCount);
    if (toReadCount > 0 || probeAsked)
    {
      module.lastAskedMs = millis(); // only when something was asked: a module asked nothing has its session reopened
    }
    if (module.lostAnswers >= VDIAG_FAILS_MAX)
    {
      module.open = false; // no answers: the session is opened again next time
      module.lostAnswers = 0;
    }
  }
}

// the state line: the one module, or how many
static void pollingState()
{
  char detail[64];
  const char *via = "";
  if (useAdapter)
  {
    via = " via the OBD adapter";
  }
  if (moduleCount == 1 && modules[0].isKwp && modules[0].viaDsg)
  {
    snprintf(detail, sizeof(detail), "TP2.0 module %02X, on the DSG's channel", modules[0].tp20Module);
  }
  else if (moduleCount == 1 && modules[0].isKwp)
  {
    snprintf(detail, sizeof(detail), "TP2.0 module %02X, tx %03X rx %03X", modules[0].tp20Module, tp20.txId(),
             tp20.rxId());
  }
  else if (moduleCount == 1)
  {
    snprintf(detail, sizeof(detail), "UDS %lX -> %lX%s", (unsigned long)modules[0].target.requestId,
             (unsigned long)modules[0].target.answerId, via);
  }
  else
  {
    uint8_t answering = 0;
    for (uint8_t m = 0; m < moduleCount; m++)
    {
      if (modules[m].open || modules[m].failedOpens == 0)
      {
        answering++;
      }
    }
    snprintf(detail, sizeof(detail), "%u modules, %u answering%s", moduleCount, answering, via);
  }
  setState("polling", detail);
}

// the session: the modules the values in use come from, round and round until something changes. Returns false when
// no module answered at all (the task counts it as a failed try, see gaveUp()).
static bool runModules(uint32_t myGeneration)
{
  const uint32_t mask = wantedValues();
  buildModules(mask);
  status.viaAdapter = useAdapter;
  uint8_t groupSize = settings.vdiagGroupSize;
  if (groupSize < 1 || groupSize > 8)
  {
    groupSize = 4;
  }
  status.groupSize = groupSize;
  groupSizeWanted = groupSize; // 0.38.0: a new session starts at the tab's size, with no gap again
  goodGroups = 0;
  turboGapNeeded = false;
  turboGapJustSet = false;
  status.turboGap = false;
  if (moduleCount == 0)
  {
    setState("polling", "no values in use - put one on a gauge page, or open this tab");
    vTaskDelay(pdMS_TO_TICKS(500));
    return true; // nothing to ask is not a failed try
  }
  setState("connecting", "opening the modules");
  bool anyAnswered = false;
  bool firstRound = true;
  uint8_t kwpModules = 0; // TP2.0 modules: with more than one, each channel is closed after its visit
  while (running(myGeneration) && wantedValues() == mask)
  {
    // counted each round: the tab's module on Protocol Auto can turn out to be TP2.0 part-way through
    kwpModules = 0;
    for (uint8_t m = 0; m < moduleCount; m++)
    {
      if (modules[m].isKwp && !modules[m].viaDsg) // not the ones on the DSG's channel
      {
        kwpModules++;
      }
    }
    uint8_t givenUp = 0;
    for (uint8_t m = 0; m < moduleCount && running(myGeneration); m++)
    {
      PollModule &module = modules[m];
      if (module.failedOpens >= VDIAG_TRIES_MAX)
      {
        givenUp++;
        continue; // not answering: left alone until the list changes
      }
      visitModule(m);
      if (module.open)
      {
        anyAnswered = true;
      }
      if (module.isKwp && !module.viaDsg && module.open && kwpModules > 1)
      {
        tp20.close(); // the next TP2.0 module needs the channel
        module.open = false;
      }
    }
    if (firstRound && !anyAnswered)
    {
      break; // nobody answered at all
    }
    firstRound = false;
    if (givenUp >= moduleCount)
    {
      break; // every module stopped answering
    }
    pollingState();
    // one module: keep its session alive between rounds. TesterPresent when nothing else has (3E 80: no answer
    // wanted; through the adapter 3E 00, as the ELM327 waits for an answer anyway)
    if (moduleCount == 1 && !modules[0].isKwp && modules[0].open && settings.vdiagSession != 0x01 &&
        millis() - lastRequestMs > VDIAG_TESTER_MS)
    {
      if (useAdapter)
      {
        const uint8_t testerPresent[2] = {0x3E, 0x00};
        uint16_t answerLen = 0;
        udsRequest(modules[0].target, testerPresent, 2, answerLen);
      }
      else
      {
        const uint8_t testerPresent[3] = {0x02, 0x3E, 0x80};
        sendFrame(modules[0].target, testerPresent, 3);
      }
      lastRequestMs = millis();
    }
    if (kwpModules == 1 && tp20.isOpen())
    {
      tp20.keepAlive(); // A3 if the channel has been quiet
    }
    vTaskDelay(1); // let the other core-0 tasks in
  }
  if (tp20.isOpen())
  {
    tp20.close();
  }
  listenId = 0;
  return anyAnswered;
}

// ---------------------------------------------------------------------------
// 0.37.0: Scan My Car's CAN / OBD adapter jobs (can2gauge_CarScan.h): UDS requests to any module, the PQ-era modules
// over TP2.0 and one module's measuring blocks. Run here because this file has the UDS and TP2.0 code.
// ---------------------------------------------------------------------------

// a TP2.0 module's identification as one line, "4B0907551K  2.7l V6 TT", for the label file lookup
static void tp20IdentLine(char *out, size_t outLen)
{
  tp20Identify(); // fills status.partNumber and status.component
  snprintf(out, outLen, "%s  %s", status.partNumber, status.component);
}

// (the label file for a TP2.0 module is found by the web UI through /api/lbl/find: this task's stack is in PSRAM,
// so it never reads the filesystem)

static void scanUds()
{
  CarScanStatus &job = carScanStatus();
  uint16_t answered = 0;
  uint16_t refused = 0;
  for (uint16_t i = 0; i < job.itemCount && !carScanStopWanted(); i++)
  {
    CarScanItem &item = carScanItem(i);
    if (i % 8 == 0)
    {
      carScanSetState("Asking %u of %u", i + 1, job.itemCount);
    }
    const DiagTarget target = makeTarget(item.txId, item.rxId, item.txExt, item.rxExt);
    uint16_t answerLen = 0;
    if (!udsRequest(target, item.request, item.requestLen, answerLen) || answerLen == 0)
    {
      item.state = CARSCAN_NO_ANSWER;
    }
    else
    {
      item.answerLen = answerLen;
      uint16_t kept = answerLen;
      if (kept > CARSCAN_ANSWER_KEEP)
      {
        kept = CARSCAN_ANSWER_KEEP;
      }
      memcpy(item.answer, answer, kept);
      if (answer[0] == 0x7F)
      {
        item.state = CARSCAN_REFUSED;
        if (answerLen >= 3)
        {
          item.nrc = answer[2];
        }
        refused++;
      }
      else
      {
        item.state = CARSCAN_ANSWERED;
        answered++;
      }
    }
    job.itemsDone = i + 1;
  }
  carScanSetState("Done: %u answered, %u refused", answered, refused);
}

static void scanTp20Modules()
{
  CarScanStatus &job = carScanStatus();
  uint16_t found = 0;
  for (uint16_t i = 0; i < job.itemCount && !carScanStopWanted(); i++)
  {
    CarScanItem &item = carScanItem(i);
    const uint8_t module = tp20TableModule(item.number);
    carScanSetState("TP2.0: module %02X (%u of %u)", item.number, i + 1, job.itemCount);
    if (tp20Open(module))
    {
      item.state = CARSCAN_ANSWERED;
      tp20IdentLine(item.text, sizeof(item.text));
      tp20.close();
      found++;
    }
    else
    {
      item.state = CARSCAN_NO_ANSWER;
    }
    job.itemsDone = i + 1;
  }
  listenId = 0;
  carScanSetState("TP2.0: %u module(s) answered", found);
}

static void scanTp20Blocks()
{
  CarScanStatus &job = carScanStatus();
  const uint8_t module = tp20TableModule(job.address);
  carScanSetState("TP2.0: channel to module %02X", job.address);
  if (!tp20Open(module))
  {
    carScanSetState("Module %02X didn't open a TP2.0 channel", job.address);
    return;
  }
  tp20IdentLine(job.moduleId, sizeof(job.moduleId));
  uint16_t found = 0;
  for (uint16_t i = 0; i < job.itemCount && !carScanStopWanted(); i++)
  {
    CarScanItem &item = carScanItem(i);
    if (i % 8 == 0)
    {
      carScanSetState("Module %02X: block %u (%u of %u)", job.address, item.number, i + 1, job.itemCount);
    }
    if (!tp20.isOpen() && !tp20Open(module)) // the channel dropped: open it again once
    {
      carScanSetState("Module %02X stopped answering at block %u", job.address, item.number);
      return;
    }
    DsgField fields[4];
    uint8_t fieldCount = 0;
    uint8_t nrc = 0;
    if (tp20.readGroup(item.number, fields, fieldCount, nrc) && fieldCount > 0)
    {
      item.state = CARSCAN_ANSWERED;
      item.fieldCount = fieldCount;
      for (uint8_t f = 0; f < fieldCount && f < 4; f++)
      {
        item.formula[f] = fields[f].formula;
        item.a[f] = fields[f].a;
        item.b[f] = fields[f].b;
        item.value[f] = fields[f].value;
        copyStr(item.unit[f], sizeof(item.unit[f]), fields[f].unit);
      }
      found++;
    }
    else if (nrc)
    {
      item.state = CARSCAN_REFUSED;
      item.nrc = nrc;
    }
    else
    {
      item.state = CARSCAN_NO_ANSWER;
    }
    job.itemsDone = i + 1;
  }
  tp20.close();
  listenId = 0;
  carScanSetState("Module %02X: %u block(s) answered", job.address, found);
}

// runs the waiting car scan job (in this task, with the measuring session closed)
static void runCarScanJob()
{
  CarScanStatus &job = carScanStatus();
  useAdapter = job.via == VDIAG_LINK_ADAPTER;
  if (!useAdapter)
  {
    vTaskDelay(pdMS_TO_TICKS(300)); // the OBD-II polling stands back (serviceCanBusy): let its last answers pass
    const uint32_t waitStart = millis();
    while (dsgCanSessionOpen() && millis() - waitStart < 3000) // the DSG closes its TP2.0 channel first
    {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  if (job.job == CARSCAN_JOB_UDS)
  {
    scanUds();
  }
  else if (job.job == CARSCAN_JOB_TP20_MODULES)
  {
    scanTp20Modules();
  }
  else if (job.job == CARSCAN_JOB_TP20_BLOCKS)
  {
    scanTp20Blocks();
  }
  if (carScanStopWanted())
  {
    carScanSetState("Stopped");
  }
  listenId = 0;
  useAdapter = false;
  clearSession(); // the identification shown on the tab was the scan's
  carScanFinish();
}

// ---------------------------------------------------------------------------
// the task: find the module's protocol (both ways, as the capture does), then read until something changes
// ---------------------------------------------------------------------------
static void clearSession()
{
  portENTER_CRITICAL(&statusMux);
  status.protocol = 0;
  status.txId = 0;
  status.rxId = 0;
  status.partNumber[0] = '\0';
  status.component[0] = '\0';
  status.swVersion[0] = '\0';
  status.asamDataset[0] = '\0';
  status.asamVersion[0] = '\0';
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    status.values[i].valid = false;
    status.values[i].supported = true;
    status.values[i].lastNrc = 0;
    status.values[i].didLen = 0;
    status.values[i].rawLen = 0;
    copyStr(status.values[i].unit, sizeof(status.values[i].unit), settings.vdiagValues[i].unit);
  }
  portEXIT_CRITICAL(&statusMux);
}

// ---------------------------------------------------------------------------
// Service tab jobs on CAN (0.16.0, can2gauge_Service.h): read or clear one module's faults, UDS first (MQB era), then
// KWP2000 over TP2.0 (PQ era) - the same requests the OBD adapter's Read / Clear DTCs send, on the gauge's own CAN.
// 0.34.0: or through the OBD adapter (UDS only), and the module scan.
// ---------------------------------------------------------------------------

// the module's UDS target: the Diagnostics tab's own ids when it's the same module, otherwise the address table
static DiagTarget serviceTarget(uint8_t address)
{
  if (address == settings.vdiagAddr)
  {
    return tabTarget(); // honours the ids typed on the Diagnostics tab
  }
  return tableTarget(address);
}

// UDS 19 02 FF: every fault with any status bit set. Answer 59 02 <mask> then [code high, middle, failure type][status]
static bool serviceUdsListFaults(const DiagTarget &target)
{
  const uint8_t request[3] = {0x19, 0x02, 0xFF}; // ReadDTCInformation, by status mask, every status
  uint16_t answerLen = 0;
  if (!udsRequest(target, request, 3, answerLen) || answerLen < 3 || answer[0] != 0x59)
  {
    return false;
  }
  serviceClearFaults();
  for (uint16_t pos = 3; pos + 3 < answerLen; pos += 4)
  {
    char code[8];
    serviceDtcText(answer[pos], answer[pos + 1], code, sizeof(code)); // "P0300"
    char codeWithType[12];
    snprintf(codeWithType, sizeof(codeWithType), "%s-%02X", code, answer[pos + 2]); // + the failure type, as in the capture
    serviceAddFault(codeWithType, answer[pos + 3]);
  }
  return true;
}

// the job over UDS. false = the module doesn't speak UDS (try TP2.0)
static bool serviceUdsJob(uint8_t job, uint8_t address, bool &ok)
{
  const DiagTarget target = serviceTarget(address);
  serviceSetState("UDS: extended session to 0x%lX (answer 0x%lX)", (unsigned long)target.requestId,
                  (unsigned long)target.answerId);
  const uint8_t sessionRequest[2] = {0x10, 0x03}; // extended session, as the capture opens it
  uint16_t answerLen = 0;
  if (!udsRequest(target, sessionRequest, 2, answerLen) || answerLen < 2 || answer[0] != 0x50)
  {
    return false; // no UDS answer
  }
  // the module's part number (F187) for the result
  char partNumber[24] = "";
  const uint8_t partRequest[3] = {0x22, 0xF1, 0x87};
  if (udsRequest(target, partRequest, 3, answerLen) && answerLen > 3 && answer[0] == 0x62)
  {
    identText(&answer[3], answerLen - 3, partNumber, sizeof(partNumber));
  }
  serviceSetProtocol("UDS", partNumber);
  ok = false;
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Clearing faults");
    const uint8_t clearRequest[4] = {0x14, 0xFF, 0xFF, 0xFF}; // ClearDiagnosticInformation, every group
    if (!udsRequest(target, clearRequest, 4, answerLen) || answerLen < 1)
    {
      serviceSetState("No answer to the clear");
      return true;
    }
    if (answer[0] != 0x54) // 54 = cleared; 7F 14 <NRC> = refused
    {
      uint8_t nrc = 0;
      if (answerLen >= 3)
      {
        nrc = answer[2];
      }
      serviceSetState("The module refused to clear its faults (NRC 0x%02X, 0x22 = conditions not correct)", nrc);
      return true;
    }
  }
  if (!serviceUdsListFaults(target))
  {
    serviceSetState("The module didn't list its faults");
    return true;
  }
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Cleared - faults still there: %u", serviceFaultCount());
  }
  else
  {
    serviceSetState("Faults stored: %u", serviceFaultCount());
  }
  ok = true;
  const uint8_t defaultSession[2] = {0x10, 0x01}; // back to the default session
  udsRequest(target, defaultSession, 2, answerLen);
  return true;
}

// the job over KWP2000 on TP2.0. false = no channel to the module
static bool serviceTp20Job(uint8_t job, uint8_t address, bool &ok)
{
  uint8_t module = address; // TP2.0 module id: the address, except where the table says otherwise
  if (address == settings.vdiagAddr)
  {
    module = tp20ModuleFor(address);
  }
  else
  {
    for (uint8_t i = 0; i < TP20_TABLE_SIZE; i++)
    {
      if (tp20Address[i] == address)
      {
        module = tp20Module[i];
      }
    }
  }
  serviceSetState("TP2.0: channel setup to module %02X", module);
  if (!tp20Open(module)) // channel + 10 89
  {
    return false;
  }
  serviceSetProtocol("KWP2000 (TP2.0)", "");
  uint16_t answerLen = 0;
  ok = false;
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Clearing faults");
    const uint8_t clearRequest[3] = {0x14, 0xFF, 0x00}; // ClearDiagnosticInformation, every group
    if (!tp20.transact(clearRequest, 3, answer, VDIAG_BUFFER, answerLen, 3000) || answerLen < 1 || answer[0] != 0x54)
    {
      serviceSetState("The module refused to clear its faults");
      tp20.close();
      return true;
    }
  }
  const uint8_t readRequest[4] = {0x18, 0x02, 0xFF, 0x00}; // ReadDiagnosticTroubleCodesByStatus, every group
  if (!tp20.transact(readRequest, 4, answer, VDIAG_BUFFER, answerLen, 3000) || answerLen < 2 || answer[0] != 0x58)
  {
    serviceSetState("The module didn't list its faults");
    tp20.close();
    return true;
  }
  serviceClearFaults();
  // 58 <count> then [high low status] per fault; the VAG fault number is high * 256 + low (scan tools show 5 digits)
  for (uint16_t pos = 2; pos + 2 < answerLen; pos += 3)
  {
    char code[12];
    snprintf(code, sizeof(code), "%05u", (unsigned)((answer[pos] << 8) | answer[pos + 1]));
    serviceAddFault(code, answer[pos + 2]);
  }
  if (job == SVC_JOB_CLEAR_FAULTS)
  {
    serviceSetState("Cleared - faults still there: %u", serviceFaultCount());
  }
  else
  {
    serviceSetState("Faults stored: %u", serviceFaultCount());
  }
  ok = true;
  tp20.close();
  return true;
}

// 0.34.0: the scan's list after the address table: the ids a module may answer on 29-bit. OBD-II's 18 DA xx F1 for the
// engine and gearbox (a 2025 Tiguan answers OBD there), and VW's own 17 FC 00 xx (OBDb: the engine at 76 on MQB Evo,
// the gearbox at 77, the HV battery at 7B)
#define SCAN_29BIT_COUNT 5
static const uint32_t scan29BitIds[SCAN_29BIT_COUNT] = {0x18DA01F1UL, 0x18DA02F1UL, 0x17FC0076UL, 0x17FC0077UL,
                                                        0x17FC007BUL};
static const char *scan29BitNames[SCAN_29BIT_COUNT] = {"Engine (OBD 29-bit)", "Gearbox (OBD 29-bit)",
                                                       "Engine (VW 29-bit)", "Gearbox (VW 29-bit)",
                                                       "HV battery (VW 29-bit)"};

// asks one module for its part number; if it answers, also its name and how many faults it has stored
static bool scanOne(const DiagTarget &target, uint8_t address, const char *name)
{
  uint16_t answerLen = 0;
  const uint8_t partRequest[3] = {0x22, 0xF1, 0x87}; // ReadDataByIdentifier F187: the part number
  if (!udsRequest(target, partRequest, 3, answerLen) || answerLen < 1)
  {
    return false; // nobody there
  }
  ServiceModule module;
  memset(&module, 0, sizeof(module));
  module.address = address;
  module.requestId = target.requestId;
  module.answerId = target.answerId;
  module.faults = -1;
  copyStr(module.name, sizeof(module.name), name);
  if (answerLen > 3 && answer[0] == 0x62)
  {
    identText(&answer[3], answerLen - 3, module.partNumber, sizeof(module.partNumber));
  }
  else
  {
    copyStr(module.partNumber, sizeof(module.partNumber), "(refused F187)"); // it's there, but won't say
  }
  const uint8_t nameRequest[3] = {0x22, 0xF1, 0x97}; // F197: the module's own name ("J533 Gateway")
  if (udsRequest(target, nameRequest, 3, answerLen) && answerLen > 3 && answer[0] == 0x62)
  {
    identText(&answer[3], answerLen - 3, module.component, sizeof(module.component));
  }
  const uint8_t faultRequest[3] = {0x19, 0x02, 0xFF}; // ReadDTCInformation, every fault with a status bit set
  if (udsRequest(target, faultRequest, 3, answerLen) && answerLen >= 3 && answer[0] == 0x59)
  {
    module.faults = (answerLen - 3) / 4; // [code high, middle, failure type][status] each
  }
  serviceAddModule(module);
  return true;
}

// Scan Modules: every address in the table on 11-bit, then the 29-bit ids, in the default session (nothing is opened
// or changed, only read)
static void runScan()
{
  const uint8_t total = UDS_TABLE_SIZE + SCAN_29BIT_COUNT;
  uint8_t found = 0;
  for (uint8_t i = 0; i < UDS_TABLE_SIZE; i++)
  {
    serviceScanProgress(i, total);
    serviceSetState("Scanning %02X %s (%u of %u)", udsAddress[i], udsModuleName[i], i + 1, total);
    const DiagTarget target = makeTarget(udsRequestId[i], udsAnswerId[i], 0, 0);
    if (scanOne(target, udsAddress[i], udsModuleName[i]))
    {
      found++;
    }
  }
  for (uint8_t i = 0; i < SCAN_29BIT_COUNT; i++)
  {
    serviceScanProgress(UDS_TABLE_SIZE + i, total);
    serviceSetState("Scanning %08lX %s (%u of %u)", (unsigned long)scan29BitIds[i], scan29BitNames[i],
                    UDS_TABLE_SIZE + i + 1, total);
    const DiagTarget target = makeTarget(scan29BitIds[i], 0, 0, 0);
    uint8_t address = 0;
    if (i < 2)
    {
      address = (scan29BitIds[i] >> 8) & 0xFF; // 18 DA 01 F1 = address 01
    }
    if (scanOne(target, address, scan29BitNames[i]))
    {
      found++;
    }
  }
  serviceScanProgress(total, total);
  serviceSetState("Scan done: %u module(s) answered", found);
}

// runs the waiting CAN / adapter job (in this task, with the VAG diag session closed)
static void runServiceJob()
{
  EXT_RAM_BSS_ATTR static ServiceStatus job; // static, in PSRAM: ~3 KB is too big for this task's 4 KB stack
  serviceStatusCopy(job);
  bool ok = false;
  useAdapter = job.via == SVC_VIA_ADAPTER; // 0.34.0
  if (useAdapter)
  {
    if (!elmConnected())
    {
      serviceSetState("The OBD adapter isn't connected");
    }
    else if (job.job == SVC_JOB_SCAN)
    {
      runScan();
      ok = true;
    }
    else if (!serviceUdsJob(job.job, job.address, ok))
    {
      serviceSetState("Module 0x%02X didn't answer UDS through the adapter (a PQ-era module needs the gauge's CAN for TP2.0)",
                      job.address);
    }
  }
  else if (!canStats().up)
  {
    serviceSetState("CAN is down");
  }
  else
  {
    vTaskDelay(pdMS_TO_TICKS(300)); // the OBD-II polling stands back (serviceCanBusy): let its last answers pass
    // 0.17.2: the DSG stands back too (serviceCanBusy): wait for it to close its channel, up to 3 s, so two TP2.0
    // channels are never open at once (both receive on 0x300)
    const uint32_t waitStart = millis();
    while (dsgCanSessionOpen() && millis() - waitStart < 3000)
    {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (job.job == SVC_JOB_SCAN)
    {
      runScan();
      ok = true;
    }
    else if (!serviceUdsJob(job.job, job.address, ok) && !serviceTp20Job(job.job, job.address, ok))
    {
      serviceSetState("Module 0x%02X answered neither UDS nor TP2.0", job.address);
    }
  }
  listenId = 0;
  useAdapter = false;
  serviceFinish(ok);
}

// 0.35.0: the VIN over the gauge's CAN, asked once (First Boot's check or the Your Car card's Detect Again): UDS
// 22 F190 then OBD 09 02 to the engine, on 11-bit (7E0) and then on 29-bit (VW's 18 DA 01 F1, the usual OBD 18 DA 10 F1)
static void runVinRead()
{
  useAdapter = false;
  bool found = false;
  char vin[18];
  // nothing is sent with Enable Other Scanners on or the ignition off
  if (canStats().up && !runtime.otherScanners && !runtime.standby)
  {
    const uint32_t requestIds[3] = {0x7E0, 0x18DA01F1UL, 0x18DA10F1UL};
    for (uint8_t idIndex = 0; idIndex < 3 && !found; idIndex++)
    {
      const DiagTarget target = makeTarget(requestIds[idIndex], 0, 0, 0);
      const uint8_t vinDid[3] = {0x22, 0xF1, 0x90}; // ReadDataByIdentifier F190: the VIN
      const uint8_t vinPid[2] = {0x09, 0x02};       // OBD mode 09 PID 02: the VIN
      uint16_t answerLen = 0;
      if (udsRequest(target, vinDid, 3, answerLen) && answerLen >= 20 && answer[0] == 0x62)
      {
        memcpy(vin, &answer[3], 17); // 62 F1 90 then the 17 characters
        vin[17] = '\0';
        found = vehicleVinValid(vin);
      }
      if (!found && udsRequest(target, vinPid, 2, answerLen) && answerLen >= 20 && answer[0] == 0x49)
      {
        memcpy(vin, &answer[answerLen - 17], 17); // 49 02 01 then the 17 characters (the last 17 bytes)
        vin[17] = '\0';
        found = vehicleVinValid(vin);
      }
    }
  }
  listenId = 0;
  if (found)
  {
    vehicleCanReadFound(vin); // the house task takes it: saving My Car / a new theme is too much for this stack
  }
  else
  {
    vehicleCanReadDone(false);
  }
}

static void vdiagTaskFn(void *parameter)
{
  (void)parameter;
  char detail[64];
  while (1)
  {
    if (vehicleCanReadWanted()) // 0.35.0: a one-off VIN read over the CAN
    {
      runVinRead();
      continue;
    }
    if (serviceCanPending()) // 0.16.0: the Service tab's job comes first
    {
      serviceCanTaken();
      runServiceJob();
      continue;
    }
    if (carScanCanPending()) // 0.37.0: Scan My Car's CAN / adapter job
    {
      carScanCanTaken();
      runCarScanJob();
      continue;
    }
    const uint32_t myGeneration = generation;
    useAdapter = settings.vdiagLink == VDIAG_LINK_ADAPTER; // 0.34.0: this session's way to the module
    if (settings.vdiagRun == VDIAG_RUN_OFF)
    {
      setState("off", "no requests on the bus");
    }
    else if (useAdapter && !elmConnected())
    {
      setState("adapter not connected", "Bluetooth OBD tab: pair an adapter (it connects while this tab needs it)");
    }
    else if (!useAdapter && !canStats().up)
    {
      setState("CAN down", "");
    }
    else if (useAdapter && elmStatus().protocol != 0 && !adapterProtocolIsCan(elmStatus().protocol))
    {
      // 0.36.8: the car's OBD is K-line or J1850: UDS can't go through it, so nothing is sent
      setState("no CAN OBD", "the car's OBD is K-line / J1850: UDS needs CAN");
    }
    else if (runtime.otherScanners)
    {
      setState("paused", "Enable Other Scanners is on - nothing sent"); // 0.37.0
    }
    else if (runtime.standby)
    {
      setState("standby", "ignition off");
      failedConnects = 0; // 0.36.8: ignition on again = a fresh start
      failedOnQuietBus = false;
    }
    else if (!linkIsMaster())
    {
      setState("follower", "the chain master reads the module");
    }
    else if (!wanted())
    {
      setState("idle", "connects while a page with a VAG diag value shows, or this tab is open");
    }
    else if (gaveUp(detail, sizeof(detail)))
    {
      setState("stopped", detail); // 0.36.8: no module answered: stop asking
    }
    else
    {
      const bool wasQuiet = !useAdapter && canQuiet(); // 0.36.8: nothing else on the CAN before this try
      clearSession();
      const bool answered = runModules(myGeneration); // 0.37.0: every module the values in use come from
      if (!answered && running(myGeneration))
      {
        // 0.36.8: count it; gaveUp() stops asking after VDIAG_TRIES_MAX (or one, on a quiet CAN)
        failedConnects++;
        failedOnQuietBus = wasQuiet && canQuiet();
        snprintf(detail, sizeof(detail), "no module answered (UDS or TP2.0) - try %u of %u", (unsigned)failedConnects,
                 (unsigned)VDIAG_TRIES_MAX);
        if (useAdapter)
        {
          snprintf(detail, sizeof(detail), "no module answered UDS through the adapter - try %u of %u",
                   (unsigned)failedConnects, (unsigned)VDIAG_TRIES_MAX);
        }
        setState("no answer", detail);
        vTaskDelay(pdMS_TO_TICKS(3000));
      }
      else if (answered)
      {
        failedConnects = 0; // 0.36.8: it answered: the count starts again if it goes away later
        failedOnQuietBus = false;
      }
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

void setupVagDiag()
{
  DEBUG("Setting up VAG diag...");
  memset(&status, 0, sizeof(status));
  copyStr(status.state, sizeof(status.state), "off");
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    status.values[i].supported = true;
  }
  rxQueue = xQueueCreate(24, sizeof(twai_message_t));
  canOnFrame(canHook);
  // 0.37.9: our TP2.0 channels answer on 0x301 (the DSG's on 0x300), and each exchange takes the CAN turn
  tp20.config.testerRxId = 0x301;
  tp20.config.canTurnOwner = CAN_DIAG_VDIAG;
  // stack in PSRAM (saves internal RAM for WiFi and the web server): this task only uses CAN, never flash
  xTaskCreatePinnedToCoreWithCaps(vdiagTaskFn,        // task function
                                  "vdiag",            // name (shows in crash reports)
                                  4096,               // stack size in bytes
                                  NULL,               // no parameter
                                  4,                  // priority (same as the DSG task)
                                  &vdiagTask,         // handle
                                  0,                  // core 0 (LVGL has core 1)
                                  MALLOC_CAP_SPIRAM); // stack in PSRAM
  DEBUG("Set up VAG diag!");
}

void vdiagRestart()
{
  generation = generation + 1; // volatile: an explicit read + write
}

bool vdiagPolling()
{
  if (settings.vdiagRun == VDIAG_RUN_OFF)
  {
    return false;
  }
  for (uint8_t i = 0; i < VDIAG_VALUES; i++)
  {
    if (settings.vdiagValues[i].kind != VDIAG_KIND_NONE)
    {
      return true;
    }
  }
  return false;
}

void vdiagProbe(uint16_t did)
{
  probeRequest = did;
  probeWaiting = true;
  if (failedConnects > 0)
  {
    generation = generation + 1; // 0.36.8: the tab had stopped trying: Read It is a fresh try (as Restart Session)
  }
  portENTER_CRITICAL(&statusMux);
  status.probeId = did;
  status.probeLen = 0;
  copyStr(status.probeText, sizeof(status.probeText), "asking...");
  portEXIT_CRITICAL(&statusMux);
}

const VdiagStatus &vdiagStatus()
{
  return status;
}

bool vdiagWantsAdapter()
{
  if (serviceAdapterBusy())
  {
    return true; // a Service job through the adapter
  }
  // the tab open (even with no values yet: Try a DID needs the session) or a page showing one of its values
  return settings.vdiagLink == VDIAG_LINK_ADAPTER && settings.vdiagRun != VDIAG_RUN_OFF && linkIsMaster() && wanted();
}

uint8_t vdiagModuleCount()
{
  return UDS_TABLE_SIZE;
}

bool vdiagModuleAt(uint8_t index, uint8_t &address, uint16_t &requestId, uint16_t &answerId, const char *&name)
{
  if (index >= UDS_TABLE_SIZE)
  {
    return false;
  }
  address = udsAddress[index];
  requestId = udsRequestId[index];
  answerId = udsAnswerId[index];
  name = udsModuleName[index];
  return true;
}
