#pragma once
// ---------------------------------------------------------------------------
// GaugeUI - the display front end. Everything on the glass goes through here so the rest of the firmware never
// touches LVGL directly:
//   uiInit()            build the pages from settings.pages, show the boot logo
//   uiTick()            call ~30x/s from any task: refreshes the active page from the signal catalogue
//   swipe left/right    next / previous page (smooth slide, settings.swipeAnimMs)
//   long press          mode menu (docs/ui-concept): one item per screen - big icon, name, a line of text, live status;
//                       swipe left / right for the next item, tap opens it, long press (or 10 s idle) back to the
//                       gauge. Items: Haldex, CAN, K-line, Output / lights, DSG, Bluetooth OBD, Gauge chain (each can be
//                       hidden with settings.menuMask), Settings
//   menu > Haldex       the Haldex gauge; swipe left / right requests the next / previous Haldex mode
//   menu > CAN / K-line / Bluetooth OBD / Output / Daisy Chain
//                       mode screens: connection status (spinner while connecting), then live data the firmware fills
//                       (uiSetModeProvider); swipe left / right = next / previous block, message or page
//   menu > Gauges       back to the gauge pages; the menu's 10 s timeout returns to the screen it was opened from
//   menu > DSG          full screen: big selector + gear (P R N D4 S3 M2), P R N D S M strip, speed (gearbox or vehicle)
//   menu > Drag Timer   0.38.0: 0-100 km/h / 0-60 mph from a countdown, with a scoreboard (DragTimer.cpp)
//   uiAlert()           banner over whatever is showing (triggers), tap to acknowledge
//   themes              can2gauge palettes, Custom, Match car and 65 manufacturer themes (include/can2gauge_themes.h);
//                       Settings > Theme on the glass opens a roller picker
// Every call that creates or changes LVGL objects takes the LVGL lock itself, so it is safe from other tasks.
// ---------------------------------------------------------------------------
#include <lvgl.h>
#include <can2gauge_defs.h>

enum uiInfoPage_t : uint8_t
{
  UI_INFO_HALDEX = 0,
  UI_INFO_CAN,
  UI_INFO_KLINE,
  UI_INFO_ADC, // output / lights
  UI_INFO_BLE,
  UI_INFO_CHAIN,
  UI_INFO_COUNT
};

// text provider for the info pages (menu > Haldex / CAN / K-line / Output). Fill buf with '\n'-separated lines.
typedef void (*uiInfoProvider_t)(char *buf, size_t len);
// callbacks from the on-screen settings page
struct UiCallbacks
{
  void (*onBrightness)(uint8_t percent);
  void (*onCanTermination)(bool enabled);
  void (*onUnits)(bool imperial);
  void (*onReboot)();
  void (*onSettingsChanged)(); // anything else edited on-screen -> persist
  void (*onPageChanged)(uint8_t page); // a gauge page was shown (gauge chain "follow page")
  void (*onModeSwipe)(uint8_t item, int8_t direction); // swipe on a mode screen (uiMenuItem_t): +1 next, -1 previous
  bool (*onHaldexMode)(uint8_t mode);           // swipe on the Haldex screen: request this mode (false = not sent)
};

// mode screens (menu > CAN / K-line / Bluetooth OBD): the firmware fills a UiModeView ~10x/s while one shows
#define UI_MODE_ROWS 5
enum uiModeState_t : int8_t
{
  UI_MS_NONE = -1, // no dot
  UI_MS_BAD = 0,   // grey dot: not connected / nothing to show
  UI_MS_OK = 1,    // green dot
  UI_MS_BUSY = 2,  // spinner: connecting, waiting for an answer
  UI_MS_ERR = 3,   // red dot: off / fault
};
struct UiModeView
{
  char title[28];  // "K-LINE  01 ENGINE"
  char status[64]; // what is happening: "Waking module 0x01 (5-baud init)"
  int8_t state;    // uiModeState_t, for the dot / spinner in front of the status
  char header[24]; // the big line: "Block 3", "0x280", "Page 1 / 3"
  int8_t headerTone; // 0 = text colour, 1 = green (on), 2 = red (off)
  char sub[48];    // under it: module ID, message name
  uint8_t rows;    // name / value rows (0..UI_MODE_ROWS)
  char rowName[UI_MODE_ROWS][24];
  char rowValue[UI_MODE_ROWS][20];
  uint8_t bytes;   // > 0: the eight-byte grid (raw CAN) instead of the rows, this many bytes
  uint8_t data[8];
  char hint[40];   // small footer, e.g. "3 of 14" ("" = none)
  uint8_t nodes;   // > 0: a row of node circles in place of the header (Daisy Chain), up to UI_MODE_NODES
  char nodeName[6][6];
  uint8_t nodeFlags[6]; // UI_NODE_*
};
#define UI_MODE_NODES 6
#define UI_NODE_SELF 1   // this gauge (filled in the accent)
#define UI_NODE_MASTER 2 // the chain master (second accent ring)
#define UI_NODE_HEARD 4  // heard recently (grey when not)

