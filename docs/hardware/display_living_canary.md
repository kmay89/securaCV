# The Living Canary — reading the system through the bird's feelings

Research base: the Flipper Zero dolphin (data-driven mood gates, weighted
idle pools, butthurt/level scalars), Tamagotchi's care loop (the user
"cares" for the bird by maintaining the security system), and Pwnagotchi's
honesty property — **every face is diagnostically truthful**: the cute
layer is a lossless compression of system state, so an experienced user
reads the face instead of the log.

## The engine (`include/canary/care/bird_mood.h` — pure, host-tested)

Two slow scalars, the Flipper pair pointed at system health instead of
petting:

- **anxiety 0–14** — floor derived from live state (+2 per stale witness,
  +4 per lost witness, +3 flapping hub, +2 links down, +1 per >12 h
  unacknowledged trouble). Rises instantly to the floor, decays one point
  per fully-quiet hour, snaps to zero on a fully-verified pass — and the
  floor always wins over a claimed verified pass (the engine doesn't
  trust its caller).
- **trust (days)** — consecutive fully-clean days, persisted in flash
  (write-light: only at the local-day rollover). One dirty day resets the
  streak; a week of clean days unlocks the rare idle flourishes, so a
  long-healthy system is *visibly* different from a day-one system.

## The face ladder (escalation by silhouette)

| Face | State | The bird |
|---|---|---|
| Calm | quiet, verified | breathes (1.4 s), blinks dithered, occasional flourish |
| Worried | anxiety 4–9, link trouble | eye narrowed, wing half-raised, scanning saccades, quicker blink |
| Searching | anxiety 4–9, a witness **late** | leans toward the edge it faces, eye held outward — looking FOR them; scans back over its shoulder, hops edgeward |
| Calling | anxiety 4–9, a witness **lost** | beak open, wing half-raised — calling out; restless fidgets |
| Distressed | anxiety 10–14 | fidgety wing, restless glances — maintenance overdue |
| Asleep | night | eye a line, beak tucked, 2.8 s breath, no blinks, no flourishes — **stillness is the information** |
| Hidden | live unacked alarm | full handoff: never cute during a real alarm; the bird's return IS the all-clear |

Searching and Calling are `bird_posture()` refinements of the Worried band
(pure, host-tested): the *cause* picks the story — lost outranks late,
and link trouble stays plain Worried because there is nobody specific to
look for.

## Reactions (one-shot, event-driven — wave 2)

Layered over the mood, then the pose settles back; each maps 1:1 to a
log-able event, and the mark refuses them while Hidden or Asleep (never
startle a sleeping bird):

| Reaction | Trigger | The bird |
|---|---|---|
| Tilt | a fully-verified pass just completed | curious head-cock: beak dips, eye rises (700 ms) |
| Startle | the glass touched while the bird is on stage | quick hop, momentarily wide eye |
| Greeting | first wake of the local day | one slow, high wing stretch |
| Joyful | trust milestone: first clean **week** / **month** | the song — hop with a long double ruffle; deferred past midnight so it plays in the morning, and dropped outright if an alarm owns the stage |

Tilt and Startle share an 8-second ration; Greeting and Joyful are rare
by construction and skip it.

## Idle flourishes (the Flipper manifest, miniaturized)

Every 25–60 s (jittered — never metronomic), one flourish by weight:
glance aside (saccade + return), preen (wing lift ×2), a small hop — and
at **trust ≥ 7 days**, the rare hop-and-ruffle. Worried mode restricts
the pool to scanning; Distressed to fidgets; Asleep plays nothing.

## Where the bird may perch (calm-tech placement)

- **Watch halo:** the engine owns the perch (wave 2). The bird stays on
  stage through Warn-band trouble — Searching above a "quiet too long"
  hero reads as *the bird looking for the late bird* — and the engine
  still pulls it entirely during a live unacked alarm. Only the
  no-clock-with-witnesses fallback remains a UI decision.
- **Dash:** the empty-nest face only, for now; with witnesses the cards
  own the wall. (The spec's perch-corner bird with bubble slots and
  look-at-the-troubled-device staging is the next pass.)

## Rules that hold

1. The bird is a gauge, not a toy — every pose names a log-able state.
2. Slow idle, sharp reactions; motion stays in the user's periphery.
3. Night is sacred: no flourishes, no sound, breath only.
4. Rarity is the reward: trust gates the specials, and trust is earned by
   security hygiene, not by grinding.
5. Exit gracefully under fire: alarms get instrument-grade UI, bird-free.

## The first meeting (splash)

The first boot ever is a meeting, not a logo. Presence before speech: an
empty beat, the hop in, a settle, the head-tilt that says *it sees you*
— and only then the speech bubble, typed one character at a time with a
breath at punctuation:

> Oh! Hello.
> I'm your canary.
> Want to know what I worked out about you?
> *(a held, narrowed beat)*
> Nothing.
> No MAC, no IP, no idea who's on your wifi.
> I could have. Everything else in this house did.
> Not that kind of bird. Call me 3f7a9e21b4c5d6e8.
> Dark means all is well. If I glow, look at me.
> Add another of me and we compare notes.
> What I see stays here. That's a promise, not a setting.

(The name in the walk-back is the device's own salted pseudonym — see
`common/identity/device_pseudonym.h`.) The privacy promise is spoken by
the bird itself, because that is who keeps it. The wordmark enters only after the last line — the name lands
AFTER the friend does. A tap always advances (first tap completes the
line, next tap moves on): the user outranks the storyboard. The meeting
is remembered in NVS (`scv-hello`), and every later boot plays the short
familiar splash instead, with the tagline swapped for a quiet
"hello again" — you two have already met, and a returning boot must
never feel slower than home.

