#include "display_manager.h"
#include <Wire.h>
#include <time.h>
#include "pins.h"

namespace {
constexpr uint32_t kScrollStepMs = 220;
constexpr uint32_t kScrollPauseMs = 700;
constexpr uint32_t kLabelMs = 900;
constexpr uint32_t kOctetMs = 1000;
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
  if (!flashing()) power(false);
}

void DisplayManager::play(const std::vector<Card>& cards) {
  if (!_ok || cards.empty()) return;
  power(true);
  _flash = cards;
  _flashIdx = 0;
  _flashStart = millis();
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

// A marquee runs from blank (text entering on the right) to blank (text gone off the left).
static String marqueeText(const String& text) { return "    " + text + "    "; }

uint32_t DisplayManager::cardMs(const Card& c) {
  if (c.marquee) return (toCells(marqueeText(c.text)).size() - 4) * gConfig.scrollMs;
  return max(c.ms, textDuration(c.text));
}

std::vector<DisplayManager::Card> DisplayManager::ipCards(const String& ip) {
  std::vector<Card> cards{{"IP", kLabelMs}};
  int start = 0;
  for (int dot; (dot = ip.indexOf('.', start)) >= 0; start = dot + 1)
    cards.push_back({ip.substring(start, dot + 1), kOctetMs});  // keep the '.' as a decimal point
  cards.push_back({ip.substring(start), kOctetMs});
  return cards;
}

std::vector<DisplayManager::Card> DisplayManager::itemCards(const DisplayItem& it, const DisplayData& d) {
  std::vector<Card> cards;
  if (!it.label.isEmpty()) cards.push_back({it.label, kLabelMs});
  if (it.type == ItemType::Ip && !d.ip.isEmpty()) {
    auto ip = ipCards(d.ip);
    if (!it.label.isEmpty()) ip.erase(ip.begin());  // a custom label replaces the "IP" card
    cards.insert(cards.end(), ip.begin(), ip.end());
  } else if (it.type == ItemType::Custom) {
    cards.push_back({it.text, 0, true});  // duration is the scroll itself, not it.seconds
  } else {
    cards.push_back({renderItem(it, d), it.seconds * 1000UL});
  }
  return cards;
}

void DisplayManager::drawText(const String& text, uint32_t elapsed, bool marquee) {
  auto cells = toCells(marquee ? marqueeText(text) : text);
  int n = cells.size();
  int offset = 0;
  if (marquee) {
    offset = min<long>(n - 4, elapsed / gConfig.scrollMs);
  } else if (n > 4) {
    long t = (long)elapsed - (long)kScrollPauseMs;
    offset = t <= 0 ? 0 : min<long>(n - 4, t / kScrollStepMs);
  }

  char frame[9];
  for (int i = 0; i < 4; i++) {
    int k = (n <= 4 && !marquee) ? i - (4 - n) : i + offset;  // short text is right-aligned
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

  if (flashing()) {
    const Card& card = _flash[_flashIdx];
    if (now - _flashStart < cardMs(card)) {
      drawText(card.text, now - _flashStart, card.marquee);
      return;
    }
    _flashIdx++;
    _flashStart = now;
    if (flashing()) return;
    _flash.clear();
    _flashIdx = 0;
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

  uint32_t el = now - _itemStart;
  for (const Card& card : itemCards(gConfig.items[enabled[_idx]], data)) {
    uint32_t ms = cardMs(card);
    if (el < ms) {
      drawText(card.text, el, card.marquee);
      return;
    }
    el -= ms;
  }
  if (el < gConfig.blankMs) {  // gap so consecutive items read as separate
    drawText("", 0);
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
      return d.ip.isEmpty() ? String("NO WIFI") : d.ip;  // itemCards() splits it into octets
    case ItemType::Custom:
      return it.text;
    default:
      return "";
  }
}
