# The display motion engine — adaptive, rationed, honest

> Code: [`include/canary/ui/motion_core.h`](../../firmware/projects/canary-display/include/canary/ui/motion_core.h)
> (pure math, host-tested) +
> [`include/canary/ui/motion.h`](../../firmware/projects/canary-display/include/canary/ui/motion.h) /
> [`src/ui/motion.cpp`](../../firmware/projects/canary-display/src/ui/motion.cpp)
> (LVGL glue). Tests:
> [`tests_host/test_motion.cpp`](../../firmware/projects/canary-display/tests_host/test_motion.cpp).
> Design law it extends: the motion budget in
> [`display_ux_design.md`](display_ux_design.md) §Design language.

## Why an engine

Every display flavor ships different physics. A full frame on the round
watch costs ~25 ms of SPI wire; the 800x480 RGB glass scans for free but
pays per repainted pixel; the PSRAM-less C3 renders single-buffered at
160 MHz. Until this wave, every face guessed — and the guesses were static
(`CD_UI_FRAME_MS`), invisible, and unmeasured. The engine replaces the
guessing with three layers:

1. **A capability model** derived from the board facts the pins headers
   already carry (geometry, bus kind and clock, PSRAM, CPU, the lean flash
   budget). Nothing is hand-assigned per board: a new `pins.h` is
   classified by its physics the day it lands.
2. **A motion tier** — `Lean` / `Standard` / `Full` — that sets the grammar
   this glass may afford: how long motions run, whether micro-polish
   animates or snaps, whether ambient scenes exist at all.
3. **A frame-time governor** that watches what each `lv_timer_handler`
   pass *actually* costs at runtime and parks decorative motion — never
   semantic motion — when the glass is spending its frames elsewhere.
   Degrade is fast (a few heavy frames), recovery is slow (sustained
   light ones), so quality never flaps.

The same wave raises LVGL's refresh ceiling from 30 ms to 16 ms (~60 fps)
on every non-lean build (`include/lv_conf.h`). Dirty-region rendering means
an idle face pays nothing for the faster ceiling; eased motion gets twice
the frames; and the governor is the honest backstop where the ceiling
proves optimistic.

## The tiers, on today's fleet

| Tier | Boards (derived, not assigned) | What it means |
|---|---|---|
| **Full** | the RGB dash/7" family (S3, PSRAM, render-bound) | the whole grammar: veil transitions, digit morphs, minute sweep, weather scenes at full budget |
| **Standard** | watch, nightstand-s3, touch169 (S3, bus-bound SPI) | everything Full runs, shorter and at half the ambient particle budget — small-region motion is cheap here, full-frame motion is metered by the wire |
| **Lean** | nightstand-c6, nightlight-c3 (no PSRAM, 160 MHz, 4 MB) | micro-polish snaps (`dur_ms(Micro) == 0`), no ambient scenes, stock 30 ms refresh — fewer frames is this hardware's honest answer |

One code path everywhere: faces call the engine, and a 0 ms duration *is*
an instant style write. The nightlight's digits go through the same
`motion::seg_opa` as the 7" bedside clock's — on the C3 it resolves to the
write the face always did.

## Effect classes and who may veto them

| Class | Members | Vetoed by |
|---|---|---|
| **Semantic** | alert breathing, ack ring, heartbeat | nothing — meaning carried by motion is never traded away |
| **Transition** | the veil ground-swap, page fades | nothing (tier sets pace; Lean snaps) |
| **Micro** | digit morphs, minute sweep, value/calendar fades | an unacked Alert/Tamper (attention belongs to the alarm) |
| **Ambient** | the weather scene, the wash breath | night, any unacked alarm, an open modal, the Lean tier, a trimmed governor |

G5 and the care-wave rules stand untouched: nothing here runs against the
Night floor, decorates an alarm, or renders safety as light. The render
pass feeds the engine its gate once per tick (`motion::set_context`), so a
face never re-implements the rules.

## What moved in this wave

- **The veil ground-swap.** Day/night, Character, rotation and clock-style
  changes used to be a one-frame hard cut (`lv_obj_clean` + rebuild). Now a
  single solid veil — colored to the *target* ground — eases over the old
  face, the rebuild runs under full cover, and the veil lifts off the new
  one. One object's `bg_opa`, never a layer composite: the LVGL v9
  compositing cliff on the 800x480 glass (see `onboard_ui.cpp`'s documented
  halt) is designed around, not risked.
- **Seven-segment digit morphs.** A minute change eases the dying segments
  out and the newly lit in (~160 ms, ease-out) on all three drawn clocks
  (7" bedside, portrait column, nightlight — snap on Lean).
- **The minute sweep.** The Analog dial's hands glide the short way to the
  new minute (ease-out) instead of jumping 6°.
- **The calendar's entrance.** A new month staggers its day numbers in,
  reading order, 8 ms apart (per-part `text_opa`). A mere day change still
  just re-inks two cells.
- **The weather scene.** The 7" bedside face's complication column gains a
  clipped field where the condition the wire already carries *moves*: rain
  falls slanted, snow sways, clouds drift in three-layer parallax, fog
  breathes sideways. Words stay the interface (no icon vocabulary, no new
  wire fields — `wx_core.h`'s plain-language set maps to a scene); the
  field is Ambient class, so it exists only on a calm, lit, unmodal day —
  and clear weather is, correctly, *still*.
- **The backlight glide.** Ladder rungs (Ambient to Active and back) ease
  over ~300 ms on PWM glass. An alarm's brightness still lands instantly;
  binary backlights and the Lean tier snap as before; the night-profile
  path and the live brightness editor cancel any glide first.
- **The wash breath answers to the governor.** The bedside wash keeps its
  color field always, but its opacity oscillation parks while the governor
  reports the glass is out of headroom, and resumes when it returns.

## The rationed-motion law, restated

The design language's table ("everything else: none") still binds — this
wave *extends the sanctioned list*, it does not repeal the ration. Every
motion above has a job (orientation, continuity, or information), a class,
and a veto path, and the engine enforces all three mechanically. A wall
object at rest is still: the weather field holds still at night, under any
alarm, under any modal, on lean glass, and in clear weather.

## Adding a motion (the checklist)

1. Pick the class (`Fx`) honestly — Ambient if it moves at rest, Micro if
   it decorates a state change, Semantic only if the motion *is* the
   information.
2. Ask the engine, don't read config: `motion::allowed(fx)`,
   `motion::ms(dur)` (a 0 result is an assignment, not an error),
   `motion::ambient_ok()` for standing decoration.
3. Animate per-part opacity or geometry — never group opacity, never
   screen-load fades (the v9 cliff).
4. The anim's `var` is the object it moves, so `lv_obj_clean` reaps it —
   or an engine-owned static that nothing deletes. No third option
   (glance_ui's use-after-free lesson).
5. Update the motion table in `display_ux_design.md` and this page in the
   same change, with the job and the veto path named.
6. If the math is non-trivial, put it in `motion_core.h` (pure, no LVGL)
   and pin it in `tests_host/test_motion.cpp`.

## Follow-ups this wave deliberately left

- **A "reduce motion" preference** (settings blob v6 + the phone settings
  page) so a person can choose stillness the way they choose a night mode.
  The engine is ready — the gate is one more input.
- The portrait column and the small nightstands render no weather field
  yet (the 7" bedside face is the proving ground).
- The AMOLED watch board (`QspiCmd` bus, brightness-as-command) classifies
  when its display env lands.
- Governor-driven refresh retiming (the period is compile-time today; the
  governor only parks decoration).
