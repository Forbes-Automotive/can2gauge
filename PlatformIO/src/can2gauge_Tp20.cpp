#include <can2gauge_Tp20.h>
#include <KLine.h> // kwp1281Decode(): TP2.0 measuring blocks use the same formula triplets as KWP1281
#include <CANLine.h> // 0.37.9: canDiagWait / canDiagRelease - the turn to ask a question on the gauge's CAN

// Moved from can2gauge_DSG.cpp (0.4.0) in 0.8.0. The rules are unchanged; queue / canSend calls became DiagLink calls.

bool Tp20Client::sendFrame(uint16_t id, const uint8_t *data, uint8_t len)
{
  if (trace)
  {
    trace('>', id, data, len, NULL);
  }
  stats.sent++;
  if (id == txChannelId)
  {
    lastTx = millis();
  }
  return link.send(id, data, len); // TP2.0 frames carry their real length (no padding), as in the capture
}

bool Tp20Client::recvFrame(DiagFrame &frame, uint32_t waitMs)
{
  if (!link.recv(frame, waitMs))
  {
    return false;
  }
  stats.received++;
  if (trace)
  {
    trace('<', frame.id, frame.data, frame.len, NULL);
  }
  return true;
}

// 0.37.9: the CAN turn for one exchange (CANLine.h): a client on the gauge's CAN waits up to 1 s for it
bool Tp20Client::takeTurn()
{
  if (config.canTurnOwner == 0)
  {
    return true; // through the adapter: no turn needed
  }
  return canDiagWait(config.canTurnOwner, 6000, 1000);
}

void Tp20Client::giveTurn()
{
  if (config.canTurnOwner != 0)
  {
    canDiagRelease(config.canTurnOwner);
  }
}

bool Tp20Client::open(uint8_t module)
{
  if (!takeTurn())
  {
    return false; // another task held the CAN too long: the caller tries again later
  }
  const bool ok = openOnTurn(module);
  giveTurn();
  return ok;
}

bool Tp20Client::transact(const uint8_t *request, uint16_t requestLen, uint8_t *response, uint16_t capacity,
                          uint16_t &responseLen, uint32_t timeoutMs)
{
  if (!takeTurn())
  {
    return false;
  }
  if (chOpen)
  {
    link.listen(rxChannelId); // 0.37.9: another exchange (a UDS module) may have moved the filter away from our channel
  }
  const bool ok = transactOnTurn(request, requestLen, response, capacity, responseLen, timeoutMs);
  giveTurn();
  return ok;
}

bool Tp20Client::keepAlive()
{
  if (!takeTurn())
  {
    return chOpen; // no turn now: try at the next go (the channel isn't known to be gone)
  }
  if (chOpen)
  {
    link.listen(rxChannelId); // 0.37.9: as transact
  }
  const bool ok = keepAliveOnTurn();
  giveTurn();
  return ok;
}

bool Tp20Client::openOnTurn(uint8_t module)
{
  chOpen = false;
  link.listen(0x200 + module);
  link.flush();
  // channel setup: [dest][C0][rx id 0x000, invalid][tx id 0x300][app type 01] - we propose to receive on 0x300; the
  // ECU answers on 0x200+dest with [00][D0][rx id][tx id][01]: we then listen on rx id and send on tx id
  // 0.37.9: our receive id from config.testerRxId (low byte, then the top 3 bits); 0x300 = the 00 03 seen in the capture
  const uint8_t request[7] = {module, 0xC0, 0x00, 0x10, (uint8_t)(config.testerRxId & 0xFF),
                              (uint8_t)((config.testerRxId >> 8) & 0x07), 0x01};
  DiagFrame frame;
  bool ok = false;

  // the capture got one D8 (refused) and succeeded by repeating the request straight away: allow one repeat
  for (int attempt = 0; attempt < 2 && !ok; attempt++)
  {
    if (!sendFrame(0x200, request, 7))
    {
      return false;
    }
    const uint32_t setupSentAt = millis();
    while (millis() - setupSentAt < 500)
    {
      if (!recvFrame(frame, 10))
      {
        continue;
      }
      if (frame.len >= 6 && frame.data[1] == 0xD0 && !(frame.data[5] & 0x10))
      {
        rxChannelId = frame.data[2] | ((frame.data[3] & 0x07) << 8);
        txChannelId = frame.data[4] | ((frame.data[5] & 0x07) << 8);
        ok = true;
        break;
      }
      if (frame.len >= 2 && (frame.data[1] & 0xF0) == 0xD0)
      {
        note("channel setup refused (D6..D8)");
        vTaskDelay(pdMS_TO_TICKS(5));
        break;
      }
    }
  }
  if (!ok)
  {
    return false;
  }

  link.listen(rxChannelId);
  link.flush();
  // channel parameters (config.params, from the capture: block size 15, T1 100 ms, T3 5 ms)
  sendFrame(txChannelId, config.params, 6);
  const uint32_t paramsSentAt = millis();
  while (millis() - paramsSentAt < 500)
  {
    if (recvFrame(frame, 10) && frame.data[0] == 0xA1)
    {
      txSeq = 0;
      rxSeqKnown = false; // 0.17.2: the ECU's sequence numbers start again with each new channel
      chOpen = true;
      return true;
    }
  }
  note("no A1 to the channel parameters");
  return false;
}

