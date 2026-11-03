#include <can2gauge_Link.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <can2gauge_DSG.h>
#include <can2gauge_VagDiag.h> // vdiagPolling(): VAG diag values count as an active source in the election
#include <can2gauge_ELM.h>
#include <can2gauge_IO.h>
#include <can2gauge_OHX.h> // OpenHaldex over ESP-NOW shares this ESP-NOW stack
#include <can2gauge_ADC2CAN.h> // 0.8.0: so does adc2can over ESP-NOW
#include <adcnow_proto.h>
#include <can2gauge_CanRelay.h> // CAN frames relayed by OpenHaldex / adc2can (include/cannow_proto.h)
#include <can2gauge_ECU.h> // an aftermarket ECU's data from usb2ip (include/ecunow_proto.h)
#include <can2gauge_Power.h> // the row wave's "Play it now" (powerTestShutdown)
#include <can2gauge_ChainUpdate.h> // 0.25.0: gauge-to-gauge updates ride on this ESP-NOW stack too
#include <can2gauge_Shared.h> // 0.30.4: the shared settings (theme, units, screensaver ...) ride on it too
#include <CANLine.h>
#include <GaugeUI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include "mbedtls/md.h"

// ---------------------------------------------------------------------------
// wire formats
// ---------------------------------------------------------------------------
// CAN DATA      base+node       [sigA][mantA lo][mantA hi][sigB][mantB lo][mantB hi][seq][decA<<4 | decB]
//               value = mant / 10^(dec-4): each value picks its own decimals, so any range fits an int16 at ~4.5 digits
// CAN HEARTBEAT base+0x10+node  [group lo][group hi][role | row<<2 | night<<7][proto][fw major<<4 | minor][sources][owned][uptime/10 s]
//               role = bits 0-1, row position = bits 2-5 (0.12.0, was always 0, so older gauges read "not in a row"), night = bit 7
//               bit 6 (0.29.2) = it has its own ignition (CAN or wire) and it is on: "Ignition from another gauge" follows it
// CAN COMMAND   base+0x20       [dst (0xFF all)][src][cmd][arg0][arg1][arg2][seq][0]    sent 3x, receivers de-duplicate
// CAN BOOT SYNC base+0x21       (0.30.11, src/can2gauge_BootSync.cpp - not handled here) [group lo][group hi][logo ms lo][logo ms hi][0..]
// ESP-NOW       'C' '2' type group(le16) node proto seq + body (<= 250 bytes, valid on ESP-NOW v1 and v2)
//   DATA        count, count x {sig, 0, float32}
//   HEARTBEAT   role|row|night (as on CAN), fw, sources, owned, uptime u32, name[16]
//   COMMAND     dst, cmd, arg0, arg1, arg2, cseq, hmac[8] = HMAC-SHA256(CHAIN_KEY, header + the 6 bytes before it)
//   SHARED      (0.30.4) rev u32, fromMaster, SharedBlock (include/can2gauge_Shared.h), hmac[8] over all before it
enum : uint8_t
{
  NOW_DATA = 1,
  NOW_HEARTBEAT = 2,
  NOW_COMMAND = 3,
  NOW_SHARED = 4, // 0.30.4: the shared settings (an older gauge ignores a type it doesn't know)
};
enum : uint8_t
{
  CMD_HALDEX_MODE = 1,
  CMD_GOTO_PAGE = 2,
  CMD_CLEAR_ALERTS = 3,
  CMD_ROW_TEST = 4, // 0.12.0: every gauge in the row plays its shutdown now (the web UI's "Play it now")
  CMD_SET_ROW = 5,     // 0.29.0: arg0 = the gauge's new place in the row (0 = not in a row) - set from another gauge
  CMD_SET_ROLE = 6,    // 0.29.0: arg0 = its new role (chainRole_t: Auto / Master / Follower)
  CMD_SHOW_NUMBER = 7, // 0.29.0: arg0 = 1 show its number big on the glass, 0 = hide it
};

struct __attribute__((packed)) NowHeader
{
  uint8_t magic[2];
  uint8_t type;
  uint16_t group;
  uint8_t node;
  uint8_t proto;
  uint8_t seq;
};
struct __attribute__((packed)) NowSignal
{
  uint8_t sig;
  uint8_t flags;
  float value;
};
struct __attribute__((packed)) NowHeartbeat
{
  NowHeader header;
  uint8_t role;
  uint8_t fw;
  uint8_t sources;
  uint8_t owned;
  uint32_t uptimeS;
  char name[NAME_LEN];
  // 0.25.0: added at the end - an older gauge only checks the packet is at least as long as it expects, so it still
  // reads the fields above and skips these
  uint8_t version[3];  // firmware major, minor, patch (fw above only has room for 4 bits of each)
  uint8_t board;       // chainUpdateBoardId()
  uint8_t updateProto; // CHAIN_UPDATE_PROTO: can take a chain update
  // 0.30.4: added at the end (zero from an older gauge)
  uint8_t ignKind;      // settings.ignSource + 1 (0 = an older gauge that doesn't say)
  uint32_t sharedRev;   // its revision of the shared settings (can2gauge_Shared.h)
  uint32_t sharedCheck; // the check value of its shared settings
};
#define NOW_HEARTBEAT_OLD_LEN offsetof(NowHeartbeat, version) // a heartbeat from a gauge before 0.25.0
struct __attribute__((packed)) NowCommand
{
  NowHeader header;
  uint8_t dst, cmd, arg0, arg1, arg2, cseq;
  uint8_t hmac[8];
};
struct __attribute__((packed)) NowShared // 0.30.4: one gauge's shared settings, for the others to take on
{
  NowHeader header;
  uint32_t rev;       // the sender's revision of them
  uint8_t fromMaster; // 1 = sent by the master (wins when the revisions are the same)
  SharedBlock block;  // the settings themselves
  uint8_t hmac[8];    // signed like a command: these change another gauge's settings
};
static_assert(sizeof(NowShared) <= 250, "the shared settings packet must fit one ESP-NOW packet");
#define NOW_MAX_SIGS 40
static_assert(sizeof(NowHeader) + 1 + NOW_MAX_SIGS * sizeof(NowSignal) <= 250, "ESP-NOW DATA packet must fit 250 bytes");

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
#define MAX_NODES 16
EXT_RAM_BSS_ATTR static LinkNode nodes[MAX_NODES]; // indexed by node id
static portMUX_TYPE nodeMux = portMUX_INITIALIZER_UNLOCKED;
static LinkStats stats;
static uint8_t myNode = 0;
static bool iAmMaster = true;
static int8_t masterNode = -1;
static bool masterSeenEver = false;
static uint32_t canHeardMs = 0;
static uint8_t dataSeq = 0;
static uint8_t commandSeq = 0;
static uint8_t espNowSeq = 0;
static uint8_t fwByte = 0;
static uint8_t collisionHits = 0;
EXT_RAM_BSS_ATTR static uint32_t lastSent[SIG_COUNT];
static uint8_t lastCommandSeq[MAX_NODES];
static volatile bool wifiReady = false;
static volatile int16_t pendingPage = -1;   // page change to broadcast (set from the LVGL task, sent by the link task)
static volatile int16_t pendingHaldex = -1; // Haldex mode request to forward to the master
static volatile bool pendingRowTest = false; // "Play it now" for the row: tell the others, then play (set by the web UI)
static SemaphoreHandle_t espNowMutex = NULL;
static QueueHandle_t commandQueue = NULL; // commands received in the CAN / WiFi callbacks, executed by the link task
static QueueHandle_t sendQueue = NULL;    // 0.29.0: commands the web UI wants sent to other gauges, sent by the link task
#define SHOW_NUMBER_MS 15000              // 0.29.0: Show Numbers keeps the number on the glass this long
#define CHANGED_NUMBER_MS 6000            // ... and a gauge whose number / role was just changed shows it this long

