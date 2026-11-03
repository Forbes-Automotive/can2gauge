#include <can2gauge_Vehicle.h>
#include <Preferences.h>       // the confirmed car, saved under its own NVS key
#include <CANLine.h>           // canOnFrame: VIN_01 on the CAN
#include <can2gauge_themes.h>  // Match car: themeVehicleDetected()
#include <LittleFS.h>           // 0.36.2: deletes My Car's old boot picture when the car changes
#include <can2gauge_API.h>     // fsUsageChanged()

// ---------------------------------------------------------------------------
// Vehicle (0.35.0) - see can2gauge_Vehicle.h. The tables came from can2gauge_ELM.cpp unchanged.
// ---------------------------------------------------------------------------

#define VIN_01_ID 0x6B4 // MQB / MLB gateway: VIN_01, the VIN in three pieces (mux 0: characters 1-3, 1: 4-10, 2: 11-17)

static VehicleInfo detected;                                  // what the last VIN said
static CarChoice car;                                         // the confirmed car
static portMUX_TYPE vehicleMux = portMUX_INITIALIZER_UNLOCKED; // guards both (the web server reads them)
static char broadcastVin[18];                                 // VIN_01 pieces as they arrive (CAN receive task)
static uint8_t broadcastParts = 0;                            // which pieces are in: bit 0 = mux 0, bit 1 = 1, bit 2 = 2
static volatile bool broadcastReady = false;                  // all three in: vehicleTick decodes it
static volatile bool canReadWanted = false;                   // a VIN read over the CAN was asked for
static volatile bool canReadBusy = false;                     // ... and isn't done yet
static void saveCar();                                        // below: writes the car to NVS
static void deleteCarLogo();                                  // below: deletes My Car's boot picture

// the NVS key for the confirmed car, and the layout number stored with it (a different layout = not read)
#define CAR_KEY "car"
#define CAR_LAYOUT 1

