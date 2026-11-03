#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// Service (0.16.0) - fault codes and basic settings over the gauge's own K-line and CAN, without an OBD adapter:
//   Read Faults / Clear Faults
//     K-line: KWP1281 (Mk4-era modules: block 0x07 read, 0x05 clear), else KWP2000 after the same 5-baud wake-up
//             (18 02 FF 00 read, 14 FF 00 clear)
//     CAN:    UDS (MQB era: 19 02 FF read, 14 FF FF FF clear, in the extended session 10 03), else KWP2000 over
//             TP2.0 (PQ era: 18 02 FF 00 / 14 FF 00 after 10 89) - the same requests the OBD adapter's tests send
//   Basic setting (K-line, KWP1281 only): a group run in basic setting mode, e.g. ME7 group 060 = throttle body
//     adaptation. The module runs its own routine and reports its progress in the group's fields. It keeps going while
//     the web UI's Service tab is open, until Stop, SERVICE_BASIC_MAX_MS, the engine starts or the car moves.
//   0.34.0: CAN jobs can go through the OBD adapter instead (SVC_VIA_ADAPTER, UDS only), and Scan Modules asks every
//   module in the address table (plus the 29-bit engine / gearbox ids) for its part number, name and fault count, like
//   a dealer-tool auto-scan.
// One job at a time. The K-line task runs K-line jobs (they take the line from whatever had it), the VAG diag task runs
// CAN jobs. The web UI starts a job and polls the result.
// ---------------------------------------------------------------------------

#define SERVICE_MAX_FAULTS 40        // faults kept from one read
#define SERVICE_BASIC_MAX_MS 60000   // a basic setting stops by itself after this long
#define SERVICE_WATCH_MS 5000        // a basic setting stops when the Service tab hasn't asked for news this long

enum serviceJob_t : uint8_t
{
  SVC_JOB_NONE = 0,
  SVC_JOB_READ_FAULTS,   // list the stored faults
  SVC_JOB_CLEAR_FAULTS,  // clear them, then list what's left
  SVC_JOB_BASIC_SETTING, // run a group in basic setting mode
  SVC_JOB_SCAN,          // 0.34.0: find the modules fitted (part number, name, fault count)
};

enum serviceVia_t : uint8_t
{
  SVC_VIA_KLINE = 0, // the gauge's own K-line wire
  SVC_VIA_CAN,       // the gauge's own CAN
  SVC_VIA_ADAPTER,   // 0.34.0: through the OBD adapter (Bluetooth / WiFi ELM327), UDS only
};

#define SERVICE_MAX_MODULES 28 // modules kept from one scan

// 0.34.0: one module a scan found
struct ServiceModule
{
  uint8_t address;      // VAG diagnostic address (0 = not a VAG address: a 29-bit id)
  uint32_t requestId;   // the ids it answered on
  uint32_t answerId;
  char name[24];        // what the table calls it ("Gateway")
  char partNumber[16];  // F187 ("5Q0907530AA")
  char component[24];   // F197, the module's own name ("J533 Gateway")
  int16_t faults;       // faults stored (19 02 FF), -1 = it wouldn't list them
};

struct ServiceFault
{
  char code[12];  // "16684" (VAG fault number), "P0300-00" (UDS: code and failure type)
  uint8_t status; // status byte as the module sent it
};

struct ServiceStatus
{
  uint8_t job;          // serviceJob_t running now, or the last one
  uint8_t via;          // serviceVia_t
  uint8_t address;      // VAG module address (0x01 engine, 0x02 gearbox, 0x17 instruments ...)
  uint8_t group;        // basic setting: the group
  bool busy;            // a job is waiting or running
  bool done;            // the last job finished (the results below are complete)
  bool ok;              // ... and it worked
  char state[80];       // what's happening / how it ended
  char protocol[24];    // "KWP1281", "KWP2000 (K-line)", "UDS", "KWP2000 (TP2.0)"
  char moduleId[48];    // the module's identification (part number and name)
  uint8_t faultCount;   // faults listed (the read after a clear lists what's left)
  ServiceFault faults[SERVICE_MAX_FAULTS];
  // basic setting
  uint32_t startedMs;   // when the basic setting started (millis)
  uint32_t reads;       // answers so far
  uint8_t fieldCount;   // fields in the last answer (0..4)
  float value[4];       // each field's value
  char unit[4][8];      // ... unit (or text, e.g. "WARM")
  uint8_t formula[4];   // ... raw formula byte and the two data bytes, as sent
  uint8_t a[4];
  uint8_t b[4];
  char title[40];       // the group's title from the label file ("" = none)
  char fieldName[4][40]; // each field's name from the label file
  // scan (0.34.0)
  uint8_t scanDone;     // modules asked so far
  uint8_t scanTotal;    // ... out of
  uint8_t moduleCount;  // modules that answered
  ServiceModule modules[SERVICE_MAX_MODULES];
};

// starts a job; false (and why) if it can't start: another job is running, the engine runs / the car moves
// (basic setting), the ignition is off
bool serviceStart(uint8_t job, uint8_t via, uint8_t address, uint8_t group, char *why, size_t whyLen);
void serviceStop();                          // ends a running basic setting
void serviceStatusCopy(ServiceStatus &out);  // for the web UI (also tells a basic setting the tab is still open)
bool serviceCanBusy();                       // a CAN job is waiting or running: the OBD-II polling stands back
bool serviceAdapterBusy();                   // 0.34.0: a job through the OBD adapter is waiting or running

// for the tasks that run the jobs
bool serviceKlinePending();                  // the K-line task should hand the line to serviceKlineRun()
void serviceKlineRun();                      // runs the waiting K-line job (in the K-line task)
bool serviceCanPending();                    // the VAG diag task should run the waiting CAN (or adapter) job
void serviceCanTaken();                      // ... and has taken it (it runs it straight away)
uint8_t serviceFaultCount();                 // faults in the list so far
// the CAN job itself is in can2gauge_VagDiag.cpp (it uses that file's UDS / TP2.0 code); it reports with these:
// (the __attribute__ lets the compiler check the % codes against the values, as it does for printf)
void serviceSetState(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void serviceSetProtocol(const char *protocol, const char *moduleId);
void serviceClearFaults();                   // empties the list (before a read)
void serviceAddFault(const char *code, uint8_t status);
void serviceScanProgress(uint8_t done, uint8_t total); // 0.34.0: the scan's progress
void serviceAddModule(const ServiceModule &module);    // 0.34.0: a module the scan found
void serviceFinish(bool ok);                 // the job is over
void serviceDtcText(uint8_t high, uint8_t low, char *out, size_t len); // two DTC bytes -> "P0300" (SAE letters)