// 0.29.0: a command waiting to go out to another gauge (dst = its node id, 0xFF = every gauge)
struct OutgoingCommand
{
  uint8_t dst, cmd, arg0;
};

struct PendingCommand
{
  uint8_t cmd, arg0, arg1, arg2;
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static const float powersOf10[] = {1e-3f, 1e-2f, 1e-1f, 1.0f, 1e1f, 1e2f, 1e3f, 1e4f}; // 10^(dec) for dec -3..4

static bool encodeValue(float value, int16_t &mantissa, uint8_t &nibble)
{
  for (int decimals = 4; decimals >= -3; decimals--)
  {
    const float scaled = value * powersOf10[decimals + 3];
    if (fabsf(scaled) <= 32767.0f)
    {
      mantissa = (int16_t)lroundf(scaled);
      nibble = (uint8_t)(decimals + 4);
      return true;
    }
  }
  return false;
}

static float decodeValue(int16_t mantissa, uint8_t nibble)
{
  const int decimals = (int)nibble - 4;
  if (decimals < -3 || decimals > 4)
  {
    return NAN;
  }
  return mantissa / powersOf10[decimals + 3];
}

// signals worth sending: not derived, not board-local
static bool isShareable(signalId_t id)
{
  switch (id)
  {
  case SIG_NONE:
  case SIG_BOOST_REL: // derived from SIG_BOOST on the receiver
  case SIG_OUTPUT:
  case SIG_OUT_FAULT:
  case SIG_BOARD_TEMP:
  case SIG_UPTIME:
  case SIG_DASH_LIGHTS: // shared as the heartbeat night bit instead ("follow night")
    return false;
  default: // a page's own source (SIG_USER_*) means something else on every gauge
    return !(id >= SIG_USER_1 && id < SIG_USER_1 + USER_SIGS_ALL);
  }
}

// rate class per signal: fast 20 Hz, slow 1 Hz, everything else 5 Hz
static uint16_t periodMs(signalId_t id)
{
  switch (id)
  {
  case SIG_RPM:
  case SIG_SPEED:
  case SIG_BOOST:
  case SIG_PEDAL:
  case SIG_THROTTLE:
  case SIG_LAMBDA:
  case SIG_TIMING:
  case SIG_GEAR:
  case SIG_SELECTOR:
  case SIG_TCU_SPEED:
  case SIG_INPUT_RPM:      // 0.26.0
  case SIG_CONVERTER_SLIP: // 0.26.0
  case SIG_HALDEX_TARGET:
  case SIG_HALDEX_ACTUAL:
  case SIG_ME7_1:
  case SIG_ME7_2:
  case SIG_ME7_3:
  case SIG_ME7_4:
  case SIG_ME7_5:
  case SIG_ME7_6:
  case SIG_ME7_7:
  case SIG_ME7_8:
    return 50;
  case SIG_COOLANT:
  case SIG_IAT:
  case SIG_OIL_TEMP:
  case SIG_FUEL_LEVEL:
  case SIG_BATTERY:
  case SIG_GEARBOX_TEMP:
  case SIG_AMBIENT:
    return 1000;
  default:
    return 200;
  }
}

static uint8_t mySources()
{
  uint8_t flags = 0;
  if (settings.elmEnabled)
  {
    flags |= LINK_SRC_ELM_CFG;
  }
  if (elmConnected())
  {
    flags |= LINK_SRC_ELM_UP;
  }
  if (runtime.klineUp)
  {
    flags |= LINK_SRC_KLINE;
  }
  if (settings.canEnabled && (settings.canProfile == CAN_PROFILE_OBD2 || settings.obdOverCan))
  {
    flags |= LINK_SRC_OBD;
  }
  if (dsgPolling() || vdiagPolling()) // gearbox reads or VAG diag values: both are diagnostic requests on the bus
  {
    flags |= LINK_SRC_DSG;
  }
  if (settings.chainRole == CHAIN_MASTER)
  {
    flags |= LINK_SRC_FORCED;
  }
  if (settings.chainRole == CHAIN_FOLLOWER)
  {
    flags |= LINK_SRC_FOLLOWER;
  }
  if (settings.shutdownSprite[0])
  {
    flags |= LINK_SRC_SHUTDOWN; // 0.29.0: it can join the row wave's Play It Now
  }
  return flags;
}

static bool isMasterCandidate(uint8_t flags)
{
  if (flags & LINK_SRC_FOLLOWER)
  {
    return false;
  }
  return (flags & (LINK_SRC_FORCED | LINK_SRC_ELM_CFG | LINK_SRC_OBD | LINK_SRC_DSG)) != 0;
}

static bool isAlive(const LinkNode &node, uint32_t now, uint32_t timeoutMs = 3000)
{
  return node.lastSeen && now - node.lastSeen < timeoutMs;
}

static void noteNode(uint8_t node, uint8_t via)
{
  // caller holds nodeMux
  LinkNode &entry = nodes[node & 0x0F];
  entry.node = node & 0x0F;
  entry.lastSeen = millis() | 1;
  entry.via |= via;
}

static void noteCollision()
{
  stats.collisions++;
  if (settings.nodeId == 0xFF && ++collisionHits >= 2)
  {
    // another gauge uses our id: move on (a random step so two clashing gauges don't keep following each other)
    myNode = (myNode + 1 + (esp_random() % 3)) & 0x0F;
    collisionHits = 0;
    DEBUG("LINK: node id clash - now G%u", myNode);
  }
}

// ---------------------------------------------------------------------------
// ESP-NOW
// ---------------------------------------------------------------------------
static void commandHmac(const uint8_t *data, size_t len, uint8_t out[8])
{
  uint8_t fullHash[32];
  const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(mdInfo, (const uint8_t *)CHAIN_KEY, strlen(CHAIN_KEY), data, len, fullHash); // 0.28.3: the built-in key
  memcpy(out, fullHash, 8);
}

static void fillHeader(NowHeader &header, uint8_t type)
{
  header.magic[0] = 'C';
  header.magic[1] = '2';
  header.type = type;
  header.group = settings.chainGroup;
  header.node = myNode;
  header.proto = LINK_PROTO_VERSION;
  header.seq = espNowSeq++;
}

static bool espNowSend(const void *data, size_t len)
{
  if (!stats.espNowUp || !espNowMutex)
  {
    return false;
  }
  static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  bool ok = false;
  if (xSemaphoreTake(espNowMutex, pdMS_TO_TICKS(5)) == pdTRUE)
  {
    ok = stats.espNowUp && esp_now_send(broadcastMac, (const uint8_t *)data, len) == ESP_OK;
    xSemaphoreGive(espNowMutex);
  }
  if (ok)
  {
    stats.txNow++;
  }
  return ok;
}

// runs in the WiFi task - keep it short, no LVGL
static void espNowReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  if (len >= 4 && data[0] == OHX_MAGIC0 && data[1] == OHX_MAGIC1) // OpenHaldex (include/ohx_proto.h)
  {
    ohxOnEspNow(info ? info->src_addr : NULL, info && info->rx_ctrl ? info->rx_ctrl->rssi : 0, data, len);
    return;
  }
  if (len >= 8 && data[0] == CANN_MAGIC0 && data[1] == CANN_MAGIC1) // relayed CAN frames (include/cannow_proto.h)
  {
    canRelayOnEspNow(info ? info->src_addr : NULL, info && info->rx_ctrl ? info->rx_ctrl->rssi : 0, data, len);
    return;
  }
  if (len >= 8 && data[0] == ADCN_MAGIC0 && data[1] == ADCN_MAGIC1) // adc2can (include/adcnow_proto.h)
  {
    adc2canOnEspNow(info ? info->src_addr : NULL, info && info->rx_ctrl ? info->rx_ctrl->rssi : 0, data, len);
    return;
  }
  if (len >= 4 && data[0] == ECUN_MAGIC0 && data[1] == ECUN_MAGIC1) // usb2ip: an Ignitron / MaxxECU's data
  {
    const uint8_t *senderMac = NULL; // who sent it (NULL if the radio didn't say)
    int8_t senderRssi = 0;           // its signal strength, dBm
    if (info != NULL)
    {
      esp_now_recv_info_t receiveInfo;
      memcpy(&receiveInfo, info, sizeof(receiveInfo)); // a copy to read with dots
      senderMac = receiveInfo.src_addr;
      if (receiveInfo.rx_ctrl != NULL)
      {
        wifi_pkt_rx_ctrl_t radioInfo;
        memcpy(&radioInfo, receiveInfo.rx_ctrl, sizeof(radioInfo)); // the radio's details of this packet
        senderRssi = radioInfo.rssi;
      }
    }
    ecuNowOnEspNow(senderMac, senderRssi, data, len);
    return;
  }
  if (len >= 4 && data[0] == 'C' && data[1] == 'U') // 0.25.0: a chain update (src/can2gauge_ChainUpdate.cpp)
  {
    if (info != NULL)
    {
      esp_now_recv_info_t receiveInfo;
      memcpy(&receiveInfo, info, sizeof(receiveInfo)); // a copy to read with dots
      chainUpdateOnEspNow(receiveInfo.src_addr, data, len);
    }
    return;
  }

