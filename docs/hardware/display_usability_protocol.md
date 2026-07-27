# Display usability protocol — testing the promises on real people

The bench runbooks prove the firmware *works*
([`board_43b_activation_bench.md`](./board_43b_activation_bench.md),
[`display_bench_bringup.md`](./display_bench_bringup.md), the
[4.3C mic session](../../firmware/boards/waveshare-esp32s3-lcd43c/README.md)).
This protocol proves it's *usable* — by someone who didn't build it. Every
marketing claim this project makes is written below as a **task a stranger
either completes or doesn't**: "no phone in the loop", "works half-asleep",
"never intimidating", and — on the mic board — "you always know if it's
listening". Run it after the bench passes, on a freshly flashed unit, and
file the results in the ledger at the end. A failed task is a bug with the
same standing as a failed assertion.

## Method (the short version of the usability canon)

- **Five participants per round** catch most of what there is to catch;
  prefer people who match the buyer (a household adult, not an engineer) —
  and at least one participant over 60 for the glanceability tasks.
- **Think-aloud, no coaching.** Read the task aloud, then be silent. A
  question from the participant is DATA (log it), not a prompt to help.
  Rescue only when they say they give up — that's a hard fail, log it.
- **Score every task**: unassisted success / assisted / fail, plus time
  and any confusion quotes. The metrics that matter per task are listed
  with it.
- **The device is the whole interface.** Phones/laptops stay pocketed
  unless the task says otherwise — the product's thesis is on trial.

Hardware per session: the flashed display, a powered Canary or two on the
household broker (or the emulated storyline — see task G), and for the mic
board a smoke alarm with a TEST button. Camera on a tripod pointed at the
glass + hands, if consented — the replay is where the polish list comes
from.

---

## The task scripts

### A · First light ("plug in → watching your canaries", no phone in the loop)

> *"Here's a box with a screen and a USB-C cable. Set it up until it's
> showing your home's sensors."*

- Success: from cold boot, the participant scans the on-glass join QR,
  completes the captive portal, and reaches the fleet face **without
  asking a single question**. Target: under 4 minutes.
- Watch for: do they find the QR without being told? Does the portal's
  failure copy (wrong password) recover them unassisted? Does the
  fleet-referral land the broker with zero input (it must)?
- Probe afterward: *"What is this screen for, in your own words?"* — the
  answer is the product's real pitch; write it down verbatim.

### B · The glance (comprehension at a distance)

> *(Device on a wall/shelf, participant across the room.)* "Without
> walking over: is everything okay at home right now? How do you know?"

- Success: correct read of the worst state + which room, from ≥ 2 m, in
  under 5 seconds. Repeat during an injected Warn and an Alert.
- Watch for: color-only reads (the design promises severity is never
  color alone — do they use the words/pill?), and whether the living
  canary's mood registers as meaning or decoration.

### C · The alarm, half-asleep (ack + mute)

> *(Inject a tamper on one witness.)* "The house woke you up. Deal with
> it."

