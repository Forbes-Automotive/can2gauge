// WiFi OBD adapter link: the TCP connection to a WiFi ELM327-type adapter (see can2gauge_ElmWiFi.h). The ELM task uses
// these the same way as the Bluetooth link, so it doesn't care which one carries the commands.
#include <can2gauge_ElmWiFi.h>
#include <can2gauge_WiFi.h> // wifiStaClaim()
#include <WiFi.h>

// ===== Variables for this file =====
static bool joined = false;    // the gauge has joined the adapter's network (as a station, its own AP stays up)
static char linkInfo[96] = ""; // the link as the web UI shows it (network, channel, address, signal), "" = not connected
static WiFiClient client;      // the TCP socket to the adapter
// the network names WiFi OBD adapters ship with [U: from adapter manuals / forums - extend when one isn't recognised]
static const char *obdNames[] = {"OBD", "ELM", "V-LINK", "VLINK", "VGATE", "ICAR", "KONNWEI", "VEEPEAK", "OBDLINK", "WIFI_OBD"};

// ===== Functions in this file =====
static bool pickNetwork(char *err, size_t errLen);                   // no network saved: finds and saves an OBD-looking one
static bool joinAdapterNetwork(const IPAddress &host, bool fixedIp); // joins the adapter's network (DHCP or a fixed address)
static bool nameLooksLikeObd(const char *name);                      // a network name an OBD adapter ships with?

// joins the adapter's network (if not on it already) and opens the TCP socket. false = not connected, with the reason
// in err (the web UI shows it)
bool elmWifiConnect(char *err, size_t errLen)
{
  err[0] = '\0';
  linkInfo[0] = '\0';
  IPAddress host;
  if (!host.fromString(settings.elmWifiHost))
  {
    snprintf(err, errLen, "adapter IP \"%s\" isn't an address", settings.elmWifiHost);
    return false;
  }
  // the gauge's own AP subnet (192.168.1.x, startSoftAP) can't be routed to
  const IPAddress apIp = WiFi.softAPIP();
  if (host[0] == apIp[0] && host[1] == apIp[1] && host[2] == apIp[2])
  {
    snprintf(err, errLen, "adapter on %u.%u.%u.x = the gauge's own AP subnet - can't route it", apIp[0], apIp[1], apIp[2]);
    return false;
  }
  if (!wifiStaClaim(true, 3000))
  {
    snprintf(err, errLen, "WiFi station not free yet");
    return false;
  }
  // the gauge's own AP carries the web UI - the only way to change these settings. Never join without it.
  if (!(WiFi.getMode() & WIFI_MODE_AP))
  {
    snprintf(err, errLen, "gauge AP not up - not joining the adapter");
    wifiStaClaim(false, 3000);
    return false;
  }
  if (!settings.elmWifiSsid[0] && !pickNetwork(err, errLen))
  {
    return false;
  }
  if (!joined || WiFi.status() != WL_CONNECTED || WiFi.SSID() != settings.elmWifiSsid)
  {
    WiFi.disconnect(false); // station only: the AP stays up
    if (!joinAdapterNetwork(host, false) && !joinAdapterNetwork(host, true))
    {
      const char *passwordHint = ""; // with a password set, it may be the wrong one
      if (settings.elmWifiPass[0])
      {
        passwordHint = " (password?)";
      }
      snprintf(err, errLen, "could not join \"%s\"%s", settings.elmWifiSsid, passwordHint);
      WiFi.disconnect(false);
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE); // DHCP again: the fixed-IP try mustn't stay for Home WiFi
      return false;
    }
    joined = true;
  }
  if (!client.connect(host, settings.elmWifiPort, 3000))
  {
    snprintf(err, errLen, "joined \"%s\" but no TCP answer from %s:%u", settings.elmWifiSsid, settings.elmWifiHost,
             settings.elmWifiPort);
    return false;
  }
  client.setNoDelay(true); // one short command at a time: don't let Nagle hold it back
  snprintf(linkInfo, sizeof(linkInfo), "WiFi \"%s\" ch %d, %s:%u from %s, %d dBm", settings.elmWifiSsid, (int)WiFi.channel(),
           settings.elmWifiHost, settings.elmWifiPort, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return true;
}

// closes the socket, leaves the adapter's network and gives the station back to the Home WiFi bridge
void elmWifiDisconnect()
{
  client.stop();
  if (joined)
  {
    WiFi.disconnect(false);
    joined = false;
  }
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE); // DHCP again before the station goes back to the Home WiFi bridge
  wifiStaClaim(false, 3000);
  linkInfo[0] = '\0';
}