struct Wmi
{
  const char *code; // the VIN's first three characters (or two, for a whole maker's block)
  const char *make; // the make's name, as the Your Car card shows it
  const char *key;  // make key: the logo / theme key (include/can2gauge_themes.h) and the Your Car make list (data/car.js)
};
// 0.36.1: every maker the VIN can name, so a VIN read over the OBD dongle picks the make for any car, not just the
// common ones. From the NHTSA vPIC / ISO 3780 WMI lists. First match wins, so every 3-character code goes before the
// 2-character "whole block" codes at the end (JTH Lexus before JT Toyota, JMB Mitsubishi before JM Mazda). A make
// key with no logo or theme of its own is fine: the gauge shows no logo and Match car stays on Custom.
static const Wmi wmis[] = {
    // ===== VW group =====
    {"WVW", "Volkswagen", "vw"},           // Germany: cars
    {"WVG", "Volkswagen", "vw"},           // Germany: SUVs (Touareg, Tiguan)
    {"WV1", "Volkswagen", "vw"},           // commercial (Caddy, Crafter)
    {"WV2", "Volkswagen", "vw"},           // Transporter / bus
    {"WV3", "Volkswagen", "vw"},           // trucks
    {"1VW", "Volkswagen", "vw"},           // USA
    {"3VW", "Volkswagen", "vw"},           // Mexico
    {"9BW", "Volkswagen", "vw"},           // Brazil
    {"8AW", "Volkswagen", "vw"},           // Argentina
    {"AAV", "Volkswagen", "vw"},           // South Africa
    {"LSV", "Volkswagen", "vw"},           // SAIC-VW China
    {"LFV", "Volkswagen", "vw"},           // FAW-VW China
    {"XW8", "Volkswagen", "vw"},           // Russia
    {"WAU", "Audi", "audi"},               // Germany: cars
    {"WA1", "Audi", "audi"},               // Germany: SUVs
    {"WUA", "Audi", "audi"},               // Audi Sport / quattro GmbH (RS models, R8)
    {"TRU", "Audi", "audi"},               // Hungary (TT, A3 saloon)
    {"VSS", "SEAT", "seat"},               // SEAT and Cupra both use VSS
    {"TMB", "Skoda", "skoda"},             // Czech Republic
    {"WP0", "Porsche", "porsche"},         // cars
    {"WP1", "Porsche", "porsche"},         // SUVs (Cayenne, Macan)
    {"SCB", "Bentley", "bentley"},         // UK
    {"ZHW", "Lamborghini", "lamborghini"}, // Italy
    {"VF9", "Bugatti", "bugatti"},         // France
    // ===== BMW group =====
    {"WBA", "BMW", "bmw"},                // cars
    {"WBS", "BMW", "bmw"},                // BMW M
    {"WBY", "BMW", "bmw"},                // BMW i
    {"WBX", "BMW", "bmw"},                // X models
    {"4US", "BMW", "bmw"},                // USA
    {"5UX", "BMW", "bmw"},                // USA: X models
    {"5YM", "BMW", "bmw"},                // USA: X M models
    {"WMW", "MINI", "mini"},              // UK built, Germany registered
    {"WMZ", "MINI", "mini"},              // Countryman / Paceman (Austria, Netherlands)
    {"SCA", "Rolls-Royce", "rollsroyce"}, // UK
    // ===== Mercedes-Benz =====
    {"WDB", "Mercedes-Benz", "mercedes"}, // cars
    {"WDC", "Mercedes-Benz", "mercedes"}, // SUVs
    {"WDD", "Mercedes-Benz", "mercedes"}, // cars
    {"WDF", "Mercedes-Benz", "mercedes"}, // vans (Vito, Sprinter)
    {"W1K", "Mercedes-Benz", "mercedes"}, // cars from 2019
    {"W1N", "Mercedes-Benz", "mercedes"}, // SUVs from 2019
    {"W1V", "Mercedes-Benz", "mercedes"}, // vans from 2019
    {"4JG", "Mercedes-Benz", "mercedes"}, // USA: SUVs
    {"55S", "Mercedes-Benz", "mercedes"}, // USA
    {"WME", "smart", "smart"},            // Germany / France
    // ===== Ford =====
    {"WF0", "Ford", "ford"},       // Germany
    {"SFA", "Ford", "ford"},       // UK
    {"NM0", "Ford", "ford"},       // Turkey (Transit)
    {"MAJ", "Ford", "ford"},       // India
    {"6FP", "Ford", "ford"},       // Australia
    {"1FA", "Ford", "ford"},       // USA: cars
    {"1FB", "Ford", "ford"},       // USA: buses
    {"1FC", "Ford", "ford"},       // USA: chassis
    {"1FD", "Ford", "ford"},       // USA: trucks
    {"1FM", "Ford", "ford"},       // USA: SUVs
    {"1FT", "Ford", "ford"},       // USA: trucks
    {"2FA", "Ford", "ford"},       // Canada
    {"2FM", "Ford", "ford"},       // Canada: SUVs
    {"3FA", "Ford", "ford"},       // Mexico
    {"1LN", "Lincoln", "lincoln"}, // USA
    {"2LM", "Lincoln", "lincoln"}, // Canada
    {"5LM", "Lincoln", "lincoln"}, // USA: SUVs
    // ===== Jaguar Land Rover and other UK makes =====
    {"SAJ", "Jaguar", "jaguar"},        // cars
    {"SAD", "Jaguar", "jaguar"},        // SUVs (F-Pace, E-Pace, I-Pace)
    {"SAL", "Land Rover", "landrover"}, // Land Rover and Range Rover
    {"SCF", "Aston Martin", "aston"},   // UK
    {"SCC", "Lotus", "lotus"},          // UK
    {"SBM", "McLaren", "mclaren"},      // UK
    {"SAR", "Rover", "rover"},          // Rover / MG Rover
    {"SAX", "Rover", "rover"},          // Austin Rover
    {"SB1", "Toyota", "toyota"},        // Toyota UK (Burnaston)
    {"SHH", "Honda", "honda"},          // Honda UK (Swindon)
    {"SHS", "Honda", "honda"},          // Honda UK: CR-V
    {"SJN", "Nissan", "nissan"},        // Nissan UK (Sunderland)
    // ===== Stellantis: Fiat, Alfa, Peugeot, Citroen, Opel, Chrysler =====
    {"ZFA", "Fiat", "fiat"},             // Fiat and Abarth
    {"ZFC", "Fiat", "fiat"},             // Fiat Professional (vans)
    {"ZAR", "Alfa Romeo", "alfa"},       // cars
    {"ZAS", "Alfa Romeo", "alfa"},       // SUVs (Stelvio, Tonale)
    {"ZLA", "Lancia", "lancia"},         // Italy
    {"ZAM", "Maserati", "maserati"},     // Italy
    {"ZFF", "Ferrari", "ferrari"},       // Italy
    {"ZAC", "Jeep", "jeep"},             // Italy (Renegade, Compass)
    {"VF3", "Peugeot", "peugeot"},       // France
    {"VR3", "Peugeot", "peugeot"},       // France, from 2018
    {"VF7", "Citroen", "citroen"},       // France
    {"VR7", "Citroen", "citroen"},       // France, from 2018
    {"VR1", "DS", "ds"},                 // France
    {"W0L", "Opel / Vauxhall", "opel"},  // Germany
    {"W0V", "Opel / Vauxhall", "opel"},  // Germany, from 2017
    {"VXK", "Opel / Vauxhall", "opel"},  // PSA built (Corsa F, Mokka B)
    {"1C3", "Chrysler", "chrysler"},     // USA: cars
    {"2C3", "Chrysler", "chrysler"},     // Canada (300, Challenger, Charger)
    {"1C4", "Jeep", "jeep"},             // USA: SUVs / MPVs (Jeep, Chrysler Pacifica)
    {"1J4", "Jeep", "jeep"},             // USA
    {"1J8", "Jeep", "jeep"},             // USA
    {"1C6", "Ram", "ram"},               // USA
    {"3C6", "Ram", "ram"},               // Mexico
    {"3D7", "Ram", "ram"},               // Mexico
    {"1B3", "Dodge", "dodge"},           // USA: cars
    {"1B7", "Dodge", "dodge"},           // USA: trucks
    {"2B3", "Dodge", "dodge"},           // Canada
    {"1D7", "Dodge", "dodge"},           // USA: trucks
    {"3C3", "Fiat", "fiat"},             // Mexico (500 for America)
    // ===== Renault group =====
    {"VF1", "Renault", "renault"}, // France
    {"VF6", "Renault", "renault"}, // France: trucks / vans
    {"UU1", "Dacia", "dacia"},     // Romania
    {"KNM", "Renault", "renault"}, // Renault Samsung, Korea
    // ===== Volvo and Saab =====
    {"YV1", "Volvo", "volvo"},       // cars
    {"YV4", "Volvo", "volvo"},       // SUVs
    {"LYV", "Volvo", "volvo"},       // China
    {"7JR", "Volvo", "volvo"},       // USA
    {"LPS", "Polestar", "polestar"}, // China
    {"YS3", "Saab", "saab"},         // Sweden
    // ===== Japan =====
    {"JH4", "Acura", "acura"},           // before the JH Honda block
    {"19U", "Acura", "acura"},           // USA
    {"5J8", "Acura", "acura"},           // USA: SUVs
    {"JHM", "Honda", "honda"},           // cars
    {"JHL", "Honda", "honda"},           // SUVs
    {"1HG", "Honda", "honda"},           // USA
    {"19X", "Honda", "honda"},           // USA
    {"2HG", "Honda", "honda"},           // Canada
    {"2HK", "Honda", "honda"},           // Canada: SUVs
    {"5FN", "Honda", "honda"},           // USA: SUVs / MPVs
    {"5J6", "Honda", "honda"},           // USA: SUVs
    {"JNK", "Infiniti", "infiniti"},     // before the JN Nissan block
    {"JNR", "Infiniti", "infiniti"},     // SUVs
    {"5N3", "Infiniti", "infiniti"},     // USA
    {"JN1", "Nissan", "nissan"},         // cars
    {"JN8", "Nissan", "nissan"},         // SUVs
    {"VSK", "Nissan", "nissan"},         // Spain
    {"1N4", "Nissan", "nissan"},         // USA
    {"1N6", "Nissan", "nissan"},         // USA: trucks
    {"3N1", "Nissan", "nissan"},         // Mexico
    {"5N1", "Nissan", "nissan"},         // USA: SUVs
    {"JTH", "Lexus", "lexus"},           // before the JT Toyota block
    {"JTJ", "Lexus", "lexus"},           // SUVs
    {"2T2", "Lexus", "lexus"},           // Canada
    {"NMT", "Toyota", "toyota"},         // Turkey
    {"VNK", "Toyota", "toyota"},         // France (Yaris)
    {"MR0", "Toyota", "toyota"},         // Thailand (Hilux)
    {"4T1", "Toyota", "toyota"},         // USA
    {"4T3", "Toyota", "toyota"},         // USA: SUVs
    {"5TD", "Toyota", "toyota"},         // USA: SUVs / MPVs
    {"5TF", "Toyota", "toyota"},         // USA: trucks
    {"2T1", "Toyota", "toyota"},         // Canada
    {"2T3", "Toyota", "toyota"},         // Canada: SUVs
    {"JDA", "Daihatsu", "daihatsu"},     // Japan
    {"JMB", "Mitsubishi", "mitsubishi"}, // Europe, before the JM Mazda block
    {"JA3", "Mitsubishi", "mitsubishi"}, // cars
    {"JA4", "Mitsubishi", "mitsubishi"}, // SUVs
    {"ML3", "Mitsubishi", "mitsubishi"}, // Thailand
    {"JM1", "Mazda", "mazda"},           // cars
    {"JM3", "Mazda", "mazda"},           // SUVs
    {"JMZ", "Mazda", "mazda"},           // Europe
    {"1YV", "Mazda", "mazda"},           // USA
    {"3MZ", "Mazda", "mazda"},           // Mexico
    {"JF1", "Subaru", "subaru"},         // cars
    {"JF2", "Subaru", "subaru"},         // SUVs
    {"4S3", "Subaru", "subaru"},         // USA
    {"4S4", "Subaru", "subaru"},         // USA: SUVs
    {"JSA", "Suzuki", "suzuki"},         // Japan: cars for Europe
    {"JS2", "Suzuki", "suzuki"},         // Japan
    {"JS3", "Suzuki", "suzuki"},         // Japan: SUVs
    {"TSM", "Suzuki", "suzuki"},         // Hungary (Swift, Vitara, SX4)
    {"MA3", "Suzuki", "suzuki"},         // India (Maruti Suzuki)
    {"JAA", "Isuzu", "isuzu"},           // trucks
    {"JAC", "Isuzu", "isuzu"},           // SUVs
    // ===== Korea =====
    {"KMT", "Genesis", "genesis"},     // before the KM Hyundai block
    {"KMH", "Hyundai", "hyundai"},     // cars
    {"KM8", "Hyundai", "hyundai"},     // SUVs
    {"TMA", "Hyundai", "hyundai"},     // Czech Republic (i30, Tucson)
    {"NLH", "Hyundai", "hyundai"},     // Turkey (i10, i20)
    {"5NP", "Hyundai", "hyundai"},     // USA
    {"5NM", "Hyundai", "hyundai"},     // USA: SUVs
    {"KNA", "Kia", "kia"},             // cars
    {"KND", "Kia", "kia"},             // SUVs
    {"U5Y", "Kia", "kia"},             // Slovakia (Ceed, Sportage)
    {"U6Y", "Kia", "kia"},             // Slovakia
    {"5XY", "Kia", "kia"},             // USA: SUVs
    {"3KP", "Kia", "kia"},             // Mexico
    {"KPT", "SsangYong", "ssangyong"}, // Korea
    {"KLA", "Daewoo", "daewoo"},       // Korea
    {"KL1", "Chevrolet", "chevrolet"}, // GM Korea
    // ===== GM and Tesla =====
    {"1G1", "Chevrolet", "chevrolet"}, // USA: cars
    {"1GC", "Chevrolet", "chevrolet"}, // USA: trucks
    {"1GN", "Chevrolet", "chevrolet"}, // USA: SUVs
    {"2G1", "Chevrolet", "chevrolet"}, // Canada
    {"3G1", "Chevrolet", "chevrolet"}, // Mexico
    {"1G4", "Buick", "buick"},         // USA
    {"1G6", "Cadillac", "cadillac"},   // USA: cars
    {"1GY", "Cadillac", "cadillac"},   // USA: SUVs
    {"1GT", "GMC", "gmc"},             // USA: trucks
    {"1GK", "GMC", "gmc"},             // USA: SUVs
    {"1G2", "Pontiac", "pontiac"},     // USA
    {"1G8", "Saturn", "saturn"},       // USA
    {"5YJ", "Tesla", "tesla"},         // USA
    {"7SA", "Tesla", "tesla"},         // USA, from 2021
    {"LRW", "Tesla", "tesla"},         // China
    {"XP7", "Tesla", "tesla"},         // Germany (Berlin)
    // ===== China and Russia =====
    {"LSJ", "MG", "mg"},               // SAIC MG
    {"LGX", "BYD", "byd"},             // China
    {"LC0", "BYD", "byd"},             // China
    {"L6T", "Geely", "geely"},         // China
    {"LGW", "Great Wall", "greatwall"}, // China (and Ora, Haval)
    {"XTA", "Lada", "lada"},           // Russia
    // ===== whole-block fallbacks (2 characters): a maker's code not listed above =====
    {"WV", "Volkswagen", "vw"},
    {"WA", "Audi", "audi"},
    {"WP", "Porsche", "porsche"},
    {"WB", "BMW", "bmw"},
    {"WD", "Mercedes-Benz", "mercedes"},
    {"W1", "Mercedes-Benz", "mercedes"},
    {"WF", "Ford", "ford"},
    {"1F", "Ford", "ford"},
    {"YV", "Volvo", "volvo"},
    {"JT", "Toyota", "toyota"},
    {"JH", "Honda", "honda"},
    {"JN", "Nissan", "nissan"},
    {"JM", "Mazda", "mazda"},
    {"JF", "Subaru", "subaru"},
    {"JS", "Suzuki", "suzuki"},
    {"KM", "Hyundai", "hyundai"},
    {"KN", "Kia", "kia"},
    {"ZF", "Fiat", "fiat"},
};

