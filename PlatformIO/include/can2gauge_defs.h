#pragma once

// ===== Includes =====
#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCoreWithCaps: a task whose stack is in PSRAM
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h" // psramCalloc / printInternalRam below: PSRAM or internal RAM

#include <can2gauge_pins.h>
#include <can2gauge_ver.h>
#include <can2gauge_signals.h>
#include <c2gs.h> // SPRITE_NAME_LEN

// The lib/ libraries (CANLine, GaugeUI, ME7Logger) include this header and PlatformIO compiles them on their own, so it
// stays light: the app's heavy includes live where they are used.
//   Preferences        -> can2gauge_EEP.cpp
//   WiFi / ESPmDNS     -> can2gauge_WiFi.cpp, can2gauge_API.cpp
//   AsyncWebServer/TCP -> can2gauge_API.h (declares the server), so API.cpp / OTA.cpp
//   ArduinoJson        -> can2gauge_EEP.h (JSON in its functions), so EEP / API / OTA
//   LittleFS           -> can2gauge_API.cpp, can2gauge_KLine.cpp, can2gauge_ME7Read.cpp

// ===== Debug switches =====
#define enableDebug 1          // 1 = debug messages on the USB serial ** CAN CHANGE THIS **
#define debugRamRegions 0      // 1 = list every internal RAM area at boot (bench) ** CAN CHANGE THIS **
#define debugI2cScan 0         // 1 = list every I2C address that answers when no touch chip is found (slows boot) ** CAN CHANGE THIS **
#define detailedDebug 0        // more detailed messages ** CAN CHANGE THIS **
#define detailedDebugCAN 0     // CAN frames (very verbose) ** CAN CHANGE THIS **
#define detailedDebugKLine 0   // K-line bytes (very verbose) ** CAN CHANGE THIS **
#define detailedDebugWiFi 0    // WiFi ** CAN CHANGE THIS **
#define detailedDebugEEP 0     // settings saves ** CAN CHANGE THIS **
#define detailedDebugIO 0      // output / dash-light input ** CAN CHANGE THIS **
#define detailedDebugUI 0      // the screen (LVGL) ** CAN CHANGE THIS **
#define detailedDebugLink 0    // gauge-chain frames (verbose) ** CAN CHANGE THIS **
#define detailedDebugDSG 0     // gearbox UDS / TP2.0 traffic (verbose) ** CAN CHANGE THIS **
#define detailedDebugELM 0     // OBD adapter command / answer lines (verbose) ** CAN CHANGE THIS **
#define detailedDebugVagDiag 0 // VAG diag requests and answers (verbose) ** CAN CHANGE THIS **

// USB bench console (src/can2gauge_Console.cpp): a PC script drives the gauge over the USB cable with the web UI's requests
#define enableConsole 1      // 1 = listen for console commands on the USB serial ** CAN CHANGE THIS **
#define CONSOLE_MARK "@@C2G" // every console reply line starts with this, so the script can tell it from debug lines

// debug printing: DEBUG adds a new line, DEBUG_ doesn't. Both vanish with enableDebug 0.
#if enableDebug
#define DEBUG(x, ...) Serial.printf(x "\n", ##__VA_ARGS__)
#define DEBUG_(x, ...) Serial.printf(x, ##__VA_ARGS__)
#else
#define DEBUG(x, ...)
#define DEBUG_(x, ...)
#endif

// ===== Timings =====
#define eepRefresh 2000     // ms between checks for settings to save
#define ioRefresh 100       // ms between dash-light input / output fault reads
#define triggerRefresh 100  // ms between trigger checks
#define uiRefresh 20        // ms between gauge value refreshes (50 Hz: needles glide between readings)
#define canStatsRefresh 1000 // ms between CAN bus statistics
#define engineTimeout 2000  // ms without an engine frame before the value counts as stale
#define haldexTimeout 1500  // ms without an OpenHaldex broadcast before the page says "no link"
#define obdPollRefresh 100  // ms between updates of who polls OBD-II and which PIDs the page shows (requests go as fast
                            // as the ECU answers)
#define oilReleaseMs 3000   // K-line oil: the cluster session closes this long after the oil page left the screen
#define kombi2Timeout 2000  // ms without mKombi_2 (0x420) before the oil page says the frame is lost
#define linkRefresh 10      // gauge-chain task tick in ms
#define MODE_RELEASE_MS 3000 // a mode screen's session closes this long after it left the screen (a menu peek costs no reconnect)

// ===== Small helpers =====
// A big table from PSRAM rather than internal RAM, zero-filled. Internal RAM (~320 KB) is what WiFi, the web server,
// the task stacks and every lock must share, and it ran out (2026-10-14: "failed to create mutex"). PSRAM is ready
// before setup() runs; without it this falls back to internal RAM rather than giving nothing.
// Use: static CanMonEntry *entries = (CanMonEntry *)psramCalloc(N, sizeof(CanMonEntry));
static inline void *psramCalloc(size_t count, size_t size)
{
  void *memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // PSRAM, zero-filled
  if (!memory)
  {
    memory = calloc(count, size); // no PSRAM: internal RAM
  }
  return memory;
}

