# ESP32-S3 Power & Battery Guide — Canary (XIAO ESP32-S3 Sense)

**Audience:** anyone powering, battery-backing, or field-deploying a Canary
WAP. Read this *before* choosing a power supply, cable, or battery.

**Companions:**
[`esp32s3_power_resilience.md`](../esp32s3_power_resilience.md) (brownout
deep-dive), [`canary_peripheral_build_plan.md`](./canary_peripheral_build_plan.md)
§6.5/§8/§9 (BOM, battery safety, climate), and
[`v1_bench_validation_runbook.md`](./v1_bench_validation_runbook.md) (how to
measure for real).

> ⚠️ **All current figures below are datasheet-typical estimates**, not bench
> measurements of this firmware. They are good enough for choosing a supply
> and sizing a battery with margin; they are **not** a substitute for
> measuring your build before committing to an unattended deployment.
> Lithium battery safety guidance in the
> [build plan §6.5](./canary_peripheral_build_plan.md) applies in full —
> you assume all risk.

---

## 1 · The board, in power terms

| Element | What it is | Power-relevant facts |
|---|---|---|
| MCU | ESP32-S3 dual-core @ 80/160/240 MHz | Firmware scales the clock per power mode |
| Radios | WiFi 2.4 GHz + BLE 5 on **one shared front-end** | TX bursts are the peak-current driver |
| Camera | OV2640 (Sense expansion) | ~80–150 mA while streaming |
| Storage | microSD (Sense expansion) | 30–100 mA write bursts |
| USB | USB-C, 5 V input, native CDC | Primary supply + flashing |
| Charger | Onboard Li-ion/LiPo charge IC on BAT+/BAT− pads | ≈100 mA charge current (board-fixed, per Seeed documentation); **LiPo/Li-ion 4.2 V only**; **no battery thermistor input** |
| Regulator | Onboard 3.3 V LDO, ~600 mA class | The brownout bottleneck — see §2 |
| Battery sense | GPIO 1 (A0) via user-added 2:1 divider | 2 × 100 kΩ; ~21 µA constant drain on the cell |

Power can enter three ways:

1. **USB-C** (recommended default) — powers the board *and* charges the battery.
2. **BAT+/BAT− pads** — single-cell LiPo/Li-ion; the board boosts/regulates it.
3. **5V pin** — only with a clean regulated 5 V source. **Do not feed the 5V
   pin and USB-C at the same time** — there is no ideal-diode OR-ing between
   them; backfeeding can damage the host port or the board.

Battery **plus** USB simultaneously is fine and is the intended UPS-style
setup: USB carries the load, the charger tops the cell, and the device rides
through outages.

---

## 2 · What it needs: supply and cable requirements

The instantaneous draw when WiFi and BLE transmit in the same window — with
camera and SD active — spikes to roughly **400–700 mA** on the 3.3 V rail
(see the [resilience doc](../esp32s3_power_resilience.md)). If the supply or
cable cannot service the spike, the SoC brownout-detects and resets, which
**looks exactly like a firmware crash** (boot loops, "BLE init failed",
safe-mode trips).

**Requirements:**

| Item | Minimum | Recommended | Why |
|---|---|---|---|
| USB supply | 5 V / 1 A | 5 V / 2 A name-brand | Headroom over the ~0.6 A worst-case spike |
| Cable | data-capable USB-C, < 1 m | 24 AWG-power-wires, ≤ 0.5 m | Long/thin cables drop 0.3–0.6 V at spike current — the #1 brownout cause |
| Field units | — | 470–1000 µF low-ESR bulk cap across 3V3/GND + 0.1 µF ceramic | Absorbs TX spikes the LDO can't |
| Battery units | Cell rated ≥ 1 C continuous with protection PCM | ≥ 1000 mAh so spikes are ≪ 1 C | A small/aged cell sags under TX bursts → brownout at "30% SoC" |

