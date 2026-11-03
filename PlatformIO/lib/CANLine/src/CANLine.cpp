#include "CANLine.h"
#include <can2gauge_defs.h>

#define MAX_HANDLERS 10 // 0.30.11: was 8 - boot sync took the last free one
static canFrameCb_t handlers[MAX_HANDLERS];
static bool handlerAir[MAX_HANDLERS]; // also gets relayed frames
static uint8_t handlerCount = 0;
// relayed frames (canInject): decoded by their own task, under decodeMutex with the RX task
struct AirFrame
{
  twai_message_t message;
  uint8_t source;
};
#define AIR_QUEUE 192
static QueueHandle_t airQueue = NULL;
static SemaphoreHandle_t decodeMutex = NULL;
static CanAirStats airStats;
#define ID_SLOTS 2048 // one slot per 11-bit CAN id
// both tables live in PSRAM (6 KB off internal RAM, which WiFi and the web server need - see psramCalloc)
static uint8_t *idOwner = (uint8_t *)psramCalloc(ID_SLOTS, sizeof(uint8_t));    // per id slot: 0 = the wire, 1..CAN_AIR_SOURCES a relay sender, 0xFF = nobody yet
static uint16_t *idOwnerMs = (uint16_t *)psramCalloc(ID_SLOTS, sizeof(uint16_t)); // millis() (low 16 bits) of the owner's last frame
static CanStats stats;
static TaskHandle_t rxTask = NULL;
static uint8_t profile = CAN_PROFILE_VAG_PQ;
static uint16_t haldexBroadcastId = 0x6B0;
static uint16_t haldexControlId = 0x6A0;
static CanFrameLog ring[16];
static uint8_t ringHead = 0;
static portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool rxParked = true; // RX task is not inside twai_receive (safe to uninstall the driver)
static bool listenOnly = false;       // TWAI_MODE_LISTEN_ONLY: no frames, no ACKs (gauge standby, src/can2gauge_Power.cpp)
static bool driverInstalled = false;   // a TWAI driver is installed (canEnd can fail to remove one: driverRemove)
static volatile bool beginRetry = false; // canBegin failed: the RX task tries again every second until canEnd
static SemaphoreHandle_t driverMutex = NULL; // canBegin / canEnd: the io task, the web API and the RX task's retry
static int8_t busTxPin = -1;
static int8_t busRxPin = -1;
static uint16_t maxxBase = 0x520; // CAN_PROFILE_MAXXECU
static uint16_t busKbps = 500;

// ---------------- decoders ----------------
// Byte numbers below are 0-based data[] indices. The VW K-matrix spreadsheets count bytes from 1 (checked against
// mBremse_1 BR1_Rad_kmh "byte 3 bit 1" = data[2] bit 1), so K-matrix "byte N" = data[N-1].
// Every decoder here uses signalSetBus(): each gauge on the same wire hears these frames itself, so the chain link
// does not re-send them over CAN.

// gear selector + gear have up to three sources on PQ; the higher-priority one wins while it is being received
static uint32_t pqSelectorMs[3] = {0, 0, 0}; // [0] mGetriebe_2 display, [1] mGetriebe_1, [2] mWaehlhebel_1
static uint32_t pqAktGangMs = 0;             // mGetriebe_2 "currently engaged gear" (preferred over Getriebe_1 target gear)
static void setPqSelector(uint8_t priority, uint8_t selector)
{
  const uint32_t now = millis();
  pqSelectorMs[priority] = now;
  for (uint8_t betterPriority = 0; betterPriority < priority; betterPriority++)
  {
    if (pqSelectorMs[betterPriority] && now - pqSelectorMs[betterPriority] < 1000)
    {
      return; // a better source is live
    }
  }
  if (selector != SEL_NONE)
  {
    signalSetBus(SIG_SELECTOR, selector);
  }
}

// PQ35/46 ACAN mGetriebe_1 GE1_Zielgang (byte 2 bits 0-3): 0 P/N, 1-5, 6 "1m", 7 R, 8 = 6th, 9 = 7th, 10 = 8th
static int pqZielgangToGear(uint8_t raw)
{
  if (raw <= 5)
  {
    return raw;
  }
  if (raw == 6)
  {
    return 1;
  }
  if (raw == 7)
  {
    return 0; // R: gear 0, selector R
  }
  if (raw <= 10)
  {
    return raw - 2;
  }
  return -1; // 14 undefined / 15 error
}

// PQ selector codes shared by GE1_Wahl_Pos (0x440) and WH1_Position (0x448):
// 1-4 Pos_1..4, 5 D, 6 N, 7 R, 8 P, 9 (Wahl_Pos: manual sport / WH1: N without spot), 10/11 (WH1: tip +/-), 12 S, 13 L, 14 tip gate
static uint8_t pqSelectorFromCode(uint8_t code, bool lever)
{
  switch (code)
  {
  case 1:
  case 2:
  case 3:
  case 4: return SEL_M;
  case 5: return SEL_D;
  case 6: return SEL_N;
  case 7: return SEL_R;
  case 8: return SEL_P;
  case 9: return lever ? SEL_N : SEL_M;
  case 10:
  case 11: return lever ? SEL_M : SEL_NONE; // Wahl_Pos 10/11 = Z1/Z2 intermediate
  case 12: return SEL_S;
  case 13: return SEL_L;
  case 14: return SEL_M;
  default: return SEL_NONE;
  }
}

static void decodeVagPq(const twai_message_t &message)
{
  const uint8_t *data = message.data;
  switch (message.identifier)
  {
  case 0x280: // mMotor_1: MO1_Drehzahl bytes 3-4 (data[2..3]) LE * 0.25, MO1_Pedalwert byte 6 (data[5]) * 0.4
    signalSetBus(SIG_RPM, ((data[3] << 8) | data[2]) * 0.25f);
    signalSetBus(SIG_PEDAL, data[5] * 0.4f);
    signalSetBus(SIG_LOAD, data[1] * 0.39f);
    break;
  case 0x288: // mMotor_2: MO2_Kuehlm_T byte 2 (data[1]) * 0.75 - 48, raw 0 = init / 255 = error
    if (data[1] != 0 && data[1] != 0xFF)
    {
      signalSetBus(SIG_COOLANT, data[1] * 0.75f - 48.0f);
    }
    break;
  case 0x380: // mMotor_3: MO3_Aussentemp byte 2 (data[1]) * 0.75 - 48, MO3_Pedalwert byte 3 (data[2]) * 0.4.
    // (0.3.0 decoded data[1] as throttle and data[2] as intake temp - the K-matrix has no intake temp in Motor_3)
    if (data[1] != 0xFF)
    {
      signalSetBus(SIG_AMBIENT, data[1] * 0.75f - 48.0f);
    }
    break;
  case 0x1A0: // mBremse_1: BR1_Rad_kmh byte 3 bit 1, 15 bits * 0.01 (32708+ = init / undervoltage / sensor fault)
  {
    const uint16_t raw = (((uint16_t)data[3] << 8) | data[2]) >> 1;
    if (raw < 32708)
    {
      signalSetBus(SIG_SPEED, raw * 0.01f);
    }
    break;
  }
  case 0x320: // mKombi_1: KO1_Tankinhalt byte 3 (data[2]) bits 0-6 = litres, 127 = error (bit 7 = OBD tank warning)
    if ((data[2] & 0x7F) != 0x7F)
    {
      signalSetBus(SIG_FUEL_LEVEL, data[2] & 0x7F);
    }
    // Oeldruck data[0] bit 2: read as the oil-pressure warning (it sits among the cluster's lamp bits - charge lamp,
    // glow plug, tank warning, coolant low), so 0 = pressure OK. Unconfirmed on a car: ignition on with the engine off
    // must show LOW
    signalSetBus(SIG_OIL_PRESS, (data[0] >> 2) & 1 ? 0.0f : 1.0f);
    break;
  case 0x420: // mKombi_2: KO2_Oeltemperatur data[3] x1 - 60 C (G266 via the cluster), Fehlerstatus_Oeltemperatur data[0]
    // bit 1. Same layout on the Mk4 1J0-920 cluster (canhack.de Golf IV log: data[1]/[2] outside temp, data[4] coolant)
    // and PQ35 (opendbc vw_pq.dbc). Status always kept for the oil page; the value only feeds SIG_OIL_TEMP when CAN is
    // the chosen oil source, so a K-line read is never mixed with it. 0xFF = no value.
    {
      OilStatus &oil = runtime.oil;
      oil.kombi2Ms = millis() | 1;
      oil.kombi2Count++;
      oil.kombi2OilRaw = data[3];
      oil.kombi2OilErr = (data[0] >> 1) & 1;
      if (settings.oilSource == OIL_SRC_CAN && !oil.kombi2OilErr && data[3] != 0xFF)
      {
        signalSetBus(SIG_OIL_TEMP, data[3] - 60.0f);
      }
      break;
    }
  case 0x440: // mGetriebe_1: GE1_Zielgang byte 2 bits 0-3, GE1_Wahl_Pos byte 2 bits 4-7 (data[1])
  {
    const int gear = pqZielgangToGear(data[1] & 0x0F);
    if (gear >= 0 && !(pqAktGangMs && millis() - pqAktGangMs < 1000))
    {
      signalSetBus(SIG_GEAR, gear);
    }
    uint8_t selector = pqSelectorFromCode(data[1] >> 4, false);
    if ((data[1] & 0x0F) == 7 && selector == SEL_NONE)
    {
      selector = SEL_R;
    }
    setPqSelector(1, selector);
    break;
  }
  case 0x448: // mWaehlhebel_1 (selector lever module): WH1_Position byte 1 bits 4-7 (data[0])
    setPqSelector(2, pqSelectorFromCode(data[0] >> 4, true));
    break;
  case 0x540: // mGetriebe_2: byte 8 (data[7]) = GE2_Variante mux (bits 0-3) + the value the cluster displays (bits 4-7)
  {
    const uint8_t mux = data[7] & 0x0F;
    const uint8_t value = data[7] >> 4;
    switch (mux)
    {
    case 15: // GE2_akt_Gang: 0 none, 2..10 = gear 1..9, 12 R, 13 N, 14 P
      pqAktGangMs = millis();
      if (value >= 2 && value <= 10)
      {
        signalSetBus(SIG_GEAR, value - 1);
      }
      else if (value == 0 || value >= 12)
      {
        signalSetBus(SIG_GEAR, 0);
      }
      break;
    case 6:  // GE2_PRNDS: 2 P, 3 R, 4 N, 5 D, 6 S
    case 1:  // GE2_PRNDL: ... 6 L
    case 7:  // GE2_PRNED: 2 P, 3 R, 4 N, 5 E, 6 D
    case 11: // GE2_PRNDB: ... 6 B
    {
      static const uint8_t selectorTable[5] = {SEL_P, SEL_R, SEL_N, SEL_D, SEL_S};
      if (value >= 2 && value <= 6)
      {
        uint8_t selector = selectorTable[value - 2];
        if (value == 6)
        {
          if (mux == 1)
          {
            selector = SEL_L;
          }
          else if (mux == 11)
          {
            selector = SEL_B;
          }
          else if (mux == 7)
          {
            selector = SEL_D;
          }
          else
          {
            selector = SEL_S;
          }
        }
        if (value == 5 && mux == 7)
        {
          selector = SEL_E;
        }
        setPqSelector(0, selector);
      }
      break;
    }
    case 12: // GE2_Tip6Gang / Tip7Gang / Tip8Gang: 2.. = gear 1.. in the tiptronic gate
    case 13:
    case 14:
      if (value >= 2)
      {
        setPqSelector(0, SEL_M);
        if (!(pqAktGangMs && millis() - pqAktGangMs < 1000))
        {
          signalSetBus(SIG_GEAR, value - 1);
        }
      }
      break;
    default:
      break;
    }
    break;
  }
  default:
    break;
  }
}

