# Grove Vision AI V2 Program — from specialized flavor to first-class Canary

**Status:** adopted roadmap — Phase 0 shipped (#786), runtime detection
config from Phase 1 shipped (#788), HA dashboard + alert automations from
Phase 2 shipped; remaining: provisioning + health logging (Phase 1), SPA
fleet card + logging parity (Phase 2), model lifecycle (Phase 3)
**Owner:** firmware maintainers + dashboard maintainers
**Companions:**
[`docs/hardware/grove_vision_ai_v2_guide.md`](../hardware/grove_vision_ai_v2_guide.md) (device guide) ·
[`firmware/PARITY_PLAN.md`](../../firmware/PARITY_PLAN.md) (canary ⇄ canary-wap program this mirrors) ·
[`firmware/FEATURES.md`](../../firmware/FEATURES.md) (CI-guarded dashboard) ·
[`spec/canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md) (signal vocabulary)

---

## 1. Decision

The Grove Vision AI V2 (Himax WiseEye2 HX6538, on-module NPU inference) is
promoted from "the sensor inside one SPECIALIZED tree" to a **supported
alternate Canary sensor style**: an optical witness whose raw pixels are
physically confined to the sensor module, with the ESP32 host receiving
semantic results only. It complements — not replaces — the camera-bearing
`canary`/`canary-wap` (XIAO ESP32-S3 Sense) and the RF/CSI signal families.

Why it earns the slot:

- **Hard privacy boundary by construction.** Inference runs inside the
  HX6538; the I2C link carries boxes/classes/scores, never frames. That is
  a *stronger* version of the privacy chokepoint than software-discarding
  frames on the host.
- **Host flexibility.** Any 3.3 V I2C host works. We support three:
  ESP32-C3 DevKit, XIAO ESP32-C3, XIAO ESP32-S3 (kit pairing).
- **Model agility.** Person detection today; gesture/face/pose/custom
  YOLOv5–v8 models loadable in minutes via SenseCraft web flasher without
  touching host firmware.
- **Cost.** ~$16 module + ~$5–8 XIAO keeps the BOM near the WAP build.

## 2. Where we are (post-Phase 0)

| Asset | State |
|---|---|
| `firmware/projects/canary-vision/` (SPECIALIZED) | Person presence FSM → MQTT + HA Discovery; signed pull-OTA; Ed25519 witness signing ✅ |
| Build envs | `canary-vision-default` (C3 DevKit), **`canary-vision-xiao-c3`**, **`canary-vision-xiao-s3`** (new) — all in `flavors.json` CI |
| Board defs | `esp32-c3`, **`xiao-esp32c3`** (new), **`xiao-esp32s3`** (new, plain non-Sense) |
| I2C pins | Now explicit from `pins.h` (was: Arduino variant defaults, which contradicted the documented GPIO4/5 DevKit wiring) |
| Device docs | `docs/hardware/grove_vision_ai_v2_guide.md` — dual-USB-C ports, Grove port, SenseCraft model loading, recovery |
| HA integration | `custom_components/securacv` auto-discovers vision devices via MQTT Discovery (no changes needed for new hosts) |
| Kernel path | `grove_vision2_ingest` (event-only serial → sealed log) exists independently |

## 3. Workflow parity principle

**A user who has set up one Canary must be able to set up any other Canary
without learning a new workflow.** Concretely, every flavor converges on:

1. **Same build/flash loop** — `cp secrets.example.h secrets.h` → `pio run
   -e <flavor> -t upload` → `pio device monitor`. (canary-vision: ✅ today.)
2. **Same provisioning** — on-device onboarding (AP + captive portal)
   instead of compile-time secrets. (canary-vision: ⚠️ — the Wi-Fi half
   shipped 2026-08 via the shared setup portal,
   `firmware/common/network/setup_portal`, plus flasher NVS seeding; the
   on-device MQTT/device-ID web UI remains Phase 1.)
3. **Same update story** — signed pull-OTA, HA `update` entity. (✅ —
   shared engine already wired.)
4. **Same observability** — boot banner, health log categories, MQTT
   `status` topic, HA Discovery. (⚠️ — health logging partial; Phase 2.)
5. **Same dashboard surfaces** — device card in the SPA fleet manager and
   HA Lovelace with flavor-appropriate widgets. (❌ — Phase 2.)
6. **Same docs shape** — board README + project README + device guide +
   BOM. (✅ after Phase 0.)

The CI-guarded `FEATURES.md` dashboard is the source of truth for this
convergence; each phase below names the cells it flips.

## 4. Phases

### Phase 0 — Multi-board enablement + documentation (this PR)

- XIAO ESP32-C3 and XIAO ESP32-S3 board defs, build envs, CI flavors.
- Explicit per-board I2C pins in `vision_mgr.cpp`.
- Device guide (dual USB-C, Grove port, SenseCraft model loading, recovery).
- This program doc.

**Exit:** all three envs build green in CI; a C3-on-hand user can assemble,
load the model, flash, and see HA entities using only repo docs.

### Phase 1 — Provisioning & workflow parity

- Runtime provisioning: bring AP + captive-portal onboarding to
  canary-vision so WiFi/MQTT/device-ID are set on-device, not in
  `secrets.h`. **Wi-Fi half shipped (2026-08):** the shared setup portal
  (`firmware/common/network/setup_portal`) raises a `SecuraCV-XXXX` SoftAP
  + captive join wizard when the board is unprovisioned or a saved join
  keeps failing fixably, and the flashers seed credentials into NVS at
  flash time. Still open: MQTT/device-ID set on-device.
- ~~Runtime detection config~~ **shipped (#788)**: `PERSON_TARGET`,
  `SCORE_MIN`, lost/dwell windows are NVS-backed settings exposed as HA
  number entities (`canary/detect_config`), so changing the loaded model
  never requires a rebuild.
- Health logging: emit `HEALTH_CAT_SENSOR` records (module unreachable, ID
  mismatch, invoke timeouts, inference perf drift) through
  `firmware/common/health`, surfaced like other flavors.

**Exit / dashboard flips:** "WiFi AP" and "Web UI" cells for canary-vision
move ❌→✅; FEATURES gains a "runtime detection config" row.

### Phase 2 — Dashboard UX/UI + logging surfaces

- ~~HA Lovelace~~ **shipped**: `securacv-vision-dashboard.yaml` — voxel
  heat grid (3×3 from the `voxel` sensor), presence/dwell glance + history,
  confidence gauge, runtime-tuning view, firmware/OTA view; companion alert
  automations in `securacv_vision_presence.yaml` (dwell/interaction +
  witness-offline).
- **SPA fleet manager (`canary-vision/spa`):** device card template for
  vision canaries (presence state, events-today, voxel mini-grid), a config
  page for the Phase 1 runtime settings, and witness-chain status chip.
- **`custom_components/securacv`:** recognize the board-neutral model
  string; add inference-perf diagnostic sensor (entity-disabled by
  default); device-trigger for `interaction_likely`.
- **Logging:** vision events join the same hash-chain/export verification
  workflow as canary records (`log_verify` parity), and the manual MQTT
  test plan gains a vision section.

**Exit:** a vision canary is indistinguishable from other canaries in
day-2 operations: same dashboards, same verification CLI, same alerting.

### Phase 3 — Model lifecycle & multi-signal vocabulary

- Documented custom-model path (SenseCraft training → deploy → set class
  map at runtime) with a validation checklist (frame size, class indices,
  score calibration).
- New claim types from alternate models, kept within
  `spec/canary_free_signals_v0.md` discipline: e.g. gesture-as-duress
  signal, animal-vs-person disambiguation, zone occupancy from voxels.
  Each new claim needs a spec PR first (Invariant VI: no silent vocabulary
  growth).
- Bench benchmarks in `docs/hardware/bench_bringup.md` style: invoke
  latency, end-to-end event latency, power draw per host, WiFi coexistence.
- Evaluate UART transport for hosts whose I2C bus is taken (mesh nodes).

**Exit:** at least one non-person model shipped end-to-end with spec,
firmware constants, HA entities, and bench numbers.

> **Worked example:**
> [`14-pose-estimation-v2-ai.md`](14-pose-estimation-v2-ai.md) takes the
> "Live Pose→3D" Grove Vision AI project and designs how a YOLOv8-Pose model
> would satisfy this exit criterion — the pose *model* runs on the HX6538 we
> already drive, while the skeleton *stream* is refused (Invariant I: it is
> approximately-reversible raw media). It ships one coarse physical claim
> (`pose_horizontal_sustained` — fall/collapse), spec-first.

## 5. SenseCraft path — considered and rejected

Seeed's own Home Assistant route (SenseCraft adapter firmware on the XIAO +
SenseCraft Data Platform account + HACS plugin, per their
[tutorial](https://wiki.seeedstudio.com/sensecraft-ai/application/application-for-homeassistant/))
was evaluated and **rejected** for production use:

| Seeed path | Our requirement |
|---|---|
| Vendor account + SenseCraft Data Platform in the loop | Local ownership, no cloud (Invariant IV) |
| Adapter firmware streams module output as-is | Our chokepoint: FSM + voxel coarsening + signing in firmware we control |
| Generic MQTT payloads, no signing | Ed25519-signed, hash-chained claims |
| Closed `.bin` adapter firmware | Auditable open firmware |

We keep SenseCraft for exactly one job: **the web flasher that loads models
onto the Himax module over its own USB port** (an offline, one-time,
physically-attended operation). Module model loading ≠ device telemetry.

## 6. Risks & mitigations

- **Class-index drift across models** (`PERSON_TARGET` assumption) →
  Phase 1 runtime class map; guide §4 step 8 documents the check today.
- **Dual-USB-C confusion bricking sessions** → guide §2; consider printed
  port labels in the enclosure design (docs/hardware enclosure work).
- **SSCMA library pin (v1.0.3) going stale** vs module firmware updates →
  pin is in one place (`envs/platformio/canary-vision.ini`); add a
  quarterly check to the COMPATIBILITY-style sweep.
- **XIAO C3 has no user LED** for field status → external LED pad documented
  in the board README; SPA/HA status remains the primary signal.
- **Backwards-seated XIAO destroys hardware** → warning in guide + board
  READMEs; enclosure keys the orientation (Phase 2 enclosure rev).
