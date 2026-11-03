#include <can2gauge_Units.h>
#include <math.h> // powf / log10f / floorf for the rounding

// conversion factors
#define KMH_TO_MPH 0.621371f   // 1 km/h in mph
#define KM_TO_MILES 0.621371f  // 1 km in miles
#define BAR_TO_PSI 14.5038f    // 1 bar in psi
#define BAR_TO_KPA 100.0f      // 1 bar in kPa
#define AFR_STOICH 14.7f       // petrol: lambda 1.0 = 14.7 AFR

UnitChoices unitsOf(const Settings &from)
{
  UnitChoices choices;
  choices.choice[UNIT_ROW_NONE] = UNIT_METRIC; // not a row
  choices.choice[UNIT_ROW_SPEED] = from.unitSpeed;
  choices.choice[UNIT_ROW_BOOST] = from.unitBoost;
  choices.choice[UNIT_ROW_PRESSURE] = from.unitPressure;
  choices.choice[UNIT_ROW_TEMP] = from.unitTemp;
  choices.choice[UNIT_ROW_MIXTURE] = from.unitMixture;
  choices.choice[UNIT_ROW_DISTANCE] = from.unitDistance;
  return choices;
}

void unitsSetQuick(uint8_t quick)
{
  uint8_t choice = UNIT_METRIC;
  if (quick == UNITS_IMPERIAL)
  {
    choice = UNIT_IMPERIAL;
  }
  settings.unitSpeed = choice;
  settings.unitBoost = choice;
  settings.unitPressure = choice;
  settings.unitTemp = choice;
  settings.unitMixture = UNIT_METRIC; // lambda stays lambda either way (AFR only when picked on its own)
  settings.unitDistance = choice;
  settings.units = UNITS_METRIC;
  if (quick == UNITS_IMPERIAL)
  {
    settings.units = UNITS_IMPERIAL;
  }
}

uint8_t unitsQuickFromRows()
{
  const bool allMetric = settings.unitSpeed == UNIT_METRIC && settings.unitBoost == UNIT_METRIC &&
                         settings.unitPressure == UNIT_METRIC && settings.unitTemp == UNIT_METRIC &&
                         settings.unitMixture == UNIT_METRIC && settings.unitDistance == UNIT_METRIC;
  const bool allImperial = settings.unitSpeed == UNIT_IMPERIAL && settings.unitBoost == UNIT_IMPERIAL &&
                           settings.unitPressure == UNIT_IMPERIAL && settings.unitTemp == UNIT_IMPERIAL &&
                           settings.unitMixture == UNIT_METRIC && settings.unitDistance == UNIT_IMPERIAL;
  if (allMetric)
  {
    return UNITS_METRIC;
  }
  if (allImperial)
  {
    return UNITS_IMPERIAL;
  }
  return UNITS_CUSTOM;
}

void unitsSanitize(Settings &target)
{
  if (target.units > UNITS_CUSTOM)
  {
    target.units = UNITS_METRIC;
  }
  if (target.unitSpeed > UNIT_IMPERIAL)
  {
    target.unitSpeed = UNIT_METRIC;
  }
  if (target.unitBoost > UNIT_KPA)
  {
    target.unitBoost = UNIT_METRIC;
  }
  if (target.unitPressure > UNIT_KPA)
  {
    target.unitPressure = UNIT_METRIC;
  }
  if (target.unitTemp > UNIT_IMPERIAL)
  {
    target.unitTemp = UNIT_METRIC;
  }
  if (target.unitMixture > UNIT_IMPERIAL)
  {
    target.unitMixture = UNIT_METRIC;
  }
  if (target.unitDistance > UNIT_IMPERIAL)
  {
    target.unitDistance = UNIT_METRIC;
  }
}

