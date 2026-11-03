#include "IOX.h"

#include <TCA9554.h>
#include <can2gauge_pins.h>

// ---------------- single half-bridge output on the TCA9554 (P4/P5 -> DRV8876 IN1/IN2) ----------------
static uint16_t ioxBits = 0;
static bool ioxOk = false;
static bool lowSideMode = false;

static void driveOutput(bool on)
{
  if (!ioxOk)
  {
    return;
  }
  if (!on)
  {
    tca9554Write(EXIO_OUT_IN1, false); // 00 = coast
    tca9554Write(EXIO_OUT_IN2, false);
    return;
  }
  // IN2 first when switching on in high-side mode so the bridge never passes through 11 (brake = OUT low)
  tca9554Write(EXIO_OUT_IN2, lowSideMode);
  tca9554Write(EXIO_OUT_IN1, true);
}

bool ioxInit(uint8_t tcaAddr)
{
  (void)tcaAddr; // single expander, brought up in setup() (tca9554Init) before setupIO()
  ioxBits = 0;
  ioxOk = tca9554Present();
  driveOutput(false);
  return ioxOk;
}

void ioxSetMode(bool lowSide)
{
  if (lowSide == lowSideMode)
  {
    return;
  }
  driveOutput(false); // never flip polarity with the output on
  lowSideMode = lowSide;
  if (ioxBits & 1)
  {
    driveOutput(true);
  }
}

bool ioxLowSide()
{
  return lowSideMode;
}

void ioxSet(uint8_t channel, bool on)
{
  if (channel >= IOX_CHANNELS)
  {
    return;
  }
  if (on)
  {
    ioxBits |= (1u << channel);
  }
  else
  {
    ioxBits &= ~(1u << channel);
  }
  driveOutput(on);
}

bool ioxGet(uint8_t channel)
{
  return channel < IOX_CHANNELS && (ioxBits >> channel) & 1;
}

uint16_t ioxState()
{
  return ioxBits;
}

void ioxAllOff()
{
  ioxSet(IOX_OUT, false);
}

bool ioxPresent()
{
  return ioxOk;
}

bool ioxFault()
{
  return ioxOk && !tca9554Read(EXIO_OUT_FLT);
}

bool ioxDashLightsRaw()
{
  return ioxOk && tca9554Read(EXIO_DIM);
}

const char *ioxChannelName(uint8_t channel)
{
  return channel == IOX_OUT ? "OUT" : "?";
}
