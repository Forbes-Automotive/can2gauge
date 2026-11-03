#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// 0.27.0: a unit per quantity (Tiguan drive notes, item 10). Six rows - speed, boost, other pressures, temperature,
// mixture, distance - each with its own choice (settings.unitSpeed ...). settings.units is the quick choice on top:
// Metric / Imperial set every row at once, Custom means they were picked one by one.
//
// What it changes: everything the glass and the web UI SHOW. A page's Min / Max / Redline are in the shown unit, and
// when a row changes, unitsConvertPages() turns them over (rounded to sensible numbers: 280 km/h -> 175 mph).
// What it doesn't: signals are stored in metric, and logs, triggers and the API stay in those units.
// ---------------------------------------------------------------------------

// the rows (quantities). A signal belongs to one by its stored unit (unitRowOf)
#define UNIT_ROW_NONE 0     // never converted (rpm, %, V ...)
#define UNIT_ROW_SPEED 1    // km/h
#define UNIT_ROW_BOOST 2    // boost (bar, relative) and manifold pressure (mbar, absolute)
#define UNIT_ROW_PRESSURE 3 // oil, fuel and other pressures in bar
#define UNIT_ROW_TEMP 4     // C
#define UNIT_ROW_MIXTURE 5  // lambda
#define UNIT_ROW_DISTANCE 6 // km
#define UNIT_ROW_COUNT 7    // rows 1-6 (0 isn't a row)

// the choice in a row
#define UNIT_METRIC 0   // km/h, bar, C, lambda, km
#define UNIT_IMPERIAL 1 // mph, psi, F, AFR, miles
#define UNIT_KPA 2      // the two pressure rows only: kPa

// settings.units, the quick choice
#define UNITS_METRIC 0
#define UNITS_IMPERIAL 1 // mph, psi, F, miles - lambda stays lambda
#define UNITS_CUSTOM 2

// the six choices together, by row number (choice[UNIT_ROW_SPEED] ...; choice[0] is unused)
struct UnitChoices
{
  uint8_t choice[UNIT_ROW_COUNT];
};

UnitChoices unitsOf(const Settings &from);          // the choices in a settings copy
void unitsSetQuick(uint8_t quick);                  // Metric / Imperial: every row at once (settings.units too)
uint8_t unitsQuickFromRows();                       // UNITS_METRIC / UNITS_IMPERIAL if the rows all match, else UNITS_CUSTOM
void unitsSanitize(Settings &target);               // a row out of range back to metric
uint8_t unitRowOf(signalId_t id);                   // which row a signal belongs to (UNIT_ROW_NONE = never converted)
// a stored (metric) value as it's shown with these choices, and the unit it's shown in ("mph", "psi", "C" ...)
float unitShow(signalId_t id, float storedValue, const UnitChoices &choices, const char **unitOut);
float unitStore(signalId_t id, float shownValue, const UnitChoices &choices); // the other way round
// after a change: every page's Min / Max / Redline (and decimals) turned from the old choices into today's
void unitsConvertPages(const UnitChoices &before);
// 0.36.14: a value shown in another unit than it's stored in (mph, psi, kPa, F, AFR, miles): the decimals that unit
// shows (23.2 psi, 160 kPa, 14.7 AFR). false = shown as stored: its own decimals apply (signalDecimals)
bool unitsConvertedDecimals(signalId_t id, uint8_t &decimals);
