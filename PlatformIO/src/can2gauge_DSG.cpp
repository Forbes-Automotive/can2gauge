#include <can2gauge_DSG.h>
#include <can2gauge_Link.h>
#include <CANLine.h>
#include <can2gauge_Tp20.h> // the TP2.0 client (0.8.0: shared with the BLE ELM path)
#include <can2gauge_ELM.h>  // elmConnected() for the "via ELM" states
#include <can2gauge_Service.h> // 0.17.2: serviceCanBusy() - the DSG stands back while a Service tab CAN job runs

// Ported from can2cluster / SpeedPulser Pro (can2cluster_uds.cpp), with the gaps found against the protocol fixed -
// see docs/DSG_UDS_gap_report.md. In short:
//  UDS   - request the GEARBOX physically (0x7E1 -> 0x7E9), not the functional 0x7DF -> engine 0x7E8; the read (DID / PID,
//          byte position, scale) is a setting instead of a hard-coded "DID 0x0101, 0.01 km/h"; multi-frame answers are
//          reassembled with a flow control; "response pending" (7F xx 78) is waited out; an unsupported read (NRC 0x31 ...)
//          stops being requested instead of being retried forever.
//  TP2.0 - modelled on a dealer-tool session with a Mk5 02E DQ250 (02E300050H), SpeedPulserPro Documents/mk5dsgvehiclespeed.csv:
//            setup  200: 02 C0 00 10 00 03 01          -> 202: 00 D0 00 03 60 07 01   (we send on 0x760, hear 0x300)
//                   (one attempt was refused with D8 and worked when repeated 5 ms later)
//            params 760: A0 0F 8A FF 32 FF             -> 300: A1 0F 8A FF 4A FF
//            session 760: 10 00 02 10 89  -> 300: B1, 10 00 02 50 89 (a single last frame) -> 760: B1
//            block  760: 1x 00 02 21 01   -> 300: B(x+1), 2y 00 0E 61 01 .., 2y .., 1y 64 vv -> 760: B(y+1)
//            no ACK within ~200 ms: the same frame again, same sequence number; then A3, and the frame again after A1
//            idle: A3 about every 550 ms; the ECU sending A3 itself = channel in trouble, the capture opened a new one
//            frames carry their real length (7 / 5 / 1 bytes), no padding
//          The tester's send id is still taken from the setup answer (it was 0x760 in the capture); the sequence number
//          runs on across messages and restarts with each new channel.
//          Block 1 of that gearbox: fields 1-2 formula 0x25 (a status value, 0 or 58 at any speed), field 3 formula 0x25
//          (constant 64), field 4 formula 0x07 = 0.01 * a * b with a = 100, i.e. b km/h - the vehicle speed.

static TaskHandle_t dsgTask = NULL;
static QueueHandle_t rxQueue = NULL;
static volatile uint16_t rxIdA = 0, rxIdB = 0; // CAN ids routed into rxQueue (0 = none)
static volatile uint32_t generation = 0;       // bumped by dsgRestart()
static volatile bool canSessionOpen = false;   // 0.17.2: a UDS session or TP2.0 channel to the gearbox is open on the CAN
static DsgStatus status;
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

static void setState(const char *state, const char *detail = NULL)
{
  portENTER_CRITICAL(&statusMux);
  copyStr(status.state, sizeof(status.state), state);
  if (detail)
  {
    copyStr(status.detail, sizeof(status.detail), detail);
  }
  portEXIT_CRITICAL(&statusMux);
}

// runs in the CAN RX task - keep it short
static void canHook(const twai_message_t &message)
{
  if (message.extd || !rxQueue)
  {
    return;
  }
  const uint16_t idA = rxIdA;
  const uint16_t idB = rxIdB;
  if ((idA && message.identifier == idA) || (idB && message.identifier == idB))
  {
    xQueueSend(rxQueue, &message, 0);
  }
}

// 0.17.2: a Service tab fault read / clear on the CAN also ends the session: both clients receive on 0x300, so two
// TP2.0 channels at once (or two to the gearbox itself) would take each other's answers
static bool running(uint32_t myGeneration, uint8_t source)
{
  return generation == myGeneration && settings.dsgSource == source && canStats().up && linkIsMaster() && !runtime.standby &&
         !serviceCanBusy() && !runtime.otherScanners; // 0.37.0: Enable Other Scanners stops the gearbox reads too
}

bool dsgCanSessionOpen()
{
  return canSessionOpen;
}

// ---------------------------------------------------------------------------
// value parsing (shared with the ELM path)
// ---------------------------------------------------------------------------
uint8_t dsgBuildRequest(const DsgReadSpec &spec, uint8_t *out)
{
  if (spec.svc == 0x22)
  {
    out[0] = 0x22;
    out[1] = spec.id >> 8;
    out[2] = spec.id & 0xFF;
    return 3;
  }
  if (spec.svc == 0x01)
  {
    out[0] = 0x01;
    out[1] = spec.id & 0xFF;
    return 2;
  }
  return 0;
}

