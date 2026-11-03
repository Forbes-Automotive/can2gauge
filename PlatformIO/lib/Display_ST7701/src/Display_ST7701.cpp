#include "Display_ST7701.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_idf_version.h"
#include <TCA9554.h>
#include "esp_rom_gpio.h"     // 0.24.3 bench: esp_rom_gpio_connect_out_signal() hands SCK / SDA back from the SPI peripheral
#include "soc/gpio_sig_map.h" // SIG_GPIO_OUT_IDX: "plain GPIO output" for the call above
#include "esp_system.h"       // 0.36.11: esp_register_shutdown_handler() - backlight off before every restart
#include "esp_rom_sys.h"      // 0.36.11: esp_rom_delay_us() - a plain wait that works while the gauge restarts

static spi_device_handle_t spiHandle = NULL;
static bool useBitBang = false; // 0.24.3 bench: register writes clocked out by hand on SCK / SDA, not by the SPI peripheral
static esp_lcd_panel_handle_t panelHandle = NULL;
static DisplayPins config;
static uint8_t backlightPct = 0;
static displayVsyncCb_t vsyncCallback = NULL;
static void *vsyncUser = NULL;
static uint8_t initStep = DISPLAY_STEP_NONE; // how far displayInit got (Diag)
static uint16_t spiErrors = 0;               // register writes the SPI driver refused (Diag)
static volatile uint32_t frameCount = 0;     // vsyncs from the RGB peripheral (volatile: counted in the interrupt)
static bool panelAsleep = false;

struct st7701Cmd
{
  uint8_t cmd;
  uint8_t len;
  uint16_t delayMs;
  uint8_t data[16];
};

// YouriTech ET021CR01-Z: the vendor's init code, line for line, in its order and with its delays (docs/ET021CR01-Z
// Initial Code 9009-20220329ver16c_FW). Gamma, power / VCOM and the GIP (E0..ED, gate timing) are this glass's own -
// do not "tidy", and do not mix with the Waveshare set below.
static const st7701Cmd youritechSequence[] = {
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, 0, {0x08}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}}, // 480 lines
    {0xC1, 2, 0, {0x0C, 0x02}}, // VBP 12, VFP 2 (the RGB timing's vsync pulse + back porch matches the VBP, see panels[])
    {0xC2, 2, 0, {0x30, 0x06}},
    {0xCC, 1, 0, {0x10}},
    {0xCD, 1, 0, {0x08}},
    {0xB0, 16, 0, {0x00, 0x0E, 0x56, 0x0D, 0x10, 0x05, 0x05, 0x07, 0x07, 0x22, 0x04, 0x13, 0x12, 0x6B, 0x73, 0xFF}},
    {0xB1, 16, 0, {0x00, 0x0E, 0x56, 0x0D, 0x10, 0x05, 0x05, 0x07, 0x07, 0x22, 0x04, 0x13, 0x12, 0x6B, 0x73, 0xFF}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x75}},
    {0xB1, 1, 0, {0x41}},
    {0xB2, 1, 0, {0x89}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x49}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x32}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 0, {0x78}},
    {0xD0, 1, 100, {0x88}},
    {0xE0, 3, 0, {0x00, 0x00, 0x02}},
    {0xE1, 11, 0, {0x05, 0xC0, 0x07, 0xC0, 0x04, 0xC0, 0x06, 0xC0, 0x00, 0x44, 0x44}},
    {0xE2, 13, 0, {0x10, 0x10, 0x33, 0x33, 0xEC, 0xC0, 0x00, 0x00, 0xEC, 0xC0, 0x00, 0x00, 0x00}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE4, 2, 0, {0x44, 0x44}},
    {0xE5, 16, 0, {0x09, 0xF1, 0x10, 0xFA, 0x0B, 0xF3, 0x10, 0xFA, 0x05, 0xED, 0x10, 0xFA, 0x07, 0xEF, 0x10, 0xFA}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE7, 2, 0, {0x44, 0x44}},
    {0xE8, 16, 0, {0x08, 0xF0, 0x10, 0xFA, 0x0A, 0xF2, 0x10, 0xFA, 0x04, 0xEC, 0x10, 0xFA, 0x06, 0xEE, 0x10, 0xFA}},
    {0xEB, 7, 0, {0x02, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x00}},
    {0xED, 16, 0, {0xFF, 0x04, 0x56, 0x7F, 0xBA, 0x2F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF2, 0xAB, 0xF7, 0x65, 0x40, 0xFF}},
    {0xEF, 6, 0, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    {0x11, 0, 120, {}},   // sleep out
    {0x29, 0, 0, {}},     // display on (backlight is still off; the RGB stream starts right after)
    {0x35, 1, 0, {0x00}}, // TE on - the TE pin isn't wired, kept as the vendor sends it
    {0x3A, 1, 0, {0x66}}, // 18-bit RGB: the board feeds RGB565 with R0 / B0 tied to GND
    {0x36, 1, 0, {0x00}},
};