**Cable sanity check:** if the device boot-loops on one cable/port and runs
on another, the cable was the problem. The firmware logs
`Last reset: brownout (supply voltage sag)` at ERROR level on the next boot
and counts it in `brownout_count` (`GET /api/battery/history`) — a nonzero,
growing count is a power problem, not a software one.

**Phone chargers with "smart" ports:** some QC/PD bricks renegotiate or
power-cycle the port on load transients. If a unit resets when the radio
fires, try a dumb 5 V/2 A supply.

---

## 3 · What it consumes: estimates by firmware power mode

The power policy engine (`power_policy`) selects a mode from battery state
and prunes features accordingly. Datasheet-typical estimates for a Canary WAP
(WiFi on, camera/SD per mode):

| Policy mode | When | CPU | WiFi | Typical avg draw @3.3 V | Notes |
|---|---|---|---|---|---|
| `PLUGGED_IN` / `USB_ONLY` | USB power or no battery | 240 MHz | PS off | **~150–250 mA** | All features incl. camera peek; peaks 400–700 mA |
| `BATTERY_NORMAL` | SoC > 50% | 160 MHz | min modem-sleep | **~90–140 mA** | Camera preview off (sensor parked); 5 s record interval |
| `BATTERY_SAVER` | SoC 15–50% | 80 MHz | max modem-sleep | **~40–80 mA** | Camera preview off; 30 s record interval |
| `LOW_POWER` | SoC 5–15% | 80 MHz | max modem-sleep | **~7–15 mA averaged** | 5 s awake / 55 s deep-sleep duty cycle; STA kept for panic events |
| `SHUTDOWN` | SoC ≤ 3% | — | — | **~0.1–2 mA** | Graceful shutdown → deep sleep, 30 min wake timer |

### What the policy actually enforces (vs. advisory)

The firmware ENFORCES these per-mode signals at runtime:

- **CPU frequency and WiFi power-save** — applied directly on every mode
  transition.
- **Camera preview (`camera_peek`)** — the peek stream/snapshot endpoints
  answer `503` with an honest reason on battery, any in-flight stream is
  stopped, and the sensor is parked (`esp_camera_deinit`, stopping its
  20 MHz clock). Sealed-vault captures (life-safety) and QR provisioning
  (explicit user action) still wake the camera on demand in every mode.
- **Record interval** — the policy value acts as a floor: the effective
  interval is max(operator setting, policy interval).
- **Deep-sleep duty cycling and shutdown** — as described above.

Independent of the battery policy, the camera also parks itself after
5 minutes without use, and a critically hot die (≥ 80 °C) stops the peek
stream and parks the sensor until it cools (vault captures remain
allowed).

The remaining `PolicyFeatures` fields (`csi`, `vision`, `mqtt`, `mesh`,
`gnss`, `touch`, `ir_rmt`, `temp_tamper`) are currently ADVISORY — the
subsystems do not yet consume them, so the per-mode draw estimates above
assume those features keep running. Acoustic detection is deliberately
never gated: a Canary that goes deaf to a smoke alarm to save battery has
failed at its job.

Per-subsystem contributions (typical, for budgeting your own variant):

| Subsystem | Typical | Peak |
|---|---|---|
| ESP32-S3 core, idle @ 80 / 160 / 240 MHz | ~20 / 30 / 45 mA | — |
| WiFi RX/listen | +60–90 mA | — |
| WiFi TX burst | — | +250–350 mA |
| BLE advertise/scan | +5–15 mA | +100 mA (TX, can overlap WiFi) |
| OV2640 camera streaming | +80–150 mA | +200 mA |
| microSD write | +20–40 mA | +100 mA |
| L76K GNSS | +20–30 mA | +35 mA (acquisition) |
| PDM microphone | +1 mA | — |
| Onboard LED / external RGB | +2–10 mA | +50 mA (full white) |

