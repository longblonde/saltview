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

  void flash(const String& text, uint32_t ms);  // one-off message, then resume
  void showNow(const String& text);             // draw immediately (for blocking steps)
  void setOverride(const String& text) { _override = text; }  // replaces the rotation while set

  static String renderItem(const DisplayItem& it, const DisplayData& d);

  DisplayData data;

 private:
  void power(bool on);
  void drawText(const String& text, uint32_t elapsedMs);
  static uint32_t textDuration(const String& text);

  Adafruit_AlphaNum4 _alpha;
  bool _ok = false;
  bool _awake = false;
  bool _powered = false;
  uint32_t _wakeAt = 0;
  uint32_t _itemStart = 0;
  size_t _idx = 0;
  String _flashText;
  uint32_t _flashStart = 0, _flashUntil = 0;
  String _override;
  char _frame[9] = {0};  // last drawn chars + dots, to skip redundant I2C writes
  uint32_t _lastTick = 0;
};