  if (!settings.chainEnabled || len < (int)sizeof(NowHeader))
  {
    return;
  }
  const NowHeader *header = (const NowHeader *)data;
  if (header->magic[0] != 'C' || header->magic[1] != '2' || header->group != settings.chainGroup)
  {
    return;
  }
  stats.rxNow++;
  const uint8_t node = header->node & 0x0F;
  if (node == myNode)
  {
    noteCollision(); // we never hear our own broadcast
    return;
  }

  if (header->type == NOW_HEARTBEAT && len >= (int)NOW_HEARTBEAT_OLD_LEN)
  {
    // 0.25.0: copied into a full-size heartbeat with the new fields zeroed, so a shorter one from an older gauge
    // reads as "version unknown, can't take a chain update"
    NowHeartbeat fullHeartbeat;
    memset(&fullHeartbeat, 0, sizeof(fullHeartbeat));
    memcpy(&fullHeartbeat, data, min((size_t)len, sizeof(fullHeartbeat)));
    const NowHeartbeat *heartbeat = &fullHeartbeat;
    portENTER_CRITICAL(&nodeMux);
    noteNode(node, 2);
    LinkNode &entry = nodes[node];
    entry.role = heartbeat->role & 0x03;
    entry.night = (heartbeat->role & 0x80) != 0;
    entry.rowPosition = (heartbeat->role >> 2) & 0x0F; // bits 2-5: its place in the row wave
    entry.ignitionOn = (heartbeat->role & 0x40) != 0;  // bit 6: its own ignition is on
    entry.proto = header->proto;
    entry.fwMajor = heartbeat->fw >> 4;
    entry.fwMinor = heartbeat->fw & 0x0F;
    entry.flags = heartbeat->sources;
    entry.owned = heartbeat->owned;
    entry.uptimeS = heartbeat->uptimeS;
    if (info && info->rx_ctrl)
    {
      entry.rssi = info->rx_ctrl->rssi;
    }
    if (info)
    {
      memcpy(entry.mac, info->src_addr, 6);
    }
    memcpy(entry.name, heartbeat->name, NAME_LEN);
    entry.name[NAME_LEN - 1] = '\0';
    memcpy(entry.version, fullHeartbeat.version, 3);
    entry.board = fullHeartbeat.board;
    entry.updateProto = fullHeartbeat.updateProto;
    entry.ignKind = fullHeartbeat.ignKind;         // 0.30.4: its ignition source + 1
    entry.sharedRev = fullHeartbeat.sharedRev;     // 0.30.4: its shared settings' revision ...
    entry.sharedCheck = fullHeartbeat.sharedCheck; // ... and check value
    portEXIT_CRITICAL(&nodeMux);
    return;
  }
  if (header->proto != LINK_PROTO_VERSION)
  {
    stats.rejected++; // listed from its heartbeat, but its data / commands are ignored
    return;
  }

  if (header->type == NOW_DATA && len >= (int)sizeof(NowHeader) + 1)
  {
    const uint8_t count = data[sizeof(NowHeader)];
    if (len < (int)(sizeof(NowHeader) + 1 + count * sizeof(NowSignal)))
    {
      return;
    }
    const NowSignal *signals = (const NowSignal *)(data + sizeof(NowHeader) + 1);
    for (uint8_t i = 0; i < count; i++)
    {
      if (signals[i].sig > SIG_NONE && signals[i].sig < SIG_COUNT && !isnan(signals[i].value))
      {
        signalSetRemote((signalId_t)signals[i].sig, signals[i].value, node);
      }
    }
    portENTER_CRITICAL(&nodeMux);
    noteNode(node, 2);
    portEXIT_CRITICAL(&nodeMux);
    return;
  }

  if (header->type == NOW_COMMAND && len >= (int)sizeof(NowCommand))
  {
    const NowCommand *command = (const NowCommand *)data;
    uint8_t expectedHmac[8];
    commandHmac(data, offsetof(NowCommand, hmac), expectedHmac);
    if (memcmp(expectedHmac, command->hmac, 8) != 0)
    {
      stats.rejected++;
      return;
    }
    if (command->dst != 0xFF && command->dst != myNode)
    {
      return;
    }
    if (lastCommandSeq[node] == command->cseq)
    {
      return; // repeat
    }
    lastCommandSeq[node] = command->cseq;
    const PendingCommand pending = {command->cmd, command->arg0, command->arg1, command->arg2};
    if (commandQueue)
    {
      xQueueSend(commandQueue, &pending, 0);
    }
  }

  // 0.30.4: another gauge's shared settings
  if (header->type == NOW_SHARED && len >= (int)sizeof(NowShared))
  {
    NowShared packet;
    memcpy(&packet, data, sizeof(packet)); // a copy, read with dots
    uint8_t expectedHmac[8];
    commandHmac(data, offsetof(NowShared, hmac), expectedHmac);
    if (memcmp(expectedHmac, packet.hmac, 8) != 0)
    {
      stats.rejected++;
      return;
    }
    bool takeIt = false;
    if (packet.rev > sharedSettingsRev)
    {
      takeIt = true; // changed more recently than ours
    }
    else if (packet.rev == sharedSettingsRev && packet.fromMaster == 1 && !iAmMaster &&
             sharedCheck(packet.block) != sharedMyCheck())
    {
      takeIt = true; // same revision but different: the master's win
    }
    if (takeIt)
    {
      sharedOffer(packet.block, packet.rev); // the house task takes it on
    }
  }
}

static void espNowStart()
{
  if (stats.espNowUp || !wifiReady || WiFi.getMode() == WIFI_OFF)
  {
    return;
  }
  xSemaphoreTake(espNowMutex, portMAX_DELAY);
  if (esp_now_init() == ESP_OK)
  {
    esp_now_register_recv_cb(espNowReceive);
    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 0; // the current WiFi channel (the AP's, or the STA bridge's once it joins)
    peer.ifidx = WIFI_IF_AP;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) == ESP_OK)
    {
      stats.espNowUp = true;
    }
    else
    {
      esp_now_deinit();
    }
  }
  xSemaphoreGive(espNowMutex);
  DEBUG("LINK: ESP-NOW %s", stats.espNowUp ? "up" : "FAILED");
}

