#pragma once
#include <can2gauge_defs.h>

// V3.4: one half-bridge output (TCA9554 P4/P5 -> DRV8876), dash-light input (P6), output fault (P7), CAN termination FET
void setupIO();                       // I2C + TCA9554 + output + CAN termination + CAN bus (the poll task: setupTasks)
void ioTick();                        // one poll: dash-light input, night mode, output fault
bool dashLightsOn();                  // debounced, after the invert setting
bool nightActive();                   // dash lights on AND night dimming enabled
uint8_t backlightTarget();            // settings.brightness, or settings.nightBrightness while night is active
void outputRequest(uint8_t channel, bool on, bool fromTrigger); // honours settings.outputManual (0 off / 1 on / 2 auto)
void outputsApply();                  // re-evaluate mode + manual override (after a settings change)
void canTerminationApply(bool enabled);
void canTerminationFromSettings();    // 0.30.15: Off / On / Auto (settings.canTermination)
bool canTerminationIsOn();            // 0.30.15: the 120R is in circuit right now
void ioInfoProvider(char *buf, size_t len);
