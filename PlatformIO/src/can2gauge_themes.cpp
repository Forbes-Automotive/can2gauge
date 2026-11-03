#include <can2gauge_defs.h>
#include <can2gauge_themes.h>
#include <GaugeUI.h> // uiApplyTheme() when Match car picks a new theme

// ===== Settings for this file =====
#define NOW 9999                                         // the "to" year of an era that is still current
#define THEME_COUNT (sizeof(themes) / sizeof(themes[0])) // rows in the theme table below
#define LOGO_COUNT (sizeof(logos) / sizeof(logos[0]))    // rows in the boot logo table below

// ===== The theme table =====
// In display order. Manufacturer rows: docs/Manufacturer themes.pdf (65 themes, 31 makes), ids 16..80 in the PDF's
// order. The era years drive Match car; where two eras overlap the newer one wins.
// Each row: id, how sure the colours are, era from / to (model years), dial colour, needle colour, name, make key, era.
// The simulator (sim/firmware.py) reads these rows straight from this file, so keep each row on one line in this form.
static const ThemeDef themes[] = {
    // the original can2gauge palettes (full palettes in GaugeUI; accent shown here for previews)
    {THEME_TEAL, CONF_KNOWN, 0, 0, 0x24D1C4, 0x24D1C4, "can2gauge - Teal (Forbes)", NULL, "the shared Forbes Automotive palette"},
    {THEME_AMBER, CONF_KNOWN, 0, 0, 0xFFB02E, 0xFFB02E, "can2gauge - Amber", NULL, ""},
    {THEME_RED, CONF_KNOWN, 0, 0, 0xFF3B4A, 0xFF3B4A, "can2gauge - Red", NULL, ""},
    {THEME_WHITE, CONF_KNOWN, 0, 0, 0xFFFFFF, 0xFFFFFF, "can2gauge - White", NULL, ""},
    {THEME_MATCH, CONF_KNOWN, 0, 0, 0xF2F4F8, 0xFF3B3B, "Match car (from the VIN)", NULL, "manufacturer theme from the VIN read over Bluetooth OBD"},
    {THEME_CUSTOM, CONF_KNOWN, 0, 0, 0xF2F4F8, 0xFF3B3B, "Custom", NULL, "dial and needle colours set in the web UI"},
    // manufacturers
    {16, CONF_KNOWN, 1997, 2011, 0xE8303A, 0xFF4A4A, "Alfa Romeo - Red", "alfa", "1997-2011 (147, 156, 159, GT, Brera)"},
    {17, CONF_APPROX, 2016, NOW, 0xF2F4F8, 0xE8303A, "Alfa Romeo - White", "alfa", "2016+ (Giulia, Stelvio)"},
    {18, CONF_KNOWN, 2004, 2016, 0x4FA3FF, 0xF2F4F8, "Aston Martin - Blue", "aston", "2004-2016 (DB9, V8 Vantage)"},
    {19, CONF_KNOWN, 1994, 2010, 0xE5202E, 0xF2F4F8, "Audi - Red", "audi", "1994-2010 (A3 8L/8P, A4 B5-B7, TT 8N, A6 C5)"},
    {20, CONF_KNOWN, 2008, NOW, 0xF2F4F8, 0xE5202E, "Audi - White", "audi", "2008+ (A4 B8, A6 C7, virtual cockpit)"},
    {21, CONF_SOURCED, 1975, 2012, 0xFF7A1A, 0xFF8C2A, "BMW - Amber", "bmw", "1975-2012 (E30 to E90)"},
    {22, CONF_SOURCED, 2012, NOW, 0xF2F4F8, 0xFF3B3B, "BMW - White", "bmw", "2012+ (F and G series)"},
    {23, CONF_APPROX, 1988, 2002, 0x58FF7A, 0xFF6A3A, "Chevrolet - Green", "chevrolet", "1988-2002 (C/K, Camaro, Cavalier)"},
    {24, CONF_APPROX, 2006, 2013, 0x7FD4FF, 0xFF5A3A, "Chevrolet - Ice blue", "chevrolet", "2006-2013 (Silverado, Cobalt, Malibu)"},
    {25, CONF_APPROX, 1995, 2007, 0x4FE0C8, 0xFF6A3A, "Dodge - Green-blue", "dodge", "1995-2007 (Neon, Ram, Durango)"},
    {26, CONF_APPROX, 2011, NOW, 0xF2F4F8, 0xFF2A2A, "Dodge - Red", "dodge", "2011+ (Charger, Challenger, SRT)"},
    {27, CONF_APPROX, 1999, 2010, 0xFF5A3A, 0xFF7A5A, "Fiat - Red", "fiat", "1999-2010 (Punto Mk2, Stilo, Bravo)"},
    {28, CONF_APPROX, 2007, NOW, 0xF2F4F8, 0xFF3B3B, "Fiat - White", "fiat", "2007+ (500, Panda 3)"},
    {29, CONF_SOURCED, 1985, 2004, 0x58FF7A, 0xFF6A3A, "Ford - Green", "ford", "1985-2004 (Escort, Fiesta Mk3-5, Focus Mk1, Mondeo Mk1-2)"},
    {30, CONF_APPROX, 2004, 2008, 0xFF3B3B, 0xF2F4F8, "Ford - Red", "ford", "2004-2008 (Focus Mk2, C-Max, Mondeo Mk3 facelift)"},
    {31, CONF_KNOWN, 2008, 2018, 0x8FD6FF, 0xFF3B3B, "Ford - Ice blue", "ford", "2008-2018 (Fiesta Mk7, Focus Mk3, Mustang S197)"},
    {32, CONF_APPROX, 2018, NOW, 0xF2F4F8, 0xFF3B3B, "Ford - White", "ford", "2018+ (Focus Mk4, Puma)"},
    {33, CONF_KNOWN, 1988, 2005, 0xFFA63A, 0xFF7A3A, "Honda - Amber", "honda", "1988-2005 (Civic EG/EK/EP, Integra, Accord)"},
    {34, CONF_KNOWN, 2006, 2015, 0x4FA8FF, 0xFF3B3B, "Honda - Blue", "honda", "2006-2015 (Civic FD/FN, Accord, CR-V)"},
    {35, CONF_APPROX, 2016, NOW, 0xF2F4F8, 0xFF3B3B, "Honda - White", "honda", "2016+ (Civic FC/FK, Jazz)"},
    {36, CONF_KNOWN, 2005, 2015, 0x3F8FFF, 0xF2F4F8, "Hyundai - Blue", "hyundai", "2005-2015 (i30, Santa Fe, Sonata)"},
    {37, CONF_APPROX, 2015, NOW, 0xF2F4F8, 0xFF3B3B, "Hyundai - White", "hyundai", "2015+ (i30 PD, Tucson, Kona)"},
    {38, CONF_KNOWN, 1999, 2013, 0xB08CFF, 0xFF8C2A, "Infiniti - Violet", "infiniti", "1999-2013 (G35, G37, FX, M)"},
    {39, CONF_SOURCED, 1986, 2008, 0x58FF7A, 0xF2F4F8, "Jaguar - Green", "jaguar", "1986-2008 (XJ40, X300, S-Type, X-Type)"},
    {40, CONF_KNOWN, 2008, NOW, 0xE8F0FF, 0xFF3B3B, "Jaguar - White", "jaguar", "2008+ (XF, XJ X351, F-Type)"},
    {41, CONF_APPROX, 2006, 2012, 0xFF4A3A, 0xF2F4F8, "Kia - Red", "kia", "2006-2012 (Cee'd, Sportage, Soul)"},
    {42, CONF_APPROX, 2012, NOW, 0xF2F4F8, 0xFF3B3B, "Kia - White", "kia", "2012+ (Cee'd JD, Sportage QL, Stinger)"},
    {43, CONF_APPROX, 1989, 2004, 0x6AFF8A, 0xFF8C2A, "Land Rover - Green", "landrover", "1989-2004 (Discovery 1-2, Defender, P38)"},
    {44, CONF_APPROX, 2005, NOW, 0xE8F0FF, 0xFF3B3B, "Land Rover - White", "landrover", "2005+ (Discovery 3, L322 facelift, Evoque)"},
    {45, CONF_KNOWN, 1989, NOW, 0xF6F8FF, 0xFF4A3A, "Lexus - Optitron", "lexus", "1989+ (all Optitron clusters)"},
    {46, CONF_SOURCED, 1985, 2002, 0x58FF7A, 0xFF6A3A, "Mazda - Green", "mazda", "1985-2002 (323, 626, MX-5 NA/NB)"},
    {47, CONF_KNOWN, 2003, 2013, 0xFF3B3B, 0xFF3B3B, "Mazda - Red", "mazda", "2003-2013 (Mazda3 BK/BL, Mazda6 GG/GH, RX-8, MX-5 NC)"},
    {48, CONF_KNOWN, 2013, NOW, 0xF2F4F8, 0xFF3B3B, "Mazda - White", "mazda", "2013+ (Skyactiv, MX-5 ND)"},
    {49, CONF_KNOWN, 1985, 2006, 0xFFF1C8, 0xFF8C2A, "Mercedes-Benz - Warm white", "mercedes", "1985-2006 (W124, W202, W210, W203)"},
    {50, CONF_KNOWN, 2007, NOW, 0xF2F4F8, 0xFF3B3B, "Mercedes-Benz - White", "mercedes", "2007+ (W204, W212, W205)"},
    {51, CONF_KNOWN, 2001, 2013, 0xFF8C2A, 0xFF8C2A, "Mini - Amber", "mini", "2001-2013 (R50, R53, R56)"},
    {52, CONF_APPROX, 2014, NOW, 0xF2F4F8, 0xFF3B3B, "Mini - White", "mini", "2014+ (F56)"},
    {53, CONF_APPROX, 2000, 2012, 0xFF3B3B, 0xF2F4F8, "Mitsubishi - Red", "mitsubishi", "2000-2012 (Evo VII-X, Outlander, Colt)"},
    {54, CONF_KNOWN, 1989, 2007, 0xFF9A3A, 0xFF7A3A, "Nissan - Amber", "nissan", "1989-2007 (Skyline R32-R34, 350Z, Micra)"},
    {55, CONF_KNOWN, 2008, NOW, 0xF2F4F8, 0xFF3B3B, "Nissan - White", "nissan", "2008+ (370Z, GT-R, Qashqai, Juke)"},
    {56, CONF_APPROX, 1991, 2004, 0x58FF7A, 0xFF8C2A, "Opel / Vauxhall - Green", "opel", "1991-2004 (Astra F/G, Vectra B, Corsa B/C)"},
    {57, CONF_KNOWN, 2004, 2015, 0xFF4A3A, 0xF2F4F8, "Opel / Vauxhall - Red", "opel", "2004-2015 (Astra H/J, Vectra C, Insignia A, Corsa D)"},
    {58, CONF_APPROX, 2015, NOW, 0xF2F4F8, 0xFF3B3B, "Opel / Vauxhall - White", "opel", "2015+ (Astra K, Insignia B)"},
    {59, CONF_KNOWN, 1987, 2001, 0xFFA63A, 0xFF7A3A, "Peugeot / Citroen - Amber", "peugeot", "1987-2001 (205, 306, 406, ZX, Xantia)"},
    {60, CONF_APPROX, 2001, NOW, 0xF2F4F8, 0xFF3B3B, "Peugeot / Citroen - White", "peugeot", "2001+ (307, 407, C4, 308, i-Cockpit)"},
    {61, CONF_KNOWN, 1965, 1998, 0x6AFF8A, 0xF2F4F8, "Porsche - Green", "porsche", "1965-1998 (911 to 993, 944, 968)"},
    {62, CONF_KNOWN, 1998, NOW, 0xF2F4F8, 0xFF3B3B, "Porsche - White", "porsche", "1998+ (996, 997, 991, Cayenne)"},
    {63, CONF_APPROX, 1990, 2002, 0xFFA63A, 0xFF7A3A, "Renault - Amber", "renault", "1990-2002 (Clio 1-2, Megane 1, Laguna 1)"},
    {64, CONF_APPROX, 2002, NOW, 0xF2F4F8, 0xFF3B3B, "Renault - White", "renault", "2002+ (Megane 2-3, Clio 3-4, Laguna 2)"},
    {65, CONF_KNOWN, 1984, 2011, 0x58FF7A, 0xFF8C2A, "Saab - Green", "saab", "1984-2011 (900, 9000, 9-3, 9-5, Night Panel)"},
    {66, CONF_KNOWN, 1999, 2012, 0xFF3B3B, 0xF2F4F8, "Seat - Red", "seat", "1999-2012 (Leon Mk1-2, Toledo, Ibiza 6L/6J)"},
    {67, CONF_APPROX, 2012, NOW, 0xF2F4F8, 0xFF3B3B, "Seat - White", "seat", "2012+ (Leon Mk3, Ibiza 6F, Ateca)"},
    {68, CONF_KNOWN, 1996, 2007, 0x58FF7A, 0xF2F4F8, "Skoda - Green", "skoda", "1996-2007 (Octavia Mk1, Fabia Mk1, Felicia)"},
    {69, CONF_APPROX, 2004, NOW, 0xF2F4F8, 0xFF3B3B, "Skoda - White", "skoda", "2004+ (Octavia Mk2+, Superb 2+, Fabia 2+)"},
    {70, CONF_APPROX, 1992, 2000, 0xFFA63A, 0xFF7A3A, "Subaru - Amber", "subaru", "1992-2000 (Impreza GC, Legacy BD)"},
    {71, CONF_KNOWN, 2001, 2014, 0xFF3B3B, 0xF2F4F8, "Subaru - Red", "subaru", "2001-2014 (Impreza GD/GR, WRX/STI, Legacy BL/BP)"},
    {72, CONF_APPROX, 2015, NOW, 0xF2F4F8, 0xFF3B3B, "Subaru - White", "subaru", "2015+ (WRX VA, Levorg, XV)"},
    {73, CONF_SOURCED, 1985, 2000, 0x58FF7A, 0xFF6A3A, "Toyota - Green", "toyota", "1985-2000 (Corolla AE, Supra, Celica, MR2)"},
    {74, CONF_SOURCED, 2000, 2008, 0x6FB8FF, 0xFF4A3A, "Toyota - Blue", "toyota", "2000-2008 (Optitron: Celica T230, MR2 W30, Avensis)"},
    {75, CONF_SOURCED, 2008, NOW, 0xF6F8FF, 0xFF4A3A, "Toyota - White", "toyota", "2008+ (Optitron, all current)"},
    {76, CONF_APPROX, 1983, 1998, 0x6AFF8A, 0xF2F4F8, "Volkswagen - Green", "vw", "1983-1998 (Golf Mk2-3, Passat B3-B4, Polo 6N)"},
    {77, CONF_SOURCED, 1999, 2009, 0x1F5BFF, 0xE0202A, "Volkswagen - Blue", "vw", "1999-2009 (Golf Mk4-5, Passat B5-B6, Polo 9N, T5)"},
    {78, CONF_KNOWN, 2009, NOW, 0xF2F4F8, 0xE0202A, "Volkswagen - White", "vw", "2009+ (Golf Mk6-8, Passat B7+, MQB)"},
    {79, CONF_KNOWN, 1975, 1999, 0xFFA63A, 0xFF7A3A, "Volvo - Amber", "volvo", "1975-1999 (240, 740, 850, S70/V70 Mk1)"},
    {80, CONF_KNOWN, 2000, NOW, 0xE8F0FF, 0xFF3B3B, "Volvo - White", "volvo", "2000+ (S60/V70 P2, S80, XC90, SPA)"},
};

