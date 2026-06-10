# XIAO ESP32-S3 Thermal Review — what we learned, what we fixed

**Target:** `firmware/canary/` (modular PlatformIO build), Seeed Studio XIAO
ESP32-S3 (Sense).
**Question reviewed:** *Are our temperature thresholds consistent with Seeed's
documentation, and does the device degrade gracefully — and recover — when it
gets too hot?*
**Companions:** [`seeed_xiao_esp32s3_wireless_review.md`](./seeed_xiao_esp32s3_wireless_review.md)
(wireless compliance), [`esp32s3_power_resilience.md`](./esp32s3_power_resilience.md)
(brownout/power).

## TL;DR

Graceful thermal degradation **was already designed in** — the camera-peek
streamer has a three-state thermal machine (normal → throttled → paused) with
hysteresis-based recovery. But it was **silently inert in every default build**:
ESP-IDF 5.x allows exactly one temperature-sensor driver instance, and three
modules each installed their own. Whichever initialized first (in practice the
tamper detector) won; the camera's install failed and its `checkThermal()`
returned without ever updating the temperature, and the diagnostics temp
self-test read NAN and failed forever. Separately, the tamper-drift detector's
"EMA baseline" was actually frozen at boot, guaranteeing a false tamper event
once normal workload warmed the die ≥ 5 °C. All fixed on this branch.

## What Seeed and Espressif document

| Fact | Value | Source |
|---|---|---|
| Board working temperature | **−20 … 65 °C** (all XIAO ESP32-S3 variants) | Seeed getting-started |
| Chip temp under plain SoftAP example | up to **~50 °C**; "may lead to network abnormalities if run for a long time"; mitigation: "cool down and retry", better antenna | Seeed WiFi usage, troubleshooting Q1 |
| Camera streaming, 1 h (Sense) | **63.6 °C** board backside peak; **53.5 °C** with dual heat sinks | Seeed getting-started |
| Heat sink | included with camera (Sense) models; "prioritize covering the Thermal PAD … directly above the ESP32S3 chip, the primary source of heat" | Seeed getting-started |
| Camera power draw | ~347 mA peak @5 V (image capture), ~140 mA average | Seeed getting-started |
| Internal sensor measures | **silicon die temperature, not ambient**; "can reflect the temperature changes very well but it can't give a precise measurement value" | Espressif temp_sensor docs |
| Sensor accuracy bands | predefined ranges; ±1 °C in −10…80 °C, **±2 °C in 20…100 °C**, ±3 °C at the extremes | Espressif temp_sensor docs |
| Only ONE driver instance | the IDF 5 `temperature_sensor` driver can be installed once per chip | Espressif temp_sensor docs |

