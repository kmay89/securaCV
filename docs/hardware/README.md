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
| [`canary_vision_pro_recamera.md`](./canary_vision_pro_recamera.md) | **Canary Vision Pro (reCamera Pro) — concept** — the flagship tier: a standalone on-device NPU camera integrated with zero new firmware via the existing webhook adapter, claim mapping, what it deliberately doesn't route (VLM/LLM captions), and where the $300 price earns its keep vs. the cheaper Vision path. Includes a real 3D-modeled mount adapter ([`canary_vision_pro_mount.scad`](./enclosure/canary_vision_pro_mount.scad), mesh-verified). |
| [`canary_vision_lite_recamera.md`](./canary_vision_lite_recamera.md) | **Canary Vision Lite (reCamera 2002w) — concept** — the coverage tier: a ~$35 RISC-V (SG2002) WiFi camera on the same zero-new-firmware webhook pattern as Vision Pro, for several mundane spots instead of one flagship chokepoint. Reuses the Vision Pro mount adapter (same reCamera-family mount interface). |
| [`grove_vision_ai_v2_guide.md`](./grove_vision_ai_v2_guide.md) | **Grove Vision AI V2 device guide** — the two USB-C ports explained, Grove I2C port pinouts per host board, loading the initial AI model via SenseCraft, bootloader recovery, and the SSCMA protocol reference. |
| [`we2_module_flasher_bench_test.md`](./we2_module_flasher_bench_test.md) | **WE2 module flasher bench smoke-test** — the first-hardware gate for the Lab's own module flasher: connect the module port, burn the pinned model, prove it with the AT handshake + live preview, with pass/fail criteria and the Vela-compat fallback. Run once on a real module before calling the flasher production-ready. |
| [`acoustic_alarm_bench_test.md`](./acoustic_alarm_bench_test.md) | Bench test for the audible chirp — does the alarm actually alarm. |
| [`canary_qr_onboarding.md`](./canary_qr_onboarding.md) | QR onboarding — a new Canary joins by looking at the display's screen. |
| [`csi_sensing_guide.md`](./csi_sensing_guide.md) | WiFi CSI sensing on the bench — hardware-side setup and validation. |
| [`mr60bha2_radar_notes.md`](./mr60bha2_radar_notes.md) | **MR60BHA2 radar deep-dive (Canary Sense)** — the ADT6101P all the way down: antenna/FoV, the wire beyond what we decode, placement physics with citations, the power budget derivation, and six bench flags. Its `SIM:` tables are the drift-gated source for the Sense Lab (`canary-local/sense.html`). |
| [`canary_fence_guard_research.md`](./canary_fence_guard_research.md) | **Fence Guard research dossier (concept)** — the Seeed XIAO ESP32S3 + Wio-SX1262 Meshtastic kit, verified: specs, pin map, power measurements, solar guidance, free pins for the vibration sensor. Feeds the coming-soon concept card and `firmware/projects/canary-fence-guard/`. |
| [`canary_vehicle_can.md`](./canary_vehicle_can.md) | **Canary Vehicle — passive CAN bus witness** — the hero feature (ignition on/off → a fleet-wide arrival/departure claim, its own dedicated `ClaimKind`), why it's read-only by design, and the SocketCAN hardware path. Backed by real shipped code: `src/adapter/can_bus.rs` + the `adapter_host` SocketCAN reader — unit-tested, not yet bench-validated against a real vehicle. |
| [`canary_vehicle_profiles.md`](./canary_vehicle_profiles.md) | **Canary Vehicle — multi-brand DBC-sourced signal profiles** — a real, sourced (opendbc, MIT) signal matrix instead of hand-guessed byte offsets, a drift-checked tool (`scripts/dbc_signal_resolve.py`) that resolves signals instead of hand-computing them, and honestly-tiered profiles for Honda Pilot/Odyssey, Toyota Corolla, and Volkswagen MQB. |
| [`vehicle_dbc/SOURCES.md`](./vehicle_dbc/SOURCES.md) | Provenance + sync policy for the vendored DBC excerpts backing the vehicle profiles above. |
| [`bom_pipeline.md`](./bom_pipeline.md) | **How the BOMs run themselves** — design intent (the CSVs) vs fetched supply-chain facts (`pricing.json`), the nightly Digi-Key/Mouser snapshot workflow, the exception policy (out-of-stock / price-jump / lifecycle issues), credentials setup, and the local runbook. |
| [`bom_pipeline_setup.md`](./bom_pipeline_setup.md) | **Hook it in — the setup guide** (radical honesty edition): the two free API keys step by step, the three repo secrets, the first-run checklist with the healthy log, the full failure truth table (every way it can break and what actually happens), what's CI-verified vs designed-but-awaiting-first-live-contact, and how anyone can adopt the pattern. |
| [`bom_canary_wap.csv`](./bom_canary_wap.csv) | Machine-readable BOM — Canary WAP (XIAO ESP32-S3 Sense). |
| [`bom_canary_vision.csv`](./bom_canary_vision.csv) | Machine-readable BOM — Canary Vision (ESP32 host + Grove Vision AI V2). |
| [`bom_canary_sense.csv`](./bom_canary_sense.csv) | Machine-readable BOM — Canary Sense (MR60BHA2 60 GHz kit + XIAO ESP32-C6). |
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
| [`fleet_link_bench_checklist.md`](./fleet_link_bench_checklist.md) | Fleet link — hardware bench smoke-test gate for the direct BLE presence/GATT channel. |
| [`canary_wap_wifi.md`](./canary_wap_wifi.md) | How the Canary WAP's WiFi works — the AP→STA link + provisioning, single-radio coexistence, and WiFi *as a sensor* (CSI presence + probe-request counting). |
| [`display_bench_bringup.md`](./display_bench_bringup.md) | Display bench bring-up — from bare board to a beating face. |
| [`dev_playground_43b.md`](./dev_playground_43b.md) | **Dev playground (bench mode)** — the Waveshare 4.3B as a safe guided peripheral test bench: doorbell/intrusion/light/cap-touch/ToF/beam-gap plus RS485·Modbus and CAN·TWAI stations, the PG1 comms standard, and the fully-loaded pin tracker. |
| [`dev_playground_todo.md`](./dev_playground_todo.md) | **Dev playground — TODO & maintainer handoff** — where every piece of the drift-lock chain lives (firmware ↔ generator ↔ json ↔ sim ↔ test ↔ website carry), the exact ritual to add a station, the gotchas, and the next-station + bench-activation TODO. Start here to continue the playground. |
| [`display_modes.md`](./display_modes.md) | **Display modes — one glass, five gears** — the built, compile-gated mode system: fleet / bench / demo / debug / arcade, the no-bloat contract, host-tested cores + gear runtimes (`src/mode/`), per-mode policy (network/OTA/watchdog), entry/exit choreography, the `-dash-modes`/`-watch-modes` CI envs, and the browser twin behind the public `/modes` page. |
| [`display_peripheral_catalog.md`](./display_peripheral_catalog.md) | **Display peripheral catalog (4.3B)** — the curated what-plugs-in ledger, by wiring surface (DI/DO/I²C/RS485/CAN/radio): why each peripheral, what it's for, honest status, the "not this board" list (cameras/RTSP → where they really live), and the ATM-style combination plays. |
| [`board_capability_map_43b.md`](./board_capability_map_43b.md) | **Board capability map (4.3B)** — an honest, cited ledger of every capability the dash board can do vs. what the firmware drives today, and for each unused one the exact feature gate + bench step to activate it (evidence vault, isolated DI/DO, RS485/Modbus, CAN, microSD, RTC/battery). |
| [`board_43b_activation_bench.md`](./board_43b_activation_bench.md) | **4.3B peripheral activation bench checklist** — the executable companion to the capability map: per capability (field I/O, RS485/Modbus, CAN/TWAI, evidence vault, RTC/battery), the exact wiring, build/flag, pass signal (the `[FIELD]`/`[RS485]`/`[CAN]` log lines), and the honesty-correction/`VERIFY` note each pass retires. |
| [`display_research.md`](./display_research.md) | Display hardware research notes. |

The CSVs use a flat, RoHS-style schema (enforced by `scripts/lint_bom.py`):

```
Item,RefDes,Qty,Required,Category,Description,Manufacturer,MPN,Mouser,DigiKey,LCSC,UnitUSD,ExtUSD,Lifecycle,RoHS,Notes
```

The CSVs carry *design intent*; live distributor SKUs, stock, price breaks
and lifecycle are fetched nightly into [`pricing.json`](./pricing.json) —
see [`bom_pipeline.md`](./bom_pipeline.md). Prices typed in the CSVs are
indicative seed values only.

> **Pin definitions are authoritative in firmware**, not here — see
> [`firmware/boards/`](../../firmware/boards/). This area documents the hardware
> *build*; the pin map uses the firmware defaults, but check the
> **Firmware-Support column** in the build plan — some peripherals (RGB LED,
> tamper) are pin-defined but not yet driven by code.

For getting a finished device online, see
[`../getting_started_canary.md`](../getting_started_canary.md).