**Deep sleep reality check:** the bare ESP32-S3 module sleeps at ~14 µA, but
a real Canary with the Sense expansion (camera + SD slot), the 200 kΩ battery
divider (~21 µA), and charger-IC quiescent draw typically lands at
**0.1–2 mA**. Don't budget deep sleep from the chip datasheet alone.

---

## 4 · Battery selection and sizing

### 4.1 Chemistry — the climate decision (and a firmware caveat)

| Chemistry | Charge window | Discharge window | Onboard charger? | Firmware SoC accurate? |
|---|---|---|---|---|
| **LiPo / Li-ion** (default) | 0…45 °C | −20…60 °C | ✅ yes | ✅ yes (curve is LiPo-calibrated) |
| **LiFePO4** | 0…55 °C | −20…60 °C | ❌ **no** — needs external 3.6 V charger | ❌ **no** — flat 3.2–3.3 V curve reads as a permanently "low" LiPo |
| **Li-SOCl2 primary** | n/a (non-rechargeable) | −40…+85 °C | ❌ never charge | ❌ no |

> **LiFePO4/Li-SOCl2 builds:** the SoC table, charge-state machine, and the
> power-policy thresholds all assume a LiPo discharge curve. A LiFePO4 cell
> sitting at 3.3 V reads ≈ 2–5% SoC, so the device would falsely live in
> LOW_POWER/SHUTDOWN. For alternative chemistries, power the board through a
> regulated 5 V buck into USB/5V-pin and **do not wire the VBAT divider** —
> the device then runs in USB-only mode and never makes battery decisions on
> wrong data.

### 4.2 Sizing: runtime estimates

Runtime ≈ `capacity × 0.80 (usable) ÷ average draw`. Using the §3 mid-range
estimates, at room temperature, healthy cell:

| Cell | `BATTERY_NORMAL` (~115 mA) | `BATTERY_SAVER` (~60 mA) | `LOW_POWER` duty-cycled (~10 mA) |
|---|---|---|---|
| 500 mAh | ~3.5 h | ~6.5 h | ~40 h |
| 1000 mAh | ~7 h | ~13 h | ~3.3 days |
| 2000 mAh | ~14 h | ~27 h | ~6.7 days |
| 3000 mAh | ~21 h | ~40 h | ~10 days |

In practice a battery-powered Canary walks **down** through the modes, so
end-to-end bridge time on a 2000 mAh cell is roughly **a day** of full
function plus a long low-power tail — comfortably covering multi-hour mains
outages, which is the design role. This is a **UPS/witness-continuity
battery, not a primary power strategy**; for permanently battery-only
deployments, plan solar or a service schedule, and validate with a real
discharge test.

**Charging time:** the onboard charger is ≈100 mA, so a full charge takes
roughly `capacity ÷ 100 mA` *plus* whatever the device is consuming while
awake — a 2000 mAh cell is ~20 h from empty **if the device is in a
low-draw mode**. In `PLUGGED_IN` mode running all features, net charge
current can approach zero; the device deliberately stays feature-rich on USB
and lets charging be slow. If you need fast turnaround, charge with the
device in a quiet state or use an external charger.

### 4.3 Temperature derating (LiPo)

| Ambient | Usable capacity | Charging allowed? | Notes |
|---|---|---|---|
| +25 °C | 100% | ✅ | Reference |
| 0 °C | ~75–85% | ⚠️ borderline — **stop at 0 °C** | Internal resistance up; TX-burst sag → earlier brownout |
| −10 °C | ~60–70% | ❌ **never** (lithium plating) | Expect LOW_POWER earlier than SoC suggests |
| −20 °C | ~50% | ❌ | LiPo discharge floor; consider Li-SOCl2 |
| +45 °C | ~100% but ages 2–4× faster | ⚠️ upper charge limit | Permanent capacity fade accelerates |
| +60 °C | — | ❌ | Discharge ceiling; risk of venting above |

