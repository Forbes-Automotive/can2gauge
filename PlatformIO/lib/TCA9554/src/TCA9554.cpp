#include "TCA9554.h"

static uint8_t tcaAddr = 0x20;
static uint8_t tcaOutputs = 0xFF;
static bool tcaOk = false;
// 0.30.15: two tasks can now change the outputs at once (the boot sync's Auto CAN termination runs while setupDisplay
// pulses the panel's reset on another pin). tcaOutputs is read, changed and written back, so one task could undo the
// other's change: this lock makes them take turns. A mutex = a "talking stick" only one task can hold at a time.
static SemaphoreHandle_t tcaMutex = NULL;

static bool regWrite(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(tcaAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static int regRead(uint8_t reg)
{
  Wire.beginTransmission(tcaAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
  {
    return -1;
  }
  if (Wire.requestFrom((int)tcaAddr, 1) != 1)
  {
    return -1;
  }
  return Wire.read();
}

bool tca9554Init(uint8_t addr, uint8_t outputDefaults, uint8_t configMask)
{
  if (tcaMutex == NULL)
  {
    tcaMutex = xSemaphoreCreateMutex();
  }
  tcaAddr = addr;
  tcaOutputs = outputDefaults;
  tcaOk = regWrite(TCA9554_REG_OUTPUT, tcaOutputs); // set levels first so nothing glitches when the pins go to output
  tcaOk &= regWrite(TCA9554_REG_POLARITY, 0x00);
  tcaOk &= regWrite(TCA9554_REG_CONFIG, configMask);
  return tcaOk;
}

void tca9554Write(uint8_t pin, bool level)
{
  if (pin < 1 || pin > 8)
  {
    return;
  }
  if (tcaMutex)
  {
    xSemaphoreTake(tcaMutex, portMAX_DELAY); // wait our turn (portMAX_DELAY = as long as it takes; a write is ~0.1 ms)
  }
  if (level)
  {
    tcaOutputs |= (1 << (pin - 1));
  }
  else
  {
    tcaOutputs &= ~(1 << (pin - 1));
  }
  regWrite(TCA9554_REG_OUTPUT, tcaOutputs);
  if (tcaMutex)
  {
    xSemaphoreGive(tcaMutex);
  }
}

bool tca9554Read(uint8_t pin)
{
  int inputs = regRead(TCA9554_REG_INPUT);
  if (inputs < 0 || pin < 1 || pin > 8)
  {
    return false;
  }
  return (inputs >> (pin - 1)) & 1;
}

uint8_t tca9554Outputs()
{
  return tcaOutputs;
}

bool tca9554Present()
{
  return tcaOk;
}
