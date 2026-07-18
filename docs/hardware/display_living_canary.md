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
| Worried | anxiety 4–9 | eye narrowed, wing half-raised, scanning saccades, quicker blink |
| Distressed | anxiety 10–14 | fidgety wing, restless glances — maintenance overdue |
| Asleep | night | eye a line, beak tucked, 2.8 s breath, no blinks, no flourishes — **stillness is the information** |
| Hidden | live unacked alarm | full handoff: never cute during a real alarm; the bird's return IS the all-clear |

## Idle flourishes (the Flipper manifest, miniaturized)

Every 25–60 s (jittered — never metronomic), one flourish by weight:
glance aside (saccade + return), preen (wing lift ×2), a small hop — and
at **trust ≥ 7 days**, the rare hop-and-ruffle. Worried mode restricts
the pool to scanning; Distressed to fidgets; Asleep plays nothing.

## Where the bird may perch (calm-tech placement)

- **Watch halo:** above the hero whenever the clock (or the empty-nest
  invitation) owns the stage — including asleep on the night clock. A
  status-word hero means something needs the room: the stage goes
  bird-free even before the alarm handoff.
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

Reaction micro-events (person-verified head-tilt, update "reading" pose,
touch-startle), the SEARCHING hop-to-the-edge and CALLING postures, dash
perch + speech bubbles + device-map staging, GREETING morning stretch,
and the JOYFUL trust-milestone one-shot.
