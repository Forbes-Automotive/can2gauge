#include "KLine.h"

// ===========================================================================
// transport
// ===========================================================================
#define klSerial Serial1 // UART1 remapped onto the K-line pins (UART0 pins are free: console is USB-CDC)
static int8_t klTxPin = -1;
static int8_t klRxPin = -1;
static uint32_t klBaudRate = 10400;
static uint32_t lastRxMs = 0;

// 0.17.4: one bench debug line on the USB serial, "KL: ..." (KL_DEBUG_ERRORS / KL_DEBUG_TIMING in KLine.h)
static void klDebugLine(const char *fmt, ...)
{
  char text[100];
  va_list arguments; // the values after fmt, handed on to vsnprintf
  va_start(arguments, fmt);
  vsnprintf(text, sizeof(text), fmt, arguments);
  va_end(arguments);
  Serial.printf("KL: %s\n", text);
}

static void uartOpen(uint32_t baud)
{
  klSerial.end();
  klSerial.setRxBufferSize(1024); // a 255-byte KWP2000 block at 124800 baud arrives in ~20 ms
  klSerial.begin(baud, SERIAL_8N1, klRxPin, klTxPin);
  klSerial.setTimeout(50);
  klBaudRate = baud;
}

bool klBegin(int8_t txPin, int8_t rxPin, uint32_t baud)
{
  klTxPin = txPin;
  klRxPin = rxPin;
  uartOpen(baud);
  return true;
}

void klEnd()
{
  klSerial.end();
  pinMode(klTxPin, OUTPUT);
  digitalWrite(klTxPin, HIGH); // K released
}

void klSetBaud(uint32_t baud)
{
  uartOpen(baud);
}

uint32_t klBaud()
{
  return klBaudRate;
}

void klSlowInit(uint8_t address, bool addParity)
{
  // ISO 9141-2 / KWP1281 5-baud init: start bit, 8 data bits LSB first, stop bit, 200 ms per bit
  // 0.17.2: the address goes out as 7 bits + odd parity in bit 7, as kw1281test and other scan tools send it. Count the 1 bits in
  // the low 7 bits; if that count is even, set bit 7 so the total is odd. 0x01 stays 0x01, the cluster's 0x17 becomes
  // 0x97, 0x03 -> 0x83, 0x09 -> 0x89. (Sent without it, 0x17 is the even-parity wake-up that Bosch RB4 / RB8 clusters
  // take as a different, non-KWP1281 mode.)
  uint8_t addressByte = address & 0x7F;
  uint8_t oneBits = 0;
  for (int bit = 0; bit < 7; bit++)
  {
    if (addressByte & (1 << bit))
    {
      oneBits++;
    }
  }
  if (oneBits % 2 == 0)
  {
    addressByte |= 0x80; // parity bit makes the number of 1s odd
  }
  if (!addParity)
  {
    // 0.17.4: KWP2000 / OBD wake-ups send the address as the plain byte, as ME7Logger (SLOW-0x11) and every scan tool
    // (0x33) do. With the parity rule above 0x33 went out as 0xB3 and 0x11 as 0x91, and the ECU didn't wake.
    addressByte = address;
  }
  klSerial.end();
  pinMode(klTxPin, OUTPUT);
  digitalWrite(klTxPin, HIGH);
  delay(300);
  digitalWrite(klTxPin, LOW); // start bit
  delay(200);
  for (int bit = 0; bit < 8; bit++)
  {
    digitalWrite(klTxPin, (addressByte >> bit) & 1);
    delay(200);
  }
  digitalWrite(klTxPin, HIGH); // stop bit
  // 0.17.3: listen from the start of the stop bit instead of after it. Some ECUs (ME7.5) send their 0x55 before our
  // 200 ms stop bit would have ended, while the UART was still closed, so the wake-up only worked now and then. The
  // UART idles high, so it carries the stop bit on; klWaitSync() allows for the extra 200 ms.
  uartOpen(10400);
  klFlush();
}

// 0.17.3: waits for the ECU's 0x55 sync byte after the 5-baud address, skipping up to KL_SYNC_STRAY_MAX stray bytes
// first (a glitch as the UART reopens, or the tail of another tester's traffic). true = 0x55 arrived.
bool klWaitSync()
{
  const uint32_t start = millis();
  for (uint8_t strayBytes = 0; strayBytes <= KL_SYNC_STRAY_MAX; strayBytes++)
  {
    const uint32_t elapsed = millis() - start;
    if (elapsed >= KL_SYNC_TIMEOUT_MS)
    {
      return false;
    }
    int received = klRead(KL_SYNC_TIMEOUT_MS - elapsed);
    if (received < 0)
    {
      return false; // nothing at all: the module didn't wake
    }
    if (received == 0x55)
    {
      return true;
    }
  }
  return false;
}

void klFastInit()
{
  klSerial.end();
  pinMode(klTxPin, OUTPUT);
  digitalWrite(klTxPin, HIGH);
  delay(300); // W5 idle before init
  digitalWrite(klTxPin, LOW);
  delay(25);
  digitalWrite(klTxPin, HIGH);
  delay(25);
  uartOpen(klBaudRate);
  klFlush();
}

size_t klWrite(const uint8_t *data, size_t len, uint16_t interByteMs)
{
  size_t echoed = 0;
  if (interByteMs == 0 && len > 1)
  {
    // 0.17.4: no gap wanted between the bytes: the whole frame in one write, then its echo read back in one go
    // (byte by byte, each write waited for its own echo - ~1 ms of turnaround per byte on top of the byte itself)
    klSerial.write(data, len);
    klSerial.flush(); // wait until the last byte has left
    for (size_t i = 0; i < len; i++)
    {
      int echo = klRead(20);
      if (echo < 0)
      {
        break; // the rest of the echo is missing
      }
      if (echo == data[i])
      {
        echoed++;
      }
    }
    return echoed;
  }
  for (size_t i = 0; i < len; i++)
  {
    klSerial.write(data[i]);
    klSerial.flush();
    // swallow the L9637D echo (arrives within one byte time)
    int echo = klRead(20);
    if (echo == data[i])
    {
      echoed++;
    }
    if (interByteMs && i + 1 < len)
    {
      delay(interByteMs); // 0.17.4: only between bytes - none after the last one (it held up every request by P4)
    }
  }
  return echoed;
}

int klRead(uint32_t timeoutMs)
{
  // 0.17.3: waits inside the UART driver, which wakes up the moment the byte arrives. Before, it checked, slept 1 ms
  // and checked again, which added up to a millisecond to every byte - and KWP1281 waits for an answer to every byte.
  klSerial.setTimeout(timeoutMs);
  uint8_t received = 0;
  if (klSerial.readBytes(&received, 1) == 1)
  {
    lastRxMs = millis();
    return received;
  }
  return -1;
}

void klFlush()
{
  while (klSerial.available())
  {
    klSerial.read();
  }
}

uint32_t klLastRxMs()
{
  return lastRxMs;
}

// ===========================================================================
// KWP1281 - VAG block protocol
// Every byte of a block is handshaken: the sender waits for the receiver to answer with the byte's complement,
// except the block-end byte 0x03. Block = [length][counter][title][data...][0x03]; length counts everything but
// the 0x03. The counter increments with every block in either direction.
// ===========================================================================
static uint8_t blockCounter = 0;
static bool k1281Up = false;
#define K1281_BYTE_TIMEOUT 250   // ms to wait for a byte / its complement (W3)
#define K1281_BLOCK_TIMEOUT 1000 // 0.17.3: ms to wait for the first byte of a block - some measuring blocks take the ECU
                                 // several hundred ms to put together; 250 ms took that as a lost session