// 0.26.0: the settings for read DSG_READ_x
const DsgReadSpec &dsgReadSpec(uint8_t which)
{
  switch (which)
  {
  case DSG_READ_SPEED:
    return settings.dsgSpeed;
  case DSG_READ_GEAR:
    return settings.dsgGear;
  case DSG_READ_TEMP:
    return settings.dsgTemp;
  case DSG_READ_LOCKUP:
    return settings.dsgLockup;
  default:
    return settings.dsgSelector;
  }
}

// read DSG_READ_x's name, for the tab's error line
static const char *readNames[DSG_READ_COUNT] = {"speed", "gear", "temperature", "lock-up", "selector"};

// is read DSG_READ_x still worth asking for (the gearbox hasn't refused it)?
static bool readSupported(uint8_t which)
{
  switch (which)
  {
  case DSG_READ_SPEED:
    return status.speedSupported;
  case DSG_READ_GEAR:
    return status.gearSupported;
  case DSG_READ_TEMP:
    return status.tempSupported;
  case DSG_READ_LOCKUP:
    return status.lockupSupported;
  default:
    return status.selectorSupported;
  }
}

// marks read DSG_READ_x as worth asking for or not (the caller holds statusMux)
static void setReadSupported(uint8_t which, bool supported)
{
  switch (which)
  {
  case DSG_READ_SPEED:
    status.speedSupported = supported;
    break;
  case DSG_READ_GEAR:
    status.gearSupported = supported;
    break;
  case DSG_READ_TEMP:
    status.tempSupported = supported;
    break;
  case DSG_READ_LOCKUP:
    status.lockupSupported = supported;
    break;
  default:
    status.selectorSupported = supported;
    break;
  }
}

// a new session: every read that is set up gets another go
static void resetReadSupport()
{
  portENTER_CRITICAL(&statusMux); // dsgRestart() (web / LVGL task) writes these too
  for (uint8_t which = 0; which < DSG_READ_COUNT; which++)
  {
    setReadSupported(which, dsgReadSpec(which).svc != 0);
  }
  status.lockupRaw = -1;
  status.selectorRaw = -1;
  portEXIT_CRITICAL(&statusMux);
}

// any read left that the gearbox hasn't refused?
static bool anyReadSupported()
{
  for (uint8_t which = 0; which < DSG_READ_COUNT; which++)
  {
    if (dsgReadSpec(which).svc != 0 && readSupported(which))
    {
      return true;
    }
  }
  return false;
}

bool dsgReadDue(uint8_t which, uint32_t pass)
{
  if (dsgReadSpec(which).svc == 0 || !readSupported(which))
  {
    return false;
  }
  if (which == DSG_READ_TEMP && pass % DSG_TEMP_EVERY != 0)
  {
    return false; // the oil temperature changes slowly: every 10th pass is plenty
  }
  return true;
}

// 0.26.0: a lock-up read's raw number -> the gauge's 0 open / 1 slipping / 2 locked (-1 = not a code we know)
static int lockupFromRaw(uint32_t raw)
{
  if (settings.dsgGearbox == GEARBOX_ZF_BMW)
  {
    // BMW EGS 22 DA22, byte 1 (OBDb, X5 2014+): 0 open, 1 controlled low, 2 controlled high, 3 closed low,
    // 4 closed high, 8 going open
    switch (raw)
    {
    case 0:
      return 0; // open
    case 1:
    case 2:
    case 8:
      return 1; // slipping (controlled, or on its way open)
    case 3:
    case 4:
      return 2; // locked
    default:
      return -1;
    }
  }
  // other gearboxes: the raw number is already the gauge's coding
  if (raw <= 2)
  {
    return raw;
  }
  return -1;
}

// 0.26.0: a selector read's raw number -> selectorPos_t (SEL_NONE = not a code we know)
static uint8_t selectorFromRaw(uint32_t raw)
{
  if (settings.dsgGearbox == GEARBOX_ZF_BMW)
  {
    // BMW EGS 22 DA2E read as 2 bytes (OBDb, X5 2014+): the first = position 0 P, 1 R, 2 N, 3 D; the second = the
    // mode in D, 3 normal, 5 Sport
    const uint8_t position = (raw >> 8) & 0xFF;
    const uint8_t mode = raw & 0xFF;
    if (position == 0)
    {
      return SEL_P;
    }
    if (position == 1)
    {
      return SEL_R;
    }
    if (position == 2)
    {
      return SEL_N;
    }
    if (position == 3)
    {
      if (mode == 5)
      {
        return SEL_S;
      }
      return SEL_D;
    }
    return SEL_NONE;
  }
  // other gearboxes: the raw number is taken as the gauge's own selector number (1 P, 2 R, 3 N, 4 D, 5 S, 6 M ...)
  if (raw > SEL_NONE && raw < selectorPos_t_MAX)
  {
    return raw;
  }
  return SEL_NONE;
}

