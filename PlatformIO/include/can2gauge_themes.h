#pragma once
// ---------------------------------------------------------------------------
// Gauge colour themes (0.5.0). One table feeds the gauge (GaugeUI palette, on-screen theme picker), the web UI
// dropdown (GET /api/themes) and Match car (VIN -> make + model year -> theme). Manufacturer colours, eras and the
// palette rules come from docs/Manufacturer themes.pdf.
//
// settings.theme holds a theme id. Ids are permanent - a saved id must always mean the same theme - so new themes
// get new ids at the end, never a renumber:
//   0..3    the original can2gauge palettes (teal Forbes, amber, red, white) - full palettes in GaugeUI
//   4       Custom: dial + needle colours from the web UI (settings.themeDial / themeNeedle)
//   5       Match car: the manufacturer theme picked from the VIN (settings.themeMatched), Custom until one is read
//   16..80  manufacturer themes; the dial and needle colours give the whole palette (GaugeUI paletteFrom)
//
// Boot logos for boot logo option 5 live on LittleFS as /logos/<key>.bin, key = a LogoDef key ("vw", "vauxhall").
// This header is included by lib/GaugeUI, so it only pulls in stdint.
// ---------------------------------------------------------------------------
#include <stdint.h>
#include <stddef.h>

#define THEME_TEAL 0
#define THEME_AMBER 1
#define THEME_RED 2
#define THEME_WHITE 3
#define THEME_CUSTOM 4
#define THEME_MATCH 5
#define THEME_OEM_FIRST 16
#define THEME_NONE 0xFF // settings.themeMatched before any VIN has been read

enum themeConf_t : uint8_t
{
  CONF_APPROX = 0, // era and hue from memory - check against a photo
  CONF_KNOWN,      // widely documented era colour
  CONF_SOURCED,    // wavelength / colour-picker value from a cited source
};

struct ThemeDef
{
  uint8_t id;
  uint8_t conf;          // themeConf_t
  uint16_t from, to;     // era in model years, to = 9999 while current; 0/0 for the non-manufacturer entries
  uint32_t dial, needle; // 0xRRGGBB (the original palettes list their accent here, for previews)
  const char *name;      // "Volkswagen - Blue"
  const char *make;      // match / logo key ("vw"); NULL for the non-manufacturer entries
  const char *era;       // "1999-2009 (Golf Mk4-5, ...)"
};

struct LogoDef
{
  const char *key;  // file name: /logos/<key>.bin
  const char *name; // shown in the web UI and as the text fallback when the file is missing
};

uint8_t themeCount();                   // every entry, display order: can2gauge palettes, Match car, Custom, makes A-Z
const ThemeDef &themeAt(uint8_t index);
const ThemeDef *themeById(uint8_t id);  // NULL = no such id
bool themeValid(uint8_t id);
const char *themeName(uint8_t id);
uint8_t themeEffective(uint8_t id);     // Match car -> the matched theme, or Custom while none; others unchanged
void themeColours(uint8_t id, uint32_t &dial, uint32_t &needle); // Custom -> settings, Match car -> resolved first
uint8_t themeMatch(const char *makeKey, uint16_t year);          // best manufacturer theme, year 0 = unknown -> newest era
void themeVehicleDetected(const char *logoKey, uint16_t year);   // ELM decoded a VIN: store the match, re-theme on Match car

uint8_t logoCount();
const LogoDef &logoAt(uint8_t index);
const char *logoName(const char *key);  // NULL = unknown key
const char *themeLogoKey();             // boot logo 5: chosen make, else the VIN's make on Match car, else the theme's make

uint32_t themeParseHex(const char *text, uint32_t fallback); // "#RRGGBB" / "RRGGBB" -> 0xRRGGBB