#define K1281_ACK_DELAY 2        // ms before answering with a complement (ECU needs a gap). 0.17.4 bench: 1 ms (and a
                                 // 5 ms block gap) made no difference on an ME7.5 - its own ~10-16 ms per byte rules

static bool sendByteAck(uint8_t txByte)
{
  const size_t echoed = klWrite(&txByte, 1);
  int received = klRead(K1281_BYTE_TIMEOUT);
  if (received != (uint8_t)~txByte)
  {
#if KL_DEBUG_ERRORS
    if (received < 0)
    {
      klDebugLine("sent %02X (echo %s), no complement back", txByte, echoed ? "ok" : "missing");
    }
    else
    {
      klDebugLine("sent %02X (echo %s), got %02X back instead of %02X", txByte, echoed ? "ok" : "missing", received,
                  (uint8_t)~txByte);
    }
#endif
    return false;
  }
  return true;
}

static int recvByteAck(bool last, uint32_t timeoutMs = K1281_BYTE_TIMEOUT)
{
  int received = klRead(timeoutMs);
  if (received < 0)
  {
#if KL_DEBUG_ERRORS
    klDebugLine("waited %lu ms for a byte from the module, none came", (unsigned long)timeoutMs);
#endif
    return -1;
  }
  if (!last)
  {
    delay(K1281_ACK_DELAY);
    uint8_t complement = ~received;
    klWrite(&complement, 1);
  }
  return received;
}

static bool sendBlock(uint8_t title, const uint8_t *data, uint8_t len)
{
  delay(10); // block gap
  const uint8_t length = 3 + len;
  if (!sendByteAck(length))
  {
    return false;
  }
  if (!sendByteAck(++blockCounter))
  {
    return false;
  }
  if (!sendByteAck(title))
  {
    return false;
  }
  for (uint8_t i = 0; i < len; i++)
  {
    if (!sendByteAck(data[i]))
    {
      return false;
    }
  }
  uint8_t blockEnd = 0x03;
  klWrite(&blockEnd, 1);
  return true;
}

// receives a block; returns title or -1. data/len filled with the payload
static int recvBlock(uint8_t *data, uint8_t &len)
{
  int length = recvByteAck(false, K1281_BLOCK_TIMEOUT); // the block's length byte: the ECU may take a while
  if (length < 3 || length > KWP1281_MAX_BLOCK + 3)
  {
#if KL_DEBUG_ERRORS
    if (length >= 0)
    {
      klDebugLine("block length byte %02X is out of range", length);
    }
#endif
    return -1;
  }
  int counter = recvByteAck(false);
  if (counter < 0)
  {
    return -1;
  }
  blockCounter = counter;
  int title = recvByteAck(false);
  if (title < 0)
  {
    return -1;
  }
  len = length - 3;
  for (uint8_t i = 0; i < len; i++)
  {
    int dataByte = recvByteAck(false);
    if (dataByte < 0)
    {
      return -1;
    }
    data[i] = dataByte;
  }
  int blockEnd = recvByteAck(true);
  if (blockEnd != 0x03)
  {
#if KL_DEBUG_ERRORS
    if (blockEnd >= 0)
    {
      klDebugLine("block ended with %02X instead of 03", blockEnd);
    }
#endif
    return -1;
  }
  return title;
}

static bool wrongKeyBytes = false; // 0.17.4: the last wake-up was answered, but not with KWP1281's key bytes

// one wake-up (5-baud address) and the identification blocks; true = session open
static bool kwp1281ConnectOnce(uint8_t address, char *ecuIdOut, size_t idLen)
{
  k1281Up = false;
  if (ecuIdOut && idLen)
  {
    ecuIdOut[0] = '\0';
  }
  klSlowInit(address, true); // KWP1281: 7 bits + odd parity
  // sync byte 0x55 then two key bytes (0x01 0x8A = KWP1281)
  if (!klWaitSync())
  {
#if KL_DEBUG_ERRORS
    klDebugLine("module %02X: no 0x55 after the 5-baud address", address);
#endif
    return false;
  }
  int keyByte1 = klRead(300);
  int keyByte2 = klRead(300);
  if (keyByte1 < 0 || keyByte2 < 0)
  {
#if KL_DEBUG_ERRORS
    klDebugLine("module %02X: key bytes missing (%d %d)", address, keyByte1, keyByte2);
#endif
    return false;
  }
  if (keyByte1 != 0x01 || keyByte2 != 0x8A)
  {
#if KL_DEBUG_ERRORS
    klDebugLine("module %02X: key bytes %02X %02X are not KWP1281", address, keyByte1, keyByte2);
#endif
    // not KWP1281 (EF 8F = KWP2000, handled by the other stack). 0.17.4: no complement back. Sending it finished the
    // module's KWP2000 wake-up, so it then sat in a KWP2000 session for 5 s and ignored the next 5-baud address (the
    // bench ME7.5 answered EF 8F when woken straight after a session we had closed). Left unanswered it gives up at once.
    wrongKeyBytes = true;
    return false;
  }
  delay(30); // W4
  uint8_t complement = ~keyByte2;
  klWrite(&complement, 1);
  blockCounter = 0;
  // ECU now sends identification blocks (0xF6, ASCII) - ACK each until it sends an ACK back
  uint8_t data[KWP1281_MAX_BLOCK];
  uint8_t len;
  size_t idPos = 0;
  for (int blockIndex = 0; blockIndex < 12; blockIndex++)
  {
    int title = recvBlock(data, len);
    if (title < 0)
    {
#if KL_DEBUG_ERRORS
      klDebugLine("module %02X: identification block %d failed", address, blockIndex + 1);
#endif
      return false;
    }
    // 0.17.2: an 0xF6 block whose first byte is 0x00 is the coding / workshop-code block, not text: left out of the id
    if (title == 0xF6 && ecuIdOut && len > 0 && data[0] != 0x00)
    {
      for (uint8_t byteIndex = 0; byteIndex < len && idPos < idLen - 1; byteIndex++)
      {
        // 0.17.2: bit 7 of the first byte means "more identification available", it isn't part of the character.
        // Dropping those bytes lost the "1" of "1J0920...", so the top bit is masked off instead.
        char character = (char)(data[byteIndex] & 0x7F);
        if (character >= 32 && character < 127)
        {
          ecuIdOut[idPos++] = character;
        }
        else if (idPos && ecuIdOut[idPos - 1] != ' ')
        {
          ecuIdOut[idPos++] = ' ';
        }
      }
      ecuIdOut[idPos] = '\0';
    }
    if (title == 0x09)
    {
      k1281Up = true;
      return true;
    }
    if (!sendBlock(0x09, NULL, 0)) // ACK
    {
      return false;
    }
  }
  return false;
}

bool kwp1281Connect(uint8_t address, char *ecuIdOut, size_t idLen)
{
  wrongKeyBytes = false;
  if (kwp1281ConnectOnce(address, ecuIdOut, idLen))
  {
    return true;
  }
  if (!wrongKeyBytes)
  {
    return false; // no answer, or it failed part way: the caller waits and tries again
  }
  // 0.17.4: it woke up speaking KWP2000. On the bench ME7.5 that happened every time on the first wake-up after we had
  // closed a session with the end block (0x06), even after 2.5 s of quiet, and the next wake-up always gave KWP1281 -
  // so try again straight away instead of leaving it to the caller's 3 s retry
  return kwp1281ConnectOnce(address, ecuIdOut, idLen);
}

bool kwp1281Connected()
{
  return k1281Up;
}