// on the adapter's network with the socket open
bool elmWifiConnected()
{
  return joined && client.connected();
}

// sends bytes to the adapter: false = not connected, or not all of them went
bool elmWifiWrite(const uint8_t *data, size_t len)
{
  return elmWifiConnected() && client.write(data, len) == len;
}

// reads what the adapter has sent, up to maxLen bytes, waiting up to timeoutMs for the first to arrive. Returns the
// byte count: 0 = nothing came in time, or the socket closed.
size_t elmWifiRead(uint8_t *buf, size_t maxLen, uint32_t timeoutMs)
{
  const uint32_t startMs = millis();
  while (1)
  {
    const int availableBytes = client.available();
    if (availableBytes > 0)
    {
      size_t readLen = maxLen; // no more than the buffer holds
      if ((size_t)availableBytes < maxLen)
      {
        readLen = (size_t)availableBytes;
      }
      return client.read(buf, readLen);
    }
    if (!client.connected() || millis() - startMs >= timeoutMs)
    {
      return 0;
    }
    delay(2);
  }
}

// throws away anything the adapter sent that hasn't been read
void elmWifiFlush()
{
  while (client.available() > 0)
  {
    client.read();
  }
}

// the link as the web UI shows it ("" while not connected)
const char *elmWifiLinkInfo()
{
  return linkInfo;
}

// no network saved: join the strongest OBD-looking network and save it. A scan takes the radio off the AP's channel for
// ~2-3 s (phones on the gauge's AP may stall), so at most one every 2 minutes.
static bool pickNetwork(char *err, size_t errLen)
{
  static uint32_t lastScan = 0; // static: kept from one call to the next (millis() of the last scan, 0 = none yet)
  if (lastScan && millis() - lastScan < 120000)
  {
    snprintf(err, errLen, "no adapter network saved - next scan in %lu s",
             (unsigned long)((120000 - (millis() - lastScan)) / 1000));
    return false;
  }
  lastScan = millis();
  const int networkCount = WiFi.scanNetworks(false, false); // ~2-3 s; the gauge's AP stays up
  int bestIndex = -1;
  for (int networkIndex = 0; networkIndex < networkCount; networkIndex++)
  {
    if (nameLooksLikeObd(WiFi.SSID(networkIndex).c_str()) &&
        (bestIndex < 0 || WiFi.RSSI(networkIndex) > WiFi.RSSI(bestIndex)))
    {
      bestIndex = networkIndex;
    }
  }
  if (bestIndex < 0)
  {
    int networksSeen = networkCount; // a failed scan answers below 0: shown as 0
    if (networkCount < 0)
    {
      networksSeen = 0;
    }
    snprintf(err, errLen, "no OBD adapter network found (%d networks seen)", networksSeen);
    WiFi.scanDelete();
    return false;
  }
  copyStr(settings.elmWifiSsid, sizeof(settings.elmWifiSsid), WiFi.SSID(bestIndex).c_str());
  settingsDirty = true;
  WiFi.scanDelete();
  return true;
}

// joins the adapter's network (settings.elmWifiSsid), by DHCP or with a fixed address on the adapter's subnet. Waits up
// to 8 s; true = joined.
static bool joinAdapterNetwork(const IPAddress &host, bool fixedIp)
{
  if (fixedIp)
  {
    // the adapter's subnet, .123 (.124 if the adapter itself is .123), the adapter as the gateway
    IPAddress stationIp = host;
    if (host[3] == 123)
    {
      stationIp[3] = 124;
    }
    else
    {
      stationIp[3] = 123;
    }
    WiFi.config(stationIp, host, IPAddress(255, 255, 255, 0));
  }
  else
  {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE); // DHCP
  }
  const char *password = NULL; // NULL = an open network (most adapters are)
  if (settings.elmWifiPass[0])
  {
    password = settings.elmWifiPass;
  }
  WiFi.begin(settings.elmWifiSsid, password);
  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 8000)
  {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// does this network name look like an OBD adapter's? (any of obdNames in it, upper or lower case)
static bool nameLooksLikeObd(const char *name)
{
  char upperName[40];
  size_t i = 0;
  for (; name[i] && i < sizeof(upperName) - 1; i++)
  {
    upperName[i] = toupper((unsigned char)name[i]);
  }
  upperName[i] = '\0';
  for (const char *keyword : obdNames) // each name in the list in turn

  {
    if (strstr(upperName, keyword))
    {
      return true;
    }
  }
  return false;
}
