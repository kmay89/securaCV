# The Nightstand Line — three new display boards (design & bring-up)

**Status:** design + hardware bring-up reference. **Firmware landed** for the 172×320 portrait flavor
(the `nightstand` flavor: `display_1in47.cpp` ST7789 HAL, `portrait_ui.cpp` face, `ambient_led.cpp`
WS2812 beacon) and the `dash7` flavor (the 7" reusing the Dash), both compiled in CI via the
`canary-display-nightstand-s3`, `canary-display-dash7` **and** `canary-display-nightstand-c6` build envs
(the C6 rides the core-3.x display base, §7) — **compile-tested, not bench-validated**, and all three are
release/flasher products. The **bedside wave** since added a fourth product on the 7" glass — the
`nightstand7` flavor (§5b: a drawn segment clock, complications, a night clock focus) — plus the
**lantern** night light §4 specified (§5c), the **ambient-life** layer and the **BOOT-button** grammar
(§5d), and weather **advisories + tomorrow** on the existing retained blob. Still staged (§7): the
**emulator / Lab** wasm wiring, portrait-native **modal polish**, and true **5-point touch gestures**.
This doc extends the existing display design — [`display_living_canary.md`](./display_living_canary.md),
[`display_nightstand.md`](./display_nightstand.md), [`display_care_wave.md`](./display_care_wave.md),
[`display_character.md`](./display_character.md) — it does **not** replace them.

**The idea:** three ordered boards that join the Canary Display family as **nightstand / ambient** nodes,
where **color is the primary language** — the onboard RGB LED and the glass communicate fleet state at a
glance across a dark room, the bird **breathes and lives**, and **a touch reveals detail**.

---

## 1 · The three boards and their roles

| Board (`BOARD_ID`) | MCU | Panel | Ambient LED | Role |
|---|---|---|---|---|
| `waveshare-esp32c6-lcd147` | ESP32-C6, 1-core 160 MHz, **no PSRAM** | ST7789 172×320 SPI | 1× WS2812 (GPIO8) | nightstand (pin-header) |
| `waveshare-esp32s3-lcd147` | ESP32-S3, 2-core 240 MHz, 8 MB PSRAM | ST7789 172×320 SPI (same panel) | 1× WS2812 (GPIO38) | plug-in ambient (USB-A stick) |
| `waveshare-esp32c3-lcd147` | ESP32-C3, 1-core 160 MHz, **no PSRAM** | ST7789T 180×320 SPI, CS/RST/**BL** via I2C EXIO expander | — (the glass IS the lamp) | **nightlight** (pocket case, kid's bedside) — 7-seg clock + companion + lamp, `CD_NIGHTLIGHT` face over this flavor's HAL, backlight duty HAL-capped at 50% (heat). See `firmware/configs/canary-display/nightlight/` |
| `waveshare-esp32s3-lcd7` | ESP32-S3R8, 8 MB **octal** PSRAM | 800×480 RGB parallel + GT911 5-pt touch | — | the big glass (desk) |

**Two families, not three:** the two 1.47" boards **share the ST7789 172×320 panel** → one new SPI HAL,
two pin maps. The 7" is **electrically the 4.3" Canary Dash at the same 800×480** → it reuses the Dash's
RGB HAL, GT911 driver, and CH422G handling; the only new work is the roomier layout and full 5-point touch.

**Hardware bring-up gotchas** (baked into the `pins.h` headers; full detail there):
- **ST7789 X-offset = 34** — the 172-px glass is a window into 240-wide controller RAM; every draw needs a
  34-px column offset at rotation 0, migrating on rotation.
- **Every 1.47" display pin differs C6↔S3**; the RGB LED is GPIO8 (C6) vs GPIO38 (S3); drive it via **RMT**
  (a bit-banged strobe glitches under Wi-Fi on the single-core C6).
- **C6 has no PSRAM + one core** → lean single-buffer rendering; the S3 (8 MB PSRAM, 2 cores) can double-buffer
  and animate richly. *Same look, two budgets.*
- **7": PSRAM is mandatory** (768 KB framebuffer), and **the backlight *and* the GT911 touch-reset ride the
  CH422G I2C expander (0x24), not native GPIOs** — the GT911 won't enumerate until reset through the expander.
  This is the #1 thing that breaks naive ports; the Dash already handles it.

---

## 2 · Color is the language — and the RGB LED is the primary ambient channel

The state vocabulary already exists and is the single source of truth ([`src/ui/theme.cpp`](../../firmware/projects/canary-display/src/ui/theme.cpp),
`character.cpp` `k_sem_canonical`): **safe = green `0x43A047`, check = amber `0xFB8C00`, help = red
`0xE53935`, signed = blue `0x03A9F4`**, with the red-shifted night palette (`theme.h` `ncol_*`). The new
boards don't invent colors — they push that language onto **two ambient surfaces**:

- **The onboard RGB LED** becomes the **primary across-the-room glance** — a single point of color you can
  read from bed without the screen lighting the room. It **breathes in sync with the bird** (§3) and is
  colored by the **worst active severity** in the fleet.
- **The glass** is the **detail-on-touch layer** — the living canary, the witness halo/cards, the timeline.
  At rest it's calm (or dark, §4); a touch brings the detail up.

This is the "utilize every bit of pixel and color" you asked for: the 262K-color panel for the rich detail
view, plus a whole extra color channel (the LED) for the ambient signal.

---

## 3 · Breathing & alive — reuse the engine, extend it to the LED

The "alive" behavior is **already built and host-tested** — we extend it, not rebuild it:
- [`bird_mood.h`](../../firmware/projects/canary-display/include/canary/care/bird_mood.h) — anxiety/trust →
  the face ladder (Calm/Worried/Searching/Calling/Distressed/Asleep/Hidden).
- [`canary_mark.cpp`](../../firmware/projects/canary-display/src/ui/canary_mark.cpp) `start_bob()` — the
  always-on breath (half-period = arousal: calm 1.4 s, alert 1.1 s, asleep 2.8 s).

**New:** the RGB LED **breathes the same waveform** — its brightness follows `start_bob()`'s phase and its
hue is `sev_color(worst_severity)`, so the point of light *inhales and exhales* with the bird. On the tall
172×320 glass the bird gets a **full-height portrait pose** (more vertical room than the 240 round), with a
soft color wash behind it keyed to the same severity. `canary_mark`'s brand hues and reaction one-shots
(Tilt/Startle/Greeting/Joyful) carry over unchanged.

---

## 4 · The honest night-light — the key design decision

Your "night light that shows state in color" collides with a **hard product invariant** the codebase enforces
everywhere ([`display_nightstand.md`](./display_nightstand.md) §Night): **silence/darkness must never be
rendered as safety** — a dark screen genuinely means "all is well," and *any* glow means "something wants
you." A green "safe" glow at night would make darkness ambiguous and teach people to ignore the glow. The
resolution is a **two-channel split** that keeps the ambient honest *and* gives you a real night light:

1. **The state channel (RGB LED + screen glow) stays honest.** At night, all-quiet + links-up → **dark**
   (the LED off, backlight 0), exactly as today. The LED/glass **breathe a color only when something wants
   you**: amber = check, red = help, blue = a fresh signed event. **"Safe" at night is *darkness*, never a
   green glow.** So the across-the-room LED is a *pure attention beacon* — dark = sleep easy, any color = look.
2. **The night-light proper is a separate, user-summoned mode**, decoupled from state. Tap/hold (or the BOOT
   button on the LED-only boards) turns on a **dim warm room light** at the calibrated night floor
   (`CD_BRIGHT_PEEK`-class), which **times out** — it is *not* a state signal, so it can be any comforting
   warm hue without ever lying about the fleet. Want a nightlight? Ask for one. The ambient channel still
   never says "safe" by glowing.

Everything else in the nightstand wave (dim-red tap-to-peek clock + comfort words, the two-phase wake, the
`character_set_night()` red-shift, the per-panel calibrated night floor) applies to these boards as-is; the
1.47" boards **add the LED as the after-dark attention beacon** the round/dash panels don't have.

---

## 5 · The 172×320 portrait layout (the real new UI work)

The existing layouts are geometry-specific — `glance_ui.cpp` assumes the 240 round (`mk_ring(…,232,10)`,
halo arc math); `dash_ui.cpp` assumes 800×480. **A tall 172×320 portrait is a genuinely new layout**, and
it's the largest firmware task here (not a config change). The design:

- **Top band — the living canary**, full-height portrait pose + a severity color wash.
- **Middle — the witness column:** instead of a round halo, a **vertical stack of witness rows** (one per
  Canary), each a color chip (`sev_color`) + room name; the worst floats up. Fits the narrow width naturally.
- **Bottom — the glance line:** `all quiet · N canaries` / time / comfort word, in the `theme` semantic colors.
- **Night:** dark by default; tap/BOOT peeks the dim-red clock + comfort line; the LED is the after-dark beacon.
- **Budget:** the **C6 renders single-buffered, dirty-region** (no PSRAM), trimming the wash/animation frame
  rate; the **S3 double-buffers** and runs the full breath/flourish set. Same layout, `#ifdef`-gated richness.

---

## 5b · The Nightstand 7 — the same 7" glass, turned toward the bed

The 7" board ships as **two products on one piece of hardware**, because a panel on a
wall and a panel on a nightstand want opposite things from the same pixels:

| | `canary-display-dash7` | `canary-display-nightstand7` |
|---|---|---|
| Standing face | `dash_ui` — card grid + timeline | `nightstand7_ui` — clock hero + complications |
| Night | data panel, backlight scheduled off | **clock focus**: red-shifted digits, tomorrow's weather, everything else black |
| Bedside wave | off (`NIGHT_BLACKOUT 0`, `WAKE_ALARM 0`) | **on**, plus `FEATURE_LANTERN` |
| OTA product | `securacv-canary-display-dash7` | `securacv-canary-display-nightstand7` |

Both keep `CD_FLAVOR_DASH` — the RGB HAL, the GT911, the CH422G and every 800×480 modal
surface are shared. The bedside build adds `CD_NIGHTSTAND7`, which swaps the standing
face and compiles `dash_ui.cpp` out entirely. **Distinct OTA products** so a bedroom glass
can never cross-grade into a wall dashboard.

**The clock is drawn, not typed.** The built-in Montserrat tops out at 48 px — nowhere near
a 7" bedside clock — so the hero is **seven-segment digits built from LVGL primitives**,
parametric on (w, h, thickness). It stays crisp at any size, recolors through the theme
choke point for free, and red-shifts at night with the rest of the palette. Unlit segments
keep a faint ghost by day (the resting shape of the instrument) and vanish at night, when a
dark room wants digits and not scaffolding.

**Every kind of "dim" on this board is rendered.** `HAS_BACKLIGHT_PWM` is 0 — the CH422G
backlight line is on/off, and I²C bit-banging it would flicker. So the whole
`night_duty`/`NIGHT_STEPS` calibration machinery is inert here, and the night lever is
**on-glass luminance**: a black ground, dimmed content, and the backlight scheduled off for
the true blackout. The gentle wake's sunrise is drawn the same way — a warm field rising
from the bottom, opacity tracking `wake_alarm_backlight()` — because there is no backlight
ramp to drive. The boot banner says this out loud so nobody debugs a missing dimmer.

## 5c · The lantern — the night light, finally built

§4 specified the honest night light and left it unbuilt. It exists now, as
[`care/lantern.h`](../../firmware/projects/canary-display/include/canary/care/lantern.h)
(pure model) + `src/care/lantern.cpp` (NVS glue), and it holds §4's contract in code:

- **User-summoned.** A tap on the affordance corner (7"), a tap anywhere (the small
  portrait glass — the whole panel is the lamp, because there is no corner to find at
  3 a.m.), or a **BOOT double-press** on the touch-less boards.
- **Timed out.** 15 minutes by default; re-summoning restarts the window. A lit lantern is
  deliberately **not persisted** — a device that reboots at 3 a.m. wakes dark, because
  dark-when-safe is the honest default and nobody asked for a light.
- **Extinguished by attention, and it does not resume.** The instant the fleet reaches
  Warn — or a link dies — the lamp yields the glass back to the truth and stays out until
  someone summons it again. A lamp that silently resumed over an alarm the user just dealt
  with would be exactly the dishonesty §4 was written to prevent.
- **Never a state signal.** Its light is a look-engine *scene*, never a semantic color, so
  it cannot say "safe" by glowing. The WS2812 beacon is **not part of the lamp** on any
  board: that channel stays a pure attention signal, dark when all is well.
- **"Lantern hours"** (an auto schedule through quiet hours — the hallway-light use case)
  exists but ships **off**. Turning it on knowingly trades away the dark-means-safe signal;
  that is a decision, not a default. The attention veto applies to it identically.

A tap while lit walks the scene ring, which now includes a **Rainbow** scene (the full
wheel on Sweep). The honesty rule covers it like every other scene: host-tested to cycle
all three primaries when calm, and to go red under Alert regardless.

## 5d · Ambient life — the glass that checks in

[`care/ambient_life.h`](../../firmware/projects/canary-display/include/canary/care/ambient_life.h)
is the Flipper-ish organic layer: on an idle, lit glass, one small living thing happens
every few minutes — the bird throws a flourish, or the status quietly surfaces and fades.
It reads as a companion checking in rather than a screen holding still.

It is deliberately **rationed and gated**, because the motion budget
([`theme.h`](../../firmware/projects/canary-display/include/canary/ui/theme.h)) is law:
3–7 minutes apart by day, 8–15 at night (a hallway lamp stirs; it does not perform). A
moment fires **only** when the fleet is calm, the links are healthy, no modal is open,
no wake window is running, and the glass is genuinely lit — daylight ambient or a lit
lantern. While the gate is shut the clock simply holds, and re-opening it never releases a
stored-up burst. The cadence is a seeded xorshift keyed to the device id, so two Canaries
on one dresser don't stir in lockstep while each stays reproducible across its own reboots
— which is what makes the whole layer host-testable.

The BOOT button finally has a grammar too
([`io/boot_button.h`](../../firmware/projects/canary-display/include/canary/io/boot_button.h),
the §7 follow-up): **tap** = peek, **double** = the lantern, **hold** = acknowledge. Double
fires on the second *press*, not its release — a light has to come on when you push the
button — and a press that is spoken for can never also mature into a tap or an
acknowledge.

## 5e · Hallway mode — the nightlight, made easy, and given a voice

§5c built the lantern and left two things unfinished. This wave closes both.

**The switch.** Everything a hallway nightlight needed was already in the tree and
none of it was reachable: you had to know the lantern has an auto schedule, that it
is called "lantern hours", that it ships off, and then separately pick a scene, a
brightness and a warmth that suit a corridor at 3 a.m. That is an expert path, and a
nightlight is not an expert feature.
[`care/hallway.h`](../../firmware/projects/canary-display/include/canary/care/hallway.h)
is one switch over that machinery — it writes the lantern's own settings rather than
adding a second way to be a lamp, so there is still exactly one lamp in the firmware
and exactly one place the honesty rules live. Three levels (Dim / Soft / Glow),
because a nightlight with a slider is a nightlight nobody finishes setting up.

It ships **off**, for the same reason `CD_LANTERN_AUTO` does. Hallway mode changes
how hard the trade is to make *deliberately*, not whether it is made for you.

**The dwell.** The piece that genuinely did not exist. A lamp that snaps to full the
instant quiet hours begin reads as a timer firing; one that rises over a few minutes
reads as evening settling. `level()` is a rise / hold / ebb envelope across the
quiet-hours window, fed by the new `hours_window_position()` in `glass_settings.h`
(midnight-wrap aware, host-tested). When the two ramps would overlap on a short
night, the lower wins — so the lamp still fades in and out, it just never reaches
full.

**The voice.** [`color/plumage.h`](../../firmware/common/color/plumage.h) is the
layer that makes the lamp read as a living thing rather than a color fade. The look
engine gives it a *palette*; plumage gives it a *language*. Birdsong is phrases of
syllables, and the syllable types are what make a species recognizable, so we render
five — **Chirp** (snap on, fall away), **Trill** (one note repeated fast, a shimmer
inside a syllable), **Warble** (pitch wandering, the lyrical one), **Whistle** (slow
in, long hold, the carrying note) and **Churr** (low and muttered, the bird talking
to itself) — mapping the two things a note has onto the two things light has:

- **pitch → a position along the active scene's palette**, so a "high" note is
  further around the same palette the lamp is already wearing. The song can never
  introduce a color the scene does not own, which is exactly what stops it from ever
  reading as a semantic color.
- **amplitude → an envelope ADDED to the resting lamp**, so the light only swells. It
  never dips: a hallway light that blinks out is a fault, not a phrase. (Host-tested
  as an invariant, not a habit.)

On the tall 172×320 glass a syllable is **a band that rises** — the note's glow
travels bottom-to-top over its duration, so a phrase reads as light climbing the
pane. That is the pattern: motion with a grammar behind it, not a scrolling gradient.
The glass itself changed to carry it: the lantern overlay was one object with a
two-stop vertical gradient (a fade), and is now a **stack of 14 bands**, each
carrying its own gradient to the next band's color — piecewise-linear, so it still
reads as one smooth field, but with enough independent stops for a glow to move
through it.

**The personality.** `Voice` is four traits — chatter, boldness, pitch, restlessness
— derived from the device id, so every Canary speaks a little differently and two on
one hallway never fall into lockstep while each stays exactly reproducible across its
own reboots. Same trick, same reason, and the same FNV-1a hash of the NVS device id
as `ambient_life`'s cadence: reproducible is what makes it host-testable. Every trait
lands in a musical middle band, so there is no id that yields a mute bird or a
strobing one.

Phrases are **rationed**, like every other motion in this family: a settle period
before the first word, a minimum gap the scheduler will never go below, and gaps that
more than double at night. A shut gate **holds** the clock rather than banking it —
re-opening it never releases a stored-up burst of speech.

**The honesty invariant, and the one place this wave touches it.** Plumage may only
dress the calm. Rather than restate that rule and risk it drifting, its two render
entry points hand `worst >= Warn` and `safe_dark` **straight back to the look
engine** — the song simply does not exist on those paths, so there is no way to call
plumage and get a decorated alarm. The host tests assert this as **bit-identical**
output, not "close".

The one thing that does change is the **beacon**. §4 and
[`hal/ambient_led.h`](../../firmware/projects/canary-display/include/canary/hal/ambient_led.h)
say the WS2812 is a pure attention channel and the lamp never routes through it.
Under Hallway mode it may (`CD_HALLWAY_BEACON`, and only while the lamp is lit). The
reasoning, written out in full at `care/hallway.h`: the invariant protects the
inference *darkness means safe*, and that inference is **already spent** the moment
lantern hours are on, because the glass is then lit all night regardless of state.
Letting the LED join a lamp that is already burning costs nothing further — the room
is provably calm, since attention extinguishes the lamp before it can be lit at all.
What would be dishonest is the reverse, a beacon glowing a scene color while the lamp
is *off*, and that never happens. With Hallway mode off — the default — the beacon
behaves exactly as it always has.

The glass publishes its lamp frame (`ui/look_state.h` `LampFrame`) for the beacon to
read, because only the glass knows whether the lantern is actually lit: the timeout,
the schedule and the veto all resolve there. The song is ticked in exactly one place
for the same reason — the beacon reads it without advancing it, so the point of light
and the pane are always mid-way through the same syllable.

**Host tests:** `firmware/tests_host/test_plumage.cpp` (honesty, add-only, per-device
reproducibility, the gate, the rising head) and the Hallway/dwell/window cases added
to `projects/canary-display/tests_host/test_bedside_models.cpp`.

**The body.** The USB-A stick finally has a case:
[`canary_s3_lcd147.scad`](./enclosure/canary_s3_lcd147.scad) — screwless, thumb-release
for the card, and a window in the back that turns the WS2812 into a wash on the wall
behind it. See the enclosure README.

---

## 6 · The 7" big glass + real 5-point touch

Same 800×480 canvas as the Dash → **the Dash layout ports straight over**, just breathier (larger type,
fatter touch targets, more whitespace) — "bigger physical glass, same pixels," as expected. The new capability
is **true 5-point multitouch** (the GT911 already reports up to 5; the pins carry `TOUCH_MAX_POINTS 5`):

- **Tap** a witness card → peek its detail (the existing Startle reaction fires).
- **Two-finger spread** on a card → zoom into that witness's timeline/proof; **pinch** → back out.
- **Swipe** between fleet card pages / the roll-call.
- Wire the **full 5-point report array**, not just point 0, so multi-finger gestures are real rather than a
  single clumsy poke — and remember the **CH422G touch-reset must fire before the GT911 enumerates**.

---

## 7 · Build status — landed vs. staged

**Landed this pass** (real firmware, compiled in CI on the S3 core-2.x graphics stack):

1. **HAL** — `src/hal/display_1in47.cpp` (ST7789 via Arduino_GFX, the 34-px offset, per-board pins, LEDC
   day/night backlight profiles) behind the new `CD_FLAVOR_NIGHTSTAND` guard. The 7" reuses `display_dash.cpp`.
2. **Ambient beacon** — `src/hal/ambient_led.cpp` + `include/canary/hal/ambient_led.h`: the WS2812 driven by
   the core's RMT `neopixelWrite`, breathing the `canary_mark` waveform, hue = worst severity, **dark-when-safe
   at night**. Ticked every loop pass in `main.cpp` (smooth breath), gated `FEATURE_AMBIENT_LED`.
3. **The portrait face** — `src/ui/portrait_ui.cpp` (§5): the breathing color wash, the full-height living
   canary (reusing the `canary_mark` engine), the severity-ordered witness column, the glance line.
4. **Config + envs + catalog** — `configs/canary-display/{nightstand,dash7}/config.h`, the
   `CD_FLAVOR_NIGHTSTAND` OTA branches, `[env:canary-display-nightstand-s3]` + `[env:canary-display-dash7]`
   (each its own OTA product) added to `flavors.json`'s build matrix, and `boards.json` `used_by` wired.
5. **Shared-surface fold-in** — the nightstand borrows the watch's small-portrait rendering for the shared
   modal/support surfaces (`splash`/`settings`/`commission`/`onboard`/`provision`, via a per-TU flavor alias);
   the standing fleet face is the bespoke `portrait_ui`.

**Landed since** (the §7 follow-ups that were staged here):

- **The C6 build** (`canary-display-nightstand-c6`) — the core-3.x display base landed:
  `[canary_display_c6_core3]` in `envs/platformio/canary-display.ini` pins the pioarduino platform (same
  55.03.38-1 release line as canary-sense/canary-wap) with the core-3 library row the Arduino CI already
  proves (`GFX@1.6.6` / `LVGL 9.x` / `NimBLE 2.x`), chain-mode LDF + an explicit `build_src_filter` for the
  shared `common/` TUs (the canary-sense pattern). In `flavors.json` `build_envs` **and**
  `isolated_core_envs` (the pioarduino platform cannot share a core dir with espressif32). Its own OTA
  product `securacv-canary-display-nightstand-c6`. Still *compile-tested* — bench validation pending like
  its siblings.
- **Release + flasher wiring** — all three boards (`dash7`, `nightstand-s3`, `nightstand-c6`) are release
  build targets now: `firmware-release.yml` (and the out-of-band `flasher-release.yml`) run their PlatformIO
  envs non-blocking and stage the binaries through the same version-string/product-string guards as the
  profile-built displays, and the flasher catalog (`gen_flash.py` → `flash.json`) carries their products.

**Landed in the bedside wave** (§5b–§5d above — compile-tested, bench validation pending like
everything else on these boards):

- **`canary-display-nightstand7`** — the 7" bedside product: its own flavor config
  (`configs/canary-display/nightstand7/`), env, OTA product, flasher-catalog entry and
  release-workflow staging (it rides dash7's core-3 core dir).
- **`src/ui/nightstand7_ui.cpp`** — the drawn segment clock, the complication column, the
  weather-advisory banner, the fleet strip, the night clock-focus layout, the rendered
  sunrise, and the lantern overlay.
- **The lantern** on all three bedside flavors (`nightstand7`, `nightstand`, `touch169`) —
  the §4 night light, built.
- **Ambient life + the BOOT-button grammar** — two of the §7 follow-ups, closed.
- **Weather got tomorrow + advisories** — optional `t_now` / `hi2` / `alert` fields on the
  same retained blob (`display_nightstand.md` § Optional fields), visual-only by rule.
- **A Rainbow scene** in the look engine (10 now, not 9).

**Landed in the clock-face wave** (the 7" touch-and-settings pass):

- **A settings doorway on the 7" faces.** The bedside face and the portrait column carry a
  quiet `⚙ settings` corner (day only — after dark the corner grammar belongs to the lantern
  and the wake tap). Before this, the Nightstand 7 compiled out the wall dashboard's only
  doorway (the transparency-sheet gear), so the shared settings surface existed on the build
  but no touch path reached it: the glass read as touch-dead.
- **The clock-face ring** (`ui/clock_styles.h`, `Settings.clock_style`, blob v4): four named,
  curated faces — Segment / Slab / Hairline / the drawn Analog dial (`ui/clock_face.cpp`) —
  flip-through picker on the settings sheet, same design law as the Character ring. A face
  changes the drawing, never the message; night still outranks everything.
- **The look, from the phone.** `/api/settings` on the dash family now serves `character`,
  `clock_style` and `orientation` (with the Character and clock catalogs BY NAME), and
  `/api/set` accepts them — applied on the loop task through the same paths an on-glass tap
  uses. The iPhone/iPad settings sheet renders the device's own catalog (GlassSettings).
- **Figures on the glass** (FLEET_FIGURES.md §10, closed): the fleet strip, the portrait
  witness list and the wall dashboard's roll call draw each witness's own generated figure
  (`ui/fleet_figure.cpp` over `common/core/fleet_figures_art.h`) beside its severity dot and
  name — an unresolvable wire type keeps a hidden fixed-size slot, never a guessed product.
- **The trustworthy bedside clock.** 12-hour digits with a quiet AM/PM (`clock_12h`,
  default on), a fifth clock face — **Calendar**: slimmer digits sharing the day hero with
  a mini month grid (`ui/calendar_math.h`, host-tested), night unchanged (a dark room keeps
  the clock as its one instrument) — and a clock-trust whisper on the date line once SNTP
  hasn't re-proved the wall clock for 48 h (or never has). Time still arrives via the
  two-source SNTP + `tz_auto`/`/api/tz` zone ladder; the whisper is the honesty layer on top.
- **Standalone weather (hub-less homes, opt-in).** `FEATURE_STANDALONE_WEATHER` +
  `net/wx_direct.cpp`: three gates (owner opt-in, stored coarse location, no hub ever
  configured), anonymous Open-Meteo HTTPS with a pinned root CA, feeding
  `bedside_on_weather` the exact retained-blob JSON a hub would publish — so the weather
  lines, tomorrow line and advisories behave identically either way. The opt-in is
  on-glass only: `/api/set` refuses `wx_direct` and `wx_loc` for every caller with
  `403 on_glass_only` (`net/settings_policy.h`, host-tested; the mirror page shows them as
  read-only rows pointing at settings → weather → fetch itself), so a host on the LAN
  cannot flip the glass's one opt-in outbound path or plant a location. That also closed
  the app's typed-place location path, and no on-glass location entry exists yet — a
  fresh device's second gate stays unsatisfied until one lands, and the weather page's own
  "set a coarse location from the app" caption is stale until the next emulator-dist
  rebuild carries a `settings_ui.cpp` fix (compile-tested). Disclosed in
  `docs/security/SECURITY_MODEL.md`; request shape pinned by `tests_host/test_wx_core.cpp`.

**Still staged (honestly deferred, needs a toolchain the CI container lacks or a follow-up):**
- **A WASM twin for the Nightstand 7.** It shares the 7" glass with dash7 but not its face, so
  the dash7 twin would misrepresent it — the flasher catalog therefore ships the product with
  **no emulated link at all** rather than one that lands on the generic gallery. (Fixing this
  properly also fixed two older overclaims: `dash-modes` and `nightstand-c6` had been deep-linking
  `fleet.html#<their own id>` for ids the emulator registry never knew. They now point at the
  sibling twin that genuinely shares their glass, labeled as a sibling and not as "1:1".) The
  emulator's Arduino shim did gain the GPIO surface the new BOOT-button code needs, with
  `emu_button()` for JS to push the level in — so the twin can drive the new gestures the moment
  a build exists.
- **Emulator + Lab** — `build.sh` `createCanaryEmuNightstand`/`…Dash7`, the `dist/*.js` + `.meta.json`
  (`fw_version == fw_train`), `registry.json` display entries (`glass{172,320,round:false,…}` / `{800,480,…}`),
  the `fleet.html` `<script>` tags, and the `app.js` `buildDisplaySheet()` **172×320 portrait sizing case**
  (today it hardcodes `round ? 232 : 464`). Needs Emscripten to build the committed wasm the `canary_local`
  tests assert on — a toolchain-session slice.
- **Portrait-native modal polish** — the shared modals render in the watch's 240-wide style on the 172-wide
  glass (functional, slightly overflowing); a portrait pass is a follow-up.
- **BOOT-button input + true 5-point gestures** — the 1.47" boards have no touch, so the button is the peek /
  summon-nightlight / acknowledge surface (unwired today); the 7" GT911 5-point gestures (§6) ride the dash UI.

**Honest status tiering** (`compile-tested → verified`): the landed flavors are *compile-tested* (CI builds
`nightstand-s3` + `dash7`); nothing is bench-validated. *Verified* waits on real hardware — the RGB/ST7789
timings, the CH422G bit map, the ST7789 offset, the S3 BGR/HSPI quirks, and the WS2812 color order confirmed
against the actual boards.

---

## 8 · The look engine — tremendous, gamma-true, honest

The nightstand's color is not hard-coded any more: it runs a small **look engine**
([`firmware/common/color`](../../firmware/common/color/)) that both ambient channels — the WS2812 beacon and
the glass wash — read, so the point of light and the pane always agree.

- **`color_engine`** — a gamma-2.2 LUT, integer HSV→RGB, warmth (white-balance), palette sampling with
  shortest-path hue interpolation, and the shared breath easing. **All integer / fixed-point**: the C6 has no
  FPU, so a float HSV per LED frame would be soft-float; LUT + integer keeps it cheap enough to run every loop
  pass on the single core. This is the honest form of "machine-code-level optimization" — hand-tuned fixed
  point + tables, not literal assembly.
- **`look_engine`** — nine **Hue-inspired scenes** (Canary Dawn, Ember, Aurora, Deep Calm, Forest, Tropical,
  Lantern, Nocturne, and Signal) as small HSV palettes with per-scene motion (Breathe / Sweep / Shimmer /
  Pulse / Comet). `led_color()` returns the beacon RGB for an instant; `wash_stops()` fills the glass gradient.
- **The inviolable rule, in code:** a scene only ever dresses the **calm**. The moment `worst >= Warn`, the
  engine abandons the scene and returns the true **semantic** color (amber / red) — the look can never hide an
  alarm — and `safe_dark` returns black, so darkness at night still means safe. This is unit-tested
  ([`firmware/tests_host/test_look_engine.cpp`](../../firmware/tests_host/test_look_engine.cpp)): the whole
  integer pipeline (gamma monotonic, HSV primaries, palette, breath, and the honest override) is proven on the
  host — a scene-blue Deep Calm still goes red under Alert — before it ever reaches a board.

The live controls (`LookParams`: scene, motion, brightness, speed, warmth, gamma, night) are held in
[`look_state.cpp`](../../firmware/projects/canary-display/src/ui/look_state.cpp). The **Look Studio** preview
(the interactive design artifact) is the faithful, playable spec for all of it.

**Staged next** (the alive info layer the studio previews): the on-glass **info carousel** — boot splash →
firmware version → witnessing count → sending status → chain verified, cycling with organic crossfades — plus
the **settings-surface scene picker** and an **MQTT look topic** (so a scene can be set from Home Assistant),
and the **BOOT-button** summon/cycle. The engine already produces everything they need.

---

*Hardware sourced from the Waveshare wikis + the CircuitPython/espp/TFT_eSPI board definitions (pin maps,
ST7789 offset, GT911 + CH422G, RGB-LED GPIOs, PSRAM). Design extends the existing display docs cited above;
the state-color source of truth is `src/ui/theme.cpp` / `character.cpp`, and the "silence is never safety"
invariant is enforced in `display_nightstand.md` and `theme.cpp`.*
