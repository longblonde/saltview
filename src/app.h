#pragma once
// Shared application state, owned by main.cpp.
#include "display_manager.h"
#include "distance_sensor.h"

extern DistanceSensor gSensor;
extern DisplayManager gDisplay;
extern Reading gLast;
extern float gUsagePerDay;  // % of tank per day, NAN until there's a day of history
extern uint32_t gRestartAt;  // millis() deadline for a deferred restart, 0 = none

void takeReading(bool log);
void applyConfig();  // push gConfig into the sensor, display and clock
String hostname();
