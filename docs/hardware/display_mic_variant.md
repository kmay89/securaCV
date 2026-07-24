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
> are now filled from the vendor's own pin-mapping table** (captured in
> `board_facts.json`, drift-locked to the board map) — facts, not guesses —
> leaving the **ES7210 register init** as the one remaining bench step. A
> distinct privacy surface = a distinct product: its own board map
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

That's the entire alarm vocabulary. A smoke alarm screaming in an empty house
is exactly the event a fleet display exists to surface, and it requires no
speech, no recognition, no cloud. One detection needs **two consecutive
on-grammar cycles** (one group of beeps can be a horn; two matching cycles
are an alarm — host-tested), re-raises at most every 30 s while standing,
and lands as an **unsigned local event** (a display holds no signing key —
same honest footing as the 4.3B's door contacts).

**And one opt-in convenience: wake the screen on a sound.** A dark wall dash
can light up the moment you walk in — a door close, a knock, a footfall
crossing a quiet room. This is **off by default** (Settings → microphone →
wake on sound) and, crucially, **rides the exact same barrier**: it watches
the same one-number-per-frame RMS envelope the alarm path does — a loud
*onset* well above the tracked ambient (host-tested `TransientDetector`,
refractory-gated so one door is one wake) — and never learns *what* the sound
was. A wake is "the room got suddenly loud", nothing more: no sample, no
classification, no recording, nothing sent. It only ever sets a wake window,
the identical path a finger-tap gives the glass. When it's off, the mic is
back to alarm patterns only.

**The cadence windows are the standards, not guesses.** The detector's
beep-duration windows are derived from the published timing plus what a room
adds: T3 is a 0.5 s ±10% beep ([ISO 8201 / ANSI S3.41 / NFPA 72](https://www.oaktreeproducts.com/smoke-detector-signal)),
T4 is four 100 ms ±10 ms pulses then a 5 s ±0.5 s pause
([UL 2034](https://www.cpsc.gov/s3fs-public/pdfs/COAlarmConformanceReportPhaseI.pdf)).
The windows stay **disjoint** (T3 ≥ 350 ms, T4 ≤ 300 ms) so a miscount can't
cross-classify smoke as CO; the group-gap sits between T3's 0.5 s intra-beep
gap and its ~1.5 s inter-cycle pause; and the streak-reset gap must exceed
T4's 5 s pause or a standing CO alarm would reset its own streak — every one
of these is host-tested against the source timing.

## How loud is a "beep"? Sensitivity presets, not a magic number

The one genuinely room-dependent value is the *level* at which a sound counts
as a beep — and the honest way to set it is **relative to the room's own
noise floor**, not as an absolute number tied to the ES7210's (unknown until
bench) gain. The physics makes this exact: a UL-listed alarm emits **≥ 85 dBA
at 10 ft** ([UL 217](https://customfiresecurity.com/blog/smoke-alarm-decibel-sound-requirements-what-ul-217-en-14604-as-3786-and-nfpa-72-require);
≥ 79 dBA low-frequency), while household ambient runs ~30 dBA (bedroom) to
~65 dBA (noisy kitchen). So an alarm stands **+14 dB above the room worst
case, +20–40 dB typically** — a threshold expressed as "N dB over the tracked
floor" lands correctly *regardless of absolute gain*.

The envelope therefore tracks the room with an **asymmetric noise-floor
follower** — it rises slowly toward a louder reading (a brief beep barely
lifts it) and falls fast toward a quieter one (the gaps pull it back), so it
settles on the room's true quiet level and a standing alarm can never inflate
the bar it must keep clearing. Three presets set the margin (Settings →
microphone → **sensitivity**, NVS-persisted):

| Preset | Room | Margin (on / off) | Trades for |
|---|---|---|---|
| **quiet** | bedroom / nightstand | +9 dB / +5 dB | sensitivity — catches a faint or through-the-wall alarm while you sleep |
| **standard** *(default)* | living areas | +10 dB / +6 dB | balance |
| **noisy** | kitchen / workshop | +13 dB / +8 dB | specificity — ignores clatter, tools, music |

The dB margins are gain-independent physics; only the dead-silence clamp
(`floor_min`) is a soft bench anchor, and it fails **safe** when set low.
Nuisance edges below the alarm cadence are harmless — the cadence detector
rejects anything that isn't a T3/T4 group — so the presets can lean
sensitive. **The `MIC1 SNAP` line prints the live `floor`/`on`/`off`** so the
bench sets `floor_min` in one glance: watch it in a quiet room, then confirm a
real alarm's beeps clear `on`.

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

**The cadence detector runs on the audio clock, not the UI loop.** Each
frame *is* 20 ms of sound; the runtime advances a frame-counted clock by
that 20 ms per frame and feeds *that* to the detector — never `millis()`.
This matters because the main loop redraws an 800×480 panel, so it runs far
slower than the frame rate and drains several buffered frames at once. Timing
those frames by wall-clock would collapse a beep group onto one instant and
silently break the duration windows (a missed alarm, never a false one — but
missed is the wrong way to fail for a smoke alarm). The DMA holds 160 ms of
depth so a UI stall loses no audio, and each listening session resets the
envelope + cadence state to silence (re-arming inherits nothing).

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
   (PG1-style): `HELLO` with the armed/pins/sensitivity state at boot,
   `EVT` on every arm/disarm/start/stop/detection/sensitivity change, and a
   1 Hz `SNAP rms=… floor=… on=… off=… sens=…` heartbeat that only exists
   while capturing — silence on the wire means a silent mic, and the
   `floor`/`on`/`off` fields are the bench's calibration readout.

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
- **The gears never listen.** The mic runs only under the fleet face:
  bench, demo, debug, and arcade modes boot before `mic_begin` and never
  call the mic loop, so entering any gear is itself a mute (debug's
  System page still reports the board's pin state truthfully — it reads
  the pin map, not the uninitialized gate).

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

The ES7210 needs register init before it clocks samples, and that init is
now **written** (`es7210_init` in `mic_alarm.cpp`): a datasheet-grounded
bring-up — soft-reset, I2S slave, 16-bit frame, mic channels powered with a
mid-scale PGA gain, DC high-passed — run right after the I2S master starts.
The bench job is no longer to write it but to **confirm its values**: the
`SNAP rms` climbing off zero in a live room is the pass signal, and the gain
(regs 0x43/0x44) and clock ratio (OSR reg 0x07) are the two knobs to turn if
capture is silent or clipped. `es7210_init=<n>` on the `MIC1` console line
reports how many config writes the ADC refused (0 = it took the whole
sequence).

After the bench passes, the contract faces its real judge: the
[usability protocol](./display_usability_protocol.md)'s **task H** — five
strangers must answer "is it listening?" correctly every time, with no
assisted passes allowed on the safety-critical probes. The indicator
invariant is host-tested; whether humans *read* it is tested there.

## Status ledger

| Piece | Where | Status |
|---|---|---|
| Board map (pins, caps, codec addrs) | `boards/waveshare-esp32s3-lcd43c` | compile-tested; audio pins **vendor-filled** (drift-locked to `board_facts.json`) |
| Gate + indicator invariant | `canary/io/mic_logic.h` + host test | **host-tested** (CI) |
| T3/T4 cadence detection | same core + host test | **host-tested** (CI) |
| I2S capture + hard mute + chip | `src/io/mic_alarm.cpp` | compile-gated (`canary-display-dash-mic`); bench-pending |
| ES7210 init | `es7210_init` in `mic_alarm.cpp` | **written** (datasheet bring-up); compile-gated; bench-validate gain/OSR |
| Settings row/page, debug line, transparency copy | gated UI edits | compile-gated; bench-pending |
| Flasher/release product | — | after the bench pass (distinct product reserved: `securacv-canary-display-dash-mic`) |
