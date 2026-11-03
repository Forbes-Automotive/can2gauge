#include <can2gauge_defs.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_Units.h> // 0.27.0: unitShow() - a unit per quantity
#include <can2gauge_UserSig.h> // 0.31.0: userSigDecimals() for signalDecimals
#include <can2gauge_ADC2CAN.h> // 0.31.0: adc2canDecimals() for signalDecimals

// table order MUST match signalId_t
static const SignalInfo signalTable[] = {
    {"none", "-", "", 0, 1, 0, 0},
    {"rpm", "RPM", "rpm", 0, 8000, 6800, engineTimeout},
    {"speed", "Speed", "km/h", 0, 280, 0, engineTimeout}, // 280: round 40 km/h scale steps
    {"coolant", "Coolant", "C", 0, 140, 110, engineTimeout},
    {"iat", "Intake", "C", -20, 80, 60, engineTimeout},
    {"oiltemp", "Oil temp", "C", 0, 160, 130, engineTimeout},
    {"map", "Boost abs", "mbar", 0, 2500, 2200, engineTimeout},
    {"boost", "Boost", "bar", -1, 2, 1.6, engineTimeout},
    {"pedal", "Pedal", "%", 0, 100, 0, engineTimeout},
    {"throttle", "Throttle", "%", 0, 100, 0, engineTimeout},
    {"load", "Load", "%", 0, 100, 0, engineTimeout},
    {"maf", "MAF", "g/s", 0, 300, 0, engineTimeout},
    {"lambda", "Lambda", "", 0.6, 1.4, 0, engineTimeout},
    {"timing", "Timing", "deg", -10, 40, 0, engineTimeout},
    {"batt", "Battery", "V", 10, 16, 15, engineTimeout},
    {"fuel", "Fuel", "%", 0, 100, 0, engineTimeout},
    {"gear", "Gear", "", 0, 7, 0, engineTimeout},
    {"hdxTarget", "Lock target", "%", 0, 100, 0, haldexTimeout},
    {"hdxActual", "Haldex lock", "%", 0, 100, 0, haldexTimeout},
    {"hdxMode", "Haldex mode", "", 0, 5, 0, haldexTimeout},
    {"hdxSpeed", "Haldex speed", "km/h", 0, 280, 0, haldexTimeout},
    {"hdxPedal", "Haldex pedal", "%", 0, 100, 0, haldexTimeout},
    {"lights", "Dash lights", "", 0, 1, 0, 1000},
    {"output", "Output", "", 0, 1, 0, 1000},
    {"outFault", "Output fault", "", 0, 1, 0, 1000},
    // ME7 1-8: nothing fills these any more (since 0.17.6 an ME7 variable is a page value, "me7:<name>"). Kept so the
    // signals after them keep their numbers; the web UI leaves them out of its lists (0.35.0)
    {"me7_1", "ME7 1", "", 0, 100, 0, 3000},
    {"me7_2", "ME7 2", "", 0, 100, 0, 3000},
    {"me7_3", "ME7 3", "", 0, 100, 0, 3000},
    {"me7_4", "ME7 4", "", 0, 100, 0, 3000},
    {"me7_5", "ME7 5", "", 0, 100, 0, 3000},
    {"me7_6", "ME7 6", "", 0, 100, 0, 3000},
    {"me7_7", "ME7 7", "", 0, 100, 0, 3000},
    {"me7_8", "ME7 8", "", 0, 100, 0, 3000},
    {"boardTemp", "Board temp", "C", 0, 100, 80, 0},
    {"uptime", "Uptime", "s", 0, 1, 0, 0},
    {"selector", "Selector", "", 0, 9, 0, engineTimeout},
    {"tcuSpeed", "DSG speed", "km/h", 0, 280, 0, 3000},
    {"gboxTemp", "Gearbox temp", "C", 0, 150, 120, engineTimeout},
    {"ambient", "Outside temp", "C", -30, 50, 0, 5000},
    {"oilLevel", "Oil level", "", 0, 1, 0, 5000},
    {"oilPress", "Oil pressure", "", 0, 1, 0, engineTimeout},
    // gauge pages from a chosen source: named / given units at runtime (signalSetLabel)
    {"user1", "User 1", "", 0, 100, 0, 3000},
    {"user2", "User 2", "", 0, 100, 0, 3000},
    {"user3", "User 3", "", 0, 100, 0, 3000},
    {"user4", "User 4", "", 0, 100, 0, 3000},
    {"user5", "User 5", "", 0, 100, 0, 3000},
    {"user6", "User 6", "", 0, 100, 0, 3000},
    {"user7", "User 7", "", 0, 100, 0, 3000},
    {"user8", "User 8", "", 0, 100, 0, 3000},
    {"user9", "User 9", "", 0, 100, 0, 3000},
    {"user10", "User 10", "", 0, 100, 0, 3000},
    {"user11", "User 11", "", 0, 100, 0, 3000},
    {"user12", "User 12", "", 0, 100, 0, 3000},
    {"user13", "User 13", "", 0, 100, 0, 3000},
    {"user14", "User 14", "", 0, 100, 0, 3000},
    {"user15", "User 15", "", 0, 100, 0, 3000},
    {"user16", "User 16", "", 0, 100, 0, 3000},
    {"user17", "User 17", "", 0, 100, 0, 3000},
    {"user18", "User 18", "", 0, 100, 0, 3000},
    {"user19", "User 19", "", 0, 100, 0, 3000},
    {"user20", "User 20", "", 0, 100, 0, 3000},
    {"user21", "User 21", "", 0, 100, 0, 3000},
    {"user22", "User 22", "", 0, 100, 0, 3000},
    {"user23", "User 23", "", 0, 100, 0, 3000},
    {"user24", "User 24", "", 0, 100, 0, 3000},
    {"user25", "User 25", "", 0, 100, 0, 3000},
    {"user26", "User 26", "", 0, 100, 0, 3000},
    {"user27", "User 27", "", 0, 100, 0, 3000},
    {"user28", "User 28", "", 0, 100, 0, 3000},
    {"user29", "User 29", "", 0, 100, 0, 3000},
    {"user30", "User 30", "", 0, 100, 0, 3000},
    {"user31", "User 31", "", 0, 100, 0, 3000},
    {"user32", "User 32", "", 0, 100, 0, 3000},
    {"user33", "User 33", "", 0, 100, 0, 3000},
    {"user34", "User 34", "", 0, 100, 0, 3000},
    {"user35", "User 35", "", 0, 100, 0, 3000},
    {"user36", "User 36", "", 0, 100, 0, 3000},
    {"user37", "User 37", "", 0, 100, 0, 3000},
    {"user38", "User 38", "", 0, 100, 0, 3000},
    {"user39", "User 39", "", 0, 100, 0, 3000},
    {"user40", "User 40", "", 0, 100, 0, 3000},
    // adc2can sensors: named / given units by the adc2can (or the gauge's ADC tab)
    {"adc1", "ADC 1", "V", 0, 5.5, 0, 1000},
    {"adc2", "ADC 2", "V", 0, 5.5, 0, 1000},
    {"adc3", "ADC 3", "V", 0, 5.5, 0, 1000},
    {"adc4", "ADC 4", "V", 0, 5.5, 0, 1000},
    {"adc5", "ADC 5", "V", 0, 5.5, 0, 1000},
    {"adc6", "ADC 6", "V", 0, 5.5, 0, 1000},
    {"adc7", "ADC 7", "V", 0, 5.5, 0, 1000},
    {"adc8", "ADC 8", "V", 0, 5.5, 0, 1000},
    {"adc9", "ADC 9", "V", 0, 5.5, 0, 1000},
    {"adc10", "ADC 10", "V", 0, 5.5, 0, 1000},
    {"adc11", "ADC 11", "V", 0, 5.5, 0, 1000},
    {"adc12", "ADC 12", "V", 0, 5.5, 0, 1000},
    {"adc13", "ADC 13", "V", 0, 5.5, 0, 1000},
    {"adc14", "ADC 14", "V", 0, 5.5, 0, 1000},
    {"adc15", "ADC 15", "V", 0, 5.5, 0, 1000},
    {"adc16", "ADC 16", "V", 0, 5.5, 0, 1000},
    // OpenHaldex over WiFi: live diagnostics (its Live Diagnostics option on)
    {"hdxClutchTemp", "Haldex clutch", "C", 0, 150, 120, haldexTimeout},
    {"hdxOilTemp", "Haldex oil", "C", 0, 150, 120, haldexTimeout},
    {"hdxVolts", "Haldex supply", "V", 10, 16, 0, haldexTimeout},
    // aftermarket ECUs (MaxxECU)
    {"egt", "EGT", "C", 0, 1100, 950, engineTimeout},
    {"oilPressure", "Oil pressure", "bar", 0, 8, 0, engineTimeout},
    {"fuelPressure", "Fuel pressure", "bar", 0, 8, 0, engineTimeout},
    {"ethanol", "Ethanol", "%", 0, 100, 0, engineTimeout},
    // an aftermarket ECU over WiFi (usb2ip)
    {"knockRetard", "Knock retard", "deg", 0, 10, 4, engineTimeout},
    {"injDuty", "Inj duty", "%", 0, 100, 85, engineTimeout},
    {"lambdaTarget", "Lambda target", "", 0.6, 1.4, 0, engineTimeout},
    // gearbox (0.26.0)
    {"shifting", "Shifting", "", 0, 1, 0, engineTimeout},
    {"lockup", "Lock-up", "", 0, 2, 0, engineTimeout},
    {"inputRpm", "Input speed", "rpm", 0, 8000, 6800, engineTimeout},
    {"convSlip", "Converter slip", "rpm", 0, 1000, 0, engineTimeout},
    // an ECU's barometric pressure (0.28.0, from a DBC): boost is worked out against it
    {"baro", "Baro", "mbar", 800, 1100, 0, engineTimeout},
    // the fuel in the tank in litres, from the VAG cluster (PQ mKombi_1, MQB / MLB Kombi_02); OBD's "fuel" is % (0.38.4)
    {"fuelLitres", "Fuel tank", "L", 0, 80, 0, engineTimeout},
};
static_assert(sizeof(signalTable) / sizeof(signalTable[0]) == SIG_COUNT, "signalTable rows must match signalId_t");
// 0.36.6: one adc signal per adc2can sensor, in one block (sensor n = SIG_ADC_1 + n): raising ADC2CAN_BOARDS needs the
// rows and signalId_t entries added too (can2gauge_defs.h says how)
static_assert(SIG_ADC_16 + 1 - SIG_ADC_1 == ADC_SENSORS, "adc2can sensors: one adc signal per sensor (ADC2CAN_BOARDS x 8)");