// ===== The boot logo table =====
// Each row: the logo key (file /logos/<key>.bin) and the make's name. Two makes share a theme with a sister brand, so
// the logo keys are finer than the theme keys: vauxhall -> opel, citroen -> peugeot (themeKeyFor below).
// The simulator reads this table too, by its name: keep the name logos.
static const LogoDef logos[] = {
    {"alfa", "Alfa Romeo"},
    {"aston", "Aston Martin"},
    {"audi", "Audi"},
    {"bmw", "BMW"},
    {"chevrolet", "Chevrolet"},
    {"citroen", "Citroen"},
    {"dodge", "Dodge"},
    {"fiat", "Fiat"},
    {"ford", "Ford"},
    {"honda", "Honda"},
    {"hyundai", "Hyundai"},
    {"infiniti", "Infiniti"},
    {"jaguar", "Jaguar"},
    {"kia", "Kia"},
    {"landrover", "Land Rover"},
    {"lexus", "Lexus"},
    {"mazda", "Mazda"},
    {"mercedes", "Mercedes-Benz"},
    {"mini", "Mini"},
    {"mitsubishi", "Mitsubishi"},
    {"nissan", "Nissan"},
    {"opel", "Opel"},
    {"peugeot", "Peugeot"},
    {"porsche", "Porsche"},
    {"renault", "Renault"},
    {"saab", "Saab"},
    {"seat", "Seat"},
    {"skoda", "Skoda"},
    {"subaru", "Subaru"},
    {"toyota", "Toyota"},
    {"vauxhall", "Vauxhall"},
    {"volvo", "Volvo"},
    {"vw", "Volkswagen"},
};

