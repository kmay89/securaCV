# Canary Sentinel — Multi-Sensor Fusion Guardian: Design & Spec

Status: **Phase 0 landed** — the board-agnostic fusion brain
(`firmware/common/fusion/sentinel_fusion.*`) is in the tree and host-tested
(`firmware/tests_host/test_sentinel_fusion.cpp`, 28 checks green under
`-Wall -Wextra -Werror`). The project wrapper, presets, board pin maps and build
envs are staged (`firmware/projects/canary-sentinel`,
`firmware/configs/canary-sentinel`, `firmware/envs/platformio/canary-sentinel.ini`).
**Hardware bench validation is pending** — see the project README checklist. This
document is the spec the firmware is built to; where a number is a bench
question it is marked `[BENCH]`.

Product family: **Canary Sentinel Lite / Standard / Heavy**
Fusion core: `firmware/common/fusion/` · Project: `firmware/projects/canary-sentinel`
Companion sensors reused wholesale: `common/sensors/mmwave_mr60` (radar),
`common/csi` (WiFi sensing), canary-wap's `rf_presence` + `common/bluetooth` (RF/BLE),
`common/sensors/bh1750` (light), `common/identity` + `common/witness` (trust).

---

## 1. What we're building, in one paragraph

A doorway/window guardian that is **near impossible to walk past unseen** — not
because any one sensor is magic, but because it fuses several *physically
independent* ones and treats disagreement and blinding as suspicion. It speaks
the same small, signed, privacy-preserving vocabulary as the rest of the fleet
(presence as a state, occupancy as 0/1/2+, range as near/mid/far — no MACs, no
centimeters, no imagery), and it ships in three cost tiers so the same brain
guards a mailbox for ~$18 or a front door with the full rig.

This is the real deal, and the honesty is part of the product: **Lite is
evadable and we say so**; Standard closes the gaps that matter for a front door;
Heavy is the "prove it" demo.

## 2. Threat model — evasion is the design driver

The question isn't "can we detect a person." Every sensor here does that. The
question is **"what does someone do to *not* be detected, and does the system
notice the attempt?"** We enumerate the evasions and answer each:

| Evasion move | Beats | Still caught by | Sentinel response |
|---|---|---|---|
| Move very slowly / freeze | PIR | radar (breathing), CSI | Present → Confirmed once a 2nd modality agrees |
| Leave phone at home | WiFi-RF, BLE | PIR, radar, CSI, light | those modalities never depended on a device |
| Cross in total darkness | (light, camera-only) | PIR, radar, CSI | thermal + radio don't need photons |
| Hold utterly still behind cover | PIR, some CSI | radar (60GHz sees micro-motion + breath) | "silent body" → dwell → **Anomaly** |
| Cover / blind a sensor | that one sensor | the rest | `Denied` vote → **suspicion**, amplified if a body is also present |
| Jam 2.4GHz | WiFi-RF/CSI, BLE | PIR, radar | denied RF channels raise anomaly, PIR/radar still vote |
| Approach with no metal/mass, touch nothing | contact/tamper | PIR, radar, CSI, light | mechanical silence is fine; other classes carry it |

The invariant that falls out: **to be invisible you must defeat every
independent modality class at the same instant a body crosses the threshold.**
The engine is built so that defeating *one* class only removes *one* class's
vote, and defeating a class by *blinding* it raises an alarm rather than
lowering the score. That asymmetry is the product.

## 3. The independent-modality principle

Detection channels are grouped into **physical modality classes**. Corroboration
is only counted across *distinct classes* — this is what keeps the anti-evasion
claim honest.

| Modality class | Physics | Channels | Evasion cost |
|---|---|---|---|
| `Thermal` | body IR emission | PIR | low — slow/insulated motion |
| `RadioReflection` | 60GHz FMCW reflection | radar | high — a still body still breathes |
| `ChannelPerturb` | 2.4/5GHz multipath bend | WiFi CSI | high — device-free, hard to null |
| `CarriedRadio` | probe/advert emission | WiFi-RF, BLE | low — leave the phone at home |
| `Optical` | visible-light change | ambient light, vision | medium — darkness / cover |
| `Mechanical` | physical actuation | door contact, tamper | medium — don't touch anything |

`CarriedRadio` deliberately holds **both** WiFi-RF and BLE: they share an evasion
(no powered radio on the target → both blind), so they must not each count as an
independent vote. `Optical` holds both ambient-light and camera for the same
reason. Being conservative here is what lets us make the "hard to evade" claim
with a straight face.

## 4. The fusion math

Implemented in `sentinel_fusion.cpp`; summarized here.

**Per channel, per tick.** Each enabled channel contributes its latest coarse
`Vote` (decayed to `None` once older than its `stale_ms`):

