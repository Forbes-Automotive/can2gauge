#pragma once
#include <can2gauge_defs.h>
#include <c2gs.h> // SPRITE_NAME_LEN

// ---------------------------------------------------------------------------
// Shared settings (0.30.4): the settings every gauge in a chain should have the same, kept in step over WiFi (ESP-NOW).
// Set them up on any gauge and the others take them - no need to log into each one.
//
//   Ignition and shutdown  Off Delay, Deep Sleep After, the Shutdown Sprite, and (0.30.10) WHICH gauge has the
//                          ignition: give one gauge the wire or CAN terminal 15 and every other gauge switches to
//                          "Another gauge in the chain". The last gauge given it wins, so one change sets the chain.
//   Screensaver            on / off, delay, its sprite, reacts to the car
//   Look                   theme and custom colours, day / night brightness, gauge feel, units
//   Boot logo              which logo, its text, the make logo, the logo -> page fade
//
// Never shared: what each page shows, the row number, name, role, rotation, drawing options, WiFi, outputs / set
// points and diagnostics.
//
// How the gauges agree (src/can2gauge_Link.cpp does the sending):
//   - each gauge keeps a revision number (sharedSettingsRev, saved with the settings). A change made on a gauge (web UI
//     or the glass) gives it a revision one higher than any heard, and it sends its shared settings to the others.
//   - a gauge takes shared settings with a higher revision than its own. With the same revision but different values
//     (a chain just updated to 0.30.4, or a gauge just added) it takes the master's.
//   - the heartbeat carries each gauge's revision and a check value of its shared settings, so a gauge that missed a
//     change (asleep, out of range) is sent it again.
// ---------------------------------------------------------------------------

// the shared settings as they travel: one fixed layout, every text field zero-filled after its end
struct __attribute__((packed)) SharedBlock // packed: sent over the air, so no gaps between the fields
{
  uint8_t layout;                       // SHARED_LAYOUT: a gauge ignores a layout it doesn't know
  // ignition and shutdown
  uint8_t ignOffDelayS;                 // settings.ignOffDelayS
  uint8_t standbyMin;                   // settings.standbyMin
  char shutdownSprite[SPRITE_NAME_LEN]; // settings.shutdownSprite
  // screensaver
  uint8_t saverEnabled;                 // settings.saverEnabled
  uint8_t saverReact;                   // settings.saverReact
  uint16_t saverDelayS;                 // settings.saverDelayS
  char saverSprite[SPRITE_NAME_LEN];    // settings.saverSprite
  // look
  uint8_t theme;                        // settings.theme
  uint32_t themeDial;                   // settings.themeDial
  uint32_t themeNeedle;                 // settings.themeNeedle
  uint8_t brightness;                   // settings.brightness
  uint8_t nightBrightness;              // settings.nightBrightness
  uint8_t gaugeFeel;                    // settings.gaugeFeel
  uint8_t unitSpeed;                    // settings.unitSpeed ... (settings.units, the quick choice, is worked out
  uint8_t unitBoost;                    //   from these rows on each gauge, so it isn't sent)
  uint8_t unitPressure;
  uint8_t unitTemp;
  uint8_t unitMixture;
  uint8_t unitDistance;
  // boot logo
  uint8_t bootLogo;                     // settings.bootLogo
  char bootText[24];                    // settings.bootText
  char logoMake[12];                    // settings.logoMake
  uint16_t bootFadeMs;                  // settings.bootFadeMs
  // 0.30.10: the gauge with the ignition
  uint8_t ignitionGauge[6];             // its WiFi MAC address (all zero = none: every gauge on switched power)
};
#define SHARED_LAYOUT 2 // bump if SharedBlock ever changes (2 = 0.30.10, the ignition gauge added)

extern uint32_t sharedSettingsRev;   // this gauge's revision of the shared settings (saved by writeEEP)
extern uint8_t ignitionGaugeMac[6];  // 0.30.10: the gauge this one follows for the ignition (saved by writeEEP)

void setupShared();                           // from setupLink: makes the lock below
void sharedBuild(SharedBlock &out);           // this gauge's shared settings, as they'd be sent
uint32_t sharedCheck(const SharedBlock &block); // a check value (CRC-32) of a block: same settings = same number
uint32_t sharedMyCheck();                     // the check value of this gauge's shared settings as last agreed
bool sharedLock();                            // the link task and the house task take turns at the shared settings
void sharedUnlock();
bool sharedOffer(const SharedBlock &block, uint32_t rev); // link: settings from another gauge, to take on (false = busy)
bool sharedPending();                         // one is waiting to be taken on
bool sharedNoteLocalChange(uint32_t highestHeardRev); // link, every second: true when this gauge's shared settings
                                                      // changed here (the revision has then been moved on)
void sharedTick();                            // house task: takes on a waiting block (theme, units, backlight)
void sharedMyMac(uint8_t mac[6]);             // 0.30.10: this gauge's WiFi MAC, as the other gauges see it
