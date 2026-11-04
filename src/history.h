#pragma once
#include <functional>
#include "distance_sensor.h"

// Append-only reading log in LittleFS (/history.csv), trimmed by half when it
// grows past ~384 KB (~10k rows, over 10 years at the default twice-daily interval).
namespace History {
struct Row {
  uint32_t ts;
  uint16_t raw;
  float distMm, depthMm, pct;
};

void begin();  // recovers from a trim interrupted by power loss; call after LittleFS mounts
void append(const Reading& r);  // skipped if the reading is invalid or has no timestamp
void forEach(uint32_t since, const std::function<void(const Row&)>& fn);
// Average salt consumption (% of tank per day) over the last `days`, ignoring refills.
float usagePctPerDay(uint16_t days);
void clear();
}  // namespace History
