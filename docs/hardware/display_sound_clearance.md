# Canary Voice — acoustic clearance

*Why the display's sounds are safe to ship: original, unencumbered, and never
confusable with a real emergency signal.* This is the companion to the sound
grammar itself (`display_trailblazer_spec.md` §5) and the engine
(`firmware/projects/canary-display/include/canary/hal/voice_score.h`). Two of
its claims are **machine-checked** — see
[`test_voice_clearance.cpp`](../../firmware/projects/canary-display/tests_host/test_voice_clearance.cpp),
run in CI — so they can't quietly rot as the palette evolves.

## 1. Originality — nothing borrowed

Every signature is an **original composition written for this device**. There
is no sampled, recorded, or transcribed third-party audio anywhere in the
engine: the firmware carries only note tables (frequency + duration + envelope)
that it synthesizes live on a piezo. Concretely:

- **The pleasant voices** (Boot, Sunrise, Heartbeat, Joined, Acknowledge, page
  turns, mute) are built from a **major-pentatonic scale** (C6–E7). Musical
  scales and the intervals between notes are not copyrightable — they are the
  common-property building blocks of music — and these short phrases are our
  own. *Checked:* the clearance test asserts these voices contain only
  pentatonic pitches, so the "family" stays what the doc says it is.
- **No product chime is reproduced.** We deliberately do not imitate any
  recognizable branded sound — operating-system startup/boot chimes, phone or
  console power-on jingles, messenger or streaming-service stingers, or any
  other trademarked audio logo. Boot is a rising warbled two-note chirp of our
  own; Joined is a plain ascending pentatonic arpeggio (a generic musical
  gesture, not anyone's mark).
- **The alarm follows a public *principle*, not a protected *melody*.** Its
  fast burst with a wide pitch jump is the shape **IEC 60601-1-8** recommends
  for a perceptually-distinct high-priority alarm — guidance in a published
  standard, applied via **two bare frequencies (2.6 / 3.1 kHz)**. We do not use
  that standard's specific notated alarm melody, nor any manufacturer's siren
  recording. *Checked:* the test asserts the alarm uses only those two
  frequencies and stays fast.

## 2. Non-confusion — a status chirp is not a life-safety alarm

The Canary display **shows** household security state; it is **not** a fire
alarm, carbon-monoxide alarm, or any listed life-safety appliance, and its
glass always carries the state in words regardless of sound. It must therefore
never emit a sound a household is *trained to obey as an evacuation order*, or
someone could ignore the real appliance — or panic at a notice. The two
regulated cadences we specifically hold clear of:

| Signal | Cadence | Meaning | Our clearance |
|---|---|---|---|
| **ISO 8201 / NFPA 72 Temporal-Three (T3)** | ~0.5 s ON, ~0.5 s OFF ×3, long gap, repeat | Fire — evacuate | No signature has half-second pulses; our fastest notices are ≤ 0.24 s |
| **Temporal-Four (T4)** | four short ~0.1 s single-tone pulses, long silence, repeat | Carbon monoxide | No signature is four isolated equal-pitch beeps; the alarm is a dense 10-pulse two-tone burst, Joined is a rising musical arpeggio with near-legato gaps |

We likewise reproduce **no** emergency-broadcast data tones (EAS/SAME), no
standardized emergency-vehicle siren sweep, and no telephone/mobile alerting
tone. *Checked:* the clearance test reduces every signature to its ON/OFF pulse
pattern and asserts none matches the T3 or T4 detector; the detectors
self-verify against the genuine cadences so the guard can't degrade into a
rubber stamp.

Distinctness *within* our own grammar (a fault must not sound like an intruder;
all-clear falls) is a separate, also-tested property — see
`test_voice_score.cpp`.

## 3. The preview can't rot either

The browser sound preview (`canary-local/voice/`) is **generated from this
firmware header by the real compiler** — never hand-transcribed — and a CI
drift gate (`canary-local.yml`) fails if the committed page falls out of step
with `voice_score.h`. So what a reviewer *hears* in the preview is always
exactly what the device *plays*, and this clearance analysis stays true to
both. See `canary-local/tools/gen_voice_preview.mjs`.

## 4. If you add or change a signature

1. Keep pleasant voices in the pentatonic set; keep the alarm on its two IEC
   frequencies and fast — the clearance test enforces both.
2. Never introduce half-second repeated pulses (T3) or four isolated
   single-tone ~0.1 s beeps (T4); the test will fail if you do.
3. Don't imitate a recognizable branded/trademarked sound — there is no
   automated check for that, so it's on the author and the reviewer.
4. Regenerate the preview (`node canary-local/tools/gen_voice_preview.mjs`) and
   commit it, or CI's drift gate will fail.
