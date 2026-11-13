#include "web_server.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include "app.h"
#include "config.h"
#include "history.h"
#include "notifications.h"
#include "portal_html.h"
#include "wifi_manager.h"

namespace Web {
static WebServer server(80);

static void sendJson(int code, const JsonDocument& doc) {
  String s;
  serializeJson(doc, s);
  server.send(code, "application/json", s);
}

static void sendError(int code, const String& msg) { server.send(code, "text/plain", msg); }

static bool readBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) {
    sendError(400, "missing JSON body");
    return false;
  }
  if (DeserializationError e = deserializeJson(doc, server.arg("plain"))) {
    sendError(400, String("bad JSON: ") + e.c_str());
    return false;
  }
  return true;
}

static const char* contentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".ico")) return "image/x-icon";
  return "text/plain";
}

static bool serveFile(String path) {
  if (path.endsWith("/")) path += "index.html";
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  server.streamFile(f, contentType(path));
  f.close();
  return true;
}

static bool isPortal() { return WifiMgr::mode() == WifiMgr::Mode::Portal; }

// In portal mode, bounce requests for other hosts (OS connectivity checks) to us.
static bool captiveRedirect() {
  if (!isPortal()) return false;
  String ap = WiFi.softAPIP().toString();
  if (server.hostHeader() == ap) return false;
  server.sendHeader("Location", "http://" + ap + "/", true);
  server.send(302, "text/plain", "");
  return true;
}

static void readingToJson(const Reading& r, JsonObject o) {
  o["ts"] = r.ts;
  o["raw"] = r.raw;
  o["distMm"] = r.distMm;  // NAN serializes as null
  o["depthMm"] = r.depthMm;
  o["pct"] = r.pct;
}

static void sendConfig() {
  JsonDocument doc;
  configToJson(gConfig, doc.to<JsonObject>());
  sendJson(200, doc);
}

// --- handlers ---------------------------------------------------------------

static void handleRoot() {
  if (captiveRedirect()) return;
  if (isPortal()) {
    server.send_P(200, "text/html", kPortalHtml);
    return;
  }
  if (!serveFile("/index.html"))
    server.send(200, "text/html",
                "<h1>SaltView</h1><p>Dashboard files missing. Run <code>pio run -t uploadfs</code>.</p>"
                "<p><a href=/wifi>Wi-Fi setup</a></p>");
}

static void handleStatus() {
  JsonDocument doc;
  doc["name"] = gConfig.name;
  doc["fw"] = kFwVersion;
  doc["mode"] = isPortal() ? "portal" : "station";
  doc["ssid"] = WiFi.SSID();
  doc["ip"] = WifiMgr::ip();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  time_t t = time(nullptr);
  doc["time"] = t >= kValidEpoch ? (uint32_t)t : 0;
  doc["sensorOk"] = gSensor.ok();
  doc["calibrated"] = !gConfig.cal.empty();
  readingToJson(gLast, doc["reading"].to<JsonObject>());
  doc["lowPct"] = gConfig.lowPct;
  doc["units"] = gConfig.units;
  doc["emptyMm"] = gConfig.emptyMm;
  doc["fullMm"] = gConfig.fullMm;
  doc["usagePerDay"] = gUsagePerDay;
  doc["daysLeft"] = gDisplay.data.daysLeft;
  doc["unread"] = Notify::unread();
  doc["displayAwake"] = gDisplay.awake();
  doc["power"] = gConfig.power == PowerMode::DeepSleep ? "sleep" : "on";
  sendJson(200, doc);
}

// Unlogged reading for the calibration page.
static void handleLive() {
  uint16_t raw = gSensor.readRaw(gConfig.samples);
  float mm = gSensor.rawToMm(raw);
  JsonDocument doc;
  doc["sensorOk"] = gSensor.ok();
  doc["raw"] = raw;
  doc["distMm"] = mm;
  float pct = (gConfig.emptyMm - mm) / (gConfig.emptyMm - gConfig.fullMm) * 100.0f;
  doc["pct"] = isnan(mm) ? NAN : constrain(pct, 0.0f, 100.0f);
  sendJson(200, doc);
}

