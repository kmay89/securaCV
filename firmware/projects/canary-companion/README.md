# canary-companion — the Night Watch and the Pocket Canary

Two products on one wrist board. **The Night Watch** is a bedside clock that goes
genuinely dark, because AMOLED lets it. **The Pocket Canary** is a virtual pet
with Tamagotchi's charm and deliberately without the guilt loop that made
Tamagotchi a story about a dead animal.

- **Design:** [`docs/design/canary_companion.md`](../../../docs/design/canary_companion.md)
- **Board:** [`waveshare-esp32s3-amoled206`](../../boards/waveshare-esp32s3-amoled206/README.md)
- **Sibling project:** [`canary-tincan`](../canary-tincan/README.md) — same board,
  same haptic add-on, different product (two children, not one child and one bird)
- **Status:** **Phase 0.** The pure cores are written and **host-tested in CI**.
  There is no PlatformIO environment and no runtime yet — see "What does not
  exist" below.

## The two ideas

**The Night Watch: black is free here.** Every bedside screen this project
researched draws the same top complaint — *still too bright at night* — and the
Canary Display could not fully fix it, because on a backlit LCD "off" is a
glowing grey rectangle. This board is AMOLED. An unlit pixel is off. So the
Night Watch inverts the display's default and ships `GoDark`, with one rule
that outranks the owner's preference: **silence is never rendered as safety.**
Trouble in the fleet, or a clock that does not know what time it is, breaks
blackout every time — a dark screen is a claim of wellness, and neither of those
states may make it.

**The Pocket Canary: the reward curve goes flat on purpose.** The 1996
Tamagotchi needed checking once an hour, scored your absences forever, and
killed the pet if you lost. This one cannot die, has no bell, has no random
reward, and stops paying growth once the day's care is done — care still
*works*, it just stops buying anything. A device whose reward curve goes flat is
a device a child puts down, and that is the intended outcome.

## What exists

Everything below builds and runs with plain `g++`, with no board attached:

```
firmware/projects/canary-companion/include/canary/companion/
  pet_model.h        the care loop, the refusals, growth
  pet_play.h         Echo and Steady, and the rule that ends a session
  night_clock.h      the brightness ladder, the honesty override, the gentle wake
  raise_gesture.h    wake-on-raise and the wrist tap
  haptic_voice.h     channel probing, honest fallback, night silence
  settings_nav.h     the touch settings tree

firmware/projects/canary-companion/tests_host/
  test_companion_cores.cpp    40 checks across 6 groups
```

```sh
cd firmware/projects/canary-companion/tests_host && make
```

## Why the cores come first

Same reason as the Tin Can, which shares this board: the decisions that are
expensive to get wrong do not need the hardware.

The tests are written to fail loudly on the **specific** mistakes, not to
exercise a happy path. Each is named after the mistake:

| Test | The mistake it exists to catch |
|---|---|
| `test_pet_never_dies_and_never_regresses` | a lifespan creeping back in — it walks **a year of total neglect** |
| `test_absence_costs_nothing_permanent` | scoring a child's absence against them |
| `test_ask_never_rings` | the pull becoming a push (a notification, a buzz, a screen wake) |
| `test_bond_daily_cap_flattens_the_reward` | the flat reward curve being "optimised" into a streak |
| `test_trouble_overrides_blackout` | a dark screen that means "trouble" as readily as "all is well" |
| `test_rollover_does_not_fire_at_night` | a bright panel firing at a sleeping face at 3 a.m. |
| `test_voice_never_claims_a_missing_channel` | claiming a buzz on a board with no motor fitted |
| `test_destructive_leaf_needs_two_gestures` | a settings mis-tap wiping a device |

`test_rollover_does_not_fire_at_night` earned its keep during development: the
first version of the raise detector measured its settle on the **Z axis alone**,
and a roll-over holds Z roughly constant for stretches while X and Y sweep
through a whole rotation. It sailed through. Stillness is now measured on all
three axes.

## The hardware caveat that governs both products

**There is no vibration motor on this board** — not in the schematic, not in the
vendor tree. A **DRV2605L + LRA on the exposed I²C port (0x5A)** is required
hardware here, because a bedside clock that can only answer with light and noise
wakes the wrong person, and a pet you cannot feel is a pet you have to be
looking at.

So `haptic_voice.h` treats output channels as an **input**, never an assumption:
the runtime probes at boot, `voice_plan()` never names a channel that is not
there, and the boot screen says so out loud — *"no motor fitted — nudges will be
seen and heard, not felt."* A device that claims it buzzed and did not is exactly
the surprise that leaves someone waiting for an answer that already arrived.

## What does not exist yet

No PlatformIO environment, no `src/`, no CO5300 display driver, no LVGL, no
DRV2605L HAL, no MQTT glue, and **no factory image** — which is why there is no
entry in the flasher catalog and no card in the desktop Flasher app.

That is the repo's rule working, not a shortfall: `gen_flash.py` verifies every
product's `env` and `board` against the firmware tree, so a catalog entry without
a runtime does not merely mislead — it does not generate. Adding a build env
before there is a runtime to build would put a green tick next to something that
does not exist.

Next, in order:

1. **Bring-up.** Confirm the touch controller is really an FT3168 (the store copy
   says CST9220 and the vendor code disagrees), get LVGL onto the CO5300 at
   410 × 502, and bench a DRV2605L + **LRA against ERM** before anything is built
   on top — a mushy buzz is a dead product for both halves of this design.
2. **The Night Watch runtime.** The glass engine is done; what is missing is the
   panel driver, the RTC read, and the MQTT subscription that makes `trouble` real.
3. **The Pocket Canary runtime.** The bird renderer, and NVS persistence for
   `PetState` (write-light — the day rollover is the only mandatory write).
4. **Then the catalog**, and the Flasher on macOS and Linux lists it
   automatically, because it reads that one generated file.

## Before anyone calls this a kids' product

Unchanged from [the Tin Can](../canary-tincan/README.md), and it is still the
section that stops a launch: CPSIA third-party testing and ASTM F963, a
child-resistant battery enclosure in the spirit of Reese's Law, a breakaway
strap, and FCC/CE. A bare Li-po strapped to a child's wrist is the largest
physical risk in the whole design and no firmware decision touches it.

The Pocket Canary adds one obligation that is not physical: every refusal in the
design is a **design** refusal, not a certification. Nobody has run this in front
of an actual seven-year-old yet.
