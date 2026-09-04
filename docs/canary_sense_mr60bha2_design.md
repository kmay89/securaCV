# Canary Sense — MR60BHA2 60GHz mmWave Witness Design & Development Plan

Status: **in progress** — Phase 0 (toolchain + sensing core) and Phase 2
(MQTT + HA discovery + signed pull-OTA + runtime config + Ed25519 witness
signing with NVS hash chain and HA-verified trust surface) are in the tree;
the hardware bench passes remain (see
`firmware/projects/canary-sense/README.md` for live status)
Hardware: Seeed Studio MR60BHA2 60GHz mmWave Kit (radar module + XIAO ESP32-C6)
Firmware project (new): `firmware/projects/canary-sense`
Sibling hardware (follow-on, same protocol family): MR60FDA2 fall-detection kit
Hardware deep-dive (chip, placement physics, power, bench flags):
[`docs/hardware/mr60bha2_radar_notes.md`](./hardware/mr60bha2_radar_notes.md) ·
Interactive bench: **the Sense Lab**, `canary-local/senselab.html` (drift-gated
against this firmware; widget-card layer in
[`docs/standard/CANARY_CARDS.md`](./standard/CANARY_CARDS.md))

---

## 1. Why this sensor

SecuraCV's thesis is *witnessing without watching*. The MR60BHA2 is the first
candidate canary sensor that is **radar-native**: it produces presence and
vital-sign claims with **no camera, no microphone waveform, no MAC addresses,
and no identity surface at all** — the raw radar IQ data never leaves the
radar module's own DSP (an ADT6101P with on-chip Cortex-M3 signal processing).
The host MCU only ever sees pre-digested scalars over UART: presence flag,
target count, distance, breathing rate, heart rate.

That makes it the strongest privacy story of any canary so far:

| | canary-vision (Grove AI V2) | canary-wap (WiFi CSI) | **canary-sense (MR60BHA2)** |
|---|---|---|---|
| Sensing medium | camera (on-module inference) | WiFi channel distortion | 60GHz FMCW radar |
| Identity surface at sensor | image exists on module | none | none |
| Works in darkness | poor | yes | yes |
| Through blankets/light cover | no | partially | yes |
| Static (sleeping) presence | no (needs visible person) | weak | **yes, up to ~6 m** |
| Breathing rate | no | best-effort (CSI FFT, 6–27 BPM) | **~90% accuracy, ≤1.5 m** |
| Heart rate | no | no | **~85% accuracy, ≤1.5 m** |
| Target count | bbox count | aggregate only | yes |
| Ambient light | no | no | yes (BH1750, 1–65,535 lux) |
| Affected by light/temp/dust | yes (camera) | no | no |

It also *complements* rather than replaces the existing canaries: CSI presence
(canary-wap) and radar presence (canary-sense) fail independently — different
physics, different interference modes — which is exactly the multi-witness
corroboration model the kernel's contract layer was built for.

### 1.1 Hardware summary

- **Radar module**: MR60BHA2, 57–64 GHz FMCW (ADT6101P SoC: 2T2R antenna,
  1 MB flash, Cortex-M3 DSP). Detection field ≈ 80° × 80° sector.
  - Static human presence: 1.5–6 m
  - Breathing rate (~90% acc) and heart rate (~85% acc): ≤1.5 m
  - Distance-to-target and target count
- **Host MCU**: XIAO ESP32-C6 (RISC-V, WiFi 6, BLE 5, 802.15.4
  Zigbee/Thread-capable), USB-C, same XIAO footprint as our ESP32-S3/C3 boards.
- **Radar ↔ host link**: UART, 115200 8N1.
- **On-board peripherals**: BH1750 lux sensor (I2C, addr 0x23), WS2812 RGB LED
  (GPIO1) — both already familiar patterns in our HAL.
- **Stock firmware**: ships pre-flashed with ESPHome (`seeed_mr60bha2`
  component, upstream in ESPHome ≥ 2024.3) exposing: person presence
  (binary), breath rate, heart rate, distance, target count, illuminance.
- **Arduino path**: official Seeed Arduino mmWave library
  (`Seeed-Studio/Seeed_Arduino_mmWave`, MR60BHA2 + MR60FDA2) gives us full
  control of the UART frame protocol for a native canary firmware.
