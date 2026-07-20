# MR60BHA2 — radar capability, placement & power notes

Deep-dive hardware notes for the Canary Sense radar (Seeed MR60BHA2 60 GHz
kit, XIAO ESP32-C6 host). This file is the **source of truth for the Sense
Lab's physics and power model**: `canary-local/tools/gen_sense.py` parses the
`SIM:` tables below into `canary-local/devices/sense.json` and CI drift-gates
them, the same anti-rot contract as every other generated page. Change a
number here and the bench page changes with it — nothing is written twice.

Companion docs: the product design ([`docs/canary_sense_mr60bha2_design.md`](../canary_sense_mr60bha2_design.md)),
the firmware protocol header (`firmware/common/sensors/mmwave_mr60/mr60_uart.h`,
whose `[BENCH]` list this file extends), and the ESPHome on-ramp
([`docs/integrations/mr60bha2_esphome.md`](../integrations/mr60bha2_esphome.md)).

Sources are cited inline. Facts we could not verify against a primary
document are marked **[unverified]** — they are bench items, not specs.

---

## 1. The radar module, all the way down

- **SoC: ADT6101P** (Andar Technologies) — a single-chip 60 GHz CMOS radar:
  57–64 GHz FMCW transceiver, **2T2R on-module microstrip patch antenna**,
  1 MB flash, radar signal-processing unit, and an ARM **Cortex-M3** that
  runs the entire vital-sign pipeline (range FFT → phase extraction →
  breath/heart separation → rate estimation, plus presence/tracking).
  The same chip powers Hi-Link's HLK-LD6002 breathing/heartbeat module.
  Sources: MR60BHA2 datasheet ("Low Power Low Cost High Resolution CMOS
  Radar", files.seeedstudio.com), andartechs.com.cn, hlktech.net.
- **Consequence, and the privacy story's backbone:** the raw IQ stream and
  the phase math never leave the module. The XIAO ESP32-C6 receives only
  result scalars over a 115200 8N1 UART. Seeed states the radar firmware is
  closed; detection-zone/chirp customization is partnership-gated. We treat
  the module as a fixed black-box claim source (design doc §1.1).
- **Detection geometry:** a three-dimensional sector of **80° × 80°**
  (datasheet). The MR60BHA1 sibling documents ±20° −3 dB beams at 4 dBi —
  the 80° sector is the detection envelope, not the −3 dB beamwidth, so
  expect SNR rolloff well inside the sector edges.
- **Ranges** (Seeed, radar firmware ≥ 1.6.x):
  - static presence: up to **6 m** marketing / **up to 4 m** effective per
    Seeed's own HA troubleshooting doc — the two Seeed documents disagree;
    treat 4–6 m as the honest envelope **[unverified which]**;
  - breathing + heart rate: **≤ 1.5 m**, core zone **0.5–1.5 m**, single
    stationary person (datasheet window 0.4–2 m);
  - distance-to-target reporting: to ~5 m (fw ≥ 1.6.10), tracking > 96 %
    at 0.5–1.5 m, > 90 % at 1.5–3 m, degraded 3–5 m.
- **Doppler bin:** the Seeed Arduino library's `RANGE_STEP 17.28f` maps a
  Doppler index to ≈ 0.17 m/s speed resolution.
- **Chirp parameters (bandwidth, slope, frame rate): not published** —
  partnership-gated. **[unverified]** Anything derived from them (range
  resolution ≈ c/2B, etc.) is inference, not spec.
- **Radar firmware upgrades** are field-possible but USB-only: an ESP32
  UART-passthrough sketch plus Seeed's Windows OTA tool (XMODEM-CRC
  bootloader, prints `C`). Notable history: 1.6.4 added presence +
  3-person detection; **1.6.12's release note admits "the previous
  breathing and heart rate algorithm had fundamental issues"** — pin the
  radar firmware version on the bench and record it in bench notes.
  Cross-flashing FDA2 firmware bricks the module (Seeed warning): there is
  no supported BHA2 ↔ FDA2 swap.