static float values[SIG_COUNT];
static uint32_t stamps[SIG_COUNT];
static uint8_t origins[SIG_COUNT];
static uint8_t sourceNodes[SIG_COUNT];
EXT_RAM_BSS_ATTR static char labels[SIG_COUNT][NAME_LEN];
EXT_RAM_BSS_ATTR static char units[SIG_COUNT][UNIT_LEN];
static portMUX_TYPE signalMux = portMUX_INITIALIZER_UNLOCKED;

const char *selectorLetter(uint8_t selector)
{
  static const char *letters[selectorPos_t_MAX] = {"-", "P", "R", "N", "D", "S", "M", "E", "L", "B"};
  return selector < selectorPos_t_MAX ? letters[selector] : "-";
}

void signalsInit()
{
  for (int id = 0; id < SIG_COUNT; id++)
  {
    values[id] = 0;
    stamps[id] = 0;
    origins[id] = SIG_ORIGIN_LOCAL;
    sourceNodes[id] = 0xFF;
    copyStr(labels[id], NAME_LEN, signalTable[id].name);
    copyStr(units[id], UNIT_LEN, signalTable[id].unit);
  }
}

static bool freshLocked(signalId_t id, uint32_t now)
{
  const uint16_t timeout = signalTable[id].timeoutMs;
  return stamps[id] != 0 && (timeout == 0 || now - stamps[id] <= timeout);
}

