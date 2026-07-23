# The mic-bearing dash — Waveshare 4.3C, and the listening contract

The Waveshare **ESP32-S3-Touch-LCD-4.3C** ("AI voice", commonly sold as
the cased **4.3C-BOX**) is the one display in the family that physically
carries microphones: a dual-MIC array behind an ES7210 ADC, with an ES8311
codec alongside — the mics listen through the grille slots on the case
top. The vendor listing also names a **PCF85063 RTC**, a TF slot, an audio
(speaker) output this firmware never initializes, and — on the BOX
edition — a screw-terminal strip whose functions are still unverified
(the board README tracks both follow-ups). Until now the repo's answer
was "prefer the mic-free 4.3/4.3B" — the family promise is *it shows, it
doesn't watch*, and a hidden mic array contradicts it. This document is the
deliberate, bounded reversal for this one SKU: **what the mics are used
for, exactly how the pipeline works, and the three mechanisms that mean you
always know whether it is listening.** Everything here is enforced by code
that is host-tested or compile-gated, not by promises.

> **Status: Built · compile-gated · bench-pending.** The decision core is
> host-tested (`tests_host/test_mic_logic.cpp`, in CI); the runtime is
> compile-verified by the `canary-display-dash-mic` env; the **audio pins
> ship `-1` (VERIFY)** so the mics are provably un-driven until the bench
> session below fills them from the vendor schematic. A distinct privacy
> surface = a distinct product: its own board map
> (`boards/waveshare-esp32s3-lcd43c`), env, and OTA product
> (`securacv-canary-display-dash-mic`) that never cross-installs with the
> mic-free dashes — the same rule that separates Sense from
> Sense-Wellbeing. It joins the flasher only after the bench pass.

## What the mics are for — and only for

**Acoustic alarm patterns.** The two regulated household alarm grammars:

| Pattern | Sound | Event on the glass | Severity |
|---|---|---|---|
| **T3** (NFPA 72 / ISO 8201) | three ~0.5 s beeps, pause, repeat — every smoke alarm | `acoustic_smoke_alarm` | Alert |
| **T4** (UL 2034) | four ~0.1 s beeps, long pause, repeat — every CO alarm | `acoustic_co_alarm` | Alert |

