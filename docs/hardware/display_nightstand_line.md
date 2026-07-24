# The Nightstand Line — three new display boards (design & bring-up)

**Status:** design + hardware bring-up reference. **Firmware landed** for the 172×320 portrait flavor
(the `nightstand` flavor: `display_1in47.cpp` ST7789 HAL, `portrait_ui.cpp` face, `ambient_led.cpp`
WS2812 beacon) and the `dash7` flavor (the 7" reusing the Dash), both compiled in CI via the
`canary-display-nightstand-s3` and `canary-display-dash7` build envs — **compile-tested, not
bench-validated**. Still staged (§7): the **C6 build** (toolchain-blocked on core-2.x vs 3.x graphics),
the **emulator / Lab** wasm wiring, portrait-native **modal polish**, and true **5-point touch gestures**.
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

**Still staged (honestly deferred, needs a toolchain the CI container lacks or a follow-up):**

- **The C6 build** (`canary-display-nightstand-c6`). Toolchain-blocked, not design-blocked: the ESP32-C6 needs
  arduino-esp32 3.x (the pioarduino fork), but `canary-display`'s graphics stack is pinned to
  `GFX@1.4.9` — the last **core-2.x**-compatible release. A core-3.x display base (GFX@^1.5.0 + an LVGL/NimBLE
  3.x audit) is the gating work. The firmware itself is already C6-ready (single internal buffer, RMT LED); the
  S3 sibling carries the flavor today.
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