// VAG selector code -> SIG_SELECTOR. MQB Getriebe_11 GE_Fahrstufe and MLB Getriebe_03 GE_Waehlhebel use the same codes:
// 5 P, 6 R, 7 N, 8 D, 9 S, 10 E (Effizienz), 13 tip in S, 14 tip in D; 0/1 intermediate/init, 15 error
static void setVagSelector(uint8_t code)
{
  static const uint8_t fahrstufeMap[16] = {SEL_NONE, SEL_NONE, SEL_NONE, SEL_NONE, SEL_NONE, SEL_P, SEL_R, SEL_N,
                                           SEL_D, SEL_S, SEL_E, SEL_NONE, SEL_NONE, SEL_M, SEL_M, SEL_NONE};
  if (fahrstufeMap[code & 0x0F] != SEL_NONE)
  {
    signalSetBus(SIG_SELECTOR, fahrstufeMap[code & 0x0F]);
  }
}

// VAG target gear -> SIG_GEAR (MQB Getriebe_11 and MLB Getriebe_03 GE_Zielgang, same coding):
// 0 P/N, 1-7 gear, 8 R, 11/12/13 = gear 8/9/10, 14 undefined, 15 error
static void setVagZielgang(uint8_t zielgang)
{
  if (zielgang <= 7)
  {
    signalSetBus(SIG_GEAR, zielgang);
  }
  else if (zielgang == 8)
  {
    signalSetBus(SIG_GEAR, 0);
  }
  else if (zielgang >= 11 && zielgang <= 13)
  {
    signalSetBus(SIG_GEAR, zielgang - 3);
  }
}

// 0.26.0: the gearbox input (turbine) speed, a 14-bit rpm (MQB Getriebe_13 / MLB Getriebe_03 GE_Eingangsdrehz).
// 16382 = still starting up, 16383 = error. The converter slip is the engine's rpm minus this.
static void setGearboxInputRpm(uint16_t inputRpm)
{
  if (inputRpm >= 16382)
  {
    return;
  }
  signalSetBus(SIG_INPUT_RPM, inputRpm);
  if (signalValid(SIG_RPM))
  {
    signalSetBus(SIG_CONVERTER_SLIP, signalGet(SIG_RPM) - inputRpm);
  }
}

// MQB FCAN (MQB_FCAN_KMatrix V7.20F; Intel byte order, DBC start bits)
static void decodeVagMqb(const twai_message_t &message)
{
  const uint8_t *data = message.data;
  switch (message.identifier)
  {
  case 0x0A8: // Motor_12: MO_Drehzahl_01 48|16 * 0.25
    signalSetBus(SIG_RPM, (((uint16_t)data[7] << 8) | data[6]) * 0.25f);
    break;
  case 0x0FD: // ESP_21: ESP_v_Signal 32|16 * 0.01 km/h
    signalSetBus(SIG_SPEED, (((uint16_t)data[5] << 8) | data[4]) * 0.01f);
    break;
  case 0x121: // Motor_20: MO_Fahrpedalrohwert_01 12|8 * 0.4
    signalSetBus(SIG_PEDAL, (uint8_t)((data[1] >> 4) | (data[2] << 4)) * 0.4f);
    break;
  case 0x640: // Motor_07: MO_Ansaugluft_Temp 8|8, MO_Kuehlmittel_Temp 24|8, both * 0.75 - 48
    signalSetBus(SIG_IAT, data[1] * 0.75f - 48.0f);
    signalSetBus(SIG_COOLANT, data[3] * 0.75f - 48.0f);
    break;
  case 0x6B7: // Kombi_02: KBI_Inhalt_Tank 40|7 litres, KBI_Aussen_Temp_gef 56|8 * 0.5 - 50
    signalSetBus(SIG_FUEL_LEVEL, data[5] & 0x7F);
    signalSetBus(SIG_AMBIENT, data[7] * 0.5f - 50.0f);
    break;
  case 0x0AD: // Getriebe_11: GE_Fahrstufe 42|4, GE_Zielgang 60|4
  {
    setVagSelector((data[5] >> 2) & 0x0F); // GE_Fahrstufe
    setVagZielgang(data[7] >> 4);          // GE_Zielgang
    // 0.26.0: GE_Schaltablauf 47|2 (bit 7 of byte 5 + bit 0 of byte 6): 0 no shift, 1 torque handover, 2 fill phase,
    // 3 speed handover - anything but 0 is a shift under way
    const uint8_t shiftPhase = (data[5] >> 7) | ((data[6] & 0x01) << 1);
    signalSetBus(SIG_SHIFTING, shiftPhase != 0 ? 1 : 0);
    // 0.26.0: GE_Status_Kraftschluss 53|3 (bits 5-7 of byte 6) - on an 8HP (AL550) the torque converter lock-up, on a
    // DSG the clutch: 0 open / no drive, 1 open with drive, 2 slipping, 3 safely open, 4 closed with micro-slip,
    // 5 closed and over-clamped, 6 / 7 errors
    const uint8_t kraftschluss = (data[6] >> 5) & 0x07;
    if (kraftschluss == 2)
    {
      signalSetBus(SIG_LOCKUP, 1); // slipping
    }
    else if (kraftschluss == 4 || kraftschluss == 5)
    {
      signalSetBus(SIG_LOCKUP, 2); // locked
    }
    else if (kraftschluss <= 3)
    {
      signalSetBus(SIG_LOCKUP, 0); // open
    }
    break;
  }
  case 0x12D: // 0.26.0: Getriebe_13 GE_Eingangsdrehz 48|14 - gearbox input speed (drivetrain CAN; may not reach every bus)
    setGearboxInputRpm(data[6] | ((data[7] & 0x3F) << 8));
    break;
  case 0x3C8: // Getriebe_14: GE_Sumpftemperatur 56|8 - 58 (raw 255 = no value)
    if (data[7] != 0xFF)
    {
      signalSetBus(SIG_GEARBOX_TEMP, data[7] - 58.0f);
    }
    break;
  default:
    break;
  }
}

// 0.26.0: Audi MLB drivetrain CAN (opendbc vw_mlb.dbc, a K-matrix extract; Intel byte order, DBC start bits). The
// A6/A7 C7, A8 D4 and Q5 8R with the ZF 8HP (VW AL551). Sits at the J533 gateway: the OBD port doesn't carry these.
// docs/8HP_research.md section 2.1.
static void decodeVagMlb(const twai_message_t &message)
{
  const uint8_t *data = message.data;
  switch (message.identifier)
  {
  case 0x105: // Motor_03: MO_Drehzahl_01 16|16 * 0.25 rpm, MO_Fahrpedalrohwert_01 48|8 * 0.4 %
    signalSetBus(SIG_RPM, (((uint16_t)data[3] << 8) | data[2]) * 0.25f);
    signalSetBus(SIG_PEDAL, data[6] * 0.4f);
    break;
  case 0x100: // ESP_01: ESP_v_Signal 32|16 * 0.01 km/h (65533 and up = no value)
  {
    const uint16_t rawSpeed = ((uint16_t)data[5] << 8) | data[4];
    if (rawSpeed < 65533)
    {
      signalSetBus(SIG_SPEED, rawSpeed * 0.01f);
    }
    break;
  }
  case 0x640: // Motor_07: MO_Kuehlmittel_Temp 24|8 * 0.75 - 48 (as MQB)
    signalSetBus(SIG_COOLANT, data[3] * 0.75f - 48.0f);
    break;
  case 0x6B7: // Kombi_02: KBI_Inhalt_Tank 40|7 litres, KBI_Aussen_Temp_gef 56|8 * 0.5 - 50 (as MQB)
    signalSetBus(SIG_FUEL_LEVEL, data[5] & 0x7F);
    signalSetBus(SIG_AMBIENT, data[7] * 0.5f - 50.0f);
    break;
  case 0x102: // Getriebe_03: GE_Zielgang 24|4, GE_Waehlhebel 44|4, GE_Eingangsdrehz 48|14
    setVagZielgang(data[3] & 0x0F);                       // same coding as MQB (11 = 8th gear)
    setVagSelector(data[5] >> 4);                         // same codes as MQB's GE_Fahrstufe
    setGearboxInputRpm(data[6] | ((data[7] & 0x3F) << 8)); // gearbox input (turbine) speed
    break;
  case 0x082: // Getriebe_01: GE_Schaltvorgang 12|1, GE_Status_Kupplung 13|2
  {
    signalSetBus(SIG_SHIFTING, (data[1] >> 4) & 0x01); // bit 4 of byte 1: 1 = shift in progress
    // bits 5-6 of byte 1: 0 open, 1 controlled (slipping), 2 closed, 3 error. Probably the converter lock-up clutch on
    // the 8HP (the K-matrix says "clutch"; bench / car check owed)
    const uint8_t clutch = (data[1] >> 5) & 0x03;
    if (clutch <= 2)
    {
      signalSetBus(SIG_LOCKUP, clutch); // 0 open, 1 slipping, 2 locked - already the gauge's coding
    }
    break;
  }
  case 0x441: // Getriebe_04: GE_Sumpftemperatur 56|8 - 58 C (raw 255 = no value)
    if (data[7] != 0xFF)
    {
      signalSetBus(SIG_GEARBOX_TEMP, data[7] - 58.0f);
    }
    break;
  default:
    break;
  }
}

// MaxxECU "Default CAN output" (https://maxxecu.com/webhelp/can-default_maxxecu_protocol.html): 500 kbit, 11-bit ids from
// the base (0x520, moved by MTune's CAN ID OFFSET), every value a little-endian SIGNED 16-bit, value = raw x scale. FAST
// frames (base + 0x00..0x08) at 50 Hz, SLOW (base + 0x10..0x22) at 10 Hz. Protocol 1.2 sends base .. +4 and +0x10 .. +0x16
// bytes 0-3; 1.3 adds the rest (oil pressure / temperature, fuel pressure, transmission temperature ...).
static float maxxBaro = 0.0f; // kPa, from base + 0x10 (boost = MAP - baro)
static inline float maxxInt16(const uint8_t *data, int offset)
{
  return (float)(int16_t)(data[offset] | data[offset + 1] << 8);
}

