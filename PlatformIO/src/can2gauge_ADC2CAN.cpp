#include <can2gauge_ADC2CAN.h>
#include <CANLine.h>
#include <math.h>
#include <adcnow_proto.h> // 0.8.0: the same frames over ESP-NOW (adc2can 1.3+)
#include <GaugeUI.h>       // 0.36.4: uiPageRangesChanged - a page shows an adc2can sensor's dial range
#include <can2gauge_Units.h> // 0.36.4: unitShow - that range in the units on show

// based on adc2can/can2gauge_integration (the adc2can project's receiver), plus the connector voltages, the gauge-side
// scaling and the base id from settings
static Adc2canBoard *boards = (Adc2canBoard *)psramCalloc(ADC2CAN_BOARDS, sizeof(Adc2canBoard)); // in PSRAM (2.6 KB off internal RAM)
static uint8_t gotParts[ADC2CAN_BOARDS][8];
static portMUX_TYPE adcMux = portMUX_INITIALIZER_UNLOCKED;

static float readFloat(const uint8_t *data)
{
  const uint32_t bits = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
  float value;
  memcpy(&value, &bits, 4);
  return value;
}

static signalId_t signalFor(uint8_t board, uint8_t ch)
{
  return (signalId_t)(SIG_ADC_1 + board * 8 + ch);
}

static float readPartFloat(const uint8_t *data)
{
  return readFloat(data + 2);
}

static void sendToBoard(uint8_t board, uint8_t frameOffset, const uint8_t *data)
{
  canSend(settings.adcBaseId + board * 16 + frameOffset, data, 8, true);
}

void adc2canRequest(uint8_t board, uint8_t ch)
{
  const uint8_t data[8] = {1, ch, 0, 0, 0, 0, 0, 0};
  boards[board < ADC2CAN_BOARDS ? board : 0].reqMs = millis();
  sendToBoard(board, 12, data);
}

void adc2canRailReset(uint8_t board, uint8_t ch)
{
  const uint8_t data[8] = {2, ch, 0, 0, 0, 0, 0, 0};
  sendToBoard(board, 12, data);
}

void adc2canSetPart(uint8_t board, uint8_t ch, uint8_t part, const uint8_t payload[6])
{
  uint8_t data[8] = {ch, part};
  memcpy(data + 2, payload, 6);
  sendToBoard(board, 13, data);
}

void adc2canSetFloat(uint8_t board, uint8_t ch, uint8_t part, float value)
{
  uint32_t bits;
  memcpy(&bits, &value, 4);
  const uint8_t payload[6] = {(uint8_t)(bits >> 24), (uint8_t)(bits >> 16), (uint8_t)(bits >> 8), (uint8_t)bits, 0, 0};
  adc2canSetPart(board, ch, part, payload);
}

void adc2canSetText(uint8_t board, uint8_t ch, bool unit, const char *text)
{
  const uint8_t first = unit ? 28 : 25;
  const uint8_t parts = unit ? 2 : 3;
  const size_t len = strlen(text);
  for (uint8_t partIndex = 0; partIndex < parts; partIndex++)
  {
    uint8_t payload[6] = {0};
    for (uint8_t byteIndex = 0; byteIndex < 6 && partIndex * 6 + byteIndex < len; byteIndex++)
    {
      payload[byteIndex] = text[partIndex * 6 + byteIndex];
    }
    adc2canSetPart(board, ch, first + partIndex, payload);
  }
}

// 0.36.4: the "got" bits a board's channel has when every part it has arrived: 30 up to adc2can 1.3, 32 from 1.4 (as
// its info frame says). 32 needs its own case: shifting a 32-bit number by 32 isn't allowed in C++.
static uint32_t allPartsMask(const Adc2canBoard &boardState)
{
  uint8_t parts = boardState.parts;
  if (parts == 0)
  {
    parts = ADC2CAN_PARTS_OLD; // no info frame yet
  }
  if (parts >= 32)
  {
    return 0xFFFFFFFFUL;
  }
  return (1UL << parts) - 1;
}