- `Strong` → `+ weight × quality / 100`
- `Weak`   → `+ weight × quality / 200`
- `None`   → nothing
- `Denied` → nothing to the score; `+ denied_suspicion` to a separate **anomaly**
  accumulator (and `+ denied_suspicion` again if a body-present modality is
  Strong at the same time — blinding-while-present).

**Independence bonus.** Let *k* = number of distinct modality classes voting
`Strong`. If *k* ≥ 2, add `independence_bonus × (k − 1)` to the score. This is
the super-linear reward for independent corroboration — the anti-evasion term.

**Score** = clamp(evidence sum + independence bonus, 0..100).

**Presence ladder** (timing-independent):

```
score ≥ confirmed_score AND strong_classes ≥ min_confirm_modalities → Confirmed
score ≥ present_score                                               → Present
score ≥ clear_score                                                 → Aware
else                                                                → Clear
```

**Overlays.**
- *Dwell:* a committed Present/Confirmed held past `loiter_dwell_ms` → `Loiter`
  if corroborated, or `Anomaly` if it's a **silent body** (a body-present
  modality Strong with nothing else even weakly agreeing).
- *Anomaly:* `anomaly ≥ anomaly_score` → `Anomaly`, latched for
  `anomaly_latch_ms`. Anomaly wins over every other level and does not wait out
  a debounce timer — evasion shouldn't get a grace period.

**Debounce.** The committed presence level only moves after the target level is
sustained: rising transitions need `present_debounce_ms`, falling transitions
need `clear_debounce_ms` (hysteresis against flapping at the threshold).

All thresholds, weights, and timers are **data** (per preset). No behavior forks
by preset — only these numbers do, per the firmware architecture's config rules.

## 5. Privacy chokepoint (non-negotiable, same as the fleet)

The fusion engine sees `Vote`s, never measurements. The only thing the
composition layer may publish is the coarse `FusionResult`:

- `level` — clear / aware / present / confirmed / loiter / anomaly
- `confidence` — 0..100
- `occupancy` — 0 / 1 / 2+ (only if a counting channel supplies it)
- `range` — near / mid / far (only if radar supplies it)
- `modality_bits` — *which classes* corroborated, never which device

No MAC is ever stored (RF/BLE are aggregate counts; canary-wap's `rf_presence`
guarantees this). No distance in centimeters, no per-target track, no imagery,
no vitals leave the device. Every published transition is Ed25519-signed over a
`sentinel` v1 canonical and hash-chained, reusing `common/identity` and
`common/witness` exactly as canary-sense does — HA TOFU-pins the pubkey and
renders the "device-verified ✓" badge with zero new verifier code.

## 6. The product line — Lite vs Standard vs Heavy

Different costs, one brain. The tier is a **board + a set of enabled channels**;
the fusion core is identical across all three.

### Sentinel Lite — "good enough, honestly labeled" · ~$18 `[BENCH]`
- **Board:** one XIAO ESP32-C3 (or C6). Onboard radio only.
- **Channels:** PIR + WiFi-RF + BLE + ambient light.
- **Modality classes available:** Thermal, CarriedRadio, Optical (3).
- **Closes:** casual walk-past, phone-carrying visitors, day/night motion.
- **Does NOT close:** a slow, device-free, still intruder — no radar, no CSI.
  We say this out loud. Good for a mailbox, a shed, an interior hallway.

### Sentinel Standard — the recommended front-door guardian · ~$45 `[BENCH]`
- **Board:** XIAO ESP32-C6 + Seeed **MR60BHA2** 60GHz radar (reuses the whole
  `canary-sense` hardware bring-up) + PIR + ambient light; WiFi-RF/CSI + BLE on
  the onboard radio.
- **Modality classes:** Thermal, RadioReflection, ChannelPerturb, CarriedRadio,
  Optical (5).
- **Closes:** the still-breathing body (radar) and the device-free walk-through
  (CSI) — the two gaps that actually matter at a front door.

### Sentinel Heavy — "the rigged demo, prove-it tier" · ~$110 `[BENCH]`
- **Boards (2):** the Standard sensor head **plus** a XIAO ESP32-S3 running
  optical corroboration (`canary-vision` person-detection) and hosting the
  fusion hub, linked head↔hub over ESP-NOW/mesh (`common/network`,
  `docs/mesh_esp_now_evaluation.md`).
- **Adds:** door-contact reed + accelerometer **tamper**, and GNSS-disciplined
  time (`common/gnss`) for satellite-attested witness records.
- **Modality classes:** all six. This is the "near impossible to evade"
  configuration and the one we demo.

> Heavy is dual-board on purpose: it's the honest way to say "one or more
> boards." The vision hub is a genuinely independent optical modality, and
> putting it on its own MCU keeps the camera off the sensor head (a doorway
> radar node with no lens is a stronger privacy story).

