# Canary Thermal Guide — heat, cold, and the Adaptive Performance system

Your Canary runs its hardware at the full safe envelope. Rather than ship a
device that idles below its potential, we let the ESP32-S3 work at its
documented limits and built an adaptive system that paces the hottest
workloads automatically — then restores full speed the moment there's
headroom. You don't manage any of this. This guide explains what the device
does on its own, what the numbers mean when you check in, and the handful of
physical choices (placement, heat sink, settings) that give it more headroom.

**Engineering background:** [`esp32s3_thermal_review.md`](./esp32s3_thermal_review.md)
holds the full audit — thresholds, hysteresis design, and the Seeed/Espressif
source data quoted below.

---

## 1. How Canary manages heat

The ESP32-S3 has an on-die temperature sensor. It measures the **silicon
itself, not the room** — under load the chip always runs warmer than the air
around it, which is normal and by design.

Two cooperating systems use it:

- **Adaptive performance (the actuator).** Camera streaming is the hottest
  thing the device does (~347 mA peaks). While streaming, the camera checks
  the die every 5 seconds and walks a three-step ladder:

  | Die temp | Mode | What changes |
  |---|---|---|
  | below 70 °C | **Full performance** | your chosen frame rate |
  | ≥ 70 °C | **Adaptive performance** | frame rate paced to roughly a third |
  | ≥ 80 °C | **Protective pause** | frames rest briefly until the die sheds heat |

  Recovery is automatic and deliberate: the device steps back up 5 °C *below*
  each entry point (65 °C / 75 °C), so it never ping-pongs between modes —
  one clean step down, one clean step up.

- **The thermal watchdog (the observer).** Always on, even when the camera
  isn't. Every 30 seconds it samples the die, keeps a lifetime history in
  flash (survives reboots and power cuts), and raises plain-language
  advisories. It never slows anything down itself — it's the flight recorder,
  not the pilot.

Seeing "Adaptive performance" on the dashboard is the system doing its job,
not a fault. The device is sustaining the maximum pace the environment
allows at that moment.

## 2. The numbers that matter

From Seeed's published testing of the XIAO ESP32-S3 Sense and Espressif's
sensor documentation:

| Fact | Value |
|---|---|
| Ambient operating range (board spec) | **−20 … 65 °C** |
| Board temp after 1 h of camera streaming | **63.6 °C** bare / **53.5 °C with heat sinks** |
| Plain WiFi SoftAP, sustained | chip up to **~50 °C** |
| Camera power draw | **~347 mA** peak, ~140 mA average @ 5 V |
| Die sensor accuracy | ±2 °C (tracks *changes* very well) |

The 70/80 °C die thresholds sit deliberately **above** Seeed's measured
worst case for the intended workload — so adaptation only engages in
genuinely demanding conditions (blocked airflow, sealed enclosure, hot sun),
never during ordinary use.

## 3. The heat sink — worth ~10 °C

Sense kits ship with heat sinks. Seeed's own testing shows they cut sustained
streaming temperature by **about 10 °C** (63.6 → 53.5 °C) — the single most
effective thing you can do.

**Install order matters:** put the first heat sink on the **thermal pad
directly above the ESP32-S3 chip** — that's the primary heat source. Add the
second to the other pad if your kit includes one. With heat sinks fitted, a
Canary streaming continuously in normal indoor conditions should never leave
Full performance.

## 4. Placement — sun, hot days, enclosures

- **Keep it out of direct sunlight.** Sun on the case can add tens of degrees
  before the electronics contribute anything. A window-facing Canary in
  summer is the most common reason for frequent adaptation.
- **Hot days:** the board is rated to 65 °C ambient — far beyond any livable
  room — but remember a car dashboard, sealed attic, or south-facing
  windowsill can exceed that. In a heat wave, expect the device to spend more
  time in Adaptive performance during streaming. That's it working correctly;
  it returns to full pace as the air cools.
- **Enclosures need airflow.** A fully sealed, unventilated mount is the
  documented worst case. A few millimetres of vent or standoff gap makes a
  measurable difference. The printable SecuraCV enclosures include
  ventilation for this reason.
- **Keep it off heat sources** — radiators, set-top boxes, power bricks,
  refrigerator tops.

## 5. Camera, WiFi, and settings that run cooler

