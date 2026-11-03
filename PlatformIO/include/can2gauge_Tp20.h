#pragma once
#include <can2gauge_DiagLink.h>
#include <can2gauge_DSG.h> // DsgField

// ---------------------------------------------------------------------------
// Tp20Client (0.8.0) - KWP2000 over VW TP2.0 (PQ35 era), moved out of can2gauge_DSG.cpp so the same client runs on the
// gauge's CAN or through the BLE ELM (DiagLink). The protocol rules are unchanged from 0.4.0 and follow a bench capture
// on a Mk5 02E DQ250 (see the notes at the top of can2gauge_DSG.cpp):
//   open(module) - channel setup on 0x200 -> answer on 0x200 + module, then the A0 / A1 channel parameters
//   transact()   - one KWP2000 message out, one back, with ACKs, the ~200 ms repeat, then A3 + one more repeat,
//                  and 7F xx 78 waited out
//   keepAlive()  - A3 when the channel has been idle config.idleMs
//   readGroup()  - KWP 21 <group> -> 61 <group> [formula a b] x n, decoded with kwp1281Decode()
// ---------------------------------------------------------------------------

struct Tp20Config
{
  uint32_t repeatMs = 200; // no ACK / data this long after a send -> send the same frames again (capture ~200 ms)
  uint32_t idleMs = 500;   // keepAlive() sends A3 after this much idle (capture ~550 ms)
  uint8_t params[6] = {0xA0, 0x0F, 0x8A, 0xFF, 0x32, 0xFF}; // A0: block size 15, T1 100 ms, T3 5 ms (as the capture)
  // 0.37.9: the CAN id the module is asked to answer on. Each client on the gauge's CAN has its own (the DSG 0x300, the
  // VAG diag task 0x301), so two channels open at once (gearbox + a diagnostics module) never take each other's frames
  uint16_t testerRxId = 0x300;
  // 0.37.9: on the gauge's CAN, whose turn each exchange takes (CAN_DIAG_VDIAG / CAN_DIAG_DSG, CANLine.h); 0 = none
  // (through the OBD adapter: the adapter is its own line)
  uint8_t canTurnOwner = 0;
};

struct Tp20Stats
{
  uint32_t sent, received, repeats, channelTests, acks;
};

class Tp20Client
{
public:
  explicit Tp20Client(DiagLink &link) : link(link) {}
  Tp20Config config;
  Tp20Stats stats = {};
  DiagTraceFn trace = nullptr;

  bool open(uint8_t module);
  void close();             // A8 when open
  void drop()
  {
    chOpen = false;
  } // forget the channel without sending (link reset, adapter re-init)

  bool isOpen() const
  {
    return chOpen;
  }

  uint16_t txId() const
  {
    return txChannelId;
  }

  uint16_t rxId() const
  {
    return rxChannelId;
  }

  bool transact(const uint8_t *request, uint16_t requestLen, uint8_t *response, uint16_t capacity, uint16_t &responseLen,
                uint32_t timeoutMs);
  bool keepAlive();         // false = the ECU did not answer A3 (channel closed)
  // true = 61 <group> parsed into fields; nrc = the 7F code when the ECU refused (0 otherwise)
  bool readGroup(uint8_t group, DsgField *fields, uint8_t &count, uint8_t &nrc);

private:
  DiagLink &link;
  uint16_t txChannelId = 0;
  uint16_t rxChannelId = 0;
  uint8_t txSeq = 0;
  uint8_t lastRxSeq = 0;   // 0.17.2: sequence number of the last data frame taken from the ECU
  bool rxSeqKnown = false; // 0.17.2: lastRxSeq is valid (a data frame has arrived on this channel)
  bool chOpen = false;
  uint32_t lastTx = 0;
  bool sendFrame(uint16_t id, const uint8_t *data, uint8_t len);
  bool recvFrame(DiagFrame &frame, uint32_t waitMs);
  // 0.37.9: open / transact / keepAlive themselves, run by the public ones with the CAN turn taken (see canTurnOwner)
  bool openOnTurn(uint8_t module);
  bool transactOnTurn(const uint8_t *request, uint16_t requestLen, uint8_t *response, uint16_t capacity,
                      uint16_t &responseLen, uint32_t timeoutMs);
  bool keepAliveOnTurn();
  bool takeTurn();
  void giveTurn();
  void note(const char *text)
  {
    if (trace)
    {
      trace('#', 0, NULL, 0, text);
    }
  }
};