bool adc2canConfigComplete(uint8_t board, uint8_t ch)
{
  if (board >= ADC2CAN_BOARDS || ch >= 8)
  {
    return false;
  }
  return (boards[board].cfg[ch].got & allPartsMask(boards[board])) == allPartsMask(boards[board]);
}

// 0.36.4: did the adc2can send this sensor's dial range (parts 30 and 31, adc2can 1.4+)?
bool adc2canHasDialRange(uint8_t board, uint8_t ch)
{
  if (board >= ADC2CAN_BOARDS || ch >= 8)
  {
    return false;
  }
  const uint32_t dialParts = (1UL << 30) | (1UL << 31); // parts 30 and 31
  return (boards[board].cfg[ch].got & dialParts) == dialParts;
}

bool adc2canPageRange(signalId_t id, float &low, float &high)
{
  if (id < SIG_ADC_1 || id >= SIG_ADC_1 + ADC2CAN_BOARDS * 8)
  {
    return false; // not an adc2can sensor
  }
  const uint8_t sensor = id - SIG_ADC_1;
  const uint8_t board = sensor / 8;
  const uint8_t channel = sensor % 8;
  if (settings.adc[sensor].mode != ADC_MODE_SENT)
  {
    return false; // scaled on this gauge (ADC tab override): the page's own Min / Max
  }
  if (!adc2canHasDialRange(board, channel))
  {
    return false; // an adc2can before 1.4, or its range hasn't arrived yet
  }
  portENTER_CRITICAL(&adcMux); // the CAN task writes these
  const float rangeLow = boards[board].cfg[channel].dialLo;
  const float rangeHigh = boards[board].cfg[channel].dialHi;
  portEXIT_CRITICAL(&adcMux);
  if (!isfinite(rangeLow) || !isfinite(rangeHigh) || rangeHigh <= rangeLow)
  {
    return false;
  }
  const UnitChoices choices = unitsOf(settings);
  low = unitShow(id, rangeLow, choices, NULL); // in the adc2can's unit: as it's shown (bar -> psi ...)
  high = unitShow(id, rangeHigh, choices, NULL);
  return true;
}

// 0.17.0: the decimals a sensor is shown with (0-15 = board 1 sensor 1 .. board 2 sensor 8): the adc2can's own setting
// when the value is scaled there and its settings have arrived, otherwise the one set on this gauge's ADC tab
uint8_t adc2canDecimals(uint8_t sensor)
{
  if (sensor >= ADC2CAN_BOARDS * 8)
  {
    return 2;
  }
  const uint8_t board = sensor / 8;
  const uint8_t channel = sensor % 8;
  if (settings.adc[sensor].mode == ADC_MODE_SENT && adc2canConfigComplete(board, channel))
  {
    return boards[board].cfg[channel].decimals;
  }
  return settings.adc[sensor].decimals;
}

static void labelChannel(uint8_t board, uint8_t ch);