// what the glass shows (Live Data in the web UI follows it)
enum uiScreen_t : uint8_t
{
  UI_SCR_BOOT = 0,
  UI_SCR_PAGE,   // a gauge page (item = index in settings.pages)
  UI_SCR_MENU,
  UI_SCR_MODE,   // a mode screen (item = uiMenuItem_t)
  UI_SCR_HALDEX,
  UI_SCR_DSG,
  UI_SCR_SETTINGS,
  UI_SCR_OTHER,
  UI_SCR_SAVER, // the screensaver
  UI_SCR_DARK,  // ignition off: the shutdown sprite, the dark standby screen or the wake sprite
  UI_SCR_GAME,  // Rev Runner (double-tap on the screensaver)
  UI_SCR_DRAG,  // 0.38.0: the drag timer (menu > Drag Timer)
};
uint8_t uiScreenKind(uint8_t *item); // uiScreen_t; item as above
typedef void (*uiModeProvider_t)(uint8_t item, UiModeView &view);
void uiSetModeProvider(uiModeProvider_t provider);

// the mode menu's live status line: fill buf for the item (uiMenuItem_t, or 100 = Settings) and return 1 = OK (text +
// green dot), 0 = not OK (grey dot), -1 = no dot (plain text)
typedef int8_t (*uiMenuStatus_t)(uint8_t item, char *buf, size_t len);
void uiSetMenuStatus(uiMenuStatus_t provider);

bool uiInit(const UiCallbacks &uiCallbacks);
void uiShowBootLogo();       // uses settings.bootLogo / bootText, returns immediately (timer hides it)
bool uiBootLogoClock(uint32_t &elapsedMs); // 0.30.11: how far into the boot logo, ms (false = no logo up)
void uiBootLogoCatchUp(uint32_t otherStartMs); // 0.30.11: jump the logo forward to another gauge's, whose logo went up
                                               // at otherStartMs (millis() here). 0.30.13: was JumpTo(targetMs)
void uiBootLogoRestart(uint32_t startAtMs); // 0.30.14: the boot logo starts again from the beginning at startAtMs
                                             // (millis()), its first frame drawn now (handshake: all gauges go)
void uiRebuildPages();       // call after settings.pages / ain names change (web UI)
void uiPageRangesChanged();  // 0.36.4: an adc2can sensor's dial range changed: the pages are rebuilt at the next uiTick
void uiRebuildMenu();        // call after settings.menuMask changes (web UI)
void uiOpenDsg();            // the DSG screen (menu > DSG)
void uiTick();               // refresh values on the visible page
void uiNextPage();
void uiPrevPage();
void uiGotoPage(uint8_t index);
uint8_t uiActivePage();
void uiOpenMenu();
void uiCloseMenu();
bool uiMenuOpen();
bool uiAlert(const char *text, bool latch); // 0.32.0: false = the screen was busy, not shown (try again)
void uiClearAlert();
// full-screen progress ring over everything (long jobs such as the ECU flash read). Swallows touches while shown.
// pct 0..100 fills the ring, pct < 0 hides it. autoHideMs > 0 hides it that long after this call.
void uiProgress(const char *title, const char *detail, int pct, bool failed = false, uint32_t autoHideMs = 0);
// 0.29.0: Show Numbers - the gauge's number in the row, big, over everything (a tap hides it). number NULL = hide it
// now; showMs 0 = stays until hidden
void uiShowNumber(const char *number, const char *detail, uint32_t showMs);
// 0.30.0: turn-by-turn navigation card (src/can2gauge_Nav.cpp fills it from the can2gauge Link phone app), laid out
// like Android Auto's: the turn arrow and the distance on a band in the accent colour, the road under it, the arrival
// time at the bottom. Over everything; a tap hides it until the next turn.
#define UI_NAV_ARROW_MAX 64 // the turn arrow is at most 64 x 64 dots (the app sends 48 x 48)
struct UiNavView
{
  bool rerouting;       // Maps is working out a new route: "Rerouting" in place of the arrow
  bool arrived;         // at the destination: "Arrived"
  char distText[16];    // distance to the turn as Maps shows it, in the phone's units ("250 m", "0.2 mi")
  char instruction[96]; // "Turn right onto High Street"
  char etaText[40];     // "12 min - 5.2 km - 22:41"
  bool hasProgress;       // 0.30.8: routeProgress is known (the ring round the edge shows it)
  uint16_t routeProgress; // 0.30.8: journey done, thousandths: the ring from the start dot (0) to the flag (1000)
  char arrowFile[24];   // 0.30.1: a stored arrow ("/nav/right.bin", made by tools/nav_arrows.py); "" = use the dots below
  uint8_t arrowWidth;   // turn arrow size in dots, 0 = no arrow
  uint8_t arrowHeight;
  uint8_t arrowBits[(UI_NAV_ARROW_MAX / 8) * UI_NAV_ARROW_MAX]; // 1 bit a dot, a row at a time, left dot = top bit
};
bool uiNavShow(const UiNavView &view, bool newTurn); // show / update the card. newTurn = the next turn changed (a
                                                     // card hidden with a tap only comes back for a new turn).
                                                     // 0.30.5: false = the screen was busy, nothing done (try again)
