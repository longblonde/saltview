# SaltView

Salt level monitor for a water softener brine tank.

An Adafruit ESP32-S2 Feather reads a VCNL4200 IR proximity sensor mounted in the tank lid, logs
the salt depth over time, and serves a dashboard on your home network. An HC-SR501 PIR wakes a
quad alphanumeric display when someone walks up, cycles through the items you choose, and turns it
off again.

| | |
|---|---|
| **Board** | Adafruit ESP32-S2 Feather |
| **Display** | Adafruit Quad Alphanumeric FeatherWing (HT16K33) |
| **Distance** | Adafruit VCNL4200 (I2C, STEMMA QT) |
| **Motion** | HC-SR501 PIR |
| **Wiring** | [docs/WIRING.md](docs/WIRING.md) |

## Features (skeleton)

- **Wi-Fi setup**: first boot (or BOOT held 5 s) starts an open access point `SaltView-XXXX` with a
  captive portal. Pick your network and the device reboots onto it. The portal page is compiled
  into the firmware, so it works before the dashboard files are uploaded.
- **Dashboard** (`http://saltview.local`): level, depth, estimated days left, usage per day,
  level-over-time chart (7 d / 30 d / 90 d / 1 y) with a selectable dot per measurement and its
  details underneath, daily usage bars, notification feed.
- **Measurements** (`log.html`, linked from Setup → Measurement): the level graph above a table of
  every reading, with the change from the previous one (+ green / − red). Select readings by dot or
  by row; likely-bad ones (closer than the sensor can see, or a sudden drop) are flagged, and any
  selection can be deleted.
- **Distance sensing**: trimmed-mean of N samples, converted to distance with a multi-point
  calibration curve (see below), then to % between your tank's empty and full marks.
- **Calibration / setup page**: live raw readings, capture calibration points, set empty/full marks
  from the live reading, low-salt threshold, logging interval, sensor LED/integration, time zone,
  units, ntfy push settings, power mode.
- **Display options page**: add, remove, reorder, enable/disable rotation items (level, depth, days
  left, status, clock, IP, custom text, live sensor), per-item label and duration, brightness,
  stay-on time, and a live 14-segment preview that renders through the firmware's own formatter, in
  the LED colour of your FeatherWing (white, green, red, blue, yellow-green or yellow). Motion only
  wakes the display: every item shows the newest logged measurement, except *Live sensor*, which
  reads raw counts twice a second for troubleshooting (above 9999 it rolls over: 12345 shows 2345).
- **Notifications**: low salt (with hysteresis), refill detected, sensor fault. Shown on the
  dashboard and optionally pushed to your phone via [ntfy](https://ntfy.sh).
- **Power modes**: *Always on* (USB, default) or *Deep sleep* (wakes on PIR or the log timer; stays
  awake for 5 min after a reset so you can reach the dashboard).

## Build and flash

Uses [PlatformIO](https://platformio.org) (VS Code extension or CLI).

```bash
pio run -t upload
```

```bash
pio run -t uploadfs
```

`upload` flashes the firmware; `uploadfs` flashes the dashboard in `data/` to LittleFS. If the
upload can't find the board, hold **BOOT**, tap **RESET**, release **BOOT**, and retry (the S2's ROM
bootloader). Afterwards press RESET to run.

The project uses the standard 4 MB partition table, which replaces the Feather's TinyUF2
drag-and-drop bootloader partition. Settings are stored in NVS, so `uploadfs` doesn't erase them,
but it does erase the reading history.

## First-time setup

1. Power up. The display scrolls `SETUP - JOIN SaltView-XXXX`.
2. Join that network from your phone; the setup page opens (or browse to `http://192.168.4.1`).
3. Pick your Wi-Fi and enter the password. The device reboots and joins it.
4. Open `http://saltview.local` (or the IP shown by the display's *IP* item / your router).
5. Go to **Setup** and calibrate (below), then set the empty and full marks.

## Calibration

The VCNL4200 reports reflected-IR counts, not distance. Counts drop roughly with the square of
distance, so the firmware interpolates calibration points in `1/sqrt(counts - baseline)`, which is
close to linear in distance.

1. *(Optional)* Point the sensor at nothing within ~1.5 m and press **Capture baseline**. Do this
   if the sensor sits behind a window or lid that reflects some IR.
2. Mount the sensor in its final position. Hold a sheet of white card (or let the salt itself
   serve) at a known distance, measure with a tape, enter it, and press **Capture point**.
3. Repeat at 3–5 distances spanning your tank, from full height down to near the bottom.
4. Set **Empty**: the distance from the sensor to the lowest useful salt level (tank floor or grid
   plate). Set **Full**: the distance to the salt surface right after you fill it. Either type
   them in or press **Use live** with the salt at that level.

Changing LED current or integration time changes the counts, so recalibrate afterwards.

## Project layout

```
platformio.ini
src/
  main.cpp              setup/loop, reading schedule, alerts, deep sleep
  pins.h                pin assignments
  config.*              settings struct, JSON (de)serialisation, NVS storage
  distance_sensor.*     VCNL4200 driver wrapper + calibration curve
  display_manager.*     HT16K33 rotation, scrolling, standby
  motion.*              PIR interrupt
  history.*             LittleFS CSV log, usage-rate estimate
  notifications.*       notification feed + ntfy push
  wifi_manager.*        station / captive-portal setup AP
  web_server.*          HTTP routes
  portal_html.h         Wi-Fi setup page (embedded)
data/                   dashboard (LittleFS): index, setup, display pages + app.js/app.css
docs/                   wiring diagram and pinouts
```

## HTTP API

| Method | Path | Body / query | Purpose |
|---|---|---|---|
| GET | `/api/status` | | Latest reading, usage, days left, network, unread count |
| GET | `/api/live` | | Unlogged reading (raw, distMm, pct) for calibration |
| POST | `/api/measure` | `{}` | Take and log a reading now |
| GET | `/api/history` | `?days=30` | `[[ts, pct, depthMm, raw, distMm], ...]` |
| POST | `/api/history/delete` | `{ts: [unixSeconds, ...]}` | Delete those readings, returns `{removed}` |
| POST | `/api/history/clear` | `{}` | Delete the log |
| GET / POST | `/api/config` | partial config JSON | Read / update settings |
| POST | `/api/calibrate` | `{action: capture\|remove\|clear\|baseline\|setEmpty\|setFull, mm?, index?}` | Calibration |
| POST | `/api/display/render` | `{items?}` | Render display items with live data |
| POST | `/api/display/wake` | `{}` | Turn the display on |
| GET / POST | `/api/notifications` | `{action: read\|clear\|test, id?}` | Notification feed |
| GET | `/api/wifi/scan` | | Async network scan |
| POST | `/api/wifi` | `{ssid, pass}` | Save credentials, reboot |
| POST | `/api/wifi/forget` | `{}` | Forget network, reboot into setup |
| POST | `/api/reboot` | `{}` | Reboot |

## Not yet done

- Dashboard has no authentication; keep it on a trusted network.
- ntfy pushes over HTTPS skip certificate verification (`setInsecure`).
- Reading history is lost on `uploadfs`, and isn't exported anywhere off-device.
- No OTA firmware updates yet (the partition table already has two app slots).
- Deep-sleep mode is lightly exercised; battery monitoring (the Feather's fuel gauge) isn't read.
