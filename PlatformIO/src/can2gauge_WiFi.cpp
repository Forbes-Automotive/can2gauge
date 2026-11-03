// can2gauge keeps its own soft-AP (per-gauge SSID "can2gauge-XXXX", optional password, the chain's ESP-NOW on the same
// interface), so the shared wifi_manager runs with manageAp = false - the Ignitron arrangement in README_OTA.md: it
// does the guarded filesystem mount, the static UI / recovery page and the Home WiFi bridge (credentials in its own
// NVS namespace, set from the OTA tab), and this file brings the AP up and down around it.
//
// A WiFi OBD adapter needs the station interface, which is otherwise the Home WiFi bridge's. The ELM task asks
// (wifiStaClaim), the house task here (pollWiFi) stops the bridge and grants it; releasing starts the bridge again.
// While claimed the bridge's tick doesn't run, so its 5-minute retry can't pull the station off the adapter's network.

#include <can2gauge_WiFi.h>
#include <can2gauge_Link.h> // ESP-NOW rides on the WiFi interface: stopped before WiFi goes down, restarted after
#include <wifi_manager.h>   // the shared Forbes Automotive WiFi module (_SharedTheme v2): Home WiFi bridge, static UI
#include <WiFi.h>
#include <esp_netif.h>             // the AP's DHCP server settings
#include <dhcpserver/dhcpserver.h> // dhcps_offer_t

// ===== File variables (static: private to this file) =====
static bool isRestartWanted = false; // the AP / station settings changed: pollWiFi restarts the WiFi
// volatile: the ELM task sets / waits on these while the house task (pollWiFi) changes them
static volatile bool isStationWanted = false;  // the WiFi OBD adapter wants the station interface
static volatile bool isStationGranted = false; // the Home WiFi bridge is stopped and the adapter has the station

// ===== Functions in this file =====
static void startSoftAP();     // the gauge's own AP up at 192.168.1.1
static void softAPLocalOnly(); // no gateway / DNS in the AP's DHCP offer, so phones keep their mobile data

// the gauge's own AP, then the shared module (Home WiFi bridge + mDNS) alongside it, then the chain's ESP-NOW
void setupWiFi()
{
  DEBUG("Setting up WiFi...");
  static wifimgr_config_t wifiConfig = wifiDefaultConfig(); // the shared module's defaults, then can2gauge's own below
  wifiConfig.hostName = settings.apSsid;
  wifiConfig.mdnsName = settings.hostname;
  wifiConfig.fwVersion = CAN2GAUGE_VERSION;
  wifiConfig.manageAp = false;      // the AP is ours (see the top of this file)
  wifiConfig.mountLittleFS = false; // mounted (guarded) at the top of setup(), before the boot logo
  startSoftAP();                    // first: with manageAp = false the module joins Home WiFi + starts mDNS alongside our AP
  wifiManagerInit(&wifiConfig);
  linkWifiUp();
  DEBUG("Set up WiFi!");
}

// WiFi upkeep (house task): a restart after a settings change, handing the station to / from the OBD adapter, and
// the Home WiFi bridge's connection
void pollWiFi()
{
  if (isRestartWanted)
  {
    isRestartWanted = false;
    linkWifiDown();
    wifiManagerStopAP(); // with manageAp = false: drops the bridge and mDNS, leaves the AP to us
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    startSoftAP();
    wifiManagerStartAP(); // Home WiFi bridge + mDNS again, alongside the new AP
    if (isStationGranted)
    {
      wifiManagerStaStop(); // the OBD adapter still has the station: it rejoins its network by itself
      WiFi.mode(WIFI_AP_STA);
    }
    linkWifiUp();
  }
  if (isStationWanted && !isStationGranted)
  {
    wifiManagerStaStop();
    WiFi.mode(WIFI_AP_STA); // the AP stays up; the station is free for the adapter's network
    isStationGranted = true;
  }
  else if (!isStationWanted && isStationGranted)
  {
    isStationGranted = false;
    wifiManagerStaStart(); // Home WiFi bridge back (no-op when none is saved)
  }
  if (isStationGranted)
  {
    runtime.wifiStaConnected = false; // the station is on the OBD adapter's network, not a home network
    runtime.wifiStaIP[0] = '\0';
    return;
  }
  wifiManagerTick(); // bridge connection tracking + back-off
  runtime.wifiStaConnected = wifiManagerStaConnected();
  // the Home WiFi address for the web UI and the screen (blank while not joined)
  if (runtime.wifiStaConnected)
  {
    copyStr(runtime.wifiStaIP, sizeof(runtime.wifiStaIP), wifiManagerStaIp().c_str());
  }
  else
  {
    copyStr(runtime.wifiStaIP, sizeof(runtime.wifiStaIP), "");
  }
}

// asks pollWiFi to restart the WiFi (after the AP / station settings change)
void wifiRestart()
{
  isRestartWanted = true;
}

// the station for a WiFi OBD adapter: want = true takes it from the Home WiFi bridge, false gives it back. Waits up to
// waitMs for pollWiFi to do it; returns whether it happened.
bool wifiStaClaim(bool want, uint32_t waitMs)
{
  isStationWanted = want;
  const uint32_t waitStartMs = millis();
  while (isStationGranted != want && millis() - waitStartMs < waitMs)
  {
    delay(20);
  }
  return isStationGranted == want;
}

// joined to a home network over the Home WiFi bridge?
bool wifiStaConnected()
{
  return runtime.wifiStaConnected;
}

// the gauge's own AP: its SSID / hostname / password from the settings, at 192.168.1.1
static void startSoftAP()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAPsetHostname(settings.hostname);
  // 192.168.1.1, as on every Forbes Automotive product (the shared wifi_manager's default; with manageAp = false it
  // doesn't set it for us, and the ESP32 default would be 192.168.4.1)
  WiFi.softAPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
  // a password only once it is long enough for WPA2 (8 characters), otherwise an open AP
  const char *password = NULL;
  if (strlen(settings.apPassword) >= 8)
  {
    password = settings.apPassword;
  }
  WiFi.softAP(settings.apSsid, password);
  softAPLocalOnly();
  DEBUG("WiFi AP \"%s\" up at %s", settings.apSsid, WiFi.softAPIP().toString().c_str());
}

// no gateway / DNS in the DHCP offer so phones keep their mobile data
static void softAPLocalOnly()
{
  esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!apNetif)
  {
    return;
  }
  esp_netif_dhcps_stop(apNetif);
  dhcps_offer_t offer = 0;
  esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &offer, sizeof(offer));
  esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
  esp_netif_dhcps_start(apNetif);
}