static void handleMeasure() {
  takeReading(true);
  JsonDocument doc;
  readingToJson(gLast, doc.to<JsonObject>());
  sendJson(200, doc);
}

static void handleHistory() {
  int days = server.hasArg("days") ? constrain(server.arg("days").toInt(), 1, 3650) : 30;
  time_t now = time(nullptr);
  uint32_t since = now >= kValidEpoch ? now - days * 86400UL : 0;

  // Stream [[ts,pct,depthMm,raw,distMm],...] in chunks; the file can be hundreds of KB.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String buf = "[";
  bool first = true;
  History::forEach(since, [&](const History::Row& r) {
    if (!first) buf += ',';
    first = false;
    buf += '[';
    buf += r.ts;
    buf += ',';
    buf += String(r.pct, 1);
    buf += ',';
    buf += String(r.depthMm, 0);
    buf += ',';
    buf += r.raw;
    buf += ',';
    buf += String(r.distMm, 0);
    buf += ']';
    if (buf.length() > 1024) {
      server.sendContent(buf);
      buf = "";
    }
  });
  buf += ']';
  server.sendContent(buf);
  server.sendContent("");
}

// {"ts":[1794591517, ...]} -> {"removed":n}; for bad readings (testing, a hand in the tank).
static void handleHistoryDelete() {
  JsonDocument body;
  if (!readBody(body)) return;
  std::vector<uint32_t> ts;
  for (JsonVariant v : body["ts"].as<JsonArray>())
    if (v.is<uint32_t>()) ts.push_back(v.as<uint32_t>());
  if (ts.empty() || ts.size() > 2000) return server.send(400, "text/plain", "send 1-2000 timestamps in \"ts\"");
  size_t removed = History::remove(ts);
  gUsagePerDay = History::usagePctPerDay(14);
  JsonDocument doc;
  doc["removed"] = removed;
  sendJson(200, doc);
}

static void handleConfigPost() {
  JsonDocument body;
  if (!readBody(body)) return;
  Config c = gConfig;
  String err;
  if (!configFromJson(body.as<JsonObjectConst>(), c, err)) return sendError(400, err);
  gConfig = c;
  if (!configSave()) return sendError(500, "could not save settings");
  applyConfig();
  sendConfig();
}

static void handleCalibrate() {
  JsonDocument body;
  if (!readBody(body)) return;
  String action = body["action"] | "";
  Config c = gConfig;
  JsonDocument out;

  if (action == "capture") {
    float mm = body["mm"] | NAN;
    if (!(mm > 0)) return sendError(400, "known distance (mm) required");
    uint16_t raw = gSensor.readRaw(max<uint8_t>(c.samples, 32));
    for (size_t i = c.cal.size(); i-- > 0;)
      if (fabsf(c.cal[i].mm - mm) < 5) c.cal.erase(c.cal.begin() + i);  // replace same distance
    if (c.cal.size() >= kMaxCalPoints) return sendError(400, "too many calibration points");
    c.cal.push_back({raw, mm});
    out["raw"] = raw;
  } else if (action == "remove") {
    size_t i = body["index"] | 999;
    if (i < c.cal.size()) c.cal.erase(c.cal.begin() + i);
  } else if (action == "clear") {
    c.cal.clear();
  } else if (action == "baseline") {
    c.baseline = gSensor.readRaw(32);
    out["raw"] = c.baseline;
  } else if (action == "setEmpty" || action == "setFull") {
    float mm = body["mm"] | NAN;  // explicit value, or measure now
    if (isnan(mm)) mm = gSensor.rawToMm(gSensor.readRaw(32));
    if (!(mm > 0)) return sendError(400, "no distance available - add calibration points first");
    (action == "setEmpty" ? c.emptyMm : c.fullMm) = mm;
    out["mm"] = mm;
  } else {
    return sendError(400, "unknown action");
  }

  if (!(c.emptyMm > c.fullMm)) return sendError(400, "empty distance must be greater than full distance");
  gConfig = c;
  configSave();
  applyConfig();
  configToJson(gConfig, out["config"].to<JsonObject>());
  sendJson(200, out);
}