bool kwp1281KeepAlive()
{
  if (!k1281Up)
  {
    return false;
  }
  if (!sendBlock(0x09, NULL, 0))
  {
    k1281Up = false;
    return false;
  }
  uint8_t data[KWP1281_MAX_BLOCK];
  uint8_t len;
  if (recvBlock(data, len) < 0)
  {
    k1281Up = false; // no answer: the session is lost
    return false;
  }
  return true;
}

bool kwp1281ReadGroup(uint8_t group, Kwp1281Group &out)
{
  if (!k1281Up)
  {
    return false;
  }
  out.group = group;
  out.count = 0;
  out.text[0] = '\0';
#if KL_DEBUG_TIMING
  const uint32_t startMs = millis(); // when the request started
#endif
  if (!sendBlock(0x29, &group, 1))
  {
#if KL_DEBUG_ERRORS
    klDebugLine("block %u: request failed", group);
#endif
    k1281Up = false;
    return false;
  }
#if KL_DEBUG_TIMING
  const uint32_t sentMs = millis(); // when the request was out and the answer started
#endif
  uint8_t data[KWP1281_MAX_BLOCK];
  uint8_t len;
  int title = recvBlock(data, len);
  if (title < 0)
  {
#if KL_DEBUG_ERRORS
    klDebugLine("block %u: answer failed", group);
#endif
    k1281Up = false;
    return false;
  }
#if KL_DEBUG_TIMING
  klDebugLine("block %u: %lu ms (request %lu, answer %lu), title %02X, %u bytes", group, (unsigned long)(millis() - startMs),
              (unsigned long)(sentMs - startMs), (unsigned long)(millis() - sentMs), title, len);
#endif
  if (title == 0xE7 && len > 12 && data[0] == 0x3F)
  {
    // 0.17.4: a text block - formula 63 first and longer than 4 fields: everything after the 0x3F is ASCII text
    // (the bench ME7.5's blocks 80-82, 39-44 bytes, came out as 4 nonsense fields with "formulas" 87 / 90 / 112)
    uint8_t textLength = 0;
    for (uint8_t i = 1; i < len && textLength < sizeof(out.text) - 1; i++)
    {
      char character = (char)data[i];
      if (character < 32 || character > 126)
      {
        character = ' '; // not printable
      }
      out.text[textLength] = character;
      textLength++;
    }
    while (textLength > 0 && out.text[textLength - 1] == ' ')
    {
      textLength--; // trailing spaces off
    }
    out.text[textLength] = '\0';
    return true;
  }
  if (title == 0xE7)
  {
    for (uint8_t offset = 0; offset + 2 < len && out.count < 4; offset += 3)
    {
      Kwp1281Field &field = out.fields[out.count++];
      field.formula = data[offset];
      field.a = data[offset + 1];
      field.b = data[offset + 2];
      field.value = kwp1281Decode(field.formula, field.a, field.b, field.unit, sizeof(field.unit));
    }
    return true;
  }
  // 0.17.2: any other block that arrived whole means the session is fine, the group just has nothing to decode:
  // 0x0A NAK = "not available" (the module has no such group), 0x09 ACK, 0x02 = a text-table group. count stays 0.
  // (Before, a NAK returned false and every caller took it as a lost session and woke the module up again.)
  return true;
}

// 0.19.0: read a measuring group over a live KWP2000 K-line session - service 0x21 <group> answered with
// 0x61 <group> [formula a b] x n, decoded with the SAME VAG formulas as kwp1281ReadGroup (TP2.0 does this over CAN
// already). Fills out just like kwp1281ReadGroup so the gauges / K-line mode read it the same way. Used when the
// K-line protocol is set to KWP2000 (a module that came up KWP2000 via the double slow-init). false = request lost.
bool kwp2000ReadGroup(uint8_t group, Kwp1281Group &out)
{
  out.group = group;
  out.count = 0;
  out.text[0] = '\0';
  uint8_t req[2] = {0x21, group};
  uint8_t resp[64];
  uint8_t respLen;
  if (!kwp2000Request(req, 2, resp, respLen, 300, sizeof(resp)))
  {
    return false; // no answer / negative response -> treat as a lost session, like kwp1281ReadGroup
  }
  if (respLen < 2 || resp[0] != 0x61)
  {
    return true; // a short/odd positive answer: link is fine, nothing to decode (count stays 0)
  }
  // resp[1] echoes the group; the field triples start at resp[2]
  if (respLen > 14 && resp[2] == 0x3F)
  {
    // text block (formula 63 first + long): everything after the 0x3F is ASCII (VIN etc.), as kwp1281ReadGroup does
    uint8_t textLength = 0;
    for (uint8_t i = 3; i < respLen && textLength < sizeof(out.text) - 1; i++)
    {
      char character = (char)resp[i];
      if (character < 32 || character > 126)
      {
        character = ' '; // not printable
      }
      out.text[textLength] = character;
      textLength++;
    }
    while (textLength > 0 && out.text[textLength - 1] == ' ')
    {
      textLength--; // trailing spaces off
    }
    out.text[textLength] = '\0';
    return true;
  }
  for (uint8_t offset = 2; offset + 2 < respLen && out.count < 4; offset += 3)
  {
    Kwp1281Field &field = out.fields[out.count++];
    field.formula = resp[offset];
    field.a = resp[offset + 1];
    field.b = resp[offset + 2];
    field.value = kwp1281Decode(field.formula, field.a, field.b, field.unit, sizeof(field.unit));
  }
  return true;
}

// 0.16.0: the module's stored faults. It answers block 0x07 with one or more 0xFC blocks of 3 bytes per fault
// (fault number high, low, status), each acknowledged with 0x09, until it sends 0x09 itself to say that was all.
// "No faults" comes as one fault FF FF 88, which is left out of the list.
bool kwp1281ReadFaults(Kwp1281Fault *faults, uint8_t maxFaults, uint8_t &count)
{
  count = 0;
  if (!k1281Up)
  {
    return false;
  }
  if (!sendBlock(0x07, NULL, 0)) // 0x07 = read faults
  {
    k1281Up = false;
    return false;
  }
  uint8_t data[KWP1281_MAX_BLOCK];
  uint8_t len;
  for (uint8_t blockIndex = 0; blockIndex < 40; blockIndex++) // a module with lots of faults sends several blocks
  {
    int title = recvBlock(data, len);
    if (title < 0)
    {
      k1281Up = false;
      return false;
    }
    if (title != 0xFC)
    {
      return true; // 0x09 = that was all (or 0x0A: the module can't list faults - count stays as it is)
    }
    for (uint8_t offset = 0; offset + 2 < len; offset += 3)
    {
      const uint16_t code = (data[offset] << 8) | data[offset + 1]; // fault number, high byte first
      if (code == 0xFFFF)
      {
        continue; // FF FF 88 = "no faults stored"
      }
      if (count < maxFaults)
      {
        faults[count].code = code;
        faults[count].status = data[offset + 2]; // status / elaboration byte
        count++;
      }
    }
    if (!sendBlock(0x09, NULL, 0)) // ACK: next block please
    {
      k1281Up = false;
      return false;
    }
  }
  return true;
}