That's the entire vocabulary. A smoke alarm screaming in an empty house is
exactly the event a fleet display exists to surface, and it requires no
speech, no recognition, no cloud. One detection needs **two consecutive
on-grammar cycles** (one group of beeps can be a horn; two matching cycles
are an alarm — host-tested), re-raises at most every 30 s while standing,
and lands as an **unsigned local event** (a display holds no signing key —
same honest footing as the 4.3B's door contacts).

## How it works — the privacy barrier

The pipeline is the WAP's proven scalars-not-samples design
(`securacv_audio.h`'s privacy barrier), reimplemented as a pure core:

```
I2S frame (~20 ms) ──► one RMS scalar ──► buffer ZEROED ──► loud/quiet edge
                                                             │
              host-tested cadence FSM (mic_logic.h) ◄────────┘
                       │
        {event, cycles, confidence}  — the ONLY thing that crosses
```

- Each ~20 ms frame is reduced to **one number** (DC-removed RMS); the
  sample buffer is `memset` to zero before the function returns
  (`src/io/mic_alarm.cpp`, `read_frame_rms`). No frame outlives its read.
- Downstream of that scalar there is nothing that could carry speech: no
  spectra, no MFCCs, no model, no recording, no streaming, no storage.
  The cadence detector consumes **booleans and milliseconds**
  (`include/canary/io/mic_logic.h` — pure, no audio types in its API).
- Nothing acoustic is published raw: detections enter the same fleet event
  pipeline as every other witness signal and are journaled like any event.

## You always know if it's on — three mechanisms, one bit

The core design rule: **the indicator is not a second flag that could
disagree with reality — it IS the driver state.**

1. **The amber ● MIC chip on the glass.** Lit exactly while the I2S
   capture driver is installed. The same gate action that installs the
   driver draws the chip and the same action that uninstalls it removes
   the chip (`apply_action` in `mic_alarm.cpp` is the only caller of
   both). The host test walks every reachable gate state and proves there
   is no listening-without-chip or chip-without-listening state to even
   represent (`test_indicator_cannot_desync`).
2. **The Settings row states it in words.** Settings → **microphone**
   reads `off`, `listening`, or `pins unset` — the same bit, rendered as
   text, with the full contract as the page caption. Debug mode's System
   page repeats it (`mic LISTENING rms N`) for the support-photo path.
3. **The console says it, timestamped.** The `MIC1` serial grammar
   (PG1-style): `HELLO` with the armed/pins state at boot, `EVT` on every
   arm/disarm/start/stop/detection, and a 1 Hz `SNAP rms=…` heartbeat
   that only exists while capturing — silence on the wire means a silent
   mic.

**Off is the default, and off is real.**

- Fresh from the flasher the mic does nothing: arming is an on-glass,
  NVS-persisted choice (Settings → microphone → listening), the exact
  siren-arm pattern. Nobody remote can arm it — there is no MQTT/web
  path to the toggle.
- Disarming performs a **hard mute**: `i2s_driver_uninstall()` — the
  driver is gone and the pins released, not a software flag over a live
  stream (the WAP's verifiable-mute precedent, quoted in its dashboard:
  "The I2S driver is uninstalled and the pins are released").
- Unset pins hard-block everything: the shipped map's `AUDIO_PIN_I2S_*`
  are `-1`, and the gate refuses to start regardless of arming (host-
  tested). A guessed microphone pin is the one guess this repo never
  ships.

The transparency sheet (tap the dash footer) changes on this board from a
flat "Never: … microphone …" to the live truth: `Mic: LISTENING (amber
chip lit)` or `Mic: off (driver uninstalled)`, plus the standing scope
line — *alarm patterns only, never speech; audio never recorded, never
leaves this board*. The mic-free dashes keep their unchanged "never
microphone" promise — this copy exists only where the silicon exists.

## The bench session (why you bought it)

The executable list lives in the board README
([`boards/waveshare-esp32s3-lcd43c`](../../firmware/boards/waveshare-esp32s3-lcd43c/README.md)):
confirm the codec silicon via the I²C census (ES7210 @0x40, ES8311 @0x18 —
and 0x51 is the PCF85063 RTC saying hello), fill the I2S pins from the
vendor schematic, arm, verify the chip lights the same instant, hold a
smoke alarm's TEST button toward the case-top grille and watch
`acoustic_smoke_alarm` land as an Alert, then disarm and verify the
driver-uninstalled report. Panel note: the C's ST7701 controller may need
an init sequence (VERIFY) — the mic layer runs headless either way, so the
mic bench is not blocked on the glass.

If the ES7210 needs register init before it clocks samples (likely), the
`SNAP rms=0` line makes that visible instantly; the init lands in
`mic_alarm.cpp` at the marked VERIFY point during the same session.

## Status ledger

| Piece | Where | Status |
|---|---|---|
| Board map (pins, caps, codec addrs) | `boards/waveshare-esp32s3-lcd43c` | compile-tested; audio pins **-1 (VERIFY)** |
| Gate + indicator invariant | `canary/io/mic_logic.h` + host test | **host-tested** (CI) |
| T3/T4 cadence detection | same core + host test | **host-tested** (CI) |
| I2S capture + hard mute + chip | `src/io/mic_alarm.cpp` | compile-gated (`canary-display-dash-mic`); bench-pending |
| ES7210 init | `mic_alarm.cpp` VERIFY point | **bench** |
| Settings row/page, debug line, transparency copy | gated UI edits | compile-gated; bench-pending |
| Flasher/release product | — | after the bench pass (distinct product reserved: `securacv-canary-display-dash-mic`) |