void Tp20Client::close()
{
  if (chOpen)
  {
    const uint8_t disconnect[1] = {0xA8};
    sendFrame(txChannelId, disconnect, 1);
  }
  chOpen = false;
  link.listen(0);
}

// one KWP2000 message out, one back. Frames: first [op|seq][len hi][len lo][data..5], then [op|seq][data..7];
// op 0x1 = last, ACK expected; 0x2 = more to come, no ACK; 0x0 = more, ACK expected (block size); 0x3 = last, no ACK.
// ACK = [B0 | next expected seq]. 9x = ACK "not ready".
bool Tp20Client::transactOnTurn(const uint8_t *request, uint16_t requestLen, uint8_t *response, uint16_t capacity,
                                uint16_t &responseLen, uint32_t timeoutMs)
{
  if (!chOpen)
  {
    return false;
  }
  link.flush();

  // build the frames once so an unanswered message can be sent again unchanged (same sequence numbers), as the capture does
  uint8_t frames[6][8], lens[6], frameCount = 0;
  uint16_t offset = 0;
  bool first = true;
  while ((first || offset < requestLen) && frameCount < 6)
  {
    uint8_t *txFrame = frames[frameCount];
    uint8_t frameLen = 1;
    if (first)
    {
      txFrame[1] = requestLen >> 8;
      txFrame[2] = requestLen & 0xFF;
      frameLen = 3;
    }
    uint16_t chunk = requestLen - offset;
    if (chunk > (uint16_t)(8 - frameLen))
    {
      chunk = 8 - frameLen;
    }
    memcpy(&txFrame[frameLen], request + offset, chunk);
    offset += chunk;
    frameLen += chunk;
    const bool last = offset >= requestLen;
    txFrame[0] = (last ? 0x10 : 0x20) | (txSeq & 0x0F);
    txSeq = (txSeq + 1) & 0x0F;
    lens[frameCount++] = frameLen;
    first = false;
  }
  auto sendAll = [&]() -> bool
  {
    for (uint8_t frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
      if (!sendFrame(txChannelId, frames[frameIndex], lens[frameIndex]))
      {
        return false;
      }
    }
    return true;
  };
  if (!sendAll())
  {
    return false;
  }

  uint32_t deadline = millis() + timeoutMs;
  uint32_t sentAt = millis();
  uint8_t retries = 0;   // 0 = first send, 1 = repeated, 2 = repeated after an A3 / A1 exchange
  bool answered = false; // an ACK or data from the ECU since the last send
  bool started = false;
  uint16_t total = 0;
  uint16_t received = 0;
  DiagFrame frame;

  while ((int32_t)(deadline - millis()) > 0)
  {
    if (!recvFrame(frame, 10))
    {
      // no reaction in config.repeatMs: the capture repeats the same frame, then tests the channel with A3 and repeats once more
      if (!answered && millis() - sentAt >= config.repeatMs && retries < 2)
      {
        if (retries == 1)
        {
          const uint8_t channelTest[1] = {0xA3};
          stats.channelTests++;
          sendFrame(txChannelId, channelTest, 1);
          const uint32_t testSentAt = millis();
          bool gotA1 = false;
          while (!gotA1 && millis() - testSentAt < 300)
          {
            gotA1 = recvFrame(frame, 10) && frame.data[0] == 0xA1;
          }
          if (!gotA1)
          {
            note("no A1 to the channel test - channel lost");
            chOpen = false;
            return false;
          }
        }
        retries++;
        stats.repeats++;
        note("no reaction - same frames again");
        if (!sendAll())
        {
          return false;
        }
        sentAt = millis();
        deadline = millis() + timeoutMs;
      }
      continue;
    }

    const uint8_t op = frame.data[0] >> 4;
    const uint8_t seq = frame.data[0] & 0x0F;
    if (op == 0xB || op == 0x9)
    {
      answered = true;
      continue; // ACK of our request (0x9 = not ready yet)
    }
    if (frame.data[0] == 0xA8 || frame.data[0] == 0xA3)
    {
      // A8: the ECU closed the channel. A3 from the ECU: in the capture it came after requests went unanswered and
      // the capture answered it by setting up a new channel - do the same
      note(frame.data[0] == 0xA8 ? "ECU closed the channel (A8)" : "ECU sent A3 - channel in trouble");
      chOpen = false;
      return false;
    }
    if (op > 0x3)
    {
      continue;
    }

    // 0.17.2: the same sequence number as the last data frame we took = the ECU sending it again because our ACK went
    // missing (it repeats after T1, 100 ms). ACK it again and drop it - before, a repeat that landed after flush() was
    // read as the start of the next answer, and every answer after it came one request late.
    if (rxSeqKnown && seq == lastRxSeq)
    {
      if (op == 0x0 || op == 0x1)
      {
        const uint8_t repeatAck[1] = {(uint8_t)(0xB0 | ((seq + 1) & 0x0F))};
        stats.acks++;
        sendFrame(txChannelId, repeatAck, 1);
      }
      note("ECU repeated a frame - ACKed again, dropped");
      continue;
    }
    rxSeqKnown = true;
    lastRxSeq = seq;

    answered = true;
    uint8_t pos = 1;
    if (!started)
    {
      if (frame.len < 3)
      {
        continue;
      }
      total = ((uint16_t)frame.data[1] << 8) | frame.data[2];
      pos = 3;
      started = true;
      received = 0;
    }
    for (; pos < frame.len && received < total; pos++)
    {
      if (received < capacity)
      {
        response[received++] = frame.data[pos];
      }
      else
      {
        received++;
      }
    }
    if (op == 0x0 || op == 0x1)
    {
      const uint8_t ack[1] = {(uint8_t)(0xB0 | ((seq + 1) & 0x0F))};
      stats.acks++;
      sendFrame(txChannelId, ack, 1);
    }
    if (op == 0x1 || op == 0x3 || received >= total)
    {
      responseLen = received < capacity ? received : capacity;
      if (responseLen >= 3 && response[0] == 0x7F && response[2] == 0x78)
      {
        started = false; // response pending: wait for the real answer
        deadline = millis() + 5000;
        continue;
      }
      return true;
    }
    deadline = millis() + 1000;
  }
  return false;
}

