#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// CAN monitor + DBC (menu > CAN, web UI CAN tab).
//   monitor  every id on the bus: last data, DLC, count, rate (from a canOnFrame handler, up to CANMON_MAX_IDS ids)
//   DBC      the car's DBC in the compact ".c2d" form (tools/dbc/dbc_tool.py pack, or the web UI's own converter for an
//            uploaded .dbc), saved as /candbc.c2d and loaded into PSRAM; frames are decoded against it on demand
// .c2d is tab-separated text, one line per item:
//   C2D  <version 1 or 2>  <name>  [v2: <bus speed, 0 = not given>  <relocatable 0 / 1>]
//   M    <id dec, 0x80000000 = extended>  <dlc>  <name>
//   S    <name>  <start bit>  <length>  <flags: 1 = Intel (little endian), 2 = signed>  <factor>  <offset>  <unit>  <mux>
//        [v2: <gauge key, "" = none>  <priority 0-9>]
//   V    <raw value>  <label>             (v2: a value label of the S line before it - only for a keyed signal)
//   K    <signal>  <min>  <max>           (v2: GaugeCheck of the M line before it - for finding the ECU, not used yet)
// where mux is -1 (plain), -2 (the multiplexer) or the multiplexer value the signal belongs to. S lines follow their M.
//
// 0.28.0 (docs/CAN_DBC_plan.md): a signal with a gauge key feeds that gauge signal straight from the frame ("rpm",
// "coolant", "gear" ...), in the gauge's own units (kPa -> mbar and so on). Its value labels turn gear / selector codes
// into the gauge's values ("8" = 8th gear, "D" = SEL_D) or mark "no value" codes ("Error", "Init") to skip. Two signals
// for one key: the higher priority wins while it has been heard in the last second. Up to three DBCs are loaded at once
// (car, gearbox, extra - e.g. a MaxxECU and an 8HP on one bus); a relocatable one is moved to settings.dbcSlotBase.
// ---------------------------------------------------------------------------
#define CANMON_MAX_IDS 160
#define CANDBC_SLOTS 3                // DBCs loaded at once: 1 car / engine, 2 gearbox, 3 extra (0.28.0)
#define CANDBC_PATH "/candbc.c2d"     // slot 1's file (the one there has always been)
#define CANDBC_PATH_2 "/candbc2.c2d"  // slot 2's file
#define CANDBC_PATH_3 "/candbc3.c2d"  // slot 3's file
#define DBC_LABEL_SKIP -32768         // a value label that means "no value" (Error / Init / Undefined ...)

struct CanMonEntry
{
  uint32_t id; // bit 31 = extended
  uint8_t dlc;
  uint8_t data[8];
  uint32_t count;
  uint32_t firstMs, lastMs;
};

struct CanDbcSignal
{
  char name[40];
  uint16_t start;
  uint8_t len;
  uint8_t flags; // 1 Intel, 2 signed
  float factor, offset;
  char unit[10];
  int16_t mux; // -1 plain, -2 multiplexer, else the multiplexer value
  uint8_t gaugeKey;    // 0.28.0: the gauge signal it feeds (signalId_t, SIG_NONE = none)
  uint8_t priority;    // 0.28.0: GaugePriority 0-9 (two signals for one key: the higher one wins)
  uint16_t firstLabel; // 0.28.0: its value labels in the label table ...
  uint8_t labelCount;  // ... and how many
};

// 0.28.0: a value label, already turned into the gauge's value when the DBC was loaded
struct CanDbcLabel
{
  int32_t raw;   // the raw number in the frame
  int16_t value; // the gauge's value for it (gear number, selectorPos_t, 0 / 1 / 2 ...), DBC_LABEL_SKIP = no value
};

struct CanDbcMessage
{
  uint32_t id; // bit 31 = extended
  uint8_t dlc;
  char name[40];
  uint16_t firstSignal, signalCount;
  uint8_t slot; // 0.28.0: which DBC slot it came from (0-2)
};

// 0.28.0: what is loaded in one DBC slot
struct CanDbcSlotInfo
{
  bool loaded;        // a file is in this slot and loaded
  char name[40];      // its name (vw_mqb, emu_black ...)
  uint16_t messages;  // messages ...
  uint16_t signals;   // ... signals ...
  uint16_t keyed;     // ... and signals that feed a gauge signal
  uint32_t busSpeed;  // GaugeBusSpeed (0 = not given)
  bool relocatable;   // GaugeRelocatable: its ids move with the ECU's base id setting
  uint16_t fileBase;  // its lowest 11-bit id as written (the base it's moved from)
  char error[48];     // why it isn't loaded ("" = fine): unreadable, or an id another slot already has
};

void setupCanMonitor();                                       // registers the frame handler, loads /candbc.c2d
uint16_t canMonitorSnapshot(CanMonEntry *out, uint16_t max, uint32_t maxAgeMs); // live ids (seen within maxAgeMs), by id
bool canMonitorGet(uint32_t id, CanMonEntry &out);
float canMonitorRate(const CanMonEntry &entry);               // frames/s over the id's lifetime

void canDbcLock();   // hold while using canDbcFind / canDbcSignal results (a reload swaps the tables)
void canDbcUnlock();
bool canDbcTryLock(); // the CAN RX task: take the lock only if it's free (true = taken, canDbcUnlock after); never waits
bool canDbcLoaded();
uint32_t canDbcGeneration(); // changes with every load / clear (indices into the tables hold for one generation)
const char *canDbcName();
uint16_t canDbcMessageCount();
uint16_t canDbcSignalCount();
const CanDbcMessage *canDbcFind(uint32_t id);
const CanDbcSignal &canDbcSignal(uint16_t index);
const char *canDbcSlotPath(uint8_t slot);                     // 0.28.0: slot 0-2's file on the gauge
bool canDbcCheckFile(const char *path, char *err, size_t errLen); // 0.28.0: does this .c2d read cleanly? (doesn't load it)
void canDbcLoadAll();                                         // 0.28.0: (re)load every slot's file, merged into one table
const CanDbcSlotInfo &canDbcSlot(uint8_t slot);               // 0.28.0: what's in slot 0-2
void canDbcClearSlot(uint8_t slot);                           // 0.28.0: forget one slot's DBC (and delete its file)
// decode one signal from frame data: false when it doesn't apply (multiplexed for another value, past the DLC)
bool canDbcDecode(const CanDbcMessage &message, const CanDbcSignal &signal, const uint8_t *data, uint8_t dlc,
                  float &value);
bool canDbcIsHousekeeping(const CanDbcSignal &signal);    // counters / checksums / CRCs: skipped on the gauge
void canDbcFormat(const CanDbcSignal &signal, float value, char *buf, size_t len); // value + unit, decimals from the factor
