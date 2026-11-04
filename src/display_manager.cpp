#include "display_manager.h"
#include <Wire.h>
#include <time.h>
#include "pins.h"

namespace {
constexpr uint32_t kScrollStepMs = 220;
constexpr uint32_t kScrollPauseMs = 700;
constexpr uint32_t kLabelMs = 900;
constexpr uint32_t kFrameMs = 30;

struct Cell {
  char c;
  bool dot;
};

// A '.' merges into the previous character's decimal point.
std::vector<Cell> toCells(const String& s) {
  std::vector<Cell> out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = toupper(s[i]);
    if (c == '.' && !out.empty() && !out.back().dot && out.back().c != ' ') {
      out.back().dot = true;
      continue;
    }
    out.push_back({c, false});
  }
  return out;
}
}  // namespace

bool DisplayManager::begin() {
  _ok = _alpha.begin(I2C_ADDR_ALNUM);
  if (!_ok) {
    Serial.println("[display] HT16K33 not found");
    return false;
  }
  applySettings();
  power(false);
  return true;
}

void DisplayManager::applySettings() {
  if (_ok) _alpha.setBrightness(gConfig.brightness);
}

// HT16K33 standby: oscillator off draws ~1 uA instead of several mA.
void DisplayManager::power(bool on) {
  if (!_ok || on == _powered) return;
  _powered = on;
  Wire.beginTransmission(I2C_ADDR_ALNUM);
  Wire.write(on ? 0x21 : 0x20);  // system setup: oscillator on/off
  Wire.endTransmission();
  if (on) {
    Wire.beginTransmission(I2C_ADDR_ALNUM);
    Wire.write(0x81);  // display on, no blink
    Wire.endTransmission();
    applySettings();
  } else {
    _alpha.clear();
    _alpha.writeDisplay();
    memset(_frame, 0, sizeof(_frame));
  }
}

void DisplayManager::wake() {
  if (!_ok) return;
  if (!_awake) {
    power(true);
    _awake = true;
    _idx = 0;
    _itemStart = millis();
  }
  _wakeAt = millis();
}

void DisplayManager::sleep() {
  _awake = false;
  if (!_flashUntil) power(false);
}

void DisplayManager::flash(const String& text, uint32_t ms) {
  if (!_ok) return;
  power(true);
  _flashText = text;
  _flashStart = millis();
  _flashUntil = _flashStart + max(ms, textDuration(text));
}

void DisplayManager::showNow(const String& text) {
  if (!_ok) return;
  power(true);
  drawText(text, 0);
}

uint32_t DisplayManager::textDuration(const String& text) {
  size_t n = toCells(text).size();
  return n <= 4 ? 0 : 2 * kScrollPauseMs + (n - 4) * kScrollStepMs;
}

void DisplayManager::drawText(const String& text, uint32_t elapsed) {
  auto cells = toCells(text);
  int n = cells.size();
  int offset = 0;
  if (n > 4) {
    long t = (long)elapsed - (long)kScrollPauseMs;
    offset = t <= 0 ? 0 : min<long>(n - 4, t / kScrollStepMs);
  }

  char frame[9];
  for (int i = 0; i < 4; i++) {
    int k = n <= 4 ? i - (4 - n) : i + offset;  // short text is right-aligned
    Cell c = (k >= 0 && k < n) ? cells[k] : Cell{' ', false};
    frame[i * 2] = c.c;
    frame[i * 2 + 1] = c.dot ? '.' : ' ';
  }
  frame[8] = 0;
  if (memcmp(frame, _frame, sizeof(frame)) == 0) return;
  memcpy(_frame, frame, sizeof(frame));
  for (int i = 0; i < 4; i++) _alpha.writeDigitAscii(i, frame[i * 2], frame[i * 2 + 1] == '.');
  _alpha.writeDisplay();
}

void DisplayManager::loop() {
  if (!_ok) return;
  uint32_t now = millis();
  if (now - _lastTick < kFrameMs) return;
  _lastTick = now;

  if (_flashUntil) {
    if ((int32_t)(now - _flashUntil) < 0) {
      drawText(_flashText, now - _flashStart);
      return;
    }
    _flashUntil = 0;
    if (!_awake) {
      power(false);
      return;
    }
    _itemStart = now;
  }

  if (!_awake) return;
  if (now - _wakeAt >= gConfig.awakeSec * 1000UL) {
    sleep();
    return;
  }

  if (!_override.isEmpty()) {
    uint32_t cycle = textDuration(_override) + 1000;
    drawText(_override, (now - _wakeAt) % cycle);
    return;
  }

  std::vector<size_t> enabled;
  for (size_t i = 0; i < gConfig.items.size(); i++)
    if (gConfig.items[i].enabled) enabled.push_back(i);
  if (enabled.empty()) {
    drawText(renderItem(DisplayItem(), data), 0);
    return;
  }
  if (_idx >= enabled.size()) _idx = 0;

  const DisplayItem& it = gConfig.items[enabled[_idx]];
  uint32_t el = now - _itemStart;
  uint32_t labelMs = it.label.isEmpty() ? 0 : kLabelMs;
  if (el < labelMs) {
    drawText(it.label, 0);
    return;
  }
  String value = renderItem(it, data);
  uint32_t valueMs = max<uint32_t>(it.seconds * 1000UL, textDuration(value));
  if (el - labelMs < valueMs) {
    drawText(value, el - labelMs);
    return;
  }
  _idx = (_idx + 1) % enabled.size();
  _itemStart = now;
}

String DisplayManager::renderItem(const DisplayItem& it, const DisplayData& d) {
  const Reading& r = d.reading;
  switch (it.type) {
    case ItemType::Level:
      return r.valid() ? String((int)lroundf(r.pct)) + "%" : String("--%");
    case ItemType::Depth: {
      if (!r.valid()) return "----";
      if (gConfig.units == "cm") return String((int)lroundf(r.depthMm / 10.0f)) + "CM";
      float in = r.depthMm / 25.4f;
      return (in < 100 ? String(in, 1) : String((int)lroundf(in))) + "\"";
    }
    case ItemType::DaysLeft: {
      if (isnan(d.daysLeft)) return "--D";
      long days = lroundf(d.daysLeft);
      return String(days > 999 ? 999 : days) + "D";
    }
    case ItemType::Status:
      if (!d.sensorOk) return "ERR";
      if (!r.valid()) return "CAL?";
      return d.low ? "LOW" : "OK";
    case ItemType::Clock: {
      time_t t = time(nullptr);
      if (t < kValidEpoch) return "--.--";
      struct tm tm;
      localtime_r(&t, &tm);
      char buf[8];
      snprintf(buf, sizeof(buf), "%2d.%02d", tm.tm_hour, tm.tm_min);
      return buf;
    }
    case ItemType::Ip:
      return d.ip.isEmpty() ? String("NO WIFI") : "IP " + d.ip;
    case ItemType::Custom:
      return it.text;
    default:
      return "";
  }
}
