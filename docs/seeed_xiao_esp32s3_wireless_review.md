# Seeed XIAO ESP32-S3 Wireless Review — BLE & WiFi-AP vs Seeed Studio Documentation

**Target:** `firmware/canary/` (modular PlatformIO build) and
`firmware/projects/canary-wap/` — Seeed Studio XIAO ESP32-S3 (Sense),
Arduino framework, NimBLE-Arduino 2.x.
**Question reviewed:** *Do our Bluetooth and 2.4 GHz WiFi Access Point ("WAP")
implementations follow Seeed Studio's documented usage and best practices for
the XIAO ESP32-S3?*
**Companion doc:** [`esp32s3_ble_wap_audit.md`](./esp32s3_ble_wap_audit.md)
covers the Espressif-side coexistence audit (F1–F7, all fixed). This review is
the Seeed-specific complement.

## Seeed sources

- WiFi usage — <https://wiki.seeedstudio.com/wifi_usage/>
- Bluetooth usage — <https://wiki.seeedstudio.com/xiao_esp32s3_bluetooth/>
- Getting started — <https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/>

(Wiki pages verified against the markdown sources in
[Seeed-Studio/wiki-documents](https://github.com/Seeed-Studio/wiki-documents).)

## TL;DR

The implementation is **compliant with Seeed's documented usage on every
functional point** — SoftAP parameters, WPA2 password rules, BLE-only stack
(no Classic on the S3), Arduino core 3.x API level, OPI-PSRAM build settings,
sleep/TX-power plumbing. Four gaps were found and fixed on this branch:

1. **Docs**: the prior audit claimed the S3 has a usable onboard antenna — it
   does not. The XIAO ESP32-S3 is U.FL-only and ships with a rod antenna that
   **must be installed**.
2. **Code**: the modular canary never called `WiFi.persistent(false)` (the
   canary-wap sketch already did), so the Arduino core silently mirrored WiFi
   credentials into the SDK's own NVS on every connect.
3. **Code**: BLE TX power diverged between variants (+3 dBm modular vs +9 dBm
   canary-wap). Now unified via `BLE_TX_POWER_DBM` (default +9).
4. **Code comment**: `network_set_tx_power()`'s parameter was named `dbm` but
   takes quarter-dBm units; renamed to `quarter_dbm`.

## Compliance matrix

| Area | Our implementation | Seeed guidance | Verdict |
|---|---|---|---|
| SoftAP call | `WiFi.softAP(ssid, pass, AP_CHANNEL=1, hidden=false, AP_MAX_CONNECTIONS=1)` (`securacv_network.cpp`) | `WiFi.softAP(ssid, password, channel, ssid_hidden, max_connection, ftm_responder)`; channels 1–13; 1–4 clients | ✅ |
| AP password | `cv-` + 5 unambiguous chars = 8 chars, device-unique from Ed25519 pubkey fingerprint (`main.cpp`) | WPA2 passphrase min 8 chars | ✅ (exactly at the minimum — by design, printed on serial/sticker) |
| 2.4 GHz only | AP ch 1, no 5 GHz assumptions anywhere | "2.4GHz WiFi — 802.11 b/g/n" only | ✅ |
| WiFi mode discipline | `WIFI_AP_STA` at provisioning, dropped to `WIFI_STA` once the home link holds (F4) | `WiFi.mode()` must be set appropriately before operations | ✅ (goes beyond Seeed: avoids Espressif's unstable AP+client+BLE combo) |
| Bluetooth Classic | Not used anywhere | "The ESP32-S3 has no Bluetooth Classic hardware in the chip" | ✅ |
| BLE library | NimBLE-Arduino 2.3.8 (`[env:full]`) | Examples use bluedroid `BLEDevice.h`; NimBLE-Arduino named as the lighter alternative | ✅ (NimBLE is the better of Seeed's two options; ~half the flash/RAM) |
| Core 3.x API level | NimBLE 2.x `NimBLEConnInfo&` callbacks, pioarduino core 3.3.8 / IDF 5.5.4 | Wiki flags breaking changes for Arduino-ESP32 ≥ 3.0.0 | ✅ |
| BLE scan params | Passive scan, interval 320 units (200 ms), window 160 units (100 ms) (`ble_scout_nimble.cpp`) | Example: `setInterval(100); setWindow(99)` (window ≤ interval); "active scan uses more power" | ✅ (window ≤ interval respected; passive chosen deliberately for privacy + power) |
| MTU | 247 (single LE Data Length Extension packet, no fragmentation) | Suggests `setMTU(517)` for max throughput | ✅ deliberate tradeoff — status reads are ≤244 B; documented at the call site |
| Notify cadence | 5 s status update interval | "delay(10) — bluetooth stack will go into congestion if too many packets are sent" | ✅ (far below congestion territory) |
| PSRAM / IDE settings | `board_build.arduino.memory_type = qio_opi`, `-DBOARD_HAS_PSRAM`, USB CDC on boot (`platformio.ini`) | PSRAM must be set to **OPI**; board package ≥ 2.0.8 | ✅ |
| WiFi sleep | `esp_wifi_set_ps(WIFI_PS_MIN_MODEM / NONE)` exposed via `network_set_wifi_power_save()` | `WiFi.setSleep()` with PS_NONE / MIN_MODEM / MAX_MODEM | ✅ (MAX_MODEM intentionally not offered — DTIM latency unacceptable for an HTTP-serving device) |
| WiFi TX power | `network_set_tx_power()` clamped to 8..84 quarter-dBm (2–21 dBm) | "configure transmit power to 15 dBm via `esp_wifi_set_max_tx_power()`" for weak-signal units | ✅ plumbing matches; 15 dBm = value 60 documented in the header |
| Power budget | Brownout reset-reason logging (F7) + `esp32s3_power_resilience.md` (≥500 mA supply guidance) | WiFi active ~100 mA, BLE active ~85 mA, modem-sleep 27 mA, deep sleep 14 µA | ✅ consistent |
| Persistent WiFi config | Own NVS keys (`NVS_KEY_WIFI_*`) + **`WiFi.persistent(false)`** (fixed here) | Wiki mentions `WiFi.persistent(true)` to retain config across power cycles | ✅ with rationale — we persist credentials ourselves; letting the core also persist them double-writes flash and resurrects cleared networks |

## Findings fixed on this branch

### S1 — Antenna documentation error (docs)
`esp32s3_ble_wap_audit.md` previously said *"The onboard antenna is fine on the
S3 (external improves range)"*. Per Seeed's getting-started and usage pages, the
XIAO ESP32-S3 has **no onboard antenna** — only a U.FL connector, and the
bundled 2.4 GHz rod antenna must be attached:

> "If you do not have an antenna installed, you may not be able to use the
> Bluetooth feature." — Bluetooth usage page
>
> "If the antenna is not installed, it may not be able to connect to the WiFi
> network." — WiFi usage page

Installation note from Seeed: angle one side of the U.FL plug into the
connector first, then press the other side down — do not push straight down.
For sustained AP/streaming workloads Seeed also notes the stock antenna "cannot
support high-intensity network work" and suggests a stick antenna upgrade.

**Fix:** corrected paragraph in `esp32s3_ble_wap_audit.md`; field/assembly
checklists should treat "antenna attached" as a hard prerequisite, and
"AP visible but weak / BLE init fine but no advertisements seen" as the
missing-antenna signature.

### S2 — `WiFi.persistent(false)` missing in modular canary (code)
`firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino` calls
`WiFi.persistent(false)` before radio bring-up; the modular
`ScvNetworkManager::begin()` did not. With the Arduino default (persistent
**true**), every `WiFi.begin()` writes the SSID/passphrase into the SDK's wifi
NVS namespace: redundant flash writes on every reconnect cycle, and after
`clearCredentials()` the SDK copy can silently auto-rejoin the supposedly
forgotten network on next boot.

**Fix:** `WiFi.persistent(false);` added at the top of
`ScvNetworkManager::begin()` (`securacv_network.cpp`), before `WiFi.mode()`.

### S3 — BLE TX power divergence between variants (code)
The modular canary's `ble_status_stack_begin()` hardcoded `setPower(3)` while
canary-wap's F5 fix standardized on an NVS-configurable default of **+9 dBm**
(the S3 maximum, "historically effective"). On a board whose only RF path is
the external antenna, the two variants advertising at different powers means
inconsistent companion-app range.

**Fix:** new `BLE_TX_POWER_DBM` define in `include/canary_config.h`
(default 9), consumed by `ble_status_stack_begin()`. NimBLE 2.x `setPower()`
takes dBm directly — never pass `ESP_PWR_LVL_*` indexes.

### S4 — Quarter-dBm parameter naming (code hygiene)
`network_set_tx_power(int8_t dbm)` takes **quarter-dBm** (8..84). A caller
reading only the prototype would pass `15` expecting 15 dBm and get the 2 dBm
floor. Renamed to `quarter_dbm` in header + implementation; header comment now
gives `60 = 15 dBm` (Seeed's weak-signal recommendation) as the worked example.

## Noted, no change required

- **Thermal interaction (watch item).** Seeed warns the module can reach
  ~50 °C under sustained WiFi load and that prolonged operation at that
  temperature "may cause network abnormalities". The die-temp tamper detector
  (`securacv_envsens`, ≥5 °C sustained step vs a 1-minute-cadence EMA baseline)
  absorbs *gradual* radio self-heating, but the start/stop of a heavy radio
  workload (camera-peek streaming over the AP) is a plausible ≥5 °C step.
  Bench item: run 10 min of `/api/peek/stream` and confirm no
  `tamper_temp_drift` event fires; if it does, raise
  `drift_threshold_tenths_c` or gate the detector during streaming.
- **AP password length.** 8 chars is exactly the WPA2 minimum
  (entropy ≈ 28.8 bits over the 54-char unambiguous alphabet). Acceptable for
  a provisioning AP that allows max 1 client and is torn down once the STA
  link holds (F4); not acceptable if the AP ever becomes long-lived — revisit
  if that design changes.
- **WiFi mesh.** Seeed's wiki demonstrates painlessMesh; this project uses
  ESP-NOW + its own mesh layer (`FEATURE_MESH_NETWORK`). Out of scope here —
  no Seeed-doc conflict.
- **Seeed's MTU 517 suggestion** trades RAM and connection-event airtime for
  throughput the status service doesn't need; 247 stays.

## Verification

- Host tests unaffected (no host test touches these lines).
- Target compile via CI (`.github/workflows/firmware.yml`) — `dev`/`release`
  envs cover `securacv_network`; `full` covers the NimBLE path.
- On-device acceptance:
  1. Provision home WiFi → reboot → STA reconnects (from project NVS only).
  2. `POST /api/wifi/disconnect` → reboot → device must **not** rejoin the old
     network (validates S2); AP `SecuraCV-XXXX` comes up at 192.168.4.1.
  3. BLE advertisement RSSI at fixed distance improves vs prior +3 dBm build
     (validates S3).
