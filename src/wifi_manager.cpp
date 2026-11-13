#include "wifi_manager.h"
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"

namespace WifiMgr {
static constexpr uint32_t kConnectTimeoutMs = 30000;
static constexpr uint8_t kJoinRetries = 2;
static constexpr uint32_t kPortalTimeoutMs = 10 * 60 * 1000;  // then retry saved network
static constexpr uint32_t kRejoinEveryMs = 30000;              // after a drop, if the library hasn't managed
static constexpr uint32_t kRestartAfterLostMs = 15 * 60 * 1000;

static Mode sMode = Mode::Connecting;
static DNSServer sDns;
static String sAp, sSsid, sPass, sLastError;
static uint32_t sPortalSince = 0;
static uint32_t sLostAt = 0, sLastAttempt = 0;
static volatile uint8_t sDisconnectReason = 0;
static volatile bool sAssociated = false;

static const char* reasonText(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      return "network not found - check the name (it's case-sensitive) and that it's 2.4 GHz";
    case WIFI_REASON_AUTH_FAIL:
      return "the router refused the connection at login, which is usually a router setting such as MAC "
             "filtering rather than the password";
    case WIFI_REASON_AUTH_EXPIRE:
      return "the router stopped responding during login";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "wrong password (or a very weak signal)";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
      return "the router refused the connection";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "signal too weak";
    default:
      return "timed out";
  }
}

static const char* authName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "other";
  }
}

// Logs every access point broadcasting the saved name, so a failed join shows what the board could see.
static void logVisibleAccessPoints() {
  int n = WiFi.scanNetworks(false, true);
  int found = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != sSsid) continue;
    found++;
    Serial.printf("[wifi] sees \"%s\" at %s, channel %d, %d dBm, %s\n", sSsid.c_str(), WiFi.BSSIDstr(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i), authName(WiFi.encryptionType(i)));
  }
  if (!found) Serial.printf("[wifi] \"%s\" not visible (%d other networks)\n", sSsid.c_str(), n);
  WiFi.scanDelete();
}

static void beginJoin() {
#ifdef SALTVIEW_TEST_BSSID  // diagnostic builds: join one access point, e.g. -DSALTVIEW_TEST_BSSID=\"aa:bb:cc:dd:ee:ff\"
  uint8_t bssid[6];
  sscanf(SALTVIEW_TEST_BSSID, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &bssid[0], &bssid[1], &bssid[2], &bssid[3], &bssid[4],
         &bssid[5]);
  WiFi.begin(sSsid.c_str(), sPass.c_str(), 0, bssid);
#else
  WiFi.begin(sSsid.c_str(), sPass.c_str());
#endif
}

static void join() {
  WiFi.disconnect();
  delay(100);
  beginJoin();
  sLastAttempt = millis();
}

static void startPortal() {
  sMode = Mode::Portal;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);  // STA half is needed for scanning
  WiFi.softAP(sAp.c_str());
  delay(100);
  sDns.setErrorReplyCode(DNSReplyCode::NoError);
  sDns.start(53, "*", WiFi.softAPIP());  // resolve every name to us -> captive portal
  WiFi.scanNetworks(true);
  sPortalSince = millis();
  Serial.printf("[wifi] setup portal: join \"%s\", open http://%s\n", sAp.c_str(),
                WiFi.softAPIP().toString().c_str());
}

void begin(const String& hostname) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ap[24];
  snprintf(ap, sizeof(ap), "SaltView-%02X%02X", mac[4], mac[5]);
  sAp = ap;

  Preferences p;
  p.begin("wifi", true);
  sSsid = p.getString("ssid", "");
  sPass = p.getString("pass", "");
  p.end();

  if (sSsid.isEmpty()) {
    startPortal();
    return;
  }

  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        if (reason != WIFI_REASON_ASSOC_LEAVE) sDisconnectReason = reason;  // our own disconnect() isn't the cause
        sAssociated = false;
        Serial.printf("[wifi] disconnected, reason %u\n", reason);
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t) {
        sAssociated = true;
        Serial.println("[wifi] associated, waiting for DHCP");
      },
      ARDUINO_EVENT_WIFI_STA_CONNECTED);
  // Lengths only, never the password itself: catches stray spaces and autocorrected input.
  Serial.printf("[wifi] joining \"%s\" (ssid %u chars, password %u chars%s)\n", sSsid.c_str(), sSsid.length(),
                sPass.length(), (sPass.length() && (isspace(sPass[0]) || isspace(sPass[sPass.length() - 1])))
                                    ? ", leading/trailing space!"
                                    : "");
  // Our credentials live in our own NVS namespace. Older firmware also let the driver save a copy on
  // every join; erase that once so no stale driver settings linger, and stop it saving again.
  WiFi.persistent(false);
  WiFi.setHostname(hostname.c_str());
  WiFi.mode(WIFI_STA);
  wifi_config_t blank = {};
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  esp_wifi_set_config(WIFI_IF_STA, &blank);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  // Scan every channel and pick the strongest access point, not the first one heard.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  if (gConfig.power == PowerMode::AlwaysOn) WiFi.setSleep(false);  // USB powered: radio power save only costs reliability
  WiFi.setAutoReconnect(true);
  logVisibleAccessPoints();
  // Join with the factory MAC marked "locally administered" (86:.. instead of 84:..). Some
  // networks refuse the factory address outright (reason 202); this one is the same on every
  // boot, so the router still sees one device and the DHCP lease (IP address) stays put.
  uint8_t staMac[6];
  esp_read_mac(staMac, ESP_MAC_WIFI_STA);
  staMac[0] = (staMac[0] & 0xFC) | 0x02;  // locally administered, unicast
  esp_wifi_set_mac(WIFI_IF_STA, staMac);
  Serial.printf("[wifi] using MAC %s\n", WiFi.macAddress().c_str());
