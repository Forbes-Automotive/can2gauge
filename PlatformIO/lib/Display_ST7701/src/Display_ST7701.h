#pragma once
// 2.1" 480x480 ST7701S IPS panel on the ESP32-S3 RGB peripheral.
// - panel registers are loaded once over a 3-wire SPI (9-bit: D/C bit + 8 data) using LCD_CS on the TCA9554
// - pixels then stream over the 16-bit RGB bus from two PSRAM frame buffers (esp_lcd_rgb)
// - backlight is the AL8860 buck CC driver: PWM on CTRL must stay <= 500 Hz for the full dimming range
// Two register sets, picked by DisplayPins::panel:
// - DISPLAY_PANEL_YOURITECH: YouriTech ET021CR01-Z, the vendor's own sequence (docs/ET021CR01-Z Initial Code
//   9009-20220329ver16c_FW) - the production panel, every build is made on it
// - DISPLAY_PANEL_WAVESHARE: the Waveshare ESP32-S3-Touch-LCD-2.1 / UGB glass, sequence from Garage Tinkering's
//   Ultimate_Gauge_Board_PIO (MIT) - for the dev board
#include <Arduino.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 480
#define DISPLAY_BL_PWM_HZ 250   // AL8860 CTRL - NOT 20 kHz
#define DISPLAY_BL_PWM_BITS 10

#define DISPLAY_PANEL_YOURITECH 0
#define DISPLAY_PANEL_WAVESHARE 1

struct DisplayPins
{
  int8_t sck, sda;            // 3-wire SPI init bus
  int8_t hsync, vsync, de, pclk;
  int8_t data[16];
  int8_t backlight;
  uint8_t exioReset, exioCs;  // TCA9554 pin numbers (1..8)
  uint8_t panel;              // DISPLAY_PANEL_x: which register set, pixel clock and porches
};

bool displayInit(const DisplayPins &pins);                                 // full bring-up: reset, registers, RGB panel, backlight off
void displayDrawBitmap(int x1, int y1, int x2, int y2, const void *pixels); // esp_lcd draw (x2/y2 exclusive)
void displaySetBacklight(uint8_t percent);                                  // 0..100
void displayBacklightDim(uint8_t shareOfSet); // 0.24.23: 0..100 % of the set level (page-change dip; the set level is kept)
uint8_t displayGetBacklight();
void displaySleep(bool sleep); // panel display-off + sleep-in (standby / before deep sleep), or sleep-out + display-on
esp_lcd_panel_handle_t displayPanel();
// 0.36.9: a slower pixel clock while an update is written to flash. Flash and PSRAM share the same wires, so a long run of
// flash writes starves the screen feed (lines / specks on the gauge being updated); a slower clock reads less from PSRAM.
// The YouriTech glass's sheet says 17 MHz minimum, so this is below spec: it's only for the update screen.
#define DISPLAY_UPDATE_PCLK_HZ 12000000 // pixel clock during an update (normal: 17 MHz YouriTech, 16 MHz Waveshare); 0.36.13: was 14 MHz, slight lines left on the gauge being updated ** CAN CHANGE THIS **
void displaySetUpdateClock(bool isUpdating); // true = DISPLAY_UPDATE_PCLK_HZ, false = the glass's normal clock
// register a callback that fires from the RGB vsync ISR (used by the LVGL driver to sync flushes)
typedef bool (*displayVsyncCb_t)(void *user);
void displaySetVsyncCallback(displayVsyncCb_t callback, void *user);

// 0.24.3 bench (USB console / POST /api/action): talk to the glass by hand while bringing a new glass up.
// bitBang = clock the bits out by hand on SCK / SDA instead of with the SPI peripheral (stays that way until a reboot).
void displaySendCommand(uint8_t cmd, const uint8_t *data, uint8_t len, bool bitBang); // one command + its data bytes
void displayReloadRegisters(bool bitBang); // reset the glass and send its whole register list again
bool displayBitBang();                     // true once the register writes are clocked out by hand

// health for the web UI's Diag tab. The glass can't answer back (3-wire SPI is write only, the RGB bus one way), so
// this is what the ESP32 side can see: how far the bring-up got, whether every register write was taken, and whether
// frames are going out to the glass.
#define DISPLAY_STEP_NONE 0      // displayInit not run
#define DISPLAY_STEP_SPI 1       // the 3-wire SPI bus for the registers is up
#define DISPLAY_STEP_REGISTERS 2 // the panel's register list has been sent
#define DISPLAY_STEP_RGB 3       // the RGB peripheral and its PSRAM frame buffers are made
#define DISPLAY_STEP_READY 4     // the RGB stream is running
struct DisplayHealth
{
  uint8_t step;        // DISPLAY_STEP_x: how far displayInit got (READY = all of it)
  uint8_t panel;       // DISPLAY_PANEL_x being driven
  uint16_t spiErrors;  // register writes the ESP32's SPI driver refused (0 expected)
  uint32_t frames;     // frames the RGB peripheral has sent to the glass since boot (about 57 a second when running)
  bool asleep;         // the panel is in sleep (standby)
};
DisplayHealth displayHealth();
