#pragma once
#include <can2gauge_defs.h>
#include <ArduinoJson.h>
#include <can2gauge_Labels.h> // LBL_NAME_LEN: how long a label file's name can be

// ---------------------------------------------------------------------------
// Car scan (0.37.0) - the gauge's half of the web UI's "Scan My Car". The web UI decides what to ask (it has the car's
// make from the VIN, the DID library and the label files); the gauge asks it and hands back what came back, one job
// at a time. The web UI then keeps only what answered and offers it as the "Car" source on the Gauges tab.
// Jobs:
//   uds          a list of requests, each to its own module (ids, extended address bytes) and up to 7 bytes long:
//                22 F187 to find the modules, 01 00 / 01 20 ... for the OBD PID lists, 22 <DID> to try each DID.
//                Over the gauge's CAN or through the OBD adapter. Run by the VAG diag task.
//   tp20Modules  PQ-era modules: a TP2.0 channel to each address, then 1A 9B (part number and name). CAN only.
//   tp20Blocks   one module's measuring blocks over TP2.0: 21 <block> for each block asked (CAN only)
//   klineModules the K-line: a 5-baud wake-up at each address (KWP1281, else KWP2000), its identification
//   klineBlocks  one module's measuring blocks over the K-line. Run by the K-line task.
//   klineObd     0.38.0: OBD-II over the K-line, any make: one wake-up of the OBD address 0x33, then a list of requests
//                like uds (01 00, 01 20 ... the PID lists), and the VIN (mode 09) for Your Car. Run by the K-line task.
// A KWP module's label file (the names of its blocks and fields) is picked from its identification, as the
// K-line browse does, and named in the results.
// ---------------------------------------------------------------------------

#define CARSCAN_MAX_ITEMS 255   // requests / modules / blocks in one job (a whole module's blocks 1-255 fit)
#define CARSCAN_ANSWER_KEEP 40  // bytes kept of each UDS answer (its whole length is kept too)

enum carScanJob_t : uint8_t
{
  CARSCAN_JOB_NONE = 0,
  CARSCAN_JOB_UDS,           // UDS / OBD requests, each to its own module
  CARSCAN_JOB_TP20_MODULES,  // which PQ-era modules open a TP2.0 channel
  CARSCAN_JOB_TP20_BLOCKS,   // one TP2.0 module's measuring blocks
  CARSCAN_JOB_KLINE_MODULES, // which modules answer on the K-line
  CARSCAN_JOB_KLINE_BLOCKS,  // one K-line module's measuring blocks
  CARSCAN_JOB_KLINE_OBD,     // 0.38.0: OBD-II requests over the K-line (to 0x33), any make
};

// what came back for one item
#define CARSCAN_WAITING 0   // not asked yet
#define CARSCAN_ANSWERED 1  // a positive answer (UDS 0x40 + the service, a module that opened, a block with fields)
#define CARSCAN_REFUSED 2   // a negative answer (UDS 7F, a refused block): the module is there, the value isn't
#define CARSCAN_NO_ANSWER 3 // nothing came back

// one request (uds), module (xxModules) or block (xxBlocks) and its answer
struct CarScanItem
{
  // asked (uds)
  uint32_t txId;            // request id, 11-bit or 29-bit
  uint32_t rxId;            // answer id, 0 = worked out from the request id
  uint8_t txExt;            // extended address byte in front of the request (BMW), 0 = none
  uint8_t rxExt;            // extended address byte in front of the answer (BMW: F1), 0 = none
  uint8_t request[7];       // the request bytes (one CAN frame's worth: what the OBD adapter can send)
  uint8_t requestLen;       // how many
  // asked (modules / blocks)
  uint8_t number;           // the module address, or the block number
  // the answer
  uint8_t state;            // CARSCAN_WAITING / ANSWERED / REFUSED / NO_ANSWER
  uint8_t nrc;              // the negative response code of a refusal (0x31 out of range ...)
  uint16_t answerLen;       // uds: the whole answer's length (only CARSCAN_ANSWER_KEEP bytes are kept)
  uint8_t answer[CARSCAN_ANSWER_KEEP]; // uds: the answer's first bytes (62 F1 87 ... / 41 00 ...)
  char text[48];            // modules: the identification (part number and name)
  char labelFile[LBL_NAME_LEN];       // modules: the label file it matched ("" = none)
  uint8_t fieldCount;       // blocks: fields in the block (0..4)
  uint8_t formula[4];       // blocks: each field's formula byte and its two data bytes, as sent
  uint8_t a[4];
  uint8_t b[4];
  float value[4];           // blocks: each field decoded
  char unit[4][8];          // blocks: each field's unit (or text)
};

struct CarScanStatus
{
  uint8_t job;              // carScanJob_t running now, or the last one
  uint8_t via;              // uds: VDIAG_LINK_CAN or VDIAG_LINK_ADAPTER
  bool busy;                // a job is waiting or running
  bool done;                // the last job finished (its results are complete)
  bool stopRequested;       // the web UI asked it to stop
  uint8_t address;          // xxBlocks: the module
  char moduleId[48];        // xxBlocks: the module's identification; klineObd: the protocol and key bytes
  char labelFile[LBL_NAME_LEN];       // xxBlocks: the label file it matched ("" = none)
  char state[80];           // what it's doing / how it ended
  uint16_t itemCount;       // items in the job
  uint16_t itemsDone;       // items asked so far
};

// the web UI (POST / GET /api/carscan)
bool carScanStart(JsonObjectConst body, char *why, size_t whyLen); // false (and why) if it can't start
void carScanStop();
void carScanToJson(JsonDocument &doc, bool withItems); // withItems false = the progress only (the web UI polls it)

// for the tasks that run the jobs
bool carScanCanPending();       // the VAG diag task should run the waiting uds / tp20 job
void carScanCanTaken();         // ... and has taken it (it runs it straight away)
bool carScanCanBusy();          // a CAN job is waiting or running: the OBD polling and the DSG stand back
bool carScanAdapterBusy();      // a uds job through the OBD adapter is waiting or running
bool carScanKlinePending();     // the K-line task should run the waiting K-line job
void carScanKlineRun();         // runs it (in the K-line task)
CarScanStatus &carScanStatus(); // the job and its progress (the runner fills it in)
CarScanItem &carScanItem(uint16_t index);
bool carScanStopWanted();       // the web UI asked the running job to stop
void carScanSetState(const char *fmt, ...) __attribute__((format(printf, 1, 2))); // (the compiler checks the % codes)
void carScanFinish();           // the job is over
