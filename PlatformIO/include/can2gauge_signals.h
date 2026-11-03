#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Signal catalogue: every value the gauge can show, trigger on, or serve to the web UI lives here.
// Producers (CAN, K-line, ME7 logger, board IO, OpenHaldex, BLE ELM, DSG diagnostics, usb2ip, other gauges) call signalSet();
// consumers call signalGet(). A signal goes stale (signalValid() == false) if it has not been written within its timeout.
//
// The enum order IS the gauge-chain wire format (the link sends the id byte): APPEND ONLY, never reorder or remove.
// ---------------------------------------------------------------------------
enum signalId_t : uint8_t
{
  SIG_NONE = 0,
  // engine / vehicle (CAN PQ, OBD-II or K-line)
  SIG_RPM,
  SIG_SPEED,     // km/h
  SIG_COOLANT,   // degC
  SIG_IAT,       // degC
  SIG_OIL_TEMP,  // degC
  SIG_BOOST,     // mbar absolute (MAP)
  SIG_BOOST_REL, // bar relative to ambient (derived: (MAP - 1013) / 1000)
  SIG_PEDAL,     // %
  SIG_THROTTLE,  // %
  SIG_LOAD,      // %
  SIG_MAF,       // g/s
  SIG_LAMBDA,    // lambda
  SIG_TIMING,    // deg BTDC
  SIG_BATTERY,   // V
  SIG_FUEL_LEVEL,// %
  SIG_GEAR,      // engaged / target gear: 0 = none (P, N, R - see SIG_SELECTOR), 1..10
  // OpenHaldex broadcast (0x6B0)
  SIG_HALDEX_TARGET,   // lock target %
  SIG_HALDEX_ACTUAL,   // engagement %
  SIG_HALDEX_MODE,     // 0 stock .. 5 expert
  SIG_HALDEX_SPEED,    // km/h as seen by the controller
  SIG_HALDEX_PEDAL,    // %
  // board IO (V3.4)
  SIG_DASH_LIGHTS, // 1 = dash lights on (J_OUT pin 3, after the invert setting)
  SIG_OUTPUT,      // 1 = output on (J_OUT pin 1)
  SIG_OUT_FAULT,   // 1 = DRV8876 fault (over-current / over-temperature)
  // ME7Logger user slots (mapped from the .cfg variable list, in order)
  SIG_ME7_1,
  SIG_ME7_2,
  SIG_ME7_3,
  SIG_ME7_4,
  SIG_ME7_5,
  SIG_ME7_6,
  SIG_ME7_7,
  SIG_ME7_8,
  // board
  SIG_BOARD_TEMP, // ESP32 die temperature
  SIG_UPTIME,     // s
  // ---- 0.4.0 (append only from here)
  SIG_SELECTOR,     // selectorPos_t: gear selector / DSG mode
  SIG_TCU_SPEED,    // km/h read from the gearbox (UDS / TP2.0 / ELM)
  SIG_GEARBOX_TEMP, // degC (MQB Getriebe_14 sump temperature)
  SIG_AMBIENT,      // degC outside air (PQ Motor_3, MQB Kombi_02, OBD PID 0x46)
  // ---- 0.6.0
  SIG_OIL_LEVEL,    // 1 = OK, 0 = low (cluster 0x17 measuring block, G266 sender)
  SIG_OIL_PRESS,    // 1 = OK, 0 = low: the oil-pressure switch as the cluster reports it (PQ mKombi_1 0x320)
  // ---- 0.6.0: gauge pages from a chosen source (settings.pages[n].valueSigs, src/can2gauge_UserSig.cpp): page n's main value is
  // SIG_USER_1 + 2n, its second value SIG_USER_2 + 2n. Their meaning is per gauge, so the chain never shares them.
  SIG_USER_1,
  SIG_USER_2,
  SIG_USER_3,
  SIG_USER_4,
  SIG_USER_5,
  SIG_USER_6,
  SIG_USER_7,
  SIG_USER_8,
  SIG_USER_9,
  SIG_USER_10,
  SIG_USER_11,
  SIG_USER_12,
  SIG_USER_13,
  SIG_USER_14,
  SIG_USER_15,
  SIG_USER_16,
  // page n's Quad values 2-4 / Custom centre stack (top, middle, bottom): SIG_USER_17 + 3n + k (settings.pages[n].slotSigs[k])
  SIG_USER_17,
  SIG_USER_18,
  SIG_USER_19,
  SIG_USER_20,
  SIG_USER_21,
  SIG_USER_22,
  SIG_USER_23,
  SIG_USER_24,
  SIG_USER_25,
  SIG_USER_26,
  SIG_USER_27,
  SIG_USER_28,
  SIG_USER_29,
  SIG_USER_30,
  SIG_USER_31,
  SIG_USER_32,
  SIG_USER_33,
  SIG_USER_34,
  SIG_USER_35,
  SIG_USER_36,
  SIG_USER_37,
  SIG_USER_38,
  SIG_USER_39,
  SIG_USER_40,
  // ---- 0.6.0 adc2can (src/can2gauge_ADC2CAN.cpp): board 0 ch 1-8, board 1 ch 1-8 - named / given units over CAN
  SIG_ADC_1,
  SIG_ADC_2,
  SIG_ADC_3,
  SIG_ADC_4,
  SIG_ADC_5,
  SIG_ADC_6,
  SIG_ADC_7,
  SIG_ADC_8,
  SIG_ADC_9,
  SIG_ADC_10,
  SIG_ADC_11,
  SIG_ADC_12,
  SIG_ADC_13,
  SIG_ADC_14,
  SIG_ADC_15,
  SIG_ADC_16,
  // ---- 0.7.0 OpenHaldex over WiFi (src/can2gauge_OHX.cpp): its live diagnostics, only sent over ESP-NOW
  SIG_HALDEX_CLUTCH_TEMP, // degC: Gen5 clutch (UDS) / Gen4 plate (KWP)
  SIG_HALDEX_OIL_TEMP,    // degC: Gen4 oil (KWP)
  SIG_HALDEX_VOLTS,       // V: the Haldex module's supply
  // ---- 0.10.0 aftermarket ECUs (MaxxECU, CAN_PROFILE_MAXXECU)
  SIG_EGT,           // degC, the highest exhaust gas temperature
  SIG_OIL_PRESSURE,  // bar (SIG_OIL_PRESS is the cluster's OK / low switch)
  SIG_FUEL_PRESSURE, // bar
  SIG_ETHANOL,       // % ethanol content (flex fuel)
  // ---- next (after 0.11.0): an aftermarket ECU's data over WiFi from usb2ip (src/can2gauge_ECU.cpp, include/ecunow_proto.h)
  SIG_KNOCK_RETARD,  // deg of knock retard, the highest of any cylinder (0 = none)
  SIG_INJ_DUTY,      // % injector duty (primary)
  SIG_LAMBDA_TARGET, // lambda the ECU is aiming for
  // (any other channel of the ECU goes on a page as a user signal: USRC_ECU, src/can2gauge_UserSig.cpp)
  // ---- 0.26.0 gearbox (8HP and DSG): from the CAN broadcast (MQB Getriebe_11 / 13, MLB Getriebe_01 / 03) or the
  // gearbox diagnostic read (settings.dsgLockup)
  SIG_SHIFTING,       // 1 = a gear change is under way
  SIG_LOCKUP,         // torque converter lock-up (DSG: the clutch): 0 open, 1 slipping, 2 locked
  SIG_INPUT_RPM,      // gearbox input (turbine) speed, rpm
  SIG_CONVERTER_SLIP, // engine rpm - gearbox input rpm (worked out when both are fresh)
  // ---- 0.28.0
  SIG_BARO,           // mbar: the ECU's own barometric pressure (from its DBC); boost = MAP - this when it's fresh
  SIG_COUNT
};
#define ADC_SIGS 16
#define USER_SIGS 16      // page main / second values
#define USER_SIGS_ALL 40  // + the slot values

