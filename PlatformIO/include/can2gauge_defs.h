#pragma once

// general ESP activities
#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCoreWithCaps: a task whose stack is in PSRAM
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h" // psramCalloc / printInternalRam below: choose PSRAM or internal RAM

#include <can2gauge_pins.h>
#include <can2gauge_ver.h>
#include <can2gauge_signals.h>
#include <c2gs.h> // SPRITE_NAME_LEN

// This header is included by the lib/ libraries (CANLine, GaugeUI, ME7Logger), which PlatformIO compiles in
// isolation - so it must stay lightweight and pull in nothing they don't already depend on. The app's heavy
// dependencies live where they are used, not here:
//   Preferences        -> can2gauge_EEP.cpp
//   WiFi / ESPmDNS     -> can2gauge_WiFi.cpp, can2gauge_API.cpp
//   AsyncWebServer/TCP -> can2gauge_API.h (declares the server), thus API.cpp / OTA.cpp
//   ArduinoJson        -> can2gauge_EEP.h (JSON in signatures), thus EEP/API/OTA
//   LittleFS           -> can2gauge_API.cpp, can2gauge_KLine.cpp, can2gauge_ME7Read.cpp

// debug options
#define enableDebug 1        // set to 1 to enable debug messages over Serial (USB-CDC); set to 0 to disable
#define debugRamRegions 0    // 0.24.17 bench: list every internal RAM area at boot (heap_caps_print_heap_info) ** CAN CHANGE THIS **
#define debugI2cScan 0 // ** CAN CHANGE THIS ** 1 = list every I2C address that answers when no touch chip is found (bench; slows boot)
#define detailedDebug 0      // more detailed debug messages (verbose)
#define detailedDebugCAN 0   // detailed CAN debug messages (very verbose)
#define detailedDebugKLine 0 // detailed K-line byte-level debug (very verbose)
#define detailedDebugWiFi 0  // detailed WiFi debug messages
#define detailedDebugEEP 0   // detailed EEPROM debug messages
#define detailedDebugIO 0    // detailed IO (TCA9554 output / dash-light input) debug messages
#define detailedDebugUI 0    // detailed UI / LVGL debug messages

// refresh rates
#define eepRefresh 2000        // EEPROM save check in ms
#define ioRefresh 100          // dash-light input / output fault poll in ms
#define triggerRefresh 100     // trigger evaluation in ms
#define uiRefresh 20           // gauge value refresh in ms (50 Hz: the needles / arcs glide between samples, see smoothTo)
#define canStatsRefresh 1000   // CAN bus statistics in ms
#define engineTimeout 2000     // ms without an engine frame before the value is shown as stale
#define haldexTimeout 1500     // ms without an OpenHaldex broadcast before the page shows "no link"
#define obdPollRefresh 100     // ms between updates of who polls OBD-II and which PIDs the page on screen shows (the
                               // requests themselves go out as fast as the ECU answers)
#define oilReleaseMs 3000      // K-line oil source: close the cluster session this long after the oil page left the screen
#define kombi2Timeout 2000     // ms without mKombi_2 (0x420) before the oil page says the frame is lost
#define linkRefresh 10         // gauge-chain link task tick in ms
#define detailedDebugLink 0    // gauge-chain frames (verbose)
#define detailedDebugDSG 0     // DSG UDS / TP2.0 traffic (verbose)
#define detailedDebugELM 0     // BLE ELM327 command / response lines (verbose)
#define detailedDebugVagDiag 0 // VAG diag (UDS / TP2.0 measuring values) requests and answers (verbose) ** CAN CHANGE THIS **

// 0.17.3: USB bench console (src/can2gauge_Console.cpp): a PC script (tools/diag/kline_bench.py) drives the gauge over
// the USB cable with the same requests the web UI makes, for testing the K-line on the bench without WiFi
#define enableConsole 1     // 1 = listen for console commands on the USB serial ** CAN CHANGE THIS **
#define CONSOLE_MARK "@@C2G" // every console reply line starts with this, so the script can tell it from debug lines

// debugging macros
#if enableDebug
#define DEBUG(x, ...) Serial.printf(x "\n", ##__VA_ARGS__)
#define DEBUG_(x, ...) Serial.printf(x, ##__VA_ARGS__)
#else
#define DEBUG(x, ...)
#define DEBUG_(x, ...)
#endif

// ---------------------------------------------------------------------------
// Settings (persisted in NVS as one blob, mirrored to/from JSON for the web UI)
// ---------------------------------------------------------------------------
#define SETTINGS_MAGIC 0xC26A
#define SETTINGS_VERSION 30 // 30 = 0.37.0: 32 VAG diag values, each with its own module; the "car" page source. 29 = 0.34.0: VAG diag 29-bit ids, adapter link, bit-level values. 28 = the 0.33.0 tidy: one list in sections. Saved settings of any other version or size
                            // are not read: the gauge starts from defaults (nothing is released yet, so nothing migrates)
#define MAX_PAGES 8
#define MAX_TRIGGERS 8
#define NAME_LEN 16
#define UNIT_LEN 8
#define WIFI_LEN 32
#define FILE_LEN 40
#define LOG_CHANNELS 16 // values the data logger can record side by side (0.16.0)

enum gaugeStyle_t : uint8_t
{
  STYLE_ARC = 0,  // big arc sweep with centre value (default)
  STYLE_NEEDLE,   // classic dial with needle + scale ticks
  STYLE_BAR,      // horizontal bar with value
  STYLE_NUMERIC,  // large number only (shown as "Single" since 0.31.0)
  STYLE_DUAL,     // two values stacked (signal + signal2)
  STYLE_HALDEX,   // OpenHaldex page: lock target/actual arc + mode
  STYLE_QUAD,     // four signals: the page signal and the next three
  STYLE_GEAR,     // DSG / auto: big selector + gear (P R N D4 S3 M2) and speed - same view as menu > DSG
  STYLE_OIL,      // OEM oil temp + level from the cluster (CAN Kombi_2 or K-line 0x17), with the connection status (shown as "Oil (PQ)")
  STYLE_CUSTOM,   // main arc gauge (the page signal) + a centre stack of three small icon readouts (the page's slots)
  STYLE_GRAPH,    // 0.31.0: the page signal over the last 30 s as a rolling line graph, with the value above it
  STYLE_TRIPLE,   // 0.31.0: three signals: the page signal on top, slots 1 and 2 below (Quad with one fewer)
  gaugeStyle_t_MAX
};

enum triggerAction_t : uint8_t
{
  ACT_ALERT = 0,  // on-screen alert banner
  ACT_OUTPUT,     // drive the output (J_OUT pin 1)
  ACT_ALERT_OUT,  // both
  triggerAction_t_MAX
};

enum klineProtocol_t : uint8_t
{
  KL_OFF = 0,
  KL_KWP1281,   // VAG 5-baud init, block protocol (Mk4 ME7 / Marelli / ... via address 0x01)
  KL_KWP2000,   // ISO 14230 fast init, OBD-II mode 01 PIDs
  KL_ME7LOGGER, // ME7Logger-style fast RAM logging using .ecu/.cfg definition files on LittleFS
  // APPEND ONLY (0.19.0): keep the numbers above stable so saved settings don't shift.
  KL_KWP2000VAG, // VAG KWP2000 measuring blocks via the double slow-init: faster than KWP1281, same groups/gauges,
                 // falls back to KWP1281 if the module won't come up KWP2000
  klineProtocol_t_MAX
};

