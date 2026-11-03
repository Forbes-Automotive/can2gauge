// Shared settings: the settings every gauge in a chain keeps the same, kept in step over ESP-NOW. See
// can2gauge_Shared.h for what is shared and how the gauges agree; src/can2gauge_Link.cpp does the sending.
#include <can2gauge_Shared.h>
#include <can2gauge_IO.h>      // outputsApply(): sets the backlight from the new brightness
#include <can2gauge_Units.h>   // unitsOf / unitsConvertPages / unitsQuickFromRows
#include <can2gauge_UserSig.h> // userSigApply(): page names and units after a units change
#include <GaugeUI.h>           // uiApplyTheme / uiRebuildPages / uiRebuildMenu
#include <esp_rom_crc.h>       // esp_rom_crc32_le(): the check value
#include <esp_mac.h>           // esp_read_mac(): this gauge's WiFi MAC

// ===== Globals (declared in can2gauge_Shared.h) =====
uint32_t sharedSettingsRev = 0;                   // this gauge's revision of the shared settings (read / saved by the EEP code)
uint8_t ignitionGaugeMac[6] = {0, 0, 0, 0, 0, 0}; // the gauge followed for the ignition (read / saved by the EEP code)

// ===== Variables for this file =====
static uint32_t pendingRev = 0;              // the waiting block's revision
static uint32_t agreedCheck = 0;             // the check value of this gauge's shared settings when last sent or taken on
static volatile bool hasPending = false;     // pendingBlock is waiting (volatile: set from the WiFi task, read by others)
static bool hasAgreedCheck = false;          // false until the first look after boot (nothing to compare with yet)
static SemaphoreHandle_t sharedMutex = NULL; // the link task and the house task take turns (made in setupShared)
static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED; // guards the waiting block (written from the WiFi task)
static SharedBlock pendingBlock;                               // settings from another gauge, waiting for the house task

// ===== Small helpers =====
// copy a name into a fixed field, zero-filled after its end (so the same name always gives the same bytes)
static void copyField(char *to, size_t size, const char *from)
{
  memset(to, 0, size);
  strncpy(to, from, size - 1);
}

// does this gauge have its own ignition (the wire, or terminal 15 on CAN)?
static bool hasOwnIgnition()
{
  return settings.ignSource == IGN_SRC_CAN || settings.ignSource == IGN_SRC_WIRE;
}

// ===== Functions in this file =====
static bool isNoGauge(const uint8_t mac[6]); // is this MAC all zeros (no gauge)?

// from setupLink: makes the lock the link task and the house task share
void setupShared()
{
  sharedMutex = xSemaphoreCreateMutex();
}

