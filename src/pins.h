#pragma once
#include <Arduino.h>

// Adafruit ESP32-S2 Feather pin assignments. See docs/WIRING.md.
//
//   I2C SDA  GPIO3  (Feather "SDA")  -> Alphanumeric FeatherWing + VCNL4200
//   I2C SCL  GPIO4  (Feather "SCL")
//   PIR OUT  GPIO5  (Feather "D5")   RTC-capable, so it can wake from deep sleep
//   BOOT     GPIO0  hold 5 s to forget Wi-Fi and re-enter setup mode

constexpr uint8_t PIN_PIR = 5;
constexpr uint8_t PIN_BUTTON = 0;

// Powers the STEMMA QT port and I2C pull-ups on the ESP32-S2 Feather.
#ifndef PIN_I2C_POWER
#define PIN_I2C_POWER 7
#endif

constexpr uint8_t I2C_ADDR_ALNUM = 0x70;  // HT16K33 on the Quad Alphanumeric FeatherWing
constexpr uint8_t I2C_ADDR_VCNL = 0x51;   // VCNL4200 fixed address