enum canProfile_t : uint8_t
{
  CAN_PROFILE_VAG_PQ = 0, // Mk4/Mk5 era PQ frames (Motor_1 0x280 etc) - passive listen
  CAN_PROFILE_OBD2,       // request OBD-II PIDs on 0x7DF, decode 0x7E8
  CAN_PROFILE_LISTEN,     // OpenHaldex broadcast only, no decoding of vehicle frames
  CAN_PROFILE_VAG_MQB,    // MQB FCAN frames (Motor_12 0x0A8, ESP_21 0x0FD, Getriebe_11 0x0AD ...) - passive listen
  CAN_PROFILE_MAXXECU,    // 0.10.0: MaxxECU "Default CAN output" 1.2 / 1.3 from settings.maxxBaseId (0x520) - passive listen
  CAN_PROFILE_VAG_MLB,    // 0.26.0: Audi MLB drivetrain CAN (A6/A7 C7, A8 D4, Q5 8R; 8HP gearbox) - passive listen
  canProfile_t_MAX
};

// long-press menu entries that can be hidden (settings.menuMask bit = 1 << item). Settings / Back are always shown.
enum uiMenuItem_t : uint8_t
{
  MENU_HALDEX = 0,
  MENU_CAN,
  MENU_KLINE,
  MENU_OUTPUT,
  MENU_DSG,
  MENU_BLE,
  MENU_CHAIN,
  MENU_GAUGES, // 0.6.0: back to the gauge pages (first on the menu)
  MENU_ADC,    // 0.6.0: the adc2can sensors
  MENU_DRAG,   // 0.38.0: the drag timer (0-100 km/h / 0-60 mph)
  MENU_ITEM_COUNT
};
#define MENU_ALL ((1UL << MENU_ITEM_COUNT) - 1)

// how the gauge reaches the ELM327-type OBD adapter (0.8.0)
enum elmTransport_t : uint8_t
{
  ELM_TR_BLE = 0, // Bluetooth LE ("iPhone compatible" adapters)
  ELM_TR_WIFI,    // WiFi adapter: the gauge joins its AP as a station (alongside its own AP) and opens a TCP socket
  elmTransport_t_MAX
};

// settings.elmExtraSource: what else the OBD adapter does (web UI "Adapter Source")
#define ELM_EXTRA_OFF 0         // nothing: OBD PIDs / DSG only
#define ELM_EXTRA_KLINE 1       // VAG KWP2000 measuring blocks over the adapter's K-line into the engine signals
#define ELM_EXTRA_CAN_MONITOR 2 // CAN broadcast monitor (ATMA): the car's frames go into the gauge's CAN decode (0.22.0)

// where the DSG page gets its gearbox data from. The passive CAN broadcast (selector / gear) is always decoded per
// the CAN profile; these add an active diagnostic read of the gearbox (speed, optionally the gear)
enum dsgSource_t : uint8_t
{
  DSG_SRC_CAN_ONLY = 0, // passive broadcast only (no requests on the bus)
  DSG_SRC_UDS,          // UDS / OBD requests over ISO-TP on the gauge's own CAN (MQB: 0x7E1 -> 0x7E9)
  DSG_SRC_TP20,         // KWP2000 over VW TP2.0 on the gauge's own CAN (PQ35 DQ250, module 0x02)
  DSG_SRC_ELM,          // UDS / OBD requests through the BLE ELM327 (header dsgTxId)
  DSG_SRC_ELM_TP20,     // 0.8.0: KWP2000 over VW TP2.0 through the BLE ELM327 in raw CAN mode (ATCAF0)
  dsgSource_t_MAX
};

// 0.26.0: which gearbox the diagnostic read talks to (settings.dsgGearbox). The web UI fills the read fields in as a
// preset when one is picked; the firmware only uses it to turn a "table" read (DSG_FMT_TABLE) into the gauge's values.
#define GEARBOX_DSG 0    // VW DSG (DQ200 / DQ250 / DQ381 ...): the reads as they always were
#define GEARBOX_ZF_VAG 1 // ZF 8HP in an Audi / VW (AL551, AL550): UDS on 7E1 / 7E9, DIDs 2104 temperature, 210F gear
#define GEARBOX_ZF_BMW 2 // ZF 8HP in a BMW F / G-series: UDS 22 DAxx to the EGS (0x18) with BMW's extended addressing
#define GEARBOX_TYPES 3  // how many there are

// DsgReadSpec.fmt: how the bytes become a value
#define DSG_FMT_SCALED 0     // raw * scale + offset
#define DSG_FMT_HIGH_NIBBLE 1 // the high nibble of the byte (OBD PID 0xA4 actual gear)
#define DSG_FMT_TABLE 2      // 0.26.0: the raw number through the gearbox type's own table (BMW lock-up / selector codes)

// one value read from an ECU: request [svc][id...] -> positive response [svc+0x40][id echo][data...]
struct DsgReadSpec
{
  uint8_t svc;   // 0 = off, 0x22 = UDS ReadDataByIdentifier (16-bit id), 0x01 = OBD mode 01 (8-bit PID)
  uint8_t pos;   // byte offset into the data after the echo
  uint8_t len;   // 1 or 2 bytes, big-endian
  uint8_t fmt;   // DSG_FMT_SCALED / DSG_FMT_HIGH_NIBBLE / DSG_FMT_TABLE
  uint16_t id;   // DID (svc 0x22) or PID (svc 0x01)
  uint16_t spare;
  float scale;
  float offset;
};

// ===== VAG diag (0.13.0, src/can2gauge_VagDiag.cpp) =====
// Measuring values read over the gauge's own CAN (or the OBD adapter), the way a dealer scan tool's measuring values screen does
// it: UDS ReadDataByIdentifier (22 DID) on MQB-era modules, or KWP2000 measuring blocks (21 block) over TP2.0 on PQ-era
// modules. See the capture notes in docs/ for what is sent and why.
// 0.37.0: 32 values (was 12), each from its own module (VdiagValue's ids / address; 0 = the Diagnostics tab's module),
// so the car scan's values from the engine, gearbox, AWD ... all run at once. Only the values in use are asked for.
#define VDIAG_VALUES 32      // values the gauge can read at once (0.37.0: was 12)
#define VDIAG_NAME_LEN 16    // a value's name ("Boost actual")
#define VDIAG_UNIT_LEN 8     // a value's unit ("mbar")

#define VDIAG_KIND_NONE 0    // empty slot
#define VDIAG_KIND_UDS 1     // UDS DID: request 22 <id high> <id low>, the value is bits bitPos.. of the DID's data
#define VDIAG_KIND_KWP 2     // KWP2000 measuring block `id` (1..255), field 1..4 (formula byte decoded like KWP1281)
#define VDIAG_KIND_OBD 3     // 0.37.6: OBD mode 01 PID `id` asked of this value's module (the gearbox's own PIDs ...),
                             // decoded with the gauge's OBD table (01 <pid> -> 41 <pid> <data>)

