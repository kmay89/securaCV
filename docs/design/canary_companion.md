# Design: the Canary Companion — a night-side clock and a pocket bird

**Status:** design + **Phase 0 cores landed**. The pure engines in
[`firmware/projects/canary-companion/include/canary/companion/`](../../firmware/projects/canary-companion/include/canary/companion/)
are written and **host-tested in CI**. There is no PlatformIO environment, no
display driver and no runtime yet — see [§8](#8--what-exists-and-what-does-not)
and the honesty rule that governs it.
· **Date:** 2026-08-02
· **Target board:** Waveshare **ESP32-S3-Touch-AMOLED-2.06** ("the watch board")

> *"It has speaker and vibration and motion sensor — let's make it awesome with
> our firmware as a night-side companion clock with touch and settings, and also
> a keychain companion version that's fun like a Tamagotchi with the Canary.
> Study the full addiction of Tamagotchi and make it kid friendly and keep all
> the charm."*

**The one-sentence version:** the same wrist board becomes two devices — **the
Night Watch**, a bedside clock that goes genuinely dark because AMOLED lets it,
and **the Pocket Canary**, a virtual pet built with Tamagotchi's charm and
deliberately without the guilt loop that made Tamagotchi a story about a dead
animal.

---

## 1 · Where this sits

This repo already has a wrist board and a design on it: the
[**Tin Can**](canary_tincan_kids_watch.md), a kids' watch where two siblings tie
a string and knock at each other with no voice, no text and no cloud. The Tin
Can is about **two children**. The Companion is about **one child and one bird**,
and about **one adult and one bedside table**. They share a board, a pin map, a
haptic add-on and a refusal list; they do not share a product.

| | Tin Can | Night Watch | Pocket Canary |
|---|---|---|---|
| Who | two siblings | one adult, one nightstand | one child |
| The point | a knock across the house | knowing the time and that all is well | a bird that is glad you came back |
| Radio | ESP-NOW strings | fleet MQTT subscriber | none required |
| Status | Phase 0 cores | **Phase 0 cores (this doc)** | **Phase 0 cores (this doc)** |

It also inherits the project's existing living canary
([`display_living_canary.md`](../hardware/display_living_canary.md)) — and §4.2
is entirely about how a *toy* bird and a *diagnostic* bird can live in one
codebase without either one lying.

---

## 2 · The hardware, honestly

Everything in [§1 of the Tin Can doc](canary_tincan_kids_watch.md) applies
unchanged, and the two traps it names are still the two traps:

- **The touch controller is FT3168 @ 0x38**, not the CST9220 the store copy
  claims. Vendor code beats vendor marketing. Confirm at bring-up: a wrong touch
  driver is a dead watch, not a degraded one.
- **There is no vibration motor on the board.** Not in the schematic, not in the
  vendor tree. A **DRV2605L + LRA on the exposed I²C port (0x5A)** is required
  hardware for both products here, because a bedside clock that can only answer
  with light and noise wakes the wrong person, and a pet you cannot feel is a
  pet you have to be looking at.

**The board is confirmed and on the requester's desk** (2026-08-02), which moves
this design past the Tin Can's "no board on anyone's desk yet" caveat: bring-up
is now a thing someone can actually do, and the two traps above are the first
two things to do it to.

> **If your board is a different model** — Waveshare's watch-shaped line also
> includes the AMOLED-1.8 (368 × 448) and a 1.85" round LCD, and some revisions
> carry a motor the 2.06 does not — then the pin map changes and nothing else
> here does. Every engine in this design is board-agnostic by construction and
> takes hardware presence as an *input* (`VoiceCapabilities`), never as an
> assumption. Register the board, write its `pins.h`, and both products follow.

### 2.1 The one thing AMOLED changes

This matters enough to be its own section, because it **reverses a bench verdict
this project already made**.

The nightstand research ([`display_nightstand.md`](../hardware/display_nightstand.md))
found the same complaint at the top of every review of every bedside screen ever
shipped: *still too bright at night*. The products people love — Braun, the
Loftie blackout, Mui — emit **zero** light until asked.

The Canary Display could not take that advice. Its bench verdict was blunt: on a
backlit LCD, "off" is not off. Backlight bleed leaves a glowing gray rectangle,
which reads *worse* in a dark room than a calibrated dim glow. So the LCD
nightstand ships a dim floor and treats blackout as a preference.

**The watch board is AMOLED, and that verdict does not carry.** An unlit AMOLED
pixel is off — no bleed, no gray, no rectangle, and no power. Black is free here
in a way it has never been free anywhere else in this project.

So the Night Watch **inverts the display's default**: `NightStyle::GoDark` is
what ships, and [§3.2](#32-the-rule-that-outranks-the-users-preference) exists
to make sure "dark" never quietly starts meaning "safe".

---

## 3 · The Night Watch

A clock you can stand to have beside your head.

### 3.1 The brightness ladder

Seven states, in
[`night_clock.h`](../../firmware/projects/canary-companion/include/canary/companion/night_clock.h).
Brightness on this panel is a **command, not a PWM duty** (there is no backlight
pin), so these are panel levels, each a bench-set destination rather than a
point on a curve:

| Mode | Level | When |
|---|---|---|
| `Awake` | 200 | touched or raised, daytime |
| `Ambient` | 90 | daytime idle |
| `Evening` | 40 | the first hour of quiet hours — someone is still reading |
| `Peek` | 20 | a deliberate look, deep night |
| `Night` | 6 | the held glow, if you chose one |
| `Dark` | 0 | genuinely off |
| `Vigil` | 40 | trouble. Overrides everything below. |

`Evening` exists because 22:30 and 03:30 are different rooms. At 22:30 someone
is awake in the room; at 03:30 nobody's eyes are adapted to anything. Collapsing
them into one "night" level is why so many bedside clocks are simultaneously too
dim to read at bedtime and too bright at three.

Night rendering is **red-shifted** (`nw_red_shifted()`), mapped onto the existing
`theme.h` `ncol_*` palette: long wavelengths disturb dark adaptation least.

### 3.2 The rule that outranks the user's preference

**Silence is never rendered as safety.**

If the fleet is Warn-or-worse, or a link is down, or the clock does not know what
time it is, the glass keeps a visible presence *no matter what the owner chose*.
A dark screen is a **claim of wellness**. A device that goes dark both when
everything is fine and when it has lost the plot has taught its owner nothing.

Two details that are easy to get wrong and are each pinned by a test:

- The override runs **before the hold timer**, so trouble arriving mid-peek does
  not politely wait for the peek to lapse (`test_trouble_interrupts_a_peek`).
- **A clock that is unsure of the time counts as trouble.** No RTC read and no
  SNTP means the device cannot make the wellness claim, so it doesn't
  (`test_trouble_overrides_blackout`).

### 3.3 The gentle wake

Three channels, in order, none of them escalating to a shriek:

| Lead | Phase | What happens |
|---|---|---|
| −15 min | `Glow` | brightness ramps from the night floor toward day, linearly |
| −2 min | `Buzz` | **haptics join** — still no sound |
| 0 | `Sound` | the chime joins, from its quietest step |

Haptics before sound is the whole reason a wrist device is a better alarm than a
nightstand one: **a buzz on a wrist wakes exactly one person in a bed.** The
alarm's job is to end sleep, not to win an argument with it.

The ramp is linear in *panel level*, not in perceived brightness, on purpose —
the perceptual curve front-loads the light, which is the opposite of gentle.

### 3.4 Wake-on-raise, and the failure that matters

[`raise_gesture.h`](../../firmware/projects/canary-companion/include/canary/companion/raise_gesture.h)
turns QMI8658 samples into a raise. It is judged on two failure modes that pull
in opposite directions and are **not symmetric**:

- **False negative** — you lift your wrist and the screen stays dark. Costs a
  deliberate second gesture. Mildly annoying.
- **False positive** — you roll over at 3 a.m. and a bright panel fires six
  inches from a sleeping face. Costs the product.

So the detector is biased hard toward silence, and `night_strict` biases it
further after dark (a steeper final tilt, and roughly double the settle time).

A raise is three things, and **all three** must be present: motion, then a tilt
toward the face, then a **hold**. The hold is the part most implementations skip
and the part that stops the 3 a.m. false positive, because a roll-over passes
through every tilt angle on its way somewhere else and rests at none of them.

> One bug found by writing the test first: an early version measured the hold on
> the **Z axis alone** — the axis that gates the tilt. A roll-over holds Z
> roughly constant for stretches while X and Y sweep through a whole rotation,
> so it sailed through. Stillness is now measured on all three axes against the
> pose the settle started from (`test_rollover_does_not_fire_at_night`).

The device also has a fallback a wrist-only product does not: this is a
nightstand clock as often as it is a watch, and **a tap on the glass always
works**. A missed raise is never the only way in.

### 3.5 Touch and settings

[`settings_nav.h`](../../firmware/projects/canary-companion/include/canary/companion/settings_nav.h)
owns *where you are* and *what a gesture does*. The stored preferences are not
redefined — the display's [`glass_settings.h`](../../firmware/projects/canary-display/include/canary/glass_settings.h)
blob already owns day brightness, night screen, red shift, peek window and quiet
hours, along with the debounced-write and separate-calibration-key discipline
that was learned the hard way. This is the navigation over it, plus the two
knobs a wrist adds: haptic strength and wake-on-raise.

**Four gestures, no more** — every additional gesture on a small glass is a
gesture someone performs by accident:

`Tap` select/step · `SwipeUp`/`SwipeDown` move · `LongPress` back

Five pages, one level deep (**Night · Wake · Feel · Bird · About**). A wrist
device with three levels of menu is a device whose settings nobody finds twice.

Three properties, each with a test named after the mistake:

1. **A mis-tap never commits something destructive.** There is exactly one
   destructive leaf ("forget everything") and it is the only one that confirms —
   two separate deliberate gestures, never one slip.
2. **You can always get out.** A long press exits from *any* depth; there is no
   screen without that escape.
3. **An abandoned edit reverts, and says so.** A settings screen that timed out
   into a *committed* change is a bug you find weeks later on a device that got
   dim for no reason you can reconstruct. A **deliberate** back-out keeps the
   edit — you were watching the value change. Only a timeout reverts, because
   only a timeout means nobody was looking.

---

## 4 · The Pocket Canary

### 4.1 Research: what the Tamagotchi actually did

The P1 shipped in Japan in November 1996 and ran three gauges — hunger (4
hearts), happiness (4 hearts) and discipline (a percentage). It rang an
**attention bell** when a gauge emptied, and counted a **care mistake** if the
bell went unanswered for about fifteen minutes. Care mistakes and discipline
mistakes together decided which adult you got: good care grew the beloved
Mametchi, bad care grew the squat, short-lived Tarakotchi. Keeping it alive meant
checking it roughly **once an hour**.

Neglect it far enough and it got sick. Then it died. And the death was *yours* —
a single owner, solely responsible, with a grave on the screen.

That last mechanic is both the reason it sold forty million units and the reason
it was banned from classrooms. The critical literature is unusually blunt about
the machinery: a school psychologist quoted at the time described a toy that
"creates a real sense of loss and a mourning process," and the design-press
retrospectives file it under **"fun pain"** — an early, extremely effective
dopamine-and-guilt loop, ancestor to the streak and the pull-to-refresh.

There is one more piece worth naming. The P1's only game was **left-or-right**:
the pet faces a direction, you guess, you are right half the time. That is a coin
toss with a sprite on it. It cannot be practiced and cannot be understood, so the
only thing bringing a child back is the reward schedule. It is, precisely, a slot
machine for seven-year-olds.

**Sources:** [Tamagotchi Wiki — the 1996 pet](https://tamagotchi.fandom.com/wiki/Tamagotchi_(1996_Pet)) ·
[the original P1 manual](https://archive.org/stream/bandai-tamagotchi-p1-1996/bandai-tamagotchi-p1-1996_djvu.txt) ·
[Thaao's P1 care guide](https://thaao.net/tama/p1/) ·
[Digital Trends — how Tamagotchi shaped tech habits](https://www.digitaltrends.com/cool-tech/how-tamagotchi-shaped-tech/) ·
[Game Developer — "Tamagotchi, FarmVille, and Fun Pain"](https://www.gamedeveloper.com/design/tamagotchi-farmville-and-quot-fun-pain-quot-) ·
[Joe Edelman — The Tamagotchi Trap](https://medium.com/what-to-build/on-the-dangers-of-mindful-and-well-being-based-design-81a165fd0597)

### 4.2 The two channels, and why they must never mix

This is the architectural decision the rest of the pet hangs off.

This repo already has a living canary, and it is a **gauge, not a toy**
([`display_living_canary.md`](../hardware/display_living_canary.md)). Its
governing property is borrowed from Pwnagotchi: *every face maps 1:1 to a state a
log line can name*. No random sadness for variety, no cheerful mask over a
degraded system. That property is what lets an experienced user read the face
instead of the log — and a pet that invents feelings would destroy it.

So the Pocket Canary runs **two channels that never blend**:

| | **The Weather** | **The Bond** |
|---|---|---|
| Source | real fleet health, via `care::BirdMood` | the child's own care history |
| Owns | the bird's **posture** (Calm/Worried/Searching/Asleep…) | the bird's **growth** and its flourishes |
| Honest? | diagnostically, always | it is openly a game and never claims otherwise |
| Vocabulary | the existing face ladder | plumage, size, earned idle moments |

They meet at **exactly one point, in one direction**: a real household kindness —
an alert an adult acknowledged, a fully-verified pass — feeds the Bond as
`Care::HouseholdTended`, a **bonus**. A healthy, attended home grows a bird a
little faster.

**There is deliberately no penalty direction.** A sick home costs the child
nothing. `test_household_is_bonus_only` asserts that an untended home is simply
the *absence* of the event and changes nothing about the pet, because a child
must never be made to feel responsible for a red fleet.

### 4.3 What we keep, and what we refuse

The charm is real and worth having. **Kept:** needs that move on their own, so
the bird has a life between visits; a visible response to care; growth you cannot
rush, so a three-week-old bird genuinely *is* different; and rare, earned
flourishes, because rarity is the reward.

**Refused** — each of these is a one-line change to make and a very hard thing to
notice later, so each has a test named after the mistake:

| Refusal | How it is enforced |
|---|---|
| **No death.** `PetState` has no dead state and no lifespan. The floor is `Fledgling` and a long sleep; the bird always comes back. | `test_pet_never_dies_and_never_regresses` walks **a year of total neglect** |
| **No guilt.** Neglect makes a *quieter* bird that brightens the moment you return — never a reproach aimed at a child. | `warmth` is the only absence-sensitive scalar, and nothing is gated on it |
| **No hourly leash.** Needs move at 2/hour, so a fed bird is content for a school day. And **the bird sleeps when the child does** — a pet that decays overnight greets a seven-year-old with a deficit they had no way to prevent. | `test_night_accrues_no_deficit` |
| **No variable reward.** There is no RNG in the pet at all. It is a pure function of elapsed time and care events — which is also what makes it host-testable. | `test_play_is_deterministic` |
| **No streak punishment.** A missed day costs one rung of warmth, never the ladder. `bond` is monotonic and never decays. | `test_absence_costs_nothing_permanent` |
| **No bell.** `pet_ask()` reports what the bird *would mention if you looked*. It is a pull, never a push — no notification, no buzz, no screen wake. | `test_ask_never_rings` |
| **No overfeeding trap.** The P1 punished overfeeding with sickness and an earlier death — a trap laid for a child who found the button that plays the animation. Here a full bird simply declines, and **nothing bad happens**. | `test_overfeeding_is_declined_not_punished` |

### 4.4 The single most important number

```c
static constexpr uint16_t BOND_DAILY_CAP = 30;
```

Once the day's bond is earned, further care still **works** — the bird is still
fed, still happier — but stops paying growth. There is no reason to keep tapping.

**A device whose reward curve goes flat is a device a child puts down.** That is
the intended outcome, and it is the exact opposite of a daily-login streak.

The same instinct runs through the two games, which are chosen against
left-or-right: both are games of **skill** a child visibly improves at, both are
deterministic, and neither has a random reward.

- **Echo** — the bird taps a rhythm and you tap it back. Reuses the Tin Can's
  reasoning about rhythm ([`knock_codec.h`](../../firmware/projects/canary-tincan/include/canary/tincan/knock_codec.h)):
  a human wrist cannot reproduce 3 ms of precision, so gaps are compared on a
  tolerance. Harder difficulties play the **same rhythms faster** rather than
  different ones, so a child who learned round three can still recognize it.
  Scoring is partial credit — there is no "you lost".
- **Steady** — hold your wrist level while the bird preens. The calmest possible
  use of a motion sensor, and deliberately the opposite of the shake-games this
  hardware invites: a device that teaches a child to whip their arm around ends
  up thrown across a room. It is also the one game that works with the glass
  nearly dark, which makes it the thing the Night Watch can offer at 3 a.m.
  without lighting a bedroom. A wobble **stops** the clock rather than resetting
  it, because resetting on a sneeze is how a calm game becomes a frustrating one.

And the session ends **because the bird decides it does**. Play costs `rest`,
`rest` recovers slowly, and a tired bird declines — the device says no in the
bird's own voice, so the child gets a story rather than a lockout. A companion
that pushes you away when it has had enough is the whole brief; `ended_by_bird`
is tracked separately from the hard ceiling precisely so only one of them gets
narrated that way.

### 4.5 Growth

Five stages, gated on **both** a minimum time at the previous stage **and** a
bond total — so growth can be neither rushed in an afternoon of button-mashing
nor missed by someone who visits twice a day.

`Hatchling` → `Fledgling` (day 1) → `Songbird` (day ~4) → `Companion` (day ~10)
→ `Elder` (day ~30)

`Elder` is not a countdown. It is the destination. There is no branch anywhere
that lowers `stage`, and a test walks a year of neglect to prove it.

---

## 5 · The voice: speaker, motor, and neither

[`haptic_voice.h`](../../firmware/projects/canary-companion/include/canary/companion/haptic_voice.h)
is built around a fact most firmware pretends away: **at authoring time we do not
know which output channels exist.** The motor is an add-on that may not be
fitted; `HAS_SPEAKER` is 1 only because the codec and PA-enable line exist, and
whether a transducer is actually populated varies by board revision.

So the runtime **probes**, and `voice_plan()` intersects what an utterance
*prefers* with what actually answered. The invariant: **a plan never names a
channel that is not there**, and something always happens — falling back to the
glass, the one output every board here is guaranteed to have. A fallback sets
`fell_back`, and the boot screen says it out loud: *"no motor fitted — nudges
will be seen and heard, not felt."* A device that claims it buzzed and did not is
exactly the surprise that leaves someone waiting for an answer that already
arrived.

Seven utterances, deliberately few. A device with forty distinct buzzes has
taught its owner nothing; a device with seven has taught them seven things.

### 5.1 Night silence

Night is sacred in this project, and a wrist device makes that **stricter**, not
looser — it is in contact with a sleeping person.

- **Nothing sounds at night. No exceptions, including a real fleet problem.**
- Haptics survive for exactly two utterances, both of which exist to reach a
  sleeping person on purpose: the wake alarm, and unacknowledged trouble.
- Whatever survives is **halved in strength** — a buzz calibrated for a wrist in
  a noisy kitchen is a startle against a sleeping one.
- And the **glass is subject to the same rule**. This is the line that is easy to
  get wrong: a "silent" flourish that still lights a bright panel beside a
  sleeping face is not silent. A courtesy utterance at night gets **no channel at
  all** (`test_night_silences_everything_but_the_two`).

One more, small: **a refusal never buzzes.** A "no thank you" that vibrates reads
as an error, and the bird declining seed is not an error.

---

## 6 · Before anyone calls this a kids' product

Unchanged from [the Tin Can's §3.3](canary_tincan_kids_watch.md), and it is still
the section that stops a launch. A bare Li-po strapped to a child is the largest
physical risk in the design and **no firmware decision touches it**. Before the
word "kids" appears on a store listing: CPSIA third-party testing and ASTM F963
(plus small parts, 16 CFR 1500.18); a child-resistant battery enclosure in the
spirit of Reese's Law; a **breakaway strap**; and FCC/CE for an intentional
radiator.

Until then this ships as a **maker kit for the builder's own household**.

The Pocket Canary adds one non-physical obligation worth writing down: everything
in §4.3 is a **design** refusal, not a certification. Nobody has run this in front
of an actual seven-year-old yet. The refusals are the right ones on the evidence
in §4.1, and they are testable, and they are not a substitute for watching a
child use it.

---

## 7 · Power

The Night Watch is a docked device and should be treated as one: charge it like a
toothbrush. The panel is off most of the time (that is the entire point of §2.1),
so the real budget is radio duty — a fleet subscriber that holds an MQTT
connection all night costs more than the glass does.

The Pocket Canary needs **no radio at all** in its base form. A pet that never
joins a network is a pet with a genuinely long battery life and an even shorter
list of things that can go wrong with it. The `Care::HouseholdTended` bridge is
the one optional exception, and it is optional.

Charging a Li-po against a wrist is the case to watch: **dock it, don't wear it
charging.**

---

## 8 · What exists, and what does not

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
  test_companion_cores.cpp    43 tests across 6 groups
```

```sh
cd firmware/projects/canary-companion/tests_host && make
```

**What does not exist yet:** no PlatformIO environment, no `src/`, no CO5300
display driver, no LVGL, no DRV2605L HAL, no MQTT glue, and **no factory image**
— which is why there is no entry in the flasher catalog. The board stays at
`compile-tested` tier.

That is deliberate, and it is the repo's rule rather than a shortfall: nothing is
claimed as working until it has been built and run. `gen_flash.py` verifies every
product's `env` and `board` against the firmware tree, so a catalog entry without
a runtime is not merely dishonest — it does not generate. The gate works.

### Next, in order

1. **Bring-up.** Confirm the touch controller really is an FT3168. Get LVGL onto
   the CO5300 at 410 × 502. Put a **DRV2605L + LRA** on the I²C port and bench
   LRA against ERM before anything is built on top — a mushy buzz is a dead
   product for both halves of this design.
2. **The Night Watch runtime.** The glass engine is done; what is missing is the
   panel driver, the RTC read, and the MQTT subscription that makes `trouble`
   real.
3. **The Pocket Canary runtime.** The bird renderer, and the NVS persistence for
   `PetState` (write-light: the day rollover is the only mandatory write).
4. **Then, and only then, the catalog.** A build env, a signed factory image, a
   `PRODUCTS` entry in `gen_flash.py` — and the Flasher on macOS and Linux lists
   it automatically, because it reads that one file.
