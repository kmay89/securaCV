# SecuraCV Canary Sentinel — Multi-Sensor Fusion Guardian

A doorway/window guardian that is **near impossible to walk past unseen** — not
because any one sensor is magic, but because it fuses several *physically
independent* ones and treats disagreement and blinding as suspicion. One
host-tested fusion brain, three cost tiers, five presets.

> **Status: Phase 0 — the fusion brain is landed and host-tested.** The novel
> core (`firmware/common/fusion`) and the preset→engine mapping are proven
> without hardware in `firmware/tests_host` (77 checks green under
> `-Wall -Wextra -Werror`, run by the existing host-tests CI job). This project
> composes that core with the on-board sensors and emits coarse transitions to
> the console. The signed witness + MQTT/HA + pull-OTA network path — the same
> proven stack as canary-sense — is **Phase 1** (checklist below), and the
> hardware bench is **pending**, so this project is intentionally not yet in
> `firmware/flavors.json`. It phases in exactly as canary-sense did.

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
level, 0..100 confidence, 0/1/2+ occupancy, near/mid/far band, and which
modality *classes* corroborated. **No MAC, no centimeters, no per-target track,
no imagery, no vitals — ever.** Every transition is Ed25519-signed over a
`sentinel` v1 canonical and hash-chained (Phase 1), reusing `common/identity` +
`common/witness` exactly as canary-sense does.

## Build & test

```
# verify the novel core without hardware (this is the CI-covered path):
make -C firmware/tests_host          # fusion + preset host suites, all green

# device builds (Phase 1 net path pending; recipe in envs/platformio/):
pio run -e canary-sentinel-door      # Standard, front-door preset
pio run -e canary-sentinel-lite      # Lite tier (C3, no radar)
pio run -e canary-sentinel-demo-head # Heavy sensor head
pio device monitor -b 115200
```

## Layout

```
projects/canary-sentinel/
  platformio.ini                 # selects envs from envs/platformio/canary-sentinel.ini
  include/canary/
    config.h                     # composition: preset macros -> housekeeping consts
    sentinel_config.h            # preset SENT_*/FEATURE_* -> securacv::fusion::FusionConfig
    sentinel_requirements.h      # R1–R10 as code
  src/main.cpp                   # reads sensors -> Vote -> fusion engine -> emit_claim()
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

## Phase 1 (the wiring that turns Phase 0 into a witness)

Wire the four onboard channels (PIR is already live; add WiFi-RF/CSI + BLE via
canary-wap's `rf_presence` / `common/csi` / `common/bluetooth`), then light up
`emit_claim()` into the shared signed witness + MQTT/HA + pull-OTA stack
canary-sense proves, add the HA discovery entity set, and land the flavor in
`firmware/flavors.json` once the C6 device build is bench-green.

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
