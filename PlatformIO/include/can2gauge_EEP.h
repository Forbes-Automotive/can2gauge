#pragma once
#include <can2gauge_defs.h>
#include <ArduinoJson.h> // settingsToJson / settingsFromJson take a JsonDocument

void setDefaults(Settings &target); // factory defaults (used on first run and by "reset")
uint8_t pageSourceOf(signalId_t id); // pageSource_t a catalogue signal is listed under
void readEEP();                // NVS -> settings (defaults on first run, or when the saved layout is a different version)
void writeEEP();               // settings -> NVS
void eepTick();                // call from loop: persists when settingsDirty (rate limited)
// JSON mirror for the web UI / backup
void settingsToJson(JsonDocument &doc);
bool settingsFromJson(const JsonDocument &doc); // merges the keys present (a partial document is fine); true if anything changed
