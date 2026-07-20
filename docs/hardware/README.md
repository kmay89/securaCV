# Canary Hardware Documentation

Build plans and bills of materials for the physical SecuraCV Canary devices —
what to buy, how peripherals wire to the MCU, and which parts are required vs.
optional.

| Document | Description |
|----------|-------------|
| [`bench_bringup.md`](./bench_bringup.md) | **Start here to test it** — minimum shopping list and steps to make a bare XIAO chirp/blink on the bench. |
| [`v1_bench_validation_runbook.md`](./v1_bench_validation_runbook.md) | **The v1 release gate** — flash → provision → signed MQTT → verified-✓ in HA, the kernel-pipeline smoke, and the 2–3 board mesh/chirp fleet validation, with pass/fail criteria and required artifacts. |
| [`enclosure/`](./enclosure/README.md) | **3D-printable enclosures, mounts & workshop tools** (parametric OpenSCAD + ready STLs) for every Canary: WAP box, Vision camera unit, doorbell, Sense radome — plus mounts, a fit-calibration coupon, bench fixture, provisioning dock and 1:1 paper drill templates. **New? Start with its README's "first hour" section.** |
| [`canary_peripheral_build_plan.md`](./canary_peripheral_build_plan.md) | Master build plan & BOM: audible chirp (buzzer), status LED, button/tamper/touch inputs, battery, and enclosure — for both Canary WAP and Canary Vision. |
| [`esp32s3_power_battery_guide.md`](./esp32s3_power_battery_guide.md) | **Power & battery guide** — supply/cable requirements, consumption estimates per firmware power mode, battery chemistry/sizing across temperatures and environments, wiring pitfalls, and the battery health/lifetime telemetry reference. |
| [`canary_vision_getting_started.md`](./canary_vision_getting_started.md) | **Canary Vision getting started** — one clean path from unboxing to a working witness: assemble, load the model, flash, HA discovery, dashboard, and aiming with the boxes-only Aim camera card. |
| [`grove_vision_ai_v2_guide.md`](./grove_vision_ai_v2_guide.md) | **Grove Vision AI V2 device guide** — the two USB-C ports explained, Grove I2C port pinouts per host board, loading the initial AI model via SenseCraft, bootloader recovery, and the SSCMA protocol reference. |
| [`acoustic_alarm_bench_test.md`](./acoustic_alarm_bench_test.md) | Bench test for the audible chirp — does the alarm actually alarm. |
| [`canary_qr_onboarding.md`](./canary_qr_onboarding.md) | QR onboarding — a new Canary joins by looking at the display's screen. |
| [`csi_sensing_guide.md`](./csi_sensing_guide.md) | WiFi CSI sensing on the bench — hardware-side setup and validation. |
| [`canary_fence_guard_research.md`](./canary_fence_guard_research.md) | **Fence Guard research dossier (concept)** — the Seeed XIAO ESP32S3 + Wio-SX1262 Meshtastic kit, verified: specs, pin map, power measurements, solar guidance, free pins for the vibration sensor. Feeds the coming-soon concept card and `firmware/projects/canary-fence-guard/`. |
| [`bom_canary_wap.csv`](./bom_canary_wap.csv) | Machine-readable BOM — Canary WAP (XIAO ESP32-S3 Sense). |
| [`bom_canary_vision.csv`](./bom_canary_vision.csv) | Machine-readable BOM — Canary Vision (ESP32 host + Grove Vision AI V2). |
| [`bom_canary_display.csv`](./bom_canary_display.csv) | Machine-readable BOM — Canary Display (watch & dash variants). |

## The display family (watch station & dash)

The Canary Display's design record, from platform vision to bring-up:

| Document | Description |
|----------|-------------|
| [`display_platform_vision.md`](./display_platform_vision.md) | Why the family has a face — the display platform's north star. |
| [`display_trailblazer_spec.md`](./display_trailblazer_spec.md) | The trailblazer device spec — first hardware to carry the vision. |
| [`display_ux_design.md`](./display_ux_design.md) | Screen-by-screen UX design. |
| [`display_character.md`](./display_character.md) | The canary character — how the mascot behaves and why. |
| [`display_living_canary.md`](./display_living_canary.md) | The living-canary mood engine — honest moods mirroring system health. |
| [`display_care_wave.md`](./display_care_wave.md) | The care wave — gentle presence signals between households. |
| [`display_nightstand.md`](./display_nightstand.md) | The nightstand ("watch") variant — sleep, glanceability, bedside manners. |
| [`display_onboarding.md`](./display_onboarding.md) | On-glass onboarding flows. |
| [`display_settings.md`](./display_settings.md) | On-glass settings — what's adjustable without an app. |
| [`display_discovery_and_resilience.md`](./display_discovery_and_resilience.md) | Discovery & resilience — finding sensors, surviving outages. |
| [`display_bench_bringup.md`](./display_bench_bringup.md) | Display bench bring-up — from bare board to a beating face. |
| [`dev_playground_43b.md`](./dev_playground_43b.md) | **Dev playground (bench mode)** — the Waveshare 4.3B as a safe guided peripheral test bench: doorbell/intrusion/light/cap-touch/ToF/beam-gap stations, the PG1 comms standard, and the fully-loaded pin tracker. |
| [`display_research.md`](./display_research.md) | Display hardware research notes. |

The CSVs use a flat, RoHS-style schema:

```
Item,RefDes,Qty,Required,Category,Description,Manufacturer,MPN,Mouser,DigiKey,LCSC,UnitUSD,ExtUSD,Lifecycle,RoHS,Notes
```

> **Pin definitions are authoritative in firmware**, not here — see
> [`firmware/boards/`](../../firmware/boards/). This area documents the hardware
> *build*; the pin map uses the firmware defaults, but check the
> **Firmware-Support column** in the build plan — some peripherals (RGB LED,
> tamper) are pin-defined but not yet driven by code.

For getting a finished device online, see
[`../getting_started_canary.md`](../getting_started_canary.md).