// 0.16.0: clears the module's stored faults (block 0x05). The module answers 0x09 (done) - some send their fault list
// back instead (0xFC), which also means it took the request. 0x0A = refused.
bool kwp1281ClearFaults(bool &accepted)
{
  accepted = false;
  if (!k1281Up)
  {
    return false;
  }
  if (!sendBlock(0x05, NULL, 0)) // 0x05 = clear faults
  {
    k1281Up = false;
    return false;
  }
  uint8_t data[KWP1281_MAX_BLOCK];
  uint8_t len;
  int title = recvBlock(data, len);
  if (title < 0)
  {
    k1281Up = false;
    return false;
  }
  accepted = title != 0x0A; // 0x0A = NAK
  // a fault list sent back: acknowledge each block until the module's own 0x09, so the session is in step again
  for (uint8_t blockIndex = 0; title == 0xFC && blockIndex < 40; blockIndex++)
  {
    if (!sendBlock(0x09, NULL, 0))
    {
      k1281Up = false;
      return false;
    }
    title = recvBlock(data, len);
    if (title < 0)
    {
      k1281Up = false;
      return false;
    }
  }
  return true;
}

// 0.16.0: a group in basic setting mode (block 0x28 + group). The module answers with the group's fields like a normal
// group read (0xE7; some send 0xF4), which is how the progress of its routine is shown.
bool kwp1281BasicSetting(uint8_t group, Kwp1281Group &out)
{
  if (!k1281Up)
  {
    return false;
  }
  out.group = group;
  out.count = 0;
  if (!sendBlock(0x28, &group, 1)) // 0x28 = basic setting
  {
    k1281Up = false;
    return false;
  }
  uint8_t data[KWP1281_MAX_BLOCK];
  uint8_t len;
  int title = recvBlock(data, len);
  if (title < 0)
  {
    k1281Up = false;
    return false;
  }
  if (title == 0xE7 || title == 0xF4)
  {
    for (uint8_t offset = 0; offset + 2 < len && out.count < 4; offset += 3)
    {
      Kwp1281Field &field = out.fields[out.count++];
      field.formula = data[offset];
      field.a = data[offset + 1];
      field.b = data[offset + 2];
      field.value = kwp1281Decode(field.formula, field.a, field.b, field.unit, sizeof(field.unit));
    }
  }
  return true; // 0x09 / 0x0A = the group has no basic setting (count stays 0)
}

void kwp1281Disconnect()
{
  if (k1281Up)
  {
    sendBlock(0x06, NULL, 0);
  }
  k1281Up = false;
}

// VAG measuring block formulas (the well-known table; unknown ids fall back to "b")
float kwp1281Decode(uint8_t formula, uint8_t a, uint8_t b, char *unit, size_t unitLen)
{
  const char *unitText = "";
  char twoCharacters[3] = {0, 0, 0}; // formula 63's text, copied into unit at the end
  float value = b;
  switch (formula)
  {
  case 1:
    value = 0.2f * a * b;
    unitText = "rpm";
    break;
  case 2:
    value = a * 0.002f * b;
    unitText = "%";
    break;
  case 3:
    value = 0.002f * a * b;
    unitText = "deg";
    break;
  case 4:
    // 0.17.2: ignition angle with a direction - b above 127 is after TDC, below is before. Positive = BTDC, the way
    // SIG_TIMING is used everywhere else (OBD PID 0E, MaxxECU), so after TDC comes out negative.
    value = (127.0f - b) * 0.01f * a;
    unitText = "deg";
    break;
  case 5:
    value = a * (b - 100.0f) * 0.1f;
    unitText = "C";
    break;
  case 6:
    value = 0.001f * a * b;
    unitText = "V";
    break;
  case 7:
    value = 0.01f * a * b;
    unitText = "km/h";
    break;
  case 8:
    value = 0.1f * a * b;
    unitText = "";
    break;
  case 9:
    value = (b - 127.0f) * 0.02f * a;
    unitText = "deg";
    break;
  case 10:
    value = b;
    unitText = b == 0 ? "COLD" : "WARM";
    break;
  case 11:
    value = 0.0001f * a * (b - 128.0f) + 1.0f;
    unitText = "";
    break;
  case 12:
    value = 0.001f * a * b;
    unitText = "Ohm";
    break;
  case 13:
    value = (b - 127.0f) * 0.001f * a;
    unitText = "mm";
    break;
  case 14:
    value = 0.005f * a * b;
    unitText = "bar";
    break;
  case 15:
    value = 0.01f * a * b;
    unitText = "ms";
    break;
  case 16:
    value = b;
    unitText = "bits";
    break;
  case 17:
    value = b;
    unitText = "chr";
    break;
  case 18:
    value = 0.04f * a * b;
    unitText = "mbar";
    break;
  case 19:
    value = a * b * 0.01f;
    unitText = "l";
    break;
  case 20:
    value = a * (b - 128.0f) / 128.0f;
    unitText = "%";
    break;
  case 21:
    value = 0.001f * a * b;
    unitText = "V";
    break;
  case 22:
    value = 0.001f * a * b;
    unitText = "ms";
    break;
  case 23:
    value = b / 256.0f * a;
    unitText = "%";
    break;
  case 24:
    value = 0.001f * a * b;
    unitText = "A";
    break;
  case 25:
    value = (b * 1.421f) + (a / 182.0f);
    unitText = "g/s";
    break;
  case 26:
    value = b - a;
    unitText = "C";
    break;
  case 27:
    // 0.17.2: ignition angle with a direction, the other way round from formula 4: b below 128 is after TDC.
    // Positive = BTDC, so after TDC comes out negative.
    value = (b - 128.0f) * 0.01f * a;
    unitText = "deg";
    break;
  case 28:
    value = b - a;
    unitText = "";
    break;
  case 29: // 0.17.2: which map the ECU is using, 1 or 2
    if (b < a)
    {
      value = 1;
    }
    else
    {
      value = 2;
    }
    unitText = "map";
    break;
  case 30:
    value = b / 12.0f * a;
    unitText = "deg k/w";
    break;
  case 31:
    value = b / 2560.0f * a;
    unitText = "C";
    break;
  case 32: // 0.17.2: b as a signed number (above 128 counts down from 0)
    if (b > 128)
    {
      value = b - 256.0f;
    }
    else
    {
      value = b;
    }
    unitText = "";
    break;
  case 33:
    // 0.17.2: a = 0 gives 100 x b (was 0), as kw1281test does
    if (a == 0)
    {
      value = 100.0f * b;
    }
    else
    {
      value = 100.0f * b / a;
    }
    unitText = "%";
    break;
  case 34:
    value = (b - 128.0f) * 0.01f * a;
    unitText = "deg"; // 0.17.4: was "kW" - "°KW" is crank degrees (Kurbelwinkel), ME7 ignition retard (block 20)
    break;
  case 35:
    value = 0.01f * a * b;
    unitText = "l/h";
    break;
  case 36:
    value = a * 2560.0f + b * 10.0f;
    unitText = "km";
    break;
  case 37:
    value = b;
    unitText = "";
    break;
  case 38:
    value = (b - 128.0f) * 0.001f * a;
    unitText = "deg k/w";
    break;
  case 39:
    value = b / 256.0f * a;
    unitText = "mg/h";
    break;
  case 40:
    value = b * 0.1f + (25.5f * a) - 400.0f;
    unitText = "A";
    break;
  case 41:
    value = b + a * 255.0f;
    unitText = "Ah";
    break;
  case 42:
    value = b * 0.1f + (25.5f * a) - 400.0f;
    unitText = "Kw";
    break;
  case 43:
    value = b * 0.1f + (25.5f * a);
    unitText = "V";
    break;
  case 44:
    // 0.17.2: a time of a:b, hours and minutes (scan tools show "aa:bb"); given here as hours with the minutes as a fraction
    value = a + b / 60.0f;
    unitText = "h";
    break;
  case 45:
    value = 0.1f * a * b / 100.0f; // 0.17.2: was 0.1 x b (the a cancelled out)
    unitText = "";
    break;
  case 46:
    value = (a * b - 3200.0f) * 0.0027f;
    unitText = "deg k/w";
    break;
  case 47:
    value = (b - 128.0f) * a;
    unitText = "ms";
    break;
  case 48:
    value = b + a * 255.0f;
    unitText = "";
    break;
  case 49:
    value = (b / 4.0f) * a * 0.1f;
    unitText = "mg/h";
    break;
  case 50:
    // 0.17.2: (b - 128) / (0.01 x a), as kw1281test does (was 8 x (b - 128) / a); a = 0 counts as a = 1
    if (a == 0)
    {
      value = (b - 128.0f) / 0.01f;
    }
    else
    {
      value = (b - 128.0f) / (0.01f * a);
    }
    unitText = "mbar";
    break;
  case 51:
    value = ((b - 128.0f) / 255.0f) * a;
    unitText = "mg/h";
    break;
  case 52:
    value = b * 0.02f * a - a;
    unitText = "Nm";
    break;
  case 53:
    value = (b - 128.0f) * 1.4222f + 0.006f * a;
    unitText = "g/s";
    break;
  case 54:
    value = a * 256.0f + b;
    unitText = "count";
    break;
  case 55:
    value = a * b / 200.0f;
    unitText = "s";
    break;
  case 56:
    value = a * 256.0f + b;
    unitText = "WSC";
    break;
  case 57:
    value = a * 256.0f + b + 65536.0f;
    unitText = "WSC";
    break;
  case 58: // 0.17.2: a rate per second; b above 128 counts down from 256
    if (b > 128)
    {
      value = 1.0225f * (256 - b);
    }
    else
    {
      value = 1.0225f * b;
    }
    unitText = "/s";
    break;
  case 59:
    value = (a * 256.0f + b) / 32768.0f;
    unitText = "";
    break;
  case 60:
    value = (a * 256.0f + b) * 0.01f;
    unitText = "sec";
    break;
  case 61: // 0.17.2: (b - 128) / a, or just b - 128 when a = 0
    if (a == 0)
    {
      value = b - 128.0f;
    }
    else
    {
      value = (b - 128.0f) / a;
    }
    unitText = "";
    break;
  case 62:
    value = 0.256f * a * b;
    unitText = "S";
    break;
  case 63: // 0.17.2: two text characters (a then b); the value is both bytes as one number, the unit shows the text
    twoCharacters[0] = '?'; // '?' if a byte isn't a printable character
    twoCharacters[1] = '?';
    if (a >= 32 && a < 127)
    {
      twoCharacters[0] = (char)a;
    }
    if (b >= 32 && b < 127)
    {
      twoCharacters[1] = (char)b;
    }
    value = a * 256.0f + b;
    unitText = twoCharacters;
    break;
  case 64:
    value = a + b;
    unitText = "Ohm";
    break;
  case 65:
    value = 0.01f * a * (b - 127.0f);
    unitText = "mm";
    break;
  case 66:
    value = (a * b) / 511.12f;
    unitText = "V";
    break;
  case 67:
    value = (640.0f * a) + b * 2.5f;
    unitText = "Deg";
    break;
  case 68:
    value = (256.0f * a + b) / 7.365f;
    unitText = "deg/s";
    break;
  case 69:
    value = (256.0f * a + b) * 0.3254f;
    unitText = "Bar";
    break;
  case 70:
    value = (256.0f * a + b) * 0.192f;
    unitText = "m/s^2";
    break;
  default: break;
  }
  if (unit && unitLen)
  {
    strncpy(unit, unitText, unitLen - 1);
    unit[unitLen - 1] = '\0';
  }
  return value;
}