static void decodeMaxx(const twai_message_t &message)
{
  if (message.data_length_code < 8 || message.identifier < maxxBase)
  {
    return;
  }
  const uint8_t *data = message.data;
  switch (message.identifier - maxxBase)
  {
  case 0x00: // RPM, throttle %, MAP kPa, lambda
  {
    signalSetBus(SIG_RPM, maxxInt16(data, 0));
    signalSetBus(SIG_THROTTLE, maxxInt16(data, 2) * 0.1f);
    const float map = maxxInt16(data, 4) * 0.1f;
    signalSetBus(SIG_BOOST, map * 10.0f); // kPa -> mbar (sets boost from 1013 mbar)
    if (maxxBaro > 50.0f)
    {
      signalSetBus(SIG_BOOST_REL, (map - maxxBaro) / 100.0f); // the ECU's own baro is better than 1013
    }
    signalSetBus(SIG_LAMBDA, maxxInt16(data, 6) * 0.001f);
    break;
  }
  case 0x01: signalSetBus(SIG_TIMING, maxxInt16(data, 4) * 0.1f); break;   // ignition angle deg BTDC
  case 0x02: signalSetBus(SIG_SPEED, maxxInt16(data, 6) * 0.1f); break;    // vehicle speed km/h
  case 0x10:                                                               // battery V, baro kPa, IAT C, coolant C
    signalSetBus(SIG_BATTERY, maxxInt16(data, 0) * 0.01f);
    maxxBaro = maxxInt16(data, 2) * 0.1f;
    signalSetBus(SIG_IAT, maxxInt16(data, 4) * 0.1f);
    signalSetBus(SIG_COOLANT, maxxInt16(data, 6) * 0.1f);
    break;
  case 0x11: signalSetBus(SIG_ETHANOL, maxxInt16(data, 2) * 0.1f); break;  // ethanol % (85 with no sensor / a sensor error)
  case 0x13: signalSetBus(SIG_EGT, maxxInt16(data, 6)); break;             // EGT highest C
  case 0x16:                                                               // gear; 1.3: oil pressure kPa, oil temp C
    signalSetBus(SIG_GEAR, maxxInt16(data, 0));
    signalSetBus(SIG_OIL_PRESSURE, maxxInt16(data, 4) * 0.1f / 100.0f);
    signalSetBus(SIG_OIL_TEMP, maxxInt16(data, 6) * 0.1f);
    break;
  case 0x17: signalSetBus(SIG_FUEL_PRESSURE, maxxInt16(data, 0) * 0.1f / 100.0f); break; // fuel pressure 1 kPa -> bar
  case 0x20: signalSetBus(SIG_GEARBOX_TEMP, maxxInt16(data, 4) * 0.1f); break;           // transmission temp C
  default:
    break;
  }
}

static void decodeOpenHaldex(const twai_message_t &message)
{
  if (message.identifier != haldexBroadcastId || message.data_length_code < 8)
  {
    return;
  }
  const uint8_t *data = message.data;
  const uint32_t now = millis();
  runtime.haldexCanMs = now | 1; // heard on CAN: mode changes go out as 0x6A0
  runtime.haldexLink = true;
  if (runtime.ohxRxMs && now - runtime.ohxRxMs < 500)
  {
    return; // the same controller is heard over WiFi, which carries the engagement % as OpenHaldex shows it
  }
  signalSetBus(SIG_HALDEX_ACTUAL, data[2] * 100.0f / 255.0f); // engagement raw 0..255 -> %
  signalSetBus(SIG_HALDEX_TARGET, data[3]);
  signalSetBus(SIG_HALDEX_SPEED, data[4]);
  signalSetBus(SIG_HALDEX_MODE, data[6]);
  signalSetBus(SIG_HALDEX_PEDAL, data[7]);
}

// ---------------- OBD-II PIDs ----------------
// How a PID's data bytes become its value (SAE J1979 service 01). A and B are the first two data bytes; "AB" is them
// together as one number, 256 * A + B. A PID with several values in it (O2 sensors, trims for two banks) gives its first.
enum obdFormula_t
{
  OBD_F_A,             // A                              (speed, counts)
  OBD_F_A_MINUS_40,    // A - 40                         (temperatures, C)
  OBD_F_PERCENT,       // A * 100 / 255                  (throttle, load, levels, %)
  OBD_F_TRIM,          // (A - 128) * 100 / 128          (fuel trims, EGR error: -100 .. +99 %)
  OBD_F_A_TIMES_3,     // A * 3                          (fuel pressure, kPa)
  OBD_F_A_TIMES_10,    // A * 10                         (MAP / barometric: kPa -> mbar, as the gauge stores boost)
  OBD_F_TIMING,        // A / 2 - 64                     (ignition timing, degrees)
  OBD_F_O2_VOLTS,      // A / 200                        (narrow-band O2 sensor, volts)
  OBD_F_AB,            // AB                             (run time s, distances km, minutes)
  OBD_F_AB_DIV_4,      // AB / 4                         (rpm)
  OBD_F_AB_DIV_10,     // AB / 10                        (fuel rail: AB * 10 kPa, shown in bar)
  OBD_F_AB_DIV_100,    // AB / 100                       (air mass, g/s)
  OBD_F_AB_DIV_1000,   // AB / 1000                      (module voltage, V)
  OBD_F_AB_PERCENT,    // AB * 100 / 255                 (absolute load, can go past 100 %)
  OBD_F_LAMBDA,        // AB / 32768                     (lambda)
  OBD_F_CAT_TEMP,      // AB / 10 - 40                   (catalyst temperatures, C)
  OBD_F_RAIL_VACUUM,   // AB * 0.079                     (fuel rail relative to the manifold, kPa)
  OBD_F_EVAP_QUARTER,  // AB as a signed number / 4      (evap system vapour pressure, Pa)
  OBD_F_EVAP_ABS,      // AB / 200                       (evap system absolute vapour pressure, kPa)
  OBD_F_SIGNED_AB,     // AB as a signed number          (evap system vapour pressure, wide range, Pa)
  OBD_F_INJ_TIMING,    // AB / 128 - 210                 (fuel injection timing, degrees)
  OBD_F_FUEL_RATE,     // AB / 20                        (fuel rate, L/h)
};

// Every standard service 01 value up to PID 0x60, in PID order. signal: the gauge signal it feeds (the 14 the gauge
// always had - rpm, coolant ...), SIG_NONE = only on a page with an "OBD" source and in the web UI's Live OBD list.
// alwaysFast: changes quickly, so it's asked for every time. Units as the gauge stores them: "C" for temperatures (shown
// in C or F), "deg" for angles. The gauge range is what a page gets when it picks the PID (adjustable).
static const ObdPidInfo obdPidTable[] = {
    // pid   name                     unit    formula            min     max      signal          alwaysFast
    {0x04, "Engine load", "%", OBD_F_PERCENT, 0, 100, SIG_LOAD, true},
    {0x05, "Coolant", "C", OBD_F_A_MINUS_40, 0, 140, SIG_COOLANT, false},
    {0x06, "Short trim bank 1", "%", OBD_F_TRIM, -25, 25, SIG_NONE, false},
    {0x07, "Long trim bank 1", "%", OBD_F_TRIM, -25, 25, SIG_NONE, false},
    {0x08, "Short trim bank 2", "%", OBD_F_TRIM, -25, 25, SIG_NONE, false},
    {0x09, "Long trim bank 2", "%", OBD_F_TRIM, -25, 25, SIG_NONE, false},
    {0x0A, "Fuel pressure", "kPa", OBD_F_A_TIMES_3, 0, 765, SIG_NONE, false},
    {0x0B, "Intake pressure (MAP)", "mbar", OBD_F_A_TIMES_10, 0, 2550, SIG_BOOST, true},
    {0x0C, "RPM", "rpm", OBD_F_AB_DIV_4, 0, 8000, SIG_RPM, true},
    {0x0D, "Speed", "km/h", OBD_F_A, 0, 280, SIG_SPEED, true},
    {0x0E, "Timing advance", "deg", OBD_F_TIMING, -10, 40, SIG_TIMING, true},
    {0x0F, "Intake air", "C", OBD_F_A_MINUS_40, -20, 80, SIG_IAT, false},
    {0x10, "Air mass (MAF)", "g/s", OBD_F_AB_DIV_100, 0, 300, SIG_MAF, false},
    {0x11, "Throttle valve", "%", OBD_F_PERCENT, 0, 100, SIG_THROTTLE, true}, // 0.37.12: the pedal feeds Throttle when it can (throttlePedalPid)
    {0x14, "O2 sensor 1", "V", OBD_F_O2_VOLTS, 0, 1.275f, SIG_NONE, false},
    {0x15, "O2 sensor 2", "V", OBD_F_O2_VOLTS, 0, 1.275f, SIG_NONE, false},
    {0x16, "O2 sensor 3", "V", OBD_F_O2_VOLTS, 0, 1.275f, SIG_NONE, false},
    {0x17, "O2 sensor 4", "V", OBD_F_O2_VOLTS, 0, 1.275f, SIG_NONE, false},
    {0x18, "O2 sensor 5", "V", OBD_F_O2_VOLTS, 0, 1.275f, SIG_NONE, false},
    {0x19, "O2 sensor 6", "V", OBD_F_O2_VOLTS, 0, 1.275f, SIG_NONE, false},
    {0x1A, "O2 sensor 7", "V", OBD_F_O2_VOLTS, 0, 1.275f, SIG_NONE, false},
    {0x1B, "O2 sensor 8", "V", OBD_F_O2_VOLTS, 0, 1.275f, SIG_NONE, false},
    {0x1F, "Run time", "s", OBD_F_AB, 0, 3600, SIG_NONE, false},
    {0x21, "Distance MIL on", "km", OBD_F_AB, 0, 1000, SIG_NONE, false},
    {0x22, "Fuel rail (vacuum ref)", "kPa", OBD_F_RAIL_VACUUM, 0, 5177, SIG_NONE, false},
    {0x23, "Fuel rail pressure", "bar", OBD_F_AB_DIV_10, 0, 2500, SIG_NONE, false},
    {0x24, "O2 1 lambda (V)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x25, "O2 2 lambda (V)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x26, "O2 3 lambda (V)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x27, "O2 4 lambda (V)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x28, "O2 5 lambda (V)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x29, "O2 6 lambda (V)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x2A, "O2 7 lambda (V)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x2B, "O2 8 lambda (V)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x2C, "Commanded EGR", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x2D, "EGR error", "%", OBD_F_TRIM, -100, 100, SIG_NONE, false},
    {0x2E, "Evap purge", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x2F, "Fuel level", "%", OBD_F_PERCENT, 0, 100, SIG_FUEL_LEVEL, false},
    {0x30, "Warm-ups since clear", "", OBD_F_A, 0, 255, SIG_NONE, false},
    {0x31, "Distance since clear", "km", OBD_F_AB, 0, 10000, SIG_NONE, false},
    {0x32, "Evap pressure", "Pa", OBD_F_EVAP_QUARTER, -8192, 8192, SIG_NONE, false},
    {0x33, "Barometric pressure", "mbar", OBD_F_A_TIMES_10, 800, 1100, SIG_NONE, false},
    {0x34, "O2 1 lambda (mA)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x35, "O2 2 lambda (mA)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x36, "O2 3 lambda (mA)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x37, "O2 4 lambda (mA)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x38, "O2 5 lambda (mA)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x39, "O2 6 lambda (mA)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x3A, "O2 7 lambda (mA)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x3B, "O2 8 lambda (mA)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_NONE, false},
    {0x3C, "Cat temp bank 1 S1", "C", OBD_F_CAT_TEMP, 0, 1000, SIG_NONE, false},
    {0x3D, "Cat temp bank 2 S1", "C", OBD_F_CAT_TEMP, 0, 1000, SIG_NONE, false},
    {0x3E, "Cat temp bank 1 S2", "C", OBD_F_CAT_TEMP, 0, 1000, SIG_NONE, false},
    {0x3F, "Cat temp bank 2 S2", "C", OBD_F_CAT_TEMP, 0, 1000, SIG_NONE, false},
    {0x42, "Battery", "V", OBD_F_AB_DIV_1000, 10, 16, SIG_BATTERY, false},
    {0x43, "Absolute load", "%", OBD_F_AB_PERCENT, 0, 200, SIG_NONE, false},
    {0x44, "Lambda (commanded)", "", OBD_F_LAMBDA, 0.6f, 1.4f, SIG_LAMBDA, true},
    {0x45, "Relative throttle", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x46, "Outside air", "C", OBD_F_A_MINUS_40, -40, 50, SIG_AMBIENT, false},
    {0x47, "Throttle B", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x48, "Throttle C", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x49, "Pedal D", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x4A, "Pedal E", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x4B, "Pedal F", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x4C, "Throttle actuator", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x4D, "Time MIL on", "min", OBD_F_AB, 0, 1000, SIG_NONE, false},
    {0x4E, "Time since clear", "min", OBD_F_AB, 0, 10000, SIG_NONE, false},
    {0x52, "Ethanol", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x53, "Evap abs pressure", "kPa", OBD_F_EVAP_ABS, 0, 330, SIG_NONE, false},
    {0x54, "Evap pressure (wide)", "Pa", OBD_F_SIGNED_AB, -32768, 32767, SIG_NONE, false},
    {0x55, "Short O2 trim bank 1", "%", OBD_F_TRIM, -25, 25, SIG_NONE, false},
    {0x56, "Long O2 trim bank 1", "%", OBD_F_TRIM, -25, 25, SIG_NONE, false},
    {0x57, "Short O2 trim bank 2", "%", OBD_F_TRIM, -25, 25, SIG_NONE, false},
    {0x58, "Long O2 trim bank 2", "%", OBD_F_TRIM, -25, 25, SIG_NONE, false},
    {0x59, "Fuel rail absolute", "bar", OBD_F_AB_DIV_10, 0, 2500, SIG_NONE, false},
    {0x5A, "Relative pedal", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x5B, "Hybrid battery", "%", OBD_F_PERCENT, 0, 100, SIG_NONE, false},
    {0x5C, "Oil temperature", "C", OBD_F_A_MINUS_40, 0, 160, SIG_OIL_TEMP, false},
    {0x5D, "Injection timing", "deg", OBD_F_INJ_TIMING, -210, 302, SIG_NONE, false},
    {0x5E, "Fuel rate", "L/h", OBD_F_FUEL_RATE, 0, 50, SIG_NONE, false},
};
static_assert(sizeof(obdPidTable) / sizeof(obdPidTable[0]) == OBD_PID_COUNT, "OBD_PID_COUNT (CANLine.h) must match the table");