bool uiNavHide();                                    // take the card away (navigation ended, or not close to a turn).
                                                     // 0.30.5: false = the screen was busy, still up (try again)
void uiSetInfoProvider(uiInfoPage_t page, uiInfoProvider_t provider);
void uiSetTheme(uint8_t theme); // palette only - screens already built keep their colours (see uiApplyTheme)
void uiApplyTheme();            // settings.theme / Custom colours / Match car changed: new palette + rebuild every screen
void uiSetNight(bool night); // dims the accent slightly and switches to the night palette
// sprites (include/c2gs.h, docs/SPRITES.md). A page with its Sprite switch on plays its pack, reacting to the page's reading. The screensaver
// (settings.saverEnabled) comes up by itself over a gauge page when the car is still and the glass untouched for
// settings.saverDelayS, and leaves on a touch, movement, an alert or a page change.
void uiSaverShow();     // show the screensaver now (web UI test); it leaves as usual
bool uiSaverActive();
// ignition on permanent power (src/can2gauge_Power.cpp): uiShutdown() plays settings.shutdownSprite's shutdown state
// (else a fade to black) and returns how long that takes (ms) - the caller turns the backlight off after it.
// uiStandby(true) leaves a black screen that is never refreshed; uiStandby(false) plays the pack's wake state (if any)
// and returns to the gauge page.
// The row wave (0.12.0): with gaugesInRow 2 or more (and a pack with the row clips), the gauges greet each other in turns
// along the row before the shutdown clip; placeFromLeft is this gauge's place in it (0 = leftmost). 0, 1 = on its own.
uint32_t uiShutdown(uint8_t placeFromLeft, uint8_t gaugesInRow);
void uiStandby(bool on);

// what the glass costs (0.11.0 frame-rate work, web UI > Diag): uiTick and the sprite decode over the last second,
// and the benchmark - the whole screen redrawn every refresh for UI_BENCH_MS, averaged over its last seconds
#define UI_BENCH_MS 6000
struct UiPerf
{
  uint32_t tickAvgUs, tickMaxUs;     // uiTick (values -> widgets, sprite decode included)
  uint32_t decodes;                  // sprite frames decoded
  uint32_t decodeAvgUs, decodeMaxUs; // one sprite frame: read from flash + decode
  uint32_t unzipAvgUs;               // 0.24.7: ... of which unzipping it (packs since 0.24.0 are zipped)
  // the last benchmark
  bool benchRunning;
  uint32_t benchMs;                  // millis() it finished (0 = never run)
  char benchScreen[16];              // what was on the glass ("page 2", "saver" ...)
  uint32_t benchFps, benchFrameAvgUs, benchFrameMaxUs, benchFlushUs;
  uint8_t benchBusyPct;
  uint32_t benchDecodeAvgUs;
};
const UiPerf &uiPerf();
void uiBenchmark(); // start the benchmark on whatever the glass shows
const char *uiStyleName(uint8_t style);