// the free internal RAM and its largest piece, for the boot log (PSRAM is on the web UI's Diag tab)
static inline void printInternalRam(const char *where)
{
  DEBUG("RAM after %s: %u KB internal free (largest piece %u KB)", where,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

// text into a fixed-size char array, always ended, never past its size (NULL copies as "")
static inline void copyStr(char *dst, size_t dstLen, const char *src)
{
  if (src == NULL)
  {
    src = "";
  }
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

// printf onto the end of buf, where written is the length so far. snprintf returns the length the text WOULD have had,
// so a plain "written += snprintf(...)" runs past the end once a line is cut short. This keeps written at the real
// length (at most len - 1) and does nothing once buf is full.
static inline void appendf(char *buf, size_t len, size_t &written, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static inline void appendf(char *buf, size_t len, size_t &written, const char *fmt, ...)
{
  if (!len)
  {
    return;
  }
  if (written + 1 >= len)
  {
    buf[len - 1] = '\0'; // full: still an ended string
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

// ===== Settings: sizes =====
#define SETTINGS_MAGIC 0xC26A
#define SETTINGS_VERSION 32 // the saved layout: settings saved by any other version (or size) aren't read, the gauge starts
                            // from defaults (a Download Backup file restores them: it goes by name). 31 = fields by type,
                            // 32 = every Menu and Tabs switch is a menu item (tabsShown gone)
#define MAX_PAGES 8
#define MAX_TRIGGERS 8
#define NAME_LEN 16
#define UNIT_LEN 8
#define WIFI_LEN 32
#define FILE_LEN 40
#define LOG_CHANNELS 16 // values the data logger records side by side

// ===== Settings: choices =====
enum gaugeStyle_t : uint8_t
{
  STYLE_ARC = 0, // big arc with the value in the middle (default)
  STYLE_NEEDLE,  // dial with a needle and scale ticks
  STYLE_BAR,     // horizontal bar with the value
  STYLE_NUMERIC, // the number only ("Single" in the web UI)
  STYLE_DUAL,    // two values stacked (signal + signal2)
  STYLE_HALDEX,  // OpenHaldex: lock target / actual arc and the mode
  STYLE_QUAD,    // four values: the page's and its three slots
  STYLE_GEAR,    // DSG / auto: selector, gear and speed (as menu > DSG)
  STYLE_OIL,     // the cluster's oil temperature and level (CAN Kombi_2 or K-line 0x17), with the link state ("Oil (PQ)")
  STYLE_CUSTOM,  // main arc plus three small readouts in the middle (the page's slots)
  STYLE_GRAPH,   // the value over the last 30 s as a rolling line, the number above it
  STYLE_TRIPLE,  // three values: the page's on top, slots 1 and 2 below
  gaugeStyle_t_MAX
};

enum triggerAction_t : uint8_t
{
  ACT_ALERT = 0, // alert on the screen
  ACT_OUTPUT,    // switch the output (J_OUT pin 1)
  ACT_ALERT_OUT, // both
  triggerAction_t_MAX
};

enum klineProtocol_t : uint8_t
{
  KL_OFF = 0,
  KL_KWP1281,   // VAG 5-baud init, block protocol (Mk4 ME7 / Marelli ... at address 0x01)
  KL_KWP2000,   // ISO 14230 fast init, OBD-II mode 01 PIDs
  KL_ME7LOGGER, // ME7Logger-style fast RAM logging from .ecu / .cfg files on the gauge
  // APPEND ONLY: saved settings hold these numbers
  KL_KWP2000VAG, // VAG KWP2000 measuring blocks via the double slow init (faster than KWP1281), else KWP1281
  klineProtocol_t_MAX
};

enum canProfile_t : uint8_t
{
  CAN_PROFILE_VAG_PQ = 0, // Mk4 / Mk5-era PQ frames (Motor_1 0x280 ...), listen only
  CAN_PROFILE_OBD2,       // asks OBD-II PIDs on 0x7DF, reads 0x7E8
  CAN_PROFILE_LISTEN,     // OpenHaldex only, no car frames decoded
  CAN_PROFILE_VAG_MQB,    // MQB frames (Motor_12 0x0A8, ESP_21 0x0FD, Getriebe_11 0x0AD ...), listen only
  CAN_PROFILE_MAXXECU,    // MaxxECU "Default CAN output" 1.2 / 1.3 from settings.maxxBaseId (0x520), listen only
  CAN_PROFILE_VAG_MLB,    // Audi MLB drivetrain CAN (A6 / A7 C7, A8 D4, Q5 8R; 8HP gearbox), listen only
  canProfile_t_MAX
};

// the screen's menu items, one per Settings > Device > Menu and Tabs switch (settings.menuMask bit = 1 << item); each
// switch shows or hides the item and its web UI tab together. Settings always shows. New items go on the end: the
// numbers are the bits saved in menuMask.
enum uiMenuItem_t : uint8_t
{
  MENU_HALDEX = 0,
  MENU_CAN,
  MENU_KLINE,
  MENU_OUTPUT,
  MENU_DSG,
  MENU_BLE,
  MENU_CHAIN,
  MENU_GAUGES,   // back to the gauge pages (first on the menu)
  MENU_ADC,      // the adc2can sensors
  MENU_DRAG,     // the drag timer (0-100 km/h / 0-60 mph)
  MENU_NAV,      // the phone's turn-by-turn: the nav link's state, and Off / Pop Up / Route
  MENU_ECU,      // usb2ip: an aftermarket ECU over WiFi
  MENU_OIL,      // the cluster's oil temperature and level
  MENU_TRIGGERS, // the set-point triggers, and which have fired
  MENU_ITEM_COUNT
};
#define MENU_ALL ((1UL << MENU_ITEM_COUNT) - 1)

// how the gauge reaches the ELM327-type OBD adapter
enum elmTransport_t : uint8_t
{
  ELM_TR_BLE = 0, // Bluetooth LE ("iPhone compatible" adapters)
  ELM_TR_WIFI,    // WiFi adapter: the gauge joins its network as a station (its own AP stays up) and opens a TCP socket
  elmTransport_t_MAX
};

// settings.elmExtraSource: what else the OBD adapter does (web UI "Adapter Source")
#define ELM_EXTRA_OFF 0         // nothing: OBD PIDs / gearbox only
#define ELM_EXTRA_KLINE 1       // VAG KWP2000 measuring blocks over the adapter's K-line into the engine values
#define ELM_EXTRA_CAN_MONITOR 2 // CAN monitor (ATMA): the car's frames go into the gauge's CAN decoding

// where the gearbox page gets its data. The CAN broadcast (selector / gear) is always decoded per the CAN profile;
// these add a diagnostic read of the gearbox (speed, optionally the gear)
enum dsgSource_t : uint8_t
{
  DSG_SRC_CAN_ONLY = 0, // the broadcast only (nothing asked on the bus)
  DSG_SRC_UDS,          // UDS / OBD over ISO-TP on the gauge's CAN (MQB: 0x7E1 -> 0x7E9)
  DSG_SRC_TP20,         // KWP2000 over VW TP2.0 on the gauge's CAN (PQ35 DQ250, module 0x02)
  DSG_SRC_ELM,          // UDS / OBD through the OBD adapter (header dsgTxId)
  DSG_SRC_ELM_TP20,     // KWP2000 over VW TP2.0 through the OBD adapter in raw CAN mode (ATCAF0)
  dsgSource_t_MAX
};

// which gearbox the diagnostic read talks to (settings.dsgGearbox). The web UI fills the read fields in as a preset;
// the firmware only uses it to turn a "table" read (DSG_FMT_TABLE) into the gauge's values.
#define GEARBOX_DSG 0    // VW DSG (DQ200 / DQ250 / DQ381 ...)
#define GEARBOX_ZF_VAG 1 // ZF 8HP in an Audi / VW (AL551, AL550): UDS on 7E1 / 7E9, DIDs 2104 temperature, 210F gear
#define GEARBOX_ZF_BMW 2 // ZF 8HP in a BMW F / G-series: UDS 22 DAxx to the EGS (0x18), BMW extended addressing
#define GEARBOX_TYPES 3  // how many there are

// DsgReadSpec.fmt: how the bytes become a value
#define DSG_FMT_SCALED 0      // raw * scale + offset
#define DSG_FMT_HIGH_NIBBLE 1 // the byte's high nibble (OBD PID 0xA4 actual gear)
#define DSG_FMT_TABLE 2       // the raw number through the gearbox type's own table (BMW lock-up / selector codes)

// ===== VAG diag (src/can2gauge_VagDiag.cpp) =====
// Measuring values over the gauge's CAN (or the OBD adapter), as a dealer scan tool's measuring values screen reads
// them: UDS ReadDataByIdentifier (22 DID) on MQB-era modules, or KWP2000 measuring blocks (21 block) over TP2.0 on
// PQ-era modules. Each value from its own module (0 = the Diagnostics tab's module); only the values in use are asked.
#define VDIAG_VALUES 32   // values read at once
#define VDIAG_NAME_LEN 16 // a value's name ("Boost actual")
#define VDIAG_UNIT_LEN 8  // a value's unit ("mbar")

#define VDIAG_KIND_NONE 0 // empty slot
#define VDIAG_KIND_UDS 1  // UDS DID: 22 <id high> <id low>, the value is bits bitPos.. of the DID's data
#define VDIAG_KIND_KWP 2  // KWP2000 measuring block id (1..255), field 1..4 (formula byte decoded like KWP1281)
#define VDIAG_KIND_OBD 3  // OBD mode 01 PID id asked of this value's module (the gearbox's own PIDs ...)

#define VDIAG_PROTO_AUTO 0 // UDS first, then TP2.0 (as scan tools do)
#define VDIAG_PROTO_UDS 1  // UDS over ISO-TP only
#define VDIAG_PROTO_TP20 2 // KWP2000 over VW TP2.0 only

#define VDIAG_RUN_OFF 0    // nothing asked on the bus
#define VDIAG_RUN_SHOWN 1  // while a page in the rotation shows a VAG diag value, or the Diagnostics tab is open
#define VDIAG_RUN_ALWAYS 2 // whenever the ignition is on (for triggers and the chain)

#define VDIAG_LINK_CAN 0     // the requests go out on the gauge's own CAN
#define VDIAG_LINK_ADAPTER 1 // ... through the OBD adapter (Bluetooth / WiFi ELM327), no CAN wire needed

#define VDIAG_FLAG_SIGNED 0x01    // a UDS value's bits are a signed (two's complement) number
#define VDIAG_FLAG_LSB_FIRST 0x02 // a UDS value longer than a byte has its least significant byte first (Intel)

// one value read from an ECU: request [svc][id...] -> answer [svc + 0x40][id echo][data...]
struct DsgReadSpec
{
  float scale;    // raw * scale
  float offset;   // ... + offset
  uint16_t id;    // DID (svc 0x22) or PID (svc 0x01)
  uint16_t spare;
  uint8_t svc;    // 0 = off, 0x22 = UDS ReadDataByIdentifier (16-bit id), 0x01 = OBD mode 01 (8-bit PID)
  uint8_t pos;    // byte offset into the data after the echo
  uint8_t len;    // 1 or 2 bytes, big-endian
  uint8_t fmt;    // DSG_FMT_SCALED / DSG_FMT_HIGH_NIBBLE / DSG_FMT_TABLE
};

// one VAG diag value (Diagnostics tab)
struct VdiagValue
{
  uint32_t txId;             // UDS request id, 11-bit (0x7E0) or 29-bit (0x17FC0076); 0 = from addr (or the tab's)
  uint32_t rxId;             // UDS answer id, 0 = worked out from the request id
  float scale;               // UDS: value = raw x scale + offset (KWP: the formula byte scales it - leave 1)
  float offset;              // UDS: added after the scale
  uint16_t id;               // UDS: the DID (0x0000..0xFFFF); KWP: the block number 1..255
  uint16_t bitPos;           // UDS: first bit of the value in the DID's data: 0 = the top bit of the byte after the
                             //      DID echo, so byte n starts at bit n x 8 (as OBDb counts)
  uint8_t kind;              // VDIAG_KIND_*
  uint8_t field;             // KWP: field 1..4 of the block
  uint8_t bitLen;            // UDS: bits long, 1..32 (8 = one byte)
  uint8_t flags;             // UDS: VDIAG_FLAG_SIGNED / VDIAG_FLAG_LSB_FIRST
  uint8_t txExt;             // extended address byte in front of every request (BMW: the module), 0 = none
  uint8_t rxExt;             // extended address byte every answer starts with (BMW: F1), 0 = none
  uint8_t addr;              // VAG diagnostic address (0x01 engine, 0x02 gearbox, 0x22 AWD ...), 0 = the tab's module
  uint8_t protocol;          // VDIAG_PROTO_* for this value's module (AUTO = the tab's setting)
  char name[VDIAG_NAME_LEN]; // on the tab and as the gauge page's value name
  char unit[VDIAG_UNIT_LEN]; // UDS: the unit shown (KWP: "" = the formula's unit)
}; // 52 bytes

// where the oil page (STYLE_OIL) gets the cluster's oil temperature / level (the G266 sump sender: cluster coding 4th
// digit 1 = QG1 flexible service or 2 = QG2 fixed service with sensor)
enum oilSource_t : uint8_t
{
  OIL_SRC_CAN = 0, // listen: mKombi_2 0x420 byte 3 (x1 - 60 C) on the drivetrain CAN, VAG PQ profile. Temperature only
  OIL_SRC_KLINE,   // KWP1281 to the cluster (0x17) while the oil page shows: temperature + level OK / low
  oilSource_t_MAX
};

enum chainRole_t : uint8_t
{
  CHAIN_AUTO = 0, // master if this gauge has a source that asks (OBD adapter, OBD polling, gearbox reads) - lowest id wins
  CHAIN_MASTER,
  CHAIN_FOLLOWER,
  chainRole_t_MAX
};

#define ROW_MAX_POSITION 15 // settings.rowPosition 1..15 (it rides in 4 spare bits of the chain heartbeat)

// settings.canTermination
#define CAN_TERM_OFF 0  // never: the car's bus is terminated already (or a gauge mid-chain)
#define CAN_TERM_ON 1   // always: the last gauge on a chain with nothing else terminating it
#define CAN_TERM_AUTO 2 // at power-up: frames heard = terminated already, leave it off; nothing heard (bench, gauges on
                        // their own) = the first gauge to give up listening switches its 120R on

enum linkTransport_t : uint8_t
{
  LINK_AUTO = 0, // CAN when a link heartbeat was heard on CAN (and CAN TX is allowed), else ESP-NOW
  LINK_CAN,
  LINK_ESPNOW,
  LINK_BOTH,
  LINK_OFF,
  linkTransport_t_MAX
};

// ignition on permanent power (src/can2gauge_Power.cpp): what says the ignition is on. With a source set, ignition off
// plays the shutdown sprite, darkens the screen (standby) and later puts the gauge into deep sleep.
enum ignSource_t : uint8_t
{
  IGN_SRC_OFF = 0, // switched power (ignition feed): no ignition logic
  IGN_SRC_CAN,     // terminal 15 from the CAN bus (settings.ignCanMode)
  IGN_SRC_WIRE,    // the illumination input (J_OUT pin 3) wired to an ignition feed: no night dimming from it
  IGN_SRC_CHAIN,   // another gauge in the chain that has its own ignition (CAN or wire): sleeps and wakes with it
  ignSource_t_MAX
};

enum ignCanMode_t : uint8_t
{
  IGN_CAN_PQ = 0,  // PQ35 / 46: BSG_Last 0x570 ZAS_Klemme_15 (bit 1), or ZAS_1 0x572 Klemme_15 (bit 1)
  IGN_CAN_MQB,     // MQB: Klemmen_Status_01 0x3C0 ZAS_Kl_15 (bit 17)
  IGN_CAN_CUSTOM,  // settings.ignCanId bit settings.ignCanBit (DBC numbering, Intel), settings.ignCanInvert
  IGN_CAN_TRAFFIC, // any 11-bit frame on the bus (not the chain's): for buses without a terminal-15 frame (Mk4)
  ignCanMode_t_MAX
};

// where a gauge page's values come from (web UI > Gauges > Source)
enum pageSource_t : uint8_t
{
  PSRC_CAN = 0, // the gauge's CAN decoding (VAG PQ / MQB, OpenHaldex) or a signal of the DBC on the gauge
  PSRC_KLINE,   // a measuring-block field (K-line session to the module while the page shows)
  PSRC_OBD,     // an OBD-II PID (OBD adapter or the wired OBD profile)
  PSRC_GAUGE,   // the gauge itself: board temperature, illumination, output, ME7 logger, gearbox reads ...
  PSRC_ADC,     // an adc2can sensor (SIG_ADC_1..16)
  PSRC_ECU,     // an aftermarket ECU over WiFi through usb2ip: its everyday values and 8 picked channels
  PSRC_DIAG,    // one of the Diagnostics tab's values (UDS DID or TP2.0 block field)
  PSRC_CAR,     // "Car": a value the car scan found (an OBD PID, a diag value or a K-line block field), web UI only
  pageSource_t_MAX
};

enum userSigSrc_t : uint8_t
{
  USRC_NONE = 0,
  USRC_CAN_DBC, // canId + sigName decoded with the DBC on the gauge
  USRC_KLINE,   // klineAddr block klineGroup field klineField
  USRC_OBD,     // mode 01 pid
  USRC_ECU,     // a channel of an aftermarket ECU over WiFi from usb2ip: ecuKind + ecuChannel
  USRC_DIAG,    // Diagnostics tab value number diagValue (1..VDIAG_VALUES)
  USRC_ME7,     // an ME7 logger variable by its .ecu name ("nmot_w") in sigName
};

// one value of a gauge page from a chosen source (web UI > Gauges > Source)
struct UserSigCfg
{
  uint32_t canId;      // USRC_CAN_DBC: CAN message id (bit 31 = extended)
  uint16_t ecuChannel; // USRC_ECU: the ECU's own channel number (a library row's channel, not its row number)
  uint8_t source;      // userSigSrc_t
  uint8_t klineAddr;   // USRC_KLINE: module address
  uint8_t klineGroup;  // USRC_KLINE: measuring block 1..255
  uint8_t klineField;  // USRC_KLINE: 0..3
  uint8_t pid;         // USRC_OBD: mode 01 PID
  uint8_t diagValue;   // USRC_DIAG: the Diagnostics value number 1..VDIAG_VALUES
  uint8_t ecuKind;     // USRC_ECU: which ECU (ecunKind_t: 1 Ignitron, 2 MaxxECU)
  char sigName[40];    // USRC_CAN_DBC: the DBC signal's name; USRC_ME7: the ME7 variable's name
};

// one adc2can channel as the gauge treats it (web UI > ADC)
enum adcMode_t : uint8_t
{
  ADC_MODE_SENT = 0, // the value the adc2can sends (scaled there: linear / table / NTC)
  ADC_MODE_LINEAR,   // scaled here: value = connector volts x scale + offset
};

// ===== adc2can sensor boards ===== the one place the number of boards is set. The adc2can protocol leaves room for
// more (CAN id = base + board x 16 + n): adc2can takes board ids 0-3 today and may go to 8. To take more: raise
// ADC2CAN_BOARDS, add the matching adc17.. rows to the signal table and signalId_t (one block straight after
// SIG_ADC_16, so sensor n is always SIG_ADC_1 + n - the build stops if they don't match), and bump SETTINGS_VERSION.
#define ADC2CAN_BOARDS 2                 // adc2can boards this gauge takes (board ids 0 .. ADC2CAN_BOARDS - 1)
#define ADC_SENSORS (ADC2CAN_BOARDS * 8) // sensors: 8 per board

struct AdcChCfg
{
  float scale;      // ADC_MODE_LINEAR: units per volt
  float offset;     // ADC_MODE_LINEAR: units at 0 V
  uint8_t mode;     // adcMode_t
  uint8_t decimals; // 0..3, for the web UI
  char name[16];    // "" = the adc2can's name
  char unit[8];     // "" = the adc2can's unit (ADC_MODE_LINEAR: the result's unit)
};

// one gauge page (web UI > Gauges), everything about it in one place
struct GaugePageCfg
{
  float minVal;
  float maxVal;
  float redline;                // the arc turns red from here (<= minVal: never)
  uint8_t enabled;              // 1 = in the swipe rotation
  uint8_t style;                // gaugeStyle_t
  uint8_t source;               // pageSource_t: what the web UI's Source shows
  uint8_t signal;               // signalId_t of the main value (SIG_USER_1 + 2n = this page's valueSigs[0])
  uint8_t signal2;              // second value for STYLE_DUAL (SIG_USER_2 + 2n = valueSigs[1])
  uint8_t decimals;             // 0..2
  uint8_t spriteOn;             // 1 = the page shows its sprite (reacting to its reading) instead of its style
  uint8_t slots[3];             // Quad values 2-4 / the Custom centre stack (signalId_t, SIG_NONE = empty;
                                // SIG_USER_17 + 3n + k = this page's slotSigs[k])
  char label[NAME_LEN];
  char sprite[SPRITE_NAME_LEN]; // the page's sprite pack, /sprites/<name>.c2gs ("" = the first on the gauge)
  UserSigCfg valueSigs[2];      // the main and second value when they come from a chosen source
  UserSigCfg slotSigs[3];       // the three slots when they come from a chosen source
};

// one trigger (web UI > Triggers)
struct TriggerCfg
{
  float threshold;
  float hysteresis;  // re-arm band
  uint16_t holdMs;   // the condition must last this long before it fires
  uint8_t enabled;
  uint8_t signal;    // signalId_t
  uint8_t above;     // 1 = fires at value >= threshold, 0 = at value <= threshold
  uint8_t action;    // triggerAction_t
  uint8_t output;    // IOX channel for ACT_OUTPUT (0 = OUT)
  uint8_t latch;     // 1 = stays on until acknowledged (tap the screen)
  uint8_t noReading; // the value has no reading (sensor unplugged, bus quiet): 0 = ignore it (the trigger waits),
                     // 1 = count it as the condition met
  char label[NAME_LEN];
};

// ===== Settings =====
// Every setting of the gauge, saved in NVS as one blob ("cfg") and sent to / from the web UI as JSON
// (src/can2gauge_EEP.cpp). The sections follow the web UI and the JSON groups; in each, the fields go by type.
// A change to this layout bumps SETTINGS_VERSION.
struct Settings
{
  uint16_t magic;   // SETTINGS_MAGIC
  uint16_t version; // SETTINGS_VERSION

  // ===== Display ===== (JSON "display")
  uint32_t themeDial;      // the Custom theme's dial colour 0xRRGGBB (also Match Car's until a VIN is read)
  uint32_t themeNeedle;    // the Custom theme's needle colour
  uint16_t swipeAnimMs;    // page slide time
  uint16_t longPressMs;    // press this long to open the menu (2000; a short tap is the sprite's tickle)
  uint8_t brightness;      // 0..100 by day
  uint8_t nightBrightness; // 0..100 with the dash lights on (night dimming)
  uint8_t theme;           // theme id (include/can2gauge_themes.h): 0-3 can2gauge, 4 Custom, 5 Match Car, 16+ makes
  uint8_t gaugeFeel;       // 0 = analogue (needles and numbers glide), 1 = digital (each reading at once)
  uint8_t startPage;       // the page shown first after the boot logo
  uint8_t upsideDown;      // 1 = the picture turned 180 degrees (a gauge fitted upside down)
  uint8_t renderMode;      // 0 = partial (strips in internal RAM, copied to the panel), 1 = direct (the panel's frame
                           // buffers, swapped on vsync: no tearing); read at boot (lvglInit)
  uint8_t simdBlend;       // 1 = LVGL's fills / RGB565 copies in ESP32-S3 SIMD assembly (src/simd), 0 = plain C
  int8_t rotateTrim;       // extra turn in half degrees, + = clockwise as you look at it (-30..30 = -15..+15 degrees).
                           // Kept even with SCREEN_ROTATION 0, so the layout never depends on that switch

  // ===== Units ===== (JSON "units", src/can2gauge_Units.cpp)
  uint8_t units;        // the quick choice: UNITS_METRIC / UNITS_IMPERIAL set every row below, UNITS_CUSTOM = one by one
  uint8_t unitSpeed;    // UNIT_METRIC km/h, UNIT_IMPERIAL mph
  uint8_t unitBoost;    // boost and manifold pressure: UNIT_METRIC bar (mbar absolute), UNIT_IMPERIAL psi, UNIT_KPA kPa
  uint8_t unitPressure; // oil, fuel and other pressures: UNIT_METRIC bar, UNIT_IMPERIAL psi, UNIT_KPA kPa
  uint8_t unitTemp;     // UNIT_METRIC C, UNIT_IMPERIAL F
  uint8_t unitMixture;  // UNIT_METRIC lambda, UNIT_IMPERIAL AFR (petrol, 14.7)
  uint8_t unitDistance; // UNIT_METRIC km, UNIT_IMPERIAL miles

  // ===== Boot logo ===== (JSON "boot")
  uint16_t bootFadeMs; // boot logo -> first page fade (ms)
  uint8_t bootLogo;    // 0 none, 1 Forbes Automotive (/logos/forbes.bin, else text), 2 OpenHaldex, 3 custom text,
                       // 4 /logo.bin, 5 make logo /logos/<key>.bin, 6 the can2gauge logo, 7 My Car's picture
  char bootText[24];   // bootLogo 3: the text shown
  char logoMake[12];   // bootLogo 5: logo key ("vw", "vauxhall" ...), "" = follow the theme / the VIN's make

  // ===== Screensaver ===== (JSON "saver")
  uint16_t saverDelayS;              // after the car has been still and the screen untouched this long (30)
  uint8_t saverEnabled;              // 1 = on (off by default)
  uint8_t saverReact;                // 1 = its sprite reacts to the car (the pack's saver moods), 0 = its loop only
  char saverSprite[SPRITE_NAME_LEN]; // its pack ("" = the first on the gauge)

  // ===== Power and ignition ===== (JSON "power", src/can2gauge_Power.cpp)
  uint32_t ignCanId;                    // IGN_CAN_CUSTOM: frame id (bit 31 = extended)
  uint8_t ignSource;                    // ignSource_t: what says the ignition is on (IGN_SRC_OFF = switched power)
  uint8_t ignCanMode;                   // ignCanMode_t
  uint8_t ignCanBit;                    // IGN_CAN_CUSTOM: 0..63, byte * 8 + bit (the DBC start bit of a 1-bit Intel signal)
  uint8_t ignCanInvert;                 // IGN_CAN_CUSTOM: 1 = bit clear means ignition on
  uint8_t ignOffDelayS;                 // ignition off this long before the shutdown starts (2)
  uint8_t standbyMin;                   // dark standby (instant wake) this long before deep sleep, 0 = deep sleep at once (10)
  char shutdownSprite[SPRITE_NAME_LEN]; // ignition off / wake pack ("" = a plain fade)

  // ===== Output and dash lights ===== (JSON "io")
  uint8_t outputMode;                 // 0 = high-side (OUT switches to 12 V, load to GND), 1 = low-side (OUT to GND)
  uint8_t nightSource;                // 0 = off, 1 = the dash-light input (J_OUT pin 3) dims the screen
  uint8_t dimInvert;                  // 1 = input LOW means lights on
  uint8_t outputManual[IOX_CHANNELS]; // web UI manual override (0 off, 1 on, 2 auto / trigger)

  // ===== Gauge pages and triggers ===== (JSON "pages", "triggers")
  GaugePageCfg pages[MAX_PAGES];
  TriggerCfg triggers[MAX_TRIGGERS];

  // ===== Menu and web UI tabs ===== (JSON "menu")
  uint32_t menuMask; // bit per uiMenuItem_t: 1 = on the screen's menu, and its web UI tab shown

  // ===== CAN bus ===== (JSON "can")
  uint16_t canKbps;        // 250 / 500 / 1000
  uint16_t maxxBaseId;     // CAN_PROFILE_MAXXECU: the first frame's id (0x520; MTune's "CAN ID OFFSET" moves it)
  uint16_t dbcSlotBase[3]; // per DBC slot (car, gearbox, extra): where a relocatable ECU's first frame really is
                           // (EMU Black / MaxxECU base id moved in the ECU's software), 0 = as written in the DBC
  uint8_t canEnabled;
  uint8_t canProfile;      // canProfile_t
  uint8_t canTermination;  // CAN_TERM_OFF / ON (Q_TERM driven, 120R in circuit) / AUTO (on only if the bus is silent)
  uint8_t useCanRelay;     // 1 = use the CAN frames an OpenHaldex / adc2can relays over WiFi (src/can2gauge_CanRelay.cpp)

  // ===== OBD-II on the gauge's CAN ===== (JSON "obd")
  uint8_t obdOverCan; // 1 = ask the engine for OBD-II PIDs on 0x7DF next to the CAN profile (VAG PQ / MQB / MaxxECU)
  uint8_t obdEcu;     // the ECU it reads: 0 = the engine (or the first to answer), 1..8 = answer id 0x7E8..0x7EF

  // ===== K-line ===== (JSON "kline")
  uint16_t klineBaud;    // 10400 default; ME7Logger's LogSpeed overrides
  uint8_t klineProtocol; // klineProtocol_t
  uint8_t klineEcuAddr;  // 0x01 engine (KWP1281); KWP2000 / OBD-II tries 0x33 too (0 = 0x33 straight away)
  uint8_t klineModeAddr; // menu > K-line: the module whose measuring blocks show (0x01 engine, 0x17 cluster ...), kept
                         // apart from klineEcuAddr so browsing the ABS doesn't move the engine logging
  uint8_t klineFastObd;  // 1 = Fast OBD on the gauge's own K-line (KWP2000): shorter P3 / P4 gaps

  // ===== ME7 logger ===== (JSON "me7")
  uint8_t me7SamplesPerSecond; // samples a second
  uint8_t me7LogToFile;        // 1 = write CSV to the gauge while logging
  uint8_t me7FastMode;         // 1 = Fast (B7 RAM routine), 0 = Normal logging. Test link is never saved
  char me7EcuFile[FILE_LEN];   // the ME7Logger .ecu file on the gauge
  char me7CfgFile[FILE_LEN];   // the ME7Logger .cfg file (which variables to log)

  // ===== OBD adapter (ELM327, Bluetooth or WiFi) ===== (JSON "elm", docs/can2gauge_BLE_ELM327_brief.md)
  uint16_t elmWifiPort;   // a WiFi adapter's TCP port (35000 on most)
  uint8_t elmEnabled;
  uint8_t elmTransport;   // elmTransport_t: Bluetooth LE or WiFi
  uint8_t elmPollObd;     // poll standard mode 01 PIDs into the gauges
  uint8_t elmFastObd;     // 1 = Fast OBD on the adapter: the "one answer" request digit + ATAT2
  uint8_t elmExtraSource; // ELM_EXTRA_* (the web UI's "Adapter Source"): what else the adapter does
  uint8_t elmKlAddr;      // ELM_EXTRA_KLINE: K-line module address (0x01 engine)
  uint8_t elmKlFast;      // ELM_EXTRA_KLINE: 1 = fast init (ISO 14230), 0 = 5-baud init to elmKlAddr
  char elmWifiSsid[36];   // a WiFi adapter's network ("" = join the first OBD-looking open network found, then saved)
  char elmWifiPass[68];   // "" = open (most are)
  char elmWifiHost[16];   // its IP, "192.168.0.10" on most

  // ===== Gearbox (DSG / ZF 8HP) ===== (JSON "dsg", menu > DSG, STYLE_GEAR pages)
  float dsgTp20SpeedDiv;     // 0 = use the VAG formula byte (02E block 1 field 4 is formula 0x07 = km/h); > 0 = (a*256 + b) / div
  uint16_t dsgTxId;          // gearbox request id (UDS / adapter header), 0x7E1
  uint16_t dsgRxId;          // gearbox answer id, 0x7E9
  uint16_t dsgPollMs;        // gap between gearbox reads
  uint8_t dsgGearbox;        // GEARBOX_DSG / GEARBOX_ZF_VAG / GEARBOX_ZF_BMW
  uint8_t dsgSource;         // dsgSource_t
  uint8_t dsgUdsSession;     // 0x01 = stay in the default session, 0x03 = extended session + TesterPresent
  uint8_t dsgSpeedPref;      // 0 = gearbox speed when valid, else vehicle speed; 1 = gearbox only; 2 = vehicle only
  uint8_t dsgTxExtAddr;      // extended addressing: the first byte of every frame sent (BMW EGS 0x18), 0 = normal
  uint8_t dsgRxExtAddr;      // ... and of every answer (BMW: 0xF1, the tester)
  uint8_t dsgTp20Module;     // TP2.0 logical address of the gearbox (0x02)
  uint8_t dsgTp20Group;      // measuring block with the speed
  uint8_t dsgTp20SpeedField; // 0..3 in that block, 0xFF = none
  uint8_t dsgTp20GearGroup;  // measuring block with the gear, 0 = none
  uint8_t dsgTp20GearField;  // 0..3
  DsgReadSpec dsgSpeed;      // UDS / adapter: how to read the gearbox speed
  DsgReadSpec dsgGear;       // UDS / adapter: optional gear read (svc 0 = off: the gear comes from the CAN broadcast)
  DsgReadSpec dsgTemp;       // gearbox oil temperature -> SIG_GEARBOX_TEMP (every 10th pass: it changes slowly)
  DsgReadSpec dsgLockup;     // torque converter lock-up -> SIG_LOCKUP
  DsgReadSpec dsgSelector;   // selector position -> SIG_SELECTOR (when the CAN broadcast doesn't give it)

  // ===== VAG diag ===== (JSON "vdiag", src/can2gauge_VagDiag.cpp): measuring values over the gauge's CAN, UDS or
  // KWP2000 over TP2.0, with a Turbo mode and grouped requests
  uint32_t vdiagTxId;      // UDS request id, 11-bit (0x7E0) or 29-bit (0x17FC0076, 0x18DA01F1); 0 = the address table
  uint32_t vdiagRxId;      // UDS answer id, 0 = worked out from the request (0x7xx + 0x6A, 18DA xx F1 -> 18DA F1 xx ...)
  uint16_t vdiagPollMs;    // normal mode: gap after each request (Turbo ignores it)
  uint8_t vdiagRun;        // VDIAG_RUN_OFF / SHOWN / ALWAYS
  uint8_t vdiagAddr;       // the module's VAG diagnostic address (0x01 engine, 0x02 gearbox, 0x22 AWD ...)
  uint8_t vdiagProtocol;   // VDIAG_PROTO_AUTO / UDS / TP20
  uint8_t vdiagTurbo;      // 1 = Turbo: requests back to back, UDS DIDs grouped
  uint8_t vdiagGroupSize;  // Turbo: most DIDs in one UDS request, 1..8
  uint8_t vdiagSession;    // UDS session opened first: 0x01 default (none), 0x03 extended
  uint8_t vdiagTp20Module; // TP2.0 module id for the 0x200 setup, 0 = from the address table (0x22 -> 0x0A ...)
  uint8_t vdiagLink;       // VDIAG_LINK_CAN (the gauge's CAN) / VDIAG_LINK_ADAPTER (the OBD adapter)
  uint8_t vdiagTxExt;      // extended address byte in front of every request (BMW: the module's address), 0 = none
  uint8_t vdiagRxExt;      // extended address byte every answer starts with (BMW: F1, the tester), 0 = none
  VdiagValue vdiagValues[VDIAG_VALUES];

  // ===== Cluster oil temperature / level ===== (JSON "oil", STYLE_OIL pages, docs/FIRMWARE.md "OEM oil temp / level")
  uint16_t oilPollMs;     // gap between measuring-block reads
  uint8_t oilSource;      // oilSource_t
  uint8_t oilClusterAddr; // KWP1281 address of the cluster, 0x17
  uint8_t oilGroup;       // measuring block, 0 = by the cluster's part number (1J0 / 3B0-920: 3, others: 50)
  uint8_t oilTempField;   // 0..3 in that block (used when oilGroup != 0)
  uint8_t oilLevelField;  // 0..3, 0xFF = none (used when oilGroup != 0)
  uint8_t oilLevelOkZero; // 1 = raw 0 means level OK; 0 (default) = non-zero means OK

  // ===== adc2can sensor boards ===== (JSON "adc", src/can2gauge_ADC2CAN.cpp)
  uint32_t adcBaseId;        // 29-bit: the adc2can "base id" (0x0AD2C000); board b sends base + b*16 + 0..7
  uint8_t adcWireless;       // 1 = take adc2can boards over ESP-NOW too (adc2can 1.3+, include/adcnow_proto.h); CAN wins
  AdcChCfg adc[ADC_SENSORS]; // board 0 ch 1-8, board 1 ch 1-8 ... (ADC_SENSORS = ADC2CAN_BOARDS x 8)

  // ===== OpenHaldex ===== (JSON "haldex", src/can2gauge_OHX.cpp, include/ohx_proto.h)
  uint16_t haldexBroadcastId; // its status frame on CAN, 0x6B0
  uint16_t haldexControlId;   // its control frame on CAN, 0x6A0
  uint8_t ohxWireless;        // 1 = listen for OpenHaldex over ESP-NOW and send it commands (default on)
  uint8_t ohxCarSignals;      // 1 = take speed / rpm / boost / pedal from it while nothing else on the gauge gives them
  char ohxKey[68];            // its WiFi AP password ("" = open AP) - signs the commands

  // ===== usb2ip (an aftermarket ECU over WiFi) ===== (JSON "ecu", src/can2gauge_ECU.cpp, include/ecunow_proto.h)
  uint8_t ecuWireless;   // 1 = listen for usb2ip over ESP-NOW (default on: costs nothing)
  uint8_t ecuCarSignals; // 1 = its rpm / boost / coolant ... fill the gauge's own values while nothing else gives them

  // ===== Nav link ===== (JSON "nav", src/can2gauge_Nav.cpp)
  uint16_t navPopupM; // near-turn pop-up distance in metres (NAV_POPUP_STEP_M steps)
  uint8_t navMode;    // NAV_MODE_OFF / NEAR_TURN / WHOLE_ROUTE

  // ===== Data logger ===== (JSON "log", src/can2gauge_Logger.cpp). The recording itself lives in PSRAM only.
  uint8_t logRateHz;                // samples a second: 1, 2, 5, 10, 20 or 50
  uint8_t logAutoStart;             // 1 = start recording by itself at power-up / ignition on
  uint8_t logManualChannels;        // 0 = log the values the gauge pages show, in page order; 1 = the channels below
  uint8_t logSignals[LOG_CHANNELS]; // signalId_t of each channel, SIG_NONE = not used

  // ===== Daisy chain ===== (JSON "chain", docs/can2gauge_GaugeChain_design.md)
  uint16_t chainGroup;    // only frames from the same group are taken
  uint16_t linkCanBaseId; // DATA 0x6C0 + node, HEARTBEAT + 0x10 + node, COMMAND + 0x20
  uint8_t chainEnabled;
  uint8_t chainRole;      // chainRole_t
  uint8_t nodeId;         // 0..15, 0xFF = auto (low 4 bits of the MAC, moved on a clash)
  uint8_t linkTransport;  // linkTransport_t
  uint8_t linkCanTx;      // allow link frames on CAN (off = ESP-NOW only, for a car bus you'd rather not add to)
  uint8_t followPage;     // followers follow the master's page changes
  uint8_t followNight;    // followers use the master's dash-light state
  uint8_t failover;       // an Auto follower takes over when the master's heartbeat stops (3 s)
  uint8_t rowPosition;    // the row wave: where this gauge sits, counted from the left as you look at the dash
                          // (1 = leftmost), 0 = not in a row (the normal goodbye). Sent in the chain heartbeat.
  char nodeName[NAME_LEN];

  // ===== WiFi ===== (JSON "wifi")
  uint8_t wifiMode; // 0 AP always, 1 AP + STA bridge, 2 AP off after 5 min idle (low power)
  char apSsid[WIFI_LEN];
  char apPassword[WIFI_LEN];
  char staSsid[WIFI_LEN];
  char staPassword[WIFI_LEN];
  char hostname[NAME_LEN];

  // ===== Learnt by the gauge ===== (JSON "learnt"): worked out or paired by the gauge itself, not set by hand
  uint32_t runnerBest;  // Rev Runner's high score (docs/mascot/RUNNER.md), kept through a power cycle
  uint8_t themeMatched; // THEME_MATCH: theme id picked from the last VIN, THEME_NONE until one is read
  uint8_t ecuKind;      // ecunKind_t the followed usb2ip last said it reads (the web UI lists that ECU's channels)
  uint8_t elmAddrType;  // the paired OBD adapter's BLE address type
  uint8_t elmProtocol;  // its last detected ATDPN protocol (0 = auto search on connect)
  uint8_t ohxMac[6];    // the OpenHaldex this gauge follows (all zero = the first one heard, then saved)
  uint8_t ecuMac[6];    // the usb2ip this gauge follows (all zero = the first one heard, then saved)
  char matchedLogo[12]; // logo key of the make from the last VIN ("" = none)
  char elmMac[18];      // the paired OBD adapter "aa:bb:cc:dd:ee:ff" ("" = bind the first adapter found)
};

extern Settings settings;
extern SemaphoreHandle_t settingsMutex;
extern bool settingsDirty; // set by anything that changes settings; the house task saves them

// ===== Live status ===== (the web UI's status and the screen's menu read it)
// the cluster's oil: written by the K-line task (cluster session) and the CAN decoding (Kombi_2), read by the oil
// page and the web UI
struct OilStatus
{
  // K-line cluster session
  uint32_t reads;
  uint32_t errors;
  uint32_t connects;  // cluster logins since boot
  float value[4];
  uint8_t group;      // measuring block being read
  uint8_t tempField;  // 0..3
  uint8_t levelField; // 0..3, 0xFF = none
  uint8_t fieldCount; // fields in the last block
  uint8_t formula[4]; // the last block's raw fields (the web UI shows them for working out a new cluster)
  uint8_t a[4];
  uint8_t b[4];
  bool connecting;    // 5-baud init / identification under way
  bool connected;     // KWP1281 session to the cluster open
  char state[40];     // shown on the oil page under the value
  char clusterId[48]; // identification ("1J0920926C  KOMBI+WEGFAHRSP VDO V05")
  char unit[4][8];
  // CAN mKombi_2 0x420
  uint32_t kombi2Ms;    // millis() of the last frame, 0 = never
  uint32_t kombi2Count;
  uint8_t kombi2OilRaw; // byte 3 (KO2 Oeltemperatur, x1 - 60)
  bool kombi2OilErr;    // byte 0 bit 1 Fehlerstatus_Oeltemperatur
};

// menu > K-line: a KWP1281 session to settings.klineModeAddr while the K-line screen shows, one measuring block at a
// time; a swipe steps the block (src/can2gauge_KLine.cpp runKlineBrowse)
struct KlineBrowse
{
  uint32_t reads;
  uint32_t errors;
  uint32_t blockMs;            // millis() of the last good read
  volatile int16_t gotoGroup;  // web UI jump, -1 = none
  float value[4];
  uint8_t addr;                // module address being read
  uint8_t group;               // block showing / being read
  uint8_t fieldCount;          // fields in the block (0..4)
  uint8_t formula[4];
  uint8_t a[4];
  uint8_t b[4];
  uint8_t labelCandidateCount;
  uint8_t obdPageCount;        // isObd: pages of the PIDs the engine has, four a page
  volatile int8_t step;        // swipe: +1 / -1 (the K-line task takes it)
  bool connecting;             // 5-baud init / identification
  bool connected;
  bool searching;              // the last block was empty: moving on in the swipe direction
  bool isObd;                  // the engine didn't answer at 0x01: OBD-II at 0x33, group = the page of PIDs showing
  volatile bool labelReload;   // the web UI picked another label file
  char state[48];              // shown on the screen under the block
  char ecuId[48];              // identification ("1J0920926C  KOMBI+WEGFAHRSP VDO V05")
  char text[64];               // a text block (ME7 81 = VIN) instead of fields, "" = none
  char unit[4][8];
  // measuring-block labels (src/can2gauge_Labels.cpp): the file in use, the others that fit, the block's names
  char labelFile[40];          // "" = none found
  char labelCandidates[6][40]; // best first (the web UI's pick first)
  char title[40];              // the block's title ("Basic Functions (Mass Air Flow)")
  char fieldName[4][40];       // "Coolant Temperature", "" = not labelled
  char fieldSpec[4][80];       // "Specification: 85.0...110.0 °C"
};

// a gauge page whose source is a K-line measuring block: one session to the module, its blocks read back to back
struct KlinePage
{
  uint32_t reads;
  uint32_t errors;
  uint8_t addr;
  bool connecting;
  bool connected;
  char state[48]; // shown under the value while it has no data: "Waking module 0x01 (5-baud init)"
};

struct RuntimeStatus
{
  uint32_t canRxCount;
  uint32_t canTxCount;
  uint32_t canErrCount;
  uint32_t canRxPerSec;
  uint32_t klineSamples;
  uint32_t me7SampleRate;               // measured samples / s
  volatile uint32_t alertAcks;          // counts up each time the driver taps an alert away (the triggers then stay quiet)
  volatile uint32_t oilPageSeenMs;      // last refresh of an oil page on the screen (GaugeUI); the K-line task follows it
  volatile uint32_t klinePageSeenMs;    // a page with a K-line source is on the screen (GaugeUI refreshPage)
  volatile uint32_t pageSeenMs;         // a gauge page is on the screen: the OBD polling puts its PIDs first
  // mode screens on the screen (or the web UI's live view) stamp these; the task that owns each connects while fresh
  volatile uint32_t klineModeSeenMs;    // menu > K-line (or web UI > K-line > Measuring blocks open)
  volatile uint32_t elmModeSeenMs;      // menu > Bluetooth OBD
  volatile uint32_t vdiagSeenMs;        // web UI > Diagnostics open (polling /api/vdiag): the VAG diag task connects
  // OpenHaldex: when its state last came over CAN (0x6B0) / over WiFi (ESP-NOW). WiFi carries the engagement % as
  // OpenHaldex shows it, so the CAN decoding steps aside while WiFi is fresh; mode requests go over CAN when it's heard
  volatile uint32_t haldexCanMs;
  volatile uint32_t ohxRxMs;
  uint8_t activePage;
  volatile uint8_t klinePageCfgIndex;   // ... which page (index in settings.pages)
  volatile uint8_t pageCfgIndex;        // ... which page (index in settings.pages)
  bool canUp;
  bool haldexLink;
  bool klineUp;
  bool me7Logging;
  bool wifiStaConnected;
  bool alertActive;
  bool wokeFromSleep;                   // this boot is a deep-sleep wake with the ignition on: the wake clip plays
  // ignition on permanent power (src/can2gauge_Power.cpp). In standby the screen is dark, CAN is listen-only and the
  // tasks that talk to the car stand down, so the gauge never keeps the car's buses awake.
  volatile bool standby;
  volatile bool otherScanners;          // Enable Other Scanners: nothing asked of the car (UDS / TP2.0 / K-line / OBD), so
                                        // another scan tool can talk to it. Not saved: off again after a restart.
  char klineState[24];
  char klineEcuId[48];
  char me7LogFile[FILE_LEN];
  char wifiStaIP[16];
  char alertText[32];
  OilStatus oil;
  KlineBrowse klineBrowse;
  KlinePage klinePage; // the K-line session behind a gauge page (source K-line)
};
extern RuntimeStatus runtime;
