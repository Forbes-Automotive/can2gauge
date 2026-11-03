#pragma once
// ---------------------------------------------------------------------------
// First Boot prompt on the glass (src/can2gauge_FirstBoot.cpp decides when it shows).
//   question card   "Set this gauge up from your phone?"  [Not Now]  [Yes]
//   login card      after Yes: the gauge's WiFi name, its password and the address to open. A tap closes it; the web
//                   UI's setup page opens by itself when the driver logs in.
// Both cards sit over everything on LVGL's top layer and swallow touches, so a swipe can't change page underneath.
// ---------------------------------------------------------------------------
#include <stdint.h>

// called once when the driver answers the question: true = Yes, false = Not Now
typedef void (*uiFirstBootAnswer_t)(bool yes);

// shows the question card delayMs from now (after the boot logo). The WiFi name / password ("" = open) / address
// ("192.168.1.1") / local name ("c2g.local") are copied, for the login card that follows a Yes. loginFirst = skip the
// question and show the login card (Yes was said on an earlier boot but the web UI was never opened).
void uiFirstBootAsk(const char *wifiName, const char *wifiPassword, const char *address, const char *localName,
                    uiFirstBootAnswer_t onAnswer, uint32_t delayMs, bool loginFirst);
void uiFirstBootHide();     // takes either card off the glass (the web UI's setup page has opened)
bool uiFirstBootShowing();  // a card is on the glass
