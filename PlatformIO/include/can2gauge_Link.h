#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// Gauge chain link (docs/can2gauge_GaugeChain_design.md). Several gauges share live values and a few commands:
//   CAN      - 11-bit frames on the daisy-chain bus: DATA base+node, HEARTBEAT base+0x10+node, COMMAND base+0x20
//              (base = settings.linkCanBaseId, 0x6C0). Needs settings.linkCanTx.
//   ESP-NOW  - broadcast packets on the WiFi channel, filtered by settings.chainGroup; commands carry an HMAC made with
//              CHAIN_KEY (built in, the same on every can2gauge).
// One gauge is the master: the only one that polls OBD on 0x7DF, reads the gearbox (DSG UDS / TP2.0), holds the BLE
// ELM327 and sends Haldex mode frames. Election: a forced master beats an Auto candidate (one with an active source),
// ties go to the lowest node id; a master that stops sending heartbeats for 3 s is dropped.
// With the chain off, linkIsMaster() is true, so a single gauge behaves exactly as before.
// ---------------------------------------------------------------------------

#define LINK_PROTO_VERSION 1
// 0.28.3: wireless commands and chain updates are signed with this one built-in key, the same on every can2gauge, so
// gauges in the same group work together with nothing to set up
#define CHAIN_KEY "can2gauge-chain"

// heartbeat "sources" bitmap
#define LINK_SRC_ELM_CFG 0x01  // BLE ELM327 enabled
#define LINK_SRC_ELM_UP 0x02   // ... and connected
#define LINK_SRC_KLINE 0x04    // K-line session up (per gauge: the wire is on this gauge's J_OUT)
#define LINK_SRC_OBD 0x08      // CAN OBD-II polling profile selected
#define LINK_SRC_DSG 0x10      // gearbox diagnostics (UDS / TP2.0) selected
#define LINK_SRC_FORCED 0x20   // role forced to master
#define LINK_SRC_FOLLOWER 0x40 // role forced to follower (never master)
#define LINK_SRC_SHUTDOWN 0x80 // 0.29.0: a shutdown sprite is picked (it can join the row wave's Play It Now)

struct LinkNode
{
  uint8_t node;
  uint8_t role;   // the node's own view: 0 standalone, 1 master, 2 follower
  uint8_t flags;  // LINK_SRC_*
  uint8_t proto;
  uint8_t fwMajor, fwMinor;
  uint8_t owned;  // signals it is sharing
  uint8_t via;    // bit 0 = heard on CAN, bit 1 = heard on ESP-NOW
  bool night;
  uint8_t rowPosition; // its place in the row wave (settings.rowPosition, 1 = leftmost), 0 = not in a row
  bool ignitionOn;     // 0.29.2: it has its own ignition (CAN or wire) and that ignition is on (heartbeat bit 6)
  int8_t rssi;    // ESP-NOW only
  uint8_t mac[6]; // ESP-NOW only (zero on CAN)
  uint8_t version[3];  // 0.25.0: full firmware version (major, minor, patch) - ESP-NOW only, zero from older gauges
  uint8_t board;       // 0.25.0: chainUpdateBoardId() of its firmware (0 = unknown)
  uint8_t updateProto; // 0.25.0: CHAIN_UPDATE_PROTO it understands (0 = can't take a chain update)
  uint8_t ignKind;     // 0.30.4: its ignition source + 1 (0 = an older gauge, or heard on CAN only)
  uint32_t sharedRev;  // 0.30.4: its revision of the shared settings (can2gauge_Shared.h)
  uint32_t sharedCheck; // 0.30.4: the check value of its shared settings
  uint32_t uptimeS;
  uint32_t lastSeen; // millis()
  char name[NAME_LEN];
};

struct LinkStats
{
  uint32_t txCan, rxCan, txNow, rxNow, rejected, collisions;
  bool canHeard;  // a link heartbeat was heard on CAN in the last 5 s
  bool espNowUp;
};

void setupLink();
void linkRestart();       // settings changed (node id / transport / group)
bool linkChainActive();   // settings.chainEnabled
bool linkIsMaster();      // chain off, alone, or elected master
uint8_t linkNodeId();
const char *linkRoleName();
bool linkHaldexMode(uint8_t mode);  // master: sends the 0x6A0 frame; follower: asks the master
void linkPageChanged(uint8_t page); // GaugeUI hook: the master broadcasts it for "follow page"
bool linkMasterNight(bool &night);  // "follow night": the master's dash-light state (false = unknown)
void linkWifiUp();                  // WiFi (re)started - ESP-NOW may be brought up
void linkWifiDown();                // WiFi about to stop - ESP-NOW is shut down first
bool linkEspNowSendTo(const uint8_t mac[6], const void *data, size_t len); // unicast on the shared ESP-NOW stack (OpenHaldex)
uint8_t linkNodes(LinkNode *out, uint8_t maxNodes); // other gauges heard in the last 10 s
// the row wave (settings.rowPosition): this gauge's place counted from the left (0 = leftmost) among the gauges in the
// row heard in the last 3 s, and how many that is with this one. false = not in a row of two or more
bool linkRowPlace(uint8_t &placeFromLeft, uint8_t &gaugesInRow);
// "Play it now" for the row: every gauge in the row plays its shutdown together. false = not in a row (play it alone)
bool linkRowTest();
// 0.29.0: who will join the row's Play It Now and who won't (no shutdown sprite), for the web UI's message
void linkRowTestText(char *text, size_t len);
// 0.29.0: set up any gauge from any gauge. nodeId = the gauge's G number (this gauge's own = linkNodeId()).
// The other gauge is sent a signed command, saves it, and shows its new number on its glass for a few seconds.
void linkSetRow(uint8_t nodeId, uint8_t rowPosition); // its place in the row, 1 = leftmost, 0 = not in a row
void linkSetRole(uint8_t nodeId, uint8_t role);       // chainRole_t. Master: any other forced master goes back to Auto
void linkShowNumbers(bool show);                      // every gauge shows its number big on the glass (false = hide)
// 0.29.2: "Ignition from another gauge": true when a gauge heard in the last 3 s has its own ignition on. whoName gets
// that gauge's name (or "G<id>"), for the web UI.
bool linkIgnitionFrom(char *whoName, size_t len);
const LinkStats &linkStats();
void linkInfoProvider(char *buf, size_t len);