static void setSignalFrom(signalId_t id, float value, uint8_t origin, uint8_t node)
{
  if (id >= SIG_COUNT || id == SIG_NONE)
  {
    return;
  }
  const uint32_t now = millis();
  portENTER_CRITICAL(&signalMux);
  // a remote copy only fills the gap: it never replaces a fresh value this gauge produced (or heard on its own bus)
  if (origin == SIG_ORIGIN_REMOTE && origins[id] != SIG_ORIGIN_REMOTE && freshLocked(id, now))
  {
    portEXIT_CRITICAL(&signalMux);
    return;
  }
  values[id] = value;
  stamps[id] = now ? now : 1;
  origins[id] = origin;
  sourceNodes[id] = node;
  portEXIT_CRITICAL(&signalMux);
  // derived signals
  if (id == SIG_BOOST)
  {
    // 0.28.0: boost against the ECU's own barometric pressure when a DBC feeds it (SIG_BARO), else a standard 1013 mbar
    float ambient = 1013.0f;
    if (signalValid(SIG_BARO))
    {
      ambient = signalGet(SIG_BARO);
    }
    setSignalFrom(SIG_BOOST_REL, (value - ambient) / 1000.0f, origin, node);
  }
}

void signalSet(signalId_t id, float value)
{
  setSignalFrom(id, value, SIG_ORIGIN_LOCAL, 0xFF);
}