// ST7701S register set for the Waveshare 2.1 / Garage Tinkering UGB glass - do not "tidy"
static const st7701Cmd waveshareSequence[] = {
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}},
    {0xC1, 2, 0, {0x0B, 0x02}},
    {0xC2, 2, 0, {0x07, 0x02}},
    {0xCC, 1, 0, {0x10}},
    {0xCD, 1, 0, {0x08}},
    {0xB0, 16, 0, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}},
    {0xB1, 16, 0, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x6D}},
    {0xB1, 1, 0, {0x37}},
    {0xB2, 1, 0, {0x81}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x43}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x20}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 0, {0x78}},
    {0xD0, 1, 0, {0x88}},
    {0xE0, 3, 0, {0x00, 0x00, 0x02}},
    {0xE1, 11, 0, {0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20}},
    {0xE2, 13, 0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x00}},
    {0xE4, 2, 0, {0x22, 0x00}},
    {0xE5, 16, 0, {0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x00}},
    {0xE7, 2, 0, {0x22, 0x00}},
    {0xE8, 16, 0, {0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xEB, 7, 0, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}},
    {0xED, 16, 0, {0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF, 0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}},
    {0xEF, 6, 0, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, 0, {0x08}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    {0x36, 1, 0, {0x00}},
    {0x3A, 1, 0, {0x66}},
    {0x11, 0, 480, {}},
    {0x20, 0, 120, {}},
    {0x29, 0, 0, {}},
};

// per glass: the register set and the RGB timing. The ST7701S C1 VBP counts from the VSYNC edge, so the RGB
// timing's pulse + back porch is set to match it (Waveshare: C1 0x0B = 3 + 8, YouriTech: C1 0x0C = 3 + 9).
// 0.24.2: the YouriTech glass gets its own clock and porches from its datasheet (docs/ET021CR01-Z.pdf, 6.2): pixel clock
// 17-27 MHz (was 16, the Waveshare value - under its minimum) and the "typical" horizontal porches 60 / 60 (were 10 / 50)
// and vertical front porch 20 (was 8). The sheet says to use the typical blanking: the panel's gate timing needs it.
// Whole frame: (480 + 8 + 60 + 60) x (480 + 3 + 9 + 20) = 608 x 512 clocks, at 17 MHz = 54.6 frames a second.
// 0.24.7: 18 -> 17 MHz (the sheet's minimum): the RGB stream reads PSRAM all the time, and the drawing shares it.
struct st7701Panel
{
  const st7701Cmd *sequence;
  uint8_t count;
  uint32_t pclkHz;         // pixel clock
  uint8_t hsyncBackPorch;  // clocks between the HSYNC pulse and the first pixel of a line
  uint8_t hsyncFrontPorch; // clocks between the last pixel of a line and the next HSYNC pulse
  uint8_t vsyncBackPorch;  // lines between the VSYNC pulse and the first line (pulse + this = the C1 VBP)
  uint8_t vsyncFrontPorch; // lines between the last line and the next VSYNC pulse
};
static const st7701Panel panels[] = {
    {youritechSequence, sizeof(youritechSequence) / sizeof(youritechSequence[0]), 17000000, 60, 60, 9, 20}, // DISPLAY_PANEL_YOURITECH
    {waveshareSequence, sizeof(waveshareSequence) / sizeof(waveshareSequence[0]), 16000000, 10, 50, 8, 8},  // DISPLAY_PANEL_WAVESHARE
};

static const st7701Panel &selectedPanel()
{
  return panels[config.panel < sizeof(panels) / sizeof(panels[0]) ? config.panel : 0];
}