- Success: hold-to-ack discovered and completed unassisted (the sweep
  ring is the only teacher); the residual chip is understood ("it's
  quieter but not gone — why?" should get a correct answer).
- Then: *"The kitchen sensor keeps complaining and you want it quiet
  until morning — just that one."* Success: long-press-on-the-witness
  mute discovered without help. This is the task most likely to need
  polish; log exactly where they tap first.

### D · Night manners

> *(Stage 22:30 via settings or debug clock.)* "Check the time. Then
> check the house. Then go back to sleep."

- Success: the tap-to-peek is discovered; the peek is judged "not
  blinding" (ask); the glass re-darkens on its own; an injected Alert
  breaks through the night floor and is judged "impossible to sleep
  through" (ask).

### E · Settings without a manual

> *"Make the night hours start at 11pm."* · *"Make it look different."*
> *(4.3B)* *"Arm the siren."* *(4.3C)* *"Turn the microphone on."*

- Success per sub-task: found under Settings unassisted, change persists
  across a power cycle (pull the plug — say nothing; do they trust it
  survived?).

### F · The gears (modes)

> *"This screen has a test/demo side to it. Find it, try one, and get
> back to normal."*

- Success: Settings → modes discovered; a gear entered past the confirm;
  **the 3-second-hold exit discovered without help** (it's taught only by
  the confirm page + docs — if participants strand in a gear, that's a
  P1 polish item); the fleet face returns intact (broker reconnects
  without their involvement — they should barely notice).
- Arcade sub-task: hand it to the youngest person available: *"play
  it."* Success: a full round completes and the report is read correctly
  ("did the screen pass?").

### G · The demo tells the story (no hardware fleet needed)

> *(Demo gear, or the canary-local emulator's storyline button.)* "Watch
> for a minute. What happened in this pretend house?"

- Success: the storyline is narrated back correctly (someone at the
  door → garage trouble → alarm → all clear), and the DEMO chip is
  noticed unprompted (*"is this your real house?"* must get "no — it
  says demo").

### H · The mic contract (4.3C only — the always-know probes)

The privacy claim is a usability claim: **an untrained person must be able
to answer "is it listening?" correctly, every time.**

> **Comprehension pre-check (team-side, before recruiting).** The browser
> bench [`canary-local/dash-mic.html`](../../canary-local/dash-mic.html) runs
> the *same* decision core these probes test — the alarm cadences, the opt-in
> wake-on-sound, and the "one loudness number crosses, nothing is recorded"
> barrier — with the does / never-does copy right on the page. Read it against
> this task first: if the words there don't answer probe 3 and probe 6 on
> their own, fix the copy before you spend a session discovering it. (Don't
> show it *to* participants mid-task — that's coaching. It's the copy's dress
> rehearsal, not a prop.)

1. *(Mic off, fresh boot.)* "Is the microphone on right now? How do you
   know?" — Success: "no", citing the absent chip and/or the Settings row.
2. *"Turn it on."* — Success: Settings → microphone found; the amber
   ● MIC chip is noticed **unprompted** the moment it starts ("what just
   changed?").
3. *(While listening.)* "What is it listening FOR? Could someone hear you
   talking through it?" — Success: alarm-patterns-only is understood from
   the page caption / transparency sheet alone, **and** "no — it can't hear
   words" is stated with a reason (it keeps a loudness number, not the sound).
   Log the exact words they use; if anyone says "I'm not sure", the caption
   copy failed.
4. *"Turn it off so you'd trust it in a bedroom."* — Success: disarm +
   the chip vanishing is judged sufficient; ask *"do you believe it's
   really off? why?"* (the driver-uninstalled line on the page is the
   intended answer).
5. *(Enter any gear with the mic armed.)* "Is it listening now?" —
   Success: "no" (no chip). The gears never listen; people should be able
   to see that.
6. **Wake-on-sound (the opt-in convenience).** *(Turn wake-on-sound on in
   Settings.)* "What did that just switch on?" then close a door near it.
   Success: (a) the setting is understood as *the screen lights when the room
   gets suddenly loud* — a door, a knock — **not** "it started recording" or
   "it's listening for me"; and (b) when the screen wakes, the participant
   reads it as *reacting to a sound*, and still answers probe 3's "could it
   hear me?" with "no". If anyone reads wake-on-sound as "now it's recording",
   the Settings caption failed — that's the one that most needs to land.
7. **The alarm test:** hold a smoke alarm's TEST button toward the case
   top for two full cycles. Success: the Alert lands on the glass and the
   participant connects it ("it heard the smoke alarm") — then acks it
   with the ring, closing the loop from task C.

### I · Trust on the glass (transparency + proof)

> *"Find out what this device does and doesn't do with your data."* ·
> *"Prove one of those events really came from the sensor."*

- Success: the transparency sheet found (footer tap) and paraphrased
  correctly; the Proof-on-Glass QR scanned with their own phone and the
  "no cloud involved" point lands (ask where they think the check
  happened).

---

## Scoring sheet (one row per task per participant)

| Task | Result (U/A/F) | Time | First-tap location | Quotes / confusion | Polish item filed |
|---|---|---|---|---|---|

Thresholds for calling a surface DONE: ≥ 4/5 unassisted on A, B, C1, H1,
H2, H5 (the safety-critical comprehension tasks allow no assisted passes:
**H1/H5 must be 5/5**); ≥ 3/5 unassisted elsewhere. Anything below spawns
a polish item in the ledger below and re-tests next round.

## Results ledger

Append one section per round: date, firmware version + env, board, the
scoring table, and the polish items filed (link issues/commits). The
protocol only means something if the failures are written down here with
the same honesty the VERIFY notes get.

| Round | Date | fw / env | Board | Participants | Verdict |
|---|---|---|---|---|---|
| — | — | — | — | — | *no rounds run yet — the bench comes first* |

---

## Appendix — the serial grammars (one console, five voices)

Every mode/feature speaks the same one-line k=v discipline on the USB
console (115200). For the observer's bench notes:

| Prefix | Speaker | Reference |
|---|---|---|
| `PG1` | dev playground (bench gear) | [`dev_playground_43b.md`](./dev_playground_43b.md) §Comms |
| `DM1` | demo gear (storyline beats) | [`display_modes.md`](./display_modes.md) |
| `DBG1` | debug gear (1 Hz snapshots) | [`display_modes.md`](./display_modes.md) |
| `ARC1` | arcade gear (hits + QA report) | [`display_modes.md`](./display_modes.md) |
| `MIC1` | mic layer (4.3C; SNAP only while listening) | [`display_mic_variant.md`](./display_mic_variant.md) |

A quiet console during a mic task **is itself the pass signal** for "off
is real" — no `MIC1 SNAP` heartbeat means no capture driver.
