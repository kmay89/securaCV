# SecuraCV Canary Sense (XIAO ESP32-C6 + MR60BHA2)

Privacy-preserving **radar-native** witness firmware: 60GHz FMCW mmWave presence
(and optional P1-gated wellbeing vitals) on the Seeed MR60BHA2 kit. No camera,
no microphone, no MAC surface — the raw radar IQ never leaves the radar module's
own DSP; the host MCU only ever sees pre-digested scalar claims over UART.

Design + roadmap: [`docs/canary_sense_mr60bha2_design.md`](../../../docs/canary_sense_mr60bha2_design.md).
Hardware deep-dive (chip, placement physics, power budget, bench flags):
[`docs/hardware/mr60bha2_radar_notes.md`](../../../docs/hardware/mr60bha2_radar_notes.md).
Try it before the bench: **the Sense Lab** (`canary-local/senselab.html`) stages
this firmware's wire protocol, FSM semantics, privacy chokepoint, placement
geometry and power model in the browser — drift-gated against this project's
source by `canary-local/tools/gen_senselab.py`.

> **Status: Phase 2 complete (signing witness).** On top of the Phase 0
> sensing core (UART frame decoder + stall-safe presence/vitals FSMs,
> host-tested in `firmware/tests_host/`) the firmware carries the same
> network stack as canary-vision — NVS-backed runtime config, supervised
> WiFi STA (exponential backoff + outage reboot), non-blocking broker
> supervision, MQTT with LWT + Home Assistant discovery, heap diagnostics
> with load-shedding, BH1750 illuminance, the shared signed pull-OTA
> engine (`firmware/common/ota`) with an HA `update` entity — **plus the
> witness trust surface**: an NVS-persisted Ed25519 identity signs every
> event over the v1 `sense` canonical (`common/identity/device_signature`,
> the same proven signer as canary-wap), every witnessed transition
> advances a domain-separated SHA-256 hash chain (NVS-persisted, survives
> broker outages and reboots), and the wap-schema `chain`/`health` topics
> let Home Assistant TOFU-pin the pubkey and render the green
> "device-verified ✓" badge. An IDF5 task watchdog guards the loop.
> Bench-pending: OTA A/B on real C6 hardware, `[BENCH]` protocol
> assumptions in `mr60_uart.h`.

## Quickstart (PlatformIO)

```
# from this directory:
cp secrets/secrets.example.h secrets/secrets.h   # fill WiFi + MQTT fields
pio run                            # canary-sense-default (presence-only)
pio run -e canary-sense-wellbeing  # adds vitals (-DCANARY_SENSE_VITALS)
pio run -e canary-sense-debug      # verbose ESP-IDF logging
pio run -t upload                  # build + flash
pio device monitor -b 115200       # USB-CDC console
```

The C6 builds on the pinned **pioarduino** platform fork (arduino-esp32 3.x);
see `../../envs/platformio/canary-sense.ini` for the pin and the rationale. The
first `pio run` downloads that platform (~hundreds of MB) — needs network.

## Serial tuning console (the flasher's bench rides this)

