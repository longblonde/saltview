#include "notifications.h"
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <vector>
#include "config.h"

namespace Notify {
struct Note {
  uint32_t id;
  uint32_t ts;
  Severity sev;
  String msg;
  bool read;
};

static const char* kPath = "/notes.json";
static constexpr size_t kMaxNotes = 30;
static const char* const kSevNames[] = {"info", "warn", "alert"};

static std::vector<Note> sNotes;
static std::vector<uint32_t> sPending;
static uint32_t sNextId = 1;

static void save() {
  JsonDocument doc;
  toJson(doc.to<JsonArray>());
  File f = LittleFS.open(kPath, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void begin() {
  File f = LittleFS.open(kPath, "r");
  if (!f) return;
  JsonDocument doc;
  if (!deserializeJson(doc, f)) {
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
      Note n{o["id"], o["ts"], Severity::Info, o["msg"] | "", o["read"] | false};
      for (uint8_t i = 0; i < 3; i++)
        if (o["sev"] == kSevNames[i]) n.sev = (Severity)i;
      sNotes.push_back(n);
      sNextId = max(sNextId, n.id + 1);
    }
  }
  f.close();
}

void post(Severity sev, const String& msg, bool push) {
  time_t now = time(nullptr);
  Note n{sNextId++, now >= kValidEpoch ? (uint32_t)now : 0, sev, msg, false};
  Serial.printf("[notify] %s: %s\n", kSevNames[(int)sev], msg.c_str());
  sNotes.push_back(n);
  if (sNotes.size() > kMaxNotes) sNotes.erase(sNotes.begin());
  if (push && !gConfig.ntfyTopic.isEmpty()) sPending.push_back(n.id);
  save();
}

static bool sendNtfy(const Note& n) {
  String url = gConfig.ntfyServer;
  if (!url.endsWith("/")) url += "/";
  url += gConfig.ntfyTopic;

  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  bool begun;
  if (url.startsWith("https://")) {
    secure.setInsecure();  // TODO: pin the ntfy root CA instead of skipping verification
    begun = http.begin(secure, url);
  } else {
    begun = http.begin(plain, url);
  }
  if (!begun) return false;
  http.addHeader("Title", gConfig.name);
  http.addHeader("Priority", n.sev == Severity::Alert ? "high" : "default");
  http.addHeader("Tags", n.sev == Severity::Alert ? "warning" : "droplet");
  int code = http.POST(n.msg);
  http.end();
  return code >= 200 && code < 300;
}

void loop() {
  if (sPending.empty() || WiFi.status() != WL_CONNECTED) return;
  uint32_t id = sPending.front();
  sPending.erase(sPending.begin());
  for (const auto& n : sNotes) {
    if (n.id == id) {
      if (!sendNtfy(n)) Serial.println("[notify] ntfy push failed");
      break;
    }
  }
}

void markRead(uint32_t id) {
  for (auto& n : sNotes)
    if (id == 0 || n.id == id) n.read = true;
  save();
}

void clear() {
  sNotes.clear();
  save();
}

uint8_t unread() {
  uint8_t c = 0;
  for (const auto& n : sNotes)
    if (!n.read) c++;
  return c;
}

size_t pendingPushes() { return sPending.size(); }

void toJson(JsonArray out) {
  for (const auto& n : sNotes) {
    JsonObject o = out.add<JsonObject>();
    o["id"] = n.id;
    o["ts"] = n.ts;
    o["sev"] = kSevNames[(int)n.sev];
    o["msg"] = n.msg;
    o["read"] = n.read;
  }
}
}  // namespace Notify
