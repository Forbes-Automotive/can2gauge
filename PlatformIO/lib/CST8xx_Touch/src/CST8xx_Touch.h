#pragma once
#include <Arduino.h>
#include <Wire.h>

// Hynitron CST816S / CST820 capacitive touch controller as fitted to the 2.1" round panel flex.
// Registers: 0x01 gesture, 0x02 finger count, 0x03/0x04 X (12 bit), 0x05/0x06 Y (12 bit), 0xA7 chip id, 0xFA IRQ ctl.
// INT pin pulses low on every touch report; we poll on INT (or every read if INT not wired).
struct TouchPoint
{
  bool pressed;
  uint16_t x;
  uint16_t y;
  uint8_t gesture; // raw gesture code from the chip (0 none, 1 down, 2 up, 3 left, 4 right, 5 tap, 0x0C long press)
};

bool touchInit(uint8_t addr, int8_t intPin, uint8_t exioReset);
void touchResetStart(uint8_t exioReset); // 0.24.28: the reset pulse only, early - touchInit then waits just what's left of 100 ms
bool touchRead(TouchPoint &point); // true when a fresh report was read
bool touchPresent();
uint8_t touchChipId();
bool touchRedetect(); // reset the controller and look for it again (after re-seating the flex); true = found

// for the web UI's Diag tab (tap the glass and watch the presses and the position change)
struct TouchStats
{
  uint32_t reads;       // reports read from the controller
  uint32_t readErrors;  // I2C reads that failed after it was found (flex loose, noise)
  uint32_t presses;     // finger down events
  uint16_t lastX;       // where the last press was (0..479)
  uint16_t lastY;
  uint8_t lastGesture;  // the chip's gesture code at that press
  uint32_t lastPressMs; // millis() of the last press, 0 = none yet
  bool pressedNow;      // a finger is on the glass
};
TouchStats touchStats();