// the PIDs the page on screen shows (obdSetPagePids): they join the fast group. volatile: written by the logic task,
// read by the tasks that poll
#define PAGE_PIDS_MAX 8
static volatile uint8_t pagePids[PAGE_PIDS_MAX];
static volatile uint8_t pagePidCount = 0;
// the PIDs with no gauge signal of their own are only asked for while something wants them: a page shows them, or the
// web UI's Live OBD list is open (obdWantAllPids, within the last OBD_ALL_PIDS_MS). Otherwise they'd slow down the
// values the gauge always shows.
#define OBD_ALL_PIDS_MS 5000
static volatile uint32_t allPidsWantedMs = 0;

// the last value of every PID decoded (any route: wired CAN, K-line, the OBD adapter), for the web UI's Live OBD list.
// Index = PID (0x00 - 0x60). In PSRAM: only the web UI reads it.
#define OBD_VALUE_SLOTS 0x61
static float *pidValues = (float *)psramCalloc(OBD_VALUE_SLOTS, sizeof(float));
static uint32_t *pidValueMs = (uint32_t *)psramCalloc(OBD_VALUE_SLOTS, sizeof(uint32_t)); // when (0 = never)

static void (*pidHook)(uint8_t pid, signalId_t id, float value) = NULL;
void obdSetPidHook(void (*hook)(uint8_t pid, signalId_t id, float value))
{
  pidHook = hook;
}

// the table row for a PID, -1 = not in the table
static int16_t obdTableIndex(uint8_t pid)
{
  for (uint8_t tableIndex = 0; tableIndex < OBD_PID_COUNT; tableIndex++)
  {
    if (obdPidTable[tableIndex].pid == pid)
    {
      return tableIndex;
    }
  }
  return -1;
}

bool obdPidInfo(uint8_t pid, ObdPidInfo &info)
{
  const int16_t tableIndex = obdTableIndex(pid);
  if (tableIndex < 0)
  {
    return false;
  }
  info = obdPidTable[tableIndex];
  return true;
}

bool obdPidInfoAt(uint8_t tableIndex, ObdPidInfo &info)
{
  if (tableIndex >= OBD_PID_COUNT)
  {
    return false;
  }
  info = obdPidTable[tableIndex];
  return true;
}

bool obdPidLatest(uint8_t pid, float &value, uint32_t &ageMs)
{
  if (pid >= OBD_VALUE_SLOTS || pidValueMs[pid] == 0)
  {
    return false;
  }
  value = pidValues[pid];
  ageMs = millis() - pidValueMs[pid];
  return true;
}

void obdWantAllPids()
{
  allPidsWantedMs = millis() | 1; // | 1: never 0, which means "not wanted"
}

// a PID's value from its data bytes, by the table's formula
static float obdFormulaValue(uint8_t formula, uint8_t A, uint8_t B)
{
  const uint16_t AB = (A << 8) | B;
  switch (formula)
  {
  case OBD_F_A: return A;
  case OBD_F_A_MINUS_40: return A - 40.0f;
  case OBD_F_PERCENT: return A * 100.0f / 255.0f;
  case OBD_F_TRIM: return (A - 128.0f) * 100.0f / 128.0f;
  case OBD_F_A_TIMES_3: return A * 3.0f;
  case OBD_F_A_TIMES_10: return A * 10.0f;
  case OBD_F_TIMING: return A / 2.0f - 64.0f;
  case OBD_F_O2_VOLTS: return A / 200.0f;
  case OBD_F_AB: return AB;
  case OBD_F_AB_DIV_4: return AB / 4.0f;
  case OBD_F_AB_DIV_10: return AB / 10.0f;
  case OBD_F_AB_DIV_100: return AB / 100.0f;
  case OBD_F_AB_DIV_1000: return AB / 1000.0f;
  case OBD_F_AB_PERCENT: return AB * 100.0f / 255.0f;
  case OBD_F_LAMBDA: return AB / 32768.0f;
  case OBD_F_CAT_TEMP: return AB / 10.0f - 40.0f;
  case OBD_F_RAIL_VACUUM: return AB * 0.079f;
  case OBD_F_EVAP_QUARTER: return (int16_t)AB / 4.0f; // (int16_t): the two bytes as a signed number (can be below 0)
  case OBD_F_EVAP_ABS: return AB / 200.0f;
  case OBD_F_SIGNED_AB: return (int16_t)AB;
  case OBD_F_INJ_TIMING: return AB / 128.0f - 210.0f;
  case OBD_F_FUEL_RATE: return AB / 20.0f;
  default: return 0;
  }
}

bool obdPidValueOf(uint8_t pid, const uint8_t *data, uint8_t len, float &value)
{
  if (len < obdPidLen(pid))
  {
    return false; // too short for this PID
  }
  const int16_t tableIndex = obdTableIndex(pid);
  if (tableIndex < 0)
  {
    return false; // not a value the gauge knows (PID lists, status bit fields)
  }
  const uint8_t A = data[0];
  uint8_t B = 0;
  if (len > 1)
  {
    B = data[1];
  }
  value = obdFormulaValue(obdPidTable[tableIndex].formula, A, B); // the same maths as the engine's PIDs
  return true;
}

// 0.37.12: the gauge's Throttle comes from the accelerator pedal when the engine has a pedal PID. PID 0x11 is the
// throttle valve: on a diesel it's the intake flap, which sits mostly open whatever the pedal does (bench: X5 F15 30d
// read 85 % all the time). 0x5A (relative pedal, already 0-100 %) is used first, then 0x49 (pedal D). Chosen from the
// engine's PID list in obdSchedulePick; 0 = no pedal PID (or the list isn't known): Throttle stays on 0x11.
static volatile uint8_t throttlePedalPid = 0;
// pedal D reads the sensor's own range (about 15 % at rest, 75-85 % flat out on most cars), so it's stretched to
// 0-100 %: the lowest reading seen is rest, the highest (at least rest + 50) is flat out. Both learn as the car is driven.
static float pedalRestPercent = 100.0f; // lowest pedal D seen since power-on
static float pedalFullPercent = 0.0f;   // highest pedal D seen since power-on

// pedal D (raw %) as 0-100 % of the pedal's travel
static float pedalTravelPercent(float raw)
{
  if (raw < pedalRestPercent)
  {
    pedalRestPercent = raw;
  }
  if (raw > pedalFullPercent)
  {
    pedalFullPercent = raw;
  }
  float full = pedalFullPercent;
  if (full < pedalRestPercent + 50.0f)
  {
    full = pedalRestPercent + 50.0f; // not pressed far yet: a typical travel until it has been
  }
  float travel = (raw - pedalRestPercent) * 100.0f / (full - pedalRestPercent);
  if (travel < 0.0f)
  {
    travel = 0.0f;
  }
  if (travel > 100.0f)
  {
    travel = 100.0f;
  }
  return travel;
}

// which PID feeds the gauge's Throttle for an engine with this PID list (0x11 when it has no pedal PID)
static uint8_t throttlePidFor(const ObdSchedule &schedule)
{
  const bool assumedAll = schedule.supported[0] == 0xFFFFFFFF && schedule.supported[1] == 0xFFFFFFFF &&
                          schedule.supported[2] == 0xFFFFFFFF;
  if (assumedAll)
  {
    return 0x11; // the ECU wouldn't say what it has (obdScheduleAssumeAll): keep the throttle
  }
  if (obdScheduleSupports(schedule, 0x5A))
  {
    return 0x5A; // relative pedal
  }
  if (obdScheduleSupports(schedule, 0x49))
  {
    return 0x49; // pedal D
  }
  return 0x11;
}

bool obdDecodePid(uint8_t pid, const uint8_t *data, uint8_t len, void (*set)(signalId_t, float))
{
  if (len < obdPidLen(pid))
  {
    return false;
  }
  const int16_t tableIndex = obdTableIndex(pid);
  if (tableIndex < 0)
  {
    return false; // not a value the gauge knows (PID lists, status bit fields)
  }
  const uint8_t A = data[0];
  uint8_t B = 0;
  if (len > 1)
  {
    B = data[1];
  }
  const ObdPidInfo info = obdPidTable[tableIndex];
  const float value = obdFormulaValue(info.formula, A, B);
  const uint8_t pedalPid = throttlePedalPid;
  // 0.37.12: on a car where the pedal feeds Throttle, the throttle valve (0x11) doesn't (a page with it still gets it)
  const bool pedalFeedsThrottle = pid == 0x11 && pedalPid != 0;
  if (info.signal != SIG_NONE && !pedalFeedsThrottle)
  {
    set(info.signal, value); // one of the gauge's own signals (rpm, coolant ...)
  }
  if (pedalPid != 0 && pid == pedalPid)
  {
    if (pid == 0x49)
    {
      set(SIG_THROTTLE, pedalTravelPercent(value)); // 0.37.12: pedal D, stretched to the pedal's travel
    }
    else
    {
      set(SIG_THROTTLE, value); // 0.37.12: relative pedal, already 0-100 %
    }
  }
  if (pidHook)
  {
    pidHook(pid, info.signal, value); // gauge pages with an OBD source (can2gauge_UserSig.cpp)
  }
  if (pid < OBD_VALUE_SLOTS)
  {
    pidValues[pid] = value;
    pidValueMs[pid] = millis() | 1; // | 1: never 0, which means "never received"
  }
  return true;
}