- **Enclosure**: a parametric 3D-printable **RADOME housing** exists —
  [`docs/hardware/enclosure/canary_sense_enclosure.scad`](./hardware/enclosure/canary_sense_enclosure.scad)
  ([README section](./hardware/enclosure/README.md#canary-sense--radome-enclosure-v02)).
  The front over the antenna zone is a thin, flat, uniform membrane (no ribs
  or labels there — 60 GHz radar transparency demands it; the air gap behind
  it is computed and asserted in the model). A `radar = "fda2"` flavor covers
  the fall-detection sibling (ceiling mount, 2.4–3.1 m, facing down); an
  in-wall single-gang faceplate (`canary_sense_gang.scad`), bedside stand
  (`canary_sense_stand.scad`) and radar+camera combo (`canary_combo.scad`)
  are in development in the same folder.
- **Constraint**: the radar module's own firmware (zone configuration etc.)
  is only modifiable under a Seeed business partnership. We treat the radar
  module as a fixed black-box claim source and do all SecuraCV work on the
  ESP32-C6 host.

References: Seeed wiki (`getting_started_with_mr60bha2_mmwave_kit`),
ESPHome `seeed_mr60bha2` component docs, Seeed `xiao-esphome-projects`
reference YAML (UART 115200 on GPIO16/17, I2C on GPIO22/23, WS2812 on GPIO1).

---

## 2. Scenario fit — what each capability becomes in SecuraCV

Every capability must pass the privacy chokepoint and map 1:1 onto the event
contract (`spec/event_contract.md`, `src/adapter/contract.rs`). Proposed
mapping:

| Sensor capability | Claim (`ClaimKind`) | Event (`EventType`) | Privacy class | Notes |
|---|---|---|---|---|
| Person presence (binary) | `PresenceInRestrictedZone` | `PresenceInRestrictedZone` | P0 | FSM-debounced like canary-vision (presence → dwelling) |
| Target count | `PresenceInRestrictedZone` (count as confidence-weighted aggregate attribute) | same | P0 | bucketed counts only (0 / 1 / 2+), never a track log |
| Distance to target | *not exported as an event* | — | P2 | used on-device for zone gating only; raw distance never leaves device (coarse `near/mid/far` may appear in HA diagnostics) |
| Breathing detected (binary lock) | wellbeing signal (health channel, not sealed-log event) | — | P0 | mirrors `core.breathing` semantics from CSI modules (`docs/csi_modules.md`): `breathing_confirmed` / `breathing_lost` |
| Breathing rate (BPM numeric) | wellbeing signal | — | **P1 opt-in** | same gate as CSI BPM today |
| Heart rate (BPM numeric) | wellbeing signal | — | **P1 opt-in** | new signal class; never sealed-logged, never precise-timestamped, HA-local only |
| Lux (BH1750) | tamper corroboration | `TamperDetected` (corroborating attribute) | P0 | "lights-out + presence" and camera-blind cross-checks |
| Radar self-check failure (UART silent, frame CRC errors) | — | health log | P0 | `HEALTH_CAT_SENSOR` |

### 2.1 Concrete deployment scenarios ("canary alternate sensor style")

1. **Restricted-zone presence witness (workshop, server closet, storage
   cage).** Ceiling/wall mount, presence FSM emits signed
   `PresenceInRestrictedZone` claims with coarse time buckets. Works in total
   darkness where canary-vision can't, and indoors where canary-wap CSI is
   noisy (HVAC airflow doesn't affect radar).
2. **After-hours occupancy corroboration.** Pairs with Frigate/canary-vision:
   radar presence + camera person-detection in the same zone bucket gives a
   two-physics corroborated event — much stronger evidentiary weight in the
   sealed log, and a contradiction (camera says person, radar says empty)
   surfaces as an anomaly worth flagging.
3. **Wellbeing / welfare-check canary (elder care, lone-worker, cell-check
   adjacent).** Bedside mount (≤1.5 m): "breathing confirmed within bucket"
   as a P0 binary, BPM numerics under P1 opt-in. This is *witnessing that a
   person is alive and present* without a camera in a bedroom — a deployment
   the vision canary can never ethically serve. The MR60FDA2 fall-detection
   sibling extends this same niche later with the same library and UART
   framing.
4. **Tamper enrichment.** BH1750 gives every canary-sense node a free
   "camera-blind"-style signal: sudden lux collapse while presence persists
   feeds the existing tamper-type sensors.
5. **Sleep/quiet-hours anomaly baseline.** Feed presence + breathing-lock
   scalars into the existing `anomaly.baseline` pattern (unusual motion /
   unusual breathing) so dashboards get the same aurora activity ribbon the
   CSI canary renders today.

### 2.2 Explicitly out of scope (privacy contract)

- No heart/breath waveform or phase data export (the Arduino lib exposes
  phases; we read and drop them at the chokepoint).
- No continuous distance tracking / trajectory reconstruction.
- No per-target IDs even when target count > 1.
- Vitals are **wellbeing signals, not medical data**: never sealed-logged,
  never exported in evidence bundles, P1-gated, and documented as
  non-diagnostic (85–90% accuracy radar estimates).

---

## 3. Firmware strategy — two tracks, one contract

### Track A (primary): native canary firmware `canary-sense`

Full SecuraCV witness device on the XIAO ESP32-C6 host: Ed25519 per-device
keys, hash-chained signed records, MQTT discovery + multi-transport publish,
OTA — identical trust posture to canary-wap/vision.

Layout (follows `firmware/ARCHITECTURE.md` layering — `common/` never imports
`boards/`/`configs/`; composition only in `projects/` + `envs/`):

```
firmware/
├── boards/xiao-esp32c6-mr60/          # NEW board def
│   └── pins/pins.h                    # UART radar TX16/RX17, I2C SDA22/SCL23,
│                                      # WS2812 GPIO1, BOOT button
├── common/sensors/mmwave_mr60/        # NEW board-agnostic radar driver wrapper
│   ├── mr60_uart.{h,cpp}              # frame parser (vendored or via Seeed lib)
│   ├── mr60_presence.{h,cpp}          # presence/count/distance FSM (P0)
│   └── mr60_vitals.{h,cpp}            # breathing/heart lock + P1 gating
├── configs/canary-sense/
│   ├── default/config.h               # presence-only: VITALS=0
│   └── wellbeing/config.h             # vitals build: VITALS=1, P1 plumbing
├── projects/canary-sense/
│   ├── platformio.ini                 # extends envs/platformio/common.ini
│   ├── src/main.cpp                   # compose: sensor → chokepoint → witness
│   └── README.md
└── envs/platformio/canary-sense.ini   # canary-sense-default / -wellbeing / -debug
```

Reuse from `common/`: `witness/` (chain + signing), `encoding/` (CBOR),
`health/` (health_log, `HEALTH_CAT_SENSOR`), `network/` (MQTT + discovery),
`ota/`, `storage/` (NVS). The only genuinely new firmware code is the UART
frame parser + the two FSMs (~comparable to canary-vision's `vision_mgr`).

Implementation notes for the new module (design constraints, not code):

- **Flag propagation respects layering**: `common/sensors/mmwave_mr60/` never
  includes `configs/canary-sense/config.h`. The vitals switch reaches the
  library translation units as a build flag (`-DCANARY_SENSE_VITALS=1`) set
  in `envs/platformio/canary-sense.ini` per environment, mirroring how other
  feature flags cross the config→common boundary today.
- **Stall-safe FSMs**: in `mr60_uart` / `mr60_vitals`, deadline checks run
  *before* data-presence guards, so a silent radar UART still drives the FSM
  to its timeout state (presence→unknown, vitals-lock→lost, health-log
  `HEALTH_CAT_SENSOR` event) instead of freezing on the last good frame.
  All timestamp math is wrap-safe signed-delta (`(int32_t)(now - then)`),
  per the existing firmware idiom for `millis()` arithmetic.

**ESP32-C6 platform risk (the one real unknown):** our PlatformIO envs build
on `espressif32` for S3/C3 (Xtensa/RISC-V). The C6 needs arduino-esp32 3.x;
official PlatformIO `espressif32` lags on C6 support, so the env will likely
pin the community `pioarduino` platform fork. That also means **arduino-esp32
core API drift**: the C6 env runs core 3.x while S3/C3 envs stay on 2.0.x, and
several shared-`common/` touchpoints changed signatures between the two (e.g.
`mbedtls_sha256()` returns `void` on 2.0.x but `int` on 3.x; LEDC, ADC and
WiFi event APIs also moved). Any `common/` code compiled into canary-sense
must be audited for 2.x/3.x compatibility (version-gated shims where needed)
rather than assuming one signature. Phase 0 below is a 1–2 day
spike to validate: C6 toolchain in CI, NimBLE on C6, hardware RNG + Ed25519
(our `Crypto` dep) on RISC-V, core 2.x/3.x API audit of the `common/` modules
we link, and OTA partition layout for the C6's 4 MB
flash. Fallback if the spike fails ugly: wire the bare MR60BHA2 module
(sold standalone) to a XIAO ESP32-S3 over UART and keep our proven board
support — the kit's C6 becomes optional rather than blocking.

**Radar protocol dependency:** prefer vendoring a minimal frame parser
(presence / count / distance / breath / heart frames only) over pulling the
full Seeed Arduino library, for the same supply-chain and size-guard reasons
canary-vision pins SSCMA. Library stays the reference implementation.

### Track B (on-ramp, near-zero effort): stock ESPHome firmware + backend adapter

One wrinkle the plan must be honest about: the **stock kit firmware speaks
the ESPHome native API to Home Assistant, not MQTT** — Seeed's reference YAML
enables `api:` with no `mqtt:` block (see `ha_with_mr60bha2` wiki page). Our
generic `mqtt_sensor` adapter (`src/adapter/mqtt_sensor.rs`) only consumes
MQTT topics, so a kit deployed as-shipped never reaches the kernel on its
own. Two supported bridge paths, both config-only on the device side:

1. **HA MQTT Statestream (preferred, kit stays unmodified).** Enable HA's
   built-in `mqtt_statestream` for the kit's entities; HA republishes their
   states to MQTT and the `mqtt_sensor` adapter's `mr60bha2` profile maps
   those statestream topics into claims. Zero device changes — the
   "10-minute on-ramp" is an HA YAML snippet we ship verbatim in the
   integration doc. Caveat: claims now transit HA, so the adapter profile
   marks provenance as `ha-bridged` in the claim metadata.
2. **ESPHome `mqtt:` overlay (one OTA, no custom firmware).** Add an
   `mqtt:` block to the kit's ESPHome config and push it over the air with
   ESPHome Web/Dashboard. Still vendor firmware, but the device then
   publishes to the broker directly, skipping the HA hop. We publish the
   known-good YAML diff alongside the adapter recipe.

We ship:

- an `adapter_host.example.toml` recipe + `docs/integrations` page covering
  both bridge paths and mapping the ESPHome entities (`has_target`,
  `breath_rate`, `heart_rate`, `distance`, target count, illuminance) into
  claims with the same privacy table as §2,
- a documented caveat: Track B claims are **kernel-signed at ingest, not
  device-signed** — the HA trust badge must render these as
  "adapter-attested" (yellow) not "device-verified ✓" (green). This
  distinction already exists in the trust model; we make the UI honest
  about it.

Track B is the demo/eval funnel ("buy kit, claims flowing in ~10 minutes of
HA configuration"); Track A is the product. Both speak the same claim
vocabulary, so dashboards, logging and verification are shared. A native
ESPHome-API adapter in the kernel (speaking the aioesphomeapi protocol
directly, no HA in the loop) stays on the shelf as a Phase 4+ option if
Track B uptake justifies it.

---

## 4. Backend plan (Rust kernel)

1. **No new `ClaimKind` needed for presence** — `PresenceInRestrictedZone`
   covers it (contract stays frozen; good).
2. **Adapter**: extend `mqtt_sensor` route config (Track B) with an
   `mr60bha2` profile (entity-name → claim mapping, confidence defaults,
   target-count bucketing). Track A devices arrive via the existing canary
   MQTT path (`securacv/<device_id>/events`, signature-verified against
   pinned pubkeys in `device_pubkeys`), so the kernel needs **zero schema
   change** for presence.
3. **Wellbeing channel**: vitals deliberately bypass the sealed log. They ride
   the health/status topic (like battery and breathing-score do today) and
   exist only as HA entities. Enforced by the contract enforcer: adapter
   descriptor for the mr60 profile allowlists only
   `PresenceInRestrictedZone` (+ `TamperDetected` for lux corroboration), so
   a buggy or malicious payload physically cannot seal a heart-rate record.
4. **Corroboration (stretch, Phase 4)**: a small post-ingest annotator that
   marks events sharing (zone, time_bucket) across ≥2 independent adapters
   with a `corroborated_by` metadata field — additive, doesn't touch the
   chain format.

---

## 5. Dashboard / UX-UI plan (Home Assistant integration + cards)

New entities (all carrying the existing `extra_state_attributes` trust
verdict from `device_trust.py` / `signature.py`):

| Entity | Platform | Class | Notes |
|---|---|---|---|
| `binary_sensor.<id>_presence` | binary_sensor | occupancy | reuses canary-vision presence/dwelling FSM pattern |
| `sensor.<id>_occupants` | sensor | none | bucketed: 0 / 1 / 2+ |
| `binary_sensor.<id>_breathing` | binary_sensor | none | P0 "breathing confirmed" lock |
| `sensor.<id>_breath_rate` | sensor | none (BPM) | **created only when P1 opt-in flag present in discovery payload** |
| `sensor.<id>_heart_rate` | sensor | none (BPM) | same P1 gate |
| `sensor.<id>_illuminance` | sensor | illuminance | BH1750 lx |
| `sensor.<id>_range_band` | sensor (diagnostic) | none | near/mid/far only |
| `update.<id>` | update | firmware | existing OTA pattern |

UI work:

1. **Timeline card** (`custom_components/securacv/www/securacv-timeline-card.js`):
   - new event glyph/row style for radar-sourced presence (distinguish
     sensing modality at a glance: camera / CSI / radar / contact),
   - render the `corroborated_by` badge when two modalities agree in a bucket,
   - trust badge tri-state already exists; ensure Track B "adapter-attested"
     renders distinctly from device-verified.
2. **Wellbeing tile** (new small Lovelace card or documented stock-card
   blueprint in `docs/homeassistant_automations.yaml` + `docs/blueprints/`):
   presence + breathing-lock + (if P1) BPM sparkline + "last confirmed alive"
   bucket. This is the welfare-check product face.
3. **Aurora activity ribbon**: feed canary-sense presence transitions into the
   same 96-slot ribbon the CSI canary renders, so mixed fleets look uniform.
4. **Canary Vision SPA fleet manager** (`canary-vision/spa/`): add device-type
   awareness (`device_type: "canary-sense"` in status payload) so discovery,
   identify (LED blink — WS2812 present), logs, and witness-chain views work
   unchanged; hide camera-specific panels for radar devices.
5. **Blueprints**: ship HA automation blueprints for (a) after-hours radar
   presence alert, (b) lights-out-with-presence tamper notice, (c) welfare
   "no breathing lock in N buckets during sleep window" notification —
   mirroring the existing battery automation blueprints pattern.

---

## 6. Logging & observability plan

- **Firmware health log** (the project's `include/canary/log.h`): new events under
  `HEALTH_CAT_SENSOR` — radar UART timeout, frame CRC failure, radar reboot
  detected, vitals-lock acquired/lost; severities per existing convention.
- **Sealed log**: presence/tamper claims only (per §2 table), standard
  10-minute bucket coarsening, monotonic sequence, Ed25519 — no format
  changes.
- **Heartbeats**: existing per-bucket heartbeat records cover canary-sense
  automatically once it publishes on the canary MQTT contract.
- **HA diagnostics**: per-transport health sensors and trust verdicts come
  free from the existing integration; add radar-link health (last frame age)
  as a diagnostic sensor.
- **docs/logging.md**: add a canary-sense section documenting which signals
  are P0/P1/P2 and *why heart rate never appears in the sealed log* — this
  doc line is the auditable promise.

---

## 7. Phased roadmap

**Phase 0 — Hardware/toolchain spike (1–2 days)**
C6 PlatformIO env builds in CI; UART frames parse on bench; Ed25519 + NVS +
NimBLE + WS2812 verified on C6; flash/OTA partition plan. Go/no-go on C6 vs
S3-fallback wiring. Exit: `envs/platformio/canary-sense.ini` compiling a
hello-witness binary, bench notes in `docs/hardware/`.

**Phase 1 — Track B on-ramp (2–3 days, parallelizable with Phase 0)**
ESPHome-kit → HA `mqtt_statestream` bridge snippet + `mqtt_sensor` adapter
`mr60bha2` profile + `adapter_host.example.toml` recipe +
`docs/integrations/mr60bha2_esphome.md` (covering both the statestream and
ESPHome-`mqtt:`-overlay paths) + "adapter-attested" badge rendering check.
Exit: stock kit (unmodified, statestream-bridged) produces verified-ingest
`PresenceInRestrictedZone` events on the timeline card with honest trust
badge.

**Phase 2 — `canary-sense` presence firmware (1–1.5 weeks)**
Board def, frame parser, presence/count FSM, privacy chokepoint, witness
chain + MQTT discovery + OTA, `flavors.json` + CI + size guard, BH1750 +
LED. Exit: device-signed presence events verify green in HA; host-side FSM
unit tests in `firmware/tests_host/`; `docs/getting_started_canary` section.

**Phase 3 — Wellbeing channel + dashboard UX (1 week)**
`wellbeing` config flavor (P1 gating end-to-end: config.h → discovery
payload → HA entity creation), breathing-lock P0 binary, wellbeing tile,
blueprints, timeline modality glyphs, SPA device-type support, logging doc
updates. Exit: P1 entities provably absent unless opted in (integration
test), welfare-check demo runbook.

**Phase 4 — Fleet polish & corroboration (3–5 days, stretch)**
`corroborated_by` annotator + timeline badge; anomaly-baseline reuse for
radar scalars; mixed-fleet bench validation runbook
(`docs/hardware/v1_bench_validation_runbook.md` addendum); MR60FDA2
fall-detection assessment memo (same library/framing — likely a config
flavor + one new claim mapping discussion, since "fall" has no current
`EventType` and would need a contract-review, the only place this program
touches the frozen contract).

Separately tracked: the kernel export stamping `attestation`
(`adapter-attested` / `ha-bridged`) on Track B events — the field the
Phase 3 timeline chips render once populated — is issue #800, not part
of the Phase 4 scope above.

Also separately scoped (software-only, no new hardware): a **coarse
mover-class** (large-vs-small mover) derived on-device from micro-Doppler
features, riding as a confidence-weighted attribute on the same
`PresenceInRestrictedZone` claim (no dictionary drift) — see
[`canary_sense_coarse_class_design.md`](./canary_sense_coarse_class_design.md).
It inherits §2.2 wholesale (class, not identity; single-window, no tracking).

Total: ~4 engineering weeks to a shippable canary-sense product with the
Track B funnel live in week 1.

## 8. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| ESP32-C6 toolchain immaturity in PlatformIO | blocks Track A | Phase 0 spike; S3 + bare-module fallback wiring |
| Radar module firmware is closed (zone control partnership-gated) | can't tune zones in-radar | do zone gating host-side from distance/range-band before the chokepoint |
| Vitals accuracy (85–90%, ≤1.5 m, single target, mounting-sensitive) | wellbeing claims overtrusted | P1 gating, "non-diagnostic" labeling in UI + docs, breathing-*lock* binary as the only P0 vitals signal |
| Multi-person vitals ambiguity | wrong BPM attribution | suppress vitals entities whenever target count ≠ 1 (firmware FSM rule) |
| Seeed library supply chain / size | bloat, drift | vendor minimal frame parser, pin versions, CI size guard like canary-vision |
| Track B unsigned-at-device claims mistaken for device-verified | trust dilution | distinct "adapter-attested" badge; docs call it out |
| Stock kit speaks ESPHome native API, not MQTT | Track B silently produces no claims | HA `mqtt_statestream` bridge (config-only) or ESPHome `mqtt:` overlay; both documented in Phase 1, provenance marked `ha-bridged` |

## 9. Decision summary

- Adopt MR60BHA2 as the third canary sensing modality (radar), project name
  **`canary-sense`**, dual-track: native witness firmware (product) +
  stock-ESPHome adapter profile (10-minute on-ramp).
- Presence/count/lux ride the existing frozen event contract unchanged;
  vitals are P1-gated wellbeing signals that never enter the sealed log.
- Dashboard gains modality-aware timeline rendering, a wellbeing tile, and
  honest trust-badge differentiation; logging gains radar-link health and an
  auditable privacy-class table.
- MR60FDA2 (falls) is the natural follow-on and the only piece that would
  ever touch the event contract.
