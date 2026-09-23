# SecuraCV Canary Sentinel — Multi-Sensor Fusion Guardian

A doorway/window guardian that is **near impossible to walk past unseen** — not
because any one sensor is magic, but because it fuses several *physically
independent* ones and treats disagreement and blinding as suspicion. One
host-tested fusion brain, three cost tiers, five presets.

> **Status: Phase 1a — compile-gated in CI, NOT released, nothing
> bench-verified.** The novel core (`firmware/common/fusion`) and the
> preset→engine mapping are proven without hardware in `firmware/tests_host`
> (under `-Wall -Wextra -Werror`, run by the host-tests CI job). Phase 1a wires
> the network/witness path — canary-sense's stack, carried here and pinned to
> it — and the project is in `firmware/flavors.json` so CI's PlatformIO leg
> compiles the `door` (C6) and `lite` (C3) envs and holds each image to its
> OTA slot. It has no release envs, no flasher product and no published OTA
> channel (its `firmware/flavors.json` entry is marked `unreleased`): it ships
> when the bench checklist below is green. The onboard-radio channels are Phase 1b and are
> not built. See the phase table below for exactly what is proven where.

Design + full spec: [`docs/canary_sentinel_fusion_design.md`](../../../docs/canary_sentinel_fusion_design.md).
Fusion engine: [`firmware/common/fusion`](../../common/fusion/README.md).

## Why it's hard to evade

Detection channels are grouped into **physical modality classes**, and
corroboration is only counted across *distinct* classes:

| Modality class | Channels | How you'd evade it | What still catches you |
|---|---|---|---|
| Thermal | PIR | move slowly, insulate | radar sees micro-motion; CSI sees you |
| RadioReflection | 60GHz radar | hold utterly still | you still breathe — radar locks it |
| ChannelPerturb | WiFi CSI | — (device-free) | your body bends the RF field |
| CarriedRadio | WiFi-RF, BLE | leave your phone at home | thermal + radar + CSI don't care |
| Optical | ambient light, vision | cross in the dark | heat + radio still radiate |
| Mechanical | contact, tamper | don't touch the door | you didn't get in without touching it |

To be invisible you must defeat **every** independent class at the same instant
a body crosses the threshold. Defeating *one* only removes one class's vote —
and defeating a class by *blinding* it (covering, jamming, unplugging) raises an
alarm instead of lowering the score. That asymmetry is the product.

## The line — Lite vs Standard vs Heavy (different costs, one brain)

| Tier | Board(s) | Channels | Modality classes | ~BOM `[BENCH]` | For |
|---|---|---|---|---|---|
| **Lite** | 1× XIAO ESP32-C3 | PIR + WiFi-RF + BLE + light | 3 | ~$18 | mailbox, shed, porch, hallway |
| **Standard** | XIAO ESP32-C6 + MR60BHA2 radar | + radar + WiFi-CSI | 5 | ~$45 | **the recommended front door** |
| **Heavy** | Standard head + XIAO ESP32-S3 vision hub | + contact + tamper + vision | 6 | ~$110 | the rigged, prove-it demo |

**Lite is honestly labeled:** with no radar and no CSI, a slow, device-free,
still intruder can evade it — right for casual threats, not a determined
adversary. **Standard** closes the two gaps that matter at a front door (the
still-breathing body via radar; the device-free walk-through via CSI).
**Heavy** is dual-board on purpose — the vision hub is a genuinely independent
optical modality on its own MCU, which also keeps the camera off the sensor head
(a doorway node with no lens is a stronger privacy story).

## Presets — fully modular, each explained

Pick one preset per build. Each is pure config data
(`firmware/configs/canary-sentinel/<preset>/`); behavior never forks by preset,
only the numbers do.

| Preset | Tier | What it does |
|---|---|---|
| [`door`](../../configs/canary-sentinel/door/README.md) | Standard | balanced front-door reflexes; fast commit; loiter alarms a lingerer |
| [`window`](../../configs/canary-sentinel/window/README.md) | Standard | tighter range, light-weighted, quicker anomaly on a blinded glass sensor |
| [`hallway`](../../configs/canary-sentinel/hallway/README.md) | Standard | occupancy not alarm — gentle, long dwell, silent-body rule off |
| [`mailbox-lite`](../../configs/canary-sentinel/mailbox-lite/README.md) | Lite | PIR+RF+BLE+light only; honest lower ceiling |
| [`perimeter-demo`](../../configs/canary-sentinel/perimeter-demo/README.md) | Heavy | everything on, all six modalities, max sensitivity |

**Modularity contract.** Every channel is (a) compile-time selectable via a
`FEATURE_*` flag in the preset and (b) runtime-weightable via its `ChannelSpec`
(weight, evasion_cost, stale_ms). Adding a sensor is: a driver in `common/`, an
adapter in `common/fusion/sentinel_channels.h`, and a weight in a preset. The
fusion core does not change.

## Decision vocabulary (coarse, signed, privacy-preserving)