// 0.24.3 bench: one 9-bit register write clocked out by hand, slowly (about 100 kHz): the D/C bit first, then the 8 bits,
// highest bit first. The glass reads SDA when SCK goes high (SPI mode 0), so SDA is set while SCK is low.
static void bitBangWrite9(uint8_t dc, uint8_t value)
{
  uint16_t word = ((uint16_t)dc << 8) | value; // 9 bits: D/C on top (bit 8), the value under it
  for (int8_t bit = 8; bit >= 0; bit--)
  {
    digitalWrite(config.sda, (word >> bit) & 1); // this bit on the data line
    delayMicroseconds(5);
    digitalWrite(config.sck, HIGH); // the glass reads it now
    delayMicroseconds(5);
    digitalWrite(config.sck, LOW);
  }
}

// 0.24.3 bench: take SCK / SDA off the SPI peripheral and drive them as plain outputs from now on (until the next boot),
// to rule out the SPI peripheral's settings when the glass doesn't come up
static void startBitBang()
{
  if (useBitBang)
  {
    return;
  }
  pinMode(config.sck, OUTPUT);
  pinMode(config.sda, OUTPUT);
  esp_rom_gpio_connect_out_signal(config.sck, SIG_GPIO_OUT_IDX, false, false); // plain GPIO, not the SPI clock
  esp_rom_gpio_connect_out_signal(config.sda, SIG_GPIO_OUT_IDX, false, false); // plain GPIO, not the SPI data
  digitalWrite(config.sck, LOW); // clock idles low (mode 0)
  digitalWrite(config.sda, LOW);
  useBitBang = true;
}

static void spiWrite9(uint8_t dc, uint8_t value)
{
  if (useBitBang)
  {
    bitBangWrite9(dc, value);
    return;
  }
  spi_transaction_t transaction = {};
  transaction.cmd = dc;      // 1 command bit: 0 = command, 1 = data
  transaction.addr = value;  // 8 data bits
  transaction.length = 0;
  if (spi_device_transmit(spiHandle, &transaction) != ESP_OK)
  {
    spiErrors++; // the ESP32 couldn't send it (the glass never confirms either way)
  }
}

static void loadPanelRegisters()
{
  tca9554Write(config.exioCs, false);
  vTaskDelay(pdMS_TO_TICKS(10));
  const st7701Panel &panel = selectedPanel();
  for (uint8_t commandIndex = 0; commandIndex < panel.count; commandIndex++)
  {
    const st7701Cmd &command = panel.sequence[commandIndex];
    spiWrite9(0, command.cmd);
    for (uint8_t byteIndex = 0; byteIndex < command.len; byteIndex++)
    {
      spiWrite9(1, command.data[byteIndex]);
    }
    if (command.delayMs)
    {
      vTaskDelay(pdMS_TO_TICKS(command.delayMs));
    }
  }
  tca9554Write(config.exioCs, true);
  vTaskDelay(pdMS_TO_TICKS(10));
}

static bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *eventData, void *user)
{
  frameCount++; // one frame sent to the glass
  if (vsyncCallback)
  {
    return vsyncCallback(vsyncUser);
  }
  return false;
}

// 0.36.11: runs on every restart (after an update, Restart in the web UI, a settings restore ...), just before the
// chip resets: the backlight goes off first, so the half-drawn frame the reset leaves on the glass is never seen.
static void backlightOffForRestart()
{
  ledcWrite(config.backlight, 0); // backlight off
  esp_rom_delay_us(30000);        // 30 ms: more than a frame (18 ms), so it's dark before the picture stops
}