- **Streaming is the heat budget.** Witness duty (the device's actual job)
  is light; live peek streaming is the workload that warms the die 10–20 °C.
  Long continuous peek sessions in a warm spot are what invite adaptation —
  shorter looks, or a lower resolution / frame rate in the peek settings,
  run proportionally cooler.
- **WiFi:** a sustained SoftAP tops out around ~50 °C chip temperature by
  itself — comfortably inside the envelope, which is why the device never
  silently degrades its own radio (that would just trade a visible behavior
  for mystery network problems). Good antenna placement helps the radio work
  less hard; keep the antenna clear of metal.
- **Power it sensibly:** a quality 5 V supply; avoid stacking the device on
  top of its own power adapter.

## 6. Cold weather

The board is specified down to **−20 °C ambient**, and the electronics
themselves are happy in the cold. Two things deserve care:

- **Never charge a LiPo battery below 0 °C.** Charging a frozen lithium
  battery damages it permanently (and unsafely). If your Canary runs on
  battery outdoors in winter, charge it indoors, or use a LiFePO₄ pack with
  an external low-temperature charge cutoff (rated to −20 °C).
- **Condensation.** Bringing a cold device into a warm room fogs it like a
  pair of glasses. Let it acclimatise before powering, or keep it in its
  enclosure during the move.

The watchdog raises a **Cold environment** advisory when the die drops below
5 °C — since the die runs warmer than the air, that means ambient is at or
near freezing, exactly the no-charging zone. The advisory clears itself when
things warm up.

## 7. Checking in after a week — or a month

The watchdog keeps lifetime history precisely so you *don't* have to watch
the device — and if you use Home Assistant, you don't even have to check in:
the device publishes **Die Temperature**, **Thermal Performance**, and a
**Thermal Advisory** problem sensor over MQTT (see
[homeassistant_setup.md](./homeassistant_setup.md)), and
[homeassistant_automations.yaml](./homeassistant_automations.yaml) includes
ready-made notifications for thermal advisories and sustained protective
pauses. Routine adaptation never pages you; conditions worth acting on do.

Whenever you do check in manually, two places show the story:

- **Dashboard → Sensing → "Adaptive performance" card** — current mode,
  die temperature, hottest/coldest ever seen, cumulative adaptive minutes,
  protective-pause count, and sensor health.
- **`GET /api/thermal`** — the same data as JSON:

```json
{
  "ok": true,
  "die_temp_c": 47.3,
  "thermal_state": "normal",
  "sensor_ok": true,
  "advisories": [],
  "history": {
    "alltime_min_c": 9, "alltime_max_c": 71,
    "total_runtime_min": 40320,
    "throttled_min": 122, "paused_min": 0,
    "throttle_events": 9, "pause_events": 0,
    "critical_events": 0, "sensor_fail_events": 0, "cold_events": 0
  },
  "thresholds": { "throttle_c": 70, "pause_c": 80, "recover_margin_c": 5,
                  "critical_c": 85, "cold_c": 5 }
}
```

Read it like this — counters are lifetime totals, so judge them against
`total_runtime_min` (a month ≈ 43,000 minutes):

| What you see | What it means | What to do |
|---|---|---|
| `throttle_events` in single digits, `paused_min` ≈ 0 | Healthy placement; adaptation only brushed in during long streams | Nothing — this is the device used to its full envelope |
| `throttled_min` growing noticeably week over week | The spot runs warm during streaming | Fit the heat sink (§3), add shade or ventilation (§4), or trim peek resolution/FPS (§5) |
| `pause_events` > 0, or a *saturation* advisory in the logs | The environment exceeded the device's cooling capacity for 10+ minutes | Relocate it out of sun/enclosed heat; reduce continuous streaming |
| `critical_events` > 0 | Die passed 85 °C — beyond what pausing should ever allow | Move the device now; check for a sealed enclosure, direct sun, or a failed heat sink |
| `sensor_fail_events` > 0 or `sensor_ok: false` | The temperature sensor itself misbehaved (protection ran conservatively meanwhile) | Check `/api/logs` for the episode; if it recurs, contact support — that's hardware |
| `alltime_min_c` below 5 | The device has seen near-freezing conditions | Fine for the electronics — just never charge the battery below 0 °C (§6) |
| `env_limited` advisory | ≥4 adaptation cycles within 30 minutes — the spot is warm enough that the device adapts routinely | Guidance, not an error: shade, ventilation, or the heat sink gives it permanent headroom |

## 8. What the device never does

- **It never hides a fault.** A failed temperature sensor is reported as an
  ERROR in the health log, shown red on the dashboard, and counted in
  history — while the camera fails safe to a paced rate rather than running
  the hottest peripheral blind.
- **It never gets stuck in a loop.** Every threshold has a 5 °C recovery
  margin below its entry point; mathematically there is no temperature at
  which the device oscillates between modes.
- **It never silently degrades the radio.** WiFi heat is well inside the
  envelope; trading visible camera pacing for invisible network problems
  would be the wrong kind of clever.
- **It never pollutes your evidence.** Thermal events go to the health log
  only — the cryptographic witness chain stays reserved for what it's for.