`Clear → Aware → Present → Confirmed → Loiter`, with `Anomaly` as an overlay
that wins and latches. The only thing published is the coarse `FusionResult`:
level, 0..100 confidence, 0..100 anomaly score, 0/1/2+ occupancy, near/mid/far
band, and which modality *classes* corroborated. **No MAC, no centimeters, no
per-target track, no imagery, no vitals — ever.** Every level transition is
Ed25519-signed over the `sentinel` v1 canonical and hash-chained, reusing
`common/identity` + `common/witness` exactly as canary-sense does:

```
securacv-canary-sig|v1|sentinel|<device_id>|<seq>|<event>|<level>|<confidence>|
    <anomaly>|<occupancy>|<range>|<modality_bits>|<bucket_uptime_s>
```

It is its own kind (`spec/witness_dictionary.json` `signature_format`), not a
reuse of canary-sense's `sense` kind, because `sense` has no slot for
confidence, anomaly or the modality bitmask — those would have ridden unsigned.
Home Assistant rebuilds it in `custom_components/securacv/signature.py`
(`verify_sentinel_event`); the firmware host test and the HA pytest share one
golden vector.

### What goes over MQTT (Phase 1a)

| Topic | Retained | Carries |
|---|---|---|
| `securacv/<id>/events` | no | one signed event per level transition: `event` (`level_changed`), `seq`, `bucket_uptime_s`, `level`, `confidence`, `anomaly`, `occupancy`, `range`, `modality_bits`, `signed` + `v`/`alg`/`fp`/`sig` |
| `securacv/<id>/state` | yes | the latest claim + `presence` (level present/confirmed/loiter), `anomaly_active`, `channel_denied`, `modalities` (class names), `strong_modalities`, `tier`, uptime |
| `securacv/<id>/status` | yes | online/offline (LWT), heartbeat, RSSI, heap health |
| `securacv/<id>/health` | yes | the public key HA TOFU-pins, firmware version |
| `securacv/<id>/chain` | yes | the signed chain head + length (the `chain` kind) |
| `securacv/<id>/update/*` | yes | the HA update entity + auto-update switch (signed pull-OTA) |

The device's own MQTT discovery creates Presence and Anomaly binary sensors, a
Channel-blinded problem sensor, Level / Confidence / Anomaly score / Occupancy
/ Range band / Corroborating modalities sensors, the canary-sense diagnostics
(last event, uptime, RSSI, free heap) and the firmware update entity. There is
no Identify button and no tuning dial yet. In the SecuraCV integration the
device's sensing modality reads "Other sensor": it fuses several media, and a
dedicated fusion modality is a later dictionary decision.

## Phases — what is proven where

| Phase | Scope | Status | Proven by |
|---|---|---|---|
| **0** | fusion brain, presets, board pins, envs | landed | `make -C firmware/tests_host` (fusion + door / mailbox-lite preset suites) |
| **1a** | network/witness path: `sentinel` canonical + chain, MQTT events + retained state, HA discovery, signed pull-OTA, setup portal, mDNS | **compile-tested by CI** — `door` (C6, core 3) and `lite` (C3, core 2) in `firmware.yml`'s PlatformIO leg with OTA-slot size guards; **not run on hardware** | the canonical: host test + HA pytest golden vector; the copy of canary-sense's stack: `firmware/scripts/check_sentinel_net_sync.sh`; the compile: CI only (no local ESP32 toolchain) |
| **1b** | onboard-radio channels: WiFi-RF, WiFi-CSI, BLE (and the fleet-link BLE beacon) | **bench-bound, NOT built** — radio coexistence with the STA link and the CSI HAL on the C6 are unproven | nothing yet; the adapter call sites in `src/main.cpp` are comments |
| **2** | bench-tuned presets, release envs, OTA channel | not started | the bench checklist below, then a release |

Until 1b lands, a Standard build fuses PIR + radar + light and a Lite build
PIR + light: the unwired channels never vote, which the engine treats as
quiet (not blinded). That is fewer corroborating classes than the tier table
above promises — one more reason nothing ships yet.

## Build & test

```
# verify the novel core + the signed canonical without hardware:
make -C firmware/tests_host          # fusion + preset + device-signature suites
firmware/scripts/check_sentinel_net_sync.sh   # the net stack still equals canary-sense's

# device builds (compile-gated in CI; recipe in envs/platformio/):
pio run -e canary-sentinel-door      # Standard, front-door preset
pio run -e canary-sentinel-lite      # Lite tier (C3, no radar)
pio run -e canary-sentinel-demo-head # Heavy sensor head (not in CI: preset-only delta from door)
pio device monitor -b 115200
```

Credentials: copy `secrets/secrets.example.h` to `secrets/secrets.h` for a
USB-provisioned bench unit; a build without it boots into the shared setup
portal (a `SecuraCV-XXXX` network) instead of joining a placeholder, exactly
as canary-sense does.

## Layout