bool displayInit(const DisplayPins &pins)
{
  config = pins;
  // backlight off during init (AL8860 CTRL low = LEDs off)
  ledcAttach(config.backlight, DISPLAY_BL_PWM_HZ, DISPLAY_BL_PWM_BITS);
  ledcWrite(config.backlight, 0);

  // hardware reset via TCA9554
  tca9554Write(config.exioReset, false);
  vTaskDelay(pdMS_TO_TICKS(10));
  tca9554Write(config.exioReset, true);
  vTaskDelay(pdMS_TO_TICKS(120)); // YouriTech's code waits 120 ms after the reset before the first register

  // 3-wire SPI for the register load (CS is on the expander so spics_io_num = -1)
  spi_bus_config_t bus = {};
  bus.mosi_io_num = config.sda;
  bus.miso_io_num = -1;
  bus.sclk_io_num = config.sck;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 64;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
  {
    return false;
  }
  spi_device_interface_config_t device = {};
  device.command_bits = 1;
  device.address_bits = 8;
  device.mode = 0;
  device.clock_speed_hz = 10 * 1000 * 1000; // the panel only needs this once; keep it slow and reliable
  device.spics_io_num = -1;
  device.queue_size = 1;
  if (spi_bus_add_device(SPI2_HOST, &device, &spiHandle) != ESP_OK)
  {
    return false;
  }
  initStep = DISPLAY_STEP_SPI;
  loadPanelRegisters();
  initStep = DISPLAY_STEP_REGISTERS;

  // RGB peripheral: 16-bit bus, two frame buffers in PSRAM, bounce buffer against tearing/drift
  esp_lcd_rgb_panel_config_t rgb = {};
  rgb.clk_src = LCD_CLK_SRC_DEFAULT;
  rgb.timings.pclk_hz = selectedPanel().pclkHz;
  rgb.timings.h_res = DISPLAY_WIDTH;
  rgb.timings.v_res = DISPLAY_HEIGHT;
  rgb.timings.hsync_pulse_width = 8;
  rgb.timings.hsync_back_porch = selectedPanel().hsyncBackPorch;
  rgb.timings.hsync_front_porch = selectedPanel().hsyncFrontPorch;
  rgb.timings.vsync_pulse_width = 3;
  rgb.timings.vsync_back_porch = selectedPanel().vsyncBackPorch;
  rgb.timings.vsync_front_porch = selectedPanel().vsyncFrontPorch;
  rgb.timings.flags.pclk_active_neg = false;
  rgb.data_width = 16;
  rgb.bits_per_pixel = 16;
  rgb.num_fbs = 2;
  rgb.bounce_buffer_size_px = 10 * DISPLAY_WIDTH;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  rgb.dma_burst_size = 64; // renamed from psram_trans_align in IDF 5.3
#else
  rgb.psram_trans_align = 64;
#endif
  rgb.hsync_gpio_num = config.hsync;
  rgb.vsync_gpio_num = config.vsync;
  rgb.de_gpio_num = config.de;
  rgb.pclk_gpio_num = config.pclk;
  rgb.disp_gpio_num = -1;
  for (int i = 0; i < 16; i++)
  {
    rgb.data_gpio_nums[i] = config.data[i];
  }
  rgb.flags.fb_in_psram = true;
  if (esp_lcd_new_rgb_panel(&rgb, &panelHandle) != ESP_OK)
  {
    return false; // most likely no room in PSRAM for the two frame buffers
  }
  initStep = DISPLAY_STEP_RGB;
  esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
  // 0.24.25: the "frame buffer done" moment, not the vsync pulse. With bounce buffers the driver starts reading the NEXT
  // frame buffer a little BEFORE vsync (it preloads the first lines). A buffer handed over in that gap only goes on the
  // glass a whole frame later - but the vsync came straight after, so LVGL thought the swap had happened and drew into
  // the buffer still being shown: flicker (the test alert, 2026-10-27), coming and going as LVGL's frame timing drifted
  // against the glass's. This callback runs at the exact moment the driver switches to the buffer it was handed.
  callbacks.on_frame_buf_complete = onVsync;
  esp_lcd_rgb_panel_register_event_callbacks(panelHandle, &callbacks, NULL);
  if (esp_lcd_panel_reset(panelHandle) != ESP_OK)
  {
    return false;
  }
  if (esp_lcd_panel_init(panelHandle) != ESP_OK)
  {
    return false;
  }
  initStep = DISPLAY_STEP_READY;
  esp_register_shutdown_handler(backlightOffForRestart); // 0.36.11: backlight off before every restart
  return true;
}

void displayDrawBitmap(int x1, int y1, int x2, int y2, const void *pixels)
{
  if (!panelHandle)
  {
    return;
  }
  if (x2 > DISPLAY_WIDTH)
  {
    x2 = DISPLAY_WIDTH;
  }
  if (y2 > DISPLAY_HEIGHT)
  {
    y2 = DISPLAY_HEIGHT;
  }
  esp_lcd_panel_draw_bitmap(panelHandle, x1, y1, x2, y2, pixels);
}

void displaySetBacklight(uint8_t percent)
{
  if (percent > 100)
  {
    percent = 100;
  }
  backlightPct = percent;
  // AL8860: CTRL duty is linear in LED current; the eye is not, so square it for a nicer slider feel
  const uint32_t maxDuty = (1u << DISPLAY_BL_PWM_BITS) - 1;
  uint32_t duty = (uint32_t)(((float)percent * percent / 10000.0f) * maxDuty);
  if (percent > 0 && duty < 12)
  {
    duty = 12; // AL8860 needs a minimum on-time to keep regulating
  }
  ledcWrite(config.backlight, duty);
}