The USB console is **two-way**: alongside the boot log and telemetry lines,
a line-based tuning console (`common/console/tuning_console.h`) listens at
115200 8N1 — no WiFi, no broker, no HA required. It is serviced from the
main loop *before* the network phase AND as the idle poll inside the
blocking boot-time WiFi connect, so even a board with no provisioned WiFi
(which otherwise spins to the boot timeout and reboots) answers `cfg`/`set`
the whole time it waits.
Both flashers' tuning suites (the browser radar bench and the desktop
Flasher's monitor panel) are UIs over exactly this protocol; `pio device
monitor -b 115200` works just as well by hand.

```
help | ?              command list + every knob with range/current value
cfg                   all knobs on one line: [cfg] debounce=300 … stream=1000 raw=0
set <knob> <value>    clamp + apply to the live FSMs + persist to NVS
reset                 restore compiled defaults
stream on|off|<ms>    periodic [radar] line (default: on, 1000 ms)
raw on|off            bench detail in the stream (raw cm/BPM; session-only)
```

Knobs (the same eleven numbers HA tunes over `cfg/*/set`): `debounce`,
`clear`, `stall`, `near`, `mid`, and — wellbeing builds — `vlock`, `vlost`,
`breath_min`, `breath_max`, `heart_min`, `heart_max`. Every `set` answers
with a `[tune] ok/err` verdict plus the refreshed `[cfg]` snapshot, so a UI
reconciles by replacing, never by diffing.

### Pet & sleep presets (what the radar can and can't do)

The flasher bakes room presets into NVS at install time (and HA/the console
retune them after). Three are pet/sleep-oriented, and the feasibility is
honest — the MR60BHA2 computes breath/heart BPM *on its own module*,
band-passed for **human** physiology, and this firmware only reads the
scalars it reports:

- **🐭 Mouse / small-pet cage — wake & sleep** (both builds): a *movement*
  watch, not vitals. For a fixed cage, the radar confirming a live, moving
  occupant reads as awake/active and sustained stillness reads as
  settled/asleep. It does **not** read a mouse's heart or breathing — those
  rates (heart 300–800 bpm ≈ 5–13 Hz, breath 80–230/min) sit 4–8× above the
  module's human passband, so the module never reports them, and the BPM
  bounds can't even express them. Close cage mount; the whole cage is the
  near band.
- **🐕 Dog kennel / crate — resting heart & breathing** (wellbeing only): a
  real vitals preset. A calm dog's heart (≈50–160 bpm) and breathing
  (≈8–35/min) overlap the human bands the module was tuned for, so a settled
  dog within ~1.5 m reads like a human torso. Bands are widened for a dog
  (small-breed hearts, faster resting breathing) and the lock waits longer
  (a dog holds still in bursts). Panting or pacing breaks the lock — this is
  a resting/sleeping monitor for a single animal.
- **🛌 Human sleep & wake** (wellbeing only): the module's native use case —
  bands trimmed to a sleeping adult so a settled sleeper locks and a restless
  one doesn't latch noise.

The `[radar]` stream is the "what does the radar see right now" heartbeat:
`[radar] state=present count=1 range=near lock=locked breath=14 heart=72
errs=0`. It speaks the same coarse vocabulary MQTT publishes; the opt-in
`raw on` mode appends `raw_dist/raw_count/raw_breath/raw_heart` for band
calibration on the ATTENDED cable only — it is never persisted and never
touches the network (the documented exception in `src/main.cpp`'s header).

Your first USB flash with real `secrets/secrets.h` seeds the unit's NVS;
generic OTA release builds afterwards inherit that identity + credentials
(same `runtime_config` scheme as canary-vision).

## MQTT Topics

Base:
- `securacv/<device_id>/events` (non-retained; presence transitions only)
- `securacv/<device_id>/state`  (retained; full coarse snapshot)
- `securacv/<device_id>/status` (retained; availability + health heartbeat)
- `securacv/<device_id>/chain`  (retained; signed hash-chain head + length)
- `securacv/<device_id>/health` (retained; pubkey for HA TOFU-pin + heap/uptime/fw)
- `securacv/<device_id>/update/{state,cmd,auto,auto/cmd}` (signed pull-OTA)

Events are `presence_detected` / `presence_cleared` / `occupancy_changed`,
carrying only the coarse vocabulary: presence state, 0/1/2+ occupant bucket,
near/mid/far range band, 10-minute uptime bucket. **No raw distance, no
per-target data, no vitals — ever** (privacy chokepoint, design doc §2).
Each event carries an Ed25519 signature (`v`/`alg`/`fp`/`sig`) over the v1
`sense` canonical; HA verifies it against the pubkey TOFU-pinned from the
health topic, and the retained chain publish reuses the generic `chain`
canonical so HA's existing verifier covers it with zero changes.

## Home Assistant entities (MQTT discovery, retained)

| Entity | Class | Notes |
|---|---|---|
| `binary_sensor.<id>_presence` | occupancy | debounced radar presence |
| `sensor.<id>_occupants` | — | bucketed 0 / 1 / 2+ |
| `sensor.<id>_range_band` | diagnostic | near/mid/far only |
| `binary_sensor.<id>_radar_link` | problem, diagnostic | ON while the radar UART is stalled |
| `sensor.<id>_frame_errors` | diagnostic | UART checksum drops (monotonic) |
| `sensor.<id>_illuminance` | illuminance | BH1750 lux (tamper corroboration) |
| `sensor.<id>_last_event` / `_uptime` | — | |
| `sensor.<id>_rssi` / `_heap_free` | diagnostic | link + heap health heartbeat |
| `update.<id>_firmware` + `switch.<id>_auto_update` | firmware | signed pull-OTA |
| `binary_sensor.<id>_breathing` | — | **wellbeing builds only**: P0 breathing lock |
| `sensor.<id>_breath_rate` / `_heart_rate` | — | **wellbeing + P1 opt-in only**; null unless locked |

The BPM entities are provably absent from a presence-only build — their
discovery payloads are compiled out with `-DCANARY_SENSE_VITALS`, and vitals
are hard-suppressed whenever the target count is not exactly one.

## Layout

```
projects/canary-sense/
  platformio.ini          # extra_configs -> common.ini + canary-sense.ini
  include/canary/         # composition headers (config/topics/net/ha/diag)
  include/secrets.ci.h    # CI stub; real secrets in secrets/secrets.h
  src/main.cpp            # sensing core + privacy chokepoint + net stack
  src/net/                # wifi_mgr (backoff supervisor), mqtt_mgr, ota_mgr
  src/ha/ha_discovery.cpp # HA MQTT discovery entity set
boards/xiao-esp32c6-mr60/ # pin map (radar UART, BH1750 I2C, WS2812, BOOT)
configs/canary-sense/
  default/   config.h     # presence-only
  wellbeing/ config.h     # presence + vitals
common/sensors/mmwave_mr60/  # board-agnostic parser + FSMs (host-tested)
common/sensors/bh1750/       # minimal BH1750 lux driver
envs/platformio/canary-sense.ini  # the three build environments
```

## Build flavors

| Env | Config | Vitals | OTA product | Notes |
|-----|--------|:------:|-------------|-------|
| `canary-sense-default`   | `configs/canary-sense/default`   | off | `securacv-canary-sense` | CI build + check env |
| `canary-sense-wellbeing` | `configs/canary-sense/wellbeing` | on  | `securacv-canary-sense-wellbeing` | `-DCANARY_SENSE_VITALS=1` |
| `canary-sense-debug`     | `configs/canary-sense/default`   | off | `securacv-canary-sense` | `CORE_DEBUG_LEVEL=4` |

The two flavors are distinct OTA products with separate signed manifests, so a
presence-only unit can never be flipped to the wellbeing image (or back) by
serving it the other manifest — the engine's product check refuses.

The vitals switch reaches `common/sensors/mmwave_mr60` only as the
`-DCANARY_SENSE_VITALS` build flag (never a `config.h` include in `common/`),
per the firmware layering rules (`firmware/ARCHITECTURE.md`).

## Bench checklist (remaining hardware validation)

- [ ] **UART frames parse on bench** — confirm the `[BENCH]`-marked
      assumptions in `mr60_uart.h` (distance units, BPM units, frame cadence).
- [ ] **OTA A/B on C6** — install + rollback cycle through the HA update
      entity (the engine is bench-proven on S3/C3; the C6 partition flow is
      not yet).
- [ ] **BOOT button pin** confirmed (assumed GPIO9 in `pins.h` — verify).
- [ ] **BH1750 lux** readings sane on the kit's I2C bus.
- [ ] **WS2812** presence colors visible (green present / blue clear /
      amber no-radar).

## License
Apache-2.0 (see repository root).