// one config part into the channel's copy
static void storeConfigPart(uint8_t board, Adc2canBoard &boardState, const uint8_t *data)
{
  const uint8_t ch = data[0];
  const uint8_t part = data[1];
  if (ch >= 8 || part >= ADC2CAN_PARTS)
  {
    return;
  }
  AdcRemoteCfg &channelCfg = boardState.cfg[ch];
  bool rangeChanged = false; // 0.36.4: parts 30 / 31 brought a different dial range
  portENTER_CRITICAL(&adcMux);
  if (part == 0)
  {
    channelCfg.mode = data[2];
    channelCfg.decimals = data[3];
    channelCfg.flags = data[4];
    channelCfg.smoothing = data[5];
  }
  else if (part == 30 || part == 31) // 0.36.4: the dial range the adc2can uses (adc2can 1.4+), [6] = 1 set by hand
  {
    const float value = readPartFloat(data);
    uint8_t byHandBit = 1; // bit 0 = Dial Min
    const bool isFirst = (channelCfg.got & (1UL << part)) == 0; // not heard before
    // a new range (or the first one) for a sensor a page might show: the pages are drawn again (after the lock below)
    if (part == 30)
    {
      if (isFirst || channelCfg.dialLo != value)
      {
        rangeChanged = true;
      }
      channelCfg.dialLo = value;
    }
    else
    {
      if (isFirst || channelCfg.dialHi != value)
      {
        rangeChanged = true;
      }
      channelCfg.dialHi = value;
      byHandBit = 2; // bit 1 = Dial Max
    }
    if (data[6])
    {
      channelCfg.dialByHand |= byHandBit;
    }
    else
    {
      channelCfg.dialByHand &= ~byHandBit;
    }
  }
  else if (part <= 24)
  {
    const float value = readPartFloat(data);
    float *floatFields[] = {NULL, &channelCfg.scale, &channelCfg.offset, &channelCfg.lowWarn, &channelCfg.highWarn,
                            &channelCfg.trim, &channelCfg.ntcR25, &channelCfg.ntcBeta, &channelCfg.ntcPullup};
    if (part <= 8)
    {
      *floatFields[part] = value;
    }
    else if (part <= 16)
    {
      channelCfg.tblV[part - 9] = value;
    }
    else
    {
      channelCfg.tblVal[part - 17] = value;
    }
  }
  else
  {
    char *textField = part <= 27 ? channelCfg.name + (part - 25) * 6 : channelCfg.unit + (part - 28) * 6;
    const size_t room = part <= 27 ? sizeof(channelCfg.name) - (part - 25) * 6 : sizeof(channelCfg.unit) - (part - 28) * 6;
    for (uint8_t byteIndex = 0; byteIndex < 6 && byteIndex < room - 1; byteIndex++)
    {
      textField[byteIndex] = data[2 + byteIndex];
    }
    channelCfg.name[sizeof(channelCfg.name) - 1] = 0;
    channelCfg.unit[sizeof(channelCfg.unit) - 1] = 0;
  }
  channelCfg.got |= 1UL << part;
  const bool named = part >= 25 && part <= 29 && ((channelCfg.got >> 25) & 0x1F) == 0x1F; // name + unit (parts 25-29) complete: they take over from the descriptors now
  if (named)
  {
    copyStr(boardState.name[ch], sizeof(boardState.name[ch]), channelCfg.name);
    copyStr(boardState.unit[ch], sizeof(boardState.unit[ch]), channelCfg.unit);
  }
  portEXIT_CRITICAL(&adcMux);
  if (named)
  {
    labelChannel(board, ch);
  }
  // 0.36.4: a page showing this sensor draws with the adc2can's dial range: draw the pages again with the new one
  if (rangeChanged)
  {
    const uint8_t sensorSignal = signalFor(board, ch);
    for (uint8_t pageIndex = 0; pageIndex < MAX_PAGES; pageIndex++)
    {
      if (settings.pages[pageIndex].enabled && settings.pages[pageIndex].signal == sensorSignal)
      {
        uiPageRangesChanged(); // the screen's own task rebuilds them (it holds the screen lock then)
        break;
      }
    }
  }
}

// the name / unit the gauge shows: its own (web UI > ADC) if set, else the adc2can's; linear scaling without a unit = V
static void labelChannel(uint8_t board, uint8_t ch)
{
  const AdcChCfg &channelCfg = settings.adc[board * 8 + ch];
  const Adc2canBoard &boardState = boards[board];
  const bool own = channelCfg.mode == ADC_MODE_LINEAR; // scaled on the gauge: its own name / unit; otherwise the adc2can's
  const char *name = own && channelCfg.name[0] ? channelCfg.name : boardState.name[ch];
  const char *unit;
  if (own)
  {
    unit = channelCfg.unit[0] ? channelCfg.unit : "V";
  }
  else
  {
    unit = boardState.unit[ch];
  }
  char defaultName[8];
  snprintf(defaultName, sizeof(defaultName), "ADC %u", board * 8 + ch + 1);
  signalSetLabel(signalFor(board, ch), name[0] ? name : defaultName, unit);
}