// SIG_SELECTOR values, normalised from PQ mGetriebe_1 / mGetriebe_2 / mWaehlhebel_1 and MQB Getriebe_11
enum selectorPos_t : uint8_t
{
  SEL_NONE = 0, // unknown / intermediate
  SEL_P,
  SEL_R,
  SEL_N,
  SEL_D,
  SEL_S,
  SEL_M, // tiptronic gate / paddles (shown as M1, M2 ...)
  SEL_E, // efficiency (MQB Fahrstufe 10)
  SEL_L,
  SEL_B,
  selectorPos_t_MAX
};
const char *selectorLetter(uint8_t selector); // "P" "R" "N" "D" "S" "M" "E" "L" "B", "-" if unknown

// who wrote a signal last - lets the gauge chain share only what the other gauges cannot see themselves
enum signalOrigin_t : uint8_t
{
  SIG_ORIGIN_LOCAL = 0, // a private source on this gauge (K-line, ME7, BLE ELM, DSG diagnostics, board IO)
  SIG_ORIGIN_BUS,       // decoded from a frame on the shared CAN bus (every gauge on the wire hears it too)
  SIG_ORIGIN_REMOTE,    // received from another gauge over the chain link
};

struct SignalInfo
{
  const char *key;   // short key used in JSON / settings
  const char *name;  // default gauge label
  const char *unit;  // default unit (metric)
  float defMin;      // default gauge range
  float defMax;
  float defRedline;
  uint16_t timeoutMs; // 0 = never stale
};

void signalsInit();
void signalSet(signalId_t id, float value);    // local private source
void signalSetBus(signalId_t id, float value); // decoded from the shared CAN bus
// from another gauge: never overwrites a fresh value this gauge produced itself (local / bus beats remote)
void signalSetRemote(signalId_t id, float value, uint8_t node);
float signalGet(signalId_t id);
bool signalValid(signalId_t id);
uint32_t signalAge(signalId_t id);
signalOrigin_t signalOrigin(signalId_t id);
uint8_t signalSourceNode(signalId_t id); // node id for SIG_ORIGIN_REMOTE, 0xFF otherwise
const SignalInfo &signalInfo(signalId_t id);
signalId_t signalFromKey(const char *key); // SIG_NONE if unknown
// ME7 / ADC producers can rename their slots at runtime (label + unit shown on gauges and in the web UI)
void signalSetLabel(signalId_t id, const char *name, const char *unit);
const char *signalName(signalId_t id);
const char *signalUnit(signalId_t id);
// 0.31.0: how many decimals a value shows on any display (Quad / Triple / Dual / Custom values, ...): a page source's
// own (ECU channel, ME7 variable), an ADC sensor's own, else from its unit. A page's main value uses the page's Decimals.
uint8_t signalDecimals(signalId_t id);
uint8_t unitDecimals(const char *unit); // 0.36.14: the decimals a unit says (lambda / bar 2, V / deg / ms 1, the rest 0)
// a stored (metric) value in the unit the owner picked for its quantity (settings.unitSpeed ..., src/can2gauge_Units.cpp),
// with that unit's text for the glass ("mph", "°F", "psi" ...). 0.27.0: was one metric / imperial switch.
float signalDisplayValue(signalId_t id, float metricValue, const char **unitOut);
// a value that shows as zero at this many decimals, as a plain 0 (no "-0.00" on the glass)
float valueNoMinusZero(float value, int decimals);
