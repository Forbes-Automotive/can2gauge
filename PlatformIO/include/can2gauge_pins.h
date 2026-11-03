#pragma once

// can2gauge V3.4 - ESP32-S3-WROOM-1-N16R8 pin map (pcb/ProPrj_can2gauge_V3-4, the O48 board for the 52 mm DIN body).
// The screen's pins are the same as Garage Tinkering's Ultimate Gauge Board / Waveshare ESP32-S3-Touch-LCD-2.1.
// IO35/36/37 belong to the octal PSRAM: never use them.

// ===== 2.1" 480x480 ST7701S screen: 16-bit RGB565, set up over 3-wire SPI =====
// The YouriTech ET021CR01-Z on the can2gauge board; -D CAN2GAUGE_WAVESHARE ([env:waveshare]) picks the Waveshare 2.1
// screen's registers and the UGB data lanes instead (setupDisplay, lib/Display_ST7701).
#define LCD_WIDTH 480
#define LCD_HEIGHT 480
#define LCD_PIN_HSYNC 38
#define LCD_PIN_VSYNC 39
#define LCD_PIN_DE 40
#define LCD_PIN_PCLK 41
// DATA0..15 = B1..B5, G0..G5, R1..R5 on the 40-pin FPC (B0 / R0 tied to GND on the PCB)
#define LCD_PIN_D0 42  // B1
#define LCD_PIN_D1 45  // B2
#define LCD_PIN_D2 48  // B3
#define LCD_PIN_D3 47  // B4
#define LCD_PIN_D4 21  // B5
#define LCD_PIN_D5 14  // G0
#define LCD_PIN_D6 13  // G1
#define LCD_PIN_D7 12  // G2
#define LCD_PIN_D8 11  // G3
#define LCD_PIN_D9 10  // G4
#define LCD_PIN_D10 9  // G5
#define LCD_PIN_D11 46 // R1
#define LCD_PIN_D12 3  // R2
#ifdef CAN2GAUGE_WAVESHARE // [env:waveshare]: the Waveshare ESP32-S3-Touch-LCD-2.1 dev board keeps the UGB lanes
#define LCD_PIN_D13 17 // R3
#define LCD_PIN_D14 18 // R4
#define LCD_PIN_D15 8  // R5
#else
#define LCD_PIN_D13 8  // R3 (V3.1: R3 and R5 swapped from UGB - module pin 12, a lane with no crossing)
#define LCD_PIN_D14 18 // R4
#define LCD_PIN_D15 17 // R5 (V3.1: module pin 10)
#endif
#define LCD_PIN_SPI_SCK 2 // ST7701 set-up bus (3-wire SPI) clock
#define LCD_PIN_SPI_SDA 1 // ... data
#define LCD_PIN_BL_PWM 6  // AL8860 CTRL: PWM at 500 Hz or less (NOT 20 kHz) for the full dimming range

// ===== I2C bus: TCA9554 at 0x20, CST8xx touch at 0x15 =====
#define I2C_PIN_SDA 15
#define I2C_PIN_SCL 7
#define I2C_FREQ_HZ 400000

// ===== TCA9554 at 0x20 (P0..P7 = EXIO1..EXIO8 on the schematic), the only expander =====
#define TCA9554_ADDR 0x20
#define EXIO_LCD_RST 1       // P0
#define EXIO_TP_RST 2        // P1
#define EXIO_LCD_CS 3        // P2
#define EXIO_CAN_TERM_EN 4   // P3 - drives Q_TERM's gate: LOW = 120R termination in circuit (the last gauge in the chain)
#define EXIO_OUT_IN1 5       // P4 -> DRV8876 IN1  \  PWM mode: 00 = coast (OFF, hi-Z), 10 = OUT high (high-side on),
#define EXIO_OUT_IN2 6       // P5 -> DRV8876 IN2  /  11 = OUT low (low-side on). OUT = J_OUT pin 1.
#define EXIO_DIM 7           // P6 <- dash-light input (J_OUT pin 3, peak-detected, clamped): HIGH = lights on (INPUT)
#define EXIO_OUT_FLT 8       // P7 <- DRV8876 nFAULT (10k pull-up): LOW = over-current / over-temp / UVLO (INPUT)
#define EXIO_INPUT_MASK 0xC0 // P6 / P7 are inputs
// The TCA9554 powers up with every port an input (hi-Z) and the DRV8876 pulls IN1 / IN2 down: the output is OFF
// (coast) until the firmware drives it. The DRV8876 protects itself (OCP 3.5 A min, thermal, auto-retry).

// ===== capacitive touch (CST8xx on the screen's flex) =====
#define TP_PIN_INT 16
#define CST8XX_ADDR 0x15

// ===== CAN (classic, TCAN1044V: VCC 5 V, VIO 3V3, STB tied low; 500 kbps by default) =====
// Board (v3.4-kfix 2026-09-29): module pin 4 = IO4 -> TCAN1044V pin 1 TXD, module pin 5 = IO5 <- pin 4 RXD. (The
// pre-V3.1 map had them the other way round, which leaves CAN silent on this board.)
#define CAN_PIN_TX 4
#define CAN_PIN_RX 5

// ===== K-line (ISO 9141 / KWP1281 / KWP2000) via L9637D (ST pinout: 1 RX, 4 TX) =====
// UART1 through the GPIO matrix, swapped from the UART0 defaults so the pair reaches the chip without crossing. Bonus:
// the ROM boot log (always on U0TXD = IO43 at reset) lands on the L9637D's RX output instead of going out onto K.
#define KLINE_PIN_TX 44 // -> L9637D pin 4 TX (idle high = K released; the L9637D pulls TX up inside)
#define KLINE_PIN_RX 43 // <- L9637D pin 1 RX (open collector, pulled up to 3V3 inside the L9637D)
#define KLINE_UART 1    // Serial1, on the pins above

// ===== output (the channel number triggers / the web UI / the settings use) =====
#define IOX_OUT 0      // the one half-bridge output -> J_OUT pin 1
#define IOX_CHANNELS 1 // how many outputs there are
// J_OUT (JST PA 3P): 1 = OUT (12 V high-side OR low-side, set in the web UI, 3 A)  2 = K-LINE  3 = dash lights (58d).
// Loads take their 12 V / GND from the harness (power and CAN come in on the JST PA-4 pair J_BUS1 / J_BUS2, 3 A per pin).

// ===== other =====
#define PIN_BOOT 0 // the BOOT button
