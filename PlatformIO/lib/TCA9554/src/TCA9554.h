#pragma once
#include <Arduino.h>
#include <Wire.h>

// TCA9554PWR 8-bit I2C expander. Pins are numbered 1..8 (EXIO1..EXIO8 on the can2gauge schematic = P0..P7).
// All pins are outputs after tca9554Init(); an output that is left as input (high-Z) at reset is exactly what the
// CAN termination FET wants (10k pull-up keeps Q_TERM off until the firmware decides), so call tca9554Init() early.
#define TCA9554_REG_INPUT 0x00
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_POLARITY 0x02
#define TCA9554_REG_CONFIG 0x03

bool tca9554Init(uint8_t addr, uint8_t outputDefaults, uint8_t configMask = 0x00); // configMask bit=1 -> input
void tca9554Write(uint8_t pin, bool level);
bool tca9554Read(uint8_t pin);
uint8_t tca9554Outputs();
bool tca9554Present();
