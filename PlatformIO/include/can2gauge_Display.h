#pragma once
#include <can2gauge_defs.h>

// Brings up the ST7701 panel + capacitive touch + LVGL + the gauge UI, registers the menu info-page providers and
// the on-screen settings callbacks, shows the boot logo and sets the backlight. Call after setupIO() (the panel's
// reset/CS lines are on the TCA9554 expander that setupIO() brings up).
void setupDisplay();
// the mode screen on the glass right now (menu > CAN / K-line / Bluetooth OBD / Output / Daisy Chain), for the web UI's
// Live Data: false when none has been drawn in the last second
struct UiModeView;
bool displayLiveView(UiModeView &view, uint8_t &item);