uint8_t unitRowOf(signalId_t id)
{
  if (id == SIG_NONE)
  {
    return UNIT_ROW_NONE;
  }
  const char *unit = signalUnit(id); // the stored unit (a page source's can change at runtime: a DBC's "km/h" ...)
  if (strcmp(unit, "km/h") == 0)
  {
    return UNIT_ROW_SPEED;
  }
  if (strcmp(unit, "mbar") == 0)
  {
    return UNIT_ROW_BOOST; // manifold pressure, absolute
  }
  if (strcmp(unit, "bar") == 0)
  {
    if (id == SIG_BOOST_REL)
    {
      return UNIT_ROW_BOOST; // boost, relative
    }
    return UNIT_ROW_PRESSURE; // oil, fuel ...
  }
  if (strcmp(unit, "C") == 0)
  {
    return UNIT_ROW_TEMP;
  }
  if (strcmp(unit, "km") == 0)
  {
    return UNIT_ROW_DISTANCE;
  }
  if (id == SIG_LAMBDA || id == SIG_LAMBDA_TARGET || strcmp(unit, "lambda") == 0)
  {
    return UNIT_ROW_MIXTURE;
  }
  return UNIT_ROW_NONE;
}

float unitShow(signalId_t id, float storedValue, const UnitChoices &choices, const char **unitOut)
{
  const char *storedUnit = signalUnit(id);
  const uint8_t row = unitRowOf(id);
  const uint8_t choice = choices.choice[row];
  float value = storedValue;
  const char *unit = storedUnit;
  switch (row)
  {
  case UNIT_ROW_SPEED:
    if (choice == UNIT_IMPERIAL)
    {
      value = storedValue * KMH_TO_MPH;
      unit = "mph";
    }
    break;
  case UNIT_ROW_TEMP:
    if (choice == UNIT_IMPERIAL)
    {
      value = storedValue * 9.0f / 5.0f + 32.0f;
      unit = "F";
    }
    break;
  case UNIT_ROW_DISTANCE:
    if (choice == UNIT_IMPERIAL)
    {
      value = storedValue * KM_TO_MILES;
      unit = "mi";
    }
    break;
  case UNIT_ROW_MIXTURE:
    if (choice == UNIT_IMPERIAL)
    {
      value = storedValue * AFR_STOICH;
      unit = "AFR";
    }
    break;
  case UNIT_ROW_BOOST:
  case UNIT_ROW_PRESSURE:
  {
    // in bar first (manifold pressure is stored in mbar), then psi or kPa. Metric keeps bar / mbar as stored.
    float bar = storedValue;
    if (strcmp(storedUnit, "mbar") == 0)
    {
      bar = storedValue / 1000.0f;
    }
    if (choice == UNIT_IMPERIAL)
    {
      value = bar * BAR_TO_PSI;
      unit = "psi";
    }
    else if (choice == UNIT_KPA)
    {
      value = bar * BAR_TO_KPA;
      unit = "kPa";
    }
    break;
  }
  default:
    break;
  }
  if (unitOut)
  {
    *unitOut = unit;
  }
  return value;
}

float unitStore(signalId_t id, float shownValue, const UnitChoices &choices)
{
  const char *storedUnit = signalUnit(id);
  const uint8_t row = unitRowOf(id);
  const uint8_t choice = choices.choice[row];
  switch (row)
  {
  case UNIT_ROW_SPEED:
    if (choice == UNIT_IMPERIAL)
    {
      return shownValue / KMH_TO_MPH;
    }
    break;
  case UNIT_ROW_TEMP:
    if (choice == UNIT_IMPERIAL)
    {
      return (shownValue - 32.0f) * 5.0f / 9.0f;
    }
    break;
  case UNIT_ROW_DISTANCE:
    if (choice == UNIT_IMPERIAL)
    {
      return shownValue / KM_TO_MILES;
    }
    break;
  case UNIT_ROW_MIXTURE:
    if (choice == UNIT_IMPERIAL)
    {
      return shownValue / AFR_STOICH;
    }
    break;
  case UNIT_ROW_BOOST:
  case UNIT_ROW_PRESSURE:
  {
    if (choice == UNIT_METRIC)
    {
      return shownValue; // bar / mbar, as stored
    }
    float bar = shownValue / BAR_TO_PSI; // psi
    if (choice == UNIT_KPA)
    {
      bar = shownValue / BAR_TO_KPA;
    }
    if (strcmp(storedUnit, "mbar") == 0)
    {
      return bar * 1000.0f;
    }
    return bar;
  }
  default:
    break;
  }
  return shownValue;
}

