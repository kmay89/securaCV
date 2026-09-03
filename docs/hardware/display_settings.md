# Display Settings — the on-glass settings panel

One design system, one renderer, every touch glass: the 1.28" round watch,
the 240×280 Touch-1.69 and 450×600 AMOLED portraits, the 4.3" / 7" dash and
its portrait column all render the **same settings tree** in the grammar
every phone already taught — a navigation bar, grouped rows on a rounded
surface, a value and a chevron where a tap opens an editor, a switch where
the decision is on/off, a check where it is one-of-few, a slider where the
screen is the preview, and a one-line footer under each group saying what
it costs. The list scrolls with momentum; nothing is ever packed to fit.

Research base: iOS/watchOS Settings (grouped inset lists, one control per
row, footers instead of help screens), automotive flat-hierarchy doctrine
(BMW iDrive "QuickSelect", Mercedes "MBUX Zero Layer"), Nest's
the-value-is-the-screen, ecobee's brightness taxonomy, Hatch's tap-to-peek,
Garmin's Red Shift — and the NN/g anti-bloat line: *every setting is a
decision the designer refused to make.*

## The five principles

1. **Zero Layer** — anything touched weekly is ≤1 tap from the root; the
   whole tree is ≤2 levels deep, ever. No "Advanced" door.
2. **The Screen Is the Preview** — every visual setting applies live while
   you adjust it. The brightness slider dims the actual glass under your
   thumb; picking an appearance restyles the panel you are looking at;
   turning the dash rotates the panel with it. No Apply buttons.
3. **One Screen, One Decision** — each editor holds exactly one value, its
   state, and a way back.
4. **Defaults You Can Come Home To** — opinionated defaults good enough
   that most people never open Settings; a visible reset that *never*
   touches the black-point calibration.
5. **Night Is a Mode, Not a Menu** — schedule, glow level, red look, and
   screen behavior live in one group and travel together; the toggles
   can't contradict.

## The tree

```
Settings                                       (Done · a stationary hold · idle timeout)
├─ DISPLAY
│  ├─ Brightness           80% ›        slider, live (day level · dash: rendered dim, 50–100%)
│  ├─ Appearance           Quiet Glass › the Character list, check on the current, live
│  ├─ Clock                Segment ›    dash only: 24-hour switch + the face list
│  └─ Orientation          Landscape ›  dash only: four quarter turns, live
├─ NIGHT
│  ├─ Quiet Hours          22:00 - 07:00 ›   two hour wheels, Starts / Ends
│  ├─ Night Light          Level 4 of 10 ›   PWM glass: slider, 1..10 above the floor, live
│  ├─ Red Shift            [switch]
│  ├─ Screen at Night      Low glow ›   Keep a Low Glow / Go Dark (+ Peek For 3 · 5 · 10 s)
│  └─ Find the Black Point Calibrated › PWM glass: the wizard
│     ↳ "During quiet hours the glass dims and goes quiet. An unacknowledged alert always lights it."
├─ CONNECTION
│  ├─ Wi-Fi                <network> ›  read-only: network · signal · hub link · this glass's address
│  ├─ Weather              Off ›        dash + standalone weather: the opt-in and its gates
│  └─ Firmware             v2.x.y ›     installed · available · Auto-Update switch · the one action
├─ SIREN        (4.3B)     Wired Siren [switch]  + footer
├─ MICROPHONE   (4.3C)     Listening ›  Listening switch · Sensitivity › · Wake on Sound switch
├─ Add a Canary ›          PWM glass (the dash has its own on the transparency sheet)
├─ Get Help ›              the Help Desk QR for the current verdict
├─ Modes ›                 when the build carries gears (bench / demo / debug / arcade)
└─ Reset Settings · Forget Wi-Fi Network        confirm pages, alert-colored verbs
```

Every destructive act (reset, forget Wi-Fi, entering a mode) is a confirm
page with one filled verb and a quiet Cancel — never a second tap on the
same row.

## Entry points

| Glass | Doorway |
|---|---|
| watch (round) | the rotation's last page invites; a long-press there opens |
| Touch-1.69 · AMOLED 2.41 | a quiet gear in the top-right corner of the portrait face, by day |
| Nightstand 7 · portrait column | the `⚙ settings` corner, by day |
| Dash (landscape poster) | the transparency sheet's gear row |
| 1.47" nightstands · nightlight (no touch) | the phone page (`glass_web`) — the panel is compiled out |