// ===== Functions in this file =====
static const char *themeKeyFor(const char *logoKey); // the theme key for a logo key (vauxhall -> opel, citroen -> peugeot)

// ===== Theme lookups =====

// how many themes there are (the web UI dropdown and the on-screen picker list them all)
uint8_t themeCount()
{
  return THEME_COUNT;
}

// the theme in row index of the table (a row past the end gives the first theme)
const ThemeDef &themeAt(uint8_t index)
{
  if (index >= THEME_COUNT)
  {
    index = 0; // past the end: the first theme
  }
  return themes[index];
}

// the theme with this id, or NULL when there is no such id
const ThemeDef *themeById(uint8_t id)
{
  for (uint8_t i = 0; i < THEME_COUNT; i++)
  {
    if (themes[i].id == id)
    {
      return &themes[i];
    }
  }
  return NULL;
}

// is this a theme id the table knows?
bool themeValid(uint8_t id)
{
  return themeById(id) != NULL;
}

// the theme's name ("Volkswagen - Blue"), "?" for an unknown id
const char *themeName(uint8_t id)
{
  const ThemeDef *found = themeById(id);
  if (found == NULL)
  {
    return "?";
  }
  const ThemeDef &theme = *found; // the table row it found
  return theme.name;
}

// the theme actually shown: Match car gives the matched theme (Custom until a VIN has been read), an unknown id gives
// teal, any other id stays as it is
uint8_t themeEffective(uint8_t id)
{
  if (id == THEME_MATCH)
  {
    if (themeValid(settings.themeMatched))
    {
      return settings.themeMatched;
    }
    return THEME_CUSTOM;
  }
  if (themeValid(id))
  {
    return id;
  }
  return THEME_TEAL;
}