**The board has no battery thermistor** — the charger will happily charge a
frozen cell. If the device can see < 0 °C while on USB power (outdoor,
garage, vehicle), either bring the battery into the enclosure's self-heated
core and verify with the onboard temperature alerts (`sys_monitor` warns
below 0 °C), add a hardware low-temp charge cutoff, or switch chemistry per
§4.1. A sealed enclosure with the device active typically runs **5–10 °C
above ambient**, which helps in cold and hurts in heat — see build plan §9
for enclosure ratings.

### 4.4 Environment profiles

| Deployment | Recommended setup |
|---|---|
| Indoor, climate-controlled | USB 5 V/2 A + 1000–2000 mAh protected LiPo on the pads. Default. |
| Garage / attic (0–50 °C swings) | Same, but verify charge-window: if winter freezes the space, treat as outdoor-freezing. |
| Outdoor, temperate (never < 0 °C) | IP66 polycarbonate enclosure (build plan §6.6), 2000–3000 mAh protected LiPo, GORE vent against condensation, bulk cap per §2. |
| Outdoor, freezing | **No LiPo charging on-site.** LiFePO4 + external low-temp-cutoff charger via 5 V (no divider), or Li-SOCl2 primary + buck, or mains-only with no battery. |
| Vehicle | Treat as freezing *and* hot (cabin > 60 °C in sun): Li-SOCl2 or supercap bridge; never a bare LiPo on the dash. |

---

## 5 · Wiring it right (and what happens if you don't)

### 5.1 Battery connection

```
Protected 3.7 V LiPo cell
  RED   (+) ──► BAT+ pad        ⚠ VERIFY POLARITY WITH A METER FIRST.
  BLACK (−) ──► BAT− pad           Pre-wired JST pigtails are NOT
                                   standardized — reversed leads are
                                   common and destroy the board instantly.
```

- Use cells with an **integral protection PCM** (over-charge/discharge/short).
- Strain-relieve the leads; a chafed lead inside a metal enclosure is a fire
  path.

### 5.2 Battery sense divider (enables all battery telemetry)

```
BAT+ ──► R4 (100 kΩ, 1%) ──┬──► GPIO 1 (A0 / D0)
                           │
                  R5 (100 kΩ, 1%)
                           │
GND  ──────────────────────┘
```

- **Never wire BAT+ directly to GPIO 1.** A full cell (4.2 V) exceeds the
  pin's absolute maximum and can kill the ADC or the SoC. The firmware logs
  `Power: ADC near rail without divider — check wiring` if it sees a
  suspicious near-rail reading at boot.
- GPIO 1 is then dedicated to battery sense — don't double-book it.
- The divider drains ~21 µA continuously (~15 mAh/month). Irrelevant for
  ≥ 500 mAh cells; skip the divider on tiny cells or primary-cell builds.

### 5.3 What the boot log should say

| Your hardware | Expected boot log | Telemetry you get |
|---|---|---|
| Divider + battery wired | `Power: divider + battery detected — HW ADC mode` | Voltage, SoC, charge state, cycles, health, runtime estimate |
| Divider wired, no battery | `Power: divider present, no battery — USB-only mode` | Presence detection keeps watching; attach a battery any time |
| No divider | `Power: no divider detected — USB-only (add divider for battery monitoring)` | None (and the device makes **no** battery-based decisions) |
| VBAT wired straight to GPIO 1 | `Power: ADC near rail without divider` **WARNING** | Power off and fix the wiring |

If you wired a divider and get "no divider detected", check the resistor
values and the GND leg — a broken bottom leg reads near-rail, a broken top
leg reads near-zero.

### 5.4 Known blind spot (by hardware design)

With a divider wired, **USB power, no battery attached**: the charger IC's
open-circuit output sits near 4.2 V, which is indistinguishable from a full
cell. The device may report a full battery that isn't there. If you run
divider-but-no-battery on purpose, expect this; the no-battery detection
catches it only when the float drifts above 4.4 V.