## 2. The wire beyond what we decode

Our parser decodes five frame types (`mr60_uart.h`). The module speaks
more; everything below arrives as a **well-framed unknown** and increments
`unknown_count()` — deliberately skipped, never an error:

| type | meaning | payload | why we drop it |
|---|---|---|---|
| `0x0A13` | breath/heart **phase waveform** | 3× float32 (total, breath, heart phase) | the chest-displacement waveform itself — privacy chokepoint says scalars only, and BPM claims already carry the wellbeing story |
| `0x0A08` | 3D point cloud | uint32 count + per-target x, y (m), doppler idx, cluster idx | per-target trajectories are exactly what the design doc's §2.2 forbids |
| `0xFFFF` | radar firmware version | uint32 {project, major, sub, modified} | worth decoding **some day** for the health log (bench note below) |

The BHA2 accepts **no public configuration commands** — the Arduino
library and both ESPHome components are receive-only for this module (the
FDA2 sibling has install-height/sensitivity/area commands; the BHA2 has
none). So the tuning surface for Canary Sense is exactly what the Sense
Lab exposes: **host-side FSM knobs and physical placement.** There is
nothing to configure in the radar, only where you put it and how the host
judges its claims.

### Bench flags (extends the `[BENCH]` list in `mr60_uart.h`)

1. **Distance unit conflict.** Our decoder assumes the `0x0A16` float is
   metres and scales ×100; the merged ESPHome component publishes the same
   float with unit **cm**. If ESPHome is right, the fix is dropping the
   scale in `decode_distance_()` — one line, flagged there. Verify on the
   bench first: a person at 1 m reading `100.0` settles it.
2. **Presence range 4 m vs 6 m** (Seeed docs disagree — see §1).
3. **Hold-last-value semantics.** Seeed documents that vitals **hold the
   last value** when the target leaves the 0.5–1.5 m core zone (and
   distance beyond 5 m does the same) but report **0** when no target at
   all. Our stall-safe FSMs already treat "same number forever" as lock
   maintenance, so confirm the lock-lost deadline actually fires on a
   walked-away target (it should: `PEOPLE_EXIST false` zeroes the
   aggregate).
4. **Empty-room phantom vitals.** Seeed documents fan/AC micro-motion
   producing a **nonzero heart rate with breath = 0 in an empty room**.
   Our vitals plausibility band (breath 6–30) rejects exactly this shape —
   keep it that way; verify on the bench with a desk fan.
5. **Frame cadence** per scalar is unpublished — measure it, then size
   `MR60_FRAME_QUEUE` headroom and the Sense Lab's stream model to match.
6. **Radar fw version frame `0xFFFF`** — consider decoding into the health
   log so fleets can spot un-upgraded modules (1.6.12 fixed vitals math).

## 3. Placement — what the physics says

Seeed's official guidance (wiki + HA troubleshooting doc), which the Sense
Lab's presets stage verbatim:

- **Wellbeing/sleep is the vitals scenario** — Seeed explicitly cautions
  against desk/exercise vitals ("significant inaccuracies"). Presence is
  the general-purpose claim; BPM is a bedside claim.
- **Reference bedside mount: 1 m above the head of the bed, tilted 45°
  down toward mid-bed, radar-to-chest ≤ 1.5 m**, boresight at the chest,
  rigid mount.
- **Interference blacklist:** twin radars in one room, wind-moved
  curtains/plants, flowing water, large metal/mirror surfaces, sensing
  through glass or thin wood, vibrating mounts, poor supplies.

What the research literature adds (the numbers behind the Lab's quality
meter):

- Chest-wall displacement: **heartbeat ≲ 0.6 mm (typically 0.1–0.5 mm),
  breathing 1–12 mm** (PMC8070581). At λ ≈ 5 mm, a 0.25 mm heartbeat is
  ~36° of two-way phase — comfortably resolvable, which is why this works
  at all.
