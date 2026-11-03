#include <can2gauge_WiFi.h>
#include <can2gauge_Link.h> // ESP-NOW rides on the WiFi interface: stopped before WiFi goes down, restarted after
#include <wifi_manager.h>   // the shared Forbes Automotive WiFi module (_SharedTheme v2): Home WiFi bridge, static UI
#include <WiFi.h>
#include <esp_netif.h>
#include <dhcpserver/dhcpserver.h>

// can2gauge keeps its own soft-AP (per-gauge SSID "can2gauge-XXXX", optional password, the chain's ESP-NOW on the same
// interface), so the shared wifi_manager runs with manageAp = false - the Ignitron arrangement in README_OTA.md: it
// does the guarded filesystem mount, the static UI / recovery page and the Home WiFi bridge (credentials in its own
// NVS namespace, set from the OTA tab), and this file brings the AP up and down around it.
static bool restartFlag = false;
// 0.8.0: a WiFi OBD adapter needs the station interface, which is otherwise the Home WiFi bridge's. The ELM task asks
// (wifiStaClaim), the loop here stops the bridge and grants it; releasing starts the bridge again. While claimed the
// bridge's tick doesn't run, so its 5-minute retry can't pull the station off the adapter's network.
static volatile bool staWanted = false;
static volatile bool staGranted = false;

static void softAPLocalOnly()
{ // no gateway / DNS in the DHCP offer so phones keep their mobile data
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

static void startSoftAP()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAPsetHostname(settings.hostname);
  // 192.168.1.1, as on every Forbes Automotive product (the shared wifi_manager's default; with manageAp = false it
  // doesn't set it for us, and the ESP32 default would be 192.168.4.1)
  WiFi.softAPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(settings.apSsid, strlen(settings.apPassword) >= 8 ? settings.apPassword : NULL);
  softAPLocalOnly();
  DEBUG("WiFi AP \"%s\" up at %s", settings.apSsid, WiFi.softAPIP().toString().c_str());
}

void setupWiFi()
{
  static wifimgr_config_t wifiConfig = wifiDefaultConfig();
  wifiConfig.hostName = settings.apSsid;
  wifiConfig.mdnsName = settings.hostname;
  wifiConfig.fwVersion = CAN2GAUGE_VERSION;
  wifiConfig.manageAp = false;      // the AP is ours (see above)
  wifiConfig.mountLittleFS = false; // mounted (guarded) at the top of setup(), before the boot logo
  startSoftAP();                    // first: with manageAp = false the module joins Home WiFi + starts mDNS alongside our AP
  wifiManagerInit(&wifiConfig);
  linkWifiUp();
}

void pollWiFi()
{
  if (restartFlag)
  {
    restartFlag = false;
    linkWifiDown();
    wifiManagerStopAP(); // with manageAp = false: drops the bridge and mDNS, leaves the AP to us
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    startSoftAP();
    wifiManagerStartAP(); // Home WiFi bridge + mDNS again, alongside the new AP
    if (staGranted)
    {
      wifiManagerStaStop(); // the OBD adapter still has the station: it rejoins its network by itself
      WiFi.mode(WIFI_AP_STA);
    }
    linkWifiUp();
  }
  if (staWanted && !staGranted)
  {
    wifiManagerStaStop();
    WiFi.mode(WIFI_AP_STA); // the AP stays up; the station is free for the adapter's network
    staGranted = true;
  }
  else if (!staWanted && staGranted)
  {
    staGranted = false;
    wifiManagerStaStart(); // Home WiFi bridge back (no-op when none is saved)
  }
  if (staGranted)
  {
    runtime.wifiStaConnected = false; // the station is on the OBD adapter's network, not a home network
    runtime.wifiStaIP[0] = '\0';
    return;
  }
  wifiManagerTick(); // bridge connection tracking + back-off
  runtime.wifiStaConnected = wifiManagerStaConnected();
  copyStr(runtime.wifiStaIP, sizeof(runtime.wifiStaIP), runtime.wifiStaConnected ? wifiManagerStaIp().c_str() : "");
}

void wifiRestart()
{
  restartFlag = true;
}

bool wifiStaClaim(bool want, uint32_t waitMs)
{
  staWanted = want;
  const uint32_t waitStartMs = millis();
  while (staGranted != want && millis() - waitStartMs < waitMs)
  {
    delay(20);
  }
  return staGranted == want;
}

bool wifiStaConnected()
{
  return runtime.wifiStaConnected;
}