#define VDIAG_PROTO_AUTO 0   // try UDS first, then TP2.0 (scan tools try both, see the capture notes in docs/)
#define VDIAG_PROTO_UDS 1    // UDS over ISO-TP only
#define VDIAG_PROTO_TP20 2   // KWP2000 over VW TP2.0 only

#define VDIAG_RUN_OFF 0      // no requests on the bus
#define VDIAG_RUN_SHOWN 1    // only while a page in the rotation has a VAG diag value (0.15.0, was: while it's on the glass), or the VAG diag tab is open
#define VDIAG_RUN_ALWAYS 2   // whenever the ignition is on (for triggers and the chain)

#define VDIAG_LINK_CAN 0     // 0.34.0: the requests go out on the gauge's own CAN wire
#define VDIAG_LINK_ADAPTER 1 // 0.34.0: ... through the OBD adapter (Bluetooth / WiFi ELM327), no CAN wire needed

#define VDIAG_FLAG_SIGNED 0x01    // a UDS value's bits are a signed (two's complement) number
#define VDIAG_FLAG_LSB_FIRST 0x02 // a UDS value longer than a byte has its least significant byte first (Intel order)

struct VdiagValue
{
  uint8_t kind;               // VDIAG_KIND_NONE / UDS / KWP
  uint8_t field;              // KWP: field 1..4 of the block
  uint8_t bitLen;             // UDS: how many bits long, 1..32 (8 = one byte, 16 = two bytes)
  uint8_t flags;              // UDS: VDIAG_FLAG_SIGNED / VDIAG_FLAG_LSB_FIRST
  uint16_t id;                // UDS: the DID (0x0000..0xFFFF); KWP: the block number 1..255
  uint16_t bitPos;            // UDS: first bit of the value in the DID's data: 0 = the top bit of the byte after the
                              //      DID echo, so byte n starts at bit n x 8 (the way OBDb counts, 0.34.0)
  float scale;                // UDS: value = raw x scale + offset (KWP: the formula byte already scales it - leave 1)
  float offset;               // UDS: added after the scale
  char name[VDIAG_NAME_LEN];  // shown on the tab and as the gauge page's value name
  char unit[VDIAG_UNIT_LEN];  // UDS: the unit to show (KWP: "" = the formula's unit)
  // 0.37.0: the module this value comes from. All 0 = the Diagnostics tab's module (its address / ids above the list).
  uint32_t txId;              // UDS request id, 11-bit (0x7E0) or 29-bit (0x17FC0076); 0 = from addr (or the tab's)
  uint32_t rxId;              // UDS answer id, 0 = worked out from the request id
  uint8_t txExt;              // extended address byte in front of every request (BMW: the module), 0 = none
  uint8_t rxExt;              // extended address byte every answer starts with (BMW: F1), 0 = none
  uint8_t addr;               // VAG diagnostic address (0x01 engine, 0x02 gearbox, 0x22 AWD ...), 0 = the tab's module
  uint8_t protocol;           // VDIAG_PROTO_AUTO / UDS / TP20 for this value's module (AUTO = the tab's setting)
}; // 52 bytes

// where the oil page (STYLE_OIL) gets the OEM oil temperature / level. Both come from the cluster, which reads the
// G266 sump sender (cluster coding 4th digit 1 = QG1 flexible service or 2 = QG2 fixed service with sensor)
enum oilSource_t : uint8_t
{
  OIL_SRC_CAN = 0, // passive: mKombi_2 0x420 byte 3 (x1 - 60 C) on the drivetrain CAN, VAG PQ profile. Temp only
  OIL_SRC_KLINE,   // KWP1281 to the cluster (0x17) while the oil page is on screen: temp + level OK / low
  oilSource_t_MAX
};

enum chainRole_t : uint8_t
{
  CHAIN_AUTO = 0, // master if this gauge has an active source (BLE ELM, OBD polling, DSG diagnostics) - lowest id wins
  CHAIN_MASTER,
  CHAIN_FOLLOWER,
  chainRole_t_MAX
};

#define ROW_MAX_POSITION 15 // settings.rowPosition 1..15 (it rides in 4 spare bits of the chain heartbeat)

// settings.canTermination (0.30.15: Auto added)
#define CAN_TERM_OFF 0  // never: the car's bus is terminated already (or a gauge mid-chain)
#define CAN_TERM_ON 1   // always: the last gauge on a chain with nothing else terminating it
#define CAN_TERM_AUTO 2 // at power-up: CAN frames heard = the bus is terminated already, leave it off; nothing heard
                        // (the bench, gauges on their own) = the first gauge to give up listening switches its 120R on

enum linkTransport_t : uint8_t
{
  LINK_AUTO = 0, // CAN when a link heartbeat was heard on CAN (and CAN TX is allowed), otherwise ESP-NOW
  LINK_CAN,
  LINK_ESPNOW,
  LINK_BOTH,
  LINK_OFF,
  linkTransport_t_MAX
};

// ignition on permanent power (src/can2gauge_Power.cpp): what says the ignition is on. With a source set, ignition off
// plays the shutdown sprite, darkens the glass (standby) and later puts the gauge into deep sleep.
enum ignSource_t : uint8_t
{
  IGN_SRC_OFF = 0, // the gauge is on switched power (ignition feed): no ignition logic, as before 0.10
  IGN_SRC_CAN,     // terminal 15 from the CAN bus (settings.ignCanMode)
  IGN_SRC_WIRE,    // the illumination input (J_OUT pin 3) wired to an ignition feed: night dimming from it is off
  IGN_SRC_CHAIN,   // 0.29.2: another gauge in the chain that has its own ignition (CAN or wire) - sleeps and wakes with it
  ignSource_t_MAX
};

enum ignCanMode_t : uint8_t
{
  IGN_CAN_PQ = 0,   // PQ35/46: BSG_Last 0x570 ZAS_Klemme_15 (bit 1), or ZAS_1 0x572 Klemme_15 (bit 1)
  IGN_CAN_MQB,      // MQB: Klemmen_Status_01 0x3C0 ZAS_Kl_15 (bit 17)
  IGN_CAN_CUSTOM,   // settings.ignCanId bit settings.ignCanBit (DBC numbering, Intel), settings.ignCanInvert
  IGN_CAN_TRAFFIC,  // any 11-bit frame on the bus (not the gauge chain's): for buses without a terminal-15 frame (Mk4)
  ignCanMode_t_MAX
};

