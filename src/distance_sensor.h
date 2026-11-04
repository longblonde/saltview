#pragma once
#include <Adafruit_VCNL4200.h>
#include "config.h"

struct Reading {
  uint32_t ts = 0;       // unix time, 0 if the clock isn't set
  uint16_t raw = 0;      // averaged VCNL4200 proximity counts
  float distMm = NAN;    // sensor face -> salt surface
  float depthMm = NAN;   // salt height above the "empty" mark
  float pct = NAN;       // 0..100 between the empty and full marks
  bool valid() const { return !isnan(pct); }
};

// VCNL4200 proximity sensor used as a short-range distance sensor.
//
// The chip reports reflected-IR counts, not distance. Counts fall off roughly
// with 1/d^2, so we interpolate calibration points in x = 1/sqrt(counts - baseline),
// which is close to linear in distance. One point gives a pure inverse-square fit;
// 3-5 points spread over the tank's range give the best results.
class DistanceSensor {
 public:
  bool begin();
  void applySettings();
  bool ok() const { return _ok; }

  uint16_t readRaw(uint8_t samples);
  float rawToMm(uint16_t raw) const;  // NAN if uncalibrated
  Reading measure();

 private:
  Adafruit_VCNL4200 _vcnl;
  bool _ok = false;
};
