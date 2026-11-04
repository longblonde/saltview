#include "config.h"
#include <Preferences.h>

// Settings live in NVS (not LittleFS) so `pio run -t uploadfs` doesn't wipe them.
static const char* kNamespace = "saltview";
static const char* kKey = "cfg";

Config gConfig;

static const char* const kItemNames[] = {"level", "depth", "days", "status", "clock", "ip", "custom"};
static_assert(sizeof(kItemNames) / sizeof(kItemNames[0]) == (size_t)ItemType::COUNT, "item names");

const char* itemTypeName(ItemType t) {
  auto i = (size_t)t;
  return i < (size_t)ItemType::COUNT ? kItemNames[i] : kItemNames[0];
}

bool itemTypeFromName(const char* s, ItemType& out) {
  if (!s) return false;
  for (size_t i = 0; i < (size_t)ItemType::COUNT; i++) {
    if (strcmp(s, kItemNames[i]) == 0) {
      out = (ItemType)i;
      return true;
    }
  }
  return false;
}

void configDefaults(Config& c) {
  c = Config();
  c.items = {
      {ItemType::Level, true, 3, "SALT", ""},
      {ItemType::DaysLeft, true, 3, "LEFT", ""},
      {ItemType::Status, true, 2, "", ""},
  };
}

template <typename T>
static void take(JsonObjectConst in, const char* key, T& v) {
  if (!in[key].isNull()) v = in[key].as<T>();
}

bool itemFromJson(JsonObjectConst in, DisplayItem& it) {
  if (!itemTypeFromName(in["type"] | "", it.type)) return false;
  it.enabled = in["enabled"] | true;
  it.seconds = constrain((int)(in["seconds"] | 3), 1, 60);
  it.label = String(in["label"] | "").substring(0, 4);
  it.text = String(in["text"] | "").substring(0, 64);
  return true;
}

void configToJson(const Config& c, JsonObject o) {
  o["name"] = c.name;
  o["tz"] = c.tz;
  o["units"] = c.units;
  o["emptyMm"] = c.emptyMm;
  o["fullMm"] = c.fullMm;
  o["lowPct"] = c.lowPct;
  o["intervalMin"] = c.intervalMin;
  o["samples"] = c.samples;
  o["ledCurrent"] = c.ledCurrent;
  o["integration"] = c.integration;
  o["baseline"] = c.baseline;
  JsonArray cal = o["cal"].to<JsonArray>();
  for (const auto& p : c.cal) {
    JsonObject j = cal.add<JsonObject>();
    j["raw"] = p.raw;
    j["mm"] = p.mm;
  }
  o["color"] = c.color;
  o["brightness"] = c.brightness;
  o["awakeSec"] = c.awakeSec;
  JsonArray items = o["items"].to<JsonArray>();
  for (const auto& it : c.items) {
    JsonObject j = items.add<JsonObject>();
    j["type"] = itemTypeName(it.type);
    j["enabled"] = it.enabled;
    j["seconds"] = it.seconds;
    j["label"] = it.label;
    j["text"] = it.text;
  }
  o["notifyLow"] = c.notifyLow;
  o["notifyRefill"] = c.notifyRefill;
  o["ntfyServer"] = c.ntfyServer;
  o["ntfyTopic"] = c.ntfyTopic;
  o["power"] = c.power == PowerMode::DeepSleep ? "sleep" : "on";
}

bool configFromJson(JsonObjectConst in, Config& c, String& err) {
  take(in, "name", c.name);
  take(in, "tz", c.tz);
  take(in, "units", c.units);
  take(in, "emptyMm", c.emptyMm);
  take(in, "fullMm", c.fullMm);
  take(in, "lowPct", c.lowPct);
  take(in, "intervalMin", c.intervalMin);
  take(in, "samples", c.samples);
  take(in, "ledCurrent", c.ledCurrent);
  take(in, "integration", c.integration);
  take(in, "baseline", c.baseline);
  take(in, "color", c.color);
  take(in, "brightness", c.brightness);
  take(in, "awakeSec", c.awakeSec);
  take(in, "notifyLow", c.notifyLow);
  take(in, "notifyRefill", c.notifyRefill);
  take(in, "ntfyServer", c.ntfyServer);
  take(in, "ntfyTopic", c.ntfyTopic);
  if (!in["power"].isNull()) c.power = in["power"] == "sleep" ? PowerMode::DeepSleep : PowerMode::AlwaysOn;

  if (!in["cal"].isNull()) {
    c.cal.clear();
    for (JsonObjectConst p : in["cal"].as<JsonArrayConst>()) {
      if (c.cal.size() >= kMaxCalPoints) break;
      c.cal.push_back({p["raw"] | (uint16_t)0, p["mm"] | 0.0f});
    }
  }
  if (!in["items"].isNull()) {
    c.items.clear();
    for (JsonObjectConst j : in["items"].as<JsonArrayConst>()) {
      if (c.items.size() >= kMaxDisplayItems) break;
      DisplayItem it;
      if (!itemFromJson(j, it)) {
        err = "unknown display item type";
        return false;
      }
      c.items.push_back(it);
    }
  }

  if (c.name.isEmpty()) c.name = "SaltView";
  if (c.units != "cm") c.units = "in";
  bool knownColor = false;
  for (const char* name : kDisplayColors)
    if (c.color == name) knownColor = true;
  if (!knownColor) c.color = "red";
  if (!(c.emptyMm > c.fullMm && c.fullMm > 0)) {
    err = "empty distance must be greater than full distance";
    return false;
  }
  c.lowPct = constrain(c.lowPct, 1, 95);
  c.intervalMin = constrain(c.intervalMin, 5, 24 * 60);
  c.samples = constrain(c.samples, 1, 64);
  c.ledCurrent = constrain(c.ledCurrent, 0, 7);
  c.integration = constrain(c.integration, 0, 5);
  c.brightness = constrain(c.brightness, 0, 15);
  c.awakeSec = constrain(c.awakeSec, 5, 600);
  return true;
}

void configLoad() {
  configDefaults(gConfig);
  Preferences prefs;
  prefs.begin(kNamespace, true);
  String s = prefs.getString(kKey, "");
  prefs.end();
  if (s.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, s)) {
    Serial.println("[config] stored config unreadable, using defaults");
    return;
  }
  Config c = gConfig;
  String err;
  if (configFromJson(doc.as<JsonObjectConst>(), c, err)) gConfig = c;
  else Serial.printf("[config] stored config rejected: %s\n", err.c_str());
}

bool configSave() {
  JsonDocument doc;
  configToJson(gConfig, doc.to<JsonObject>());
  String s;
  serializeJson(doc, s);
  if (s.length() > 3900) return false;  // NVS string limit is ~4000 bytes
  Preferences prefs;
  prefs.begin(kNamespace, false);
  bool ok = prefs.putString(kKey, s) == s.length();
  prefs.end();
  return ok;
}