// the value in a positive answer, as the spec describes it. raw = the number before scale / table (0.26.0)
static bool parseSpec(const DsgReadSpec &spec, const uint8_t *response, uint16_t len, float &out, uint32_t &raw)
{
  const uint8_t echoLen = spec.svc == 0x22 ? 3 : 2; // [62][hi][lo] / [41][pid]
  if (len < echoLen || response[0] != (uint8_t)(spec.svc + 0x40))
  {
    return false;
  }
  if (spec.svc == 0x22 && (response[1] != (spec.id >> 8) || response[2] != (spec.id & 0xFF)))
  {
    return false;
  }
  if (spec.svc == 0x01 && response[1] != (spec.id & 0xFF))
  {
    return false;
  }
  const uint16_t valuePos = echoLen + spec.pos;
  if (valuePos + spec.len > len)
  {
    return false;
  }
  raw = spec.len == 2 ? ((uint32_t)response[valuePos] << 8) | response[valuePos + 1] : response[valuePos];
  if (spec.fmt == DSG_FMT_HIGH_NIBBLE)
  {
    raw = response[valuePos] >> 4;
  }
  if (spec.fmt == DSG_FMT_TABLE)
  {
    out = raw; // the gearbox type's table turns it into the value (dsgHandleResponse)
  }
  else
  {
    out = raw * spec.scale + spec.offset;
  }
  return true;
}

// NRCs that mean "this read will never work here" (vs busy / conditions not correct, which are worth retrying)
static bool nrcFatal(uint8_t nrc)
{
  return nrc == 0x11 || nrc == 0x12 || nrc == 0x31 || nrc == 0x7E || nrc == 0x7F || nrc == 0x33;
}

void dsgHandleResponse(uint8_t which, const uint8_t *response, uint16_t len)
{
  if (which >= DSG_READ_COUNT)
  {
    return;
  }
  const DsgReadSpec &spec = dsgReadSpec(which);
  if (len >= 3 && response[0] == 0x7F)
  {
    char detail[48];
    snprintf(detail, sizeof(detail), "%s read: NRC 0x%02X", readNames[which], response[2]);
    portENTER_CRITICAL(&statusMux);
    status.errCount++;
    status.lastNrc = response[2];
    if (nrcFatal(response[2]))
    {
      setReadSupported(which, false); // this read will never work here: stop asking
    }
    copyStr(status.detail, sizeof(status.detail), detail);
    portEXIT_CRITICAL(&statusMux);
    return;
  }
  float value;
  uint32_t raw;
  if (!parseSpec(spec, response, len, value, raw))
  {
    portENTER_CRITICAL(&statusMux);
    status.errCount++;
    portEXIT_CRITICAL(&statusMux);
    return;
  }
  portENTER_CRITICAL(&statusMux);
  status.okCount++;
  portEXIT_CRITICAL(&statusMux);

  switch (which)
  {
  case DSG_READ_SPEED:
    signalSet(SIG_TCU_SPEED, value);
    break;
  case DSG_READ_GEAR:
    signalSet(SIG_GEAR, roundf(value));
    break;
  case DSG_READ_TEMP:
    // a sensible temperature only: BMW answers 0xFFCF for "no value", which would come out at 65,000 C
    if (value > -60.0f && value < 250.0f)
    {
      signalSet(SIG_GEARBOX_TEMP, value);
    }
    break;
  case DSG_READ_LOCKUP:
  {
    portENTER_CRITICAL(&statusMux);
    status.lockupRaw = raw; // for the tab: what the gearbox actually sent
    portEXIT_CRITICAL(&statusMux);
    int lockup = (int)lroundf(value);
    if (spec.fmt == DSG_FMT_TABLE)
    {
      lockup = lockupFromRaw(raw);
    }
    if (lockup >= 0 && lockup <= 2)
    {
      signalSet(SIG_LOCKUP, lockup);
    }
    break;
  }
  default: // DSG_READ_SELECTOR
  {
    portENTER_CRITICAL(&statusMux);
    status.selectorRaw = raw;
    portEXIT_CRITICAL(&statusMux);
    uint8_t selector = SEL_NONE;
    if (spec.fmt == DSG_FMT_TABLE)
    {
      selector = selectorFromRaw(raw);
    }
    else
    {
      // not a table read: the scaled value is taken as the gauge's own selector number (1 P, 2 R, 3 N, 4 D ...)
      const long number = lroundf(value);
      if (number > SEL_NONE && number < selectorPos_t_MAX)
      {
        selector = number;
      }
    }
    if (selector != SEL_NONE)
    {
      signalSet(SIG_SELECTOR, selector);
    }
    break;
  }
  }
}

// ---------------------------------------------------------------------------
// ISO-TP (ISO 15765-2) request / response, 11-bit ids, 8-byte frames padded with 0xAA. Normal addressing, or
// (0.26.0) BMW's extended addressing: one extra byte at the front of every frame naming who it's for - the EGS (0x18)
// on the way out, the tester (0xF1) on the way back (settings.dsgTxExtAddr / dsgRxExtAddr, 0 = none). It costs one
// byte of each frame: a single frame carries 6 bytes instead of 7, a consecutive frame 6 instead of 7.
// ---------------------------------------------------------------------------
static bool sendPadded(uint16_t id, const uint8_t *data, uint8_t len)
{
  uint8_t frame[8];
  memset(frame, 0xAA, sizeof(frame));
  memcpy(frame, data, len > 8 ? 8 : len);
  return canSend(id, frame, 8);
}