- Accuracy vs distance is **U-shaped with the optimum near 0.7 m**: MAE
  0.8 bpm (breath) / 3.2 bpm (heart) at ~70 cm, collapsing below ~0.6 m
  (near-field) and beyond ~1.0–1.2 m (SNR/multipath) (arXiv:2603.09791).
  That study also found beat-to-beat intervals (HRV) unusable (≥ 15–30 %
  error) — average rates only, which is all our wellbeing channel claims.
- Range is antenna-gain-limited: cavity/leaky-wave antennas extend 60 GHz
  vitals to 2–4 m in the lab (MDPI Electronics 14(20):4014, PMC10146583);
  the BHA2's small patch array is why its envelope stops at 1.5 m.
- Breathing **harmonics** (2nd–4th of 0.2–0.4 Hz) land inside the 0.8–2 Hz
  heart band and often out-power the heartbeat (PMC8747437) — one reason
  radar HR reads high on some subjects.
- Orientation: chest-facing gives the largest displacement; side is
  weakest; back-facing still carries breathing through the back wall
  (Sensors 20(12):3489 recovered rates at all four orientations).
- Field reports to keep us humble (and why vitals are P1, non-diagnostic):
  a Seeed-forum benchmark vs a Polar H10 found HR correlation ≈ 0.33 with
  readings 10–20 bpm high; another user measured breath rate consistently
  +4 bpm vs a metronome. Claims of 90 %/85 % accuracy are Seeed's own.

### SIM:hardware (parsed into sense.json — the Lab's physics constants)

| key | value | grounding |
|---|---|---|
| soc | ADT6101P 60GHz CMOS (2T2R, Cortex-M3) | datasheet |
| band | 57–64 GHz FMCW | datasheet |
| fov_deg | 80 | datasheet detection sector (80°×80°) |
| presence_min_m | 0.4 | datasheet window |
| presence_max_m | 6.0 | Seeed spec (4 m effective per §1 — bench flag 2) |
| vitals_max_m | 1.5 | Seeed core-zone ceiling |
| vitals_ref_m | 0.7 | U-curve optimum, arXiv:2603.09791 |
| vitals_grace | 1.33 | hold-last-value zone beyond 1.5 m (≈2 m datasheet edge) |
| breath_disp_mm | 1–12 | PMC8070581 |
| heart_disp_mm | 0.1–0.5 | PMC8070581 |
| orientation_facing | 1.0 | max radial chest displacement |
| orientation_side | 0.35 | weakest orientation (literature) |
| orientation_back | 0.7 | breathing carries through the back wall |
| fan_penalty | 0.45 | Seeed interference blacklist (moving reflector) |
| fan_false_presence | 0.08 | Seeed: phantom vitals in empty rooms |
| vitals_period_ms | 1000 | bench flag 5 — cadence unpublished, modeled 1 Hz |
| vitals_dropout | 0.6 | dropout probability scale at zero quality |
| breath_jitter_bpm | 6 | +4 bpm field offset report, rounded up |
| heart_jitter_bpm | 18 | 10–20 bpm-high field benchmark |

(The four `vitals_*`/jitter/fan rows are the **bench model** — honest
knobs for the simulator, labeled as such on the page, not module specs.)

## 4. Power — every milliwatt should witness something

The kit is a mains-powered witness (design doc), but heat is still budget:
what we spend on radios and lights is heat that isn't sensing. Published
anchors:

- **Kit, official (5 V in): 0.5 W standby · 0.8 W typical active · 1.4 W
  driving a Grove relay** (Seeed wiki spec table). Supply spec 5 V/1 A.
- **Radar module alone:** BHA2-specific draw is only in the gated
  datasheet **[unverified]**. Sibling anchors: MR60BHA1 module **150 mA
  typ @ 5 V** (750 mW); HLK-LD6002 (same SoC) lists 600 mA @ 3.3 V burst.
  The kit's 0.8 W total implies Seeed's firmware duty-cycles chirping well
  below chip peak.