// where a gauge page's values come from (web UI > Gauges > Source)
enum pageSource_t : uint8_t
{
  PSRC_CAN = 0, // the gauge's CAN decode (VAG PQ / MQB, OpenHaldex) or a signal of the DBC on the gauge
  PSRC_KLINE,   // a measuring-block field (K-line session to the module while the page shows)
  PSRC_OBD,     // an OBD-II PID (Bluetooth ELM327 or the wired OBD profile)
  PSRC_GAUGE,   // this gauge: board temperature, illumination, output, ME7 logger, DSG diagnostics ...
  PSRC_ADC,     // an adc2can sensor (SIG_ADC_1..16)
  PSRC_ECU,     // an aftermarket ECU over WiFi through usb2ip: its everyday values and the 8 picked channels (SIG_ECU_1..8)
  PSRC_DIAG,    // 0.13.0: one of the VAG diag tab's measuring values (UDS DID or TP2.0 block field, src/can2gauge_VagDiag.cpp)
  PSRC_CAR,     // 0.37.0: "Car" - a value the car scan found (an OBD PID, a diag value or a K-line block field), web UI only
  pageSource_t_MAX
};

enum userSigSrc_t : uint8_t
{
  USRC_NONE = 0,
  USRC_CAN_DBC, // canId + sigName decoded with the DBC on the gauge
  USRC_KLINE,   // klineAddr block klineGroup field klineField
  USRC_OBD,     // mode 01 pid
  USRC_ECU,     // a channel of an aftermarket ECU over WiFi from usb2ip: ecuKind + ecuChannel (src/can2gauge_ECU.cpp)
  USRC_DIAG,    // 0.13.0: VAG diag measuring value number diagValue (1..VDIAG_VALUES) on the VAG diag tab
  USRC_ME7,     // 0.17.6: an ME7 logger variable by its .ecu name ("nmot_w") in sigName
};

// one value of a gauge page that comes from a chosen source (web UI > Gauges > Source)
struct UserSigCfg
{
  uint8_t source;      // userSigSrc_t
  uint8_t klineAddr;   // USRC_KLINE: K-line module address
  uint8_t klineGroup;  // USRC_KLINE: measuring block 1..255
  uint8_t klineField;  // USRC_KLINE: 0..3
  uint8_t pid;         // USRC_OBD: OBD mode 01 PID
  uint8_t diagValue;   // USRC_DIAG: the VAG diag value number 1..VDIAG_VALUES (0.37.0: 32)
  uint8_t ecuKind;     // USRC_ECU: which ECU (ecunKind_t: 1 Ignitron, 2 MaxxECU)
  uint16_t ecuChannel; // USRC_ECU: the ECU's own channel number (a library row's channel, not its row number)
  uint32_t canId;      // USRC_CAN_DBC: CAN message id (bit 31 = extended)
  char sigName[40];    // USRC_CAN_DBC: the DBC signal's name; USRC_ME7: the ME7 variable's name
};

// one adc2can channel as the gauge treats it (web UI > ADC)
enum adcMode_t : uint8_t
{
  ADC_MODE_SENT = 0, // the value the adc2can sends (scaled there: linear / table / NTC)
  ADC_MODE_LINEAR,   // scaled here: value = connector volts x scale + offset
};

// ===== adc2can sensor boards ===== 0.36.6: the one place the number of boards is set. The adc2can protocol leaves room
// for more (CAN id = base + board x 16 + n; the board number is a byte over ESP-NOW): adc2can takes board ids 0-3 today
// and may go to 8. To take more here: raise ADC2CAN_BOARDS, add the matching adc17.. rows to the signal table and
// signalId_t (one block, straight after SIG_ADC_16, so sensor n is always SIG_ADC_1 + n - the build stops if they
// don't match), and bump SETTINGS_VERSION (settings.adc grows). The web UI takes the board count from /api/adc.
#define ADC2CAN_BOARDS 2                    // adc2can boards this gauge takes (board ids 0 .. ADC2CAN_BOARDS - 1)
#define ADC_SENSORS (ADC2CAN_BOARDS * 8)    // sensors: 8 per board

struct AdcChCfg
{
  uint8_t mode;     // adcMode_t
  uint8_t decimals; // 0..3, for the web UI
  float scale;      // ADC_MODE_LINEAR: units per volt
  float offset;     // ADC_MODE_LINEAR: units at 0 V
  char name[16];    // "" = the adc2can's name
  char unit[8];     // "" = the adc2can's unit (ADC_MODE_LINEAR: the unit of the result)
};

// one gauge page (web UI > Gauges), everything about it in one place
struct GaugePageCfg
{
  uint8_t enabled;  // 1 = in the swipe rotation
  uint8_t style;    // gaugeStyle_t
  uint8_t source;   // pageSource_t: what the web UI's Source shows
  uint8_t signal;   // signalId_t of the main value (SIG_USER_1 + 2n = this page's valueSigs[0])
  uint8_t signal2;  // second value for STYLE_DUAL (SIG_USER_2 + 2n = valueSigs[1])
  uint8_t slots[3]; // Quad values 2-4 / the Custom centre stack top, middle, bottom (signalId_t, SIG_NONE = empty;
                    // SIG_USER_17 + 3n + k = this page's slotSigs[k])
  char label[NAME_LEN];
  float minVal;
  float maxVal;
  float redline;    // value from which the arc turns red (<= minVal disables)
  uint8_t decimals; // 0..2
  uint8_t spriteOn; // 1 = the page shows its sprite (reacting to its reading) instead of its style
  char sprite[SPRITE_NAME_LEN]; // the page's sprite pack, /sprites/<name>.c2gs ("" = the first on the gauge)
  UserSigCfg valueSigs[2];      // the main and second value when they come from a chosen source
  UserSigCfg slotSigs[3];       // the three slots when they come from a chosen source
};

// one trigger (web UI > Triggers)
struct TriggerCfg
{
  uint8_t enabled;
  uint8_t signal;    // signalId_t
  uint8_t above;     // 1 = trigger when value >= threshold, 0 = when <= threshold
  float threshold;
  float hysteresis;  // re-arm band
  uint8_t action;    // triggerAction_t
  uint8_t output;    // IOX channel for ACT_OUTPUT (0 = OUT)
  uint8_t latch;     // 1 = stays on until acknowledged (tap screen)
  uint8_t noReading; // 0.32.0: when the value has no reading (sensor unplugged, bus quiet): 0 = ignore it (the
                     // trigger waits), 1 = count it as the condition being met
  uint16_t holdMs;   // minimum time the condition must persist before firing
  char label[NAME_LEN];
};

// Every setting of the gauge, saved in NVS as one blob ("cfg") and sent to / from the web UI as JSON (src/can2gauge_EEP.cpp).
// The sections follow the web UI and the JSON groups. A change to this layout bumps SETTINGS_VERSION.
struct Settings
{
  uint16_t magic;   // SETTINGS_MAGIC
  uint16_t version; // SETTINGS_VERSION