### 5.5 Setup checklist

1. ☐ 5 V/2 A supply, short data-capable USB-C cable.
2. ☐ Cell polarity verified with a meter **before** soldering/connecting.
3. ☐ Protected cell, sized ≥ 1000 mAh (≥ 2000 mAh outdoor).
4. ☐ Divider `BAT+ → 100k → GPIO1 → 100k → GND` if you want telemetry.
5. ☐ Boot log line matches your hardware (table in §5.3).
6. ☐ `GET /api/battery/history` shows `battery_present:true`, sane
   `voltage_mv` (2800–4350), and `brownout_count` not growing.
7. ☐ Pull USB: charge state goes `discharging` within ~1 min; replug:
   `charging`. (Trend-based detection needs ~1 min of samples.)
8. ☐ Outdoor/freezing: chemistry per §4.1, enclosure per build plan §6.6/§9.

---

## 6 · Firmware behavior reference

### 6.1 Monitoring (`power_monitor` / `securacv_power`)

- **Sampling:** ADC every 1 s, 16-sample median filter, eFuse-calibrated
  (±20 mV → ±2% SoC). 60-sample voltage trend (mV/min) drives charge-state
  detection.
- **Charge states:** `charging` / `full` / `discharging` / `low` (≤ 15%) /
  `critical` (≤ 5%) / `no_battery`. Transitions are hysteresis-guarded
  (2–3 consecutive readings) so they can't flap.
  **Charging always wins over low/critical** — a depleted battery on USB
  reports `charging`, so the device never power-saves while mains-powered.
- **Presence is live:** unplugging the battery flips to USB-only after 5
  confirming samples; re-attaching it restores HW-ADC monitoring without a
  reboot.
- **Persisted history (NVS, survives power loss):** charge cycles (counted
  on full 3.8 V→4.1 V swings), total minutes on battery, all-time voltage
  min/max, all-time SoC minimum, brownout count (persisted immediately on
  detection), last-full-charge timestamp. Saved every 10 min and at every
  graceful shutdown.

### 6.2 Health & lifetime estimation

- **`health_pct`** — estimated remaining capacity from a cycle-fade model
  (consumer LiPo ≈ 20% fade per 500 full cycles), computed on demand from
  the persisted cycle count so it can never go stale. Clamped at 60%:
  below that, replace the cell rather than trust a linear model.
  *Rule of thumb: plan replacement at `health_pct ≤ 80` for outage-bridging
  duty, or after ~3 years calendar life, whichever comes first — calendar
  aging is real even at zero cycles.*
- **`est_runtime_min`** — minutes until the 3.3 V cutoff at the *measured*
  discharge slope. Coarse ("hours vs. days" planning). Reports **0 when it
  doesn't know** (charging, trend not yet established, no battery) instead
  of guessing.

### 6.3 Policy modes & graceful degradation (`power_policy`)

- Thresholds: > 50% normal · 15–50% saver · 5–15% low-power · ≤ 3% shutdown.
  Hysteresis: 2 s to downgrade, 5 s to upgrade — no mode flapping at a
  boundary.
- USB/charging always forces `PLUGGED_IN` (full features).
- `LOW_POWER`: 5 s awake / 55 s deep-sleep duty cycle; witness chain state
  and battery history are persisted before every sleep; WiFi STA retained
  during wake windows so panic events still reach Home Assistant.
- `SHUTDOWN`: persists everything, deep-sleeps with a 30 min wake timer to
  re-check (a recharged/charging cell resumes normal operation on wake).
  A **60 s boot warmup** blocks shutdown right after boot so a dead battery
  just plugged into USB charges instead of sleeping.
- Manual mode override **cannot** select LOW_POWER/SHUTDOWN (they create
  unwitnessed windows — only the battery state machine may).

### 6.4 Where to read it