- **XIAO ESP32-C6** (Seeed): modem-sleep 30 mA, light-sleep 3.1 mA,
  deep-sleep 15 µA. ESP32-C6 datasheet: WiFi TX ≈ 382 mA peak
  (802.11b, 20.5 dBm), RX listen ≈ 78 mA.
- **No radar sleep/duty-cycle command exists publicly** (§2). The only
  radar duty-cycling is external rail gating, at the cost of the
  algorithm's settling time (BHA1 documents a 20 s observation set-up
  time) — a poor trade for a continuous witness; we don't do it.
- BH1750 ≈ 120 µA active; WS2812 up to ~60 mA full-white (we run dim
  status colours).

### SIM:power (parsed into sense.json — the Lab's rail model, mW)

Rails are chosen so the **model's default total reproduces the published
0.8 W kit-active figure** — the calibration anchor — with each component
tied to its nearest published number:

| rail | mW | grounding |
|---|---|---|
| radar_mw | 450 | kit standby 0.5 W minus C6 idle share; BHA1 sibling 750 mW [unverified for BHA2] |
| c6_active_mw | 100 | C6 CPU active (radio idle), datasheet modem-sleep band 17–38 mA |
| wifi_listen_mw | 250 | C6 RX listen ≈ 78 mA @ 3.3 V |
| wifi_modem_sleep_mw | 30 | DTIM wake windows (Seeed 30 mA figure covers CPU too) |
| wifi_tx_mw | 1150 | C6 TX burst ≈ 350 mA @ 3.3 V, duty-cycled by publish cadence |
| led_mw | 30 | WS2812 dim status colours |
| bh1750_mw | 1 | ~120 µA active |

Default model total ≈ 0.83 W ≈ Seeed's 0.8 W active — honest to the anchor.
The Lab's levers (all real firmware levers, none radar-side):

1. **WiFi modem sleep** (`WIFI_POWER_SAVE`, off by default on mains —
   `include/canary/config.h` documents the ~20 mA trade) — the single
   biggest discretionary saving (~220 mW), at publish-latency cost.
2. **Heartbeat cadence** (`CS_HEARTBEAT_MS`) — TX bursts are ~1.15 W for
   milliseconds; at 5 s cadence they average single-digit mW. Stretching
   the heartbeat mostly buys nothing — the diagnostics ladder already
   stretches it under pressure. The lesson the Lab teaches: **the radar
   dominates; the C6's radio is the only knob that matters, and even it is
   second-order.** Deep-sleeping the C6 is pointless for a continuous
   witness — the radar cannot nap without a 20 s wake-up amnesia.
3. **LED off** buys ~30 mW; lux costs ~1 mW (keep it — it's the tamper
   channel).

"Maximum useful computation per unit heat" therefore resolves to: keep the
radar witnessing (it IS the useful computation, and it lives on the
module's own Cortex-M3), keep the C6 pipeline lean (it already is:
parse + FSM + sign is microseconds per frame), and spend radio heat only
on claims worth signing — which is precisely the publish-on-change +
coarse-heartbeat design the firmware ships.

## 5. What "best it can be" means for this device

The honest capability envelope, stated plainly (the Sense Lab renders
this): **presence to ~4–6 m with high confidence in the 80° sector;
occupancy buckets reliable for 0/1/2+; range bands honest; breathing lock
solid at bedside geometry (≤ 1.5 m, chest-facing, still); BPM numerics
usable as averaged wellbeing trends at 0.5–1.5 m and nothing more.** HRV,
diagnosis, multi-person vitals, through-wall claims: out of envelope, and
the firmware refuses to pretend otherwise (plausibility bands, single-
target suppression, P1 gating). The strongest upgrades available to us are
not radar settings (there are none) but **placement discipline** — which
this Lab exists to rehearse — and **radar-module firmware currency**
(≥ 1.6.12; see bench flag 6).
