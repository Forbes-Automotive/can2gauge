#pragma once
#include <can2gauge_defs.h>

// K-line task: runs whichever protocol settings.klineProtocol selects and feeds the signal catalogue
void setupKLine();
void klineRestart();            // after a settings change (protocol / files)
void klineInfoProvider(char *buf, size_t len);

// 0.24.1 bench: listen on the K-line without sending - an OBD dongle drives it - and time every byte, to see where an
// OBD request's time goes (the ECU's answer delay, the gaps between bytes and between requests). Only while the
// gauge's own K-line protocol is off. USB console: POST /api/kline/listen {"ms":3000}, then GET /api/kline/listen.
#define KL_LISTEN_MAX 3000 // bytes kept per listen (PSRAM)
bool klineListenStart(uint32_t ms);  // false = the K-line is in use (protocol on) or already listening
bool klineListenDone();
uint16_t klineListenCount();
void klineListenByte(uint16_t index, uint32_t &microsFromStart, uint8_t &value);
// ME7 logger controls (web UI)
#define ME7_MAX_SPS 50 // the most samples a second the ME7 logger aims for (the web UI's Samples/s, as ME7Logger's 1-50)
const char *me7StartLogging(); // 0.17.6: returns what happened, for the web UI ("logging", "starts when ... connects")
void me7StopLogging();
uint8_t me7SamplesWanted(); // 0.17.6: the samples a second the logger aims for (the web UI's Samples/s)

// 0.17.6: ME7 logger bench probe (USB console, tools/diag/kline_bench.py): one hand-made KWP2000 request sent inside
// the running logger session, between two samples, `repeats` times - to try what the ECU accepts and time its answers
struct Me7Probe
{
  bool pending;          // queued, not run yet
  bool done;             // run: the fields below are filled
  bool ok;               // the last try got a positive answer
  uint8_t nrc;           // its negative response code (7F xx code), 0 = none
  uint8_t answer[255];   // the last answer's data (no header / checksum)
  uint8_t answerLength;  // how many bytes of it
  uint16_t repeats;      // tries asked for
  uint16_t answered;     // tries that got any answer (positive or negative)
  uint32_t sendUs;       // the last try: sending the request (and its echo)
  uint32_t waitMinUs;    // the ECU's wait before its first answer byte: shortest,
  uint32_t waitAvgUs;    // average
  uint32_t waitMaxUs;    // and longest over the tries
  uint32_t answerUs;     // the last try: receiving the answer
  uint32_t cycleAvgUs;   // average time per try, start to end
};
bool me7ProbeQueue(const uint8_t *request, uint8_t length, uint16_t repeats); // false = the logger isn't logging
const Me7Probe &me7ProbeResult();

// 0.19.0: ME7 fast-path bench diagnostic (USB console). One bounded run inside the logger session: install + hook the
// Test-link routine, then send 3E / a RAM read / B7 / BA and record the RAW reply bytes (even when the checksum fails,
// which it does when our hooked routine frames its answer wrongly), then always un-hook. It settles whether a hooked
// B7 reaches our handler (the reply changes) or the hook just breaks dispatch (everything stops answering).
struct Me7DiagStep
{
  char label[12];     // "version", "3E hooked", "B7 hooked", ...
  uint8_t request[8]; // the request bytes sent
  uint8_t requestLen; // how many
  uint8_t answer[48]; // the raw reply payload (no header / checksum), whatever came back
  uint8_t answerLen;  // how many bytes came back (0 = nothing / timeout)
  bool checksumOk;    // did the reply's checksum validate (a clean message) or not (garbage / our mis-frame)
};
struct Me7FastDiag
{
  bool pending;           // queued, not run yet
  bool done;              // run: the fields below are filled
  bool installed;         // the blob write + hook succeeded
  char installMsg[64];    // the install status line (includes which pointer was hooked)
  Me7DiagStep steps[16];  // the probes, in order
  uint8_t stepCount;      // how many are filled
  uint32_t testAddrs[8];  // 0.19.0: the var addresses to install v1 with (empty = the logger's own variables)
  uint8_t testAddrCount;  // how many
};
bool me7FastDiagQueue(const uint32_t *addrs, uint8_t count); // addrs/count override the var list (count 0 = logger's)
const Me7FastDiag &me7FastDiagResult();
// 0.19.0 "Try KWP2000": double slow-init at 0x01 (KWP1281 then KWP2000). true = a KWP2000 session is up, keyBytes
// filled. Call from the K-line task. Connect half only; the KWP2000 read path for K-line mode is still to come.
bool kwp2000TryDoubleInit(uint8_t keyBytes[2]);
// KWP1281 group -> signal map (defaults for ME7; editable from the web UI later)
struct KwpGroupMap
{
  uint8_t group;
  uint8_t field; // 0..3
  uint8_t signal;
};
// 0.8.0: the same engine block map for VAG KWP2000 measuring blocks read through the OBD adapter's K-line (ELM)
uint8_t klineEngineGroups(const uint8_t **groups); // the blocks the engine logging cycles through, returns the count
void klineApplyGroup(uint8_t group, const float *values, uint8_t count); // decoded field values -> signals