// the dial and needle colours of a theme: Custom reads them from the settings, Match car is turned into its theme first
void themeColours(uint8_t id, uint32_t &dial, uint32_t &needle)
{
  id = themeEffective(id);
  if (id == THEME_CUSTOM)
  {
    dial = settings.themeDial;
    needle = settings.themeNeedle;
    return;
  }
  const ThemeDef &theme = *themeById(id); // always found: themeEffective only gives ids in the table
  dial = theme.dial;
  needle = theme.needle;
}

// ===== Match car =====

// the make's era that contains the year (the newest one where two overlap), else the nearest era; year 0 = the newest
uint8_t themeMatch(const char *makeKey, uint16_t year)
{
  if (!makeKey || !makeKey[0])
  {
    return THEME_NONE;
  }
  uint8_t best = THEME_NONE;  // the best theme id so far
  int bestYearsAway = 0x7FFF; // how many years the best one's era is from the car's year
  uint16_t bestFrom = 0;      // the year the best one's era starts
  for (uint8_t i = 0; i < THEME_COUNT; i++)
  {
    const ThemeDef &theme = themes[i];
    if (!theme.make || strcmp(theme.make, makeKey))
    {
      continue; // another make (or one of the can2gauge palettes)
    }
    int yearsAway; // 0 = the car's year is inside this era
    if (year == 0)
    {
      yearsAway = 0;
    }
    else if (year < theme.from)
    {
      yearsAway = theme.from - year;
    }
    else if (year > theme.to)
    {
      yearsAway = year - theme.to;
    }
    else
    {
      yearsAway = 0;
    }
    if (yearsAway < bestYearsAway || (yearsAway == bestYearsAway && theme.from > bestFrom))
    {
      best = theme.id;
      bestYearsAway = yearsAway;
      bestFrom = theme.from;
    }
  }
  return best;
}

