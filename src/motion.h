#pragma once
#include <Arduino.h>

// HC-SR501 PIR. Output goes high (3.3 V) on motion; edge is latched by an ISR.
namespace Motion {
void begin(uint8_t pin, uint32_t warmupMs);  // ignore triggers while the PIR settles
bool takeTriggered();                        // rising edge since last call
bool active();                               // output currently high
}  // namespace Motion
