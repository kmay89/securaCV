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

## The 7" wave — orientation, brightness, network, firmware (dash glass)

The 7" Dash 7 (wall) and Nightstand 7 (bedside) ride the same RGB HAL as the
4.3" Dash, so the settings sheet grows four rows on every `CD_FLAVOR_DASH`
build. They follow the same five principles — one screen, one decision, and
*the screen is the preview*, taken literally.

```
settings  (dash family: 4.3" · Dash 7 · Nightstand 7)
├─ … the shared rows above …
├─ orientation   (landscape · portrait · their two flips — live)
├─ brightness    (50–100% sustained — a rendered dim, not backlight PWM)
├─ network       (wifi · signal in a word · hub link · the glass's .local address)
└─ firmware      (installed version · check · install · auto-update)
```

**Orientation.** The 800×480 glass turns into a 480×800 column for a wall
mount stood on end or a tall bedside face. LVGL software-rotates the whole UI
(`lvgl_port_set_rotation` → `lv_display_set_rotation`); the panel keeps
scanning its native landscape, and raw GT911 touch is un-rotated back into the
logical frame in the HAL (`touch_set_rotation` → `rotation_map_touch`, the
exact inverse of the render turn — host-tested by round-trip). Landing on an
option *is* choosing: the glass — settings sheet and all — turns under your
thumb. Portrait swaps the landscape poster (`dash_ui` / `nightstand7_ui`) for
one shared portrait column, `portrait7_ui`: a segment-clock hero, the
household's one-word state in its own hue, the living canary, a vertical
witness list (worst floated to the top), and an honest glance line — the same
instrument whether it's on a wall or a nightstand. Night red-shifts it and
strips it to the clock + the state channel; dark-when-safe still holds.

> **Bench-validation pending**, like the rest of the 7" line. LVGL's software
> rotation in partial-render mode is the intended path; the coordinate + touch
> math is proven on the host, but the panel render itself must be confirmed on
> real glass. Landscape (rotation 0) is the default and a no-op, so boot is
> never sideways.

**Brightness — "up to 50% sustained."** The 7"/dash backlight is binary
(CH422G, no PWM — see *Dash hardware truth* above), so a brightness setting
can't lower real backlight power. Instead it's an honest *rendered* dim: a
black scrim on LVGL's top layer, live as you step it, over the always-on
glass. It bottoms out at 50% on purpose (`BRIGHT_PCT_MIN`) — darker than that
is Night's job, and a wall panel sitting all day behind a heavier scrim would
be dishonest about a backlight that's still at full. Honesty outranks the dim:
an unacked Alert/Tamper takes the scrim fully off, and the night face (already
dark, backlight possibly cut) never carries a day scrim on top.

**Network.** Read-only by design — joining is the onboarding wizard's job and
forgetting lives under *reset* — so the page is pure honest state: the joined
network's name, the signal as a word (`signal_word`, never dBm), the hub link,
and the one address where this glass answers on the LAN. The address is
composed by the same `canary/net/hostname.h` recipe mDNS registers (the
MAC-free pseudonym suffix, Invariant III), so what the glass tells you to type
is what actually resolves — it opens the glass's own web page: a live mirror,
these same settings, and a spinnable 3D model of the device. The page
refreshes ~1 Hz while open, so carrying the panel around the house *is* the
signal survey. While the link is down, the signal row carries *why*, in the
same shared `join_failure_label` words the onboarding portal and the serial
log use — one vocabulary, three surfaces.

**Firmware.** The version the glass is running, and the one signed,
rollback-safe path to a newer one — the same pull-OTA engine Home Assistant
drives (`securacv_ota`, `docs/firmware_ota.md`), surfaced to the sheet through
a thin `canary::net` facade so the UI never touches the raw engine header. The
page reads live (installed vs. available, a status line, install progress) and
offers exactly one action for the state — *check for updates* or *install now*
— plus the nightly *auto-update* toggle. No version strings are typed on the
glass; the signed manifest and the pinned release key are the only truth. A
wall Dash 7 and a bedside Nightstand 7 are distinct OTA products, so neither
can cross-grade into the other.

Settings model: `rotation` and `bright_pct` join the versioned `Settings`
blob (`glass_settings.h`), which steps to v3 and migrates a v1/v2 blob
field-for-field — turning the glass or dimming it never costs a user the night
hours, glow, or Character they already tuned.

## Honesty contract (unchanged by any setting)

- "Go dark" only darkens a healthy, configured glass: any Warn+ condition
  or dead WiFi/hub link keeps the night glow. A never-configured display
  guards nothing and earns its dark room.
- An unacked Alert/Tamper always lights the glass at day brightness, and
  the gentle-wake sunrise ramp outranks every night level.
- Night wakes peek dim (`CD_BRIGHT_PEEK`), never day-blast; the peek
  length (3/5/10 s) is the one timeout users may tune.