While open, the panel owns every gesture. Three exits, all promised:
**Done** (or the bar's back chevron up to the root), a **stationary
long-press** anywhere that is not a control (a thumb resting on a slider
knob or an hour wheel never counts), and the **idle timeout** (60 s on the
watch, 120 s elsewhere). **A live unacked Alert/Tamper closes it
instantly** — a settings sheet never stands between a person and a tamper.
Values persist to flash debounced (~2 s after the last change), as a
versioned blob that degrades to defaults on any layout/corruption mismatch.

## One tree, every glass

The layout is measured from the live canvas (`compute_metrics` in
`ui/settings_ui.cpp`), not from a flavor:

| Class | Glass | Rows | What changes |
|---|---|---|---|
| Compact | 240 round, 240×280 | 44 px | a row's value sits **under** its title in caption type — a 180 px row cannot hold "Screen at Night" and "Low glow" side by side, and a watch face has always answered that by stacking |
| Regular | 450×600, 480×800 column | 56 px | title left, value right, as a phone |
| Regular · sheet | 800×480 dash | 56 px | a centered 620×440 sheet on the true ground, so the poster stays a poster behind it |

The round puck insets its rows 30 px so they clear the rim from the bar
down to the last fully visible row (chord math in `ui/round_frame_core.h`)
and pads the bottom of the list so the last row can scroll up out of the
bezel — the Apple Watch answer to a circle. Every other glass is full-bleed.

## Input: LVGL-native, policy unchanged

The panel is the first surface that lets LVGL own input. `ui/lvgl_port.cpp`
registers one pointer device on every flavor (v8 and v9) that reports the
**last fed sample**; `main.cpp` keeps polling the touch controller and its
gesture policy (wake, page, hold-to-ack, mute) is untouched — it only hands
samples to `settings_ui_handle_touch` while the panel is open, so the faces
never see an LVGL click. Rows click, lists scroll with momentum and
scrollbar, switches flip, sliders and hour wheels drag, and a press
highlights the row under it. On LVGL 9 the dash glass rotates pointer
samples itself; the HAL already un-rotated them, so the feed pre-inverts
LVGL's transform (`rotation_to_lvgl_indev`, host-tested against LVGL's own
formula in `tests_host/test_display_settings.cpp`) and a tap lands where the
finger is on every quarter turn.

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
works in ratios, so equal-ratio steps look equal-sized. The AMOLED and the
Touch-1.69 map both profiles onto their own dimming (panel command / PWM)
through the same HAL calls, so the row set is identical across the PWM
family.

## "Find the black point" — the calibration wizard

Per-panel floor variance (transistor V<sub>be</sub>, LED binning, module
revision) makes hardcoding wrong; pro monitor calibration proves the
method (black-level patterns, done in the room the display lives in).

1. **Invite** — do it at night, lights how you sleep. *Begin.*
2. **Descend** — the glass shows the clock and dims itself ~18% per beat
   from ~2% duty; *tap the moment the glow disappears* (the whole screen is
   the target). The wizard steps back up a margin → floor candidate.
3. **Comfort** — pick the 3 a.m. glow (a slider, 1..10 above the floor),
   live. *Keep This.*
4. **Blink test** — the glass winks between off and the floor; confirming
   you saw it proves the floor actually emits light (and didn't just read
   as "on" from capacitance). *No* doubles the floor and retries.
5. **Store** — floor + glow persist. A floor ≥ ~5% duty is refused with a
   warning (inverted backlight polarity / wrong pin territory), and the
   factory floor stays. If storage balks the page says "kept for tonight"
   instead of a false "saved".

The calibration lives under its own storage key: resetting settings keeps
it, re-running the wizard never touches your other preferences.

## Dash hardware truth