## The performance engine (`firmware/common/story/` — pure, host-tested)

The meeting above used to be a hard-coded beat array inside `splash.cpp`. It is
a **Scene** now, played by a shared engine, and that change is worth more than
the extra lines it bought.

A `Scene` is a list of `Beat`s; a `Beat` is one moment — a **Pose** for the
bird, a **Gesture** for the light, a **Tone**, and optionally a line of copy
that types on. (`Pose` and the line are dispatched today; `Gesture` and `Tone`
are authored in the scripts but not yet consumed by any renderer — see the
status notes below.) `StoryTeller` walks them and answers one question per frame:
what should be on stage right now. It knows nothing about LVGL, Arduino or any
particular glass, because the point is that the same performance runs on the
watch, the dash, the 1.47" stick, the 7" bedside glass and — as the engine is
adopted — the phone and the wrist. **A personality that is re-typed per surface
is four personalities.**

Keeping the script as *data* (`story_scripts.h`) is the other half: the writing
can be edited by someone who is not editing a renderer, the timing is visible in
one place, and the whole arc is host-testable — which is the only way a joke
gets to be the same joke twice.

### The meeting, grown up

`kHello` is the arc, and the shape is deliberate:

- **Presence, then speech.** Three wordless beats — arrive, settle, look at
  you — before a character is typed. It costs a second and a half and it is the
  most important thing in the scene.
- **The setup.** It leans in and offers to tell you what it worked out about
  you. The light pulls in and dims: the held breath. Every instinct built by
  every other device in this category says the next line is going to be
  impressive and faintly threatening.
- **The punchline is "Nothing."** — one word, on its own beat. The itemized
  nothing is one line (no MAC, no IP, no idea who's on your wifi) and the flex
  is one more: *it could have — everything else in this house did.*
- **The walk-back.** One warm line doing double duty: the joke's button, and
  the handover of the only name it has — a pseudonym it made up itself.
- **Then the actual job** — the color language in a single line ("dark means
  all is well; if I glow, look at me") and the fleet in one more — the part a
  user needs to retain, landing right after the laugh.
- **The promise last**, in the bird's own voice, because it is the bird's to
  keep.

The meeting says each of those things exactly once — ten spoken lines, and
every one of them is either the chuckle or something the user needs (a tap
still completes the line, and the next tap moves on). A first meeting is
charming in inverse proportion to how long it holds the stage.

**The joke is only funny because it is true.** `common/identity/device_pseudonym.h`
reads no hardware MAC by construction — the `esp_mac.h` include is deliberately
absent — so the bird is describing its own implementation. If that ever stopped
being true the joke would have to go, which is exactly the right incentive.

### Idle vignettes (the Flipper property — written, not yet wired)

A device that is plugged in and idle should occasionally be *caught doing
something*. `care/ambient_life.h` already rationed the moments; it had nothing
to release. The scenes now exist — one- and two-beat scenes (a preen, a glance aside, a
stretch, "Checking on the others", "All quiet. Carry on.") weighted so **roughly
two thirds are wordless**. A device that speaks every time you look at it stops
being company and starts being a chatbot on your wall. The rare one is gated on
trust ≥ 7 days, like every other special here.

**Status:** these are data and tests, not yet behavior. `main.cpp` still
handles an `AmbientLife` moment by calling `canary_mark_react()` / the glance
helper directly; nothing calls `story_pick_idle()` or holds a steady-state
teller. Wiring that dispatch — and `kAlertApproach` on the attention edge —
is the next slice.

### Alerts: character in the approach, never on the alarm

This is the one place personality and honesty could genuinely fight, so the
separation is exact. **Rule 5 above is not negotiable** — the bird does not
stand in front of a live alert making it charming, and no scripted line is ever
shown on an alarm screen.

What has character is the **approach**: the second between "the glass is calm"
and "the glass is an instrument". Cutting straight from a warm idle lamp to a
red panel is a jump scare, and a jump scare is a device people unplug. So
`kAlertApproach` gives the bird one beat to go upright and still, the light one
beat to pull in — and then the bird **exits** and the truth owns the stage. Two
beats, no words, and a departure. The words on an alarm belong to the UI, which
knows what is actually wrong.

**Status:** `kAlertApproach` is written and host-tested (including that it can
never be tapped past and that it still runs at night), but nothing plays it on
the attention edge yet. Same slice as the idle dispatch above.

### What the engine enforces

1. **The truth preempts the story.** `interrupt()` drops a scene instantly and
   it does **not** resume — a performance that picked up where it left off after
   an alarm would be saying the alarm was an interruption to the charm rather
   than the other way round.
2. **Ambient only, never diagnosis.** The same line `ui/character.h`'s Voice
   draws. The host tests assert this structurally: a banned-word list fails the
   build if a script line ever picks up diagnostic vocabulary.
3. **The user outranks the storyboard.** First tap completes the line, next tap
   moves on, and the tests prove a determined tapper is out in seconds.
4. **Night is sacred.** Every idle vignette is `day_only`; the meeting and the
   alert approach are the two that still run at 3 a.m., because both are things
   the user is present for.

The writing rules are tested too — plain ASCII (the Montserrat tables carry no
emoji), short enough to read at three meters, and the comic timing itself is
asserted, because here timing is data: the setup must be sly, the light must
narrow for it, a silent beat must hold, and the punchline must land alone.

## Next pass (spec locked, not yet built)

The dash stage: perch-corner bird with speech-bubble slots and
look-at-the-troubled-device staging. Plus the update "reading" pose,
which waits on a visible witness-update signal reaching the display.
(Wave 2 delivered the rest: Searching/Calling postures, verified-pass
tilt, touch-startle, the morning greeting, and the trust-milestone song.)
