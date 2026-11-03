#include <can2gauge_CanRelay.h>

// ---------------------------------------------------------------------------
// senders (slot i = canInject source i + 1)
// ---------------------------------------------------------------------------
struct Sender
{
  uint8_t mac[6];
  uint8_t kind;
  int8_t rssi;
  uint8_t lastSeq;
  uint32_t lastMs; // 0 = free slot
  uint32_t frames, airLost, srcLost;
  uint32_t secondStartMs, secondFrames, perSec;
};
static Sender senders[CANRELAY_SENDERS];
static portMUX_TYPE relayMux = portMUX_INITIALIZER_UNLOCKED;

// the sender's slot: its own, else a free / silent (10 s) one. -1 = all taken
static int slotFor(const uint8_t *mac, uint32_t now)
{
  for (int slot = 0; slot < CANRELAY_SENDERS; slot++)
  {
    if (senders[slot].lastMs && !memcmp(senders[slot].mac, mac, 6))
    {
      return slot;
    }
  }
  for (int slot = 0; slot < CANRELAY_SENDERS; slot++)
  {
    if (!senders[slot].lastMs || now - senders[slot].lastMs > 10000)
    {
      memset(&senders[slot], 0, sizeof(Sender));
      memcpy(senders[slot].mac, mac, 6);
      senders[slot].secondStartMs = now;
      return slot;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// receive (WiFi task - short: the frames only go into CANLine's queue)
// ---------------------------------------------------------------------------
void canRelayOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len)
{
  if (!mac || len < (int)sizeof(CannHdr))
  {
    return;
  }
  const CannHdr *header = (const CannHdr *)data;
  if (header->magic[0] != CANN_MAGIC0 || header->magic[1] != CANN_MAGIC1 || header->proto != CANN_PROTO)
  {
    return;
  }
  const uint32_t now = millis();

  portENTER_CRITICAL(&relayMux);
  const int slot = slotFor(mac, now);
  if (slot >= 0)
  {
    Sender &sender = senders[slot];
    if (sender.lastMs)
    {
      const uint8_t seqGap = (uint8_t)(header->seq - sender.lastSeq - 1);
      if (seqGap < 128) // a big jump backwards is a sender reboot, not loss
      {
        sender.airLost += seqGap;
      }
    }
    sender.lastSeq = header->seq;
    sender.lastMs = now | 1;
    sender.kind = header->kind;
    sender.rssi = rssi;
    sender.srcLost += header->lost;
    sender.frames += header->count;
    sender.secondFrames += header->count;
    const uint32_t windowMs = now - sender.secondStartMs;
    if (windowMs >= 1000)
    {
      // over the window's real length: after a quiet spell it spans many seconds, not one
      sender.perSec = (uint32_t)((uint64_t)sender.secondFrames * 1000 / windowMs);
      sender.secondFrames = 0;
      sender.secondStartMs = now;
    }
  }
  portEXIT_CRITICAL(&relayMux);
  if (slot < 0 || !settings.useCanRelay)
  {
    return;
  }

  size_t offset = sizeof(CannHdr);
  for (uint8_t frameIndex = 0; frameIndex < header->count; frameIndex++)
  {
    if (offset + 2 > (size_t)len)
    {
      break;
    }
    uint16_t tag;
    memcpy(&tag, data + offset, 2);
    offset += 2;
    twai_message_t frame = {};
    const uint8_t dataLength = (tag >> CANN_TAG_LEN_SHIFT) & 0x0F;
    if (dataLength > 8)
    {
      break; // malformed: drop the rest
    }
    if (tag & CANN_TAG_EXT)
    {
      if (offset + 4 > (size_t)len)
      {
        break;
      }
      uint32_t id;
      memcpy(&id, data + offset, 4);
      offset += 4;
      frame.identifier = id & 0x1FFFFFFF;
      frame.extd = 1;
    }
    else
    {
      frame.identifier = tag & CANN_TAG_ID;
    }
    if (offset + dataLength > (size_t)len)
    {
      break;
    }
    frame.data_length_code = dataLength;
    memcpy(frame.data, data + offset, dataLength);
    offset += dataLength;
    canInject(frame, slot + 1);
  }
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------
void setupCanRelay()
{
  memset(senders, 0, sizeof(senders));
  canAirBegin();
}

bool canRelayWantsEspNow()
{
  return settings.useCanRelay != 0;
}

uint8_t canRelaySenders(CanRelaySender *out, uint8_t max)
{
  const uint32_t now = millis();
  uint8_t count = 0;
  portENTER_CRITICAL(&relayMux);
  for (int slot = 0; slot < CANRELAY_SENDERS && count < max; slot++)
  {
    const Sender &sender = senders[slot];
    if (!sender.lastMs || now - sender.lastMs > 10000)
    {
      continue;
    }
    CanRelaySender &outSender = out[count++];
    memcpy(outSender.mac, sender.mac, 6);
    outSender.kind = sender.kind;
    outSender.rssi = sender.rssi;
    outSender.ageMs = now - sender.lastMs;
    outSender.perSec = outSender.ageMs < 2000 ? sender.perSec : 0;
    outSender.frames = sender.frames;
    outSender.airLost = sender.airLost;
    outSender.srcLost = sender.srcLost;
  }
  portEXIT_CRITICAL(&relayMux);
  return count;
}

const char *canRelayKindName(uint8_t kind)
{
  switch (kind)
  {
  case CANN_K_OPENHALDEX: return "OpenHaldex";
  case CANN_K_ADC2CAN: return "adc2can";
  default: return "relay";
  }
}
