#pragma once
#include <Adafruit_LEDBackpack.h>
#include "config.h"
#include "distance_sensor.h"

// Values the display items render from; refreshed by main after each reading.
struct DisplayData {
  Reading reading;
  float daysLeft = NAN;
  bool low = false;
  bool sensorOk = true;
  String ip;
};

// Drives the Quad Alphanumeric FeatherWing. Off (HT16K33 standby) until woken by
// motion, then cycles through gConfig.items for gConfig.awakeSec seconds.
class DisplayManager {
 public:
  bool begin();
  void applySettings();
  void loop();

  void wake();  // turn on / extend the awake window
  void sleep();
  bool awake() const { return _awake; }

  // One screen of text; text longer than 4 characters scrolls, extending `ms` if needed.
  // A marquee card scrolls in from the right and out the left at gConfig.scrollMs per
  // character, and lasts exactly that long (`ms` is ignored).
  struct Card {
    String text;
    uint32_t ms;
    bool marquee = false;
  };

  void play(const std::vector<Card>& cards);  // one-off cards (even while asleep), then resume
  void flash(const String& text, uint32_t ms) { play({{text, ms}}); }
  void showNow(const String& text);  // draw immediately (for blocking steps)
  // Cards that loop in place of the rotation while set (e.g. the Wi-Fi setup screen).
  void setOverride(const std::vector<Card>& cards) { _override = cards; }

  // The cards a rotation item shows: optional label, then its value(s).
  static std::vector<Card> itemCards(const DisplayItem& it, const DisplayData& d);
  // "IP", then one octet per card ("192." "168." "1." "232"), each shown for `ms`.
  static std::vector<Card> ipCards(const String& ip, uint32_t ms = 1000);
  static String renderItem(const DisplayItem& it, const DisplayData& d);
  static uint32_t cardMs(const Card& c);

  DisplayData data;

 private:
  void power(bool on);
  void drawText(const String& text, uint32_t elapsedMs, bool marquee = false);
  bool flashing() const { return _flashIdx < _flash.size(); }
  static uint32_t textDuration(const String& text);

  Adafruit_AlphaNum4 _alpha;
  bool _ok = false;
  bool _awake = false;
  bool _powered = false;
  uint32_t _wakeAt = 0;
  uint32_t _itemStart = 0;
  size_t _idx = 0;
  std::vector<Card> _flash;
  size_t _flashIdx = 0;
  uint32_t _flashStart = 0;
  std::vector<Card> _override;
  char _frame[9] = {0};  // last drawn chars + dots, to skip redundant I2C writes
  uint32_t _lastTick = 0;
};
