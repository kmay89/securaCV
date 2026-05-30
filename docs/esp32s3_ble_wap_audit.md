# ESP32-S3 BLE + WiFi-AP (WAP) Implementation Audit

**Target:** `firmware/projects/canary-wap/` (Canary WAP) — XIAO ESP32-S3 Sense,
Arduino framework, NimBLE-Arduino 2.3.8.
**Question audited:** *Are we initializing BLE correctly in firmware and hardware,
and is running BLE alongside the WiFi Access Point sound?*

## TL;DR

The BLE bring-up was **mostly correct but had two real defects and one
architectural coexistence risk**:

1. `NimBLEDevice::init()`'s return value was ignored in **both** init paths, so
   the documented "graceful degradation if BLE hardware unavailable" never
   actually happened — and `createServer()` was dereferenced without a null
   check, so a failed stack bring-up crashed instead of degrading.
2. **Two modules each called `NimBLEDevice::init()`** with different device
   names and TX powers. NimBLE init is idempotent, so the second name was
   silently dropped and the second `setPower()` clobbered the first.
3. The device ran **WiFi `AP_STA` + BLE simultaneously and never tore the AP
   down** — Espressif rates `SoftAP (connected) + BLE` as **C1: supported but
   performance unstable**. `STA + BLE` is the stable (Y) combo.

All three are fixed on this branch. Items #1/#2 are pure correctness fixes; #3
drops the AP once the home-WiFi STA link is healthy and re-raises it on STA loss.

## Research baseline — what "correct" looks like

### Single radio, time-division multiplexed
The ESP32-S3 has **one** 2.4 GHz RF front-end shared by WiFi and BLE; the
controller time-slices it. Espressif's RF-Coexistence support matrix
([esp-idf coexist guide], [esp-faq]) rates the relevant combinations:

| WiFi state | + BLE (adv/scan/conn) | Rating |
|---|---|---|
| STA scan / connecting / connected | any | **Y — stable** |
| SoftAP TX beacon (AP up, no client) | any | **Y — stable** |
| **SoftAP connecting / connected (client joined)** | any | **C1 — supported, unstable** |

So a WiFi **Access Point with a client attached + BLE** is the unstable case.
Running `AP_STA` (AP **and** STA) **and** ESP-NOW mesh **and** CSI on top of BLE,
as the FULL build does, piles even more onto that one radio.

SW coexistence (`CONFIG_SW_COEXIST_ENABLE`) is **enabled by default** in the
arduino-esp32 core build, and the arbiter already defaults to a **BALANCE**
preference. The legacy `esp_coex_preference_set()` knob is **deprecated in
IDF5** ([esp_coexist.h]) and would only re-assert that default — so the
meaningful lever is the radio **mode**, not a preference call.

### NimBLE-Arduino 2.x init contract ([NimBLE-Arduino], [2.x migration])
- `NimBLEDevice::init()` returns **`bool`** — check it.
- It is **idempotent**: a second call while already initialized is a no-op and
  does **not** change the device name or re-apply settings.
- `createServer()` may return `nullptr`.
- `setPower(int dBm)` takes dBm directly (not an `ESP_PWR_LVL_*` index).
- `getInitialized()` → `isInitialized()`; callbacks now take `NimBLEConnInfo&`.

### Power / hardware
Simultaneous WiFi+BLE TX spikes draw **~400–700 mA** ([ESP32 brownout threads]).
The XIAO ESP32-S3's onboard LDO (~600 mA class) is marginal under AP+STA+BLE
(+camera). An undersized 3.3 V rail / missing bulk decoupling causes brownout
resets that present as "BLE init failed" or boot loops — and a boot loop trips
this firmware's `safe_mode_check()`, which then disables BLE/WiFi entirely,
masking the real (power) cause. The onboard antenna is fine on the S3 (external
improves range); the C3 variant **requires** the IPEX antenna.

## Findings & resolution

