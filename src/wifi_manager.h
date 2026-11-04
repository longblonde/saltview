#pragma once
#include <Arduino.h>

// Station mode with saved credentials; falls back to an open setup access
// point ("SaltView-XXXX") with a captive portal when none are saved or the
// network can't be joined.
namespace WifiMgr {
enum class Mode { Connecting, Station, Portal };

void begin(const String& hostname);
void loop();
Mode mode();
String ip();
String apName();
String savedSsid();
String lastError();  // why the saved network couldn't be joined, empty if it was
String scanJson();  // {"scanning":true} until an async scan finishes

void saveCredentials(const String& ssid, const String& pass);
void forget();
}  // namespace WifiMgr