uint8_t obdPidLen(uint8_t pid)
{
  // SAE J1979 service 01 data lengths. 0 = not known here (above 0x65 the sources disagree): a multi-PID reply can't
  // be parsed past such a PID, so the caller stops there rather than misread every value after it
  if (pid >= 0x14 && pid <= 0x1B)
  {
    return 2; // O2 sensor voltage + short-term trim
  }
  if (pid >= 0x24 && pid <= 0x2B)
  {
    return 4; // wide-range O2: lambda + voltage
  }
  if (pid >= 0x34 && pid <= 0x3B)
  {
    return 4; // wide-range O2: lambda + current
  }
  if (pid >= 0x3C && pid <= 0x3F)
  {
    return 2; // catalyst temperatures
  }
  if (pid >= 0x55 && pid <= 0x59)
  {
    return 2; // secondary O2 trims, fuel rail absolute pressure
  }
  switch (pid)
  {
  case 0x00:
  case 0x01:
  case 0x20:
  case 0x40:
  case 0x41:
  case 0x4F:
  case 0x50:
  case 0x60:
  case 0x80:
  case 0xA0:
  case 0xC0:
  case 0xE0:
  case 0xA4:
  case 0xA6:
    return 4;
  case 0x02:
  case 0x03:
  case 0x0C:
  case 0x10:
  case 0x1F:
  case 0x21:
  case 0x22:
  case 0x23:
  case 0x31:
  case 0x32:
  case 0x42:
  case 0x43:
  case 0x44:
  case 0x4D:
  case 0x4E:
  case 0x53:
  case 0x54:
  case 0x5D:
  case 0x5E:
  case 0x63:
  case 0x65:
    return 2;
  case 0x64:
    return 5;
  default:
    return pid <= 0x62 ? 1 : 0;
  }
}

// ---------------- OBD-II PID picker (shared with the K-line and the OBD adapter) ----------------
void obdScheduleReset(ObdSchedule &schedule)
{
  memset(&schedule, 0, sizeof(schedule));
}