```
projects/canary-sentinel/
  platformio.ini                 # selects envs from envs/platformio/canary-sentinel.ini
  include/canary/
    config.h                     # composition: preset macros -> housekeeping + net consts
    sentinel_config.h            # preset SENT_*/FEATURE_* -> securacv::fusion::FusionConfig
    sentinel_requirements.h      # R1–R10 as code
    types.h / topics.h           # the coarse claim + snapshot; the MQTT topic set
    witness.h                    # Ed25519 identity + chain over the `sentinel` canonical
    net/{wifi,mdns,ota}_mgr.h ha/ log.h version.h runtime_config.h diagnostics.h
                                 # canary-sense's, byte-identical (pinned)
    net/mqtt_mgr.h               # the sentinel's publishers over canary-sense's transport
  src/main.cpp                   # reads sensors -> Vote -> fusion engine -> emit_claim()
  src/witness.cpp                # canary-sense's key/chain plumbing (pinned per function),
                                 #   signing + chaining the `sentinel` canonical
  src/net/mqtt_mgr.cpp           # canary-sense's transport + trust surface (pinned per
                                 #   function), the sentinel's state/heartbeat payloads
  src/ha/ha_discovery.cpp        # the sentinel's HA entity set
  src/net/{wifi,mdns,ota}_mgr.cpp src/runtime_config.cpp src/diagnostics.cpp
                                 # canary-sense's (wifi_mgr: all but the setup-network name)
  secrets/secrets.example.h      # USB-provisioning template (secrets.h is git-ignored)
common/fusion/                   # the board-agnostic, host-tested fusion brain
configs/canary-sentinel/<preset> # door / window / hallway / mailbox-lite / perimeter-demo
boards/xiao-esp32c6-sentinel     # Standard/Heavy head pins (radar + PIR + lux)
boards/xiao-esp32c3-sentinel-lite# Lite pins (PIR + lux)
envs/platformio/canary-sentinel.ini
```

## Bench checklist (hardware validation pending)

- [ ] PIR pin + debounce on the chosen XIAO carriers (C6 GPIO2, C3 GPIO3) `[BENCH]`
- [ ] Radar reuse: confirm canary-sense MR60BHA2 UART bring-up drives the radar
      channel unchanged `[BENCH]`
- [ ] CSI + RF + BLE coexistence on one radio without starving the net path `[BENCH]`
- [ ] Light-blinding → `Denied` on a real BH1750 `[BENCH]`
- [ ] Standard walk-in → `Confirmed` latency target ≤ 1.5 s `[BENCH]`
- [ ] Heavy head↔hub ESP-NOW link + vision vote round-trip `[BENCH]`
- [ ] False-alarm soak: pets, HVAC, sun-through-blinds vs the `Anomaly` rate
- [ ] Phase 1a on hardware, both tiers: an unprovisioned unit raises the setup
      network; a provisioned one joins, publishes discovery, and its signed
      events verify in Home Assistant ("verified" = the Ed25519 signature
      checked against the TOFU-pinned key) `[BENCH]`
- [ ] Pull-OTA boot self-test confirms (and a failed probe rolls back) on the
      C6 and the C3 `[BENCH]`
- [ ] NVS wear: chain head + length persist on every level transition — soak
      a busy doorway and confirm the write rate is acceptable `[BENCH]`

## Phase 1b (the wiring still to do, on a bench)

Wire the onboard-radio channels — WiFi-RF and BLE counting via canary-wap's
`rf_presence` / `common/bluetooth`, WiFi-CSI via `common/csi` — into the
`observe()` call sites `src/main.cpp` shows as comments, prove on a real C6
that they coexist with the STA link Phase 1a brings up (and that the CSI HAL
works there at all), then add the fleet-link BLE beacon. Release envs and an
OTA channel follow the bench checklist, not this list.

## Keeping the copied stack honest

`src/net`, `src/runtime_config.cpp`, `src/diagnostics.cpp` and the witness
key/chain plumbing are canary-sense's, carried rather than promoted to
`firmware/common` (a common/net promotion would move canary-sense,
canary-vision and their sketch mirrors — its own milestone).
`firmware/scripts/check_sentinel_net_sync.sh` runs in `firmware.yml` and fails
on any drift: the shared files byte-identical, `wifi_mgr.cpp` but for its
setup-network product name, and `mqtt_mgr.cpp` / `witness.cpp` function by
function. Fix canary-sense first, then carry the change here.

## About the creator

Canary Sentinel's fusion logic comes straight from a career in **ATM security,
sensing, and fraud detection** — no trade secrets here, just the mindset that
field beats into you. A single signal lies, and a determined adversary will
defeat any one of your sensors; what they can't cheaply defeat is **consistency
across independent channels**, and the moment to worry is when a channel that
*should* be reporting goes quiet. That's how banks catch skimming and card
fraud, and it's exactly what this engine does with heat, radar, radio and light.
This isn't a gadget demo — it's that hard-won posture, pointed at your front
door, with the goal of helping ordinary people know when someone's there.

## License

Apache-2.0 (repository license).
