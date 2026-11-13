// SaltView - water softener brine tank salt level monitor
//
// Adafruit ESP32-S2 Feather + Quad Alphanumeric FeatherWing + VCNL4200 + HC-SR501 PIR.
// The VCNL4200 looks down at the salt from the tank lid; readings are logged to
// LittleFS and served on a local dashboard. The PIR wakes the display.

#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include <esp_sleep.h>
#include "app.h"
#include "config.h"
#include "history.h"
#include "motion.h"
#include "notifications.h"
#include "pins.h"
#include "web_server.h"
#include "wifi_manager.h"

static constexpr uint32_t kPirWarmupMs = 30000;        // HC-SR501 false-triggers right after power-up
static constexpr uint32_t kMaintenanceMs = 5 * 60000;  // stay awake after a reset, even in sleep mode
static constexpr uint32_t kForgetHoldMs = 5000;
static constexpr float kRefillJumpPct = 20;
static constexpr uint32_t kSplashMs = 900;
static constexpr uint32_t kSetupPauseMs = 800;

DistanceSensor gSensor;
DisplayManager gDisplay;
Reading gLast;
float gUsagePerDay = NAN;
uint32_t gRestartAt = 0;

// Survive deep sleep.
RTC_DATA_ATTR static uint32_t rtcLastLogTs = 0;
RTC_DATA_ATTR static float rtcPrevPct = NAN;
RTC_DATA_ATTR static bool rtcLowLatched = false;
RTC_DATA_ATTR static bool rtcSensorErr = false;

static bool sFromSleep = false;
static bool sOffline = false;  // PIR wake in sleep mode: display only, no Wi-Fi

static void enableI2CPower() {
  // The rest state of the I2C power pin differs between Feather revisions:
  // read it, then drive the opposite level to switch the STEMMA QT rail on.
  pinMode(PIN_I2C_POWER, INPUT);
  delay(1);
  bool rest = digitalRead(PIN_I2C_POWER);
  pinMode(PIN_I2C_POWER, OUTPUT);
  digitalWrite(PIN_I2C_POWER, !rest);
  delay(10);
}

String hostname() {
  String h;
  for (char c : gConfig.name) {
    if (isalnum(c)) h += (char)tolower(c);
    else if (!h.isEmpty() && h[h.length() - 1] != '-') h += '-';
  }
  return h.isEmpty() ? String("saltview") : h;
}

void applyConfig() {
  setenv("TZ", gConfig.tz.c_str(), 1);
  tzset();
  gSensor.applySettings();
  gDisplay.applySettings();
}

static bool logDue() {
  time_t t = time(nullptr);
  return t >= kValidEpoch && (uint32_t)t - rtcLastLogTs >= gConfig.intervalMin * 60UL;
}

static void evaluateAlerts(const Reading& r) {
  if (!gSensor.ok()) {
    if (!rtcSensorErr) Notify::post(Severity::Warn, "Distance sensor not responding - check the VCNL4200 cable");
    rtcSensorErr = true;
    return;
  }
  rtcSensorErr = false;
  if (!r.valid()) return;

  if (!rtcLowLatched && r.pct <= gConfig.lowPct) {
    rtcLowLatched = true;
    if (gConfig.notifyLow)
      Notify::post(Severity::Alert, "Salt is low (" + String((int)r.pct) + "%) - time to add salt");
  } else if (rtcLowLatched && r.pct >= gConfig.lowPct + 10) {
    rtcLowLatched = false;  // hysteresis so noise near the threshold doesn't re-alert
  }

  if (!isnan(rtcPrevPct) && r.pct - rtcPrevPct >= kRefillJumpPct && gConfig.notifyRefill)
    Notify::post(Severity::Info,
                 "Refill detected: " + String((int)rtcPrevPct) + "% -> " + String((int)r.pct) + "%");
  rtcPrevPct = r.pct;
}

void takeReading(bool log) {
  Reading r = gSensor.ok() ? gSensor.measure() : Reading();
  gLast = r;
  if (log) {
    time_t t = time(nullptr);
    rtcLastLogTs = t >= kValidEpoch ? (uint32_t)t : 0;
    History::append(r);
    gUsagePerDay = History::usagePctPerDay(14);
    evaluateAlerts(r);
  }
  DisplayData& d = gDisplay.data;
  d.reading = r;
  d.sensorOk = gSensor.ok();
  d.low = r.valid() && r.pct <= gConfig.lowPct;
  d.daysLeft = (r.valid() && gUsagePerDay > 0.05f) ? r.pct / gUsagePerDay : NAN;
}