// Renders display items (posted, unsaved ones or the saved list) with live data.
static void handleDisplayRender() {
  JsonDocument body;
  if (server.hasArg("plain")) deserializeJson(body, server.arg("plain"));
  std::vector<DisplayItem> items;
  if (body["items"].is<JsonArrayConst>()) {
    for (JsonObjectConst j : body["items"].as<JsonArrayConst>()) {
      DisplayItem it;
      if (itemFromJson(j, it)) items.push_back(it);
    }
  } else {
    items = gConfig.items;
  }
  JsonDocument doc;
  JsonArray arr = doc["items"].to<JsonArray>();
  for (const auto& it : items) {
    JsonArray cards = arr.add<JsonObject>()["cards"].to<JsonArray>();
    for (const auto& c : DisplayManager::itemCards(it, gDisplay.data)) {
      JsonObject j = cards.add<JsonObject>();
      j["text"] = c.text;
      j["ms"] = DisplayManager::cardMs(c);  // includes scroll time
      if (c.marquee) j["marquee"] = true;
    }
  }
  sendJson(200, doc);
}

static void handleNotifications() {
  JsonDocument doc;
  doc["unread"] = Notify::unread();
  Notify::toJson(doc["items"].to<JsonArray>());
  sendJson(200, doc);
}

static void handleNotificationsPost() {
  JsonDocument body;
  if (!readBody(body)) return;
  String action = body["action"] | "";
  if (action == "read") Notify::markRead(body["id"] | 0);
  else if (action == "clear") Notify::clear();
  else if (action == "test") Notify::post(Severity::Info, "Test notification from " + gConfig.name);
  else return sendError(400, "unknown action");
  handleNotifications();
}

static void handleWifiSave() {
  JsonDocument body;
  if (!readBody(body)) return;
  String ssid = body["ssid"] | "";
  if (ssid.isEmpty() || ssid.length() > 32) return sendError(400, "invalid network name");
  WifiMgr::saveCredentials(ssid, body["pass"] | "");
  server.send(200, "application/json", "{\"ok\":true}");
  gDisplay.flash("JOINING " + ssid, 3000);
  gRestartAt = millis() + 1500;
}

static void ok() { server.send(200, "application/json", "{\"ok\":true}"); }

void begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_GET, [] { server.send_P(200, "text/html", kPortalHtml); });

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/measure", HTTP_POST, handleMeasure);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/history/clear", HTTP_POST, [] {
    History::clear();
    gUsagePerDay = NAN;
    ok();
  });
  server.on("/api/history/delete", HTTP_POST, handleHistoryDelete);
  server.on("/api/config", HTTP_GET, sendConfig);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/calibrate", HTTP_POST, handleCalibrate);
  server.on("/api/display/render", HTTP_POST, handleDisplayRender);
  server.on("/api/display/wake", HTTP_POST, [] {
    gDisplay.wake();
    ok();
  });
  server.on("/api/notifications", HTTP_GET, handleNotifications);
  server.on("/api/notifications", HTTP_POST, handleNotificationsPost);
  server.on("/api/wifi/scan", HTTP_GET, [] { server.send(200, "application/json", WifiMgr::scanJson()); });
  server.on("/api/wifi/status", HTTP_GET, [] {
    JsonDocument doc;
    doc["ssid"] = WifiMgr::savedSsid();
    doc["error"] = WifiMgr::lastError();
    sendJson(200, doc);
  });
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/wifi/forget", HTTP_POST, [] {
    WifiMgr::forget();
    ok();
    gRestartAt = millis() + 1000;
  });
  server.on("/api/reboot", HTTP_POST, [] {
    ok();
    gRestartAt = millis() + 1000;
  });

  server.onNotFound([] {
    if (captiveRedirect()) return;
    if (!serveFile(server.uri())) sendError(404, "not found");
  });
  server.begin();
}

void loop() { server.handleClient(); }
}  // namespace Web