bool Tp20Client::keepAliveOnTurn()
{
  if (!chOpen || millis() - lastTx < config.idleMs)
  {
    return chOpen;
  }
  const uint8_t channelTest[1] = {0xA3}; // channel test -> A1 + parameters
  stats.channelTests++;
  sendFrame(txChannelId, channelTest, 1);
  DiagFrame frame;
  const uint32_t testSentAt = millis();
  while (millis() - testSentAt < 300)
  {
    if (recvFrame(frame, 10) && frame.data[0] == 0xA1)
    {
      return true;
    }
  }
  note("no A1 to the keep-alive - channel lost");
  chOpen = false;
  return false;
}

bool Tp20Client::readGroup(uint8_t group, DsgField *fields, uint8_t &count, uint8_t &nrc)
{
  const uint8_t request[2] = {0x21, group};
  uint8_t response[64];
  uint16_t responseLen = 0;
  nrc = 0;
  count = 0;
  if (!transact(request, 2, response, sizeof(response), responseLen, 500))
  {
    return false;
  }
  if (responseLen >= 3 && response[0] == 0x7F)
  {
    nrc = response[2];
    return false;
  }
  if (responseLen < 2 || response[0] != 0x61 || response[1] != group)
  {
    return false;
  }
  for (uint16_t byteIndex = 2; byteIndex + 2 < responseLen && count < 4; byteIndex += 3, count++)
  {
    DsgField &field = fields[count];
    field.formula = response[byteIndex];
    field.a = response[byteIndex + 1];
    field.b = response[byteIndex + 2];
    field.value = kwp1281Decode(field.formula, field.a, field.b, field.unit, sizeof(field.unit));
  }
  return true;
}