// house task: take on a block another gauge sent
void sharedTick()
{
  if (!hasPending)
  {
    return;
  }
  if (!sharedLock())
  {
    return; // the link task is looking: next time round
  }
  SharedBlock block;
  uint32_t rev;
  portENTER_CRITICAL(&pendingMux);
  memcpy(&block, &pendingBlock, sizeof(block));
  rev = pendingRev;
  hasPending = false;
  portEXIT_CRITICAL(&pendingMux);

  const UnitChoices unitsBefore = unitsOf(settings);
  const uint8_t themeBefore = settings.theme;
  const uint32_t dialBefore = settings.themeDial;
  const uint32_t needleBefore = settings.themeNeedle;
  const uint8_t feelBefore = settings.gaugeFeel;

  settings.ignOffDelayS = block.ignOffDelayS;
  settings.standbyMin = block.standbyMin;
  copyField(settings.shutdownSprite, sizeof(settings.shutdownSprite), block.shutdownSprite);
  settings.saverEnabled = block.saverEnabled;
  settings.saverReact = block.saverReact;
  settings.saverDelayS = block.saverDelayS;
  copyField(settings.saverSprite, sizeof(settings.saverSprite), block.saverSprite);
  settings.theme = block.theme;
  settings.themeDial = block.themeDial;
  settings.themeNeedle = block.themeNeedle;
  settings.brightness = block.brightness;
  settings.nightBrightness = block.nightBrightness;
  settings.gaugeFeel = block.gaugeFeel;
  settings.unitSpeed = block.unitSpeed;
  settings.unitBoost = block.unitBoost;
  settings.unitPressure = block.unitPressure;
  settings.unitTemp = block.unitTemp;
  settings.unitMixture = block.unitMixture;
  settings.unitDistance = block.unitDistance;
  settings.bootLogo = block.bootLogo;
  copyField(settings.bootText, sizeof(settings.bootText), block.bootText);
  copyField(settings.logoMake, sizeof(settings.logoMake), block.logoMake);
  settings.bootFadeMs = block.bootFadeMs;
  settings.units = unitsQuickFromRows(); // the quick choice follows the rows, as on the web UI

  // the gauge with the ignition. The last gauge given it wins: if that's another gauge, this one follows it (even if
  // it had the wire or CAN set before - that's the "one ignition source" rule).
  uint8_t myMac[6];
  sharedMyMac(myMac);
  bool shouldSendOurs = false; // this gauge knows better about its own ignition: it sends its settings as a change
  if (isNoGauge(block.ignitionGauge))
  {
    if (settings.ignSource == IGN_SRC_CHAIN)
    {
      settings.ignSource = IGN_SRC_OFF; // nobody to follow: back to switched power
    }
    else if (hasOwnIgnition())
    {
      shouldSendOurs = true; // keep ours, and send it as a change (the others then follow this gauge)
    }
  }
  else if (memcmp(block.ignitionGauge, myMac, 6) == 0)
  {
    // it's this gauge: it keeps its own setting
    if (!hasOwnIgnition())
    {
      shouldSendOurs = true; // ... but it no longer has one: it tells the others (none), so they stop following
    }
  }
  else
  {
    settings.ignSource = IGN_SRC_CHAIN; // another gauge has it: follow that one
    memcpy(ignitionGaugeMac, block.ignitionGauge, 6);
  }

  sharedSettingsRev = rev;
  settingsDirty = true; // saved by eepTick (this task, next time round)

  if (shouldSendOurs)
  {
    agreedCheck = sharedCheck(block); // what it was sent: ours now differs, so the link sends ours as a new change
  }
  else
  {
    SharedBlock nowMine;
    sharedBuild(nowMine);
    agreedCheck = sharedCheck(nowMine); // so the link doesn't take this as a change made here
  }
  hasAgreedCheck = true;
  sharedUnlock();
  DEBUG("shared: settings from another gauge taken on - revision %lu", (unsigned long)rev);

  // now make the screen show them (outside the lock: these can take a while)
  const UnitChoices unitsAfter = unitsOf(settings);
  const bool unitsChanged = memcmp(&unitsBefore, &unitsAfter, sizeof(UnitChoices)) != 0;
  if (unitsChanged)
  {
    unitsConvertPages(unitsBefore); // each page's Min / Max / Redline into the new units
    userSigApply();
  }
  const bool themeChanged = themeBefore != settings.theme || dialBefore != settings.themeDial ||
                            needleBefore != settings.themeNeedle;
  if (themeChanged)
  {
    uiApplyTheme(); // new colours: every screen rebuilt (pages and menu too)
  }
  else if (unitsChanged || feelBefore != settings.gaugeFeel)
  {
    uiRebuildPages();
    uiRebuildMenu();
  }
  outputsApply(); // sets the backlight from the (maybe new) brightness
}

// link (ESP-NOW receive): settings from another gauge, left for the house task to take on. false = a layout this
// firmware doesn't know.
bool sharedOffer(const SharedBlock &block, uint32_t rev)
{
  if (block.layout != SHARED_LAYOUT)
  {
    return false; // a layout this firmware doesn't know
  }
  portENTER_CRITICAL(&pendingMux);
  memcpy(&pendingBlock, &block, sizeof(pendingBlock));
  pendingRev = rev;
  hasPending = true;
  portEXIT_CRITICAL(&pendingMux);
  return true;
}

// a block from another gauge is waiting to be taken on
bool sharedPending()
{
  return hasPending;
}