  // ===== Display ===== (JSON "display")
  uint8_t brightness;      // 0..100 day
  uint8_t nightBrightness; // 0..100 while the dash lights are on (night dimming)
  uint8_t theme;           // theme id (include/can2gauge_themes.h): 0-3 can2gauge palettes, 4 Custom, 5 Match car, 16+ makes
  uint32_t themeDial;      // THEME_CUSTOM dial colour 0xRRGGBB (also the Match car fallback until a VIN is read)
  uint32_t themeNeedle;    // THEME_CUSTOM needle colour
  uint8_t gaugeFeel;       // 0 = analog (needles and numbers glide like a real pointer), 1 = digital (each reading at once)
  uint16_t swipeAnimMs;    // page slide duration
  uint16_t longPressMs;    // long press to open the menu (2000; a short tap is the sprite's tickle)
  uint8_t startPage;       // the page shown first after the boot logo
  uint8_t upsideDown;      // 1 = the picture turned 180 degrees (a gauge fitted upside down)
  int8_t rotateTrim;       // extra turn in half degrees, + = clockwise as you look at the screen (-30..30 = -15..+15 degrees).
                           // Kept even when SCREEN_ROTATION is 0, so the settings layout never depends on that switch
  uint8_t renderMode;      // 0 = partial (strips in internal RAM, copied to the panel), 1 = direct (into the panel's frame
                           // buffers, swapped on vsync: no tearing) - applied at boot (lvglInit)
  uint8_t simdBlend;       // 1 = LVGL's opaque fills / RGB565 copies on the ESP32-S3 SIMD assembly (src/simd), 0 = in C

  // ===== Units ===== (JSON "units", src/can2gauge_Units.cpp)
  uint8_t units;        // the quick choice: UNITS_METRIC / UNITS_IMPERIAL set every row below, UNITS_CUSTOM = picked one by one
  uint8_t unitSpeed;    // UNIT_METRIC km/h, UNIT_IMPERIAL mph
  uint8_t unitBoost;    // boost and manifold pressure: UNIT_METRIC bar (mbar for absolute), UNIT_IMPERIAL psi, UNIT_KPA kPa
  uint8_t unitPressure; // oil, fuel and other pressures: UNIT_METRIC bar, UNIT_IMPERIAL psi, UNIT_KPA kPa
  uint8_t unitTemp;     // UNIT_METRIC C, UNIT_IMPERIAL F
  uint8_t unitMixture;  // UNIT_METRIC lambda, UNIT_IMPERIAL AFR (petrol, 14.7)
  uint8_t unitDistance; // UNIT_METRIC km, UNIT_IMPERIAL miles

  // ===== Boot logo ===== (JSON "boot")
  uint8_t bootLogo;    // 0 none, 1 Forbes Automotive (/logos/forbes.bin, else text), 2 OpenHaldex, 3 custom text, 4 /logo.bin,
                       // 5 make logo /logos/<key>.bin, 6 the can2gauge logo, 7 My Car's picture (/logos/car.bin, 0.36.2)
  char bootText[24];   // bootLogo 3: the text shown
  char logoMake[12];   // bootLogo 5: logo key ("vw", "vauxhall" ...), "" = follow the theme / the VIN's make
  uint16_t bootFadeMs; // boot logo -> first page fade (ms)

  // ===== Screensaver ===== (JSON "saver")
  uint8_t saverEnabled;              // 1 = screensaver on (off by default)
  uint16_t saverDelayS;              // screensaver after the car has been still and the screen untouched this long (30)
  char saverSprite[SPRITE_NAME_LEN]; // the screensaver's pack ("" = the first on the gauge)
  uint8_t saverReact;                // 1 = the screensaver's sprite reacts to the car (the pack's saver moods), 0 = its loop only

  // ===== Power and ignition ===== (JSON "power", src/can2gauge_Power.cpp)
  uint8_t ignSource;                    // ignSource_t: what says the ignition is on (IGN_SRC_OFF = switched power)
  uint8_t ignCanMode;                   // ignCanMode_t
  uint32_t ignCanId;                    // IGN_CAN_CUSTOM: frame id (bit 31 = extended)
  uint8_t ignCanBit;                    // IGN_CAN_CUSTOM: 0..63, byte * 8 + bit (the DBC start bit of a 1-bit Intel signal)
  uint8_t ignCanInvert;                 // IGN_CAN_CUSTOM: 1 = bit clear means ignition on
  uint8_t ignOffDelayS;                 // ignition off this long before the shutdown starts (2)
  uint8_t standbyMin;                   // dark standby (instant wake) this long before deep sleep, 0 = deep sleep at once (10)
  char shutdownSprite[SPRITE_NAME_LEN]; // ignition off / wake pack ("" = a plain fade)

  // ===== Output and dash lights ===== (JSON "io")
  uint8_t outputMode;                 // 0 = high-side (OUT switches to 12 V, load to GND), 1 = low-side (OUT switches to GND)
  uint8_t outputManual[IOX_CHANNELS]; // web-UI manual override state (0 off, 1 on, 2 auto/trigger)
  uint8_t nightSource;                // 0 = off, 1 = dash-light input (J_OUT pin 3) dims the screen
  uint8_t dimInvert;                  // 1 = input LOW means lights on

  // ===== Gauge pages and triggers ===== (JSON "pages", "triggers")
  GaugePageCfg pages[MAX_PAGES];
  TriggerCfg triggers[MAX_TRIGGERS];

  // ===== Menu and web UI tabs ===== (JSON "menu")
  uint32_t menuMask; // bit per uiMenuItem_t: 1 = shown on the long-press menu
  uint8_t tabsShown; // web UI tabs that have no item on the screen's menu: TAB_SHOW_* bits, set = shown

  // ===== CAN bus ===== (JSON "can")
  uint8_t canEnabled;
  uint16_t canKbps;        // 250 / 500 / 1000
  uint8_t canProfile;      // canProfile_t
  uint8_t canTermination;  // CAN_TERM_OFF / ON (drive Q_TERM, 120R in circuit) / AUTO (on only if the bus is silent at power-up)
  uint8_t useCanRelay;     // 1 = use the CAN frames an OpenHaldex / adc2can relays over WiFi (src/can2gauge_CanRelay.cpp)
  uint16_t maxxBaseId;     // CAN_PROFILE_MAXXECU: the first frame's id (0x520; MTune's "CAN ID OFFSET" moves it)
  uint16_t dbcSlotBase[3]; // per DBC slot (car, gearbox, extra): where a relocatable ECU's first frame really is
                           // (EMU Black / MaxxECU base id moved in the ECU's software), 0 = as written in the DBC

  // ===== OBD-II on the gauge's CAN ===== (JSON "obd")
  uint8_t obdOverCan; // 1 = ask the engine for OBD-II PIDs on 0x7DF next to the CAN profile (VAG PQ / MQB / MaxxECU)
  uint8_t obdEcu;     // the ECU it reads: 0 = the engine (or the first to answer), 1..8 = answer id 0x7E8..0x7EF (0x7E9 = gearbox)

  // ===== K-line ===== (JSON "kline")
  uint8_t klineProtocol; // klineProtocol_t
  uint8_t klineEcuAddr;  // 0x01 engine (KWP1281), 0x10 / 0x33 for KWP2000
  uint8_t klineModeAddr; // menu > K-line: the module whose measuring blocks are shown (0x01 engine, 0x17 cluster ...),
                         // separate from klineEcuAddr so browsing the ABS doesn't move the engine logging
  uint16_t klineBaud;    // 10400 default; ME7Logger LogSpeed overrides
  uint8_t klineFastObd;  // 1 = Fast OBD on the gauge's own K-line (KWP2000): shorter P3 / P4 gaps