// Setup mode: the heading scrolls full width, a pause, then the network to join, looping.
static void showSetupScreen() {
  String heading = WifiMgr::lastError().isEmpty() ? "WIFI SETUP" : "WIFI FAILED";
  gDisplay.setOverride({
      {heading, 0, true},
      {"", kSetupPauseMs},
      {"JOIN " + WifiMgr::apName(), 0, true},
      {"", kSetupPauseMs},
  });
  gDisplay.wake();
}

static void handleButton() {
  static uint32_t downAt = 0;
  bool down = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && !downAt) {
    downAt = now;
    gDisplay.wake();
  } else if (down && now - downAt >= kForgetHoldMs) {
    gDisplay.showNow("WIFI");
    WifiMgr::forget();
    delay(500);
    ESP.restart();
  } else if (!down) {
    downAt = 0;
  }
}

static void maybeDeepSleep() {
  if (gConfig.power != PowerMode::DeepSleep) return;
  if (!sFromSleep && millis() < kMaintenanceMs) return;
  if (WifiMgr::mode() == WifiMgr::Mode::Portal || gRestartAt) return;
  if (gDisplay.awake() || Motion::active() || Notify::pendingPushes()) return;
  if (!sOffline && logDue() && millis() < 30000) return;  // still waiting on NTP for the first log

  Serial.println("[power] deep sleep");
  Serial.flush();
  gDisplay.sleep();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_PIR, 1);
  esp_sleep_enable_timer_wakeup((uint64_t)gConfig.intervalMin * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  enableI2CPower();
  Wire.begin();
  if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS mount failed");
  History::begin();

  configLoad();
  applyConfig();
  Notify::begin();
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  sFromSleep = cause != ESP_SLEEP_WAKEUP_UNDEFINED;
  bool pirWake = cause == ESP_SLEEP_WAKEUP_EXT0;
  Motion::begin(PIN_PIR, sFromSleep ? 0 : kPirWarmupMs);

  gDisplay.begin();
  if (pirWake) {
    gDisplay.wake();
  } else if (!sFromSleep) {
    // Power-up splash; Wi-Fi setup below blocks, so draw these directly.
    gDisplay.showNow("SALT");
    delay(kSplashMs);
    gDisplay.showNow("VIEW");
    delay(kSplashMs);
  }

  gSensor.begin();
  gUsagePerDay = History::usagePctPerDay(14);
  takeReading(false);

  // In sleep mode a PIR wake just shows the display, unless a log is also due.
  sOffline = gConfig.power == PowerMode::DeepSleep && pirWake && !logDue();
  if (sOffline) return;

  // Printed late so the USB serial link is up: 1 power-on, 3 software restart, 4 crash, 5-7 watchdog.
  Serial.printf("[boot] reset reason %d\n", (int)esp_reset_reason());
  if (!gDisplay.awake()) gDisplay.showNow("WIFI");
  WifiMgr::begin(hostname());
  if (WifiMgr::mode() == WifiMgr::Mode::Portal) {
    showSetupScreen();
  } else {
    configTzTime(gConfig.tz.c_str(), "pool.ntp.org", "time.nist.gov");
    gDisplay.data.ip = WifiMgr::ip();
#ifdef SALTVIEW_FORCE_SETUP_SCREEN  // test builds: show the setup screen without leaving Wi-Fi
    showSetupScreen();
#else
    if (!sFromSleep) gDisplay.play(DisplayManager::ipCards(gDisplay.data.ip));  // regardless of item settings
    if (!gDisplay.awake()) gDisplay.sleep();
#endif
  }
  Web::begin();
}

void loop() {
  if (!sOffline) {
    WifiMgr::loop();
    Web::loop();
    Notify::loop();
  }
  handleButton();

  if (Motion::takeTriggered()) {
    if (!gDisplay.awake()) takeReading(false);  // fresh value for the display
    gDisplay.wake();
  }
  gDisplay.loop();

  if (logDue()) takeReading(true);
  if (gRestartAt && (int32_t)(millis() - gRestartAt) >= 0) ESP.restart();

  maybeDeepSleep();
  delay(5);
}
