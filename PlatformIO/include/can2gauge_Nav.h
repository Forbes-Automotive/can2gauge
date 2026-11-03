#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// Nav link (0.30.0) - Google Maps turn-by-turn on the glass, Android Auto style.
//
// Google Maps --(its navigation notification)--> can2gauge Link phone app --(Bluetooth LE)--> this file --> nav card
//
// The phone app (android/) reads the notification Maps already shows while navigating (next turn arrow, distance, road,
// arrival time) and writes it to the gauge over Bluetooth LE. Maps isn't changed and Android Auto isn't needed. The
// gauge advertises a service of its own; the app finds it once, remembers it and reconnects by itself each time the
// ignition comes on. Packet layout: docs/nav-link/NAV_LINK.md (version 1), mirrored in the app's NavProtocol.kt.
//
// The card (GaugeUI uiNavShow) comes up over whatever the glass shows: near a turn (settings.navPopupM) or for the
// whole route, as set on the web UI's Navigation tab or the screen's menu > Navigation. A tap or a long press sends it
// away and the gauges come back: in Pop Up until the next turn, in Route for the rest of the route.
//
// Bluetooth is shared with the OBD adapter link (src/can2gauge_BLE.cpp): one NimBLE stack, the gauge a central to the
// adapter and a peripheral to the phone at the same time.
// ---------------------------------------------------------------------------

// Bluetooth ids (must match NavProtocol.kt in the phone app)
#define NAV_SERVICE_UUID "c26a0001-6f72-4265-7341-75746f6d6f74" // the nav link's service (the app scans for it)
#define NAV_STATE_UUID "c26a0002-6f72-4265-7341-75746f6d6f74"   // STATE packet: distance, road, arrival time, flags
#define NAV_ARROW_UUID "c26a0003-6f72-4265-7341-75746f6d6f74"   // ARROW packet: the turn arrow, 1 bit a dot

#define NAV_PROTO_VERSION 1           // first byte of every packet
#define NAV_STATE_MAX_BYTES 256       // biggest STATE packet taken
#define NAV_ARROW_MAX_SIDE 64         // biggest arrow taken, in dots each way (the app sends 48 x 48)
#define NAV_TIMEOUT_MS 30000          // no STATE packet for this long = not navigating (the app repeats it every 10 s)
#define NAV_DIST_UNKNOWN 0xFFFFFFFFUL // distToTurnM when the app couldn't read the distance
#define NAV_PROGRESS_UNKNOWN 0xFFFF   // 0.30.8: routeProgress when the app couldn't work it out
#define NAV_TEST_MS 15000             // Show Test Turn: the test card stays up this long

// 0.30.1: the turn arrows kept on the gauge (data/nav/<name>.bin, drawn by tools/nav_arrows.py), like the boot logos.
// The app works out which one from Maps' instruction ("take the 2nd exit") and sends its number in the STATE packet
// (byte 2); 0 = none of these, so the app sends Maps' own arrow as dots instead. Same numbers as NavProtocol.kt.
#define NAV_ARROW_NONE 0         // no stored arrow: use the dots from the ARROW packet
#define NAV_ARROW_STRAIGHT 1     // straight on / continue / follow
#define NAV_ARROW_LEFT 2         // turn left
#define NAV_ARROW_RIGHT 3        // turn right
#define NAV_ARROW_SLIGHT_LEFT 4  // slight left / keep left / bear left
#define NAV_ARROW_SLIGHT_RIGHT 5 // slight right / keep right / bear right
#define NAV_ARROW_UTURN 6        // U-turn
#define NAV_ARROW_ROUNDABOUT1 7  // roundabout, 1st exit (8 = 2nd ... 11 = 5th; clockwise, UK)
#define NAV_ARROW_ARRIVE 12      // the destination (chequered flag)
#define NAV_ARROW_COUNT 12       // the last number in use

// STATE packet flags (byte 1)
#define NAV_FLAG_NAVIGATING 0x01 // bit 0: Maps is navigating
#define NAV_FLAG_REROUTING 0x02  // bit 1: Maps is working out a new route
#define NAV_FLAG_ARRIVED 0x04    // bit 2: at the destination

// settings.navMode: when the card comes up
#define NAV_MODE_OFF 0        // nav link off (Bluetooth isn't started for it)
#define NAV_MODE_NEAR_TURN 1  // pops up as a turn gets close (settings.navPopupM), away again after it
#define NAV_MODE_WHOLE_ROUTE 2 // up all the time Maps is navigating
#define NAV_POPUP_DEFAULT_M 300 // settings.navPopupM to start with
#define NAV_POPUP_STEP_M 10     // settings.navPopupM goes in 10 m steps (10..2550 m)

// what the web UI's Bluetooth tab and the status show
struct NavStatus
{
  bool started;         // the gauge is advertising the nav service (setting on, Bluetooth running)
  bool phoneConnected;  // the phone app is connected
  bool navigating;      // Maps is navigating (and the app was heard from in the last NAV_TIMEOUT_MS)
  bool rerouting;
  bool arrived;
  bool testShowing;     // Show Test Turn's card is up
  bool cardShowing;     // the card is on the glass now
  uint32_t distToTurnM; // metres to the next turn, NAV_DIST_UNKNOWN = unknown
  uint16_t routeProgress; // 0.30.8: journey done, thousandths (0 = set off, 1000 = there), NAV_PROGRESS_UNKNOWN
  uint8_t arrowId;      // 0.30.1: the stored arrow the app picked (NAV_ARROW_*), 0 = Maps' own arrow as dots
  char distText[16];    // as Maps shows it ("250 m", "0.2 mi")
  char instruction[96]; // "Turn right onto High Street"
  char etaText[40];     // "12 min - 5.2 km - 22:41"
  uint32_t packets;     // STATE packets taken since boot
  uint32_t ageMs;       // ms since the last STATE packet (0 = none yet)
  bool advertising;     // 0.30.5: a phone can see the gauge now (checked every 5 s)
  uint32_t advertRestarts; // 0.30.5: times advertising had stopped and was started again since boot
};

void setupNav();                 // at boot, before WiFi (Bluetooth needs the internal RAM WiFi takes later)
void navTick();                  // from the logic task, ~10x a second: timeout, and the card up / down
void navRestart();               // settings.navMode changed (web UI): start or stop the nav service
bool navWanted();                // settings ask for the nav link (Bluetooth has to stay up for it)
void navStatus(NavStatus &out);  // a copy of the latest, for the web UI
void navShowTest();              // web UI Show Test Turn: a right turn on the glass for NAV_TEST_MS, no phone needed
uint32_t navPopupMetres();       // settings.navPopupM: the near-turn pop-up distance in metres
void navBluetoothStopped();      // from bleShutdown(): the nav service went with Bluetooth, make it again next time
bool navNeedsRestart();          // 0.37.15: switched on while Bluetooth was already running: it starts after a restart
// the screen's own Navigation item (menu > Navigation): Off / Pop Up / Route picked there, the card sent away there
void navSetMode(uint8_t mode);   // NAV_MODE_*: saved like a web UI change; brings back a card that was sent away
void navDismissedOnGlass();      // the card was tapped / held away: in Route it stays away for the rest of the route
bool navSilenced();              // Route, and the card was sent away (it comes back for the next route, or Route again)
void navHouseTick();             // from the house task: starts / stops the phone link after a mode picked on the screen
