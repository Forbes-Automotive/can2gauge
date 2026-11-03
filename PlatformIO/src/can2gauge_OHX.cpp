#include <can2gauge_OHX.h>
#include <can2gauge_Link.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include "mbedtls/md.h"

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static portMUX_TYPE ohxMux = portMUX_INITIALIZER_UNLOCKED;
static OhxStatus lastStatus; // latest status from the followed controller
static volatile uint32_t lastStatusMs = 0;
static uint32_t statusCount = 0; // under ohxMux
static int8_t lastStatusRssi = 0;
static uint8_t myMac[6];
static uint32_t carWrote[SIG_COUNT]; // when this module last wrote a car signal (to step aside for other sources)

struct OtherController
{
  uint8_t mac[6];
  char name[24];
  int8_t rssi;
  uint32_t heardMs;
};
static OtherController others[OHX_OTHERS];

// the command in flight - written by ohxCommand (web / LVGL task) and the OHX task, under ohxMux
struct PendingCommand
{
  uint8_t cmd = 0, arg = 0, seq = 0, tries = 0, state = OHX_CS_NONE, result = OHX_R_NONE;
  bool needSend = false;
  uint32_t sentMs = 0, sentCount = 0, doneMs = 0;
};
static PendingCommand pendingCommand;

static bool zeroMac(const uint8_t *mac)
{
  for (int i = 0; i < 6; i++)
  {
    if (mac[i])
    {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// receive (WiFi task - short)
// ---------------------------------------------------------------------------
// a car signal from OpenHaldex: only while no other source on this gauge (its own CAN decode, OBD ...) is writing it
static void setCarSignal(signalId_t id, float value, uint32_t now)
{
  const bool otherFresh = signalValid(id) && (carWrote[id] == 0 || signalAge(id) < now - carWrote[id]);
  if (otherFresh)
  {
    return;
  }
  signalSetBus(id, value);
  carWrote[id] = millis() | 1; // after the write: never earlier than the signal's own time stamp (or we'd step aside from ourselves)
}

static void publishStatus(const OhxStatus &status, uint32_t now)
{
  // the Haldex values: heard by every gauge in range, like a frame on the shared bus
  signalSetBus(SIG_HALDEX_TARGET, status.lockTarget);
  if (status.lockActual != OHX_NONE8)
  {
    signalSetBus(SIG_HALDEX_ACTUAL, status.lockActual);
  }
  signalSetBus(SIG_HALDEX_MODE, status.mode);
  if (status.speed != OHX_NONE16)
  {
    signalSetBus(SIG_HALDEX_SPEED, status.speed);
  }
  if (status.throttle != OHX_NONE8)
  {
    signalSetBus(SIG_HALDEX_PEDAL, status.throttle);
  }
  if (status.clutchTemp != OHX_NONE_T)
  {
    signalSetBus(SIG_HALDEX_CLUTCH_TEMP, status.clutchTemp / 10.0f);
  }
  if (status.oilTemp != OHX_NONE_T)
  {
    signalSetBus(SIG_HALDEX_OIL_TEMP, status.oilTemp / 10.0f);
  }
  if (status.supplyMv != OHX_NONE16)
  {
    signalSetBus(SIG_HALDEX_VOLTS, status.supplyMv / 1000.0f);
  }

  if (!settings.ohxCarSignals || !(status.flags[0] & OHX_F0_CHASSIS_CAN))
  {
    return;
  }
  if (status.speed != OHX_NONE16)
  {
    setCarSignal(SIG_SPEED, status.speed, now);
  }
  if (status.rpm != OHX_NONE16)
  {
    setCarSignal(SIG_RPM, status.rpm, now);
  }
  if (status.boost != OHX_NONE16)
  {
    setCarSignal(SIG_BOOST, status.boost + 1013.0f, now); // OpenHaldex sends mbar above ambient, SIG_BOOST is absolute (MAP)
  }
  if (status.throttle != OHX_NONE8)
  {
    setCarSignal(SIG_PEDAL, status.throttle, now);
  }
}

void ohxOnEspNow(const uint8_t *mac, int8_t rssi, const uint8_t *data, int len)
{
  if (!settings.ohxWireless || !mac || len < (int)sizeof(OhxStatus) || data[2] != OHX_T_STATUS || data[3] != OHX_PROTO)
  {
    return;
  }
  const OhxStatus *status = (const OhxStatus *)data;
  const uint32_t now = millis();

  if (zeroMac(settings.ohxMac))
  {
    memcpy(settings.ohxMac, mac, 6); // follow the first one heard, and remember it
    settingsDirty = true;
    DEBUG("OHX: following OpenHaldex %02X:%02X:%02X:%02X:%02X:%02X \"%.24s\"", mac[0], mac[1], mac[2], mac[3], mac[4],
          mac[5], status->name);
  }
  if (memcmp(mac, settings.ohxMac, 6) != 0)
  {
    portENTER_CRITICAL(&ohxMux); // another controller in range: list it for the web UI
    int slot = -1;
    for (int i = 0; i < OHX_OTHERS && slot < 0; i++)
    {
      if (!memcmp(others[i].mac, mac, 6))
      {
        slot = i;
      }
    }
    for (int i = 0; i < OHX_OTHERS && slot < 0; i++)
    {
      if (!others[i].heardMs || now - others[i].heardMs > 10000)
      {
        slot = i;
      }
    }
    if (slot >= 0)
    {
      memcpy(others[slot].mac, mac, 6);
      memcpy(others[slot].name, status->name, sizeof(others[slot].name));
      others[slot].name[sizeof(others[slot].name) - 1] = '\0';
      others[slot].rssi = rssi;
      others[slot].heardMs = now | 1;
    }
    portEXIT_CRITICAL(&ohxMux);
    return;
  }

  portENTER_CRITICAL(&ohxMux);
  memcpy(&lastStatus, status, sizeof(OhxStatus));
  lastStatus.name[sizeof(lastStatus.name) - 1] = '\0';
  lastStatusMs = now | 1;
  lastStatusRssi = rssi;
  statusCount++;
  portEXIT_CRITICAL(&ohxMux);
  runtime.ohxRxMs = now | 1;
  publishStatus(*status, now);
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------
static void signCommand(OhxCommand &command)
{
  uint8_t fullHmac[32];
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(md, (const uint8_t *)settings.ohxKey, strlen(settings.ohxKey), (const uint8_t *)&command,
                  offsetof(OhxCommand, hmac), fullHmac);
  memcpy(command.hmac, fullHmac, 8);
}

bool ohxHeard()
{
  // signed: a packet stamped in the same millisecond (| 1) is a tiny negative age, not 4 billion ms ago
  return settings.ohxWireless && lastStatusMs && (int32_t)(millis() - lastStatusMs) < haldexTimeout;
}

bool ohxWantsEspNow()
{
  return settings.ohxWireless != 0;
}

bool ohxCommand(uint8_t cmd, uint8_t arg0)
{
  if (!ohxHeard())
  {
    return false;
  }
  bool ok = false;
  portENTER_CRITICAL(&ohxMux);
  if (pendingCommand.state != OHX_CS_SENDING || (cmd == OHX_C_MODE && pendingCommand.cmd == OHX_C_MODE))
  {
    pendingCommand.cmd = cmd;
    pendingCommand.arg = arg0;
    pendingCommand.seq++;
    pendingCommand.tries = 0;
    pendingCommand.state = OHX_CS_SENDING;
    pendingCommand.result = OHX_R_NONE;
    pendingCommand.needSend = true;
    ok = true;
  }
  portEXIT_CRITICAL(&ohxMux);
  return ok;
}

static void sendPendingCommand()
{
  OhxCommand command;
  memset(&command, 0, sizeof(command));
  command.h.magic[0] = OHX_MAGIC0;
  command.h.magic[1] = OHX_MAGIC1;
  command.h.type = OHX_T_COMMAND;
  command.h.proto = OHX_PROTO;
  uint8_t destinationMac[6];

  portENTER_CRITICAL(&ohxMux);
  command.nonce = lastStatus.nonce;
  command.seq = pendingCommand.seq;
  command.cmd = pendingCommand.cmd;
  command.arg[0] = pendingCommand.arg;
  pendingCommand.tries++;
  pendingCommand.needSend = false;
  pendingCommand.sentMs = millis();
  pendingCommand.sentCount = statusCount;
  memcpy(destinationMac, settings.ohxMac, 6);
  portEXIT_CRITICAL(&ohxMux);

  signCommand(command);
  const bool sent = linkEspNowSendTo(destinationMac, &command, sizeof(command));
#if detailedDebugLink
  DEBUG("OHX: cmd %u arg %u seq %u try %u %s", command.cmd, command.arg[0], command.seq, pendingCommand.tries,
        sent ? "sent" : "send failed");
#else
  (void)sent;
#endif
}

// the OHX task: sends the command, reads its answer from the next status, resends on stale / no answer
static void ohxTaskFn(void *)
{
  for (;;)
  {
    const uint32_t now = millis();
    bool shouldSend = false;
    portENTER_CRITICAL(&ohxMux);
    if (pendingCommand.state == OHX_CS_SENDING)
    {
      if (pendingCommand.needSend)
      {
        shouldSend = true;
      }
      else if (statusCount != pendingCommand.sentCount && lastStatus.ackSrc[0] == myMac[4] &&
               lastStatus.ackSrc[1] == myMac[5] && lastStatus.ackSeq == pendingCommand.seq &&
               lastStatus.ackResult != OHX_R_NONE)
      {
        if (lastStatus.ackResult == OHX_R_STALE && pendingCommand.tries < 4)
        {
          shouldSend = true; // another command got in first: sign again with the new nonce
        }
        else
        {
          pendingCommand.result = lastStatus.ackResult;
          pendingCommand.state = OHX_CS_DONE;
          pendingCommand.doneMs = now;
        }
      }
      else if (now - pendingCommand.sentMs > 400)
      {
        if (pendingCommand.tries < 3)
        {
          shouldSend = true;
        }
        else
        {
          pendingCommand.state = OHX_CS_NO_ANSWER;
          pendingCommand.doneMs = now;
        }
      }
    }
    portEXIT_CRITICAL(&ohxMux);
    if (shouldSend && ohxHeard())
    {
      sendPendingCommand();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------
void setupOpenHaldexEspNow()
{
  memset(&lastStatus, 0, sizeof(lastStatus));
  memset(others, 0, sizeof(others));
  memset(carWrote, 0, sizeof(carWrote));
  esp_read_mac(myMac, ESP_MAC_WIFI_SOFTAP); // ESP-NOW goes out on the AP interface: OpenHaldex acks this MAC
  // stack in PSRAM (saves internal RAM for WiFi and the web server): it only sends commands over ESP-NOW, never
  // touches flash (a settings change just sets settingsDirty; the house task does the saving)
  xTaskCreatePinnedToCoreWithCaps(ohxTaskFn,         // task function
                                  "ohx",             // name (shows in crash reports)
                                  3072,              // stack size in bytes
                                  NULL,              // no parameter
                                  4,                 // priority
                                  NULL,              // no handle kept
                                  0,                 // core 0 (LVGL has core 1)
                                  MALLOC_CAP_SPIRAM); // stack in PSRAM
}

void ohxView(OhxView &view)
{
  memset(&view, 0, sizeof(view));
  const uint32_t now = millis();

  portENTER_CRITICAL(&ohxMux);
  const int32_t ageMs = (int32_t)(now - lastStatusMs); // signed, as in ohxHeard
  view.heard = settings.ohxWireless && lastStatusMs && ageMs < haldexTimeout;
  view.ageMs = 0;
  if (lastStatusMs && ageMs > 0)
  {
    view.ageMs = ageMs;
  }
  view.rssi = lastStatusRssi;
  memcpy(view.mac, settings.ohxMac, 6);
  if (lastStatusMs)
  {
    memcpy(&view.status, &lastStatus, sizeof(lastStatus));
  }
  view.cmd = pendingCommand.cmd;
  view.arg = pendingCommand.arg;
  view.cmdState = pendingCommand.state;
  view.result = pendingCommand.result;
  view.cmdAgeMs = pendingCommand.doneMs ? now - pendingCommand.doneMs : 0;
  for (int i = 0; i < OHX_OTHERS; i++)
  {
    if (others[i].heardMs && now - others[i].heardMs < 10000 && view.others < OHX_OTHERS)
    {
      memcpy(view.otherMac[view.others], others[i].mac, 6);
      memcpy(view.otherName[view.others], others[i].name, sizeof(view.otherName[0]));
      view.otherRssi[view.others] = others[i].rssi;
      view.others++;
    }
  }
  portEXIT_CRITICAL(&ohxMux);

  uint8_t channel = 0;
  wifi_second_chan_t secondChannel;
  if (esp_wifi_get_channel(&channel, &secondChannel) == ESP_OK)
  {
    view.myChannel = channel;
  }
}

void ohxFollow(const uint8_t mac[6])
{
  portENTER_CRITICAL(&ohxMux);
  memcpy(settings.ohxMac, mac, 6);
  lastStatusMs = 0;
  memset(&lastStatus, 0, sizeof(lastStatus));
  pendingCommand.state = OHX_CS_NONE;
  for (int i = 0; i < OHX_OTHERS; i++)
  {
    if (!memcmp(others[i].mac, mac, 6))
    {
      others[i].heardMs = 0; // it is the followed one now
    }
  }
  portEXIT_CRITICAL(&ohxMux);
  runtime.ohxRxMs = 0;
  settingsDirty = true;
}

const char *ohxResultText(uint8_t result)
{
  switch (result)
  {
  case OHX_R_OK: return "done";
  case OHX_R_STALE: return "OpenHaldex was busy with another gauge's command - try again";
  case OHX_R_AUTH: return "refused: wrong OpenHaldex WiFi password on this gauge";
  case OHX_R_DISABLED: return "refused: 'Allow Gauges to Change Settings' is off on the OpenHaldex";
  case OHX_R_MOVING: return "refused: stop the car first";
  case OHX_R_NO_HALDEX: return "refused: no Haldex CAN data on the OpenHaldex";
  case OHX_R_BUSY: return "refused: a learn is already running";
  case OHX_R_INVALID: return "refused: not valid";
  case OHX_R_UNSUPPORTED: return "refused: not available for this generation";
  case OHX_R_CTRL_OFF: return "refused: the controller is disabled";
  default: return "";
  }
}

const char *ohxGenerationName(uint8_t generation)
{
  switch (generation)
  {
  case 1: return "Generation 1";
  case 2: return "Generation 2";
  case 3: return "Generation 3 - Volvo";
  case 4: return "Generation 4";
  case 41: return "Generation 4 - GM/SAAB";
  case 50: return "Generation 5 (0CQ)";
  case 51: return "Generation 5 (0AY)";
  case 52: return "Generation 5 (0CQ VAQ)";
  case 60: return "Generation 6 (MQB evo+)";
  default: return "Generation ?";
  }
}

bool haldexHeardOnCan()
{
  return runtime.haldexCanMs && millis() - runtime.haldexCanMs < haldexTimeout;
}

bool haldexRequestMode(uint8_t mode)
{
  if (haldexHeardOnCan())
  {
    return linkHaldexMode(mode); // wired: 0x6A0 (or through the chain master) - no key needed
  }
  if (ohxHeard())
  {
    return ohxCommand(OHX_C_MODE, mode);
  }
  return linkHaldexMode(mode); // nothing heard: as before (a follower asks the master)
}

const char *haldexHeardVia()
{
  const bool onCan = haldexHeardOnCan();
  const bool onWiFi = ohxHeard();
  if (onCan && onWiFi)
  {
    return "CAN + WiFi";
  }
  if (onCan)
  {
    return "CAN";
  }
  if (onWiFi)
  {
    return "WiFi";
  }
  return "";
}