static void espNowStop()
{
  if (!espNowMutex)
  {
    return;
  }
  xSemaphoreTake(espNowMutex, portMAX_DELAY);
  if (stats.espNowUp)
  {
    stats.espNowUp = false;
    esp_now_unregister_recv_cb();
    esp_now_deinit();
  }
  xSemaphoreGive(espNowMutex);
}

// unicast (OpenHaldex commands): ESP-NOW acks and retries it at the MAC layer, unlike the broadcasts
bool linkEspNowSendTo(const uint8_t mac[6], const void *data, size_t len)
{
  if (!stats.espNowUp || !espNowMutex)
  {
    return false;
  }
  bool ok = false;
  if (xSemaphoreTake(espNowMutex, pdMS_TO_TICKS(20)) == pdTRUE)
  {
    if (stats.espNowUp && !esp_now_is_peer_exist(mac))
    {
      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, mac, 6);
      peer.channel = 0; // the current WiFi channel
      peer.ifidx = WIFI_IF_AP;
      peer.encrypt = false;
      esp_now_add_peer(&peer);
    }
    ok = stats.espNowUp && esp_now_send(mac, (const uint8_t *)data, len) == ESP_OK;
    xSemaphoreGive(espNowMutex);
  }
  if (ok)
  {
    stats.txNow++;
  }
  return ok;
}

void linkWifiUp()
{
  wifiReady = true;
} // the link task starts ESP-NOW when the chain (or OpenHaldex) wants it

void linkWifiDown()
{
  wifiReady = false;
  espNowStop();
}

// ---------------------------------------------------------------------------
// CAN
// ---------------------------------------------------------------------------
static bool canTxAllowed()
{
  return settings.linkCanTx && canStats().up;
}

static bool useCanForData()
{
  if (!canTxAllowed())
  {
    return false;
  }
  switch (settings.linkTransport)
  {
  case LINK_CAN:
  case LINK_BOTH:
    return true;
  case LINK_AUTO:
    return stats.canHeard || !stats.espNowUp;
  default:
    return false;
  }
}

static bool useEspNowForData()
{
  if (!stats.espNowUp)
  {
    return false;
  }
  switch (settings.linkTransport)
  {
  case LINK_ESPNOW:
  case LINK_BOTH:
    return true;
  case LINK_AUTO:
    return !useCanForData();
  default:
    return false;
  }
}

// runs in the CAN RX task - short
static void canReceive(const twai_message_t &message)
{
  if (!settings.chainEnabled || message.extd)
  {
    return;
  }
  const uint16_t base = settings.linkCanBaseId;
  if (message.identifier < base || message.identifier > (uint32_t)base + 0x20)
  {
    return;
  }
  const uint8_t *data = message.data;
  stats.rxCan++;

  if (message.identifier < base + 0x10u) // DATA
  {
    const uint8_t node = message.identifier - base;
    if (node == myNode)
    {
      noteCollision();
      return;
    }
    if (message.data_length_code < 8)
    {
      return;
    }
    for (int slot = 0; slot < 2; slot++)
    {
      const uint8_t sig = data[slot * 3];
      const int16_t mantissa = (int16_t)(data[slot * 3 + 1] | (data[slot * 3 + 2] << 8));
      const uint8_t nibble = slot == 0 ? data[7] >> 4 : data[7] & 0x0F;
      const float value = decodeValue(mantissa, nibble);
      if (sig > SIG_NONE && sig < SIG_COUNT && !isnan(value))
      {
        signalSetRemote((signalId_t)sig, value, node);
      }
    }
    portENTER_CRITICAL(&nodeMux);
    noteNode(node, 1);
    portEXIT_CRITICAL(&nodeMux);
  }
  else if (message.identifier < base + 0x20u) // HEARTBEAT
  {
    const uint8_t node = message.identifier - base - 0x10;
    if (message.data_length_code < 8 || (data[0] | (data[1] << 8)) != settings.chainGroup)
    {
      return;
    }
    if (node == myNode)
    {
      noteCollision();
      return;
    }
    canHeardMs = millis() | 1;
    portENTER_CRITICAL(&nodeMux);
    noteNode(node, 1);
    LinkNode &entry = nodes[node];
    entry.role = data[2] & 0x03;
    entry.night = (data[2] & 0x80) != 0;
    entry.rowPosition = (data[2] >> 2) & 0x0F; // bits 2-5: its place in the row wave
    entry.ignitionOn = (data[2] & 0x40) != 0;  // bit 6: its own ignition is on
    entry.proto = data[3];
    entry.fwMajor = data[4] >> 4;
    entry.fwMinor = data[4] & 0x0F;
    entry.flags = data[5];
    entry.owned = data[6];
    entry.uptimeS = data[7] * 10u;
    portEXIT_CRITICAL(&nodeMux);
  }
  else // COMMAND
  {
    if (message.data_length_code < 8)
    {
      return;
    }
    const uint8_t dst = data[0];
    const uint8_t src = data[1] & 0x0F;
    if (src == myNode || (dst != 0xFF && dst != myNode) || lastCommandSeq[src] == data[6])
    {
      return;
    }
    lastCommandSeq[src] = data[6];
    const PendingCommand pending = {data[2], data[3], data[4], data[5]};
    if (commandQueue)
    {
      xQueueSend(commandQueue, &pending, 0);
    }
  }
}

