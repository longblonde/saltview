#include "history.h"
#include <LittleFS.h>
#include <algorithm>

namespace History {
static const char* kPath = "/history.csv";
static const char* kTmpPath = "/history.tmp";
static constexpr size_t kMaxBytes = 384 * 1024;
static constexpr float kNoisePct = 1.0f;    // drops smaller than this are sensor noise
static constexpr float kRefillPct = 10.0f;  // rises larger than this are a refill

// Copies the newer half to a temp file, then renames it over the log. LittleFS
// renames atomically, so after a power cut the log is either the old file or
// the trimmed one; begin() cleans up whatever temp file is left behind.
static void trim() {
  File in = LittleFS.open(kPath, "r");
  File out = LittleFS.open(kTmpPath, "w");
  if (!in || !out) return;
  in.seek(in.size() / 2);
  in.readStringUntil('\n');  // realign to a row boundary
  uint8_t buf[512];
  bool ok = true;
  while (size_t n = in.read(buf, sizeof(buf))) {
    if (out.write(buf, n) != n) {
      ok = false;  // flash full or failing: keep the untrimmed log
      break;
    }
  }
  in.close();
  out.close();
  if (!ok || !LittleFS.rename(kTmpPath, kPath)) LittleFS.remove(kTmpPath);
}

void begin() {
  if (!LittleFS.exists(kTmpPath)) return;
  if (LittleFS.exists(kPath)) {
    LittleFS.remove(kTmpPath);  // trim was interrupted before the rename; the log is intact
  } else {
    LittleFS.rename(kTmpPath, kPath);  // only the trimmed copy survived
  }
}

void append(const Reading& r) {
  if (!r.ts || !r.valid()) return;
  File f = LittleFS.open(kPath, FILE_APPEND);
  if (!f) return;
  f.printf("%lu,%u,%.0f,%.0f,%.1f\n", (unsigned long)r.ts, r.raw, r.distMm, r.depthMm, r.pct);
  size_t size = f.size();
  f.close();
  if (size > kMaxBytes) trim();
}

void forEach(uint32_t since, const std::function<void(const Row&)>& fn) {
  File f = LittleFS.open(kPath, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    unsigned long ts;
    unsigned raw;
    Row r;
    if (sscanf(line.c_str(), "%lu,%u,%f,%f,%f", &ts, &raw, &r.distMm, &r.depthMm, &r.pct) != 5) continue;
    if (ts < since) continue;
    r.ts = ts;
    r.raw = raw;
    fn(r);
  }
}

float usagePctPerDay(uint16_t days) {
  time_t now = time(nullptr);
  if (now < kValidEpoch) return NAN;
  float ref = NAN, used = 0;
  uint32_t first = 0, last = 0;
  forEach(now - days * 86400UL, [&](const Row& r) {
    if (!first) first = r.ts;
    last = r.ts;
    if (isnan(ref)) {
      ref = r.pct;
    } else if (r.pct < ref - kNoisePct) {
      used += ref - r.pct;
      ref = r.pct;
    } else if (r.pct > ref + kRefillPct) {
      ref = r.pct;
    }
  });
  float spanDays = (last - first) / 86400.0f;
  return spanDays < 1.0f ? NAN : used / spanDays;
}

bool last(Row& out) {
  bool found = false;
  forEach(0, [&](const Row& r) {
    out = r;
    found = true;
  });
  return found;
}

// Rewrites the log without the given rows, via the same temp-file-and-rename as trim(),
// so a power cut leaves either the old log or the new one.
size_t remove(const std::vector<uint32_t>& timestamps) {
  if (timestamps.empty()) return 0;
  File in = LittleFS.open(kPath, "r");
  if (!in) return 0;
  File out = LittleFS.open(kTmpPath, "w");
  if (!out) return 0;
  size_t removed = 0;
  bool ok = true;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    if (line.isEmpty()) continue;
    uint32_t ts = strtoul(line.c_str(), nullptr, 10);
    if (std::find(timestamps.begin(), timestamps.end(), ts) != timestamps.end()) {
      removed++;
      continue;
    }
    line += '\n';
    if (out.print(line) != line.length()) {
      ok = false;
      break;
    }
  }
  in.close();
  out.close();
  if (!ok || !removed || !LittleFS.rename(kTmpPath, kPath)) {
    LittleFS.remove(kTmpPath);  // nothing matched, or the flash write failed: keep the log as it was
    return 0;
  }
  return removed;
}

void clear() { LittleFS.remove(kPath); }
}  // namespace History
