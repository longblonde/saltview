#pragma once
#include <ArduinoJson.h>

enum class Severity : uint8_t { Info, Warn, Alert };

// Dashboard notification feed (persisted to /notes.json), optionally pushed to
// a phone through ntfy (https://ntfy.sh or a self-hosted server).
namespace Notify {
void begin();
void loop();  // sends queued pushes once Wi-Fi is up
void post(Severity sev, const String& msg, bool push = true);
void markRead(uint32_t id);  // 0 = all
void clear();
uint8_t unread();
size_t pendingPushes();
void toJson(JsonArray out);
}  // namespace Notify
