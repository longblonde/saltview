#include "distance_sensor.h"
#include <algorithm>
#include "pins.h"

bool DistanceSensor::begin() {
  _ok = _vcnl.begin(I2C_ADDR_VCNL);
  if (!_ok) {
    Serial.println("[sensor] VCNL4200 not found");
    return false;
  }
  _vcnl.setALSshutdown(true);  // ambient light channel unused
  _vcnl.setProxShutdown(false);
  _vcnl.setProxHD(true);  // 16-bit proximity output
  _vcnl.setProxMultiPulse(VCNL4200_PS_MPS_8);
  _vcnl.setProxSunCancelEnable(true);
  _vcnl.setProxActiveForce(true);  // measure only when triggered
  applySettings();
  return true;
}

void DistanceSensor::applySettings() {
  if (!_ok) return;
  _vcnl.setProxLEDCurrent((vcnl4200_led_i_t)gConfig.ledCurrent);
  _vcnl.setProxIntegrationTime((vcnl4200_ps_it_t)gConfig.integration);
}

uint16_t DistanceSensor::readRaw(uint8_t samples) {
  if (!_ok) return 0;
  samples = constrain(samples, 1, 64);
  uint16_t buf[64];
  for (uint8_t i = 0; i < samples; i++) {
    _vcnl.triggerProx();
    delay(8);
    buf[i] = _vcnl.readProxData();
  }
  // Trimmed mean: drop the top and bottom quarter to reject glints and splashes.
  std::sort(buf, buf + samples);
  uint8_t lo = samples / 4, hi = samples - samples / 4;
  uint32_t sum = 0;
  for (uint8_t i = lo; i < hi; i++) sum += buf[i];
  return sum / (hi - lo);
}

float DistanceSensor::rawToMm(uint16_t raw) const {
  const auto& cal = gConfig.cal;
  if (cal.empty()) return NAN;

  const float baseline = gConfig.baseline;
  auto x = [baseline](uint16_t r) {
    float v = (float)r - baseline;
    return 1.0f / sqrtf(v < 1.0f ? 1.0f : v);
  };

  std::vector<CalPoint> pts = cal;
  std::sort(pts.begin(), pts.end(), [&](const CalPoint& a, const CalPoint& b) { return x(a.raw) < x(b.raw); });

  float xi = x(raw);
  if (pts.size() == 1) return pts[0].mm * xi / x(pts[0].raw);

  // Piecewise linear in x, extrapolating from the end segments.
  size_t i = 1;
  while (i < pts.size() - 1 && xi > x(pts[i].raw)) i++;
  const CalPoint& a = pts[i - 1];
  const CalPoint& b = pts[i];
  float xa = x(a.raw), xb = x(b.raw);
  if (fabsf(xb - xa) < 1e-9f) return a.mm;
  return a.mm + (xi - xa) * (b.mm - a.mm) / (xb - xa);
}

Reading DistanceSensor::measure() {
  Reading r;
  time_t now = time(nullptr);
  r.ts = now >= kValidEpoch ? (uint32_t)now : 0;
  r.raw = readRaw(gConfig.samples);
  r.distMm = rawToMm(r.raw);
  if (isnan(r.distMm)) return r;

  float span = gConfig.emptyMm - gConfig.fullMm;
  r.depthMm = max(0.0f, gConfig.emptyMm - r.distMm);
  r.pct = constrain((gConfig.emptyMm - r.distMm) / span * 100.0f, 0.0f, 100.0f);
  return r;
}