// one adc2can frame, from CAN or (0.8.0) from an ESP-NOW packet carrying the same frames (include/adcnow_proto.h)
static void handleFrame(uint8_t board, uint8_t frameOffset, const uint8_t *data, bool fromNow)
{
  Adc2canBoard &boardState = boards[board];
  boardState.lastMs = millis();
  if (fromNow)
  {
    boardState.lastNowMs = boardState.lastMs;
  }
  else
  {
    boardState.lastCanMs = boardState.lastMs;
  }

  if (frameOffset <= 3) // values as the adc2can scaled them
  {
    for (uint8_t channelInFrame = 0; channelInFrame < 2; channelInFrame++)
    {
      const uint8_t ch = frameOffset * 2 + channelInFrame;
      const float value = readFloat(&data[channelInFrame * 4]);
      if (isfinite(value) && settings.adc[board * 8 + ch].mode == ADC_MODE_SENT) // NaN = not valid: the signal goes stale
      {
        signalSetBus(signalFor(board, ch), value);
      }
    }
  }
  else if (frameOffset == 4)
  {
    portENTER_CRITICAL(&adcMux);
    boardState.validMask = data[0];
    boardState.railOnMask = data[1];
    boardState.railFaultMask = data[2];
    boardState.rangeMask = data[3];
    boardState.flags = data[4];
    boardState.busV = data[5] * 0.05f;
    boardState.vbat = ((data[6] << 8) | data[7]) / 1000.0f;
    portEXIT_CRITICAL(&adcMux);
  }
  else if (frameOffset == 5) // names / units, 7 characters at a time
  {
    const uint8_t ch = data[0] >> 4;
    const uint8_t part = data[0] & 0x0F;
    if (ch >= 8 || part > 2)
    {
      return;
    }
    char text[8] = {0};
    memcpy(text, &data[1], 7);
    portENTER_CRITICAL(&adcMux);
    if (part < 2)
    {
      memcpy(&boardState.name[ch][part * 7], text, 7);
    }
    else
    {
      copyStr(boardState.unit[ch], sizeof(boardState.unit[ch]), text);
    }
    boardState.name[ch][14] = 0;
    portEXIT_CRITICAL(&adcMux);
    gotParts[board][ch] |= 1 << part;
    if (gotParts[board][ch] == 0x07 && part == 2)
    {
      labelChannel(board, ch);
    }
  }
  else if (frameOffset == 8) // the adc2can's settings, a part at a time
  {
    storeConfigPart(board, boardState, data);
  }
  else if (frameOffset == 9) // info: a new revision means something changed there - ask for the whole config again
  {
    const uint32_t revision = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    const bool fresh = !boardState.info || revision != boardState.rev;
    boardState.info = true;
    boardState.rev = revision;
    boardState.fwMajor = data[4];
    boardState.fwMinor = data[5];
    boardState.parts = data[7]; // 0.36.4: its config parts per channel (30 up to 1.3, 32 from 1.4)
    if (boardState.parts > ADC2CAN_PARTS)
    {
      boardState.parts = ADC2CAN_PARTS; // a newer adc2can: the parts this gauge knows
    }
    bool complete = true;
    for (uint8_t ch = 0; ch < 8 && complete; ch++)
    {
      complete = adc2canConfigComplete(board, ch);
    }
    // the request goes on CAN: not for a board heard only over the air (its slow background cycle sends the config to
    // us anyway, ~24 s for all of it), and never from the WiFi task
    if (!fromNow && (fresh || !complete) && millis() - boardState.reqMs > 2000)
    {
      adc2canRequest(board, 0xFF);
    }
  }
  else if (frameOffset == 6 || frameOffset == 7) // connector voltages; the gauge's own scaling works from these
  {
    for (uint8_t channelInFrame = 0; channelInFrame < 4; channelInFrame++)
    {
      const uint8_t ch = (frameOffset - 6) * 4 + channelInFrame;
      const float value = ((data[channelInFrame * 2] << 8) | data[channelInFrame * 2 + 1]) / 1000.0f;
      boardState.pinV[ch] = value;
      const AdcChCfg &channelCfg = settings.adc[board * 8 + ch];
      if (channelCfg.mode == ADC_MODE_LINEAR && ((boardState.railOnMask >> ch) & 1) && !((boardState.railFaultMask >> ch) & 1))
      {
        signalSetBus(signalFor(board, ch), value * channelCfg.scale + channelCfg.offset);
      }
    }
  }
}