  // ===== ME7 logger ===== (JSON "me7")
  char me7EcuFile[FILE_LEN];   // the ME7Logger .ecu file on the gauge
  char me7CfgFile[FILE_LEN];   // the ME7Logger .cfg file (which variables to log)
  uint8_t me7SamplesPerSecond; // samples a second
  uint8_t me7LogToFile;        // 1 = write CSV to LittleFS while logging
  uint8_t me7FastMode;         // 1 = Fast (B7 RAM routine), 0 = Normal logging. Test link is never saved

  // ===== OBD adapter (ELM327, Bluetooth or WiFi) ===== (JSON "elm", docs/can2gauge_BLE_ELM327_brief.md)
  uint8_t elmEnabled;
  uint8_t elmTransport;   // elmTransport_t: Bluetooth LE or WiFi
  uint8_t elmPollObd;     // poll standard mode 01 PIDs into the gauges
  uint8_t elmFastObd;     // 1 = Fast OBD on the adapter: "one answer" request digit + ATAT2
  char elmWifiSsid[36];   // a WiFi adapter's AP ("" = join the first OBD-looking open network found, then saved)
  char elmWifiPass[68];   // "" = open (most are)
  char elmWifiHost[16];   // its IP, "192.168.0.10" on most
  uint16_t elmWifiPort;   // its TCP port (35000 on most)
  uint8_t elmExtraSource; // ELM_EXTRA_* (the web UI's "Adapter Source"): what else the adapter does next to OBD PIDs / DSG
  uint8_t elmKlAddr;      // ELM_EXTRA_KLINE: K-line module address (0x01 engine)
  uint8_t elmKlFast;      // ELM_EXTRA_KLINE: 1 = fast init (ISO 14230), 0 = 5-baud init to elmKlAddr

  // ===== Gearbox (DSG / ZF 8HP) ===== (JSON "dsg", menu > DSG, STYLE_GEAR pages)
  uint8_t dsgGearbox;        // GEARBOX_DSG / GEARBOX_ZF_VAG / GEARBOX_ZF_BMW
  uint8_t dsgSource;         // dsgSource_t
  uint8_t dsgUdsSession;     // 0x01 = stay in the default session, 0x03 = extended session + TesterPresent
  uint8_t dsgSpeedPref;      // 0 = gearbox speed when valid, else vehicle speed; 1 = gearbox only; 2 = vehicle only
  uint16_t dsgTxId;          // gearbox request id (UDS / ELM header), 0x7E1
  uint16_t dsgRxId;          // gearbox response id, 0x7E9
  uint8_t dsgTxExtAddr;      // extended addressing: the first byte of every frame sent (BMW EGS 0x18), 0 = normal addressing
  uint8_t dsgRxExtAddr;      // ... and of every answer (BMW: 0xF1, the tester)
  uint16_t dsgPollMs;        // gap between gearbox reads
  DsgReadSpec dsgSpeed;      // UDS / ELM: how to read the gearbox speed
  DsgReadSpec dsgGear;       // UDS / ELM: optional gear read (svc 0 = off - the gear then comes from the CAN broadcast)
  DsgReadSpec dsgTemp;       // gearbox oil temperature -> SIG_GEARBOX_TEMP (read every 10th pass: it changes slowly)
  DsgReadSpec dsgLockup;     // torque converter lock-up -> SIG_LOCKUP
  DsgReadSpec dsgSelector;   // selector position -> SIG_SELECTOR (when the CAN broadcast doesn't give it, e.g. through the adapter)
  uint8_t dsgTp20Module;     // TP2.0 logical address of the gearbox (0x02)
  uint8_t dsgTp20Group;      // measuring block with the speed
  uint8_t dsgTp20SpeedField; // 0..3 in that block, 0xFF = none
  float dsgTp20SpeedDiv;     // 0 = use the VAG formula byte (02E block 1 field 4 is formula 0x07 = km/h); > 0 = (a*256 + b) / div
  uint8_t dsgTp20GearGroup;  // measuring block with the gear, 0 = none
  uint8_t dsgTp20GearField;  // 0..3

  // ===== VAG diag ===== (JSON "vdiag", src/can2gauge_VagDiag.cpp): measuring values from one module over the gauge's
  // CAN, UDS or KWP2000 over TP2.0, with a Turbo mode and grouped requests
  uint8_t vdiagRun;        // VDIAG_RUN_OFF / SHOWN / ALWAYS
  uint8_t vdiagAddr;       // the module's VAG diagnostic address (0x01 engine, 0x02 gearbox, 0x22 AWD ...)
  uint8_t vdiagProtocol;   // VDIAG_PROTO_AUTO / UDS / TP20
  uint8_t vdiagTurbo;      // 1 = Turbo: requests back to back, UDS DIDs grouped (the capture's fast mode with grouped UDS requests)
  uint8_t vdiagGroupSize;  // Turbo: most DIDs in one UDS request, 1..8 (the group size seen in the capture)
  uint8_t vdiagSession;    // UDS session opened first: 0x01 default (none), 0x03 extended (what the capture opens)
  uint8_t vdiagTp20Module; // TP2.0 module id for the 0x200 setup, 0 = from the address table (0x22 -> 0x0A ...)
  uint8_t vdiagLink;       // 0.34.0: VDIAG_LINK_CAN (the gauge's CAN) / VDIAG_LINK_ADAPTER (the OBD adapter)
  uint8_t vdiagTxExt;      // 0.34.0: extended address byte in front of every request (BMW: the module's address), 0 = none
  uint8_t vdiagRxExt;      // 0.34.0: extended address byte every answer starts with (BMW: F1, the tester), 0 = none
  uint16_t vdiagPollMs;    // normal mode: gap after each request (Turbo ignores it)
  uint32_t vdiagTxId;      // UDS request id, 11-bit (0x7E0) or 29-bit (0x17FC0076, 0x18DA01F1); 0 = the address table
  uint32_t vdiagRxId;      // UDS answer id, 0 = worked out from the request (0x7xx + 0x6A, 18DA xx F1 -> 18DA F1 xx ...)
  VdiagValue vdiagValues[VDIAG_VALUES];

  // ===== OEM oil temperature / level ===== (JSON "oil", STYLE_OIL pages, docs/FIRMWARE.md "OEM oil temp / level")
  uint8_t oilSource;      // oilSource_t
  uint8_t oilClusterAddr; // KWP1281 address of the cluster, 0x17
  uint8_t oilGroup;       // measuring block, 0 = auto from the cluster part number (1J0/3B0-920: 3, others: 50)
  uint8_t oilTempField;   // 0..3 in that block (used when oilGroup != 0)
  uint8_t oilLevelField;  // 0..3, 0xFF = none (used when oilGroup != 0)
  uint8_t oilLevelOkZero; // 1 = raw 0 means level OK; 0 (default) = non-zero means OK
  uint16_t oilPollMs;     // gap between measuring-block reads

