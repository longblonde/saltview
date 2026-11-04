#include "wifi_manager.h"
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

namespace WifiMgr {
static constexpr uint32_t kConnectTimeoutMs = 20000;
static constexpr uint32_t kPortalTimeoutMs = 10 * 60 * 1000;  // then retry saved network

static Mode sMode = Mode::Connecting;
static DNSServer sDns;
static String sAp, sSsid;
static uint32_t sPortalSince = 0;

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

  WiFi.setHostname(hostname.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(sSsid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kConnectTimeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[wifi] could not join \"%s\"\n", sSsid.c_str());
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