// ===========================================================================
// KWP2000 / ISO 14230-4
// ===========================================================================
static bool k2000Up = false;
static uint8_t k2000Target = 0x33;
static uint8_t k2000LastNrc = 0;
static uint16_t k2000P3 = 0; // ms after the ECU's last byte before we may send
static uint8_t k2000P4 = 5;  // ms between request bytes
#define TESTER_ADDR 0xF1
#define KWP_NRC_RESPONSE_PENDING 0x78
#define KWP_PENDING_TIMEOUT 5000 // P2* max while the ECU keeps answering "response pending"
#define KWP_PENDING_MAX 40

void kwp2000SetTiming(uint16_t p3MinMs, uint8_t p4Ms)
{
  k2000P3 = p3MinMs;
  k2000P4 = p4Ms;
}

uint8_t kwp2000LastNrc()
{
  return k2000LastNrc;
}

// 0.17.6: timing of the last request (kwp2000LastTiming), in microseconds since boot
static uint32_t k2000SendStartUs = 0;  // the request started going out
static uint32_t k2000SendDoneUs = 0;   // its last byte (and echo) done
static uint32_t k2000FirstByteUs = 0;  // the ECU's first answer byte arrived
static uint32_t k2000AnswerDoneUs = 0; // the answer's checksum arrived

void kwp2000LastTiming(uint32_t &sendUs, uint32_t &waitUs, uint32_t &answerUs)
{
  sendUs = k2000SendDoneUs - k2000SendStartUs;
  waitUs = 0;
  answerUs = 0;
  if (k2000FirstByteUs >= k2000SendDoneUs)
  {
    waitUs = k2000FirstByteUs - k2000SendDoneUs;
  }
  if (k2000AnswerDoneUs >= k2000FirstByteUs)
  {
    answerUs = k2000AnswerDoneUs - k2000FirstByteUs;
  }
}

uint8_t kwp2000Checksum(const uint8_t *data, uint8_t len)
{
  uint8_t checksum = 0;
  for (uint8_t i = 0; i < len; i++)
  {
    checksum += data[i];
  }
  return checksum;
}

// reads one response message: [fmt][target=F1][source]([len])[payload][cs] -> payload
static bool kwp2000ReadMessage(uint8_t *resp, uint8_t &respLen, uint32_t timeoutMs, uint16_t respCap)
{
  respLen = 0;
  int fmt = klRead(timeoutMs);
  if (fmt < 0)
  {
    return false;
  }
  k2000FirstByteUs = micros(); // 0.17.6: the ECU has started answering (kwp2000LastTiming)
  uint16_t len = fmt & 0x3F;
  // 0.17.4: the format byte's top two bits say whether address bytes follow (ISO 14230-2): 00 = none (header mode 0,
  // ME7Logger's "HM0"), 10 = physical, 11 = functional. Address bytes used to be read whatever it said.
  int target = 0;
  int source = 0;
  if ((fmt & 0xC0) != 0)
  {
    target = klRead(50);
    source = klRead(50);
    if (target < 0 || source < 0)
    {
      return false;
    }
  }
  if (len == 0)
  { // additional length byte
    int extraLen = klRead(50);
    if (extraLen < 0)
    {
      return false;
    }
    len = extraLen;
  }
  uint8_t sum = (uint8_t)fmt + (uint8_t)target + (uint8_t)source + ((fmt & 0x3F) == 0 ? (uint8_t)len : 0); // 0 + 0 with no addresses
  const bool fits = len <= respCap && len <= 255;
  for (uint16_t i = 0; i < len; i++)
  {
    int dataByte = klRead(50);
    if (dataByte < 0)
    {
      return false;
    }
    if (fits)
    {
      resp[respLen++] = dataByte;
    }
    sum += dataByte;
  }
  int checksum = klRead(50);
  k2000AnswerDoneUs = micros(); // 0.17.6: the whole answer is in
  return fits && checksum >= 0 && (uint8_t)checksum == sum;
}