// link, every second: true when this gauge's shared settings were changed here (web UI or the screen). The revision
// then moves one past the highest heard, so every other gauge takes them.
bool sharedNoteLocalChange(uint32_t highestHeardRev)
{
  SharedBlock mine;
  sharedBuild(mine);
  const uint32_t check = sharedCheck(mine);
  if (!hasAgreedCheck)
  {
    agreedCheck = check; // the first look after boot: this is where we start from
    hasAgreedCheck = true;
    return false;
  }
  if (check == agreedCheck)
  {
    return false;
  }
  // changed here: one revision past anything heard, so every other gauge takes it
  uint32_t newRev = sharedSettingsRev;
  if (highestHeardRev > newRev)
  {
    newRev = highestHeardRev;
  }
  sharedSettingsRev = newRev + 1;
  settingsDirty = true; // saves the new revision with the settings
  agreedCheck = check;
  DEBUG("shared: changed here - revision %lu", (unsigned long)sharedSettingsRev);
  return true;
}

// this gauge's shared settings, as they'd be sent
void sharedBuild(SharedBlock &out)
{
  memset(&out, 0, sizeof(out)); // no leftover bytes: the check value must only depend on the settings
  out.layout = SHARED_LAYOUT;
  out.ignOffDelayS = settings.ignOffDelayS;
  out.standbyMin = settings.standbyMin;
  copyField(out.shutdownSprite, sizeof(out.shutdownSprite), settings.shutdownSprite);
  out.saverEnabled = settings.saverEnabled;
  out.saverReact = settings.saverReact;
  out.saverDelayS = settings.saverDelayS;
  copyField(out.saverSprite, sizeof(out.saverSprite), settings.saverSprite);
  out.theme = settings.theme;
  out.themeDial = settings.themeDial;
  out.themeNeedle = settings.themeNeedle;
  out.brightness = settings.brightness;
  out.nightBrightness = settings.nightBrightness;
  out.gaugeFeel = settings.gaugeFeel;
  out.unitSpeed = settings.unitSpeed;
  out.unitBoost = settings.unitBoost;
  out.unitPressure = settings.unitPressure;
  out.unitTemp = settings.unitTemp;
  out.unitMixture = settings.unitMixture;
  out.unitDistance = settings.unitDistance;
  out.bootLogo = settings.bootLogo;
  copyField(out.bootText, sizeof(out.bootText), settings.bootText);
  copyField(out.logoMake, sizeof(out.logoMake), settings.logoMake);
  out.bootFadeMs = settings.bootFadeMs;
  // which gauge has the ignition, as this gauge sees it
  if (hasOwnIgnition())
  {
    sharedMyMac(out.ignitionGauge); // this one
  }
  else if (settings.ignSource == IGN_SRC_CHAIN)
  {
    memcpy(out.ignitionGauge, ignitionGaugeMac, 6); // the one it follows
  }
  // switched power: left all zero (none)
}

// a check value (CRC-32) of a block: the same settings give the same number
uint32_t sharedCheck(const SharedBlock &block)
{
  return esp_rom_crc32_le(0, (const uint8_t *)&block, sizeof(block)); // the block's bytes as one number
}

// the check value of this gauge's shared settings as last agreed (sent or taken on)
uint32_t sharedMyCheck()
{
  return agreedCheck;
}

// takes the lock the link task and the house task share (waits up to 200 ms). false = not set up yet, or busy
bool sharedLock()
{
  if (!sharedMutex)
  {
    return false; // not set up yet
  }
  return xSemaphoreTake(sharedMutex, pdMS_TO_TICKS(200)) == pdTRUE;
}

// gives the lock back
void sharedUnlock()
{
  xSemaphoreGive(sharedMutex);
}

// this gauge's WiFi MAC address, the one the other gauges hear its ESP-NOW packets from
void sharedMyMac(uint8_t mac[6])
{
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
}

// is this MAC all zeros (no gauge)?
static bool isNoGauge(const uint8_t mac[6])
{
  for (uint8_t i = 0; i < 6; i++)
  {
    if (mac[i] != 0)
    {
      return false;
    }
  }
  return true;
}