// 0.36.1: the WMIs whose VINs carry the VW-group model code in characters 7-8 (European-layout VINs: WVWZZZ1JZ...).
// US, Mexican and Chinese VW VINs use those places for something else, so they're left out.
static const char *vwModelWmis[] = {"WVW", "WVG", "WV1", "WV2", "WAU", "WA1", "WUA", "TRU", "VSS", "TMB"};

// VW-group model code = VIN chars 7-8 [U: best effort, the code alone doesn't always pin the model]. year = a typical
// model year of that generation (0 = spans an illumination change): it settles the VIN's 30-year cycle and gives
// Match car the generation's era, which is what the cluster colour follows.
struct ModelCode
{
  const char *code;
  const char *model;
  uint16_t year;
};
// 0.37.8: BMW's model from VIN characters 4-5 (the start of BMW's 4-character "type code", e.g. KS82). Only codes seen
// on real VINs are listed (bench 2026-11-01: WBAKS82... is an X5 F15); add more as real VINs confirm them.
static const ModelCode bmwModels[] = {
    {"FE", "X5 (E70)", 2008},
    {"KR", "X5 (F15)", 2015},
    {"KS", "X5 (F15)", 2016},
    {"CR", "X5 (G05)", 2019},
};

static const ModelCode vwModels[] = {
    {"17", "Golf I", 1980},           // 0.36.1: older VW-group cars, from here to Ibiza 6K / KJ
    {"19", "Golf II", 1988},
    {"1G", "Golf II / Jetta II", 1990},
    {"50", "Corrado", 1991},
    {"53", "Scirocco", 1985},
    {"25", "Transporter T3", 1985},
    {"70", "Transporter T4", 1997},
    {"6X", "Lupo", 2001},
    {"8X", "A1 8X", 2013},
    {"GB", "A1 GB", 2020},
    {"6K", "Ibiza II", 1997},
    {"KJ", "Ibiza V", 2019},
    {"1H", "Golf III / Vento", 1995},
    {"1J", "Golf IV / Bora", 2001},
    {"1K", "Golf V / Jetta", 2006},
    {"5K", "Golf VI", 2011},
    {"AJ", "Golf VI", 2011},
    {"AU", "Golf VII", 2016},
    {"5G", "Golf VII", 2016},
    {"CD", "Golf 8", 2022},
    {"9C", "New Beetle", 2003},
    {"5C", "Beetle", 2014},
    {"3B", "Passat B5", 2001},
    {"3C", "Passat B6 / B7", 0},
    {"3G", "Passat B8", 2017},
    {"35", "Passat CC", 2011},
    {"1T", "Touran", 0},
    {"5T", "Touran II", 2018},
    {"5N", "Tiguan", 2012},
    {"AD", "Tiguan II", 2019},
    {"6N", "Polo III", 1998},
    {"9N", "Polo IV", 2005},
    {"6R", "Polo V", 2013},
    {"AW", "Polo VI", 2020},
    {"13", "Scirocco III", 2011},
    {"7N", "Sharan II", 2014},
    {"1F", "Eos", 2010},
    {"7H", "Transporter T5", 2008},
    {"8L", "A3 8L", 2000},
    {"8P", "A3 8P", 2007},
    {"8V", "A3 8V", 2016},
    {"8Y", "A3 8Y", 2022},
    {"8N", "TT 8N", 2002},
    {"8J", "TT 8J", 2010},
    {"FV", "TT 8S", 2017},
    {"8D", "A4 B5", 1998},
    {"8E", "A4 B6 / B7", 2004},
    {"8K", "A4 B8", 2011},
    {"8W", "A4 B9", 2018},
    {"4B", "A6 C5", 2001},
    {"4F", "A6 C6", 2008},
    {"4G", "A6 C7", 2014},
    {"1M", "Leon I", 2002},
    {"1P", "Leon II", 2009},
    {"5F", "Leon III", 2015},
    {"KL", "Leon IV", 2022},
    {"6L", "Ibiza III", 2005},
    {"6J", "Ibiza IV", 2012},
    {"1U", "Octavia I", 2001},
    {"1Z", "Octavia II", 2008},
    {"5E", "Octavia III", 2016},
    {"NX", "Octavia IV", 2022},
    {"6Y", "Fabia I", 2003},
    {"5J", "Fabia II", 2010},
    {"3U", "Superb I", 2005},
    {"3T", "Superb II", 2011},
    {"3V", "Superb III", 2018},
};

