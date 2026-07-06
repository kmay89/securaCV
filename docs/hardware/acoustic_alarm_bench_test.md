# Acoustic Alarm Detection — Bench Test & Field Verification

How to verify — on the bench, before trusting it on a wall — that a
Canary hears your actual smoke / CO alarm. Written after a field report
of "pressed my smoke alarm's TEST button, nothing happened": every step
below turns one of the silent-failure causes we found into a check you
can run.

**Applies to:** `firmware/canary` (ESP32-S3 tree, all sensing builds
including release since 2026-07) and `canary-wap` (DEV/FULL profiles on
hardware with a PDM mic).

---

## 1. What the detector is (and isn't)

The Canary listens for the two **standardized alarm cadences** with a
two-stage detector, the same structure the industry uses for
alarm-sound recognition (spectral gate + temporal template — see the
research notes in §7):

- **T3 smoke** — ISO 8201 / NFPA 72: three 0.5 s beeps, 0.5 s gaps,
  1.5 s pause, repeating (every UL 217 smoke alarm sold in North
  America since 1996).
- **T4 CO** — UL 2034: four 100 ms beeps, 100 ms gaps, 5 s pause.

**Stage 1 (spectral):** each beep must be *alarm-band dominant* — a
band-pass biquad centered at 3.4 kHz (≈2.6–4.4 kHz passband) checks
that the beep's energy sits where UL sounders put their ~3.0–4.0 kHz
fundamental. Voices, TV, door slams and music fail this gate.

**Stage 2 (temporal):** the on/off envelope must fit the published
cadence within tolerance (±200 ms per T3 phase, ±60 ms per T4 beep).

It is a **notification aid, not certified life-safety equipment** — the
same honest framing Apple uses for HomePod Sound Recognition. It cannot
replace the alarm itself, interconnected alarms, or a monitored panel.

**Privacy:** only per-20 ms scalars (RMS + band RMS) ever exist; raw
samples are wiped inside the read loop; what crosses the module
boundary is `{event_type, confidence, 10-min bucket, cycle_count}`.

## 2. Prerequisites — hardware and build

| Check | How |
|---|---|
| Board is a XIAO ESP32-S3 **Sense** | The plain S3 has **no microphone**; the I2S driver still installs silently and hears only noise. The Sense's camera/mic daughterboard must be fitted. |
| Build has the feature | `release` / `release_ha` (shipped OTA images) include it since 2026-07; `dev` / `full` always did. If your device predates that, **update the firmware first — this was the root cause of the original field report.** |
| Mic not muted | Dashboard mic toggle, or `GET /api/audio/level` → `"muted": false`. The mute is hard (I2S driver uninstalled) and **persists across reboots**. |
| Power policy allows it | On battery-degraded modes the acoustic pipeline is gated off (`pf->acoustic`); bench-test on USB power. |

## 3. Five-minute bench test

Work at **1–3 m** from the device in a reasonably quiet room.

1. **Level meter sanity.** `GET /api/audio/level` (or the dashboard's
   mic panel). Clap: `rms` should jump well past the ON threshold
   (default 800) and fall back. If `rms` sits near 0 and never moves →
   wrong board / muted / feature absent (§2).
2. **Start a self-test window.** `POST /api/audio/test/start`
   (canary tree) or `POST /api/audio/selftest` (wap). This relaxes the
   timing tolerances and lowers the confidence floor for ~30 s.
   Self-test matches are **deliberately not forwarded to Home
   Assistant** — a TEST press must never trigger real smoke
   automations.
3. **Hold the alarm's TEST button** through at least **one full cycle
   plus its pause** — for T3 that means ≥ 4 s of beeping *and then
   1.5 s of quiet while still in the window*. A quick 1-s chirp cannot
   match; many alarms need the button held the whole time.
4. **Read the result.** `GET /api/audio/test/status` (canary) /
   `GET /api/audio/selftest` (wap): `matched_type` 1 = T3, 2 = T4, with
   a confidence. Matched → done; mount it and run §5 once.
5. **If it didn't match, read the cadence trace.**
   `GET /api/audio/transitions` returns the last envelope transitions,
   newest first: `{on, age_ms, dur_ms, tone}`.
   - **No transitions at all** → the envelope never fired: too far
     away / too quiet (move closer, or lower the ON threshold via the
     config endpoint), or §2 failures.
   - **Transitions with `dur_ms` ≈ 500 on the beeps but no match** →
     look at `tone` on the beep-ending (`on:0`) entries. It is the
     alarm-band ratio ×100 the stage-1 gate checks: ≥ 50 passes
     (self-test ≥ 30). A UL sounder at close range reads ~85–100.
     Low `tone` on a real alarm usually means severe echo/distance —
     move the device closer to the alarm's room.
   - **Beep `dur_ms` far from 500 (T3) / 100 (T4)** → echoey room
     merging beeps, or a non-standard sounder (§6).

## 4. What each `tone` value means

| `tone` (ratio ×100) | Reading |
|---|---|
| 85–100+ | Alarm-band pure tone — UL sounder signature |
| ~50 | Broadband noise (white-noise-like); right at the normal floor |
| 20–40 | Voice, music, TV, motors |
| ≤ 20 | Low-frequency: slams, knocks, hum, HVAC |

Gate floors: **50** normal, **30** during self-test
(`AUDIO_TONE_MIN_X100` / `AUDIO_TONE_MIN_RELAXED`).

## 5. End-to-end field check (once mounted)

With the device in its final position, trigger the alarm for real
(canned smoke spray, or a long TEST press **outside** self-test mode)
and confirm in Home Assistant: the T3/T4 event arrives (signed into
the witness chain when `FEATURE_SENSING_WITNESS` is on) and your
automations fire. Expect detection after the **first complete cycle +
pause** — for T3 that's ~5.5 s from the first beep, not instant.

## 6. Known limitations

- **520 Hz low-frequency sounders** (NFPA 72 18.4.5 sleeping-area
  requirement, mostly commercial; also some bedroom alarms for the
  hard-of-hearing) are **not detected** — their in-band harmonics are
  too weak for the stage-1 gate.
- Non-UL "chirping" gadgets, foreign cadence patterns, and low-battery
  single chirps are out of scope by design.
- Severe reverberation can merge the 0.5 s T3 gaps; if the transitions
  trace shows one long ON instead of three beeps, relocate the device.
- Detection cost is negligible: the whole pipeline (RMS + biquad +
  matcher) is well under 1 % of one 240 MHz core — no measurable heat
  or battery impact.

## 7. Research base

The two-stage design follows the published art for residential alarm
listening:

- ISO 8201:2017 / NFPA 72 — the T3 temporal pattern spec
  (0.5 s ± 10 % phases).
- UL 2034 — the T4 CO cadence.
- US patents 9,087,447 and 8,269,625 — frequency-band gate feeding a
  temporal-template matcher for T3 detection (the Nest Protect /
  listening-device family).
- Apple HomePod Sound Recognition — precedent for fully on-device
  alarm-sound detection presented honestly as an aid, not a certified
  life-safety device.

Alarm fundamentals cluster at 3.0–4.0 kHz (typ. ~3.2 kHz piezo), which
sets the biquad center (3.4 kHz) and width (Q ≈ 1.8).
