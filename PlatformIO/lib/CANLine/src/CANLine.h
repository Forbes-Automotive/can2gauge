#pragma once
#include <Arduino.h>
#include "driver/twai.h"

// ---------------------------------------------------------------------------
// CANLine - one classic-CAN bus (TCAN1044V on the ESP32-S3 TWAI peripheral).
//   canBegin()               bring the bus up, start the RX task (if it fails, the RX task retries every second until canEnd)
//   bus-off                  the RX task checks the controller every second: bus-off -> recovery, stopped -> restart
//   canOnFrame(callback)     register a frame handler (up to 10; called from the RX task, keep them short)
//   canSend()                queue a frame
//   canSetObdPolling(on)     profile OBD2: this gauge asks the ECU for PIDs. Each request goes out the moment the last
//                            answer arrives (in the RX task), so it runs as fast as the ECU answers
// Decoders (all feed the signal catalogue via signalSetBus - every gauge on the wire hears them):
//   VAG PQ  - passive (PQ35/46 ACAN K-matrix): mMotor_1 0x280 (rpm, pedal), mMotor_2 0x288 (coolant), mMotor_3 0x380
//             (outside temp), mBremse_1 0x1A0 (speed), mKombi_1 0x320 (fuel litres), mGetriebe_1 0x440 (target gear +
//             selector), mWaehlhebel_1 0x448 (lever), mGetriebe_2 0x540 (what the cluster shows: PRNDS / engaged gear / M1..)
//   VAG MQB - passive (MQB FCAN K-matrix): Motor_12 0x0A8 rpm, ESP_21 0x0FD speed, Motor_20 0x121 pedal, Motor_07 0x640
//             IAT + coolant, Kombi_02 0x6B7 fuel + outside temp, Getriebe_11 0x0AD selector + gear, Getriebe_14 0x3C8
//             gearbox sump temp
//   OBD-II  - active: one 0x7DF "0100" finds the engine ECU (0x7E8) and the PIDs it has, then up to 6 PIDs per request
//             straight to it (0x7E0): the 14 that feed gauge signals (0x0C rpm, 0x0D speed, 0x05 coolant, 0x0B MAP,
//             0x0F IAT, 0x11 throttle, 0x04 load, 0x10 MAF, 0x0E timing, 0x42 battery, 0x2F fuel, 0x5C oil temp,
//             0x44 lambda, 0x46 ambient), and any other standard value up to 0x60 while a page or the web UI wants it.
//             Long answers (ISO-TP first + consecutive frames) are put back together; followers decode them passively.
//             It can also run next to VAG PQ / MQB / MaxxECU (canSetObdAlongside, settings.obdOverCan)
//   MaxxECU - passive: its "Default CAN output" 1.2 / 1.3 from a configurable base (0x520): rpm, throttle, MAP / boost,
//             lambda, timing, speed, battery, IAT, coolant, ethanol, EGT, gear, oil pressure / temp, fuel pressure, gearbox temp
//   OpenHaldex - 0x6B0 broadcast [0][standalone][engagement raw][lock target][speed][override][mode][pedal]
//             and 0x6A0 external control (byte 0 = mode) so the gauge can change the Haldex mode.
// ---------------------------------------------------------------------------
#include <can2gauge_signals.h>

typedef void (*canFrameCb_t)(const twai_message_t &message);

// OBD-II mode 01 helpers shared with the BLE ELM327 path: data length of a PID, and decode of its data bytes
// (data = the bytes after [41][pid]) into the catalogue through set (signalSet / signalSetBus). false = unknown PID.
// obdPidLen returns 0 for a PID whose length isn't known (stop parsing a multi-PID reply there).
uint8_t obdPidLen(uint8_t pid);
bool obdDecodePid(uint8_t pid, const uint8_t *data, uint8_t len, void (*set)(signalId_t, float));
// 0.37.6: one PID's value out of its answer bytes (the bytes after [41][pid]), with nothing set anywhere - for a PID
// read from another ECU (the gearbox's own PIDs, the VAG diag task's "OBD PID" values). false = not a PID the gauge knows.
bool obdPidValueOf(uint8_t pid, const uint8_t *data, uint8_t len, float &value);
// called with every PID decoded by obdDecodePid (ELM or wired), after set()
void obdSetPidHook(void (*hook)(uint8_t pid, signalId_t id, float value));

