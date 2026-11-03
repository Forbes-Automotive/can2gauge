#pragma once
#include <can2gauge_defs.h>

// The gauge's I/O (board V3.4): one half-bridge output (TCA9554 P4/P5 -> DRV8876), the dash-light input (P6), the
// output's fault pin (P7) and the CAN termination FET

// setting up and the regular poll
void setupIO(); // I2C + TCA9554 + output + CAN termination + CAN bus (the poll task: setupTasks)
void ioTick();  // one poll: dash-light input, night mode, output fault

// dash lights, night dimming and the backlight
bool dashLightsOn();       // debounced, after the invert setting
bool nightActive();        // dash lights on AND night dimming enabled
uint8_t backlightTarget(); // settings.brightness, or settings.nightBrightness while night is active

// the output
void outputRequest(uint8_t channel, bool on, bool fromTrigger); // honours settings.outputManual (0 off / 1 on / 2 auto)
void outputsApply();                                            // re-evaluate mode + manual override (after a settings change)

// CAN termination (the 120R)
void canTerminationApply(bool enabled); // true = 120R in circuit
void canTerminationFromSettings();      // Off / On / Auto (settings.canTermination)
bool canTerminationIsOn();              // the 120R is in circuit right now

// the menu's output / dash lights info page
void ioInfoProvider(char *buf, size_t len);