| # | Severity | Finding | Fix |
|---|----------|---------|-----|
| **F1** | High | `NimBLEDevice::init()` return ignored in `bluetooth_channel::init()` and `ble_manager::init()`; `g_ble_available`/`return true` set unconditionally. "Graceful degradation" was not implemented. | Both paths now check the `bool` and `return false` on failure with a logged error. `ble_manager` derives `g_ble_available` from `NimBLEDevice::isInitialized()`. |
| **F2** | High | `createServer()` result not null-checked; next line dereferenced it → crash on a failed bring-up. | Null-checked; logs, `deinit(true)`, and `return false` before any deref. |
| **F3** | Medium | Two init owners called `init()` with different names; NimBLE idempotency silently dropped `ble_manager`'s `SCV-xxxx` name. | `bluetooth_channel::init()` is the **single init owner** (name/power/MTU/security). `ble_manager` only inits if `!isInitialized()` (fallback when `FEATURE_BLUETOOTH` is off). |
| **F4** | Medium | `AP_STA + BLE`, AP never torn down → permanent C1/unstable zone; AP also pinned ch1 vs STA's home-channel. | After the STA holds the home link `AP_DROP_GRACE_MS` (8 s), `wifi_drop_ap()` switches to `WIFI_STA` (stable STA+BLE). `wifi_raise_ap()` restores the AP + captive portal on STA loss so the device stays reconfigurable. |
| **F5** | Medium | `setPower(3)` then `setPower(9)` across the two owners (last-writer-wins = 9). | Single owner sets power once. Default `tx_power` bumped 3→9 to preserve the historically-effective +9 dBm; explicit NVS overrides are now correctly honored. |
| **F6** | Verify | PlatformIO `platform = espressif32@6.9.0` (= core 2.0.17 / IDF 4.4, inherited from `common_esp32s3`) is **inconsistent** with the IDF5 APIs the code uses (`esp_task_wdt_config_t.idle_core_mask`, NimBLE 2.x). The canonical build is **arduino-cli with `esp32:esp32` latest (3.x / IDF5)** — see `.github/workflows/firmware.yml`. | **Fixed (follow-up):** canary-wap PlatformIO envs now pin `platform = espressif32 @ ^7.0.0` (core 3.x / IDF5), matching the arduino-cli core and the modular `canary/` build. |
| **F7** | Info | No brownout/power handling in firmware; a brownout reset masquerades as a BLE/WiFi init failure or boot-loops into safe mode. | **Fixed (follow-up):** `setup()` now logs the last reset cause via `esp_reset_reason()` (brownout → `ERROR` with a 3.3 V-rail hint). Hardware guidance (≥500 mA supply, 470–1000 µF + 0.1 µF decoupling) documented in [`esp32s3_power_resilience.md`](./esp32s3_power_resilience.md). |

**Already correct (kept as-is):** `WiFi.softAP()` return checked; safe-mode
gating of radios; callers check `init()` return values; correct NimBLE 2.x
callback signatures (`NimBLEConnInfo&`, `setScanCallbacks`); privacy-preserving
passive scout scan; MTU 247; thoughtful dBm `setPower` comments.

## Files changed
- `firmware/projects/canary-wap/arduino/canary_wap/bluetooth_channel.cpp` — F1/F2/F5.
- `firmware/projects/canary-wap/arduino/canary_wap/ble_manager.h` — F1/F3/F5.
- `firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino` — F4 (drop/raise AP, state-machine wiring, stale-comment updates).

## Verification
- Host tests (`firmware/projects/canary-wap/tests_host`, `make`) pass — they
  cover mesh/chirp/captive/beacon modules (no regression from these edits).
- **Target compile is via CI** (`arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3`);
  the ESP32 core is not installable in the dev sandbox, so on-target build is
  validated by `.github/workflows/firmware.yml`.
- On-device acceptance: exactly one NimBLE init in the boot log; **unprovisioned**
  → AP up + BLE advertising; **after joining home WiFi** → mode flips to STA-only,
  AP drops, BLE keeps advertising (stable); **pull home WiFi** → AP re-raises and
  `canary.local` is reachable again. No brownout/boot-loop into safe mode.

## Sources
- Espressif RF Coexistence guide: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html>
- esp-faq coexistence: <https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/coexistence.html>
- `esp_coexist.h` (deprecation of `esp_coex_preference_set`): <https://github.com/espressif/esp-idf/blob/master/components/esp_coex/include/esp_coexist.h>
- NimBLE-Arduino: <https://github.com/h2zero/NimBLE-Arduino> · 1.x→2.x migration: <https://github.com/h2zero/NimBLE-Arduino/blob/master/docs/1.x_to2.x_migration_guide.md>
- arduino-esp32 WiFi+BLE coexistence (#8293): <https://github.com/espressif/arduino-esp32/issues/8293>
- ESP32 brownout / WiFi+BLE current spikes: <https://github.com/espressif/arduino-esp32/issues/863>

[esp-idf coexist guide]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html
[esp-faq]: https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/coexistence.html
[esp_coexist.h]: https://github.com/espressif/esp-idf/blob/master/components/esp_coex/include/esp_coexist.h
[NimBLE-Arduino]: https://github.com/h2zero/NimBLE-Arduino
[2.x migration]: https://github.com/h2zero/NimBLE-Arduino/blob/master/docs/1.x_to2.x_migration_guide.md
[ESP32 brownout threads]: https://github.com/espressif/arduino-esp32/issues/863