// OBD-II PID picker, shared by the wired CAN, the gauge's own K-line and the OBD adapter. Only PIDs the ECU says it
// has are asked for. "Fast" PIDs (rpm, boost, speed, throttle, load, timing, lambda, plus anything on the page on
// screen) are asked for every time; the rest take turns in one "slow" place.
//   up to 6 PIDs per request (CAN): 5 fast (a moving window when there are more) + 1 slow
//   1 PID per request (K-line, J1850, or CAN when the ECU can't take 6): fast ones, every 4th request a slow one
#define OBD_PID_COUNT 80  // PIDs the gauge can decode: every standard value up to PID 0x60 (the table in CANLine.cpp)
#define OBD_MAX_PIDS 6    // most PIDs in one request (SAE J1979 allows 6 on CAN)

// one standard OBD value: its name, unit and gauge range for the web UI and gauge pages, and the gauge signal it feeds
struct ObdPidInfo
{
  uint8_t pid;
  const char *name;  // "Fuel rail pressure"
  const char *unit;  // as the gauge stores units: "C" for temperatures, "deg" for angles, "" for none
  uint8_t formula;   // how the data bytes become the value (obdFormula_t, CANLine.cpp)
  float gaugeMin;    // the range a page gets when it picks this PID
  float gaugeMax;
  signalId_t signal; // the gauge signal it feeds (rpm, coolant ...), SIG_NONE = none: only a page's OBD source uses it
  bool alwaysFast;   // changes quickly: asked for every time
};
bool obdPidInfo(uint8_t pid, ObdPidInfo &info);            // a PID's row in the table (false = not a value we know)
bool obdPidInfoAt(uint8_t tableIndex, ObdPidInfo &info);   // row 0 .. OBD_PID_COUNT - 1, for listing them all
bool obdPidLatest(uint8_t pid, float &value, uint32_t &ageMs); // the last value decoded, any route (false = never)
void obdWantAllPids(); // the web UI's Live OBD list is open: ask for every PID the car has for the next 5 s
struct ObdSchedule
{
  uint32_t supported[3]; // the ECU's PID lists: [0] 01-20, [1] 21-40, [2] 41-60 (bit 31 = the lowest PID); all 0 = not read
  uint8_t fastIndex;     // where the fast rotation carries on
  uint8_t slowIndex;     // where the slow rotation carries on
  uint8_t turn;          // one-PID requests: counts them so every 4th is a slow PID
  uint8_t pageIndex;     // 0.24.1, one-PID requests: where the rotation of the page-on-screen's PIDs carries on
  uint8_t backIndex;     // 0.24.1, one-PID requests: where the rotation of everything else carries on
};
void obdScheduleReset(ObdSchedule &schedule);                                       // forget the PID lists
void obdScheduleSetList(ObdSchedule &schedule, uint8_t listPid, const uint8_t *data); // the 4 bytes after [41][00 / 20 / 40]
void obdScheduleAssumeAll(ObdSchedule &schedule);                                   // the ECU won't say: try every PID
bool obdScheduleHasList(const ObdSchedule &schedule);                               // a PID list has been read
bool obdScheduleSupports(const ObdSchedule &schedule, uint8_t pid);
uint8_t obdSchedulePick(ObdSchedule &schedule, uint8_t *pids, uint8_t maxPids);     // fills pids, returns how many
uint8_t obdPidForSignal(signalId_t id);                    // the PID that feeds a gauge signal, 0 = none
void obdSetPagePids(const uint8_t *pids, uint8_t count);   // the PIDs the page on screen shows: they join the fast ones

struct CanStats
{
  bool up;
  uint32_t rx, tx, err, rxPerSec, busOff;
  uint32_t lastRxMs;
};

// 0.37.9: whose turn it is to ask a question on the gauge's CAN. The OBD polling, the VAG diag task (UDS, OBD and TP2.0
// values, the car scan, Service jobs) and the DSG each ask ECUs questions; two asking the same ECU at once would take
// each other's answers (one 7E8 for the OBD polling and an engine DID). Each takes the turn for one question and its
// answer, then hands it back. A task waits for it (canDiagWait); the OBD polling, which runs in the CAN receive task and
// mustn't wait, only takes it when it's free (canDiagClaim) and skips a go otherwise. Someone waiting gets the next turn.
#define CAN_DIAG_NONE 0  // nobody has the turn
#define CAN_DIAG_OBD 1   // the OBD-II polling (this file)
#define CAN_DIAG_VDIAG 2 // the VAG diag task (src/can2gauge_VagDiag.cpp)
#define CAN_DIAG_DSG 3   // the gearbox reads (src/can2gauge_DSG.cpp)
bool canDiagClaim(uint8_t owner, uint32_t holdMs);                  // true = the turn is ours (for holdMs at most)
bool canDiagWait(uint8_t owner, uint32_t holdMs, uint32_t waitMs);  // tasks: waits up to waitMs for the turn
void canDiagRelease(uint8_t owner);                                 // done: the next one's turn