bool vehicleVinValid(const char *vin)
{
  if (strlen(vin) != 17)
  {
    return false;
  }
  for (int i = 0; i < 17; i++)
  {
    if (!isalnum((unsigned char)vin[i]) || vin[i] == 'I' || vin[i] == 'O' || vin[i] == 'Q')
    {
      return false;
    }
  }
  return true;
}

// Match car follows the confirmed car if there is one, otherwise the VIN just read
static void matchCar()
{
  CarChoice choice;
  VehicleInfo info;
  vehicleCarCopy(choice);
  vehicleDetectedCopy(info);
  if (choice.confirmed && choice.makeKey[0])
  {
    themeVehicleDetected(choice.makeKey, choice.year);
  }
  else if (info.vin[0])
  {
    themeVehicleDetected(info.makeKey, info.year);
  }
}

void vehicleTakeVin(const char *vin, uint8_t source, bool oldCycle)
{
  if (!vehicleVinValid(vin))
  {
    return;
  }
  VehicleInfo info;
  memset(&info, 0, sizeof(info));
  copyStr(info.vin, sizeof(info.vin), vin);
  info.source = source;
  uint16_t wmiCount = sizeof(wmis) / sizeof(wmis[0]); // how many rows the table has (over 200 now, so not a uint8_t)
  for (uint16_t i = 0; i < wmiCount; i++)
  {
    if (strncmp(vin, wmis[i].code, strlen(wmis[i].code)) == 0)
    {
      copyStr(info.make, sizeof(info.make), wmis[i].make);
      copyStr(info.makeKey, sizeof(info.makeKey), wmis[i].key);
      break;
    }
  }
  // VW-group model code: only on the WMIs that put it in characters 7-8
  bool vag = false; // true = this VIN carries a VW-group model code
  uint8_t vwWmiCount = sizeof(vwModelWmis) / sizeof(vwModelWmis[0]);
  for (uint8_t i = 0; i < vwWmiCount; i++)
  {
    if (strncmp(vin, vwModelWmis[i], 3) == 0)
    {
      vag = true;
    }
  }
  uint16_t modelYear = 0; // typical year of the VW-group generation, 0 = unknown
  if (vag)
  {
    info.modelCode[0] = vin[6];
    info.modelCode[1] = vin[7];
    info.modelCode[2] = '\0';
    uint8_t modelCount = sizeof(vwModels) / sizeof(vwModels[0]);
    for (uint8_t i = 0; i < modelCount; i++)
    {
      if (!strncmp(vwModels[i].code, &vin[6], 2))
      {
        copyStr(info.model, sizeof(info.model), vwModels[i].model);
        modelYear = vwModels[i].year;
        break;
      }
    }
  }
  // 0.37.8: BMW's model from characters 4-5 (only the codes in bmwModels; the rest stay make + year)
  if (!strcmp(info.makeKey, "bmw"))
  {
    uint8_t bmwCount = sizeof(bmwModels) / sizeof(bmwModels[0]);
    for (uint8_t i = 0; i < bmwCount; i++)
    {
      if (!strncmp(bmwModels[i].code, &vin[3], 2))
      {
        copyStr(info.model, sizeof(info.model), bmwModels[i].model);
        modelYear = bmwModels[i].year;
        break;
      }
    }
  }
  // model year (position 10): a 30-year cycle. The generation's year picks the cycle when known; otherwise a K-line /
  // J1850 car is the earlier cycle, and anything else the later cycle unless that would be a future model year.
  static const char yearCodes[] = "ABCDEFGHJKLMNPRSTVWXY123456789";
  const char *yearCode = strchr(yearCodes, vin[9]);
  if (yearCode && vin[9])
  {
    const int earlyYear = 1980 + (int)(yearCode - yearCodes);
    const int lateYear = earlyYear + 30;
    int year = earlyYear;
    if (modelYear)
    {
      if (abs(lateYear - modelYear) < abs(earlyYear - modelYear))
      {
        year = lateYear;
      }
    }
    else if (!oldCycle && lateYear <= 2027)
    {
      year = lateYear;
    }
    info.year = year;
  }
  portENTER_CRITICAL(&vehicleMux);
  detected = info;
  portEXIT_CRITICAL(&vehicleMux);
  DEBUG("Vehicle: VIN %s from %s: %s %s %u", info.vin, vehicleSourceName(source), info.make, info.model, info.year);
  // 0.36.2: the first OBD scan (the dongle, or the gauge's CAN) that names the make sets My Car by itself, when no car
  // is saved yet. A saved car is never replaced this way (a dongle tried in a friend's car mustn't change it). The
  // K-line's text block isn't an OBD scan, so it only fills in the "detected" car as before.
  bool isObdScan = source == VIN_SRC_ADAPTER || source == VIN_SRC_CAN_BROADCAST || source == VIN_SRC_CAN_OBD;
  CarChoice saved;
  vehicleCarCopy(saved);
  if (isObdScan && info.make[0] && saved.confirmed == CAR_NOT_SET)
  {
    CarChoice fromVin;
    memset(&fromVin, 0, sizeof(fromVin));
    copyStr(fromVin.vin, sizeof(fromVin.vin), info.vin);
    copyStr(fromVin.makeKey, sizeof(fromVin.makeKey), info.makeKey);
    copyStr(fromVin.make, sizeof(fromVin.make), info.make);
    copyStr(fromVin.modelCode, sizeof(fromVin.modelCode), info.modelCode);
    copyStr(fromVin.model, sizeof(fromVin.model), info.model);
    fromVin.year = info.year;
    fromVin.paint = 0x8A96A3; // the pictures' grey until the driver picks a paint colour
    portENTER_CRITICAL(&vehicleMux);
    car = fromVin;
    car.confirmed = CAR_FROM_VIN;
    portEXIT_CRITICAL(&vehicleMux);
    saveCar();
    deleteCarLogo();
    DEBUG("Vehicle: My Car set from the VIN: %s %s", fromVin.make, fromVin.model);
  }
  // Match car: the generation decides the illumination (a late Golf V is still blue), so its year wins over the VIN's
  if (modelYear && car.confirmed != CAR_FROM_DRIVER)
  {
    themeVehicleDetected(info.makeKey, modelYear);
  }
  else
  {
    matchCar();
  }
}