static void sendCommand(uint8_t dst, uint8_t cmd, uint8_t arg0, uint8_t arg1 = 0, uint8_t arg2 = 0)
{
  const uint8_t seq = ++commandSeq;
  for (int repeat = 0; repeat < 3; repeat++)
  {
    if (canTxAllowed())
    {
      const uint8_t frame[8] = {dst, myNode, cmd, arg0, arg1, arg2, seq, 0};
      if (canSend(settings.linkCanBaseId + 0x20, frame, 8))
      {
        stats.txCan++;
      }
    }
    if (stats.espNowUp)
    {
      NowCommand command;
      fillHeader(command.header, NOW_COMMAND);
      command.dst = dst;
      command.cmd = cmd;
      command.arg0 = arg0;
      command.arg1 = arg1;
      command.arg2 = arg2;
      command.cseq = seq;
      commandHmac((const uint8_t *)&command, offsetof(NowCommand, hmac), command.hmac);
      espNowSend(&command, sizeof(command));
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------------------------------------------------------------------
// periodic work
// ---------------------------------------------------------------------------
static void electMaster(uint32_t now)
{
  const uint8_t myFlags = mySources();
  int8_t bestNode = isMasterCandidate(myFlags) ? myNode : -1;
  bool bestForced = (myFlags & LINK_SRC_FORCED) != 0;
  int8_t lowest = (myFlags & LINK_SRC_FOLLOWER) ? -1 : myNode; // fallback when nobody is a candidate
  bool peerMasterAlive = false;

  portENTER_CRITICAL(&nodeMux);
  for (uint8_t nodeId = 0; nodeId < MAX_NODES; nodeId++)
  {
    const LinkNode &node = nodes[nodeId];
    if (nodeId == myNode || !isAlive(node, now) || node.proto != LINK_PROTO_VERSION)
    {
      continue;
    }
    if (node.role == 1)
    {
      peerMasterAlive = true;
    }
    if (!(node.flags & LINK_SRC_FOLLOWER) && (lowest < 0 || nodeId < lowest))
    {
      lowest = nodeId;
    }
    if (!isMasterCandidate(node.flags))
    {
      continue;
    }
    const bool forced = (node.flags & LINK_SRC_FORCED) != 0;
    if (bestNode < 0 || (forced && !bestForced) || (forced == bestForced && nodeId < bestNode))
    {
      bestNode = nodeId;
      bestForced = forced;
    }
  }
  portEXIT_CRITICAL(&nodeMux);

  const int8_t winner = bestNode >= 0 ? bestNode : lowest;
  bool master = winner == myNode;
  if (peerMasterAlive)
  {
    masterSeenEver = true;
  }
  // failover off: once another master has been seen, an Auto gauge never promotes itself
  if (master && !settings.failover && masterSeenEver && settings.chainRole == CHAIN_AUTO && !peerMasterAlive &&
      !(myFlags & LINK_SRC_FORCED) && !iAmMaster)
  {
    master = false;
  }
  if (master != iAmMaster)
  {
    DEBUG("LINK: G%u is now %s", myNode, master ? "MASTER" : "follower");
  }
  iAmMaster = master;
  masterNode = winner;
}

static void sendHeartbeat(uint32_t now, uint8_t owned)
{
  const bool night = nightActive();
  uint8_t role = 2; // bits 0-1: 2 = follower
  if (iAmMaster)
  {
    role = 1; // 1 = master
  }
  role = role | ((settings.rowPosition & 0x0F) << 2); // bits 2-5: our place in the row wave (0 = not in a row)
  if (night)
  {
    role = role | 0x80; // bit 7: night (the dash lights are on)
  }
  // bit 6: this gauge has its own ignition (terminal 15 on CAN, or the wire) and it is on, so the gauges set to
  // "Ignition from another gauge" stay awake. A gauge on switched power, or one following the chain itself, never
  // sets it (two followers would otherwise keep each other awake for ever).
  if ((settings.ignSource == IGN_SRC_CAN || settings.ignSource == IGN_SRC_WIRE) && ignitionOn())
  {
    role = role | 0x40;
  }
  const uint8_t sources = mySources();
  if (canTxAllowed() && settings.linkTransport != LINK_ESPNOW && settings.linkTransport != LINK_OFF)
  {
    const uint8_t frame[8] = {(uint8_t)(settings.chainGroup & 0xFF), (uint8_t)(settings.chainGroup >> 8), role,
                              LINK_PROTO_VERSION, fwByte, sources, owned, (uint8_t)((now / 10000) & 0xFF)};
    if (canSend(settings.linkCanBaseId + 0x10 + myNode, frame, 8))
    {
      stats.txCan++;
    }
  }
  if (stats.espNowUp && settings.linkTransport != LINK_CAN && settings.linkTransport != LINK_OFF)
  {
    NowHeartbeat heartbeat;
    fillHeader(heartbeat.header, NOW_HEARTBEAT);
    heartbeat.role = role;
    heartbeat.fw = fwByte;
    heartbeat.sources = sources;
    heartbeat.owned = owned;
    heartbeat.uptimeS = now / 1000;
    memset(heartbeat.name, 0, sizeof(heartbeat.name));
    copyStr(heartbeat.name, NAME_LEN, settings.nodeName);
    chainUpdateVersion(heartbeat.version);           // 0.25.0: the full version ...
    heartbeat.board = chainUpdateBoardId();          // ... which board ...
    heartbeat.updateProto = CHAIN_UPDATE_PROTO;      // ... and that it can take a chain update
    heartbeat.ignKind = settings.ignSource + 1;      // 0.30.4: its ignition source (+ 1, so 0 = an older gauge)
    heartbeat.sharedRev = sharedSettingsRev;         // 0.30.4: its shared settings' revision ...
    heartbeat.sharedCheck = sharedMyCheck();         // ... and check value
    espNowSend(&heartbeat, sizeof(heartbeat));
  }
}

// which of our values go out: private local sources always; values heard on the car bus only over ESP-NOW (a wired
// follower hears them itself) and only from the master (so N gauges don't all repeat the same frames)
static bool wantShare(signalId_t id, bool overCan)
{
  if (!isShareable(id) || !signalValid(id))
  {
    return false;
  }
  const signalOrigin_t origin = signalOrigin(id);
  if (origin == SIG_ORIGIN_LOCAL)
  {
    return true;
  }
  return origin == SIG_ORIGIN_BUS && !overCan && iAmMaster;
}

static uint8_t sendData(uint32_t now)
{
  const bool viaCan = useCanForData();
  const bool viaEspNow = useEspNowForData();
  uint8_t owned = 0;
  // CAN: two due signals per frame
  uint8_t pending = 0;
  uint8_t pendingSig = 0;
  int16_t pendingMantissa = 0;
  uint8_t pendingNibble = 0;
  uint8_t frames = 0;
  // ESP-NOW: everything we share, one packet per 50 ms
  static uint32_t lastEspNowMs = 0;
  const bool espNowDue = viaEspNow && now - lastEspNowMs >= 50;
  uint8_t packet[250];
  NowHeader *header = (NowHeader *)packet;
  uint8_t &count = packet[sizeof(NowHeader)];
  NowSignal *signals = (NowSignal *)(packet + sizeof(NowHeader) + 1);
  count = 0;

  for (uint8_t sigIndex = 1; sigIndex < SIG_COUNT; sigIndex++)
  {
    const signalId_t id = (signalId_t)sigIndex;
    if (wantShare(id, false))
    {
      owned++;
    }
    if (espNowDue && count < NOW_MAX_SIGS && wantShare(id, false))
    {
      signals[count].sig = sigIndex;
      signals[count].flags = 0;
      signals[count].value = signalGet(id);
      count++;
    }
    if (!viaCan || frames >= 6 || !wantShare(id, true) || now - lastSent[sigIndex] < periodMs(id))
    {
      continue;
    }
    int16_t mantissa;
    uint8_t nibble;
    if (!encodeValue(signalGet(id), mantissa, nibble))
    {
      continue;
    }
    lastSent[sigIndex] = now;
    if (!pending)
    {
      pending = 1;
      pendingSig = sigIndex;
      pendingMantissa = mantissa;
      pendingNibble = nibble;
      continue;
    }
    const uint8_t frame[8] = {pendingSig, (uint8_t)(pendingMantissa & 0xFF), (uint8_t)((uint16_t)pendingMantissa >> 8),
                              sigIndex, (uint8_t)(mantissa & 0xFF), (uint8_t)((uint16_t)mantissa >> 8), dataSeq++,
                              (uint8_t)((pendingNibble << 4) | nibble)};
    if (canSend(settings.linkCanBaseId + myNode, frame, 8))
    {
      stats.txCan++;
    }
    frames++;
    pending = 0;
  }

  if (pending)
  {
    const uint8_t frame[8] = {pendingSig, (uint8_t)(pendingMantissa & 0xFF), (uint8_t)((uint16_t)pendingMantissa >> 8), 0, 0, 0,
                              dataSeq++, (uint8_t)(pendingNibble << 4)};
    if (canSend(settings.linkCanBaseId + myNode, frame, 8))
    {
      stats.txCan++;
    }
  }
  if (espNowDue)
  {
    lastEspNowMs = now;
    if (count)
    {
      fillHeader(*header, NOW_DATA);
      espNowSend(packet, sizeof(NowHeader) + 1 + count * sizeof(NowSignal));
    }
  }
  return owned;
}

// 0.29.0: this gauge's number (its place in the row) big on the glass, with its name and role underneath
static void showOwnNumber(uint32_t showMs)
{
  char number[4];
  if (settings.rowPosition == 0)
  {
    copyStr(number, sizeof(number), "?"); // not given a place in the row yet
  }
  else
  {
    snprintf(number, sizeof(number), "%u", settings.rowPosition);
  }
  // the role as set (Master / Follower), or what Auto has made it right now
  const char *roleText = "Auto";
  if (settings.chainRole == CHAIN_MASTER)
  {
    roleText = "Master";
  }
  else if (settings.chainRole == CHAIN_FOLLOWER)
  {
    roleText = "Follower";
  }
  else if (iAmMaster)
  {
    roleText = "Auto (Master now)";
  }
  else
  {
    roleText = "Auto (Follower now)";
  }
  char detail[64]; // two lines under the number: which gauge (or "not in the row"), then its role
  if (settings.rowPosition == 0)
  {
    snprintf(detail, sizeof(detail), "Not in the row yet\n%s", roleText);
  }
  else if (settings.nodeName[0])
  {
    snprintf(detail, sizeof(detail), "%s\n%s", settings.nodeName, roleText);
  }
  else
  {
    snprintf(detail, sizeof(detail), "G%u\n%s", myNode, roleText);
  }
  uiShowNumber(number, detail, showMs);
}

// 0.29.0: a new place in the row for this gauge (from the web UI on this gauge or another one)
static void applyRow(uint8_t rowPosition)
{
  if (rowPosition > ROW_MAX_POSITION)
  {
    return; // out of range: ignore it
  }
  if (settings.rowPosition != rowPosition)
  {
    settings.rowPosition = rowPosition;
    settingsDirty = true; // the EEP task saves it
  }
  showOwnNumber(CHANGED_NUMBER_MS); // the new number on the glass, so you can see which gauge took it
}

// 0.29.0: a new role for this gauge (Auto / Master / Follower)
static void applyRole(uint8_t role)
{
  if (role >= chainRole_t_MAX)
  {
    return; // not a role this firmware knows: ignore it
  }
  if (settings.chainRole != role)
  {
    settings.chainRole = role;
    settingsDirty = true; // the EEP task saves it; the next election (within 0.1 s) uses it
  }
  showOwnNumber(CHANGED_NUMBER_MS);
}

static void runCommand(const PendingCommand &command)
{
  switch (command.cmd)
  {
  case CMD_HALDEX_MODE:
    if (iAmMaster)
    {
      canHaldexSetMode(command.arg0);
    }
    break;
  case CMD_GOTO_PAGE:
    if (!iAmMaster && settings.followPage && command.arg0 != uiActivePage())
    {
      uiGotoPage(command.arg0);
    }
    break;
  case CMD_CLEAR_ALERTS:
    uiClearAlert();
    break;
  case CMD_ROW_TEST:
    // another gauge in the row pressed "Play it now": join in, if this gauge is in the row too
    if (settings.rowPosition != 0)
    {
      powerTestShutdown();
    }
    break;
  case CMD_SET_ROW: // 0.29.0: another gauge's web UI gave this gauge a new place in the row
    applyRow(command.arg0);
    break;
  case CMD_SET_ROLE: // ... or a new role
    applyRole(command.arg0);
    break;
  case CMD_SHOW_NUMBER: // ... or pressed Show Numbers / Hide Numbers
    if (command.arg0)
    {
      showOwnNumber(SHOW_NUMBER_MS);
    }
    else
    {
      uiShowNumber(NULL, NULL, 0);
    }
    break;
  default:
    break;
  }
}

// 0.30.4: send this gauge's shared settings to the others (signed)
static uint32_t lastSharedSendMs = 0; // millis() they last went out
static void sendShared(const SharedBlock &block)
{
  NowShared packet;
  memset(&packet, 0, sizeof(packet));
  fillHeader(packet.header, NOW_SHARED);
  packet.rev = sharedSettingsRev;
  packet.fromMaster = 0;
  if (iAmMaster)
  {
    packet.fromMaster = 1;
  }
  memcpy(&packet.block, &block, sizeof(block));
  commandHmac((const uint8_t *)&packet, offsetof(NowShared, hmac), packet.hmac);
  espNowSend(&packet, sizeof(packet));
  lastSharedSendMs = millis() | 1;
}

// 0.30.4, once a second: a shared setting changed here goes to every gauge at once; a gauge that's behind (or, for the
// master, one that differs) is sent ours again, at most every 3 s
static void sharedStep(uint32_t now)
{
  if (!stats.espNowUp || sharedPending())
  {
    return; // no WiFi link, or the house task is still taking on a block from another gauge
  }
  const uint32_t myCheck = sharedMyCheck();
  uint32_t highestRev = 0;    // the highest revision heard
  bool isSomeoneBehind = false; // a gauge with an older revision than ours
  bool doesSomeoneDiffer = false; // a gauge with our revision but different settings
  uint8_t ownIgnitionMac[6] = {0, 0, 0, 0, 0, 0}; // 0.30.10: a gauge heard with its own ignition (all zero = none)
  bool hasOwnIgnitionGauge = false;
  portENTER_CRITICAL(&nodeMux);
  for (uint8_t nodeId = 0; nodeId < MAX_NODES; nodeId++)
  {
    const LinkNode &node = nodes[nodeId];
    if (nodeId == myNode || !isAlive(node, now, 5000) || node.ignKind == 0)
    {
      continue; // us, gone, or an older gauge that doesn't share settings
    }
    if (node.sharedRev > highestRev)
    {
      highestRev = node.sharedRev;
    }
    if (node.sharedRev < sharedSettingsRev)
    {
      isSomeoneBehind = true;
    }
    else if (node.sharedRev == sharedSettingsRev && node.sharedCheck != myCheck)
    {
      doesSomeoneDiffer = true;
    }
    const bool hasOwnIgnition = node.ignKind == IGN_SRC_CAN + 1 || node.ignKind == IGN_SRC_WIRE + 1;
    if (hasOwnIgnition && !hasOwnIgnitionGauge)
    {
      memcpy(ownIgnitionMac, node.mac, 6);
      hasOwnIgnitionGauge = true;
    }
  }
  portEXIT_CRITICAL(&nodeMux);

  if (!sharedLock())
  {
    return;
  }
  // 0.30.10: "Another gauge in the chain" picked by hand on the web UI, before this gauge knew which gauge has the
  // ignition: follow the one heard with its own (so the change sent below names it, not "none")
  if (settings.ignSource == IGN_SRC_CHAIN && hasOwnIgnitionGauge)
  {
    bool isFollowingNobody = true;
    for (uint8_t i = 0; i < 6; i++)
    {
      if (ignitionGaugeMac[i] != 0)
      {
        isFollowingNobody = false;
      }
    }
    if (isFollowingNobody)
    {
      memcpy(ignitionGaugeMac, ownIgnitionMac, 6);
    }
  }
  const bool changedHere = sharedNoteLocalChange(highestRev); // moves our revision on if so
  SharedBlock mine;
  sharedBuild(mine);
  sharedUnlock();

  bool shouldSend = changedHere;
  if (now - lastSharedSendMs >= 3000)
  {
    if (isSomeoneBehind)
    {
      shouldSend = true;
    }
    else if (doesSomeoneDiffer && iAmMaster)
    {
      shouldSend = true;
    }
  }
  if (shouldSend)
  {
    sendShared(mine);
  }
}

static void linkTask(void *)
{
  uint32_t lastHeartbeat = 0;
  uint32_t lastElect = 0;
  uint8_t owned = 0;
  for (;;)
  {
    const uint32_t now = millis();
    if (!settings.chainEnabled)
    {
      iAmMaster = true;
      masterNode = -1;
      if (ohxWantsEspNow() || settings.adcWireless || canRelayWantsEspNow() || ecuNowWantsEspNow()) // OpenHaldex / adc2can / CAN relay / usb2ip over WiFi keep ESP-NOW up without the chain
      {
        if (!stats.espNowUp && wifiReady)
        {
          espNowStart();
        }
      }
      else if (stats.espNowUp)
      {
        espNowStop();
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if ((ohxWantsEspNow() || settings.adcWireless || canRelayWantsEspNow() || ecuNowWantsEspNow() ||
         (settings.linkTransport != LINK_CAN && settings.linkTransport != LINK_OFF)) &&
        !stats.espNowUp && wifiReady)
    {
      espNowStart();
    }
    stats.canHeard = canHeardMs && now - canHeardMs < 5000;

    PendingCommand command;
    while (xQueueReceive(commandQueue, &command, 0) == pdTRUE)
    {
      runCommand(command);
    }
    if (pendingPage >= 0)
    {
      const uint8_t page = (uint8_t)pendingPage;
      pendingPage = -1;
      if (iAmMaster)
      {
        sendCommand(0xFF, CMD_GOTO_PAGE, page);
      }
    }
    if (pendingHaldex >= 0)
    {
      const uint8_t mode = (uint8_t)pendingHaldex;
      pendingHaldex = -1;
      if (iAmMaster)
      {
        canHaldexSetMode(mode);
      }
      else
      {
        sendCommand(masterNode >= 0 ? masterNode : 0xFF, CMD_HALDEX_MODE, mode);
      }
    }
    OutgoingCommand outgoing;
    while (xQueueReceive(sendQueue, &outgoing, 0) == pdTRUE) // 0.29.0: commands from the web UI for other gauges
    {
      sendCommand(outgoing.dst, outgoing.cmd, outgoing.arg0);
    }
    if (pendingRowTest)
    {
      pendingRowTest = false;
      sendCommand(0xFF, CMD_ROW_TEST, 0); // the other gauges start on the first of the 3 copies
      powerTestShutdown();                // and this one straight after, so they all start together
    }

    if (now - lastElect >= 100)
    {
      lastElect = now;
      electMaster(now);
    }
    if (settings.linkTransport != LINK_OFF)
    {
      owned = sendData(now);
    }
    if (now - lastHeartbeat >= 1000)
    {
      lastHeartbeat = now;
      sendHeartbeat(now, owned);
      sharedStep(now); // 0.30.4: keep the shared settings in step (0.30.10: which gauge has the ignition too)
    }
    vTaskDelay(pdMS_TO_TICKS(linkRefresh));
  }
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------
static uint8_t autoNodeId()
{
  const uint64_t mac = ESP.getEfuseMac();
  return (uint8_t)((mac >> 40) & 0x0F); // low nibble of the last octet
}

void linkRestart()
{
  myNode = settings.nodeId <= 15 ? settings.nodeId : autoNodeId();
  collisionHits = 0;
  portENTER_CRITICAL(&nodeMux);
  memset(nodes, 0, sizeof(nodes));
  portEXIT_CRITICAL(&nodeMux);
  memset(lastCommandSeq, 0xFF, sizeof(lastCommandSeq));
  masterSeenEver = false;
  if (stats.espNowUp)
  {
    espNowStop(); // the link task restarts it with the new settings
  }
}

void setupLink()
{
  unsigned versionMajor = 0;
  unsigned versionMinor = 0;
  sscanf(CAN2GAUGE_VERSION, "%u.%u", &versionMajor, &versionMinor);
  fwByte = (uint8_t)(((versionMajor & 0x0F) << 4) | (versionMinor & 0x0F));
  memset(&stats, 0, sizeof(stats));
  memset(lastSent, 0, sizeof(lastSent));
  espNowMutex = xSemaphoreCreateMutex();
  setupShared(); // 0.30.4: the shared settings' lock
  commandQueue = xQueueCreate(8, sizeof(PendingCommand));
  sendQueue = xQueueCreate(8, sizeof(OutgoingCommand)); // 0.29.0: up to 8 web UI commands waiting to go out
  // 0.29.0: start the command count somewhere random. It used to start at 1 after every boot, and the other gauges
  // throw away a command whose number matches the last one they had from this gauge (they take it for a repeat), so
  // the first command after this gauge rebooted (Play It Now ...) could be lost.
  commandSeq = (uint8_t)esp_random();
  linkRestart();
  canOnFrame(canReceive);
  xTaskCreatePinnedToCore(linkTask, "link", 4096, NULL, 5, NULL, 0);
}

bool linkChainActive()
{
  return settings.chainEnabled;
}

bool linkIsMaster()
{
  return !settings.chainEnabled || iAmMaster;
}

uint8_t linkNodeId()
{
  return myNode;
}

const LinkStats &linkStats()
{
  return stats;
}

const char *linkRoleName()
{
  if (!settings.chainEnabled)
  {
    return "standalone";
  }
  return iAmMaster ? "master" : "follower";
}

bool linkHaldexMode(uint8_t mode)
{
  if (linkIsMaster())
  {
    return canHaldexSetMode(mode);
  }
  pendingHaldex = mode; // the link task forwards it to the master (3 repeats)
  return true;
}

void linkPageChanged(uint8_t page)
{
  if (settings.chainEnabled && iAmMaster)
  {
    pendingPage = page;
  }
}

bool linkMasterNight(bool &night)
{
  if (!settings.chainEnabled || iAmMaster || masterNode < 0)
  {
    return false;
  }
  bool known = false;
  portENTER_CRITICAL(&nodeMux);
  const LinkNode &master = nodes[masterNode & 0x0F];
  if (isAlive(master, millis()))
  {
    night = master.night;
    known = true;
  }
  portEXIT_CRITICAL(&nodeMux);
  return known;
}

uint8_t linkNodes(LinkNode *out, uint8_t maxNodes)
{
  uint8_t count = 0;
  const uint32_t now = millis();
  portENTER_CRITICAL(&nodeMux);
  for (uint8_t nodeId = 0; nodeId < MAX_NODES && count < maxNodes; nodeId++)
  {
    if (nodeId != myNode && isAlive(nodes[nodeId], now, 10000))
    {
      out[count++] = nodes[nodeId];
    }
  }
  portEXIT_CRITICAL(&nodeMux);
  return count;
}

// the row wave: count the gauges in the row (ours plus every one heard in the last 3 s with a position set) and how
// many of them sit to our left. Every gauge works this out the same way, so they all agree on the order.
bool linkRowPlace(uint8_t &placeFromLeft, uint8_t &gaugesInRow)
{
  placeFromLeft = 0;
  gaugesInRow = 1; // this gauge
  if (!settings.chainEnabled || settings.rowPosition == 0)
  {
    return false;
  }
  const uint32_t now = millis();
  portENTER_CRITICAL(&nodeMux);
  for (uint8_t nodeId = 0; nodeId < MAX_NODES; nodeId++)
  {
    const LinkNode &node = nodes[nodeId];
    if (nodeId == myNode || node.rowPosition == 0 || !isAlive(node, now))
    {
      continue; // us, not in the row, or not heard lately
    }
    gaugesInRow++;
    bool isLeftOfUs = false;
    if (node.rowPosition < settings.rowPosition)
    {
      isLeftOfUs = true;
    }
    else if (node.rowPosition == settings.rowPosition && nodeId < myNode)
    {
      isLeftOfUs = true; // two gauges given the same position: the lower node id counts as the left one
    }
    if (isLeftOfUs)
    {
      placeFromLeft++;
    }
  }
  portEXIT_CRITICAL(&nodeMux);
  return gaugesInRow >= 2;
}

bool linkRowTest()
{
  uint8_t placeFromLeft = 0;
  uint8_t gaugesInRow = 0;
  if (!linkRowPlace(placeFromLeft, gaugesInRow))
  {
    return false;
  }
  pendingRowTest = true; // the link task sends the command, then plays it here
  return true;
}

// 0.29.0: is this heartbeat's firmware 0.29.0 or later? (older gauges never set the shutdown sprite bit)
static bool knowsSpriteBit(const LinkNode &node)
{
  const uint32_t version = node.version[0] * 10000u + node.version[1] * 100u + node.version[2];
  return version >= 2900; // 0.29.0
}

// 0.29.2: "Ignition from another gauge" - is a gauge with its own ignition on, heard in the last 3 s? A gauge that has
// gone to standby still sends heartbeats over WiFi, but with bit 6 clear; one in deep sleep sends nothing. Both count
// as off.
bool linkIgnitionFrom(char *whoName, size_t len)
{
  whoName[0] = '\0';
  if (!settings.chainEnabled)
  {
    return false;
  }
  int8_t foundNode = -1;    // the gauge whose ignition is on (-1 = none)
  char foundName[NAME_LEN]; // its name (copied out under the lock, formatted after)
  foundName[0] = '\0';
  const uint32_t now = millis();
  portENTER_CRITICAL(&nodeMux);
  for (uint8_t nodeId = 0; nodeId < MAX_NODES; nodeId++)
  {
    const LinkNode &node = nodes[nodeId];
    if (nodeId != myNode && isAlive(node, now) && node.ignitionOn)
    {
      foundNode = nodeId;
      memcpy(foundName, node.name, NAME_LEN);
      break;
    }
  }
  portEXIT_CRITICAL(&nodeMux);
  if (foundNode < 0)
  {
    return false;
  }
  if (foundName[0])
  {
    snprintf(whoName, len, "%s", foundName);
  }
  else
  {
    snprintf(whoName, len, "G%d", foundNode);
  }
  return true;
}

// 0.29.0: the web UI's message for Play It Now - which numbers join, and which can't (no shutdown sprite)
void linkRowTestText(char *text, size_t len)
{
  char joining[48] = "";  // "1, 3"
  char missing[48] = "";  // "2"
  size_t joiningUsed = 0; // characters written into each so far
  size_t missingUsed = 0;
  if (settings.rowPosition != 0)
  {
    appendf(joining, sizeof(joining), joiningUsed, "%u", settings.rowPosition); // this gauge (it needs a sprite to get here)
  }
  // copy what's needed out of the node table first (no text formatting while other tasks are held off)
  uint8_t rowNumbers[MAX_NODES]; // each other gauge in the row: its number ...
  bool hasSprite[MAX_NODES];     // ... and whether it can join
  uint8_t rowCount = 0;
  const uint32_t now = millis();
  portENTER_CRITICAL(&nodeMux);
  for (uint8_t nodeId = 0; nodeId < MAX_NODES; nodeId++)
  {
    const LinkNode &node = nodes[nodeId];
    if (nodeId == myNode || node.rowPosition == 0 || !isAlive(node, now))
    {
      continue; // us, not in the row, or not heard lately
    }
    rowNumbers[rowCount] = node.rowPosition;
    hasSprite[rowCount] = (node.flags & LINK_SRC_SHUTDOWN) != 0 || !knowsSpriteBit(node); // an older gauge: assume it can
    rowCount++;
  }
  portEXIT_CRITICAL(&nodeMux);

  for (uint8_t i = 0; i < rowCount; i++)
  {
    if (hasSprite[i])
    {
      if (joiningUsed > 0)
      {
        appendf(joining, sizeof(joining), joiningUsed, ", ");
      }
      appendf(joining, sizeof(joining), joiningUsed, "%u", rowNumbers[i]);
    }
    else
    {
      if (missingUsed > 0)
      {
        appendf(missing, sizeof(missing), missingUsed, ", ");
      }
      appendf(missing, sizeof(missing), missingUsed, "%u", rowNumbers[i]);
    }
  }
  if (missing[0])
  {
    snprintf(text, len, "playing the row wave on gauges %s - no shutdown sprite on %s, so it won't join", joining, missing);
  }
  else
  {
    snprintf(text, len, "playing the row wave on gauges %s, then the wake sprite", joining);
  }
}

// 0.29.0: put a command in the link task's queue for another gauge (the link task sends it 3 times)
static void queueCommand(uint8_t dst, uint8_t cmd, uint8_t arg0)
{
  if (!sendQueue)
  {
    return;
  }
  OutgoingCommand outgoing;
  outgoing.dst = dst;
  outgoing.cmd = cmd;
  outgoing.arg0 = arg0;
  xQueueSend(sendQueue, &outgoing, 0);
}

void linkSetRow(uint8_t nodeId, uint8_t rowPosition)
{
  if (nodeId == myNode)
  {
    applyRow(rowPosition); // this gauge: straight away
    return;
  }
  queueCommand(nodeId & 0x0F, CMD_SET_ROW, rowPosition);
}

void linkSetRole(uint8_t nodeId, uint8_t role)
{
  if (role == CHAIN_MASTER)
  {
    // only one gauge is ever set to Master: any other one that is goes back to Auto
    uint8_t forcedMasters[MAX_NODES];
    uint8_t forcedCount = 0;
    const uint32_t now = millis();
    portENTER_CRITICAL(&nodeMux);
    for (uint8_t otherId = 0; otherId < MAX_NODES; otherId++)
    {
      const LinkNode &node = nodes[otherId];
      if (otherId != myNode && otherId != nodeId && isAlive(node, now, 10000) && (node.flags & LINK_SRC_FORCED))
      {
        forcedMasters[forcedCount] = otherId;
        forcedCount++;
      }
    }
    portEXIT_CRITICAL(&nodeMux);
    for (uint8_t i = 0; i < forcedCount; i++)
    {
      queueCommand(forcedMasters[i], CMD_SET_ROLE, CHAIN_AUTO);
    }
    if (nodeId != myNode && settings.chainRole == CHAIN_MASTER)
    {
      applyRole(CHAIN_AUTO); // this gauge was the Master: back to Auto
    }
  }
  if (nodeId == myNode)
  {
    applyRole(role);
    return;
  }
  queueCommand(nodeId & 0x0F, CMD_SET_ROLE, role);
}

void linkShowNumbers(bool show)
{
  if (show)
  {
    queueCommand(0xFF, CMD_SHOW_NUMBER, 1); // every other gauge (0xFF = all of them) ...
    showOwnNumber(SHOW_NUMBER_MS);          // ... and this one
  }
  else
  {
    queueCommand(0xFF, CMD_SHOW_NUMBER, 0); // every other gauge hides it ...
    uiShowNumber(NULL, NULL, 0);            // ... and so does this one
  }
}

void linkInfoProvider(char *buf, size_t len)
{
  if (!settings.chainEnabled)
  {
    snprintf(buf, len, "chain off\n\nenable it in the web UI\n(Chain tab)");
    return;
  }
  static const char *transportNames[] = {"auto", "CAN", "ESP-NOW", "CAN + ESP-NOW", "off"};
  size_t written = 0;
  appendf(buf, len, written, "G%u %s  group %04X\n%s%s  CAN tx %s\n", myNode, linkRoleName(), settings.chainGroup,
          transportNames[settings.linkTransport < linkTransport_t_MAX ? settings.linkTransport : 0],
          stats.espNowUp ? " (ESP-NOW up)" : "", settings.linkCanTx ? "on" : "off");
  LinkNode list[MAX_NODES];
  const uint8_t count = linkNodes(list, MAX_NODES);
  if (!count && written < len)
  {
    appendf(buf, len, written, "\nno other gauges heard");
  }
  for (uint8_t i = 0; i < count && written < len; i++)
  {
    appendf(buf, len, written, "\nG%u %s %s %s%s %lu ms", list[i].node, list[i].name[0] ? list[i].name : "",
            list[i].role == 1 ? "master" : "follower", (list[i].via & 1) ? "CAN" : "", (list[i].via & 2) ? " NOW" : "",
            (unsigned long)(millis() - list[i].lastSeen));
  }
}