bool canBegin(int8_t txPin, int8_t rxPin, uint16_t kbps);
void canEnd();
bool canSend(uint32_t id, const uint8_t *data, uint8_t len, bool extended = false);
bool canOnFrame(canFrameCb_t callback, bool air = false); // air = also called for frames relayed over the air (canInject)
const CanStats &canStats();
// Frames another device heard on the car's CAN and relayed over the air (OpenHaldex / adc2can over ESP-NOW,
// src/can2gauge_CanRelay.cpp). They are decoded like wired frames - the profile decoders, and the handlers registered
// with air = true (the CAN monitor, so the DBC / user signals too) - but never the OpenHaldex 0x6B0 decoder, and nothing
// that answers on the bus. Each id has one source at a time: the wire always wins, and a relayed copy is taken only
// once no other source has sent that id for CAN_AIR_HOLD_MS.
#define CAN_AIR_SOURCES 4   // relay senders (source 1..4; 0 is the wire)
#define CAN_AIR_HOLD_MS 1000
void canAirBegin();                                            // once at boot: the queue and the task that decodes them
bool canInject(const twai_message_t &message, uint8_t source); // any task (the WiFi callback): false = queue full, dropped
struct CanAirStats
{
  uint32_t rx, perSec; // decoded
  uint32_t ignored;    // the id came from the wire / another sender
  uint32_t dropped;    // queue full
  uint32_t lastRxMs;
};
const CanAirStats &canAirStats();
// listen-only: the controller sends nothing at all, not even ACKs (canSend returns false). Used while the gauge is in
// standby on permanent power so it can watch for the ignition without keeping the car's bus awake. Survives
// canEnd / canBegin (a settings change restarts the bus in the same mode).
void canSetListenOnly(bool on);
bool canIsListenOnly();
// OBD-II polling on the wire (profile OBD2 only): on = this gauge asks (the chain master, not in standby). Called
// often from a task; the requests themselves run in the RX task, one straight after each answer.
void canSetObdPolling(bool on);
void canSetObdAlongside(bool on); // also ask for OBD-II PIDs on a listen profile (VAG PQ / MQB / MaxxECU)
void canSetObdEcu(uint16_t answerId); // the ECU to read by its answer id (0x7E8 engine, 0x7E9 gearbox ...), 0 = the engine
struct CanObdStats
{
  bool ecuFound;          // the engine ECU answered the "which PIDs" request
  uint16_t ecuId;         // its answer id (0x7E8)
  uint8_t pidsPerRequest; // 6 = several PIDs per request, 1 = one at a time (the ECU didn't cope with 6)
  uint32_t requests;      // sent since boot
  uint32_t answers;       // complete answers since boot
  uint32_t misses;        // no answer in time, or fewer PIDs back than asked
  uint32_t answersPerSec; // last second
  uint32_t pidsPerSec;    // PID values decoded in the last second
  uint8_t answeredMask;   // the ECUs that answered "0100" since boot: bit n = answer id 0x7E8 + n
};
const CanObdStats &canObdStats();
void canSetProfile(uint8_t newProfile);
void canSetMaxxBase(uint16_t base); // CAN_PROFILE_MAXXECU: the first frame's id (0x520)
void canSetHaldexIds(uint16_t broadcastId, uint16_t controlId);
bool canHaldexSetMode(uint8_t mode); // sends 0x6A0 [mode]
// last raw frames for the diagnostics page (ring of 16)
struct CanFrameLog
{
  uint32_t id;
  uint8_t len;
  uint8_t data[8];
  uint32_t ms;
  uint8_t air; // relayed over the air (canInject), not heard on the wire
};
uint8_t canRecentFrames(CanFrameLog *out, uint8_t maxFrames);
