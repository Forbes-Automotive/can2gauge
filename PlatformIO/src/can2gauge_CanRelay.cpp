// The car's CAN frames relayed over WiFi (ESP-NOW) by an OpenHaldex or an adc2can, see can2gauge_CanRelay.h.
// Each sender gets a slot here (slot i = canInject source i + 1); its frames go into CANLine as if heard on the wire.

#include <can2gauge_CanRelay.h>

// one sender heard over ESP-NOW (this file's own list, never saved or sent)
struct Sender
{
  uint32_t lastMs;        // millis() of its last packet (0 = free slot)
  uint32_t frames;        // frames received from it
  uint32_t airLost;       // packets lost in the air (gaps in its seq)
  uint32_t srcLost;       // frames it reported it could not relay
  uint32_t secondStartMs; // start of the window its frames a second are counted over
  uint32_t secondFrames;  // frames in that window so far
  uint32_t perSec;        // frames a second, from the last full window
  uint8_t mac[6];         // its WiFi address
  uint8_t kind;           // cannKind_t: OpenHaldex / adc2can
  uint8_t lastSeq;        // the seq of its last packet
  int8_t rssi;            // signal strength of its last packet
};

// ===== File variables (static: private to this file) =====
static Sender senders[CANRELAY_SENDERS];                     // the senders heard (slot i = canInject source i + 1)
static portMUX_TYPE relayMux = portMUX_INITIALIZER_UNLOCKED; // guards senders (the WiFi task writes, the web server reads)

// ===== Functions in this file =====
static int findSenderSlot(const uint8_t *mac, uint32_t now); // the sender's slot, or a free one (-1 = all taken)

// an empty sender list, and CANLine's queue + task that decode the relayed frames
void setupCanRelay()
{
  DEBUG("Setting up the CAN relay...");
  memset(senders, 0, sizeof(senders));
  canAirBegin();
  DEBUG("Set up the CAN relay!");
}

// ESP-NOW must be up for the relay (switched on in the settings)
bool canRelayWantsEspNow()
{
  return settings.useCanRelay != 0;
}

// A relay packet from the shared ESP-NOW receive callback (WiFi task - kept short: the frames only go into CANLine's
// queue). The sender is noted in its slot (seq gaps, frames a second), then each frame is unpacked and handed to
// CANLine as if heard on the wire.
void canRelayOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len)
{
  if (!mac || len < (int)sizeof(CannHdr))
  {
    return;
  }
  // the packet's header, copied out of the packet (the length check above makes sure it is all there)
  CannHdr header;
  memcpy(&header, data, sizeof(header));
  if (header.magic[0] != CANN_MAGIC0 || header.magic[1] != CANN_MAGIC1 || header.proto != CANN_PROTO)
  {
    return;
  }
  const uint32_t now = millis();

  portENTER_CRITICAL(&relayMux);
  const int slot = findSenderSlot(mac, now);
  if (slot >= 0)
  {
    Sender &sender = senders[slot];
    if (sender.lastMs)
    {
      const uint8_t seqGap = (uint8_t)(header.seq - sender.lastSeq - 1);
      if (seqGap < 128) // a big jump backwards is a sender reboot, not loss
      {
        sender.airLost += seqGap;
      }
    }
    sender.lastSeq = header.seq;
    sender.lastMs = now | 1; // | 1: never 0, which would mean a free slot
    sender.kind = header.kind;
    sender.rssi = rssi;
    sender.srcLost += header.lost;
    sender.frames += header.count;
    sender.secondFrames += header.count;
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

  // the frames, back to back: a 2-byte tag each (the 11-bit id, the data length, a flag for a 29-bit id), then the
  // 4-byte id if it is a 29-bit one, then the data bytes
  size_t offset = sizeof(CannHdr);
  for (uint8_t frameIndex = 0; frameIndex < header.count; frameIndex++)
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

// the senders heard in the last 10 s, for the web UI (up to max of them into out); returns how many
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
    CanRelaySender &outSender = out[count];
    count++;
    memcpy(outSender.mac, sender.mac, 6);
    outSender.kind = sender.kind;
    outSender.rssi = sender.rssi;
    outSender.ageMs = now - sender.lastMs;
    // frames a second only while it is still sending (nothing for 2 s = 0)
    if (outSender.ageMs < 2000)
    {
      outSender.perSec = sender.perSec;
    }
    else
    {
      outSender.perSec = 0;
    }
    outSender.frames = sender.frames;
    outSender.airLost = sender.airLost;
    outSender.srcLost = sender.srcLost;
  }
  portEXIT_CRITICAL(&relayMux);
  return count;
}

// a sender's kind as text, for the web UI
const char *canRelayKindName(uint8_t kind)
{
  switch (kind)
  {
  case CANN_K_OPENHALDEX: return "OpenHaldex";
  case CANN_K_ADC2CAN: return "adc2can";
  default: return "relay";
  }
}

// the sender's slot: its own, else a free / silent (10 s) one. -1 = all taken
static int findSenderSlot(const uint8_t *mac, uint32_t now)
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
