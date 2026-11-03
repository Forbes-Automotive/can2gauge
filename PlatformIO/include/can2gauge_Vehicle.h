#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// Vehicle (0.35.0) - which car the gauge is in. The VIN can come from four places, whichever answers first:
//   the OBD adapter        09 02, else 22 F190 on the engine (can2gauge_ELM.cpp)
//   the CAN broadcast      MQB / MQB Evo / MLB gateways send it by themselves in VIN_01 (0x6B4), no requests
//   OBD over the gauge's CAN   22 F190 / 09 02 to the engine, asked once when First Boot or the Your Car card asks
//   the gauge's K-line     the engine's measuring block 081 on ME7 (VIN and immobiliser ID as a text block)
// The VIN gives the make (its first 3 characters, the WMI) and, on VW-group cars, the model generation (characters
// 7-8) and the model year (character 10). The driver then confirms the car (or picks it by hand) on First Boot's
// step 4 or the Settings > Device > Your Car card, with a paint colour for its picture; the confirmed car is saved in
// NVS under its own key (outside the settings blob) and decides Match Car.
// ---------------------------------------------------------------------------

#define VIN_SRC_NONE 0          // no VIN yet
#define VIN_SRC_ADAPTER 1       // the OBD adapter (Bluetooth / WiFi ELM327)
#define VIN_SRC_CAN_BROADCAST 2 // VIN_01 on the gauge's CAN (MQB / MLB)
#define VIN_SRC_CAN_OBD 3       // asked over the gauge's CAN (22 F190 / 09 02)
#define VIN_SRC_KLINE 4         // the gauge's K-line (a text block, ME7 block 081)

// what the VIN says
struct VehicleInfo
{
  char vin[18];        // "WVWZZZ1JZ3W000000", "" = none yet
  uint8_t source;      // VIN_SRC_*
  char make[24];       // "Volkswagen", "" = WMI not in the table
  char makeKey[12];    // logo / theme key ("vw", "audi" ...), "" = unknown
  char model[32];      // VW group: "Golf IV / Bora", "" = not known
  char modelCode[3];   // VW group: VIN characters 7-8 ("1J")
  uint16_t year;       // model year from character 10 (0 = not coded)
};

// 0.36.2: how My Car was set (CarChoice.confirmed)
#define CAR_NOT_SET 0     // no car saved yet
#define CAR_FROM_DRIVER 1 // the driver pressed This Is My Car (or picked the car by hand)
#define CAR_FROM_VIN 2    // set by itself from the first VIN an OBD scan read (dongle or CAN); the driver can still change it

// the picture of My Car for the boot screen (boot logo 7): drawn by the web UI in the car's paint, deleted here
// whenever My Car changes so an old car's picture never shows
#define CAR_LOGO_PATH "/logos/car.bin"

// the car the driver confirmed (saved)
struct CarChoice
{
  uint8_t confirmed;   // CAR_NOT_SET / CAR_FROM_DRIVER / CAR_FROM_VIN"
  char vin[18];        // the VIN it was confirmed with ("" = picked by hand)
  char makeKey[12];    // "vw"
  char make[24];       // "Volkswagen"
  char modelCode[3];   // VW group generation code ("AD"), "" = none
  char model[32];      // "Tiguan II", or what the driver typed
  uint16_t year;       // model year
  char body[12];       // body type for the picture when there's no model picture ("suv", "hatch" ...)
  uint32_t paint;      // paint colour for the picture, 0xRRGGBB
};

void setupVehicle();                     // loads the saved car, listens for VIN_01 on the CAN
void vehicleTick();                      // house task: decodes a VIN the CAN broadcast delivered
bool vehicleVinValid(const char *vin);   // 17 characters, no I / O / Q
// a VIN from any source: decoded into vehicleDetected(). oldCycle = the car is pre-CAN (K-line / J1850 OBD), which
// picks the earlier of the two model years character 10 can mean when the generation doesn't settle it
void vehicleTakeVin(const char *vin, uint8_t source, bool oldCycle);
bool vehicleTakeText(const char *text, uint8_t source); // a VIN inside a text (a K-line text block); true = found one
void vehicleDetectedCopy(VehicleInfo &out);
void vehicleCarCopy(CarChoice &out);
void vehicleSetCar(const CarChoice &car); // the driver confirmed / changed the car: saved, Match Car follows it
void vehicleForgetCar();                  // back to "not confirmed"
void vehicleCarLabel(char *out, size_t outSize); // 0.36.2: My Car's name, make + model ("SEAT Ibiza IV"), "" = none
const char *vehicleSourceName(uint8_t source);
// 0.35.0: the one-off VIN read over the gauge's CAN (run by the VAG diag task, which has the ISO-TP code)
void vehicleRequestCanRead();             // First Boot / the Your Car card asks for it
bool vehicleCanReadWanted();
void vehicleCanReadDone(bool found);
void vehicleCanReadFound(const char *vin); // the VIN it read: taken by the house task (it has the stack for it)
bool vehicleCanReadBusy();                // asked and not done yet (the web UI shows "reading")