void signalSetBus(signalId_t id, float value)
{
  setSignalFrom(id, value, SIG_ORIGIN_BUS, 0xFF);
}

void signalSetRemote(signalId_t id, float value, uint8_t node)
{
  setSignalFrom(id, value, SIG_ORIGIN_REMOTE, node);
}

signalOrigin_t signalOrigin(signalId_t id)
{
  return (signalOrigin_t)(id < SIG_COUNT ? origins[id] : SIG_ORIGIN_LOCAL);
}

uint8_t signalSourceNode(signalId_t id)
{
  return id < SIG_COUNT ? sourceNodes[id] : 0xFF;
}

float signalGet(signalId_t id)
{
  if (id >= SIG_COUNT)
  {
    return 0;
  }
  if (id == SIG_UPTIME)
  {
    return millis() / 1000.0f;
  }
  portENTER_CRITICAL(&signalMux);
  float value = values[id];
  portEXIT_CRITICAL(&signalMux);
  return value;
}

bool signalValid(signalId_t id)
{
  if (id >= SIG_COUNT || id == SIG_NONE)
  {
    return false;
  }
  if (id == SIG_UPTIME)
  {
    return true;
  }
  const uint16_t timeout = signalTable[id].timeoutMs;
  if (stamps[id] == 0)
  {
    return false;
  }
  return (timeout == 0) || (millis() - stamps[id] <= timeout);
}

uint32_t signalAge(signalId_t id)
{
  if (id >= SIG_COUNT || stamps[id] == 0)
  {
    return 0xFFFFFFFF;
  }
  return millis() - stamps[id];
}

const SignalInfo &signalInfo(signalId_t id)
{
  return signalTable[id < SIG_COUNT ? id : SIG_NONE];
}

signalId_t signalFromKey(const char *key)
{
  if (!key)
  {
    return SIG_NONE;
  }
  for (int id = 0; id < SIG_COUNT; id++)
  {
    if (strcasecmp(signalTable[id].key, key) == 0)
    {
      return (signalId_t)id;
    }
  }
  return SIG_NONE;
}

void signalSetLabel(signalId_t id, const char *name, const char *unit)
{
  if (id >= SIG_COUNT)
  {
    return;
  }
  if (name && name[0])
  {
    copyStr(labels[id], NAME_LEN, name);
  }
  if (unit)
  {
    copyStr(units[id], UNIT_LEN, unit);
  }
}

const char *signalName(signalId_t id)
{
  return labels[id < SIG_COUNT ? id : SIG_NONE];
}

const char *signalUnit(signalId_t id)
{
  return units[id < SIG_COUNT ? id : SIG_NONE];
}

static uint8_t ownDecimals(signalId_t id); // below

// 0.31.0: the decimals a value shows on every display, worked out the same way for every source. Before, Quad used the
// page's Decimals for all four values and Custom its own short list, so an ADC sensor set to two places showed "0 V".
// 0.36.14: a value shown in another unit (psi, kPa, AFR ...) takes that unit's decimals, else its own.
uint8_t signalDecimals(signalId_t id)
{
  uint8_t decimals = 0;
  if (unitsConvertedDecimals(id, decimals))
  {
    return decimals; // 23.2 psi, 160 kPa, 14.7 AFR
  }
  return ownDecimals(id);
}

