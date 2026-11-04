#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

constexpr const char* kFwVersion = "0.1.0";
constexpr time_t kValidEpoch = 1700000000;  // anything earlier means NTP hasn't synced
constexpr size_t kMaxCalPoints = 10;
constexpr size_t kMaxDisplayItems = 12;
// The Quad Alphanumeric FeatherWing is sold in these LED colours.
constexpr const char* kDisplayColors[] = {"white", "green", "red", "blue", "yellowgreen", "yellow"};

// Things the quad display can show in its rotation.
enum class ItemType : uint8_t { Level, Depth, DaysLeft, Status, Clock, Ip, Custom, COUNT };
const char* itemTypeName(ItemType t);
bool itemTypeFromName(const char* s, ItemType& out);

struct DisplayItem {
  ItemType type = ItemType::Level;
  bool enabled = true;
  uint16_t seconds = 3;  // how long the value stays up (long text scrolls at least once)
  String label;          // optional caption (<= 4 chars) flashed before the value
  String text;           // Custom only
};

// One calibration sample: raw VCNL4200 proximity counts at a known distance.
struct CalPoint {
  uint16_t raw;
  float mm;
};

enum class PowerMode : uint8_t { AlwaysOn, DeepSleep };

struct Config {
  String name = "SaltView";
  String tz = "EST5EDT,M3.2.0,M11.1.0";  // POSIX TZ string
  String units = "in";                   // "in" | "cm" (display + UI only; storage is mm)

  // Tank geometry, as distances from the sensor face (mm).
  float emptyMm = 900;  // sensor -> lowest useful salt level (tank floor / grid plate)
  float fullMm = 250;   // sensor -> salt surface when the tank is full
  uint8_t lowPct = 25;  // low-salt alert threshold

  // Sensor
  uint16_t intervalMin = 720;  // logging interval; twice a day suits a softener that regenerates daily
  uint8_t samples = 16;       // samples averaged per reading
  uint8_t ledCurrent = 7;     // vcnl4200_led_i_t (7 = 200 mA)
  uint8_t integration = 5;    // vcnl4200_ps_it_t (5 = 9T)
  uint16_t baseline = 0;      // counts with no target in range (crosstalk / cover glass)
  std::vector<CalPoint> cal;

  // Display
  String color = "red";     // LED colour of the FeatherWing you bought (dashboard preview only)
  uint8_t brightness = 8;   // 0..15
  uint16_t awakeSec = 30;   // how long the display stays on after motion
  uint16_t blankMs = 300;   // blank gap between rotation items
  uint16_t scrollMs = 250;  // custom text scroll speed, ms per character
  std::vector<DisplayItem> items;

  // Notifications
  bool notifyLow = true;
  bool notifyRefill = true;
  String ntfyServer = "https://ntfy.sh";
  String ntfyTopic;  // empty = dashboard only, no phone push

  PowerMode power = PowerMode::AlwaysOn;
};

extern Config gConfig;

void configDefaults(Config& c);
void configLoad();
bool configSave();
void configToJson(const Config& c, JsonObject out);
// Applies only the keys present in `in`; validates the result.
bool configFromJson(JsonObjectConst in, Config& c, String& err);
bool itemFromJson(JsonObjectConst in, DisplayItem& it);