  // ===== adc2can sensor boards ===== (JSON "adc", src/can2gauge_ADC2CAN.cpp)
  uint32_t adcBaseId;  // 29-bit: the adc2can "base id" (default 0x0AD2C000); board b sends base + b*16 + 0..7
  uint8_t adcWireless; // 1 = take adc2can boards over ESP-NOW too (adc2can 1.3+, include/adcnow_proto.h); CAN wins
  AdcChCfg adc[ADC_SENSORS]; // board 0 ch 1-8, board 1 ch 1-8 ... (ADC_SENSORS = ADC2CAN_BOARDS x 8)

  // ===== OpenHaldex ===== (JSON "haldex", src/can2gauge_OHX.cpp, include/ohx_proto.h)
  uint16_t haldexBroadcastId; // its status frame on CAN, 0x6B0
  uint16_t haldexControlId;   // its control frame on CAN, 0x6A0
  uint8_t ohxWireless;        // 1 = listen for OpenHaldex over ESP-NOW and send it commands (default on)
  uint8_t ohxCarSignals;      // 1 = take speed / rpm / boost / pedal from it while nothing else on the gauge provides them
  char ohxKey[68];            // its WiFi AP password ("" = open AP) - signs the commands

  // ===== usb2ip (an aftermarket ECU over WiFi) ===== (JSON "ecu", src/can2gauge_ECU.cpp, include/ecunow_proto.h)
  uint8_t ecuWireless;   // 1 = listen for usb2ip over ESP-NOW (default on: costs nothing, a gauge with power only needs it)
  uint8_t ecuCarSignals; // 1 = its rpm / boost / coolant ... fill the gauge's own signals while nothing else provides them

  // ===== Nav link ===== (JSON "nav", src/can2gauge_Nav.cpp)
  uint8_t navMode;      // NAV_MODE_OFF / NEAR_TURN / WHOLE_ROUTE
  uint16_t navPopupM;   // near-turn pop-up distance in metres (NAV_POPUP_STEP_M steps)

  // ===== Data logger ===== (JSON "log", src/can2gauge_Logger.cpp). The recording itself lives in PSRAM only.
  uint8_t logSignals[LOG_CHANNELS]; // signalId_t of each channel, SIG_NONE = channel not used
  uint8_t logRateHz;                // samples a second: 1, 2, 5, 10, 20 or 50
  uint8_t logAutoStart;             // 1 = start recording by itself at power-up / ignition on
  uint8_t logManualChannels;        // 0 = log the values the active gauge pages show, in gauge order; 1 = the channels above

  // ===== Daisy chain ===== (JSON "chain", docs/can2gauge_GaugeChain_design.md)
  uint8_t chainEnabled;
  uint8_t chainRole;       // chainRole_t
  uint8_t nodeId;          // 0..15, 0xFF = auto (low 4 bits of the MAC, bumped on a clash)
  char nodeName[NAME_LEN];
  uint16_t chainGroup;     // only frames from the same group are accepted
  uint8_t linkTransport;   // linkTransport_t
  uint8_t linkCanTx;       // allow link frames on CAN (off = ESP-NOW only, for a car bus you'd rather not add to)
  uint16_t linkCanBaseId;  // DATA 0x6C0+node, HEARTBEAT +0x10+node, COMMAND +0x20
  uint8_t followPage;      // followers mirror the master's page changes
  uint8_t followNight;     // followers use the master's dash-light state
  uint8_t failover;        // an Auto follower takes over when the master's heartbeat stops (3 s)
  uint8_t rowPosition;     // the row wave: where this gauge sits, counted from the left as you look at the dash
                           // (1 = leftmost), 0 = not in a row (the normal goodbye). Sent in the chain heartbeat.

  // ===== WiFi ===== (JSON "wifi")
  uint8_t wifiMode; // 0 AP always, 1 AP + STA bridge, 2 AP off after 5 min idle (low power)
  char apSsid[WIFI_LEN];
  char apPassword[WIFI_LEN];
  char staSsid[WIFI_LEN];
  char staPassword[WIFI_LEN];
  char hostname[NAME_LEN];

  // ===== Learnt by the gauge ===== (JSON "learnt"): worked out or paired by the gauge itself, not set by hand
  uint8_t themeMatched;   // THEME_MATCH: theme id picked from the last VIN, THEME_NONE until one is read
  char matchedLogo[12];   // logo key of the make from the last VIN ("" = none)
  uint8_t ecuKind;        // ecunKind_t the followed usb2ip last said it reads (the web UI lists that ECU's channels)
  char elmMac[18];        // the paired OBD adapter "aa:bb:cc:dd:ee:ff" ("" = bind the first adapter found)
  uint8_t elmAddrType;    // its BLE address type
  uint8_t elmProtocol;    // its last detected ATDPN protocol (0 = auto search on connect)
  uint8_t ohxMac[6];      // the OpenHaldex this gauge follows (all zero = the first one heard, then saved)
  uint8_t ecuMac[6];      // the usb2ip this gauge follows (all zero = the first one heard, then saved)
  uint32_t runnerBest;    // Rev Runner's high score (docs/mascot/RUNNER.md), kept through a power cycle
};
// settings.tabsShown bits: the web UI tabs that aren't on the screen's menu (the others follow settings.menuMask)
#define TAB_SHOW_OIL 0x01      // Oil tab
#define TAB_SHOW_ECU 0x02      // ECU tab (usb2ip)
#define TAB_SHOW_TRIGGERS 0x04 // Triggers tab
#define TAB_SHOW_ALL 0x07      // every one of them

extern Settings settings;
extern SemaphoreHandle_t settingsMutex;
extern bool settingsDirty; // set by anything that changes settings; EEP task persists

// ---------------------------------------------------------------------------
// Runtime status (read by the web UI status endpoint and the on-screen menu)
// ---------------------------------------------------------------------------
// OEM oil source: written by the K-line task (cluster session) and the CAN decoder (Kombi_2), read by the oil page + API
struct OilStatus
{
  // K-line cluster session
  bool connecting;       // 5-baud init / identification in progress
  bool connected;        // KWP1281 session to the cluster is open
  char state[40];        // shown on the oil page under the value
  char clusterId[48];    // identification string ("1J0920926C  KOMBI+WEGFAHRSP VDO V05")
  uint8_t group;         // measuring block being read
  uint8_t tempField;     // 0..3
  uint8_t levelField;    // 0..3, 0xFF = none
  uint8_t fieldCount;    // fields in the last block
  uint8_t formula[4], a[4], b[4]; // raw fields of the last block (web UI shows them for bench mapping)
  float value[4];
  char unit[4][8];
  uint32_t reads, errors;
  uint32_t connects;     // successful cluster logins since boot
  // CAN mKombi_2 0x420
  uint32_t kombi2Ms;         // millis() of the last frame, 0 = never
  uint32_t kombi2Count;
  uint8_t kombi2OilRaw;      // byte 3 (KO2 Oeltemperatur, x1 - 60)
  bool kombi2OilErr;         // byte 0 bit 1 Fehlerstatus_Oeltemperatur
};