// 0.17.4: ISO 9141-2 (OBD-II on K-line before KWP2000; the ECU says so with key bytes 08 08 or 94 94 after the 5-baud
// 0x33). Requests are [68][6A][F1][data][cs], answers [48][6B][ECU address][data][cs] - no length byte, so an answer
// ends when the line goes quiet for ISO9141_END_GAP_MS (P1max is 20 ms between the ECU's bytes).
static bool k2000Iso9141 = false;
static bool k2000WithAddresses = true; // 0.17.4: false = header mode 0 (format byte = length, no target / source)
#define ISO9141_END_GAP_MS 25
#define ISO9141_RELEASE_MS 5200 // P3max (5 s) + a little: after this long without a request the ECU ends the session

static bool iso9141ReadMessage(uint8_t *resp, uint8_t &respLen, uint32_t timeoutMs, uint16_t respCap)
{
  respLen = 0;
  uint8_t message[64]; // the whole answer: 3 header bytes, data, checksum (OBD answers are 11 bytes at most)
  uint8_t messageLength = 0;
  int received = klRead(timeoutMs); // the first byte: P2max is 50 ms, the caller gives more
  while (received >= 0 && messageLength < sizeof(message))
  {
    message[messageLength] = (uint8_t)received;
    messageLength++;
    received = klRead(ISO9141_END_GAP_MS); // the next byte, or -1 once the line has gone quiet
  }
  if (messageLength < 5) // header (3) + at least 1 data byte + checksum
  {
#if KL_DEBUG_ERRORS
    if (messageLength > 0)
    {
      klDebugLine("ISO 9141: answer only %u bytes long", messageLength);
    }
#endif
    return false;
  }
  const uint8_t checksum = kwp2000Checksum(message, messageLength - 1); // the same plain sum as KWP2000
  if (checksum != message[messageLength - 1])
  {
#if KL_DEBUG_ERRORS
    klDebugLine("ISO 9141: checksum %02X, the answer says %02X", checksum, message[messageLength - 1]);
#endif
    return false;
  }
  for (uint8_t i = 3; i < messageLength - 1 && respLen < respCap; i++)
  {
    resp[respLen] = message[i]; // the data, without the header and checksum
    respLen++;
  }
  return true;
}

bool kwp2000IsIso9141()
{
  return k2000Iso9141;
}

// builds the KWP2000 request frame into buf (header, payload, checksum) for the addressing mode in use and returns
// the total number of bytes to send. Shared by kwp2000Request and kwp2000RequestRaw so the two can never frame a
// request differently.
static uint8_t kwp2000FrameRequest(uint8_t *buf, const uint8_t *req, uint8_t reqLen)
{
  // format byte: 0x80 = physical addressing (one ECU by its own address), 0xC0 = functional (ISO 14230-4 OBD: 0x33 is
  // "every emissions ECU", not anyone's own address). 0.17.4: OBD requests to 0x33 went out physical, so an ECU whose
  // address isn't 0x33 could ignore them - the bench ME7.5 never answered the fast init's C1 33 F1 81 sent as 81 33 F1 81
  uint8_t headerLength = 3; // format, target, source
  if (k2000Iso9141)
  {
    buf[0] = 0x68; // ISO 9141-2 header: priority / type
    buf[1] = 0x6A; // target: OBD-II request
    buf[2] = TESTER_ADDR;
  }
  else if (!k2000WithAddresses)
  {
    buf[0] = reqLen; // 0.17.4: header mode 0 - just the length, no address bytes (ME7Logger's "HM0")
    headerLength = 1;
  }
  else if (k2000Target == KWP2000_OBD_ADDRESS)
  {
    buf[0] = 0xC0 | reqLen;
    buf[1] = k2000Target;
    buf[2] = TESTER_ADDR;
  }
  else
  {
    buf[0] = 0x80 | reqLen;
    buf[1] = k2000Target;
    buf[2] = TESTER_ADDR;
  }
  memcpy(buf + headerLength, req, reqLen);
  buf[headerLength + reqLen] = kwp2000Checksum(buf, headerLength + reqLen);
  return headerLength + reqLen + 1; // header + data + the checksum
}

// sends the framed bytes: waits out P3 (the ECU's idle time), flushes, and writes them (klWrite reads back the echo)
static void kwp2000SendFrame(const uint8_t *buf, uint8_t total)
{
  // P3: give the ECU its minimum idle time after its last byte
  if (k2000P3)
  {
    const uint32_t since = millis() - klLastRxMs();
    if (since < k2000P3)
    {
      delay(k2000P3 - since);
    }
  }
  klFlush();
  k2000SendStartUs = micros(); // 0.17.6: timing for kwp2000LastTiming
  k2000FirstByteUs = 0;
  k2000AnswerDoneUs = 0;
  klWrite(buf, total, k2000P4); // header, data and the checksum
  k2000SendDoneUs = micros();
}

// sends [0x80|len][target][0xF1][payload][cs], returns the response payload (header + checksum stripped)
bool kwp2000Request(const uint8_t *req, uint8_t reqLen, uint8_t *resp, uint8_t &respLen, uint32_t timeoutMs, uint16_t respCap)
{
  uint8_t buf[64];
  k2000LastNrc = 0;
  respLen = 0;
  if (reqLen > 60)
  {
    return false;
  }
  const uint8_t total = kwp2000FrameRequest(buf, req, reqLen);
  kwp2000SendFrame(buf, total);
  uint32_t wait = timeoutMs;
  for (uint8_t pending = 0; pending <= KWP_PENDING_MAX; pending++)
  {
    bool gotMessage = false;
    if (k2000Iso9141)
    {
      gotMessage = iso9141ReadMessage(resp, respLen, wait, respCap);
    }
    else
    {
      gotMessage = kwp2000ReadMessage(resp, respLen, wait, respCap);
    }
    if (!gotMessage)
    {
#if KL_DEBUG_ERRORS
      klDebugLine("KWP2000 request %02X %02X (%u bytes): no answer / bad checksum", req[0], reqLen > 1 ? req[1] : 0,
                  reqLen);
#endif
      return false;
    }
    if (respLen >= 3 && resp[0] == 0x7F)
    {
      if (resp[2] == KWP_NRC_RESPONSE_PENDING)
      { // the ECU is still working on it - the real answer follows without a new request
        wait = KWP_PENDING_TIMEOUT;
        continue;
      }
      k2000LastNrc = resp[2];
#if KL_DEBUG_ERRORS
      klDebugLine("KWP2000 request %02X %02X (%u bytes): refused, code %02X", req[0], reqLen > 1 ? req[1] : 0, reqLen,
                  resp[2]);
#endif
      return false; // negative response [7F][sid][code]
    }
    return true;
  }
  return false;
}

// 0.38.0: one more answer message to the request just made, with no new request. OBD-II on the K-line answers some
// requests in several messages (fault codes 3 to a message, the VIN in 5, and every emissions ECU answers 0x33 on its
// own), so after kwp2000Request the caller reads on until this says nothing more came within timeoutMs.
bool kwp2000ReadNext(uint8_t *resp, uint8_t &respLen, uint32_t timeoutMs, uint16_t respCap)
{
  respLen = 0;
  if (k2000Iso9141)
  {
    return iso9141ReadMessage(resp, respLen, timeoutMs, respCap);
  }
  return kwp2000ReadMessage(resp, respLen, timeoutMs, respCap);
}

