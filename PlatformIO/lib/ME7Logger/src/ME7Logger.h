#pragma once
#include <Arduino.h>
#include <FS.h>

// ---------------------------------------------------------------------------
// ME7Logger - a re-implementation of setzi's ME7Logger idea for the gauge:
//   * .ecu file (ME7Info output) describes the ECU: how to connect, identification, and every RAM variable with
//     address / size / bitmask / conversion.
//   * .cfg file lists which variables to log and the sample rate.
//   * we connect over K-line (KWP2000), switch the ECU to the fast LogSpeed, define the variable block with
//     DynamicallyDefineLocalIdentifier (0x2C, by memory address) and stream it with ReadDataByLocalIdentifier
//     (0x21). 0.17.4: over several identifiers (F0, F1 ...) when the ECU caps the entries per identifier (an ME7.5
//     takes 3). If the ECU refuses 0x2C the logger falls back to ReadMemoryByAddress (0x23), one read per group of
//     neighbouring variables (it was one per variable) - also used when that needs no more requests.
//   * every sample updates SIG_ME7_1..8 (first 8 logged variables) and optionally appends to a CSV on LittleFS.
//
// File formats (both are plain INI-style text, ';' comments, {braces} around strings):
//   [Communication]  Connect=SLOW-0x01 | FAST-0x10   Communicate=HM0 | HM2-0x10   LogSpeed=56000
//   [Identification] HWNumber={..} SWNumber={..} PartNumber={..} SWVersion={..} EngineId={..}
//   [Measurements]   name, {alias}, address, size, bitmask, {unit}, S, I, A, B, {comment}
//                    phys = A * raw - B   (I=1: phys = A / (raw - B)),  S=1 signed, bitmask != 0 -> bit field
//   cfg: [Configuration] SamplesPerSecond=20   [LogVariables] name ; alias ; comment  (one per line)
// ---------------------------------------------------------------------------
#define ME7_MAX_VARS 32
#define ME7_NAME_LEN 24
#define ME7_ALIAS_LEN 32
#define ME7_UNIT_LEN 10

struct Me7Var
{
  char name[ME7_NAME_LEN];
  char alias[ME7_ALIAS_LEN];
  char unit[ME7_UNIT_LEN];
  uint32_t address;
  uint8_t size;      // 1 or 2 bytes
  uint16_t bitmask;  // 0 = numeric
  bool isSigned;
  bool inverse;
  float factor;      // A
  float offset;      // B
  bool found;        // matched in the .ecu file
  float value;       // last decoded value
  int32_t raw;
};

struct Me7EcuInfo
{
  char connect[16];     // SLOW-0x01 / FAST-0x10
  char communicate[16]; // HM0 / HM2-0x10
  uint32_t logSpeed;
  char hwNumber[24];
  char swNumber[24];
  char partNumber[24];
  char swVersion[12];
  char engineId[24];
};

enum me7State_t : uint8_t
{
  ME7_IDLE = 0,
  ME7_LOADING,
  ME7_CONNECTING,
  ME7_DEFINING,
  ME7_LOGGING,
  ME7_ERROR
};

// load definitions: parses cfg (variable list) then streams the ecu file for those variables
bool me7Load(fs::FS &fs, const char *ecuPath, const char *cfgPath, char *err, size_t errLen);
const Me7EcuInfo &me7EcuInfo();
uint8_t me7VarCount();
const Me7Var &me7Var(uint8_t index);
uint8_t me7SamplesPerSecond();
// 0.17.6: variables the gauge's pages show, logged as well as the .cfg's (they go after the .cfg's, so ME7 1-8 stay the
// .cfg's first 8, and a name the .cfg has already isn't added twice). Clear, add each, then me7Load().
void me7ClearExtras();
void me7AddExtra(const char *name);
int8_t me7FindVar(const char *name); // its number in the logged list, -1 = not logged (not in the .ecu, or no room)

// 0.19.0: B7 RAM-routine fast logging. Set the mode before me7Connect(). If the routine installs the session uses
// it; else it quietly stays on the normal 0x21/0x23 reads. See can2gauge_ME7Fast.
#define ME7_MODE_NORMAL 0    // normal logging: one 0x21/0x23 read per value, nothing written to the ECU
#define ME7_MODE_TEST_LINK 1 // test link: installs the v0 test routine, a B7 should return "C2G!" (no engine data) -
                             // proves writing to ECU RAM + hooking B7 works before trying Fast
#define ME7_MODE_FAST 2      // fast: installs the v1 routine, one B7 reads the whole block per sample
void me7SetFastMode(uint8_t mode);
uint8_t me7GetFastMode(); // the mode armed now (ME7_MODE_NORMAL / TEST_LINK / FAST)
bool me7FastRunning(); // true while a session is actually on the fast path

// session control - all blocking, call from the K-line task
bool me7Connect(char *err, size_t errLen);      // K-line init + session + fast baud + block definition
bool me7Sample();                               // one read of all variables; updates values (+ CSV if open)
void me7Disconnect();
me7State_t me7State();
const char *me7StateName();
uint32_t me7SampleCount();
uint32_t me7SampleRate(); // measured samples/s
uint8_t me7RequestsPerSample(); // 0.17.6: requests one sample takes (each ~10 ms on an ME7.5): what limits the rate
bool me7LogStart(fs::FS &fs, const char *dir); // opens /logs/me7_<n>.csv, writes header
void me7LogStop();
const char *me7LogFileName();
// helper: KWP2000 baud rate identifier per ISO 14230-2: baud = (mantissa + 32) * 2^exponent * 200
uint8_t me7BaudByte(uint32_t baud);