// K-line mode (menu > K-line): a KWP1281 session to settings.klineEcuAddr while the K-line screen is showing, one
// measuring block at a time; swiping steps the block (src/can2gauge_KLine.cpp runKlineBrowse)
struct KlineBrowse
{
  bool connecting;       // 5-baud init / identification
  bool connected;
  uint8_t addr;          // module address being read
  char state[48];        // shown on the glass under the block
  char ecuId[48];        // identification ("1J0920926C  KOMBI+WEGFAHRSP VDO V05")
  uint8_t group;         // block showing / being read
  bool searching;        // the last block was empty: moving on in the swipe direction
  uint8_t fieldCount;    // fields in the block (0..4)
  char text[64];         // 0.17.4: a text block (ME7 81 = VIN) instead of fields, "" = none
  uint8_t formula[4], a[4], b[4];
  float value[4];
  char unit[4][8];
  uint32_t reads, errors;
  uint32_t blockMs;      // millis() of the last good read
  volatile int8_t step;  // swipe request: +1 / -1 (consumed by the K-line task)
  volatile int16_t gotoGroup; // web UI jump, -1 = none
  // Measuring-block labels (src/can2gauge_Labels.cpp): the file in use, the others that fit, and the showing block's names
  char labelFile[40];         // "" = none found
  char labelCandidates[6][40];     // best first (the web UI's pick first)
  uint8_t labelCandidateCount;
  char title[40];             // the block's title ("Basic Functions (Mass Air Flow)")
  char fieldName[4][40];      // "Coolant Temperature", "" = not labelled
  char fieldSpec[4][80];      // "Specification: 85.0...110.0 °C"
  volatile bool labelReload;  // the web UI picked another file
};

// a gauge page whose source is a K-line measuring block: one session to the module, its block(s) read back to back
struct KlinePage
{
  bool connecting, connected;
  uint8_t addr;
  char state[48]; // shown under the value while it has no data: "Waking module 0x01 (5-baud init)"
  uint32_t reads, errors;
};

struct RuntimeStatus
{
  bool canUp;
  uint32_t canRxCount;
  uint32_t canTxCount;
  uint32_t canErrCount;
  uint32_t canRxPerSec;
  bool haldexLink;
  bool klineUp;
  char klineState[24];
  char klineEcuId[48];
  uint32_t klineSamples;
  bool me7Logging;
  char me7LogFile[FILE_LEN];
  uint32_t me7SampleRate; // measured samples/s
  bool wifiStaConnected;
  char wifiStaIP[16];
  uint8_t activePage;
  bool alertActive;
  char alertText[32];
  volatile uint32_t alertAcks; // 0.32.0: counts up each time the driver taps an alert away (the triggers then stay quiet)
  OilStatus oil;
  volatile uint32_t oilPageSeenMs; // last refresh of a STYLE_OIL page on screen (GaugeUI); the K-line task follows it
  KlineBrowse klineBrowse;
  KlinePage klinePage;                      // the K-line session behind a gauge page (source K-line)
  volatile uint32_t klinePageSeenMs;     // a page with a K-line source is on the glass (GaugeUI refreshPage)
  volatile uint8_t klinePageCfgIndex;         // ... and which one (index in settings.pages)
  volatile uint32_t pageSeenMs;     // a gauge page is on the glass (GaugeUI pageClaims): the OBD polling puts its PIDs first
  volatile uint8_t pageCfgIndex;    // ... and which one (index in settings.pages)
  // mode screens on the glass (or the web UI's live view) stamp these; the owning task connects while they are fresh
  volatile uint32_t klineModeSeenMs; // menu > K-line (or web UI > K-line > Measuring blocks open)
  volatile uint32_t elmModeSeenMs;   // menu > Bluetooth OBD
  volatile uint32_t vdiagSeenMs;     // 0.13.0: web UI > VAG diag open (polling /api/vdiag) - the VAG diag task connects
  // OpenHaldex: when its state last arrived over CAN (0x6B0) / over WiFi (ESP-NOW). Wireless carries the engagement %
  // as OpenHaldex shows it, so the CAN decode yields while wireless is fresh; mode requests go over CAN when it is heard
  volatile uint32_t haldexCanMs;
  volatile uint32_t ohxRxMs;
  // ignition on permanent power (src/can2gauge_Power.cpp). In standby the glass is dark, CAN is listen-only and the
  // tasks that talk to the car (OBD polling, K-line, the OBD adapter, gearbox reads) stand down, so the gauge never
  // keeps the car's buses awake.
  volatile bool standby;
  volatile bool otherScanners; // 0.37.0: Enable Other Scanners - no UDS / TP2.0 / K-line / OBD requests from this gauge, so
                               // another scan tool can talk to the car. Not saved: off again after a restart.
  bool wokeFromSleep; // this boot is a deep-sleep wake with the ignition on: the wake clip plays instead of the boot logo
};
#define MODE_RELEASE_MS 3000 // a mode's session closes this long after its screen left (a menu peek costs no reconnect)
extern RuntimeStatus runtime;

// helpers shared across modules

// A big table taken from PSRAM instead of internal RAM, zero-filled. The ESP32-S3's internal RAM (~320 KB) is what
// WiFi, the web server's network buffers, the tasks' stacks and every lock/queue must use, and it ran out (2026-10-14:
// opening index.html aborted with "failed to create mutex"). Tables only tasks touch can live in the 8 MB PSRAM.
// Used as the starting value of a table: `static CanMonEntry *entries = (CanMonEntry *)psramCalloc(N, sizeof(...))`.
// PSRAM is ready before any of that runs (CONFIG_SPIRAM_BOOT_INIT), so the table exists before setup(). If PSRAM is
// somehow missing it falls back to internal RAM rather than handing back nothing.
static inline void *psramCalloc(size_t count, size_t size)
{
  void *memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // PSRAM, zero-filled
  if (!memory)
  {
    memory = calloc(count, size); // no PSRAM: internal RAM, as before
  }
  return memory;
}

// The free internal RAM and its largest single piece, for the boot log (the RAM WiFi, the web server and the tasks
// share - PSRAM is reported separately in the web UI's Diag tab)
static inline void printInternalRam(const char *where)
{
  DEBUG("RAM after %s: %u KB internal free (largest piece %u KB)", where,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

static inline void copyStr(char *dst, size_t dstLen, const char *src)
{
  strncpy(dst, src ? src : "", dstLen - 1);
  dst[dstLen - 1] = '\0';
}

// printf onto the end of buf, where written is the length of the text so far. snprintf returns the length the text
// WOULD have had, so a plain `written += snprintf(buf + written, len - written, ...)` runs past len once a line is cut
// short, and the next call's `len - written` wraps round to a huge size_t. This keeps written at the real string length
// (at most len - 1) and does nothing once the buffer is full.
static inline void appendf(char *buf, size_t len, size_t &written, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static inline void appendf(char *buf, size_t len, size_t &written, const char *fmt, ...)
{
  if (!len)
  {
    return;
  }
  if (written + 1 >= len)
  {
    buf[len - 1] = '\0'; // full: still a terminated string
    return;
  }
  va_list args;
  va_start(args, fmt);
  const int added = vsnprintf(buf + written, len - written, fmt, args);
  va_end(args);
  if (added > 0)
  {
    written += (size_t)added;
  }
  if (written > len - 1)
  {
    written = len - 1;
  }
}
