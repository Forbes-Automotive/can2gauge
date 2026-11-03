#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// adc2can receiver (J:/My Drive/PlatformIO/adc2can, docs: CAN protocol). Up to two adc2can boards on the gauge bus, 8
// sensors each: board 0 -> SIG_ADC_1..8, board 1 -> SIG_ADC_9..16. 29-bit ids settings.adcBaseId + board * 16 + n:
//   n 0-3  CH1+2 .. CH7+8 value, float32 big-endian (NaN = channel not valid)       - already scaled by the adc2can
//   n 4    status: valid / rail-on / rail-fault / out-of-range masks, flags, sensor bus /50 mV, VBAT mV
//   n 5    descriptor: ch << 4 | part, 7 ASCII chars (name 0-6, name 7-13, unit)      - the adc2can's names
//   n 6-7  CH1-4 / CH5-8 connector voltage, uint16 mV big-endian
//   n 8    config part (adc2can 1.1+): [0] ch [1] part [2..7] - every setting of the channel, 30 parts (AdcRemoteCfg);
//          adc2can 1.4+: 32, parts 30 / 31 = the dial range in use ([6] = 1 set by hand, 0 Auto)
//   n 9    info: config revision, firmware, protocol. A new revision = the gauge asks for the whole config again (n 12),
//          so whatever is set on the adc2can's own web UI shows up here by itself.
//   n 12 / 13 (gauge -> adc2can): request the config / set one part - the gauge's ADC tab edits the adc2can's settings
// Per channel the gauge either shows the adc2can's value (ADC_MODE_SENT) or scales the connector voltage itself
// (ADC_MODE_LINEAR: volts x scale + offset, web UI > ADC). A name / unit set on the gauge wins over the adc2can's.
// Frames are heard by every gauge on the wire, so values go in with signalSetBus() (the chain link doesn't re-send them).
// 0.8.0: adc2can 1.3+ also broadcasts the same frames over ESP-NOW (include/adcnow_proto.h, settings.adcWireless), so a
// gauge with power only shows the sensors too; they go through the same decoder. A board heard on CAN in the last second
// uses CAN and its air copy is ignored. Over the air it's one-way: config requests / edits (n 12 / 13) need CAN, and the
// config arrives from the adc2can's slow background cycle (~24 s for all of it).
// ---------------------------------------------------------------------------
// ADC2CAN_BOARDS / ADC_SENSORS live in can2gauge_defs.h (0.36.6: the settings need them too)
#define ADC2CAN_PARTS 32     // config parts per channel at most (protocol 2): 30 up to adc2can 1.3, 32 from 1.4
#define ADC2CAN_PARTS_OLD 30 // what a board has until its info frame says (adc2can 1.1-1.3)

// one channel's settings as stored on the adc2can (adc2can ChCfg)
struct AdcRemoteCfg
{
  uint8_t mode;       // 0 linear, 1 table, 2 NTC
  uint8_t decimals;
  uint8_t flags;      // b0 rail on, b1 ratiometric
  uint8_t smoothing;  // 1 none .. 250 heavy
  float scale, offset, lowWarn, highWarn, trim, ntcR25, ntcBeta, ntcPullup;
  float tblV[8], tblVal[8];
  char name[16], unit[8];
  float dialLo, dialHi; // 0.36.4: the dial range the adc2can uses for this sensor (parts 30 / 31, adc2can 1.4+)
  uint8_t dialByHand;   // 0.36.4: bit 0 = its Dial Min was set by hand, bit 1 = its Dial Max (0 = Auto)
  uint32_t got;         // the parts received, bit n = part n (complete = all the board has: Adc2canBoard.parts)
};

struct Adc2canBoard
{
  bool seen;            // a frame from this board in the last 2 s
  uint8_t validMask;    // per channel: rail OK and reading valid
  uint8_t railOnMask;
  uint8_t railFaultMask;
  uint8_t rangeMask;    // outside the adc2can's low / high warn
  uint8_t flags;        // b0 sensor-bus back-feed, b1 VBAT low, b2 VBAT high, b3/b4 ADS1115 fail, b5 TCA9554 fail, b6 no 12 V
  float busV, vbat;     // sensor supply, battery
  float pinV[8];        // connector volts (NaN until heard)
  char name[8][15];     // the adc2can's names / units
  char unit[8][8];
  uint32_t lastMs;
  AdcRemoteCfg cfg[8];  // its settings (adc2can 1.1+)
  bool info;            // an info frame was heard (= protocol 2: the config is shared)
  uint32_t rev;         // its config revision
  uint8_t fwMajor, fwMinor;
  uint8_t parts;        // 0.36.4: config parts per channel it says it has (info frame [7]): 30 up to 1.3, 32 from 1.4
  uint32_t reqMs;       // last config request
  uint32_t lastCanMs;   // 0.8.0: last frame heard on CAN
  uint32_t lastNowMs;   // 0.8.0: last ESP-NOW packet from it (adc2can 1.3+)
  int8_t nowRssi;       // its ESP-NOW signal
  uint8_t nowMac[6];
};

void setupADC2CAN();                 // registers the CANLine frame handler
void adc2canApply();                 // settings changed: names / units
const Adc2canBoard &adc2canBoard(uint8_t board);
bool adc2canAny();                   // any board heard
void adc2canInfoProvider(char *buf, size_t len);
bool adc2canConfigComplete(uint8_t board, uint8_t ch);
bool adc2canHasDialRange(uint8_t board, uint8_t ch); // 0.36.4: the adc2can sent this sensor's dial range (1.4+)
// 0.36.4: the range a gauge page showing signal id draws with, when id is an adc2can sensor scaled on the adc2can that
// sent its dial range: low / high in the units on show. false = not one (the page uses its own Min / Max)
bool adc2canPageRange(signalId_t id, float &low, float &high);
uint8_t adc2canDecimals(uint8_t sensor); // decimals sensor 0-15 is shown with (the adc2can's, or this gauge's override)
// edit the adc2can's settings over CAN (n 13): one part; the adc2can echoes it back and saves
void adc2canSetPart(uint8_t board, uint8_t ch, uint8_t part, const uint8_t payload[6]);
void adc2canSetFloat(uint8_t board, uint8_t ch, uint8_t part, float value);
void adc2canSetText(uint8_t board, uint8_t ch, bool unit, const char *text); // name (parts 25-27) or unit (28-29)
void adc2canRequest(uint8_t board, uint8_t ch);   // n 12: send the config of ch (0xFF = all)
void adc2canRailReset(uint8_t board, uint8_t ch); // n 12: clear a latched rail fault (0xFF = all)
// 0.8.0: an ESP-NOW packet with the 'A','D' magic (called from the link's receive callback, WiFi task)
void adc2canOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len);