#ifdef SALTVIEW_TEST_BSSID
  Serial.printf("[wifi] TEST: only access point %s\n", SALTVIEW_TEST_BSSID);
#endif
  beginJoin();
  uint32_t t0 = millis();
  uint8_t retries = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kConnectTimeoutMs) {
    // The Wi-Fi library gives up for good after a rejected login, so ask again ourselves.
    if (WiFi.status() == WL_CONNECT_FAILED && retries < kJoinRetries) {
      retries++;
      Serial.printf("[wifi] rejected, retry %u of %u\n", retries, kJoinRetries);
      delay(900);
      join();
    }
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    sLastError = (sAssociated && WiFi.isConnected())
                     ? "joined the network but the router gave no IP address (DHCP)"
                     : reasonText(sDisconnectReason);
    Serial.printf("[wifi] could not join \"%s\": %s (reason %u)\n", sSsid.c_str(), sLastError.c_str(),
                  sDisconnectReason);
    startPortal();
    return;
  }

  sMode = Mode::Station;
  if (MDNS.begin(hostname.c_str())) MDNS.addService("http", "tcp", 80);
  Serial.printf("[wifi] connected: http://%s  (http://%s.local)\n", ip().c_str(), hostname.c_str());
}

// The library reconnects by itself only for some disconnect reasons (not, for example, when the
// router refuses a login), so watch the link and rejoin ourselves until it comes back.
static void watchStation() {
  uint32_t now = millis();
#ifdef SALTVIEW_TEST_HEARTBEAT  // diagnostic builds: link state every 10 s, and whether the router answers
  static uint32_t lastBeat = 0;
  if (now - lastBeat >= 10000) {
    lastBeat = now;
    WiFiClient probe;
    bool gw = WiFi.isConnected() && probe.connect(WiFi.gatewayIP(), 80, 1000);
    probe.stop();
    Serial.printf("[wifi] beat: status %d, ip %s, ap %s, %d dBm, router %s\n", WiFi.status(),
                  WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(), WiFi.RSSI(), gw ? "reachable" : "NOT reachable");
  }
#endif
#ifdef SALTVIEW_TEST_DROP  // diagnostic builds: drop the link once, 60 s in, to exercise the rejoin path
  static bool dropped = false;
  if (!dropped && now > 60000 && WiFi.status() == WL_CONNECTED) {
    dropped = true;
    Serial.println("[wifi] TEST: dropping the connection");
    WiFi.disconnect();  // a voluntary leave: the library won't reconnect by itself
  }
#endif
  if (WiFi.status() == WL_CONNECTED) {
    if (sLostAt) Serial.printf("[wifi] back online after %lus: %s\n", (unsigned long)(now - sLostAt) / 1000, ip().c_str());
    sLostAt = 0;
    return;
  }
  if (!sLostAt) {
    sLostAt = sLastAttempt = now;  // give the library's own reconnect a first chance
    Serial.printf("[wifi] connection lost (reason %u)\n", sDisconnectReason);
    return;
  }
  if (now - sLostAt >= kRestartAfterLostMs) {
    Serial.println("[wifi] offline too long, restarting");
    ESP.restart();
  }
  if (now - sLastAttempt >= kRejoinEveryMs) {
    Serial.printf("[wifi] rejoining (offline %lus, last reason %u)\n", (unsigned long)(now - sLostAt) / 1000,
                  sDisconnectReason);
    join();
  }
}

void loop() {
  if (sMode == Mode::Station) return watchStation();
  if (sMode != Mode::Portal) return;
  sDns.processNextRequest();
  if (!sSsid.isEmpty() && millis() - sPortalSince > kPortalTimeoutMs) ESP.restart();
}

Mode mode() { return sMode; }
String apName() { return sAp; }
String savedSsid() { return sSsid; }
String lastError() { return sLastError; }

String ip() {
  if (sMode == Mode::Portal) return WiFi.softAPIP().toString();
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
}

String scanJson() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return "{\"scanning\":true}";
  if (n < 0) {
    WiFi.scanNetworks(true);
    return "{\"scanning\":true}";
  }
  JsonDocument doc;
  JsonArray nets = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    bool dup = false;
    for (JsonObject o : nets)
      if (o["ssid"] == ssid) dup = true;
    if (dup) continue;
    JsonObject o = nets.add<JsonObject>();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  return out;
}

void saveCredentials(const String& ssid, const String& pass) {
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

void forget() {
  Preferences p;
  p.begin("wifi", false);
  p.clear();
  p.end();
}
}  // namespace WifiMgr
