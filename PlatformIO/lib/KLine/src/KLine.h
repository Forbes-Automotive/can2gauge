#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// KLine - ISO 9141 / K-line transport on the L9637D + two basic diagnostic stacks:
//   KWP1281 (VAG block protocol, 5-baud init, "measuring blocks")  - Mk4 Golf era ME7 / Marelli / Simos / ...
//   KWP2000 / ISO 14230-4 (fast init, OBD-II mode 01 PIDs)          - anything OBD-II over K-line
// The ME7Logger stack lives in its own library and uses this transport.
//
// The L9637D echoes every byte we transmit back into RX. klWrite() swallows the echo so callers only ever see
// what the ECU sent. All functions block; run them from the K-line task, never from the UI.
// ---------------------------------------------------------------------------

// 0.17.4 bench switches: print on the USB serial why a KWP1281 block failed (which byte, what came back), and how long
// each measuring-block read took from request to answer
#define KL_DEBUG_ERRORS 1 // 1 = print each K-line failure and where it happened ** CAN CHANGE THIS **
#define KL_DEBUG_TIMING 0 // 1 = print the time of every measuring-block read (lots of lines) ** CAN CHANGE THIS **

// ---- transport
bool klBegin(int8_t txPin, int8_t rxPin, uint32_t baud = 10400);
void klEnd();
void klSetBaud(uint32_t baud);
uint32_t klBaud();
// 5-baud address byte on TX, bit-banged ~2 s, UART re-opened. addParity: 7 bits + odd parity, as scan tools wake KWP1281
// modules (0x17 -> 0x97); false = the plain byte, for KWP2000 / OBD (0x11, 0x33) - 0.17.4
void klSlowInit(uint8_t address, bool addParity);
void klFastInit();                // ISO 14230 fast init: 25 ms low, 25 ms high
// 0.17.3: after klSlowInit(), waits for the ECU's 0x55 sync byte, skipping a few stray bytes first; true = it came
bool klWaitSync();
#define KL_SYNC_TIMEOUT_MS 1500 // 0.17.3: our 200 ms stop bit + the ECU's up-to-300 ms W1 delay, with room to spare
#define KL_SYNC_STRAY_MAX 16    // 0.17.3: stray bytes skipped before the 0x55
size_t klWrite(const uint8_t *data, size_t len, uint16_t interByteMs = 0); // returns bytes whose echo was seen
int klRead(uint32_t timeoutMs);   // -1 on timeout
void klFlush();
uint32_t klLastRxMs();

// ---- KWP1281
// payload bytes of the longest block (the length byte can say up to 255, which counts 3 more). 0.17.4: was 32 - the
// bench ME7.5's blocks 80 / 81 (text) are 39 / 44 long, and one too long dropped the session, so K-line mode
// reconnected over and over on them
#define KWP1281_MAX_BLOCK 252
struct Kwp1281Field
{
  uint8_t formula;
  uint8_t a;
  uint8_t b;
  float value;
  char unit[8];
};
struct Kwp1281Group
{
  uint8_t group;
  uint8_t count; // fields decoded (0..4)
  Kwp1281Field fields[4];
  // 0.17.4: a text block instead of fields (count 0): the first field's formula is 63 (0x3F) and the block is longer
  // than 4 fields - the rest is ASCII, as scan tools show it (ME7: block 81 = VIN, 82 = immobiliser id). "" = not a text block
  char text[64];
};

// one stored fault (0.16.0): the VAG fault number (scan tools show it as 5 digits, e.g. 16684) and its status byte
struct Kwp1281Fault
{
  uint16_t code;   // fault number
  uint8_t status;  // status / elaboration byte, as sent
};

bool kwp1281Connect(uint8_t address, char *ecuIdOut, size_t idLen); // 5-baud init + identification blocks
// false only when the session was lost (it is then closed). A group the module doesn't have (NAK) comes back true
// with count 0 (0.17.2).
bool kwp1281ReadGroup(uint8_t group, Kwp1281Group &out);
// 0.19.0: the same measuring group over a KWP2000 session (service 0x21), decoded identically. Used by K-line mode
// when the KWP2000 protocol is chosen (a module reached via the double slow-init). false = the request was lost.
bool kwp2000ReadGroup(uint8_t group, Kwp1281Group &out);
// 0.16.0 service functions. Each returns false if the session was lost (it is then closed).
bool kwp1281ReadFaults(Kwp1281Fault *faults, uint8_t maxFaults, uint8_t &count); // block 0x07 -> 0xFC blocks
bool kwp1281ClearFaults(bool &accepted);         // block 0x05; accepted = false if the module refused it (NAK)
// block 0x28: the group in "basic setting" mode (the module runs its own routine, e.g. ME7 group 060 = throttle body
// adaptation) and reads its fields back, as kwp1281ReadGroup does. Send it again and again to keep the mode running.
bool kwp1281BasicSetting(uint8_t group, Kwp1281Group &out);
bool kwp1281KeepAlive();  // send an ACK block (call when idle > 1 s)
void kwp1281Disconnect(); // end-output block 0x06
bool kwp1281Connected();
float kwp1281Decode(uint8_t formula, uint8_t a, uint8_t b, char *unit, size_t unitLen); // VAG measuring-block formulas