// 0.26.0: sends one ISO-TP frame (PCI + data), with the extended address byte in front when one is set
static bool sendAddressed(uint16_t id, const uint8_t *data, uint8_t len)
{
  uint8_t frame[8];
  uint8_t frameLen = 0;
  if (settings.dsgTxExtAddr != 0)
  {
    frame[frameLen++] = settings.dsgTxExtAddr; // BMW: the module the frame is for (EGS 0x18)
  }
  for (uint8_t i = 0; i < len && frameLen < 8; i++)
  {
    frame[frameLen++] = data[i];
  }
  return sendPadded(id, frame, frameLen);
}

// sends a single-frame request (<= 7 bytes, 6 with an extended address) and collects the answer (single or
// multi-frame). A "response pending" (7F sid 78) extends the wait. Returns the UDS payload (positive or 7F) in response.
static bool isoTpRequestOnTurn(uint16_t tx, uint16_t rx, const uint8_t *request, uint8_t requestLen, uint8_t *response,
                               uint16_t capacity, uint16_t &responseLen, uint32_t timeoutMs);

// 0.37.9: one request on the gauge's CAN, on the DSG's turn (CANLine.h): the OBD polling and the VAG diag task wait
// meanwhile, so the gearbox's answer isn't taken by another asker
static bool isoTpRequest(uint16_t tx, uint16_t rx, const uint8_t *request, uint8_t requestLen, uint8_t *response,
                         uint16_t capacity, uint16_t &responseLen, uint32_t timeoutMs)
{
  responseLen = 0;
  if (!canDiagWait(CAN_DIAG_DSG, 6000, 1000))
  {
    return false; // the CAN was busy too long: counted as no answer
  }
  const bool ok = isoTpRequestOnTurn(tx, rx, request, requestLen, response, capacity, responseLen, timeoutMs);
  canDiagRelease(CAN_DIAG_DSG);
  return ok;
}