// a VIN has been decoded: keep the make's theme and logo, and re-theme the gauge if it is on Match car
void themeVehicleDetected(const char *logoKey, uint16_t year)
{
  const char *key = logoKey; // the make's logo key ("vw"), "" when none
  if (key == NULL)
  {
    key = "";
  }
  const uint8_t id = themeMatch(themeKeyFor(key), year);
  if (id == settings.themeMatched && !strcmp(settings.matchedLogo, key))
  {
    return; // nothing new
  }
  settings.themeMatched = id;
  copyStr(settings.matchedLogo, sizeof(settings.matchedLogo), key);
  settingsDirty = true;
  const char *matchedText; // for the debug line
  if (id == THEME_NONE)
  {
    matchedText = "no theme (Custom)";
  }
  else
  {
    matchedText = themeName(id);
  }
  DEBUG("theme: VIN make '%s' %u -> %s", key, year, matchedText);
  if (settings.theme == THEME_MATCH)
  {
    uiApplyTheme();
  }
}

// ===== Boot logos =====

// how many boot logos there are
uint8_t logoCount()
{
  return LOGO_COUNT;
}

// the logo in row index of the table (a row past the end gives the first logo)
const LogoDef &logoAt(uint8_t index)
{
  if (index >= LOGO_COUNT)
  {
    index = 0; // past the end: the first logo
  }
  return logos[index];
}