Implication for thresholds: die temperature runs well above ambient under load
(Seeed's 63.6 °C *backside* after an hour of streaming implies a hotter die),
so die-temp thresholds in the 70–85 °C band are the correct order of magnitude
for a board specified to 65 °C ambient — and any *absolute* comparison should
assume ±2 °C error, while *delta*-based logic (drift detection) is unaffected
by the band's absolute offset.

## Current protections (verified)

### Camera thermal state machine — `securacv_camera.{h,cpp}`
| State | Entry | Effect | Recovery |
|---|---|---|---|
| NORMAL | die < 70 °C | user frame rate | — |
| THROTTLED | die ≥ 70 °C | 3× frame delay, clamped 120–500 ms (~8→2 fps) | back to NORMAL below 65 °C |
| PAUSED | die ≥ 80 °C | frame capture skipped, 2 fps poll | down to THROTTLED below 75 °C |

Checked every 5 s during streaming; transitions logged (`LOG_LEVEL_WARNING`
entering, `INFO` recovering). The 5 °C recovery margin prevents flapping.
**Threshold sanity vs docs:** 70/80 °C die sits sensibly above Seeed's measured
worst case for the *intended* workload (their 1-h stream peaked ~63.6 °C
backside *without* the now-bundled heat sink) and below the 20…100 °C band's
ceiling, so the protection engages only in genuinely abnormal conditions
(blocked airflow, no heat sink, hot enclosure) — and recovers on its own. Kept.

### Tamper-drift detector — `securacv_envsens`
5 °C step threshold, 60 s cadence, 5-min event suppression. The threshold is
delta-based, so the sensor band's ±2 °C absolute error doesn't matter; what
matters is step size, and the legitimate tamper signatures (case opened, heat
gun, relocation) are multi-degree steps. Kept.

### Diagnostics self-test — `securacv_diagnostics`
`temp_range` test passes for −20 °C < die < 85 °C. Lower bound matches the
board's −20 °C working spec; 85 °C upper bound flags a die hotter than the
camera's pause threshold — i.e., something is wrong beyond what throttling
handles. Kept.

### What is deliberately NOT done
No system-level auto-reduction of WiFi TX power or forced modem-sleep on heat:
the camera is the dominant controllable heat source and is now actually
throttled; Seeed's plain-SoftAP ceiling (~50 °C) is far from the 80 °C pause
threshold; and silently degrading the radio would *create* the "network
abnormalities" symptom we're trying to avoid. A brownout or panic from a
pathological thermal event is already caught by the reset-reason logging and
safe-mode path (`esp32s3_power_resilience.md`).

## Findings fixed on this branch

### T1 — Three owners of a single-instance driver (high)
`securacv_envsens` (install at boot, held forever), `securacv_camera`
(`checkThermal()`, lazy install) and `securacv_diagnostics`
(Arduino `temperatureRead()`, which keeps its own HAL handle) each installed
the IDF 5 temperature-sensor driver. Only one instance is permitted, so with
the default feature set (`FEATURE_TEMP_TAMPER=1`):

- camera thermal protection **never saw a temperature** → no throttle, no
  pause, regardless of heat — exactly the user-facing thermal risk;
- the diagnostics `temp_range` self-test read NAN → **permanently failed**,
  silently dragging the health score down.

**Fix:** new shared provider `lib/securacv_thermal/` — single lazy-installed
handle, mutex-guarded (`thermal_read_die_c()` is called from both the main
loop and the httpd streaming task), range 20…100 °C (±2 °C — covers normal
operation through the 80 °C pause threshold; drift logic is delta-based so the
band's absolute offset cancels). envsens, camera, and diagnostics all read
through it now; none install their own handle.

### T2 — "EMA baseline" was frozen at boot (high)
The envsens header and `library.json` both documented an EMA baseline; the
implementation locked the mean of the first 5 samples and **never updated it**.
A device booted cool (e.g., after a power cut at night) warms 5–15 °C die under
perfectly normal AP/CSI workload → guaranteed false `tamper_temp_drift` event,
**signed into the witness chain** as kind `SENSING_WITNESS_TEMP_DRIFT`. A
tamper-evidence product must not write false tamper evidence.

**Fix:** baseline is now the EMA the docs always claimed: sub-threshold deltas
adapt at alpha 1/16 per 60 s sample (~16 min time constant, minimum step
0.1 °C so it always converges); deltas ≥ 5 °C fire instead of adapting — the
tamper signature is the fast step, not the slow trend.

### T3 — Camera streaming is thermally indistinguishable from a heat gun (medium)
Starting a peek stream steps the die 10–20 °C in minutes — faster than any EMA
should absorb, identical in shape to the heat-gun attack the detector exists to
catch. New `envsens_set_high_load(bool)` hint: the main loop reports
`camera_get_instance().isPeekActive()`; while active (plus a 10-min cooldown
covering the cool-down ramp) the baseline fast-tracks the temperature (alpha
1/2) and detection is suspended, then resumes against a settled baseline. The
trade-off is explicit: temperature-based tamper detection is blind during
streaming — acceptable because the touch-pad tamper path (the *primary*
case-open detector) remains armed, and an attacker can't enable streaming
without already being authenticated.

### T4 — Wireless-review doc described the unfixed behavior (doc)
`seeed_xiao_esp32s3_wireless_review.md` called the baseline an EMA (inherited
from the envsens header) and framed false-fire risk as a bench item. Updated to
reflect T2/T3 as fixed and point here.

## Verification

- Target compile via CI (PlatformIO Canary dev/full envs cover envsens,
  camera, diagnostics, and the new lib; LDF picks `securacv_thermal` up from
  the `#include`).
- On-device acceptance:
  1. Boot with `FEATURE_TEMP_TAMPER=1` + camera: serial shows **no**
     "Envsens: temp install failed", and `/api/peek/stream` under load logs
     `[CAMERA] Thermal:` transitions with a real die temp (validates T1).
  2. `/api/selftest` → `temp_range` passes with envsens running (T1).
  3. Cold-boot the device, run normal AP workload 1 h → no
     `tamper_temp_drift` event; `/api/sensing` baseline tracks ~die (T2).
  4. Stream camera 10 min, stop, wait 10 min → no drift event during stream,
     cooldown, or settle (T3).
  5. Heat-gun test (bench only): with the device idle, warm the case rapidly →
     drift event fires with confidence scaling, 5-min suppression observed
     (detector still does its actual job).
  6. Optional: block airflow / no heat sink, stream until die ≥ 70 °C →
     observe THROTTLED log + frame-rate drop, then recovery after cooling
     (graceful degradation end-to-end).

## Follow-up: thermal watchdog (closes the remaining gaps)

Three gaps remained after this review and are now closed by the passive
thermal watchdog (`lib/securacv_thermal_watchdog/`, `FEATURE_THERMAL_WATCHDOG`,
default on):

1. **Blind when idle** — `checkThermal()` only ran inside the peek-streaming
   loop; the watchdog samples the shared provider every 30 s regardless, and
   `/api/peek/status` now reports its fresh cache instead of a stale reading.
2. **Silent sensor failure** — a failed `thermal_read_die_c()` kept the last
   state forever with no trace. The watchdog surfaces an ERROR health-log
   entry after 4 consecutive misses (~2 min), and the camera now fails safe
   to THROTTLED after 3 misses at its 5 s cadence instead of streaming the
   hottest peripheral blind at full rate.
3. **No persistence** — lifetime history (all-time min/max, throttled/paused
   minutes, throttle/pause/critical/cold/sensor-fail event counts) is
   NVS-persisted on a 10-min dirty-flag cadence (same pattern as the battery
   history) and served at `GET /api/thermal`, plus an "Adaptive performance"
   dashboard card.

The watchdog is strictly an observer — the camera state machine above remains
the sole actuator. It mirrors the same 70/80/5 hysteresis as a *shadow*
classifier so history counts the same conditions the actuator responds to,
and raises advisories (critical ≥ 85 °C, saturation = 10 min continuous
shadow-pause, env-limited = ≥4 throttle entries/30 min, cold < 5 °C die)
through the health log only — never the witness chain.

## Operational guidance (user-facing, from Seeed)

User-facing placement, heat-sink, hot/cold-weather, and history-reading
guidance now lives in [`thermal_guide.md`](./thermal_guide.md). The short
version:

- Install the bundled heat sink on Sense units — on the thermal pad directly
  over the ESP32-S3 first. It's worth ~10 °C under sustained streaming.
- Keep the device inside the −20…65 °C ambient spec; remember the die runs
  hotter than the room.
- Sustained streaming in an enclosed, unventilated mount is the worst case:
  expect throttling to engage by design rather than the device failing.