## 7. Presets — fully modular, each explained

Presets live in `firmware/configs/canary-sentinel/<preset>/` as pure config
data (channel enables + weights + thresholds + timers). Pick one per build; tune
at runtime over the same NVS-backed number entities as canary-sense.

| Preset | Tier | What it's for | What it does differently |
|---|---|---|---|
| `door` | Standard | front door / entry threshold | range-gated to the doorway, fast `present_debounce`, `loiter` alarms a lingerer; balanced weights |
| `window` | Standard | window / sill | tighter range, **light + tamper weighted up** (a hand at the glass, a shadow across the sill), quicker anomaly |
| `hallway` | Standard | interior corridor, occupancy | gentle debounce, presence-oriented not alarm-oriented, radar range widened |
| `mailbox-lite` | Lite | mailbox / shed / porch | PIR + RF + BLE + light only; radar/CSI absent; honest lower ceiling |
| `perimeter-demo` | Heavy | the full-rig demo | every channel on, all six modalities, sensitivities maxed, tamper + vision live |

**Modularity contract:** every channel is (a) compile-time selectable via a
`FEATURE_*` flag in the preset's `config.h`, and (b) runtime-weightable via its
`ChannelSpec` (weight, evasion_cost, stale_ms). Adding a new sensor is: write a
driver in `common/`, adapt its output to a `Vote` in the project's channel
adapter, and give it a weight in a preset. The fusion core doesn't change.

## 8. What each channel contributes (the modular menu)

| Channel | Driver reused | Vote semantics | Notes |
|---|---|---|---|
| **PIR** | GPIO (board pin) | motion pulse → `Strong`; recent-but-settled → `Weak` | cheap, zero-emission, dark-capable; first-alert |
| **Radar** | `common/sensors/mmwave_mr60` | presence+count → `Strong`; edge → `Weak`; UART stalled → `Denied` | still-body + breathing; feeds `range`/`occupancy` |
| **WiFi CSI** | `common/csi` | motion/breathing core → `Strong`/`Weak` | device-free; the anti-"leave-your-phone" channel |
| **WiFi RF** | canary-wap `rf_presence` | device-count over threshold → `Weak`/`Strong` | aggregate only, no MAC; corroboration not primary |
| **BLE** | `common/bluetooth` + `rf_presence` | device-count over threshold → `Weak`/`Strong` | same class as WiFi-RF (carried radio) |
| **Ambient light** | `common/sensors/bh1750` | threshold/shadow delta → `Weak`; sensor blinded → `Denied` | tamper corroboration; cheap |
| **Contact** | reed on GPIO (Heavy) | door open → `Strong` | the opening itself |
| **Vision** | `canary-vision` (Heavy hub) | person-detect → `Strong` | independent optical, on its own MCU |
| **Tamper** | accel (Heavy) | enclosure disturbed → `Strong`/`Denied` | anti-defeat |

## 9. Bench checklist (hardware validation pending)

- [ ] PIR pin + debounce on the chosen XIAO carrier `[BENCH]`
- [ ] Radar reuse: confirm `canary-sense` UART bring-up drives Sentinel's radar
      channel unchanged `[BENCH]`
- [ ] CSI + RF + BLE coexistence on one radio without starving MQTT `[BENCH]`
      (see `docs/network_coexistence.md`)
- [ ] Light-blinding → `Denied` path on real BH1750 `[BENCH]`
- [ ] Standard-tier fusion latency (walk-in → Confirmed) target ≤ 1.5 s `[BENCH]`
- [ ] Heavy head↔hub ESP-NOW link budget + vision vote round-trip `[BENCH]`
- [ ] Field false-alarm soak: pets, HVAC, sun-through-blinds vs `Anomaly` rate

## 10. Roadmap

- **Phase 0 (done):** fusion brain + host tests + spec + scaffolding.
- **Phase 1:** Standard-tier project build on real C6+MR60 hardware; wire the
  four onboard channels into the engine; signed `sentinel` witness canonical.
- **Phase 2:** presets tuned on the bench; HA discovery entity set; pull-OTA.
- **Phase 3:** Heavy dual-board demo; vision hub as an independent optical vote.

## 11. About the creator

Canary Sentinel's fusion logic comes straight from a career in **ATM security,
sensing, and fraud detection** — no trade secrets here, just the mindset. The
lesson that field teaches, over and over, is that a single signal lies and a
determined adversary will defeat any one of your sensors; what they can't
cheaply defeat is **consistency across independent channels**, and the moment to
worry is when a channel that *should* be reporting goes quiet. That's exactly how
banks catch skimming and card fraud, and it's exactly what this engine does with
heat, radar, radio and light. This isn't a gadget demo — it's that hard-won
posture, pointed at your front door, with the goal of helping ordinary people
know when someone's there.