// the decimals the value itself has, in the unit it's stored in
static uint8_t ownDecimals(signalId_t id)
{
  uint8_t decimals = 0;
  if (userSigDecimals(id, decimals))
  {
    return decimals; // a page source that knows: an ECU channel's list, an ME7 variable's .ecu entry
  }
  if (id >= SIG_ADC_1 && id < SIG_ADC_1 + ADC_SIGS)
  {
    return adc2canDecimals(id - SIG_ADC_1); // the adc2can's own setting, or the ADC tab's override
  }
  switch (id) // built-in values whose unit doesn't say it
  {
  case SIG_BOOST_REL:
  case SIG_LAMBDA:
  case SIG_LAMBDA_TARGET:
    return 2; // 1.25 bar, 0.98 lambda
  case SIG_BATTERY:
  case SIG_HALDEX_VOLTS:
  case SIG_KNOCK_RETARD:
  case SIG_TIMING:
    return 1; // 13.8 V, 2.5 deg
  case SIG_GEAR:
  case SIG_SELECTOR:
  case SIG_HALDEX_MODE:
    return 0;
  default:
    break;
  }
  // anything else (a DBC signal, an OBD PID, a K-line field ...): from its unit
  const char *unit = signalUnit(id);
  const bool isPageSource = id >= SIG_USER_1 && id < SIG_ADC_1; // a page's own value (DBC, OBD, K-line, diagnostics ...)
  if (unit[0] == 0 && !isPageSource)
  {
    return 0; // a built-in value with no unit (counts, states)
  }
  if (unit[0] == 0)
  {
    return 2; // a page source's value with no unit (a ratio, most likely)
  }
  return unitDecimals(unit);
}

// 0.36.14: the decimals a unit says a value has (also the K-line screen's fields). Any upper / lower case: a DBC's
// "Lambda" counts the same as "lambda".
uint8_t unitDecimals(const char *unit)
{
  if (!strcasecmp(unit, "bar") || !strcasecmp(unit, "lambda") || !strcmp(unit, "\xCE\xBB") || !strcmp(unit, "-") ||
      !strcasecmp(unit, "ratio"))
  {
    return 2; // pressures in bar, lambda (also written as the Greek letter), ratios
  }
  if (!strcmp(unit, "V") || !strcasecmp(unit, "deg") || !strcmp(unit, "ms") || !strcmp(unit, "g/s") ||
      !strcasecmp(unit, "AFR") || !strcmp(unit, "A") || !strcmp(unit, "l/h"))
  {
    return 1; // volts, degrees, milliseconds, air mass, air-fuel ratio, amps, fuel flow
  }
  return 0; // rpm, temperatures, speed, mbar / kPa, % and the rest: whole numbers
}

float signalDisplayValue(signalId_t id, float value, const char **unitOut)
{
  // 0.27.0: the unit chosen for this signal's quantity (speed, boost, temperature ...), not one metric / imperial switch
  const char *unit = "";
  value = unitShow(id, value, unitsOf(settings), &unit);
  // shown with the degree sign: "°C" / "°F" / "°" (the stored unit stays "C" so the conversion above can find it)
  if (strcmp(unit, "C") == 0)
  {
    unit = "\xC2\xB0"
           "C";
  }
  else if (strcmp(unit, "F") == 0)
  {
    unit = "\xC2\xB0"
           "F";
  }
  else if (strcmp(unit, "deg") == 0)
  {
    unit = "\xC2\xB0";
  }
  if (unitOut)
  {
    *unitOut = unit;
  }
  return value;
}

// 0.17.0: a value that rounds to zero at the decimals shown comes back as a plain 0, so "%.2f" prints "0.00" and not
// "-0.00" (boost sitting a hair under atmospheric flicked between the two). Anything below zero after rounding keeps
// its minus.
float valueNoMinusZero(float value, int decimals)
{
  float halfStep = 0.5f; // half of the smallest step shown: 0.5 at 0 decimals, 0.005 at 2
  for (int i = 0; i < decimals; i++)
  {
    halfStep = halfStep / 10.0f;
  }
  if (value < 0 && value > -halfStep)
  {
    return 0.0f;
  }
  return value;
}