// 0.19.0 diagnostic: send one request and hand back WHATEVER the ECU returned, even if the checksum didn't match
// (kwp2000Request throws a bad-checksum reply away). Used by the fast-path bench diagnostic to see the raw bytes our
// hooked RAM routine makes the ECU send. Returns the payload byte count; *checksumOk (if given) says whether it
// validated. No response-pending retry - it is a single read, on purpose, so what comes back is exactly one message.
uint8_t kwp2000RequestRaw(const uint8_t *req, uint8_t reqLen, uint8_t *resp, uint16_t respCap, bool *checksumOk,
                          uint32_t timeoutMs)
{
  if (checksumOk)
  {
    *checksumOk = false;
  }
  if (reqLen > 60)
  {
    return 0;
  }
  uint8_t buf[64];
  const uint8_t total = kwp2000FrameRequest(buf, req, reqLen);
  kwp2000SendFrame(buf, total);
  uint8_t respLen = 0;
  // kwp2000ReadMessage fills resp/respLen with the payload as it reads, then returns false if the checksum is wrong -
  // so the bytes are there whatever it returns. A timeout leaves respLen 0.
  const bool ok = kwp2000ReadMessage(resp, respLen, timeoutMs, respCap);
  if (checksumOk)
  {
    *checksumOk = ok;
  }
  return respLen;
}

bool kwp2000Connect(uint8_t ecuAddress, bool fastInit, uint8_t *keyBytesOut)
{
  k2000Up = false;
  k2000Iso9141 = false;
  k2000WithAddresses = true; // the wake-up's own messages carry addresses; a caller switches after it if it wants
  k2000Target = ecuAddress;
  // 0.17.4: ISO 14230 default gaps (P3 55 ms before each request, P4 5 ms between our bytes) until the caller sets its
  // own. The library default was 0 / 5, so after a 5-baud wake-up the first request went out at once.
  kwp2000SetTiming(55, 5);
  uint8_t resp[16];
  uint8_t respLen = 0;
  if (fastInit)
  {
    klFastInit();
    const uint8_t start[1] = {0x81}; // StartCommunication
    if (!kwp2000Request(start, 1, resp, respLen, 300, sizeof(resp)))
    {
#if KL_DEBUG_ERRORS
      klDebugLine("KWP2000 fast init to %02X: no answer to StartCommunication (NRC %02X)", ecuAddress, k2000LastNrc);
#endif
      return false;
    }
    if (respLen < 3 || resp[0] != 0xC1)
    {
#if KL_DEBUG_ERRORS
      klDebugLine("KWP2000 fast init to %02X: answer %02X (%u bytes) is not C1", ecuAddress, resp[0], respLen);
#endif
      return false;
    }
    if (keyBytesOut)
    {
      keyBytesOut[0] = resp[1];
      keyBytesOut[1] = resp[2];
    }
  }
  else
  {
    klSlowInit(ecuAddress, false); // KWP2000 / OBD: the plain address byte
    if (!klWaitSync())
    {
#if KL_DEBUG_ERRORS
      klDebugLine("KWP2000 5-baud init to %02X: no 0x55", ecuAddress);
#endif
      return false;
    }
    int keyByte1 = klRead(300);
    int keyByte2 = klRead(300);
    if (keyByte1 < 0 || keyByte2 < 0)
    {
#if KL_DEBUG_ERRORS
      klDebugLine("KWP2000 5-baud init to %02X: key bytes missing (%d %d)", ecuAddress, keyByte1, keyByte2);
#endif
      return false;
    }
    if (keyBytesOut)
    {
      keyBytesOut[0] = keyByte1;
      keyBytesOut[1] = keyByte2;
    }
    if (keyByte1 == 0x01 && keyByte2 == 0x8A)
    {
      // that's KWP1281. 0.17.4: checked before the complement goes back, so the module isn't left in a KWP1281
      // session we won't use (it then ignores the next wake-up)
#if KL_DEBUG_ERRORS
      klDebugLine("KWP2000 5-baud init to %02X: key bytes 01 8A = KWP1281", ecuAddress);
#endif
      return false;
    }
    delay(30); // W4
    uint8_t complement = ~keyByte2;
    klWrite(&complement, 1);
    int addressComplement = klRead(300); // ECU answers with ~address
#if KL_DEBUG_ERRORS
    klDebugLine("KWP2000 5-baud init to %02X: key bytes %02X %02X, address complement %d", ecuAddress, keyByte1, keyByte2,
                addressComplement);
#else
    (void)addressComplement;
#endif
    // 0.17.4: key bytes 08 08 / 94 94 = ISO 9141-2 (the bench ME7.5 answers the OBD address 0x33 this way): its own
    // message framing from here on (68 6A F1 ... out, 48 6B .. back)
    if ((keyByte1 == 0x08 && keyByte2 == 0x08) || (keyByte1 == 0x94 && keyByte2 == 0x94))
    {
      k2000Iso9141 = true;
    }
  }
  k2000Up = true;
  return true;
}

bool kwp2000ReadPid(uint8_t pid, uint8_t *data, uint8_t dataSize, uint8_t &len)
{
  if (!k2000Up)
  {
    return false;
  }
  uint8_t req[2] = {0x01, pid};
  uint8_t resp[16];
  uint8_t respLen = 0;
  if (!kwp2000Request(req, 2, resp, respLen, 300, sizeof(resp)))
  {
    return false;
  }
  if (respLen < 3 || resp[0] != 0x41 || resp[1] != pid)
  {
    return false;
  }
  if (respLen - 2 > dataSize)
  {
    return false; // longer than the caller's buffer (two ECUs' answers run together)
  }
  len = respLen - 2;
  memcpy(data, resp + 2, len);
  return true;
}

// 0.38.0: OBD-II fault codes over the K-line (ISO 15031-5): mode 03 = stored, 07 = pending. Each answer message is
// 43 / 47 then three codes of 2 bytes each (00 00 fills the places with no code); more codes, or a second ECU, come as
// more messages. The raw 2-byte codes go into codes[] (each once); answered = an ECU gave a positive answer at all.
uint8_t kwp2000ReadObdCodes(uint8_t mode, uint16_t *codes, uint8_t maxCodes, bool &answered)
{
  answered = false;
  if (!k2000Up)
  {
    return 0;
  }
  uint8_t count = 0;
  const uint8_t request[1] = {mode};
  uint8_t resp[64];
  uint8_t respLen = 0;
  if (!kwp2000Request(request, 1, resp, respLen, 1000, sizeof(resp)))
  {
    return 0; // no answer, or refused
  }
  bool gotMessage = true;
  while (gotMessage)
  {
    if (respLen >= 1 && resp[0] == (uint8_t)(mode + 0x40))
    {
      answered = true;
      uint8_t start = 1; // the first code's high byte
      if ((respLen - 1) % 2 == 1)
      {
        start = 2; // an odd byte after the service byte: a count of codes first (the CAN layout), skipped
      }
      for (uint8_t pos = start; pos + 1 < respLen; pos += 2)
      {
        const uint16_t code = (resp[pos] << 8) | resp[pos + 1]; // high byte, low byte
        if (code == 0 || count >= maxCodes)
        {
          continue; // 00 00 = no code in this place (or the list is full)
        }
        bool already = false; // the same code from a second ECU only once
        for (uint8_t i = 0; i < count; i++)
        {
          if (codes[i] == code)
          {
            already = true;
          }
        }
        if (!already)
        {
          codes[count] = code;
          count++;
        }
      }
    }
    gotMessage = kwp2000ReadNext(resp, respLen, 100, sizeof(resp)); // the next message, if there is one
  }
  return count;
}

