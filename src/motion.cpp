#include "motion.h"

namespace Motion {
static uint8_t sPin;
static uint32_t sWarmupUntil;
static volatile bool sTriggered = false;

static void IRAM_ATTR onRise() { sTriggered = true; }

void begin(uint8_t pin, uint32_t warmupMs) {
  sPin = pin;
  sWarmupUntil = millis() + warmupMs;
  pinMode(pin, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(pin), onRise, RISING);
}

bool takeTriggered() {
  if (!sTriggered) return false;
  sTriggered = false;
  return (int32_t)(millis() - sWarmupUntil) >= 0;
}

bool active() { return digitalRead(sPin) == HIGH; }
}  // namespace Motion
