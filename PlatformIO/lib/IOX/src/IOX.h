#pragma once
#include <Arduino.h>
#include <Wire.h>

// ---------------------------------------------------------------------------
// V3.4 board: ONE output = DRV8876 half-bridge (OUT1) on the TCA9554 @0x20, P4/P5 = IN1/IN2 (PWM mode).
//   off            IN1/IN2 = 00  -> coast, OUT hi-Z
//   on, high-side  IN1/IN2 = 10  -> OUT = 12 V  (load wired from OUT to GND)
//   on, low-side   IN1/IN2 = 11  -> OUT = GND   (load wired from 12 V to OUT)
// The mode is a web-UI setting; the driver never sits in the "other" on-state, so a load wired for one mode is simply
// off in the other. nFAULT (P7, active low) reports over-current / over-temperature (the driver retries on its own).
// P6 is the dash-light input (lights on = HIGH before the invert setting).
// ---------------------------------------------------------------------------
bool ioxInit(uint8_t tcaAddr);              // true when the TCA9554 answered
void ioxSetMode(bool lowSide);               // false = high-side (default), true = low-side
bool ioxLowSide();
void ioxSet(uint8_t channel, bool on);       // channel 0 (IOX_OUT)
bool ioxGet(uint8_t channel);
uint16_t ioxState();                          // bit n = channel n
void ioxAllOff();
bool ioxPresent();
bool ioxFault();                              // DRV8876 nFAULT low
bool ioxDashLightsRaw();                      // P6 level (true = input high)
const char *ioxChannelName(uint8_t channel); // "OUT"