bool vehicleTakeText(const char *text, uint8_t source)
{
  // a VIN in a text block ("WVWZZZ1JZ3W386752 VWZ7Z0B3456789" on ME7 block 081): 17 VIN characters in a row
  const size_t length = strlen(text);
  for (size_t start = 0; start + 17 <= length; start++)
  {
    char candidate[18];
    memcpy(candidate, text + start, 17);
    candidate[17] = '\0';
    bool startsWord = start == 0 || text[start - 1] == ' ';
    bool endsWord = start + 17 == length || text[start + 17] == ' ';
    if (startsWord && endsWord && vehicleVinValid(candidate))
    {
      vehicleTakeVin(candidate, source, source == VIN_SRC_KLINE);
      return true;
    }
  }
  return false;
}

// CAN receive task: VIN_01 pieces (keep it short: copy the bytes, vehicleTick decodes)
static void canHook(const twai_message_t &message)
{
  if (message.extd || message.identifier != VIN_01_ID || message.data_length_code < 8 || broadcastReady)
  {
    return;
  }
  const uint8_t mux = message.data[0] & 0x03; // VIN_01_MUX: bits 0-1 of byte 0
  if (mux == 0)
  {
    broadcastVin[0] = (char)message.data[5]; // VIN_1
    broadcastVin[1] = (char)message.data[6]; // VIN_2
    broadcastVin[2] = (char)message.data[7]; // VIN_3
    broadcastParts = broadcastParts | 0x01;
  }
  else if (mux == 1)
  {
    for (uint8_t i = 0; i < 7; i++)
    {
      broadcastVin[3 + i] = (char)message.data[1 + i]; // VIN_4 .. VIN_10
    }
    broadcastParts = broadcastParts | 0x02;
  }
  else if (mux == 2)
  {
    for (uint8_t i = 0; i < 7; i++)
    {
      broadcastVin[10 + i] = (char)message.data[1 + i]; // VIN_11 .. VIN_17
    }
    broadcastParts = broadcastParts | 0x04;
  }
  if (broadcastParts == 0x07)
  {
    broadcastVin[17] = '\0';
    broadcastReady = true;
  }
}

