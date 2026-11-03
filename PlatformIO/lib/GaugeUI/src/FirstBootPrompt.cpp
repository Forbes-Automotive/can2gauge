// First Boot prompt on the glass: the question card, then the login card (see FirstBootPrompt.h)
#include "FirstBootPrompt.h"
#include <lvgl.h>
#include <LVGL_Driver.h>         // lvglLock / lvglUnlock
#include <can2gauge_defs.h>      // settings (for the theme), copyStr, WIFI_LEN
#include <can2gauge_themes.h>    // themeColours: the accent colour of the theme in use

// static = private to this file: nothing else needs to reach these
static lv_obj_t *backdrop = NULL;     // full-screen black layer holding both cards (catches every touch)
static lv_obj_t *questionBox = NULL;  // "Set this gauge up from your phone?" + the two buttons
static lv_obj_t *loginBox = NULL;     // WiFi name / password / address
static lv_timer_t *askTimer = NULL;   // shows the question after the boot logo
static uiFirstBootAnswer_t answerCallback = NULL; // where the driver's answer goes (src/can2gauge_FirstBoot.cpp)
static char loginWifiName[WIFI_LEN];     // the gauge's own WiFi name, for the login card
static char loginWifiPassword[WIFI_LEN]; // its password ("" = open network)
static char loginAddress[32];            // the address to type in the browser (192.168.1.1)
static char loginLocalName[32];          // ... or its short name ("c2g.local")
static bool showLoginFirst = false;      // true = straight to the login card (Yes was said on an earlier boot)

#define PROMPT_TEXT_COLOUR 0xFFFFFF  // white text
#define PROMPT_MUTED_COLOUR 0x8A94A6 // grey for the small print
#define PROMPT_PANEL_COLOUR 0x1C222B // the "Not Now" button

// the accent colour of the theme in use (the dial colour)
static uint32_t accentHex()
{
  uint32_t dial = 0;
  uint32_t needle = 0;
  themeColours(themeEffective(settings.theme), dial, needle);
  return dial;
}

// one centred line (or wrapped block) of text on a card
static lv_obj_t *promptLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t colourHex, int y, const char *text)
{
  lv_obj_t *textLabel = lv_label_create(parent);
  lv_obj_set_style_text_font(textLabel, font, 0);
  lv_obj_set_style_text_color(textLabel, lv_color_hex(colourHex), 0);
  lv_obj_set_style_text_align(textLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(textLabel, 340); // keeps the text inside the round glass
  lv_label_set_long_mode(textLabel, LV_LABEL_LONG_WRAP);
  lv_label_set_text(textLabel, text);
  lv_obj_align(textLabel, LV_ALIGN_CENTER, 0, y);
  return textLabel;
}

// an empty see-through box the size of the screen, to group one card's items
static lv_obj_t *cardBox()
{
  lv_obj_t *box = lv_obj_create(backdrop);
  lv_obj_set_size(box, 480, 480);
  lv_obj_center(box);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE); // taps go to the buttons, or through to the backdrop
  lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
  return box;
}