// a converted range end rounded to a sensible step: a twentieth of the span's power of ten (280 km/h -> 174 mph ->
// 175 with a step of 5; 1.6 bar -> 23.2 psi -> 23.0 with a step of 0.5)
static float roundToStep(float value, float span)
{
  if (span <= 0.0f)
  {
    return value;
  }
  const float step = powf(10.0f, floorf(log10f(span))) / 20.0f;
  return roundf(value / step) * step;
}

// how many decimals a page shows in its new unit
static uint8_t decimalsFor(uint8_t row, uint8_t choice, signalId_t id)
{
  switch (row)
  {
  case UNIT_ROW_MIXTURE:
    if (choice == UNIT_IMPERIAL)
    {
      return 1; // 14.7 AFR
    }
    return 2; // 1.00 lambda
  case UNIT_ROW_BOOST:
    if (choice == UNIT_IMPERIAL)
    {
      return 1; // 23.2 psi
    }
    if (choice == UNIT_KPA || id == SIG_BOOST)
    {
      return 0; // 160 kPa, 2200 mbar
    }
    return 2; // 1.60 bar
  case UNIT_ROW_PRESSURE:
    if (choice == UNIT_METRIC)
    {
      return 1; // 4.5 bar
    }
    return 0; // 65 psi, 450 kPa
  default:
    return 0; // speed, temperature, distance
  }
}

bool unitsConvertedDecimals(signalId_t id, uint8_t &decimals)
{
  const uint8_t row = unitRowOf(id);
  if (row == UNIT_ROW_NONE)
  {
    return false; // never converted (rpm, %, V ...)
  }
  const UnitChoices now = unitsOf(settings);
  if (now.choice[row] == UNIT_METRIC)
  {
    return false; // shown in the unit it's stored in
  }
  decimals = decimalsFor(row, now.choice[row], id);
  return true;
}

void unitsConvertPages(const UnitChoices &before)
{
  const UnitChoices now = unitsOf(settings);
  for (uint8_t pageIndex = 0; pageIndex < MAX_PAGES; pageIndex++)
  {
    GaugePageCfg &page = settings.pages[pageIndex];
    const signalId_t id = (signalId_t)page.signal;
    const uint8_t row = unitRowOf(id);
    if (row == UNIT_ROW_NONE || before.choice[row] == now.choice[row])
    {
      continue; // not a converted quantity, or its unit didn't change
    }
    const bool hasRedline = page.redline > page.minVal; // a redline at or below Min means "none"
    // each end: shown in the old unit -> stored (metric) -> shown in the new unit
    float newMin = unitShow(id, unitStore(id, page.minVal, before), now, NULL);
    float newMax = unitShow(id, unitStore(id, page.maxVal, before), now, NULL);
    float newRedline = unitShow(id, unitStore(id, page.redline, before), now, NULL);
    const float span = fabsf(newMax - newMin);
    newMin = roundToStep(newMin, span);
    newMax = roundToStep(newMax, span);
    newRedline = roundToStep(newRedline, span);
    page.minVal = newMin;
    page.maxVal = newMax;
    if (hasRedline)
    {
      page.redline = newRedline;
    }
    else
    {
      page.redline = newMin; // still "none"
    }
    page.decimals = decimalsFor(row, now.choice[row], id);
    DEBUG("Units: page %u now %.2f .. %.2f, redline %.2f", pageIndex + 1, page.minVal, page.maxVal, page.redline);
  }
  settingsDirty = true;
}