- **Serial:** power monitor + policy status boxes (`print_status`).
- **HTTP:** `GET /api/battery/history` →
  ```json
  {"ok":true,"voltage_mv":3924,"soc_pct":61,"charge_state":"discharging",
   "monitor_mode":"hw_adc","battery_present":true,"divider_detected":true,
   "samples_taken":5412,"min_voltage_mv":3611,"max_voltage_mv":4198,
   "trend_mv_per_min":-2,"charge_cycles":14,"health_pct":99,
   "est_runtime_min":312,"total_runtime_min":8421,"soc_min_pct":9,
   "brownout_count":0,"uptime_sec":5412}
  ```
- **BLE:** standard Battery Service (0x180F) battery level.
- **MQTT / Home Assistant:** the retained `{prefix}/{device_id}/health`
  topic carries `battery` (SoC, or 100 on mains), `battery_present`,
  `charge_state`, `battery_health_pct`, and `battery_mv`. The SecuraCV
  integration's Health sensor derives healthy/warning/critical from it —
  battery thresholds (warning < 25%, critical < 10%) apply only while
  *discharging*, so a charging or mains-powered device never false-alarms —
  and exposes the battery detail as entity attributes for automations.
  Ready-made automations (low-battery push alert, worn-cell replacement
  reminder, recovery notice) ship in
  [`homeassistant/automations/securacv_battery_health.yaml`](../../homeassistant/automations/securacv_battery_health.yaml).
- **Self-test:** `power` metric in the self-test API reports mode, state,
  and divider detection.

---

## 7 · Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Boot loop; "BLE/WiFi init failed"; safe-mode trips | **Brownout** — weak cable/supply, or aged/cold battery sagging under TX | Known-good 5 V/2 A supply + short cable; check `brownout_count`; add bulk cap; replace cell if `health_pct` low |
| `no divider detected` though divider is wired | Wrong resistor values, broken divider leg, wrong pin | Verify 100 kΩ/100 kΩ to GPIO 1 (A0/D0) and the GND leg |
| `ADC near rail without divider` warning | VBAT wired straight to GPIO 1 | **Power off and rewire** — pin overstress |
| Reports full battery with no battery attached | §5.4 blind spot (charger float ≈ full-cell voltage) | Expected with divider + USB + no cell |
| SoC pinned ~100% but runtime is short | Cell aged (voltage high at rest, collapses under load) | Check `health_pct`, `charge_cycles`, `min_voltage_mv` dips; replace cell |
| SoC reads 2–15% constantly on a "good" battery | LiFePO4 or other non-LiPo chemistry | See §4.1 — don't use the divider with non-LiPo chemistries |
| `est_runtime_min` is 0 | Not discharging, or < ~1 min of trend data | Normal — it refuses to guess |
| `charge_cycles` never increases | Cell never swings below 3.8 V and back above 4.1 V (always topped up) | Normal for USB-powered units — that's a *good* sign for cell longevity |
| Charges very slowly / not at all while running | ≈100 mA charger vs. device draw in full-feature mode | Expected; let it charge in a quiet state or charge externally |
| Dies in cold weather at "40% SoC" | Cold derating + internal resistance (§4.3) | Bigger cell, warmer placement, or chemistry change |

---

## 8 · References

- Espressif, *ESP32-S3 Series Datasheet* — current consumption by RF mode,
  sleep modes, ADC characteristics.
- Seeed Studio wiki, *XIAO ESP32S3 (Sense)* — charger behavior, BAT pads,
  deep-sleep figures.
- Internal: [`esp32s3_power_resilience.md`](../esp32s3_power_resilience.md),
  [`esp32s3_thermal_review.md`](../esp32s3_thermal_review.md),
  [`canary_peripheral_build_plan.md`](./canary_peripheral_build_plan.md),
  [`v1_bench_validation_runbook.md`](./v1_bench_validation_runbook.md).