void vehicleTick()
{
  if (!broadcastReady)
  {
    return;
  }
  char vin[18];
  copyStr(vin, sizeof(vin), broadcastVin);
  VehicleInfo info;
  vehicleDetectedCopy(info);
  if (strcmp(vin, info.vin) != 0)
  {
    vehicleTakeVin(vin, VIN_SRC_CAN_BROADCAST, false);
  }
  // listen again only after a while: the gateway repeats it every second or so, once is enough
  static uint32_t heardMs = 0;
  if (heardMs == 0)
  {
    heardMs = millis();
  }
  if (millis() - heardMs > 60000)
  {
    heardMs = 0;
    broadcastParts = 0;
    broadcastReady = false;
  }
}

void vehicleDetectedCopy(VehicleInfo &out)
{
  portENTER_CRITICAL(&vehicleMux);
  out = detected;
  portEXIT_CRITICAL(&vehicleMux);
}

void vehicleCarCopy(CarChoice &out)
{
  portENTER_CRITICAL(&vehicleMux);
  out = car;
  portEXIT_CRITICAL(&vehicleMux);
}

// 0.36.2: My Car changed: its boot picture (drawn by the web UI) is out of date, so it goes; the web UI draws the new
// one the next time it's open
static void deleteCarLogo()
{
  if (LittleFS.exists(CAR_LOGO_PATH))
  {
    LittleFS.remove(CAR_LOGO_PATH);
    fsUsageChanged(); // the space used changed
  }
}