// the make's name for a logo key ("vw" -> "Volkswagen"), NULL for an unknown key
const char *logoName(const char *key)
{
  if (!key)
  {
    return NULL;
  }
  for (uint8_t i = 0; i < LOGO_COUNT; i++)
  {
    if (!strcmp(logos[i].key, key))
    {
      return logos[i].name;
    }
  }
  return NULL;
}

// which logo boot logo option 5 shows: the make chosen in the web UI, else the VIN's make on Match car, else the make
// of the theme in use (NULL for the can2gauge palettes and Custom)
const char *themeLogoKey()
{
  if (settings.logoMake[0])
  {
    return settings.logoMake;
  }
  if (settings.theme == THEME_MATCH && settings.matchedLogo[0])
  {
    return settings.matchedLogo;
  }
  const ThemeDef *found = themeById(themeEffective(settings.theme));
  if (found == NULL)
  {
    return NULL;
  }
  const ThemeDef &theme = *found; // the table row it found
  return theme.make;
}

// ===== Colours from the web UI =====

// "#RRGGBB" or "RRGGBB" -> 0xRRGGBB; anything else gives fallback
uint32_t themeParseHex(const char *text, uint32_t fallback)
{
  if (!text)
  {
    return fallback;
  }
  if (text[0] == '#')
  {
    text++; // skip the #
  }
  if (strlen(text) != 6)
  {
    return fallback;
  }
  char *end = NULL; // where strtoul stopped reading
  const uint32_t value = strtoul(text, &end, 16);
  if (end != NULL && end[0] == '\0')
  {
    return value; // all six were hex digits
  }
  return fallback;
}

// ===== Detail functions =====

// two makes share a theme with a sister brand: the theme key for a logo key (vauxhall -> opel, citroen -> peugeot)
static const char *themeKeyFor(const char *logoKey)
{
  if (!strcmp(logoKey, "vauxhall"))
  {
    return "opel";
  }
  if (!strcmp(logoKey, "citroen"))
  {
    return "peugeot";
  }
  return logoKey;
}