// 0.24.23: the backlight at a share of the level displaySetBacklight() set, for the page-change dip (lvglDipTo). The set
// level isn't changed, so displaySetBacklight(level) - or another task setting it meanwhile - puts it back.
void displayBacklightDim(uint8_t shareOfSet)
{
  if (shareOfSet > 100)
  {
    shareOfSet = 100;
  }
  const uint32_t percent = (uint32_t)backlightPct * shareOfSet / 100;
  const uint32_t maxDuty = (1u << DISPLAY_BL_PWM_BITS) - 1;
  uint32_t duty = (uint32_t)(((float)percent * percent / 10000.0f) * maxDuty); // squared, as displaySetBacklight
  if (percent > 0 && duty < 12)
  {
    duty = 12; // AL8860 minimum on-time
  }
  ledcWrite(config.backlight, duty);
}

void displaySleep(bool sleep)
{
  if (!spiHandle)
  {
    return;
  }
  tca9554Write(config.exioCs, false);
  vTaskDelay(pdMS_TO_TICKS(2));
  if (sleep)
  {
    spiWrite9(0, 0x28); // display off
    vTaskDelay(pdMS_TO_TICKS(20));
    spiWrite9(0, 0x10); // sleep in: the panel's own supply current drops to its standby figure
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  else
  {
    spiWrite9(0, 0x11); // sleep out: the ST7701S needs 120 ms before the next command
    vTaskDelay(pdMS_TO_TICKS(120));
    spiWrite9(0, 0x29); // display on (the RGB stream never stopped)
  }
  tca9554Write(config.exioCs, true);
  panelAsleep = sleep;
}

DisplayHealth displayHealth()
{
  DisplayHealth health;
  health.step = initStep;
  health.panel = config.panel;
  health.spiErrors = spiErrors;
  health.frames = frameCount;
  health.asleep = panelAsleep;
  return health;
}

uint8_t displayGetBacklight()
{
  return backlightPct;
}

esp_lcd_panel_handle_t displayPanel()
{
  return panelHandle;
}

// 0.36.9: the update clock (see the header). The RGB driver takes the new clock at the start of the next frame.
void displaySetUpdateClock(bool isUpdating)
{
  if (panelHandle == NULL)
  {
    return; // the glass isn't running
  }
  uint32_t clockHz = selectedPanel().pclkHz; // the glass's normal clock
  if (isUpdating && DISPLAY_UPDATE_PCLK_HZ < clockHz)
  {
    clockHz = DISPLAY_UPDATE_PCLK_HZ; // only ever slower, never faster than normal
  }
  esp_lcd_rgb_panel_set_pclk(panelHandle, clockHz);
}

void displaySetVsyncCallback(displayVsyncCb_t callback, void *user)
{
  vsyncCallback = callback;
  vsyncUser = user;
}

// 0.24.3 bench: one command and its data bytes straight to the glass (e.g. 0x23 = every pixel on, whatever the RGB bus
// sends: if the glass goes white, the register link works)
void displaySendCommand(uint8_t cmd, const uint8_t *data, uint8_t len, bool bitBang)
{
  if (!spiHandle)
  {
    return; // displayInit never got the bus up
  }
  if (bitBang)
  {
    startBitBang();
  }
  tca9554Write(config.exioCs, false); // CS low: the glass listens
  vTaskDelay(pdMS_TO_TICKS(2));
  spiWrite9(0, cmd);
  for (uint8_t byteIndex = 0; byteIndex < len; byteIndex++)
  {
    spiWrite9(1, data[byteIndex]);
  }
  tca9554Write(config.exioCs, true); // CS high: done
}

// 0.24.3 bench: reset the glass and send its whole register list again (the RGB stream keeps running). bitBang: clocked
// out by hand instead of by the SPI peripheral.
void displayReloadRegisters(bool bitBang)
{
  if (!spiHandle)
  {
    return;
  }
  if (bitBang)
  {
    startBitBang();
  }
  tca9554Write(config.exioReset, false); // hardware reset of the glass
  vTaskDelay(pdMS_TO_TICKS(10));
  tca9554Write(config.exioReset, true);
  vTaskDelay(pdMS_TO_TICKS(120));
  loadPanelRegisters();
}

bool displayBitBang()
{
  return useBitBang;
}