static void saveCar()
{
  Preferences prefs;
  prefs.begin("can2gauge", false);
  uint8_t blob[1 + sizeof(CarChoice)];
  blob[0] = CAR_LAYOUT; // the layout number first, then the car
  CarChoice copy;
  vehicleCarCopy(copy);
  memcpy(&blob[1], &copy, sizeof(copy));
  prefs.putBytes(CAR_KEY, blob, sizeof(blob));
  prefs.end();
}

void vehicleSetCar(const CarChoice &choice)
{
  portENTER_CRITICAL(&vehicleMux);
  car = choice;
  car.confirmed = CAR_FROM_DRIVER;
  car.paint = car.paint & 0xFFFFFF;
  portEXIT_CRITICAL(&vehicleMux);
  saveCar();
  deleteCarLogo(); // 0.36.2: the web UI uploads the new picture straight after
  DEBUG("Vehicle: confirmed %s %s %u", choice.make, choice.model, choice.year);
  matchCar();
}

void vehicleForgetCar()
{
  portENTER_CRITICAL(&vehicleMux);
  memset(&car, 0, sizeof(car));
  portEXIT_CRITICAL(&vehicleMux);
  saveCar();
  deleteCarLogo(); // 0.36.2: no car, no picture
  matchCar();
}

// 0.36.2: My Car's name for the boot screen and the web UI: make and model ("SEAT Ibiza IV"), just the make when the
// model isn't known, "" when no car is saved
void vehicleCarLabel(char *out, size_t outSize)
{
  CarChoice saved;
  vehicleCarCopy(saved);
  out[0] = '\0';
  if (saved.confirmed == CAR_NOT_SET)
  {
    return;
  }
  if (saved.model[0])
  {
    snprintf(out, outSize, "%s %s", saved.make, saved.model);
  }
  else
  {
    copyStr(out, outSize, saved.make);
  }
}