static void onCanFrame(const twai_message_t &message)
{
  if (!message.extd || message.data_length_code < 8 || message.identifier < settings.adcBaseId)
  {
    return;
  }
  const uint32_t idOffset = message.identifier - settings.adcBaseId;
  if (idOffset >= ADC2CAN_BOARDS * 16)
  {
    return;
  }
  handleFrame(idOffset >> 4, idOffset & 0x0F, message.data, false);
}

// runs in the WiFi task (can2gauge_Link.cpp espNowReceive) - only decoding, no CAN sends
void adc2canOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len)
{
  if (!settings.adcWireless || len < (int)sizeof(AdcnHdr))
  {
    return;
  }
  const AdcnHdr *header = (const AdcnHdr *)data;
  if (header->magic[0] != ADCN_MAGIC0 || header->magic[1] != ADCN_MAGIC1 || header->proto != ADCN_PROTO ||
      header->board >= ADC2CAN_BOARDS)
  {
    return;
  }
  if (len < (int)(sizeof(AdcnHdr) + header->count * sizeof(AdcnFrame)))
  {
    return;
  }

  Adc2canBoard &boardState = boards[header->board];
  boardState.nowRssi = rssi;
  if (mac)
  {
    memcpy(boardState.nowMac, mac, 6);
  }
  if (boardState.lastCanMs && millis() - boardState.lastCanMs < 1000)
  {
    boardState.lastNowMs = millis(); // heard on CAN as well: CAN wins, but note that the air copy is there
    return;
  }

  const AdcnFrame *frames = (const AdcnFrame *)(data + sizeof(AdcnHdr));
  for (uint8_t frameIndex = 0; frameIndex < header->count; frameIndex++)
  {
    if (frames[frameIndex].n < 16)
    {
      handleFrame(header->board, frames[frameIndex].n, frames[frameIndex].d, true);
    }
  }
}

void setupADC2CAN()
{
  memset(boards, 0, ADC2CAN_BOARDS * sizeof(Adc2canBoard)); // boards is a PSRAM table now: sizeof(boards) would be just the pointer
  memset(gotParts, 0, sizeof(gotParts));
  for (uint8_t boardIndex = 0; boardIndex < ADC2CAN_BOARDS; boardIndex++)
  {
    for (float &pinVolts : boards[boardIndex].pinV)
    {
      pinVolts = NAN;
    }
  }
  canOnFrame(onCanFrame);
  adc2canApply();
}

void adc2canApply()
{
  for (uint8_t board = 0; board < ADC2CAN_BOARDS; board++)
  {
    for (uint8_t ch = 0; ch < 8; ch++)
    {
      labelChannel(board, ch);
    }
  }
}

const Adc2canBoard &adc2canBoard(uint8_t board)
{
  Adc2canBoard &boardState = boards[board < ADC2CAN_BOARDS ? board : 0];
  boardState.seen = boardState.lastMs && millis() - boardState.lastMs < 2000;
  return boardState;
}

bool adc2canAny()
{
  for (uint8_t board = 0; board < ADC2CAN_BOARDS; board++)
  {
    if (adc2canBoard(board).seen)
    {
      return true;
    }
  }
  return false;
}

void adc2canInfoProvider(char *buf, size_t len)
{
  size_t written = 0;
  buf[0] = 0;
  for (uint8_t board = 0; board < ADC2CAN_BOARDS && written < len; board++)
  {
    const Adc2canBoard &boardState = adc2canBoard(board);
    if (!boardState.seen)
    {
      continue;
    }
    appendf(buf, len, written, "%sadc2can#%u: %u/8 valid, faults 0x%02X%s, bus %.2fV", written ? " | " : "",
            board, __builtin_popcount(boardState.validMask), boardState.railFaultMask,
            (boardState.flags & 1) ? " BACK-FEED" : "", boardState.busV);
  }
  if (!written)
  {
    snprintf(buf, len, "adc2can: not heard");
  }
}
