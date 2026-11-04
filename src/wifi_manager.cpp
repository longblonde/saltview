#include "wifi_manager.h"
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

namespace WifiMgr {
static constexpr uint32_t kConnectTimeoutMs = 30000;
static constexpr uint32_t kPortalTimeoutMs = 10 * 60 * 1000;  // then retry saved network

static Mode sMode = Mode::Connecting;
static DNSServer sDns;
static String sAp, sSsid, sLastError;
static uint32_t sPortalSince = 0;
static volatile uint8_t sDisconnectReason = 0;
static volatile bool sAssociated = false;

static const char* reasonText(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      return "network not found - check the name (it's case-sensitive) and that it's 2.4 GHz";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "wrong password, or the network's security mode isn't supported";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
      return "the router refused the connection";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "signal too weak";
    default:
      return "timed out";
  }
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
  String pass = p.getString("pass", "");
  p.end();

  if (sSsid.isEmpty()) {
    startPortal();
    return;
  }

  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        sDisconnectReason = info.wifi_sta_disconnected.reason;
        Serial.printf("[wifi] disconnected, reason %u\n", sDisconnectReason);
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
                pass.length(), (pass.length() && (isspace(pass[0]) || isspace(pass[pass.length() - 1])))
                                   ? ", leading/trailing space!"
                                   : "");
  WiFi.setHostname(hostname.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(sSsid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kConnectTimeoutMs) delay(100);
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

void loop() {
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
