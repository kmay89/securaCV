# Room sensing — a calibration verdict, a reference mover, and the room graph (design)

**Status:** proposed. Nothing here is built. Five small additions to the
indoor sensing the fleet already ships, each placed on a seam that exists
today, and one path stated up front as the one we do not take.
**Owner:** firmware and hub maintainers.
**Related:** [`esp32_mesh_sensing_design.md`](../esp32_mesh_sensing_design.md)
(the 2-link gate and the empty-room baseline),
[`canary_sense_mr60bha2_design.md`](../canary_sense_mr60bha2_design.md) and
[`canary_sense_coarse_class_design.md`](../canary_sense_coarse_class_design.md)
(the radar and its unbuilt mover class),
[`canary_sentinel_fusion_design.md`](../canary_sentinel_fusion_design.md)
(independent-modality corroboration),
[`household_presence_and_posture.md`](household_presence_and_posture.md)
(the identity decisions this doc inherits),
[`BENCHMARKS.md`](../BENCHMARKS.md) (measure, never promise),
[`spec/canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md)
(Invariants A to F).

> One paragraph: a Canary Sense or a CSI node is honest about its own room
> and knows nothing about the rooms around it, nothing about whether it was
> mounted somewhere it cannot see from, and nothing about how well its
> presets fit the room it landed in. This doc proposes (1) a **calibration
> walk that ends in a verdict** on each node, shown in both flashers and in
> Home Assistant; (2) a **robot vacuum as the labeled reference mover** for
> the bench harness, so presence, range and the rigid-versus-limbed feature
> get ground truth without a person being tracked; (3) an operator-typed
> **room graph** the hub consults as a present-tense plausibility check,
> never as a path; (4) a **two-signal, one-shot latch** for the
> notification-only novelty path; and (5) **held-out reporting** as the rule
> for every calibration we ship. It also records (§2) why the obvious indoor
> design, following a resident's phone from room to room and acting on it, is
> not the one we build.

## 1. The gap

Today an indoor witness publishes coarse room words, `clear` / `present`,
`0` / `1` / `2+`, breathing locked or not
([`src/fleet_peers.rs`](../../src/fleet_peers.rs) keeps them per peer while
the peer is proven online). The CSI stack subtracts an empty-room baseline
(`meta.empty_room_baseline`) and needs two independent links to agree before
it says `confirmed` (`core.multilink_fusion`). The hub attention engine
([`src/relay/attention.rs`](../../src/relay/attention.rs), not yet wired)
earns `important` only from two modality classes agreeing. Each of these is
right as far as it goes, and three things are missing around them:

1. **No verdict on placement.** A Sense mounted behind a metal shelf, or a
   CSI node whose probe peer is through two walls, reports `clear` all day
   and nobody can tell that from an empty room. The empty-room baseline
   makes a quiet node quieter; it cannot say "this node is not seeing the
   room."
2. **No ground truth.** Rule 4 forbids a performance claim without a
   benchmark, and the radar's mover-class design has no labeled data to
   start from. The only labeled mover we have is a person walking, and a
   person is the one thing the fleet refuses to track.
3. **No relation between rooms.** Two witnesses `present` at once in rooms
   that do not touch, with the room between them `clear`, is either two
   occupants or one false trigger. The hub has the words to notice and no
   model to notice with.

## 2. The path we do not take

The design most indoor projects reach for first is the one the fleet has
already refused on the record: recognize a resident's phone by the identity
resolving key it hands to bonded peers, estimate its distance from each
scanner, fit those estimates to a floor plan, and let the result drive
things, a door lock above all. Every piece of that is settled here:

- The owner-phone identity store was dropped as a strengthening change
  ([`household_presence_and_posture.md`](household_presence_and_posture.md)
  D1), and the BLE Scout stays unwired (D3). A long-lived identifier that
  resolves any advertisement ever captured is the persistent identity
  Invariant A forbids, and a list that changes what the witness reports
  breaks Invariant F ("device lists are internal calibration only").
- Acting on presence is Invariant D: the Canary presents signals and has no
  actuator interface. "Unlock when the phone walks up" is the clearest case
  of the thing it must not do, and the known failure of such designs, a
  phone that drops off one radio for a minute while its owner sits in the
  room, is a door that opens by itself.
- "Where in the house is this person" is a movement history, named in the
  free-signals non-goals as a hard stop.

So nothing below identifies a device, and nothing below acts. The
techniques that survive the subtraction of identity are still worth having:
a fit with a quality verdict, a labeled non-human reference, a topology
check, a latch, and honest reporting.

## 3. P1 — a calibration walk that ends in a verdict

**What.** After install, the operator walks every room once with the Lab or
the flasher open. For each node the firmware exposes, for the length of the
walk only, the response it already computes and throws away: the radar's
presence word and range band per second, the CSI `wander` and `jitter`
metrics per window. The tool fits a per-node response model: how strongly
the node's output moves as the room's occupancy changes, over the walk.

**The verdict.** The one number that matters is the slope of that response.
A node whose output barely changes across a walk through its room is
misplaced (mounted next to metal, pointed at a wall, or the operator put it
in a different room than the one it is assigned to). A node whose response
is near full scale the whole time is saturated (too close to a fan, a
curtain, a fish tank). Both show as a plain verdict with a plain fix: *"This
Canary did not see you walk through the living room. Check what it is
mounted on and whether it is in the right room."* The same verdict becomes a
Home Assistant diagnostic `binary_sensor` (`problem`), like the existing
`radar_link` sensor.

**Where it lands.**

- Firmware: a `[calib]` stream on the attended serial console and the
  authenticated local API, open for the walk and closed after, in the mold of
  the Sense's `raw on` mode (attended cable only, never persisted, never on
  the network). The CSI side already has `csi_wander_jitter.h`.
- Both frontends (rule 7): the in-browser Lab (`canary-local/assets/`) and
  the desktop Flasher (`desktop/src/`) each get the walk screen and the
  verdict, from one shared pure fitting function with one test vector set,
  the way `desktop_parity.test.js` already pins the two provisioning forms
  equal.
- Home Assistant: one diagnostic entity per device, off until a walk has
  been run.

**What it never does.** The walk records response, never who walked. It
needs no phone, no beacon, no tag: the operator is "a mover". The fitted
numbers are per-node scalars (a slope, a scatter), kept in the node's NVS
beside its presets, and they move no event semantics (Invariant F): a bad
verdict is a diagnostic word, not a suppressed or an added event.

## 4. P2 — a robot vacuum as the labeled reference mover

**The idea.** A robot vacuum knows where it is, drives every square foot of
open floor, never gets bored, and is not a person. A hub that already runs
Home Assistant can read the robot's pose through the vacuum's own
integration. Recording that pose on the same clock as each indoor witness's
coarse output gives the bench harness a labeled mover for free:

| Ground truth from the robot | What it validates |
|---|---|
| In room *R* at time *t* | the room's `present` word; neighbors' `clear` words; the 2-link gate's agreement rate |
| Range from the Sense at *t* | the range band (`near` / `mid` / `far`) the radar reports |
| Rigid body, no limbs, constant speed | the "spectrally narrow, no cadence" negative class the coarse mover-class design needs (its §3 features); the vacuum is the cleanest *small rigid mover* example we will ever get |
| Known heading across a link | the multilink "which side of the room" estimate |

**Where it lands.** A row set in the benchmark harness, documented in
[`BENCHMARKS.md`](../BENCHMARKS.md), under its rule: the harness prints
agreement rates and errors to the terminal and **no number is committed**
anywhere, ever (`scripts/lint_bench_rows.py`). The robot's map frame is
not ours; a one-time similarity fit from four to six matched points (dock, a
corner, a doorway) maps robot millimeters onto the operator's room table,
and the fit is redone whenever the robot remaps. The vacuum's own radios are
invisible to the test: the fleet treats every RF source identically
(Invariant F), and a calibration-only list of "this is the reference" lives
in the harness, not on any device.

**A self-check for the recording.** During a run, a node whose output stops
correlating with the robot's pose for a few minutes is flagged, which is the
field version of P1's verdict and catches a node that died mid-run or a
robot that stopped moving under a chair. Thresholds should err toward
flagging too little, because every false flag stops a run.

**The honesty line.** The robot proves what the fleet does with a *rigid
small mover*. It does not stand in for a person walking, a dog, or two
people, and the harness row names what it measured. A result on the robot
is a result on the robot.

## 5. P3 — the room graph as a present-tense plausibility check

**What.** Beside the operator's zone table (the only place a zone has a
name), an adjacency list: which rooms share a doorway. Nothing more: no
geometry, no scanner coordinates, no grid.

**How the hub uses it.** As one more input to corroboration, never as
state:

- Two indoor witnesses `present` in non-adjacent rooms, with every room on
  any path between them `clear`, means *at least two* occupants or one
  false trigger; the engine treats the pair as uncorroborated (rule 6,
  `digest`) rather than as agreement.
- A `present` in a room whose every neighbor has been `clear` for the whole
  corroboration window, from a single weak channel, is the single-channel
  case already, and the graph adds nothing; it only ever withholds a
  promotion the engine would otherwise grant, and it never lowers a
  `critical` (rule 1) or touches the household posture (rule 2).

**The line it must not cross.** The graph is consulted against the fleet
summary's *current* per-room words, the ones [`fleet_peers.rs`](../../src/fleet_peers.rs)
already holds for 180 s, and against nothing older. Storing which rooms
were `present` in sequence, or inferring "went from the hall to the
bedroom", is a movement history, a named hard stop in the free-signals
spec, and the `Copy`-only, no-timestamp types of the attention engine are
the structural guard: an `AttentionInput` has no room to carry a path, and
the destructuring tests fail to compile if a field is added.

**Where it lives.** The graph is a map of where someone lives. It is hub
configuration, written `0600` like the peer summary, read by the engine
alone, carried in no event, never served on `/api/fleet` (which says
"never a zone"), never published to a broker, never synced to a phone. A
household can refuse to type one; the engine then decides as today.

**Why not a floor plan.** Half-foot grids, wall-crossing counts and distance
estimates buy a dot on a map, and a dot on a map is a person's position.
The adjacency list is the whole of the topology that a plausibility check
needs and the least that could be taken from a hub's disk.

## 6. P4 — a two-signal, one-shot latch for novelty

The notification-only novelty path (D2 in the posture record) wants to
quiet a *routine* arrival without ever dropping one. The pattern that keeps
such a quieting honest:

1. **Arm on two classes agreeing on absence.** The "empty" state that a
   later arrival is novel against is entered only when two independent
   modality classes (the engine's `CorroborationClasses`, where Wi-Fi and
   BLE from one phone are one class) both read the zone clear for a dwell.
   One radio losing one device for a minute, the classic failure, cannot
   arm it.
2. **One shot.** The latch quiets at most one arrival per emptying, then
   clears on that arrival. A second arrival in the same hour is judged
   fresh.
3. **Self-clear.** A latch left armed while two classes read the zone
   occupied for a long dwell is cleared on a timer, so a missed transition
   cannot hold a zone quiet.

This stays inside D2's conditions: RAM-only, it may move a radio-only
arrival between `digest` and `important`, it never drops a notification and
never changes an event. It is also a plainer mechanism than the salted
time-bucket filter `familiar.h` keeps today, and if D2b survives review, it
is the candidate to replace it.

## 7. P5 — held-out reporting as the rule for calibration

Any calibration we fit (P1's per-node slope, the Sense presets' band edges,
a CSI threshold) is reported the same way the benchmark harness reports:
fit on four fifths of the recorded spots, locate or classify the held-out
fifth, repeat five times, print the median and the 90th-percentile error to
the terminal, commit nothing. A fit that cannot report a held-out number is
not a fit; a held-out number copied into a doc is a claim. The harness and
`lint_bench_rows.py` already hold this line for the kernel; this extends it
to every calibration a Canary carries.

## 8. Already in the tree

So nobody re-proposes them:

- **Identify.** Sense publishes an `identify` button that flashes the LED
  white for ten seconds (`ha_discovery.cpp`); the wizard's "which device is
  which" moment is covered.
- **Empty-room baseline** (`meta.empty_room_baseline`): a 10-minute mean
  captured on pairing and in quiet hours. P1 is the verdict it lacks, not a
  replacement.
- **Two-link confirmation** (`core.multilink_fusion`) and the Sentinel's
  independent-modality scoring: the per-room corroboration P3 extends
  across rooms.
- **Household posture lapses** on a short TTL and falls to `away` on any
  doubt: the fail-safe P4's self-clear mirrors.

## 9. Order of work

| Step | Scope | Gates it trips |
|---|---|---|
| P5 then P1 | one pure fitting function (JS, shared by both flashers), the firmware `[calib]` stream, one HA diagnostic entity | `canary-local` page tests and the two dist gates; `desktop_parity`; `lint_fleet_word`, `lint_spelling`; both flashers (rule 7) |
| P2 | harness rows + the robot-frame fit script + `BENCHMARKS.md` section | `lint_bench_rows.py`; `cargo test` compiles the harness |
| P3 | after the attention engine is wired: the adjacency table, the plausibility input, fixture vectors | `lint_dictionary_sync.py` if any reason text is added; `spec/fixtures/attention_v1.json` replay |
| P4 | with D2b, if it survives review | `test_notify_familiar` (D2a's new test) |

None of these touches the chain, the dictionary's event vocabulary, Beacon,
Chirp or the raw-frame path. Every one of them is a diagnostic, a bench
fixture, or a reason to *withhold* a louder tier, which is the direction
the governance rules let us move without an RFC.
