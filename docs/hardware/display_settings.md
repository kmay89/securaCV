# Display Settings — the on-glass settings surface

One design system, two renderers: the 1.28" round watch gets page-per-
setting screens (one screen, one decision), the 4.3" dash renders the same
tree in a roomier sheet. Research base: automotive HMI flat-hierarchy
doctrine (BMW iDrive "QuickSelect", Mercedes "MBUX Zero Layer"), watchOS
one-control-per-screen, Nest's the-value-is-the-screen, ecobee's brightness
taxonomy, Hatch's tap-to-peek, Garmin's Red Shift — and the NN/g anti-bloat
line: *every setting is a decision the designer refused to make.*

## The five principles

1. **Zero Layer** — anything touched weekly is ≤1 tap from the tree's root;
   the whole tree is ≤2 levels deep, ever. No "Advanced" ghetto.
2. **The Screen Is the Preview** — every visual setting applies live while
   you adjust it. Brightness dims the actual glass. No Apply buttons.
3. **One Screen, One Decision** — on the round display, always: each editor
   holds exactly one value, its state, and a way back.
4. **Defaults You Can Come Home To** — opinionated defaults good enough
   that most people never open Settings; a visible reset that *never*
   touches the black-point calibration.
5. **Night Is a Mode, Not a Menu** — schedule, glow level, red look, and
   screen behavior travel together; the toggles can't contradict.

## The tree (both flavors)

```
settings
├─ day light        (watch only — dash backlight is on/off in hardware)
├─ night light      (watch only — level 1..10 above the calibrated floor)
├─ night hours      (starts / ends, hour steps)
├─ night look       (soft red / plain)
├─ at night         (keep a low glow / go dark · tap peeks + peek 3/5/10 s)
├─ find the black point   (watch only — the calibration wizard)
└─ reset            (defaults; the black point stays)
```

Entry points: **watch** — the rotation's last page is a settings doorway;
long-press opens (a sleepy tap-cycle can never rearrange the screen).
**Dash** — the transparency sheet (footer tap) carries a gear row.

While open, the surface owns every gesture; long-press is the quick exit,
idle times it out, and **a live unacked alert closes it instantly** — a
settings sheet never stands between a person and a tamper. Values persist
to flash debounced (~2 s after the last change), as a versioned blob that
degrades to defaults on any layout/corruption mismatch.

## The backlight: two PWM profiles (watch)

The ESP32-S3 LEDC constraint chain is `freq × 2^bits ≤ 80 MHz`, and the
real floor of a small LED backlight is minimum *pulse width*, not minimum
duty fraction — too short an on-pulse and the LED chain never conducts.

| Profile | Freq | Bits | Why |
|---|---|---|---|
| Day | 5 kHz | 8 | silent, flicker-free; coarse steps invisible at reading brightness |
| Night | 1 kHz | 13 | 1 ms period keeps a duty-1 pulse (122 ns) near the turn-on edge; ~30 distinguishable steps inside what the day profile calls "duty 1" |

The night-light setting is a level 1..10 on a **log curve** from the
calibrated floor to ~10× the floor — at the bottom of a backlight the eye
works in ratios, so equal-ratio steps look equal-sized.

## "Find the black point" — the calibration wizard

Per-panel floor variance (transistor V<sub>be</sub>, LED binning, module
revision) makes hardcoding wrong; pro monitor calibration proves the
method (black-level patterns, done in the room the display lives in).

1. **Invite** — do it at night, lights how you sleep.
2. **Descend** — the glass shows the clock and dims itself ~18% per beat
   from ~2% duty; *tap the moment the glow disappears* (the whole screen is
   the target). The wizard steps back up a margin → floor candidate.
3. **Comfort** — pick the 3 a.m. glow (level 1..10 above the floor), live.
4. **Blink test** — the glass winks between off and the floor; confirming
   you saw it proves the floor actually emits light (and didn't just read
   as "on" from capacitance). "No" doubles the floor and retries.
5. **Store** — floor + glow persist. A floor ≥ ~5% duty is refused with a
   warning (inverted backlight polarity / wrong pin territory), and the
   factory floor stays.

The calibration lives under its own storage key: resetting settings keeps
it, re-running the wizard never touches your other preferences.

## Dash hardware truth

The 4.3B's backlight-enable line sits on the CH422G I/O expander —
**on/off only**; I2C bit-banged "dimming" would flicker horribly. So the
dash's night choices are honest ones: *stay lit with the dark look* or *go
dark, tap to wake*. There is a documented one-wire mod (decouple the
MP3302 EN pin from the expander and wire it to a free GPIO, e.g. GPIO6 —
see the Waveshare wiki / ESP32_Display_Panel discussion #185) that enables
true PWM; the firmware's day/night profile plumbing would drive it
unchanged, but the mod is documented, not required.

## Honesty contract (unchanged by any setting)

- "Go dark" only darkens a healthy, configured glass: any Warn+ condition
  or dead WiFi/hub link keeps the night glow. A never-configured display
  guards nothing and earns its dark room.
- An unacked Alert/Tamper always lights the glass at day brightness, and
  the gentle-wake sunrise ramp outranks every night level.
- Night wakes peek dim (`CD_BRIGHT_PEEK`), never day-blast; the peek
  length (3/5/10 s) is the one timeout users may tune.
