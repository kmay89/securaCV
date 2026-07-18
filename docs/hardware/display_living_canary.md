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

## Next pass (spec locked, not yet built)

The dash stage: perch-corner bird with speech-bubble slots and
look-at-the-troubled-device staging. Plus the update "reading" pose,
which waits on a visible witness-update signal reaching the display.
(Wave 2 delivered the rest: Searching/Calling postures, verified-pass
tilt, touch-startle, the morning greeting, and the trust-milestone song.)