const char *vehicleSourceName(uint8_t source)
{
  switch (source)
  {
  case VIN_SRC_ADAPTER:
    return "OBD adapter";
  case VIN_SRC_CAN_BROADCAST:
    return "CAN broadcast";
  case VIN_SRC_CAN_OBD:
    return "OBD over CAN";
  case VIN_SRC_KLINE:
    return "K-line";
  default:
    return "none";
  }
}

void vehicleRequestCanRead()
{
  canReadBusy = true;
  canReadWanted = true;
}

bool vehicleCanReadWanted()
{
  return canReadWanted;
}

void vehicleCanReadDone(bool found)
{
  (void)found;
  canReadWanted = false;
  canReadBusy = false;
}

bool vehicleCanReadBusy()
{
  return canReadBusy;
}

void setupVehicle()
{
  DEBUG("Setting up the vehicle...");
  memset(&detected, 0, sizeof(detected));
  memset(&car, 0, sizeof(car));
  Preferences prefs;
  prefs.begin("can2gauge", true); // read only
  uint8_t blob[1 + sizeof(CarChoice)];
  if (prefs.getBytesLength(CAR_KEY) == sizeof(blob))
  {
    prefs.getBytes(CAR_KEY, blob, sizeof(blob));
    if (blob[0] == CAR_LAYOUT)
    {
      memcpy(&car, &blob[1], sizeof(car));
      car.vin[sizeof(car.vin) - 1] = '\0'; // strings always end, whatever was stored
      car.makeKey[sizeof(car.makeKey) - 1] = '\0';
      car.make[sizeof(car.make) - 1] = '\0';
      car.modelCode[sizeof(car.modelCode) - 1] = '\0';
      car.model[sizeof(car.model) - 1] = '\0';
      car.body[sizeof(car.body) - 1] = '\0';
    }
  }
  prefs.end();
  canOnFrame(canHook);
  DEBUG("Set up the vehicle!");
}
