#include "CST8xx_Touch.h"
#include <TCA9554.h>

static uint8_t touchAddr = 0x15;
static int8_t touchIntPin = -1;
static bool touchOk = false;
static uint8_t chipId = 0;
static TouchPoint lastPoint = {false, 0, 0, 0};
static uint8_t touchExioReset = 0;   // TCA9554 pin of TP_RST (kept for touchRedetect)
static TouchStats stats = {};        // Diag tab

static int regRead(uint8_t reg, uint8_t *buf, uint8_t len)
{
  Wire.beginTransmission(touchAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
  {
    return -1;
  }
  if (Wire.requestFrom((int)touchAddr, (int)len) != len)
  {
    return -1;
  }
  for (uint8_t byteIndex = 0; byteIndex < len; byteIndex++)
  {
    buf[byteIndex] = Wire.read();
  }
  return len;
}

static bool regWrite(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(touchAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// 0.24.28: when the reset pulse was let go (0 = not started early). The touch chip wants 100 ms after it before it
// answers; started before the panel's set-up (lvglInit), that wait passes while the panel does its own ~260 ms.
static uint32_t resetReleasedMs = 0;

void touchResetStart(uint8_t exioReset)
{
  tca9554Write(exioReset, false); // reset pulse through the TCA9554 (TP_RST)
  delay(10);
  tca9554Write(exioReset, true);
  resetReleasedMs = millis();
  if (resetReleasedMs == 0)
  {
    resetReleasedMs = 1; // 0 means "not started"
  }
}

bool touchInit(uint8_t addr, int8_t intPin, uint8_t exioReset)
{
  touchAddr = addr;
  touchIntPin = intPin;
  touchExioReset = exioReset;
  if (touchIntPin >= 0)
  {
    pinMode(touchIntPin, INPUT_PULLUP);
  }
  if (resetReleasedMs == 0)
  {
    touchResetStart(exioReset); // not started early: do the reset pulse now
  }
  while (millis() - resetReleasedMs < 100) // the chip's 100 ms after reset (already gone if the panel ran meanwhile)
  {
    delay(1);
  }
  resetReleasedMs = 0; // a later touchInit / redetect does its own reset
  uint8_t id = 0;
  touchOk = regRead(0xA7, &id, 1) == 1;
  chipId = id;
  if (touchOk)
  {
    regWrite(0xFA, 0x60); // IRQ on touch + change (EnTouch | EnChange), keeps INT pulsing while pressed
    regWrite(0xFE, 0x01); // disable auto sleep so the first tap is never lost
  }
  return touchOk;
}

bool touchRead(TouchPoint &point)
{
  if (!touchOk)
  {
    point = lastPoint;
    return false;
  }
  uint8_t report[6];
  if (regRead(0x01, report, 6) != 6)
  {
    stats.readErrors++;
    point = lastPoint;
    return false;
  }
  stats.reads++;
  const uint8_t fingers = report[1] & 0x0F;
  point.gesture = report[0];
  point.pressed = fingers > 0;
  if (point.pressed)
  {
    point.x = ((report[2] & 0x0F) << 8) | report[3];
    point.y = ((report[4] & 0x0F) << 8) | report[5];
    if (point.x > 479)
    {
      point.x = 479;
    }
    if (point.y > 479)
    {
      point.y = 479;
    }
  }
  else
  {
    point.x = lastPoint.x;
    point.y = lastPoint.y;
  }
  if (point.pressed && !lastPoint.pressed)
  {
    // a new finger down: remember it for the Diag tab
    stats.presses++;
    stats.lastX = point.x;
    stats.lastY = point.y;
    stats.lastGesture = point.gesture;
    stats.lastPressMs = millis();
  }
  stats.pressedNow = point.pressed;
  lastPoint = point;
  return true;
}

bool touchPresent()
{
  return touchOk;
}

uint8_t touchChipId()
{
  return chipId;
}

bool touchRedetect()
{
  return touchInit(touchAddr, touchIntPin, touchExioReset);
}

TouchStats touchStats()
{
  return stats;
}