The 4.3B's backlight-enable line sits on the CH422G I/O expander —
**on/off only**; I2C bit-banged "dimming" would flicker horribly. So the
dash's night choices are honest ones: *keep a low glow* (the dark look,
still lit) or *go dark, tap to wake*. There is a documented one-wire mod
(decouple the MP3302 EN pin from the expander and wire it to a free GPIO,
e.g. GPIO6 — see the Waveshare wiki / ESP32_Display_Panel discussion #185)
that enables true PWM; the firmware's day/night profile plumbing would
drive it unchanged, but the mod is documented, not required.

## The dash rows — orientation, brightness, clock

**Orientation.** The 800×480 glass turns into a 480×800 column for a wall
mount stood on end or a tall bedside face. LVGL software-rotates the whole UI
(`lvgl_port_set_rotation` → `lv_display_set_rotation`); the panel keeps
scanning its native landscape, and raw GT911 touch is un-rotated back into the
logical frame in the HAL (`touch_set_rotation` → `rotation_map_touch`, the
exact inverse of the render turn — host-tested by round-trip). Landing on an
option *is* choosing: the glass — panel and all — turns under your thumb, and
the panel re-measures its canvas and rebuilds in the new shape. Portrait
swaps the landscape poster (`dash_ui` / `nightstand7_ui`) for one shared
portrait column, `portrait7_ui`.

> **Bench-validation pending**, like the rest of the 7" line. LVGL's software
> rotation in partial-render mode is the intended path; the coordinate + touch
> math is proven on the host, but the panel render itself must be confirmed on
> real glass. Landscape (rotation 0) is the default and a no-op, so boot is
> never sideways.

**Brightness — "up to 50% sustained."** The 7"/dash backlight is binary
(CH422G, no PWM — see *Dash hardware truth* above), so a brightness setting
can't lower real backlight power. Instead it's an honest *rendered* dim: a
black scrim on LVGL's top layer, live as you drag, over the always-on glass.
It bottoms out at 50% on purpose (`BRIGHT_PCT_MIN`) — darker than that is
Night's job, and the footer says so. Honesty outranks the dim: an unacked
Alert/Tamper takes the scrim fully off, and the night face never carries a
day scrim on top.

**Clock.** The 24-hour switch and the face list (Segment / Slab / Hairline /
Analog / Calendar, each with its one-line caption) share a page; the footer
says the face wears it when you leave Settings, because the hero digits live
under the sheet.

## Connection rows — on every touch glass

These used to be dash-only; every flavor links the same `canary::net`
facades, so the watch and the portrait glasses now carry them too.

**Wi-Fi.** Read-only by design — joining is the onboarding wizard's job and
forgetting lives under *Reset* — so the page is pure honest state: the joined
network's name, the signal as a word (`signal_word`, never dBm), the hub
link, and the one address where this glass answers on the LAN. The address
is composed by the same `canary/net/hostname.h` recipe mDNS registers (the
MAC-free pseudonym suffix, Invariant III) — and the `.local` name is only
ever *claimed* while `discovery_up()` says mDNS actually registered it this
boot; otherwise the page shows the numeric IP, which `glass_web` answers on
regardless. The footer keeps rule 4 honest: the *page* is LAN-only and
witness data never leaves the home, but the firmware does touch the internet
for signed update checks — and, on an opted-in hub-less build, the coarse
weather fetch — so it says exactly that. The page refreshes ~1 Hz while open
(never under a finger, and back to the same scroll offset), so carrying the
panel around the house *is* the signal survey. While the link is down, the
signal row carries *why*, in the same shared `join_failure_label` words the
onboarding portal and the serial log use.

**Firmware.** The version the glass is running, and the one signed,
rollback-safe path to a newer one — the same pull-OTA engine Home Assistant
drives (`securacv_ota`, `docs/firmware_ota.md`), through the thin
`canary::net` facade. Installed vs. available, a status line, an Auto-Update
switch, and exactly one action for the state — *Check for Updates* or
*Install Now* (a muted "Installing N%" while busy). No version strings are
typed on the glass; the signed manifest and the pinned release key are the
only truth.

## Honesty contract (unchanged by any setting)

- "Go dark" only darkens a healthy, configured glass: any Warn+ condition
  or dead WiFi/hub link keeps the night glow. A never-configured display
  guards nothing and earns its dark room.
- An unacked Alert/Tamper always lights the glass at day brightness, and
  the gentle-wake sunrise ramp outranks every night level.
- Night wakes peek dim (`CD_BRIGHT_PEEK`), never day-blast; the peek
  length (3/5/10 s) is the one timeout users may tune.
- The panel never adds a capability: every row edits a value the firmware
  already held. What it removes is the packing — no row is ever dropped or
  squeezed to fit a build's optional extras.