// listPid 0x00 / 0x20 / 0x40 says which of the three lists; data = its 4 bytes (bit 7 of the first byte = the lowest PID)
void obdScheduleSetList(ObdSchedule &schedule, uint8_t listPid, const uint8_t *data)
{
  const uint8_t block = listPid / 0x20;
  if (listPid % 0x20 != 0 || block >= 3)
  {
    return;
  }
  schedule.supported[block] = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

void obdScheduleAssumeAll(ObdSchedule &schedule)
{
  schedule.supported[0] = 0xFFFFFFFF;
  schedule.supported[1] = 0xFFFFFFFF;
  schedule.supported[2] = 0xFFFFFFFF;
}

bool obdScheduleHasList(const ObdSchedule &schedule)
{
  return schedule.supported[0] != 0 || schedule.supported[1] != 0 || schedule.supported[2] != 0;
}

bool obdScheduleSupports(const ObdSchedule &schedule, uint8_t pid)
{
  if (pid == 0 || pid > 0x60)
  {
    return false;
  }
  const uint8_t block = (pid - 1) / 0x20;
  const uint8_t bit = (pid - 1) % 0x20;
  return (schedule.supported[block] >> (31 - bit)) & 1;
}

uint8_t obdSchedulePick(ObdSchedule &schedule, uint8_t *pids, uint8_t maxPids)
{
  // the page's PIDs, copied once so the page changing half way can't mix two lists
  uint8_t pageList[PAGE_PIDS_MAX];
  uint8_t pageCount = pagePidCount;
  if (pageCount > PAGE_PIDS_MAX)
  {
    pageCount = PAGE_PIDS_MAX;
  }
  for (uint8_t i = 0; i < pageCount; i++)
  {
    pageList[i] = pagePids[i];
  }
  const bool isAllWanted = allPidsWantedMs != 0 && millis() - allPidsWantedMs < OBD_ALL_PIDS_MS;

  // 0.37.12: the gauge's Throttle from the pedal when this engine has a pedal PID (see throttlePedalPid)
  uint8_t pedalPid = throttlePidFor(schedule);
  if (pedalPid == 0x11)
  {
    pedalPid = 0;
  }
  throttlePedalPid = pedalPid;

  // sort the PIDs the ECU has into the fast group and the slow group
  uint8_t fastPids[OBD_PID_COUNT];
  uint8_t fastCount = 0;
  uint8_t slowPids[OBD_PID_COUNT];
  uint8_t slowCount = 0;
  for (uint8_t tableIndex = 0; tableIndex < OBD_PID_COUNT; tableIndex++)
  {
    const uint8_t pid = obdPidTable[tableIndex].pid;
    if (!obdScheduleSupports(schedule, pid))
    {
      continue;
    }
    bool onPage = false;
    for (uint8_t i = 0; i < pageCount; i++)
    {
      if (pageList[i] == pid)
      {
        onPage = true;
      }
    }
    // a PID with no gauge signal: only while a page shows it or the web UI's Live OBD list is open.
    // 0.37.12: with the pedal feeding Throttle, the pedal PID counts as Throttle's (fast) and 0x11 as one with no signal
    bool feedsSignal = obdPidTable[tableIndex].signal != SIG_NONE;
    bool isFast = obdPidTable[tableIndex].alwaysFast;
    if (pedalPid != 0 && pid == pedalPid)
    {
      feedsSignal = true;
      isFast = true;
    }
    if (pedalPid != 0 && pid == 0x11)
    {
      feedsSignal = false;
    }
    if (!feedsSignal && !onPage && !isAllWanted)
    {
      continue;
    }
    if (isFast || onPage)
    {
      fastPids[fastCount] = pid;
      fastCount++;
    }
    else
    {
      slowPids[slowCount] = pid;
      slowCount++;
    }
  }
  if (maxPids == 0 || (fastCount == 0 && slowCount == 0))
  {
    return 0;
  }
  if (maxPids > OBD_MAX_PIDS)
  {
    maxPids = OBD_MAX_PIDS;
  }
  // the lists may have got shorter since last time (another page): bring the rotation back inside them
  if (fastCount > 0)
  {
    schedule.fastIndex = schedule.fastIndex % fastCount;
  }
  if (slowCount > 0)
  {
    schedule.slowIndex = schedule.slowIndex % slowCount;
  }

  if (maxPids == 1 && pageCount > 0)
  {
    // 0.24.1: one PID per request (K-line, J1850) with a page on screen: its PIDs get 3 requests in 4, everything else
    // (the other fast ones, then the slow ones) takes turns in the 4th. Before, the page's PIDs queued with every
    // "always fast" PID, so on an ISO 9141 car through an OBD dongle (5.4 requests/s) the RPM on screen came every
    // 1.25 s; now about 4 times a second.
    uint8_t onPage[OBD_PID_COUNT];
    uint8_t onPageCount = 0;
    uint8_t others[OBD_PID_COUNT];
    uint8_t othersCount = 0;
    for (uint8_t i = 0; i < fastCount; i++)
    {
      bool shown = false;
      for (uint8_t k = 0; k < pageCount; k++)
      {
        if (pageList[k] == fastPids[i])
        {
          shown = true;
        }
      }
      if (shown)
      {
        onPage[onPageCount] = fastPids[i];
        onPageCount++;
      }
      else
      {
        others[othersCount] = fastPids[i];
        othersCount++;
      }
    }
    for (uint8_t i = 0; i < slowCount; i++)
    {
      others[othersCount] = slowPids[i];
      othersCount++;
    }
    if (onPageCount > 0)
    {
      const bool backgroundTurn = schedule.turn == 3;
      schedule.turn = (schedule.turn + 1) % 4;
      if (backgroundTurn && othersCount > 0)
      {
        schedule.backIndex = schedule.backIndex % othersCount;
        pids[0] = others[schedule.backIndex];
        schedule.backIndex = (schedule.backIndex + 1) % othersCount;
      }
      else
      {
        schedule.pageIndex = schedule.pageIndex % onPageCount;
        pids[0] = onPage[schedule.pageIndex];
        schedule.pageIndex = (schedule.pageIndex + 1) % onPageCount;
      }
      return 1;
    }
    // the page's PIDs aren't ones this car has: the usual rotation below
  }

  if (maxPids == 1)
  {
    // one PID per request: the fast ones in turn, and every 4th request a slow one
    const bool slowTurn = schedule.turn == 3;
    schedule.turn = (schedule.turn + 1) % 4;
    if (slowCount > 0 && (slowTurn || fastCount == 0))
    {
      pids[0] = slowPids[schedule.slowIndex];
      schedule.slowIndex = (schedule.slowIndex + 1) % slowCount;
    }
    else
    {
      pids[0] = fastPids[schedule.fastIndex];
      schedule.fastIndex = (schedule.fastIndex + 1) % fastCount;
    }
    return 1;
  }

  // several PIDs per request: as many fast ones as fit, keeping one place for a slow one
  uint8_t count = 0;
  uint8_t fastPlaces = maxPids;
  if (slowCount > 0)
  {
    fastPlaces = maxPids - 1;
  }
  uint8_t fastTake = fastCount;
  if (fastTake > fastPlaces)
  {
    fastTake = fastPlaces;
  }
  for (uint8_t offset = 0; offset < fastTake; offset++)
  {
    pids[count] = fastPids[(schedule.fastIndex + offset) % fastCount];
    count++;
  }
  if (fastCount > fastTake)
  {
    schedule.fastIndex = (schedule.fastIndex + fastTake) % fastCount; // more fast PIDs than places: the window moves on
  }
  // then slow ones in the places left (one, or more when the page has few fast PIDs)
  uint8_t slowTake = maxPids - count;
  if (slowTake > slowCount)
  {
    slowTake = slowCount;
  }
  for (uint8_t offset = 0; offset < slowTake; offset++)
  {
    pids[count] = slowPids[(schedule.slowIndex + offset) % slowCount];
    count++;
  }
  if (slowCount > 0)
  {
    schedule.slowIndex = (schedule.slowIndex + slowTake) % slowCount;
  }
  return count;
}

uint8_t obdPidForSignal(signalId_t id)
{
  if (id == SIG_NONE)
  {
    return 0; // an empty page slot - and most table rows have no signal, so it would match the first of them
  }
  if (id == SIG_THROTTLE && throttlePedalPid != 0)
  {
    return throttlePedalPid; // 0.37.12: this car's Throttle comes from the pedal
  }
  for (uint8_t tableIndex = 0; tableIndex < OBD_PID_COUNT; tableIndex++)
  {
    if (obdPidTable[tableIndex].signal == id)
    {
      return obdPidTable[tableIndex].pid;
    }
  }
  return 0;
}

void obdSetPagePids(const uint8_t *pids, uint8_t count)
{
  if (count > PAGE_PIDS_MAX)
  {
    count = PAGE_PIDS_MAX;
  }
  pagePidCount = 0; // empty while the list is rewritten, so a poll in between sees no page PIDs rather than a mix
  for (uint8_t i = 0; i < count; i++)
  {
    pagePids[i] = pids[i];
  }
  pagePidCount = count;
}

// ---------------- OBD-II polling on the wire (profile OBD2) ----------------
// 1. one "0100" to 0x7DF (every ECU) finds the engine ECU - 0x7E8 when it answers - and its first PID list. Its other
//    lists (0120, 0140) are then asked for straight from it.
// 2. requests go straight to that ECU (its answer id - 8, so 0x7E0) with up to 6 PIDs each. The next request goes out
//    the moment an answer is complete (obdAnswerDone, in the RX task), so the polling runs as fast as the ECU answers.
//    The RX task only watches the clock for an answer that never comes (obdService).
// A 6-PID answer is too long for one CAN frame, so the ECU sends a first frame, waits for our "carry on" (a flow
// control frame), then sends the rest as consecutive frames (ISO 15765-2, "ISO-TP"). Other gauges on the wire put the
// same frames back together and decode them too, without answering.
#define OBD_FUNCTIONAL_ID 0x7DF  // "every ECU" request id
#define OBD_ENGINE_ID 0x7E8      // the engine ECU's answer id under ISO 15765-4 (preferred when several answer)
#define OBD_FIRST_FRAME_MS 60    // an answer must start this soon (ISO 15765-4 P2 is 50 ms)
#define OBD_NEXT_FRAME_MS 150    // and each frame of a long answer follow the last this soon (ISO 15765-2 N_Cr)
#define OBD_SEARCH_RETRY_MS 1000 // nothing answered "0100" (ignition off?): ask again this often
#define OBD_MISSES_TO_SINGLE 3   // bad 6-PID answers in a row before asking one PID at a time
#define OBD_MISSES_TO_SEARCH 20  // missed answers in a row before looking for the ECU again
#define OBD_PAD 0x55             // filler for the unused bytes of a request frame

enum obdStep_t
{
  OBD_STEP_SEARCH = 0, // looking for the engine ECU (0100 to 0x7DF)
  OBD_STEP_LISTS,      // reading its other PID lists (0120, 0140)
  OBD_STEP_POLL,       // asking for values
};

static volatile bool obdWanted = false;  // canSetObdPolling: this gauge asks (volatile: set by the logic task)
static volatile uint16_t obdChosenId = 0;    // canSetObdEcu: the ECU to read (its answer id 0x7E8-0x7EF), 0 = the engine,
                                             // else the first to answer
static volatile bool obdRestartWanted = false; // canSetObdEcu changed the ECU: search again
static volatile bool obdAlongside = false; // canSetObdAlongside: ask for OBD-II PIDs next to a listen profile (VAG PQ /
                                           // MQB / MaxxECU), not only on the OBD-II profile (volatile: set by the logic task)
static bool obdRunning = false;          // the RX task's own copy, so it notices a change and starts over
static uint8_t obdStep = OBD_STEP_SEARCH; // obdStep_t
static ObdSchedule obdSchedule;          // the engine ECU's PID lists and where the rotation is
static uint16_t obdEcuId = 0;            // the engine ECU's answer id (0x7E8), 0 = not found yet
static uint8_t obdListPid = 0;           // OBD_STEP_LISTS: the list being read (0x20, 0x40)
static bool obdWaiting = false;          // a request is out and its answer isn't complete yet
static uint32_t obdLastMs = 0;           // when the request went out, or the last frame of its answer came in
static uint32_t obdRetryMs = 0;          // OBD_STEP_SEARCH: no new "0100" before this
static uint8_t obdAsked[OBD_MAX_PIDS];   // the PIDs in the request that's out
static uint8_t obdAskedCount = 0;
static uint8_t obdMaxPids = OBD_MAX_PIDS; // drops to 1 when the ECU doesn't cope with 6
static uint8_t obdMissesInRow = 0;
static CanObdStats obdStats;
static uint32_t obdAnswersThisSecond = 0;
static uint32_t obdPidsThisSecond = 0;
// a long answer being put back together (one at a time: the ECU answers one request after another)
static uint8_t longAnswer[64];
static uint16_t longAnswerFrom = 0;   // the ECU sending it, 0 = none in progress
static uint16_t longAnswerLength = 0; // bytes in the whole answer
static uint16_t longAnswerHave = 0;   // bytes so far
static uint8_t longAnswerNext = 0;    // sequence number (0..15) the next consecutive frame must carry

// ---------------------------------------------------------------------------
// 0.37.9: the turn to ask a question on the CAN (see CANLine.h)
// ---------------------------------------------------------------------------
static portMUX_TYPE diagTurnMux = portMUX_INITIALIZER_UNLOCKED; // guards the four below (several tasks use them)
static uint8_t diagTurnOwner = CAN_DIAG_NONE;  // who has the turn
static uint32_t diagTurnUntilMs = 0;           // ... until then at the latest (a turn never handed back runs out)
static uint8_t diagTurnWaiter = CAN_DIAG_NONE; // who is waiting for it (gets it next)
static uint32_t diagTurnWaiterMs = 0;          // ... and when it last said so

bool canDiagClaim(uint8_t owner, uint32_t holdMs)
{
  const uint32_t now = millis();
  bool got = false;
  portENTER_CRITICAL(&diagTurnMux);
  bool isFree = diagTurnOwner == CAN_DIAG_NONE || diagTurnOwner == owner || (int32_t)(now - diagTurnUntilMs) >= 0;
  bool otherWaiting = diagTurnWaiter != CAN_DIAG_NONE && diagTurnWaiter != owner && now - diagTurnWaiterMs < 100;
  if (isFree && !otherWaiting)
  {
    diagTurnOwner = owner;
    diagTurnUntilMs = now + holdMs;
    if (diagTurnWaiter == owner)
    {
      diagTurnWaiter = CAN_DIAG_NONE; // it was waiting: not any more
    }
    got = true;
  }
  portEXIT_CRITICAL(&diagTurnMux);
  return got;
}

bool canDiagWait(uint8_t owner, uint32_t holdMs, uint32_t waitMs)
{
  const uint32_t start = millis();
  while (true)
  {
    if (canDiagClaim(owner, holdMs))
    {
      return true;
    }
    portENTER_CRITICAL(&diagTurnMux);
    const bool waiterFree = diagTurnWaiter == CAN_DIAG_NONE || diagTurnWaiter == owner || millis() - diagTurnWaiterMs >= 100;
    if (waiterFree)
    {
      diagTurnWaiter = owner; // next in line: the OBD polling lets this one go first
      diagTurnWaiterMs = millis();
    }
    portEXIT_CRITICAL(&diagTurnMux);
    if (millis() - start >= waitMs)
    {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void canDiagRelease(uint8_t owner)
{
  portENTER_CRITICAL(&diagTurnMux);
  if (diagTurnOwner == owner)
  {
    diagTurnOwner = CAN_DIAG_NONE;
  }
  portEXIT_CRITICAL(&diagTurnMux);
}

// sends one request in a single frame: byte 0 = how many bytes follow, then the request, padded to 8 bytes.
// 0.37.9: only on its turn - another task asking an ECU now means this request waits for the next go
static void obdSendRequest(uint16_t id, const uint8_t *request, uint8_t length)
{
  if (!canDiagClaim(CAN_DIAG_OBD, 1000))
  {
    return; // not our turn: obdWaiting stays false, so obdService asks again on its next go
  }
  uint8_t frame[8] = {OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD};
  frame[0] = length;
  memcpy(frame + 1, request, length);
  canSend(id, frame, 8);
  obdWaiting = true;
  obdLastMs = millis();
  obdStats.requests++;
}

// the next request: straight after an answer, or after a missed one
static void obdSendNext()
{
  obdWaiting = false;
  canDiagRelease(CAN_DIAG_OBD); // 0.37.9: the last question has its answer (or gave up): hand the turn back
  if (!obdRunning)
  {
    return;
  }
  if (obdStep == OBD_STEP_SEARCH)
  {
    if ((int32_t)(millis() - obdRetryMs) < 0)
    {
      return; // obdService tries again when the time comes
    }
    obdRetryMs = millis() + OBD_SEARCH_RETRY_MS;
    uint8_t request[2];
    request[0] = 0x01; // service 01: current data
    request[1] = 0x00; // PID 00: "which PIDs 01-20 do you have"
    obdAskedCount = 0;
    obdSendRequest(OBD_FUNCTIONAL_ID, request, 2);
    return;
  }
  if (obdStep == OBD_STEP_LISTS)
  {
    uint8_t request[2];
    request[0] = 0x01;       // service 01
    request[1] = obdListPid; // PID 20 / 40: the next PID list
    obdAskedCount = 0;
    obdSendRequest(obdEcuId - 8, request, 2);
    return;
  }
  obdAskedCount = obdSchedulePick(obdSchedule, obdAsked, obdMaxPids);
  if (obdAskedCount == 0)
  {
    // the ECU has none of the PIDs the gauge decodes: look again in a while
    obdStep = OBD_STEP_SEARCH;
    obdRetryMs = millis() + OBD_SEARCH_RETRY_MS;
    return;
  }
  uint8_t request[1 + OBD_MAX_PIDS];
  request[0] = 0x01; // service 01, then the PIDs
  memcpy(request + 1, obdAsked, obdAskedCount);
  obdSendRequest(obdEcuId - 8, request, 1 + obdAskedCount);
}

// a request went unanswered, or came back with fewer PIDs than asked
static void obdMiss()
{
  obdStats.misses++;
  obdMissesInRow++;
  if (obdMaxPids > 1 && obdMissesInRow >= OBD_MISSES_TO_SINGLE)
  {
    obdMaxPids = 1; // this ECU doesn't cope with several PIDs per request: one at a time from now on
    obdMissesInRow = 0;
  }
  else if (obdMissesInRow >= OBD_MISSES_TO_SEARCH)
  {
    obdStep = OBD_STEP_SEARCH; // the ECU has gone quiet (ignition off?): find it again
    obdEcuId = 0;
    obdMaxPids = OBD_MAX_PIDS;
    obdMissesInRow = 0;
  }
}

// our request has its complete answer: on to the next one at once
static void obdAnswerDone(uint8_t askedDecoded)
{
  obdStats.answers++;
  obdAnswersThisSecond++;
  if (obdStep == OBD_STEP_POLL && obdAskedCount > 1 && askedDecoded < obdAskedCount)
  {
    obdMiss(); // fewer PIDs back than asked: the ECU may not take 6 at a time
  }
  else
  {
    obdMissesInRow = 0;
  }
  obdSendNext();
}

// a PID list (the answer to 0100 / 0120 / 0140). ours = the answer to our own request. Returns true when it moved
// the polling on (a list was taken).
static bool obdTakeList(uint16_t fromId, uint8_t listPid, const uint8_t *data, bool ours)
{
  if (listPid == 0x00 && fromId >= 0x7E8 && fromId <= 0x7EF)
  {
    obdStats.answeredMask = obdStats.answeredMask | (1 << (fromId - 0x7E8)); // this ECU speaks OBD-II
  }
  // 0x7E8 answering after another ECU got in first: the engine ECU takes over (only when no ECU is chosen)
  bool engineTakesOver = false;
  if (obdChosenId == 0 && listPid == 0x00 && fromId == OBD_ENGINE_ID && obdEcuId != 0 && obdEcuId != OBD_ENGINE_ID)
  {
    engineTakesOver = true;
  }
  if (engineTakesOver || (ours && obdStep == OBD_STEP_SEARCH && listPid == 0x00))
  {
    obdEcuId = fromId;
    obdScheduleReset(obdSchedule);
    obdScheduleSetList(obdSchedule, 0x00, data);
  }
  else if (ours && obdStep == OBD_STEP_LISTS && listPid == obdListPid)
  {
    obdScheduleSetList(obdSchedule, listPid, data);
  }
  else
  {
    return false;
  }
  // bit 0 of the list's last byte = "the next list exists too"
  if ((data[3] & 0x01) && listPid < 0x40)
  {
    obdStep = OBD_STEP_LISTS;
    obdListPid = listPid + 0x20;
  }
  else
  {
    obdStep = OBD_STEP_POLL;
  }
  return true;
}

// one complete answer from an ECU (a single frame, or a long answer put back together). Every gauge decodes the
// values; the gauge that asked also moves on to its next request. fromWire = false for a frame relayed over WiFi: that
// is only decoded.
static void obdAnswer(uint16_t fromId, const uint8_t *payload, uint16_t length, bool fromWire)
{
  bool ours = false;
  if (fromWire && obdRunning && obdWaiting)
  {
    ours = fromId == obdEcuId;
    if (obdStep == OBD_STEP_SEARCH && (obdChosenId == 0 || fromId == obdChosenId))
    {
      ours = true; // while searching, any ECU's answer counts - or only the chosen one's
    }
  }
  if (length >= 3 && payload[0] == 0x7F && payload[1] == 0x01)
  {
    // negative answer to service 01: [7F][01][reason]
    if (ours && payload[2] == 0x78)
    {
      obdLastMs = millis(); // 0x78 = "still working on it": keep waiting
    }
    else if (ours)
    {
      obdMiss();
      obdSendNext();
    }
    return;
  }
  if (length < 1 || payload[0] != 0x41)
  {
    return; // not a service 01 answer
  }
  // [41] then [pid][data] pairs
  uint8_t askedDecoded = 0;
  bool movedOn = false;
  uint16_t position = 1;
  while (position < length)
  {
    const uint8_t pid = payload[position];
    const uint8_t pidLength = obdPidLen(pid);
    if (pidLength == 0 || position + 1 + pidLength > length)
    {
      break; // a PID whose length isn't known: can't read past it
    }
    const uint8_t *data = &payload[position + 1];
    if (pid == 0x00 || pid == 0x20 || pid == 0x40)
    {
      if (fromWire && obdRunning && obdTakeList(fromId, pid, data, ours))
      {
        movedOn = true;
      }
    }
    else if (obdDecodePid(pid, data, pidLength, signalSetBus))
    {
      obdPidsThisSecond++;
      for (uint8_t askedIndex = 0; askedIndex < obdAskedCount; askedIndex++)
      {
        if (obdAsked[askedIndex] == pid)
        {
          askedDecoded++;
        }
      }
    }
    position += 1 + pidLength;
  }
  if (ours || movedOn)
  {
    obdAnswerDone(askedDecoded);
  }
}

// a frame from 0x7E8..0x7EF (an ECU's OBD answer). Single frames are decoded at once; long answers are put back
// together first. air = relayed over WiFi: decoded only when it's a single frame, never answered.
static void decodeObd(const twai_message_t &message, bool air)
{
  if (message.identifier < 0x7E8 || message.identifier > 0x7EF || message.data_length_code < 2)
  {
    return;
  }
  const uint16_t fromId = message.identifier;
  const uint8_t frameType = message.data[0] >> 4; // ISO-TP: 0 = single frame, 1 = first frame, 2 = consecutive frame
  if (frameType == 0)
  {
    const uint8_t length = message.data[0] & 0x0F; // bytes that follow
    if (length >= 1 && length < message.data_length_code)
    {
      obdAnswer(fromId, &message.data[1], length, !air);
    }
    return;
  }
  if (air || message.data_length_code < 8)
  {
    return;
  }
  if (frameType == 1)
  {
    // first frame: [1 + length high nibble][length low byte][6 bytes]
    const uint16_t total = ((message.data[0] & 0x0F) << 8) | message.data[1];
    if (total < 8 || total > sizeof(longAnswer))
    {
      longAnswerFrom = 0;
      return;
    }
    longAnswerFrom = fromId;
    longAnswerLength = total;
    memcpy(longAnswer, &message.data[2], 6);
    longAnswerHave = 6;
    longAnswerNext = 1;
    // the gauge that asked tells the ECU to send the rest
    if (obdRunning && obdWaiting && fromId == obdEcuId)
    {
      uint8_t flowControl[8] = {OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD, OBD_PAD};
      flowControl[0] = 0x30; // flow control: carry on sending
      flowControl[1] = 0x00; // block size 0: all of it, no more flow controls
      flowControl[2] = 0x00; // no gap needed between the frames
      canSend(obdEcuId - 8, flowControl, 8);
      obdLastMs = millis();
    }
    return;
  }
  if (frameType == 2 && fromId == longAnswerFrom)
  {
    // consecutive frame: [2 + sequence number][7 bytes]
    if ((message.data[0] & 0x0F) != longAnswerNext)
    {
      longAnswerFrom = 0; // a frame went missing: drop this answer (the timeout moves the polling on)
      return;
    }
    uint16_t take = longAnswerLength - longAnswerHave;
    if (take > 7)
    {
      take = 7;
    }
    memcpy(longAnswer + longAnswerHave, &message.data[1], take);
    longAnswerHave += take;
    longAnswerNext = (longAnswerNext + 1) & 0x0F;
    if (obdRunning && obdWaiting && fromId == obdEcuId)
    {
      obdLastMs = millis();
    }
    if (longAnswerHave >= longAnswerLength)
    {
      longAnswerFrom = 0;
      obdAnswer(fromId, longAnswer, longAnswerLength, true);
    }
  }
}

// called by the RX task after every frame and after every empty wait (at most a few ms apart while polling): starts
// and stops the polling, and moves on when an answer doesn't come
static void obdService()
{
  bool obdOnThisBus = false; // the OBD-II profile, or OBD-II switched on next to the car's own profile
  if (profile == CAN_PROFILE_OBD2 || obdAlongside)
  {
    obdOnThisBus = true;
  }
  const bool wantedNow = obdWanted && obdOnThisBus && stats.up && !listenOnly;
  if (obdRestartWanted)
  {
    obdRestartWanted = false;
    obdRunning = !wantedNow; // forces the start-over below
  }
  if (wantedNow != obdRunning)
  {
    obdRunning = wantedNow;
    obdWaiting = false;
    longAnswerFrom = 0;
    obdStep = OBD_STEP_SEARCH;
    obdEcuId = 0;
    obdMaxPids = OBD_MAX_PIDS;
    obdMissesInRow = 0;
    obdRetryMs = 0;
  }
  if (!obdRunning)
  {
    canDiagRelease(CAN_DIAG_OBD); // 0.37.9: stopped: the turn goes back
    return;
  }
  if (!obdWaiting)
  {
    obdSendNext(); // the first request, or the next search once its retry time has come
    return;
  }
  uint32_t limit = OBD_FIRST_FRAME_MS;
  if (longAnswerFrom != 0)
  {
    limit = OBD_NEXT_FRAME_MS;
  }
  if (millis() - obdLastMs < limit)
  {
    return;
  }
  // no (complete) answer in time
  longAnswerFrom = 0;
  if (obdStep == OBD_STEP_LISTS)
  {
    obdStep = OBD_STEP_POLL; // a later list didn't come: poll with the lists there are
  }
  else if (obdStep == OBD_STEP_POLL)
  {
    obdMiss();
  }
  obdSendNext();
}

void canSetObdPolling(bool on)
{
  obdWanted = on;
}

void canSetObdAlongside(bool on)
{
  obdAlongside = on;
}

void canSetObdEcu(uint16_t answerId)
{
  if (answerId != 0 && (answerId < 0x7E8 || answerId > 0x7EF))
  {
    answerId = 0; // not an OBD-II answer id: the engine
  }
  if (answerId != obdChosenId)
  {
    obdChosenId = answerId;
    obdRestartWanted = true;
  }
}

const CanObdStats &canObdStats()
{
  return obdStats;
}

// ---------------- transport ----------------
// one id slot per 11-bit id; 29-bit ids are folded in (a clash only means a relayed copy waits for the hold time)
static inline uint16_t idSlot(const twai_message_t &message)
{
  const uint32_t id = message.identifier;
  return (message.extd ? (id ^ (id >> 11) ^ (id >> 22)) : id) & 0x7FF;
}

// may source (0 = wire) deliver this id? The wire always may; a relay sender while nobody else has sent it lately
static bool claimId(const twai_message_t &message, uint8_t source)
{
  const uint16_t slot = idSlot(message);
  const uint16_t now = (uint16_t)millis();
  const uint8_t owner = idOwner[slot];
  if (source != 0 && owner != source && owner != 0xFF && (uint16_t)(now - idOwnerMs[slot]) < CAN_AIR_HOLD_MS)
  {
    return false;
  }
  idOwner[slot] = source;
  idOwnerMs[slot] = now;
  return true;
}

// one frame through the ring log, the decoders and the handlers - wired (RX task) or relayed (air task)
static void processFrame(const twai_message_t &message, bool air, uint32_t now)
{
  if (decodeMutex)
  {
    xSemaphoreTake(decodeMutex, portMAX_DELAY);
  }
  portENTER_CRITICAL(&ringMux);
  CanFrameLog &entry = ring[ringHead];
  entry.id = message.identifier;
  entry.len = message.data_length_code;
  memcpy(entry.data, message.data, 8);
  entry.ms = now;
  entry.air = air;
  ringHead = (ringHead + 1) & 15;
  portEXIT_CRITICAL(&ringMux);

  if (!air)
  {
    decodeOpenHaldex(message); // relayed: the OpenHaldex state comes in its own packets, and this would claim "heard on CAN"
  }
  if (!message.extd)
  {
    if (profile == CAN_PROFILE_VAG_PQ)
    {
      decodeVagPq(message);
    }
    else if (profile == CAN_PROFILE_VAG_MQB)
    {
      decodeVagMqb(message);
    }
    else if (profile == CAN_PROFILE_VAG_MLB)
    {
      decodeVagMlb(message); // 0.26.0
    }
    else if (profile == CAN_PROFILE_MAXXECU)
    {
      decodeMaxx(message);
    }
    else if (profile == CAN_PROFILE_OBD2)
    {
      decodeObd(message, air);
    }
    if (profile != CAN_PROFILE_OBD2 && obdAlongside)
    {
      decodeObd(message, air); // OBD-II answers (0x7E8-0x7EF) as well as the car's own frames
    }
  }
  for (uint8_t handlerIndex = 0; handlerIndex < handlerCount; handlerIndex++)
  {
    if (!air || handlerAir[handlerIndex])
    {
      handlers[handlerIndex](message);
    }
  }
  if (decodeMutex)
  {
    xSemaphoreGive(decodeMutex);
  }
}

static void airTaskFn(void *)
{
  AirFrame frame;
  uint32_t secondStartMs = millis();
  uint32_t framesThisSecond = 0;
  for (;;)
  {
    if (xQueueReceive(airQueue, &frame, pdMS_TO_TICKS(200)) == pdTRUE)
    {
      if (claimId(frame.message, frame.source))
      {
        airStats.rx++;
        framesThisSecond++;
        airStats.lastRxMs = millis();
        processFrame(frame.message, true, airStats.lastRxMs);
      }
      else
      {
        airStats.ignored++;
      }
    }
    if (millis() - secondStartMs >= 1000)
    {
      airStats.perSec = framesThisSecond;
      framesThisSecond = 0;
      secondStartMs = millis();
    }
  }
}

void canAirBegin()
{
  if (airQueue)
  {
    return;
  }
  memset(idOwner, 0xFF, ID_SLOTS * sizeof(uint8_t)); // nobody owns any id yet
  decodeMutex = xSemaphoreCreateMutex();
  // the relayed-frame queue (192 frames, ~4.6 KB) in PSRAM: it's filled by the ESP-NOW receive callback and emptied by
  // the canAir task, never from an interrupt, so it doesn't need internal RAM
  airQueue = xQueueCreateWithCaps(AIR_QUEUE, sizeof(AirFrame), MALLOC_CAP_SPIRAM);
  if (!airQueue)
  {
    airQueue = xQueueCreate(AIR_QUEUE, sizeof(AirFrame)); // no PSRAM: internal RAM, as before
  }
  // the canAir task's stack in PSRAM too: it only decodes frames and never reads or writes flash (a task with its
  // stack in PSRAM must not - flash access switches PSRAM off for a moment)
  xTaskCreatePinnedToCoreWithCaps(airTaskFn,         // task function
                                  "canAir",          // name (shows in crash reports)
                                  4096,              // stack size in bytes
                                  NULL,              // no parameter
                                  5,                 // priority
                                  NULL,              // no handle kept
                                  0,                 // core 0 (LVGL has core 1)
                                  MALLOC_CAP_SPIRAM); // stack in PSRAM
}

bool canInject(const twai_message_t &message, uint8_t source)
{
  if (!airQueue || source == 0 || source > CAN_AIR_SOURCES)
  {
    return false;
  }
  AirFrame frame;
  frame.message = message;
  frame.source = source;
  if (xQueueSend(airQueue, &frame, 0) == pdTRUE)
  {
    return true;
  }
  airStats.dropped++;
  return false;
}

const CanAirStats &canAirStats()
{
  return airStats;
}

static bool driverInstall();

static void rxTaskFn(void *)
{
  twai_message_t message;
  uint32_t secondStartMs = millis();
  uint32_t framesThisSecond = 0;
  uint32_t retryMs = 0;
  for (;;)
  {
    if (!stats.up)
    {
      // driver stopped / uninstalled (canEnd): twai_receive would fail instantly and spin this priority-6 task
      rxParked = true;
      // canBegin failed (e.g. canEnd left a driver mid bus-off recovery): try again, never holding up canBegin / canEnd
      if (beginRetry && millis() - retryMs >= 1000 && xSemaphoreTake(driverMutex, 0) == pdTRUE)
      {
        retryMs = millis();
        if (beginRetry && !stats.up)
        {
          beginRetry = !driverInstall();
        }
        xSemaphoreGive(driverMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    rxParked = false;
    // while OBD polling, wake at least every 5 ms so a missing answer is noticed quickly (obdService)
    TickType_t waitTicks = pdMS_TO_TICKS(100);
    if (obdRunning || obdWanted)
    {
      waitTicks = pdMS_TO_TICKS(5);
    }
    if (twai_receive(&message, waitTicks) == ESP_OK)
    {
      stats.rx++;
      framesThisSecond++;
      stats.lastRxMs = millis();
      claimId(message, 0);
      processFrame(message, false, stats.lastRxMs);
    }
    obdService();
    if (millis() - secondStartMs >= 1000)
    {
      stats.rxPerSec = framesThisSecond;
      framesThisSecond = 0;
      obdStats.answersPerSec = obdAnswersThisSecond;
      obdStats.pidsPerSec = obdPidsThisSecond;
      obdAnswersThisSecond = 0;
      obdPidsThisSecond = 0;
      obdStats.ecuFound = obdEcuId != 0;
      obdStats.ecuId = obdEcuId;
      obdStats.pidsPerRequest = obdMaxPids;
      secondStartMs = millis();
      twai_status_info_t status;
      if (twai_get_status_info(&status) == ESP_OK)
      {
        stats.err = status.bus_error_count;
        if (status.state == TWAI_STATE_BUS_OFF)
        {
          stats.busOff++;
          twai_initiate_recovery();
        }
        else if (status.state == TWAI_STATE_STOPPED)
        {
          twai_start();
        }
      }
    }
  }
}

// twai_stop() needs RUNNING and twai_driver_uninstall() needs STOPPED or BUS_OFF, so a controller in the middle of bus-off
// recovery (RECOVERING) refuses both. Recovery needs 128 runs of 11 recessive bits - one per frame gap on a busy bus - so
// wait for it. A bus held dominant (wiring fault) keeps it there: the driver stays installed and the next
// driverInstall() (canBegin, or the RX task's retry) tries again. Called under driverMutex.
static void driverRemove()
{
  if (!driverInstalled)
  {
    return;
  }
  twai_status_info_t status;
  const uint32_t startMs = millis();
  while (twai_get_status_info(&status) == ESP_OK && status.state == TWAI_STATE_RECOVERING && millis() - startMs < 300)
  {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  twai_stop();
  if (twai_driver_uninstall() == ESP_OK)
  {
    driverInstalled = false;
  }
}

// under driverMutex
static bool driverInstall()
{
  driverRemove(); // one canEnd could not remove
  if (driverInstalled)
  {
    return false;
  }
  twai_general_config_t generalConfig =
      TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)busTxPin, (gpio_num_t)busRxPin, listenOnly ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
  generalConfig.rx_queue_len = 64;
  generalConfig.tx_queue_len = 16;
  twai_timing_config_t timing250 = TWAI_TIMING_CONFIG_250KBITS();
  twai_timing_config_t timing500 = TWAI_TIMING_CONFIG_500KBITS();
  twai_timing_config_t timing1000 = TWAI_TIMING_CONFIG_1MBITS();
  twai_timing_config_t timing125 = TWAI_TIMING_CONFIG_125KBITS();
  twai_timing_config_t timing;
  if (busKbps == 250)
  {
    timing = timing250;
  }
  else if (busKbps == 1000)
  {
    timing = timing1000;
  }
  else if (busKbps == 125)
  {
    timing = timing125;
  }
  else
  {
    timing = timing500;
  }
  twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&generalConfig, &timing, &filter) != ESP_OK)
  {
    return false;
  }
  driverInstalled = true;
  if (twai_start() != ESP_OK)
  {
    driverRemove();
    return false;
  }
  memset(&stats, 0, sizeof(stats));
  stats.up = true;
  return true;
}

bool canBegin(int8_t txPin, int8_t rxPin, uint16_t kbps)
{
  if (!driverMutex)
  {
    driverMutex = xSemaphoreCreateMutex(); // the first canBegin is from setup, before any other task touches the bus
  }
  xSemaphoreTake(driverMutex, portMAX_DELAY);
  if (stats.up) // already running: refuse rather than pull the driver out from under the RX task
  {
    xSemaphoreGive(driverMutex);
    return false;
  }
  busTxPin = txPin;
  busRxPin = rxPin;
  busKbps = kbps;
  const bool ok = driverInstall();
  beginRetry = !ok;
  if (!rxTask) // started even when the install failed: the task does the retry
  {
    // stack in PSRAM (saves internal RAM): this task and the frame hooks it runs (power, link, DSG, CAN monitor,
    // adc2can, gauge pages) only decode frames and talk to the CAN controller - none of them reads or writes flash
    xTaskCreatePinnedToCoreWithCaps(rxTaskFn,          // task function
                                    "canRx",           // name (shows in crash reports)
                                    4096,              // stack size in bytes
                                    NULL,              // no parameter
                                    6,                 // priority (above the other CAN users)
                                    &rxTask,           // handle, so it is only started once
                                    0,                 // core 0 (LVGL has core 1)
                                    MALLOC_CAP_SPIRAM); // stack in PSRAM
  }
  xSemaphoreGive(driverMutex);
  return ok;
}

void canEnd()
{
  if (!driverMutex)
  {
    return; // never begun
  }
  xSemaphoreTake(driverMutex, portMAX_DELAY);
  beginRetry = false;
  if (stats.up)
  {
    stats.up = false;
    // let the RX task leave twai_receive (100 ms timeout) before the driver and its queue go away
    const uint32_t startMs = millis();
    while (rxTask && !rxParked && millis() - startMs < 300)
    {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  driverRemove();
  xSemaphoreGive(driverMutex);
}

// Sending onto a bus nobody is on: a CAN frame needs another node to acknowledge it, and without one the controller
// resends the same frame for ever, counting a bus error each time (2026-10-14, on the bench with no car: the error
// count climbing non-stop). But the gauge can't just wait to hear the car first: on a gateway car the OBD port is
// silent until it's asked something. So while nothing has been heard for BUS_QUIET_MS, one frame a second is let
// through as a "knock", sent once only (single-shot: no resending). A car answers it and everything runs as normal from
// then on; an empty bus costs one error a second.
#define BUS_QUIET_MS 5000   // nothing received for this long: the bus counts as quiet
#define QUIET_KNOCK_MS 1000 // while quiet: one frame through per second
static uint32_t lastKnockMs = 0; // when the last knock went out

bool canSend(uint32_t id, const uint8_t *data, uint8_t len, bool extended)
{
  if (!stats.up || listenOnly)
  {
    return false;
  }
  const uint32_t now = millis();
  const bool isBusQuiet = stats.lastRxMs == 0 || now - stats.lastRxMs > BUS_QUIET_MS;
  if (isBusQuiet)
  {
    if (now - lastKnockMs < QUIET_KNOCK_MS)
    {
      return false; // held back: a knock already went out this second (not an error - nothing was tried)
    }
    lastKnockMs = now;
  }
  twai_message_t message = {};
  message.identifier = id;
  message.extd = extended;
  if (isBusQuiet)
  {
    message.ss = 1; // single-shot: tried once, never resent if nobody acknowledges it
  }
  message.data_length_code = len > 8 ? 8 : len;
  memcpy(message.data, data, message.data_length_code);
  if (twai_transmit(&message, pdMS_TO_TICKS(10)) == ESP_OK)
  {
    stats.tx++;
    return true;
  }
  stats.err++;
  return false;
}

bool canOnFrame(canFrameCb_t callback, bool air)
{
  if (handlerCount >= MAX_HANDLERS)
  {
    return false;
  }
  handlerAir[handlerCount] = air;
  handlers[handlerCount++] = callback;
  return true;
}

const CanStats &canStats()
{
  return stats;
}

void canSetMaxxBase(uint16_t base)
{
  maxxBase = base;
}

void canSetListenOnly(bool on)
{
  if (on == listenOnly)
  {
    return;
  }
  listenOnly = on;
  if (stats.up) // restart the driver in the new mode (the RX task parks while it is down)
  {
    canEnd();
    canBegin(busTxPin, busRxPin, busKbps);
  }
}

bool canIsListenOnly()
{
  return listenOnly;
}

void canSetProfile(uint8_t newProfile)
{
  profile = newProfile;
}

void canSetHaldexIds(uint16_t broadcastId, uint16_t controlId)
{
  haldexBroadcastId = broadcastId;
  haldexControlId = controlId;
}

bool canHaldexSetMode(uint8_t mode)
{
  uint8_t data[8] = {mode, 0, 0, 0, 0, 0, 0, 0};
  return canSend(haldexControlId, data, 8);
}

uint8_t canRecentFrames(CanFrameLog *out, uint8_t maxFrames)
{
  uint8_t count = 0;
  portENTER_CRITICAL(&ringMux);
  for (uint8_t age = 0; age < 16 && count < maxFrames; age++)
  {
    const CanFrameLog &entry = ring[(ringHead + 15 - age) & 15];
    if (entry.ms == 0)
    {
      break;
    }
    out[count++] = entry;
  }
  portEXIT_CRITICAL(&ringMux);
  return count;
}