// 0.38.0: clears the OBD-II fault codes (mode 04) of every emissions ECU listening on 0x33. true = one said 44 (done)
bool kwp2000ClearObdCodes()
{
  if (!k2000Up)
  {
    return false;
  }
  const uint8_t request[1] = {0x04}; // mode 04: clear fault codes and the MIL
  uint8_t resp[16];
  uint8_t respLen = 0;
  bool cleared = false;
  bool gotMessage = kwp2000Request(request, 1, resp, respLen, 2000, sizeof(resp));
  while (gotMessage)
  {
    if (respLen >= 1 && resp[0] == 0x44)
    {
      cleared = true;
    }
    gotMessage = kwp2000ReadNext(resp, respLen, 100, sizeof(resp)); // a second ECU's answer
  }
  return cleared;
}

// 0.38.0: the VIN over the K-line (mode 09 PID 02). ISO 9141 / KWP2000 send it as 5 messages, each 49 02 <message
// number 1-5> and 4 bytes (the first message is 3 filler bytes and the VIN's first letter); some ECUs send it as one
// long message (49 02 01 and the 17 characters). The characters are put back in order, fillers dropped.
bool kwp2000ReadObdVin(char *vin, uint8_t vinLen)
{
  vin[0] = '\0';
  if (!k2000Up || vinLen < 18)
  {
    return false;
  }
  const uint8_t request[2] = {0x09, 0x02}; // mode 09: vehicle information, PID 02: the VIN
  uint8_t resp[64];
  uint8_t respLen = 0;
  if (!kwp2000Request(request, 2, resp, respLen, 1000, sizeof(resp)))
  {
    return false;
  }
  uint8_t bytes[40]; // the VIN's bytes in order, fillers included
  memset(bytes, 0, sizeof(bytes));
  uint8_t byteCount = 0;
  bool gotMessage = true;
  while (gotMessage)
  {
    if (respLen == 7 && resp[0] == 0x49 && resp[1] == 0x02 && resp[2] >= 1 && resp[2] <= 8)
    {
      const uint8_t place = (resp[2] - 1) * 4; // message n carries bytes 4(n-1) .. 4(n-1)+3
      memcpy(&bytes[place], &resp[3], 4);
      if (place + 4 > byteCount)
      {
        byteCount = place + 4;
      }
    }
    else if (respLen > 7 && resp[0] == 0x49 && resp[1] == 0x02)
    {
      byteCount = respLen - 3; // one long message: everything after 49 02 <count>
      if (byteCount > sizeof(bytes))
      {
        byteCount = sizeof(bytes);
      }
      memcpy(bytes, &resp[3], byteCount);
    }
    gotMessage = kwp2000ReadNext(resp, respLen, 100, sizeof(resp)); // the next of the 5
  }
  // the letters and digits only (a VIN is A-Z and 0-9), the last 17 of them
  char letters[41];
  uint8_t letterCount = 0;
  for (uint8_t i = 0; i < byteCount; i++)
  {
    const char c = (char)bytes[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))
    {
      letters[letterCount] = c;
      letterCount++;
    }
  }
  if (letterCount < 17)
  {
    return false; // not a whole VIN
  }
  memcpy(vin, &letters[letterCount - 17], 17);
  vin[17] = '\0';
  return true;
}

bool kwp2000TesterPresent()
{
  if (!k2000Up)
  {
    return false;
  }
  uint8_t req[1] = {0x3E};
  uint8_t resp[8];
  uint8_t respLen;
  return kwp2000Request(req, 1, resp, respLen, 300, sizeof(resp));
}

// 0.17.2: AccessTimingParameters (service 0x83). ISO 14230-3 timing parameter ids: 0x00 = read the ECU's limits,
// 0x01 = back to defaults, 0x02 = read the current values, 0x03 = set values. (The ME7 logger used to send 0x02,
// which only reads, so nothing was ever set.) Asks for the limits, then sets them. The five values, in ISO units:
// P2min (x 0.5 ms), P2max (x 25 ms), P3min (x 0.5 ms), P3max (x 250 ms), P4min (x 0.5 ms).
// If the ECU won't say its limits, fallbackValues (5 bytes) are set instead; NULL = leave the timing alone.
// Once the ECU takes a set, our gap before each request becomes its P3min and our gap between bytes testerP4Ms.
bool kwp2000AccessTiming(const uint8_t *fallbackValues, uint8_t testerP4Ms)
{
  if (!k2000Up)
  {
    return false;
  }
  uint8_t resp[16];
  uint8_t respLen = 0;
  uint8_t timingValues[5];
  bool haveValues = false;
  uint8_t readLimits[2] = {0x83, 0x00}; // 0x00 = read limits
  // answer: [C3][00][P2min][P2max][P3min][P3max][P4min]
  if (kwp2000Request(readLimits, 2, resp, respLen, 400, sizeof(resp)) && respLen >= 7 && resp[0] == 0xC3 && resp[1] == 0x00)
  {
    memcpy(timingValues, resp + 2, 5);
    haveValues = true;
  }
  else if (fallbackValues)
  {
    memcpy(timingValues, fallbackValues, 5);
    haveValues = true;
  }
  if (!haveValues)
  {
    return false;
  }
  uint8_t setValues[7] = {0x83, 0x03, timingValues[0], timingValues[1], timingValues[2], timingValues[3], timingValues[4]};
  if (!kwp2000Request(setValues, 7, resp, respLen, 400, sizeof(resp)))
  {
    return false; // refused: the ECU keeps its own timing, and so do we
  }
  // P3min and P4min are in 0.5 ms steps. 0.17.4: our gap between bytes is the ECU's own P4min when that is longer
  // than testerP4Ms (rounded up); the ME7 logger now asks for 0 and gets what the ECU allows (0 on an ME7.5)
  uint8_t p4Ms = (uint8_t)((timingValues[4] + 1) / 2);
  if (p4Ms < testerP4Ms)
  {
    p4Ms = testerP4Ms;
  }
  kwp2000SetTiming(timingValues[2] / 2, p4Ms);
#if KL_DEBUG_ERRORS
  klDebugLine("KWP2000 timing set: P2min %u P2max %u P3min %u P3max %u P4min %u (raw) - our P3 %u ms, P4 %u ms",
              timingValues[0], timingValues[1], timingValues[2], timingValues[3], timingValues[4], timingValues[2] / 2,
              p4Ms);
#endif
  return true;
}

void kwp2000Disconnect()
{
  if (k2000Up && !k2000Iso9141) // ISO 9141-2 has no StopCommunication: the ECU ends it after 5 s without a request
  {
    uint8_t req[1] = {0x82}; // StopCommunication
    uint8_t resp[8];
    uint8_t respLen;
    kwp2000Request(req, 1, resp, respLen, 200, sizeof(resp));
  }
  if (k2000Up && k2000Iso9141)
  {
    // 0.17.4: ISO 9141-2 has no StopCommunication - the ECU holds the session until P3max (5 s) passes without a
    // request, and ignores any wake-up until then (the bench ME7.5's flash read, started straight after an OBD
    // session, got no 0x55). So the line stays quiet until the ECU has let go.
    const uint32_t quietMs = millis() - klLastRxMs();
    if (quietMs < ISO9141_RELEASE_MS)
    {
      delay(ISO9141_RELEASE_MS - quietMs);
    }
  }
  k2000Up = false;
}

bool kwp2000Connected()
{
  return k2000Up;
}

uint8_t kwp2000Target()
{
  return k2000Target;
}

void kwp2000SetTarget(uint8_t target)
{
  k2000Target = target;
}

void kwp2000SetAddressBytes(bool withAddresses)
{
  k2000WithAddresses = withAddresses;
}