static bool isoTpRequestOnTurn(uint16_t tx, uint16_t rx, const uint8_t *request, uint8_t requestLen, uint8_t *response,
                               uint16_t capacity, uint16_t &responseLen, uint32_t timeoutMs)
{
  const uint8_t rxExtAddr = settings.dsgRxExtAddr; // BMW: every answer starts with 0xF1 (0 = normal addressing)
  uint8_t maxSingle = 7; // request bytes a single frame can carry
  if (settings.dsgTxExtAddr != 0)
  {
    maxSingle = 6; // the address byte takes one
  }
  if (requestLen == 0 || requestLen > maxSingle)
  {
    return false;
  }
  rxIdA = rx;
  rxIdB = 0;
  xQueueReset(rxQueue);
  uint8_t singleFrame[8];
  singleFrame[0] = requestLen;
  memcpy(&singleFrame[1], request, requestLen);
  if (!sendAddressed(tx, singleFrame, requestLen + 1))
  {
    return false;
  }
#if detailedDebugDSG
  DEBUG("DSG UDS -> %03X %02X %02X %02X", tx, request[0], requestLen > 1 ? request[1] : 0, requestLen > 2 ? request[2] : 0);
#endif

  uint32_t deadline = millis() + timeoutMs;
  bool multiFrame = false;
  uint16_t total = 0;
  uint16_t received = 0;
  uint8_t nextSn = 1;
  twai_message_t message;
  while ((int32_t)(deadline - millis()) > 0)
  {
    if (xQueueReceive(rxQueue, &message, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      continue;
    }
    // 0.26.0: the frame without the extended address byte, so everything below reads the same either way
    uint8_t frame[8];
    uint8_t frameLen = 0;
    uint8_t firstByte = 0; // where the ISO-TP part starts in the CAN frame
    if (rxExtAddr != 0)
    {
      if (message.data_length_code < 2 || message.data[0] != rxExtAddr)
      {
        continue; // not addressed to us (the tester)
      }
      firstByte = 1;
    }
    for (uint8_t i = firstByte; i < message.data_length_code && i < 8; i++)
    {
      frame[frameLen++] = message.data[i];
    }
    if (frameLen == 0)
    {
      continue;
    }
    const uint8_t fullFrame = 8 - firstByte; // a full CAN frame's ISO-TP bytes: 8, or 7 with the address byte

    const uint8_t pci = frame[0] >> 4;
    if (pci == 0 && !multiFrame)
    {
      uint8_t payloadLen = frame[0] & 0x0F;
      if (payloadLen == 0 || payloadLen > 7 || payloadLen + 1 > frameLen)
      {
        continue;
      }
      if (payloadLen >= 3 && frame[1] == 0x7F && frame[2] == request[0] && frame[3] == 0x78)
      {
        deadline = millis() + 5000; // response pending
        continue;
      }
      if (frame[1] != (uint8_t)(request[0] + 0x40) && !(frame[1] == 0x7F && frame[2] == request[0]))
      {
        continue; // not ours
      }
      if (payloadLen > capacity)
      {
        payloadLen = capacity;
      }
      memcpy(response, &frame[1], payloadLen);
      responseLen = payloadLen;
      return true;
    }
    if (pci == 1 && !multiFrame)
    {
      total = ((frame[0] & 0x0F) << 8) | frame[1];
      if (frameLen < fullFrame || total < 7 || frame[2] != (uint8_t)(request[0] + 0x40))
      {
        continue; // a first frame is always a full frame
      }
      received = frameLen - 2; // the data bytes after the 2-byte PCI (6, or 5 with the address byte)
      if (received > capacity)
      {
        received = capacity;
      }
      memcpy(response, &frame[2], received);
      multiFrame = true;
      nextSn = 1;
      const uint8_t flowControl[3] = {0x30, 0x00, 0x00}; // continue to send, no block limit, no gap
      sendAddressed(tx, flowControl, 3);
      deadline = millis() + 1000;
      continue;
    }
    if (pci == 2 && multiFrame)
    {
      if ((frame[0] & 0x0F) != (nextSn & 0x0F))
      {
        return false; // lost a consecutive frame
      }
      nextSn++;
      uint16_t chunk = total - received;
      if (chunk > fullFrame - 1)
      {
        chunk = fullFrame - 1; // 7 data bytes a consecutive frame, 6 with the address byte
      }
      if (frameLen < chunk + 1)
      {
        return false; // too short for the bytes still due: the rest of the frame would be stale data
      }
      if (received + chunk > capacity)
      {
        chunk = capacity > received ? capacity - received : 0;
      }
      memcpy(response + received, &frame[1], chunk);
      received += chunk;
      if (received >= total || received >= capacity)
      {
        responseLen = received;
        return true;
      }
      deadline = millis() + 1000;
    }
  }
  return false;
}

// one read (DSG_READ_x) over ISO-TP on the gauge's CAN
static bool udsRead(uint8_t which)
{
  uint8_t request[4];
  uint8_t response[64];
  const uint8_t requestLen = dsgBuildRequest(dsgReadSpec(which), request);
  if (!requestLen)
  {
    return false;
  }
  uint16_t responseLen = 0;
  if (!isoTpRequest(settings.dsgTxId, settings.dsgRxId, request, requestLen, response, sizeof(response), responseLen, 200))
  {
    portENTER_CRITICAL(&statusMux);
    status.errCount++;
    portEXIT_CRITICAL(&statusMux);
    return false;
  }
  dsgHandleResponse(which, response, responseLen);
  return response[0] != 0x7F;
}

static void runUds(uint32_t myGeneration)
{
  const uint16_t tx = settings.dsgTxId;
  const uint16_t rx = settings.dsgRxId;
  char detail[64];
  snprintf(detail, sizeof(detail), "UDS %03X -> %03X, session 0x%02X", tx, rx, settings.dsgUdsSession);
  setState("connecting", detail);
  resetReadSupport(); // every read that is set up gets a go
  canSessionOpen = true; // 0.17.2: a Service tab CAN job waits until this is false again
  if (settings.dsgUdsSession == 0x03)
  {
    const uint8_t request[2] = {0x10, 0x03};
    uint8_t response[16];
    uint16_t responseLen = 0;
    if (!isoTpRequest(tx, rx, request, 2, response, sizeof(response), responseLen, 500) || response[0] != 0x50)
    {
      canSessionOpen = false;
      snprintf(detail, sizeof(detail), "no answer to 10 03 on %03X%s", tx,
               responseLen && response[0] == 0x7F ? " (refused)" : "");
      setState("no answer", detail);
      vTaskDelay(pdMS_TO_TICKS(3000));
      return;
    }
  }
  setState("polling");

  uint32_t lastTesterPresent = millis();
  uint8_t fails = 0;
  uint32_t pass = 0; // 0.26.0: counts the passes, so the slow reads (temperature) are asked for every 10th
  while (running(myGeneration, DSG_SRC_UDS))
  {
    if (settings.dsgUdsSession == 0x03 && millis() - lastTesterPresent >= 2000)
    {
      const uint8_t testerPresent[3] = {0x02, 0x3E, 0x80}; // TesterPresent, suppress positive response
      sendAddressed(tx, testerPresent, 3);
      lastTesterPresent = millis();
    }
    bool ok = false;
    bool asked = false; // something was asked for on this pass
    for (uint8_t which = 0; which < DSG_READ_COUNT; which++)
    {
      if (dsgReadDue(which, pass))
      {
        asked = true;
        ok |= udsRead(which);
      }
    }
    pass++;
    if (!anyReadSupported())
    {
      setState("not supported", "the gearbox rejected every configured read - see Gearbox settings");
      while (running(myGeneration, DSG_SRC_UDS))
      {
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      break;
    }
    if (!asked)
    {
      vTaskDelay(pdMS_TO_TICKS(settings.dsgPollMs));
      continue; // only a slow read is set up, and it isn't due on this pass
    }
    fails = ok ? 0 : fails + 1;
    if (fails >= 10)
    {
      setState("no answer", "10 reads in a row timed out");
      vTaskDelay(pdMS_TO_TICKS(2000));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(settings.dsgPollMs));
  }
  rxIdA = 0;
  canSessionOpen = false;
}

// ---------------------------------------------------------------------------
// VW TP2.0 (KWP2000 over CAN, PQ35 era) - the client is Tp20Client (can2gauge_Tp20.cpp, 0.8.0); here it runs over the
// gauge's own TWAI bus. The BLE ELM path runs the same client over ElmRawLink and reports through dsgTp20Result().
// ---------------------------------------------------------------------------
class TwaiLink : public DiagLink
{
public:
  bool send(uint16_t id, const uint8_t *data, uint8_t len) override
  {
    return canSend(id, data, len);
  }

  bool recv(DiagFrame &frame, uint32_t waitMs) override
  {
    twai_message_t message;
    if (xQueueReceive(rxQueue, &message, pdMS_TO_TICKS(waitMs)) != pdTRUE)
    {
      return false;
    }
    frame.id = message.identifier;
    frame.len = message.data_length_code > 8 ? 8 : message.data_length_code;
    memcpy(frame.data, message.data, frame.len);
    return true;
  }

  void listen(uint16_t id) override
  {
    rxIdA = id;
    rxIdB = 0;
  }

  void flush() override
  {
    xQueueReset(rxQueue);
  }
};
static TwaiLink twaiLink;
static Tp20Client tp20(twaiLink);

// 0.37.10: the DSG's TP2.0 channel is shared with the VAG diag task. A module allows one channel, so a diagnostics value
// from the module the DSG reads (the gearbox) borrows this one instead of opening a second (dsgTp20Read). The lock keeps
// the two tasks' reads apart; the last answer per block is kept, so a value from a block the DSG has just read costs no
// request at all.
#define DSG_SHARED_BLOCKS 4 // blocks whose last answer is kept
struct DsgBlockAnswer
{
  uint8_t group;      // the block, 0 = empty
  bool ok;            // it answered with fields
  uint8_t nrc;        // its refusal code, 0 = none
  uint8_t count;      // fields in it
  DsgField fields[4]; // the fields
  uint32_t ms;        // when (millis)
};
static DsgBlockAnswer blockAnswers[DSG_SHARED_BLOCKS];
static uint8_t blockAnswerNext = 0;      // the entry the next new block goes in
static SemaphoreHandle_t tp20Lock = NULL; // one task on the channel at a time (made in setupDSG)
static portMUX_TYPE blockAnswerMux = portMUX_INITIALIZER_UNLOCKED; // guards blockAnswers (written and read by two tasks)
static volatile bool channelShared = false; // the channel is open and its session started: others may read on it

// keeps a block's answer (replacing its old one, or the oldest entry)
static void keepBlockAnswer(uint8_t group, bool ok, const DsgField *fields, uint8_t count, uint8_t nrc)
{
  int8_t slot = -1;
  for (uint8_t i = 0; i < DSG_SHARED_BLOCKS; i++)
  {
    if (blockAnswers[i].group == group)
    {
      slot = (int8_t)i;
    }
  }
  if (slot < 0)
  {
    slot = (int8_t)blockAnswerNext;
    blockAnswerNext = (blockAnswerNext + 1) % DSG_SHARED_BLOCKS;
  }
  portENTER_CRITICAL(&blockAnswerMux);
  DsgBlockAnswer &answer = blockAnswers[slot];
  answer.group = group;
  answer.ok = ok;
  answer.nrc = nrc;
  answer.count = count;
  if (count > 4)
  {
    answer.count = 4;
  }
  memcpy(answer.fields, fields, answer.count * sizeof(DsgField));
  answer.ms = millis();
  portEXIT_CRITICAL(&blockAnswerMux);
}

// one block on the channel, under the lock, its answer kept
static bool lockedReadGroup(uint8_t group, DsgField *fields, uint8_t &count, uint8_t &nrc)
{
  if (tp20Lock == NULL || xSemaphoreTake(tp20Lock, pdMS_TO_TICKS(1000)) != pdTRUE)
  {
    return false;
  }
  const bool ok = tp20.readGroup(group, fields, count, nrc);
  keepBlockAnswer(group, ok, fields, count, nrc);
  xSemaphoreGive(tp20Lock);
  return ok;
}

bool dsgTp20Shares(uint8_t module)
{
  return settings.dsgSource == DSG_SRC_TP20 && settings.dsgTp20Module == module;
}

bool dsgTp20Read(uint8_t group, DsgField *fields, uint8_t &count, uint8_t &nrc, uint32_t maxAgeMs)
{
  count = 0;
  nrc = 0;
  const uint32_t now = millis();
  bool found = false;
  bool foundOk = false;
  portENTER_CRITICAL(&blockAnswerMux);
  for (uint8_t i = 0; i < DSG_SHARED_BLOCKS; i++)
  {
    const DsgBlockAnswer &answer = blockAnswers[i];
    if (!found && answer.group == group && answer.group != 0 && now - answer.ms < maxAgeMs)
    {
      memcpy(fields, answer.fields, answer.count * sizeof(DsgField)); // the DSG has just read it: use that
      count = answer.count;
      nrc = answer.nrc;
      foundOk = answer.ok;
      found = true;
    }
  }
  portEXIT_CRITICAL(&blockAnswerMux);
  if (found)
  {
    return foundOk;
  }
  if (!channelShared || !tp20.isOpen())
  {
    return false; // the DSG's channel isn't open now (it opens it again by itself)
  }
  return lockedReadGroup(group, fields, count, nrc);
}

void dsgSetState(const char *state, const char *detail)
{
  setState(state, detail);
}

void dsgTp20Channel(uint16_t tx, uint16_t rx)
{
  status.tp20Tx = tx;
  status.tp20Rx = rx;
}

void dsgTp20Result(bool gearBlock, bool ok, const DsgField *fields, uint8_t fieldCount, uint8_t nrc)
{
  if (!ok)
  {
    portENTER_CRITICAL(&statusMux);
    status.errCount++;
    if (nrc)
    {
      status.lastNrc = nrc;
    }
    portEXIT_CRITICAL(&statusMux);
    return;
  }
  portENTER_CRITICAL(&statusMux);
  status.okCount++;
  portEXIT_CRITICAL(&statusMux);
  if (gearBlock)
  {
    portENTER_CRITICAL(&statusMux);
    memcpy(status.gearFields, fields, fieldCount * sizeof(DsgField));
    status.gearFieldCount = fieldCount;
    portEXIT_CRITICAL(&statusMux);
    if (settings.dsgTp20GearField < fieldCount)
    {
      signalSet(SIG_GEAR, roundf(fields[settings.dsgTp20GearField].value));
    }
    return;
  }
  portENTER_CRITICAL(&statusMux);
  memcpy(status.fields, fields, fieldCount * sizeof(DsgField));
  status.fieldCount = fieldCount;
  portEXIT_CRITICAL(&statusMux);
  const uint8_t speedField = settings.dsgTp20SpeedField;
  if (speedField < fieldCount)
  {
    const DsgField &field = fields[speedField];
    float value = field.value;
    if (field.formula != 7 && settings.dsgTp20SpeedDiv > 0) // formula 7 = 0.01*a*b km/h (block 1 field 4 on the 02E)
    {
      value = (field.a * 256.0f + field.b) / settings.dsgTp20SpeedDiv;
    }
    signalSet(SIG_TCU_SPEED, value);
  }
}

static void runTp20(uint32_t myGeneration)
{
  char detail[64];
  setState("connecting", "TP2.0 channel setup on 0x200");
  if (!tp20.open(settings.dsgTp20Module))
  {
    snprintf(detail, sizeof(detail), "module 0x%02X did not open a TP2.0 channel", settings.dsgTp20Module);
    setState("no channel", detail);
    rxIdA = 0;
    vTaskDelay(pdMS_TO_TICKS(3000));
    return;
  }
  canSessionOpen = true; // 0.17.2: a Service tab CAN job waits until this is false again
  dsgTp20Channel(tp20.txId(), tp20.rxId());
  // KWP2000 StartDiagnosticSession 0x89 (the session used for measuring blocks)
  const uint8_t sessionRequest[2] = {0x10, 0x89};
  uint8_t response[16];
  uint16_t responseLen = 0;
  if (!tp20.transact(sessionRequest, 2, response, sizeof(response), responseLen, 1000) || responseLen < 1)
  {
    setState("no session", "no answer to 10 89");
    tp20.close();
    canSessionOpen = false;
    vTaskDelay(pdMS_TO_TICKS(3000));
    return;
  }
  // 0.17.2: a module that refuses 10 89 (7F 10 xx) still reads its blocks in the session it's already in, so carry on
  // instead of giving up
  const bool sessionRefused = response[0] != 0x50;
  snprintf(detail, sizeof(detail), "TP2.0 tx %03X rx %03X, group %u", tp20.txId(), tp20.rxId(), settings.dsgTp20Group);
  if (sessionRefused)
  {
    snprintf(detail, sizeof(detail), "TP2.0 tx %03X rx %03X, group %u (10 89 refused)", tp20.txId(), tp20.rxId(),
             settings.dsgTp20Group);
  }
  setState("polling", detail);
  channelShared = true; // 0.37.10: the session is started: the VAG diag task may read on the channel now

  uint8_t fails = 0;
  bool gearTurn = false;
  while (running(myGeneration, DSG_SRC_TP20) && tp20.isOpen())
  {
    const bool readGear = gearTurn && settings.dsgTp20GearGroup;
    DsgField fields[4];
    uint8_t fieldCount = 0;
    uint8_t nrc = 0;
    uint8_t group = settings.dsgTp20Group;
    if (readGear)
    {
      group = settings.dsgTp20GearGroup;
    }
    const bool ok = lockedReadGroup(group, fields, fieldCount, nrc); // 0.37.10: shared with the VAG diag task
    dsgTp20Result(readGear, ok, fields, fieldCount, nrc);
    gearTurn = !gearTurn;
    fails = ok ? 0 : fails + 1;
    if (fails >= 5)
    {
      setState("lost", "5 block reads in a row failed");
      break;
    }
    if (tp20Lock != NULL && xSemaphoreTake(tp20Lock, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
      tp20.keepAlive(); // A3 if the channel has been quiet (under the lock: the VAG diag task may be reading on it)
      xSemaphoreGive(tp20Lock);
    }
    vTaskDelay(pdMS_TO_TICKS(settings.dsgPollMs));
  }
  channelShared = false; // 0.37.10: first, so the VAG diag task stops borrowing it
  canSessionOpen = false;
  if (tp20Lock != NULL && xSemaphoreTake(tp20Lock, pdMS_TO_TICKS(1000)) == pdTRUE)
  {
    tp20.close();
    xSemaphoreGive(tp20Lock);
  }
  else
  {
    tp20.close();
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ---------------------------------------------------------------------------
static void dsgTaskFn(void *)
{
  for (;;)
  {
    const uint32_t myGeneration = generation;
    const uint8_t source = settings.dsgSource;
    if (source == DSG_SRC_CAN_ONLY)
    {
      setState("off", "passive CAN broadcast only");
    }
    else if (source == DSG_SRC_ELM)
    {
      setState("via BLE ELM", "requests go through the ELM327 (Bluetooth OBD tab)");
    }
    else if (source == DSG_SRC_ELM_TP20)
    {
      // the ELM task owns the state while it talks to the gearbox (dsgSetState / dsgTp20Result)
      if (!elmConnected())
      {
        setState("via BLE ELM", "TP2.0 through the ELM327 - waiting for the adapter (Bluetooth OBD tab)");
      }
    }
    else if (!canStats().up)
    {
      setState("CAN down");
    }
    else if (runtime.standby)
    {
      setState("standby", "ignition off");
    }
    else if (!linkIsMaster())
    {
      setState("follower", "the chain master reads the gearbox");
    }
    else if (serviceCanBusy()) // 0.17.2: stand back while the Service tab reads or clears faults over the CAN
    {
      setState("paused", "a Service tab job is using the CAN");
    }
    else if (source == DSG_SRC_UDS)
    {
      runUds(myGeneration);
      continue;
    }
    else if (source == DSG_SRC_TP20)
    {
      runTp20(myGeneration);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void setupDSG()
{
  tp20.config.canTurnOwner = CAN_DIAG_DSG; // 0.37.9: each TP2.0 exchange takes the CAN turn (answers stay on 0x300)
  tp20Lock = xSemaphoreCreateMutex();      // 0.37.10: the channel is shared with the VAG diag task (dsgTp20Read)
  memset(&status, 0, sizeof(status));
  copyStr(status.state, sizeof(status.state), "off");
  for (uint8_t which = 0; which < DSG_READ_COUNT; which++)
  {
    setReadSupported(which, true); // every read gets a go (dsgReadDue skips the ones that aren't set up)
  }
  status.lockupRaw = -1; // nothing read yet
  status.selectorRaw = -1;
  rxQueue = xQueueCreate(16, sizeof(twai_message_t));
  canOnFrame(canHook);
  // stack in PSRAM (saves internal RAM for WiFi and the web server): the gearbox reads only use CAN, never flash
  xTaskCreatePinnedToCoreWithCaps(dsgTaskFn,         // task function
                                  "dsg",             // name (shows in crash reports)
                                  4096,              // stack size in bytes
                                  NULL,              // no parameter
                                  4,                 // priority
                                  &dsgTask,          // handle
                                  0,                 // core 0 (LVGL has core 1)
                                  MALLOC_CAP_SPIRAM); // stack in PSRAM
}

void dsgRestart()
{
  generation = generation + 1; // volatile: an explicit read + write (++ on a volatile is deprecated in C++20)
  portENTER_CRITICAL(&statusMux);
  for (uint8_t which = 0; which < DSG_READ_COUNT; which++)
  {
    setReadSupported(which, true); // every read gets another go with the new settings
  }
  status.lockupRaw = -1;
  status.selectorRaw = -1;
  status.lastNrc = 0;
  status.fieldCount = 0;
  status.gearFieldCount = 0;
  portEXIT_CRITICAL(&statusMux);
}

uint32_t dsgGeneration()
{
  return generation;
}

bool dsgPolling()
{
  return settings.dsgSource == DSG_SRC_UDS || settings.dsgSource == DSG_SRC_TP20;
}

const DsgStatus &dsgStatus()
{
  return status;
}

void dsgInfoProvider(char *buf, size_t len)
{
  static const char *sourceNames[] = {"CAN broadcast", "UDS (CAN)", "TP2.0 (CAN)", "UDS via BLE ELM", "TP2.0 via BLE ELM"};
  size_t written = 0;
  appendf(buf, len, written, "%s: %s\n%s\nok %lu  err %lu",
          sourceNames[settings.dsgSource < dsgSource_t_MAX ? settings.dsgSource : 0], status.state,
          status.detail, (unsigned long)status.okCount, (unsigned long)status.errCount);
  if (status.lastNrc && written < len)
  {
    appendf(buf, len, written, "  NRC %02X", status.lastNrc);
  }
  if (settings.dsgSource == DSG_SRC_TP20 || settings.dsgSource == DSG_SRC_ELM_TP20)
  {
    for (uint8_t fieldIndex = 0; fieldIndex < status.fieldCount && written < len; fieldIndex++)
    {
      appendf(buf, len, written, "\n%u: f%02X %02X %02X = %.2f %s", fieldIndex,
              status.fields[fieldIndex].formula, status.fields[fieldIndex].a, status.fields[fieldIndex].b,
              status.fields[fieldIndex].value, status.fields[fieldIndex].unit);
    }
  }
}
