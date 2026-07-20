# SecuraCV Canary Sense (XIAO ESP32-C6 + MR60BHA2)

Privacy-preserving **radar-native** witness firmware: 60GHz FMCW mmWave presence
(and optional P1-gated wellbeing vitals) on the Seeed MR60BHA2 kit. No camera,
no microphone, no MAC surface — the raw radar IQ never leaves the radar module's
own DSP; the host MCU only ever sees pre-digested scalar claims over UART.

Design + roadmap: [`docs/canary_sense_mr60bha2_design.md`](../../../docs/canary_sense_mr60bha2_design.md).
Hardware deep-dive (chip, placement physics, power budget, bench flags):
[`docs/hardware/mr60bha2_radar_notes.md`](../../../docs/hardware/mr60bha2_radar_notes.md).
Try it before the bench: **the Sense Lab** (`canary-local/sense.html`) stages
this firmware's wire protocol, FSM semantics, privacy chokepoint, placement
geometry and power model in the browser — drift-gated against this project's
source by `canary-local/tools/gen_sense.py`.

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
- [ ] **WS2812** presence colours visible (green present / blue clear /
      amber no-radar).

## License
Apache-2.0 (see repository root).
