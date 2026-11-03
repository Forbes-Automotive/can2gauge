#pragma once
#include <can2gauge_defs.h>
#include <can2gauge_BLE.h>
#include <can2gauge_VagDiag.h> // 0.34.0: DiagTarget (UDS requests for the Diagnostics and Service tabs through the adapter)

// ---------------------------------------------------------------------------
// ElmSession + VehicleIdentifier + OBD scheduler over the BLE link (docs/can2gauge_BLE_ELM327_brief.md, build steps
// 1-2 and part of 4). Values land on the same signal ids as CAN / K-line, so gauges don't care where they come from.
//   connect  -> ATZ, ATE0 ATL0 ATS0 ATH1 ATAT1, ATI / STI (STN detect), ATRV
//   protocol -> ATSP<cached> or ATSP0 + 0100 (long first search), ATDPN cached in settings.elmProtocol
//   identify -> 0902 (VIN), else UDS 22 F190 on 7E0; WMI -> make, VW-group model code (VIN chars 7-8), year hint
//   poll     -> supported-PID bitmaps (0100/0120/0140) x the PIDs the gauge can decode; on CAN up to 6 PIDs per
//               request (5 fast PIDs, a rotating window when more are supported, + one slow PID in rotation), physical header 7E0; one PID at a time on
//               K-line / J1850
//   DSG      -> when settings.dsgSource == DSG_SRC_ELM: ATSH<dsgTxId> (29-bit car: 18 DA <gearbox> F1, 0.25.3) + the gearbox read every third cycle
//   terminal -> raw AT / OBD commands from the web UI, answers in a 40-line log
//   0.8.0: raw CAN mode (ElmRawLink, ATCAF0) so the TP2.0 client runs through the adapter: DSG_SRC_ELM_TP20;
//          VAG KWP2000 on the adapter's K-line (settings.elmExtraSource: engine blocks -> the K-line signals);
//          WiFi adapters as well as BLE (settings.elmTransport, can2gauge_ElmWiFi.h);
//          bench tests (probe / raw / dtc / tp20 / kline / all) that log every exchange to /logs/elm_<kind>_NNN.txt
// Only the chain master holds the adapter (adapters accept one central).
// ---------------------------------------------------------------------------

struct ElmStatus
{
  char state[24];
  char adapter[48];   // ATI (+ STI)
  bool stn;
  float volts;        // ATRV
  uint8_t protocol;   // ATDPN digit (0 = none yet)
  char vin[18];
  char make[24];
  char model[32];     // VW-group model name from the VIN model code, if known
  char modelCode[3];
  char year[12];
  char makeKey[12];   // logo / theme key of the make ("vw", "vauxhall" ...), "" if the WMI is unknown
  uint16_t yearNum;   // model year from VIN position 10 (0 = not coded), cycle picked by model code / protocol
  uint32_t supported[3]; // PID bitmaps: [0] 01-20, [1] 21-40, [2] 41-60 (bit 31 = lowest PID)
  float reqPerSec;
  uint32_t requests, noData, errors;
  char lastError[48];
};

void setupELM();
void elmRestart();                            // settings changed (enable / adapter): reconnect
bool elmConnected();
const char *elmLinkInfo();                    // 0.8.0: the BLE or WiFi link description
void elmRequestScan();                        // web UI "Scan" (results in elmFound)
uint8_t elmFound(BleFound *out, uint8_t max); // last scan results
bool elmScanning();
void elmPair(const char *mac, uint8_t type);  // bind this adapter (saved) and connect
void elmForget();                             // clear the saved adapter
void elmReidentify();                         // re-read VIN / protocol
bool elmQueueCommand(const char *cmd);        // raw terminal (false = queue full)
void elmTerminalText(char *buf, size_t len);  // the terminal log, oldest first
const ElmStatus &elmStatus();
const char *elmProtocolName(uint8_t protocol);
void elmInfoProvider(char *buf, size_t len);  // menu > Bluetooth OBD

// bench tests (0.8.0) - read only, except "clear" (0.14.0) which clears the fault codes; the adapter is reset and set
// up again afterwards
#define ELM_TEST_RESULT_LEN 640
struct ElmTestStatus
{
  bool running;
  uint16_t runs;                   // finished tests since boot (the UI / script notice a new result)
  char kind[8];                    // "all" "probe" "raw" "dtc" "tp20" "kline" "clear"
  char step[48];                   // what it is doing now, "done" at the end
  char file[40];                   // /logs/elm_<kind>_NNN.txt
  char result[ELM_TEST_RESULT_LEN]; // the RESULT lines joined with " | "
};
// kind as above; module / group for dtc / clear (TP2.0 part) and tp20 (0 = engine 0x01 / block 1). false = not connected,
// a test already queued / running, or an unknown kind
bool elmStartTest(const char *kind, uint8_t module, uint8_t group);
const ElmTestStatus &elmTestStatus();

// 0.34.0: one UDS request through the adapter for another task (the Diagnostics tab, the Service tab), to any module: 11-bit
// or 29-bit ids (the adapter switches between CAN 11-bit and 29-bit for it and back), BMW extended addressing too.
// The ELM task sends it between its own OBD requests; this waits for the answer. The request must fit one CAN frame
// (7 bytes, 6 with extended addressing): an ELM327 can't send longer ones. answer = the payload (positive or 7F).
// false = no answer, adapter not connected, or not a CAN car.
bool elmDiagTransact(const DiagTarget &target, const uint8_t *request, uint8_t requestLen, uint8_t *answer,
                     uint16_t answerMax, uint16_t &answerLen, uint32_t waitMs);
bool elmCar29Bit();            // the car answers OBD on 29-bit CAN (protocol 7 / 9)
uint8_t elmEngineAddress29();  // ... the engine's address there (Tiguan: 0x01, from 18 DA F1 01)
uint8_t elmGearboxAddress29(); // ... and the gearbox's (0x02)
