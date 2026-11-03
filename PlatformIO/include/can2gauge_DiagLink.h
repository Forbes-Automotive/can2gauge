#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// DiagLink (0.8.0) - a raw 11-bit CAN frame pipe for the diagnostic clients, so one client (VW TP2.0,
// can2gauge_Tp20.cpp) runs over either
//   - the gauge's own TWAI bus (TwaiLink in can2gauge_DSG.cpp), or
//   - the BLE ELM327 in raw mode, ATCAF0 (ElmRawLink in can2gauge_ELM.cpp).
//   listen(id) - the one id recv() returns (0 = none). On the ELM this becomes ATCRA, applied with the next send
//   send()     - one frame at its real length (no padding). On the ELM, send() blocks for the adapter's answer window
//                (ATST), and every frame heard in that window is queued for recv(). The ELM only listens while a
//                command runs, so a frame that arrives after the window is lost. The ACK / repeat rules of the client
//                cover that.
//   recv()     - the next queued frame, false after waitMs without one
//   flush()    - drop the queued frames
// ---------------------------------------------------------------------------

struct DiagFrame
{
  uint16_t id;
  uint8_t len;
  uint8_t data[8];
};

class DiagLink
{
public:
  virtual bool send(uint16_t id, const uint8_t *data, uint8_t len) = 0;
  virtual bool recv(DiagFrame &frame, uint32_t waitMs) = 0;
  virtual void listen(uint16_t id) = 0;
  virtual void flush() = 0;
  virtual ~DiagLink() {}
};

// optional trace for the bench logs: dir '>' = sent, '<' = received, '#' = a note from the client (data may be NULL)
typedef void (*DiagTraceFn)(char dir, uint16_t id, const uint8_t *data, uint8_t len, const char *note);