// ---- KWP2000 (ISO 14230-4, physical header 0x80 | len, target, source 0xF1; 0.17.4: functional 0xC0 | len to 0x33)
#define KWP2000_OBD_ADDRESS 0x33 // ISO 14230-4 functional address for OBD-II
bool kwp2000Connect(uint8_t ecuAddress, bool fastInit, uint8_t *keyBytesOut);
// Sends one request and waits for the answer. A "response pending" (7F xx 78) is waited out, not returned.
// Returns false on timeout, bad checksum, a response longer than respCap, or a negative response (resp then holds
// [7F][sid][code] and kwp2000LastNrc() returns the code).
bool kwp2000Request(const uint8_t *req, uint8_t reqLen, uint8_t *resp, uint8_t &respLen, uint32_t timeoutMs = 300, uint16_t respCap = 250);
// 0.38.0: the next answer message to the same request (OBD-II on K-line: fault codes 3 to a message, the VIN in 5, one
// answer per ECU). false = nothing more within timeoutMs (or a broken message)
bool kwp2000ReadNext(uint8_t *resp, uint8_t &respLen, uint32_t timeoutMs = 100, uint16_t respCap = 250);
// 0.19.0 diagnostic: like kwp2000Request but hands back the raw reply bytes even on a bad checksum (returns the byte
// count; *checksumOk says if it validated). One single read, no response-pending retry. For the fast-path bench diag.
uint8_t kwp2000RequestRaw(const uint8_t *req, uint8_t reqLen, uint8_t *resp, uint16_t respCap, bool *checksumOk, uint32_t timeoutMs = 300);
uint8_t kwp2000LastNrc(); // negative response code of the last request, 0 = none (timeout / checksum / ok)
// 0.17.6: where the last kwp2000Request's time went, in microseconds: sending the request (and reading back its echo),
// waiting for the ECU's first answer byte, and receiving the rest of the answer
void kwp2000LastTiming(uint32_t &sendUs, uint32_t &waitUs, uint32_t &answerUs);
// tester timing: P3 = min gap after the ECU's last byte before a new request, P4 = gap between request bytes.
// Defaults 0 / 5 ms (what every stack used so far); the ECU flash read sets the ECU's negotiated limits.
void kwp2000SetTiming(uint16_t p3MinMs, uint8_t p4Ms);
// 0.17.2: AccessTimingParameters - reads the ECU's timing limits (83 00) and sets them (83 03 + 5 values); if it won't
// give its limits, fallbackValues (P2min P2max P3min P3max P4min, 5 bytes, ISO units) are set; NULL = don't set.
// When the ECU accepts, our P3 becomes its P3min and our P4 testerP4Ms. true = the ECU took the new timing.
bool kwp2000AccessTiming(const uint8_t *fallbackValues, uint8_t testerP4Ms);
bool kwp2000ReadPid(uint8_t pid, uint8_t *data, uint8_t &len); // mode 01
// 0.38.0: OBD-II fault codes over the K-line, any make (session to 0x33): mode 0x03 stored / 0x07 pending -> the raw
// 2-byte codes (each once, from every ECU that answers); answered = an ECU gave a positive answer at all
uint8_t kwp2000ReadObdCodes(uint8_t mode, uint16_t *codes, uint8_t maxCodes, bool &answered);
bool kwp2000ClearObdCodes();                         // 0.38.0: mode 04 - clears them (and the MIL); true = done
bool kwp2000ReadObdVin(char *vin, uint8_t vinLen);   // 0.38.0: mode 09 02 - the 17-character VIN (vinLen >= 18)
bool kwp2000TesterPresent();
void kwp2000Disconnect();
bool kwp2000Connected();
bool kwp2000IsIso9141(); // 0.17.4: the session is ISO 9141-2 (key bytes 08 08 / 94 94), not KWP2000
uint8_t kwp2000Target();
// 0.17.4: the address requests go to after the wake-up, when it differs from the wake-up address (ME7: woken with
// 0x11, talks as 0x10 - ME7Logger's Connect=SLOW-0x11 / Communicate=HM2-0x10)
void kwp2000SetTarget(uint8_t target);
// 0.17.4: false = header mode 0 - requests are [length][data][checksum] with no address bytes (ME7Logger's
// Communicate=HM0); true (the default after every connect) = [0x80 | length][target][F1][data][checksum]
void kwp2000SetAddressBytes(bool withAddresses);
uint8_t kwp2000Checksum(const uint8_t *data, uint8_t len);