// a rounded button with centred text
static lv_obj_t *promptButton(lv_obj_t *parent, int x, uint32_t backgroundHex, uint32_t textHex, const char *text,
                              lv_event_cb_t onClick)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_size(button, 150, 58);
  lv_obj_align(button, LV_ALIGN_CENTER, x, 110);
  lv_obj_set_style_bg_color(button, lv_color_hex(backgroundHex), 0);
  lv_obj_set_style_radius(button, 29, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_t *buttonLabel = lv_label_create(button);
  lv_obj_set_style_text_font(buttonLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(buttonLabel, lv_color_hex(textHex), 0);
  lv_label_set_text(buttonLabel, text);
  lv_obj_center(buttonLabel);
  lv_obj_add_event_cb(button, onClick, LV_EVENT_CLICKED, NULL);
  return button;
}

// Yes: save the answer and swap to the login card
static void yesClicked(lv_event_t *event)
{
  lv_event_stop_bubbling(event); // the backdrop's own tap (close the login card) must not see this tap too
  lv_obj_add_flag(questionBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(loginBox, LV_OBJ_FLAG_HIDDEN);
  if (answerCallback)
  {
    answerCallback(true);
  }
}

// Not now: save the answer and clear the glass
static void notNowClicked(lv_event_t *event)
{
  lv_event_stop_bubbling(event);
  lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
  if (answerCallback)
  {
    answerCallback(false);
  }
}

// a tap anywhere on the login card closes it (the web UI still opens the setup page when the driver logs in)
static void backdropClicked(lv_event_t *event)
{
  if (lv_obj_has_flag(loginBox, LV_OBJ_FLAG_HIDDEN))
  {
    return; // the question card is up: only its two buttons answer it
  }
  lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
}

// builds the backdrop and both cards (once, the first time the question shows)
static void buildPrompt()
{
  const uint32_t accent = accentHex();

  backdrop = lv_obj_create(lv_layer_top());
  lv_obj_set_size(backdrop, 480, 480);
  lv_obj_center(backdrop);
  lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(backdrop, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(backdrop, 0, 0);
  lv_obj_set_style_radius(backdrop, 0, 0);
  lv_obj_set_style_pad_all(backdrop, 0, 0);
  lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE); // swallows swipes / long presses meant for the gauge page below
  lv_obj_add_event_cb(backdrop, backdropClicked, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);

  // the question
  questionBox = cardBox();
  promptLabel(questionBox, &lv_font_montserrat_32, accent, -125, "Welcome");
  promptLabel(questionBox, &lv_font_montserrat_24, PROMPT_TEXT_COLOUR, -60, "Set this gauge up\nfrom your phone?");
  promptLabel(questionBox, &lv_font_montserrat_16, PROMPT_MUTED_COLOUR, 20,
              "It checks what is connected and\nsuggests the gauge pages to show.");
  promptButton(questionBox, -82, PROMPT_PANEL_COLOUR, PROMPT_TEXT_COLOUR, "Not Now", notNowClicked);
  promptButton(questionBox, 82, accent, 0x000000, "Yes", yesClicked);

  // how to log in
  loginBox = cardBox();
  promptLabel(loginBox, &lv_font_montserrat_28, accent, -140, "Log in to set up");
  promptLabel(loginBox, &lv_font_montserrat_16, PROMPT_MUTED_COLOUR, -90, "1.  Join the WiFi");
  promptLabel(loginBox, &lv_font_montserrat_28, PROMPT_TEXT_COLOUR, -58, loginWifiName);
  if (loginWifiPassword[0])
  {
    char passwordLine[WIFI_LEN + 16];
    snprintf(passwordLine, sizeof(passwordLine), "password  %s", loginWifiPassword);
    promptLabel(loginBox, &lv_font_montserrat_20, PROMPT_TEXT_COLOUR, -20, passwordLine);
  }
  else
  {
    promptLabel(loginBox, &lv_font_montserrat_20, PROMPT_MUTED_COLOUR, -20, "no password");
  }
  promptLabel(loginBox, &lv_font_montserrat_16, PROMPT_MUTED_COLOUR, 30, "2.  Open in the browser");
  promptLabel(loginBox, &lv_font_montserrat_28, accent, 62, loginAddress);
  if (loginLocalName[0])
  {
    char localLine[40];
    snprintf(localLine, sizeof(localLine), "or  %s", loginLocalName);
    promptLabel(loginBox, &lv_font_montserrat_18, PROMPT_TEXT_COLOUR, 96, localLine);
  }
  promptLabel(loginBox, &lv_font_montserrat_14, PROMPT_MUTED_COLOUR, 140,
              "Setup starts when the page opens.\nTap to close.");
}

// the boot logo has had its time: put the question up
static void askTimerFired(lv_timer_t *timer)
{
  askTimer = NULL; // repeat count 1: LVGL deletes the timer itself after this callback
  if (!backdrop)
  {
    buildPrompt();
  }
  if (showLoginFirst)
  {
    lv_obj_add_flag(questionBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(loginBox, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    lv_obj_remove_flag(questionBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(loginBox, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(backdrop); // above an alert or the progress ring
}

void uiFirstBootAsk(const char *wifiName, const char *wifiPassword, const char *address, const char *localName,
                    uiFirstBootAnswer_t onAnswer, uint32_t delayMs, bool loginFirst)
{
  showLoginFirst = loginFirst;
  copyStr(loginWifiName, sizeof(loginWifiName), wifiName);
  copyStr(loginWifiPassword, sizeof(loginWifiPassword), wifiPassword);
  copyStr(loginAddress, sizeof(loginAddress), address);
  copyStr(loginLocalName, sizeof(loginLocalName), localName);
  answerCallback = onAnswer;
  if (!lvglLock(100))
  {
    return; // no display (or it is stuck): the web UI still offers the setup page
  }
  if (askTimer)
  {
    lv_timer_delete(askTimer);
  }
  if (delayMs < 1)
  {
    delayMs = 1; // an LVGL timer needs a period
  }
  askTimer = lv_timer_create(askTimerFired, delayMs, NULL);
  lv_timer_set_repeat_count(askTimer, 1); // fire once
  lvglUnlock();
}

void uiFirstBootHide()
{
  if (!lvglLock(100))
  {
    return;
  }
  if (askTimer) // not shown yet: never show it
  {
    lv_timer_delete(askTimer);
    askTimer = NULL;
  }
  if (backdrop)
  {
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
  }
  lvglUnlock();
}

bool uiFirstBootShowing()
{
  if (askTimer)
  {
    return true; // about to show
  }
  if (!backdrop)
  {
    return false;
  }
  return !lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
}
