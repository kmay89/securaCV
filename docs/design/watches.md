# Watches — attention that expires on purpose

> **Status: designed · pure engine built and host-tested · hub wiring is
> the next step.** The decision core (`custom_components/securacv/watches.py`)
> is pure and covered by `tests/test_watches.py`; what remains is binding a
> watch to live sensor updates and the notification lane. Marked honestly
> here rather than in a roadmap nobody reads.

## The problem, stated properly

Every alerting tool — Home Assistant automations, IFTTT, the app that came
with your doorbell — assumes **attention is permanent**. You define a rule,
it runs until you delete it. That assumption is wrong about people, and it
is the reason most of this stuff never gets built:

- **The concern is usually temporary.** The cat had surgery on Tuesday, so
  for two weeks you want to know about litter box behavior. In March you
  don't care. Nobody wants a permanent litter box rule.
- **It's August, so you're watching soil moisture.** In January that
  sensor is under snow and the alert is noise.
- **Setup cost is paid once; teardown cost is paid never.** Deleting a
  stale automation is a chore with no reward, so people don't. Six months
  later the system cries wolf, and they stop trusting it or turn it off.

So the cost of a two-week question is: pick a trigger, pick a threshold you
have no way to know, pick a notification action, test it, tune it because
the threshold was wrong, and then remember to dismantle it. Nobody does
that for a cat. **The feature isn't the alert. The feature is that caring
about something for two weeks costs one sentence and cleans up after
itself.**

## The idea: a watch

A **watch** is a *bounded period of attention* on something the fleet
already senses. Three properties define it, and all three are the point:

1. **It expires by itself.** Every watch has an end date — not optionally,
   structurally. Teardown is the default, not a chore. This is the whole
   design in one line.
2. **It learns what normal is; you never pick a number.** A watch spends
   its first stretch *settling in* — just observing — and then reports
   deviation from what it actually saw. You are never asked "alert above
   how many?" because you don't know, and neither does the vendor.
3. **It's created in one breath.** "Keep an eye on the litter box for two
   weeks." Not a trigger, a condition, and an action.

> **Naming note.** A **watch** (lowercase, with an article: *a watch*, *the
> litter box watch*) is this bounded attention span. **The Night Watch**
> (capitalized, definite) remains the bedside clock product in
> [`docs/GLOSSARY.md`](../GLOSSARY.md). The words are cousins on purpose —
> both mean "keeping an eye out for a while" — but only one is a product.

## What you say, and what it does

| You say | What gets created |
|---|---|
| "Keep an eye on the litter box for two weeks" | subject: the litter box Canary · concern: `unusual` · ends in 14 days |
| "Watch the soil moisture until October" | subject: that sensor · concern: `less` · ends Oct 1 |
| "Tell me if the back door stops reporting this week" | subject: back door · concern: `stopped` · ends in 7 days |
| "What am I watching?" | the roster, with days remaining |

Then, later, without you doing anything:

> *"The litter box watch ends today. Fourteen days, visits held steady at
> about three a day, nothing unusual. Want me to keep going, or let it go?"*

Silence lets it go. **That sentence is the product.**

## The five concerns — how people actually think

Thresholds are a programmer's mental model. People think in *worries*. The
whole vocabulary is five words, and every watch picks exactly one:

| Concern | The human sentence | What it does |
|---|---|---|
| `stopped` | "tell me if it stops" | fires on absence — no observations for notably longer than the baseline gap |
| `unusual` | "tell me if it changes" | fires on deviation in either direction |
| `more` | "tell me if it goes up" | fires on deviation upward only |
| `less` | "tell me if it drops" | fires on deviation downward only |
| `every` | "tell me every time" | a plain relay, no baseline needed |

The cat case is `unusual` (both directions matter: straining *and* not
going). Soil moisture is `less`. An elderly parent's kettle is `stopped`.
That's the whole model, and it fits on a card.

## Sensitivity is three words, never a number

`gentle` · `normal` · `keen`. Internally these are multipliers on the
baseline's own spread; externally there is no number to get wrong, and
"turn it down a bit" is a thing a person can say. A watch that cries wolf
gets one adjustment, not a tuning session.

## The settling-in period — why there is no threshold to type

A new watch does not alert immediately. It observes for a **settling-in
window** (default: the first quarter of the watch, floor of one day) and
builds a baseline from what it actually saw — **median and median absolute
deviation**, chosen because they survive a weird day without being dragged
by it, and because they work on the handful of observations a two-week
watch actually has.

While settling, the watch says so out loud if asked. It will not alert on
data it hasn't earned an opinion about — a watch with too few observations
reports *"still getting a feel for normal"* rather than guessing. **A
confident alert from an ignorant baseline is worse than no alert**, which
is the same reasoning behind the alarm detector's two-cycle rule.

## What a watch cannot do — the honest boundaries

A watch is **attention, not capture**. It cannot:

- **Turn on any new sensing.** A watch attends to signals the fleet
  already produces. It never enables a camera, never widens what is
  recorded, never adds a capability. If nothing senses the litter box, the
  answer is "I can't see that" — not a prompt to install a camera.
- **Reach backward.** A watch begins when you start it. It cannot be
  applied to data already recorded — that is **Invariant VI** (no
  retroactive capability expansion), and here the invariant is doing
  exactly the work it was written for: you cannot decide today to have
  been watching last month.
- **Change the security posture.** Watches don't arm, disarm, mute, or
  unseal anything. They change *what you are told*, never *what is kept*.
- **Outlive its expiry silently.** When a watch ends it says so. Silence
  is never rendered as safety — the same rule the Night Watch's blackout
  obeys.

## Voice: which half is safe to speak

The voice contract ([`whisper_local_voice.md`](../research/whisper_local_voice.md) §3.1)
says voice may query and nudge but may not change the security posture. A
watch is not security posture, so watches are the first thing voice is
allowed to *create* — but the split is asymmetric, and deliberately:

- **Starting a watch: allowed by voice.** It only ever *adds* attention.
  The worst a stray sentence from a television can do is tell you more
  than you wanted for a while, and the watch announces itself and expires
  on its own. That failure direction is safe.
- **Ending a watch early: not allowed by voice.** That *removes*
  attention, which is the silencing direction — the same reason voice
  cannot mute an Alert. Ending early happens on an authenticated surface.
  Expiry is automatic, so this is rarely needed anyway.

The rule underneath, worth stating once because it generalizes: **voice may
make you better informed, never less.**

## Data model

Small on purpose — the whole thing is one dict per watch:

```
id            stable identifier
label         what the person called it ("the litter box")
subject       {kind: event|numeric, ref: device_id or entity_id}
concern       stopped | unusual | more | less | every
sensitivity   gentle | normal | keen
started_at    when attention began (never before this — Invariant VI)
ends_at       required; there is no "forever"
settle_until  baseline window end
observations  [(timestamp, value)] — bounded ring, coarse
state         settling | watching | ended
fired         count of times it spoke, so an end-of-watch summary is honest
```

## Recipes — because a blank page is also config

A blank "create a watch" form is its own kind of burden. The recipes are
shaped around the situations people are actually in, and each one is a
complete watch with a sensible expiry:

| Recipe | Shape |
|---|---|
| **Recovery** — after a vet visit or a hospital discharge | `unusual`, gentle, 14 days |
| **Growing season** — soil, greenhouse, water | `less`, normal, until a date you pick |
| **Away** — a trip | `every`, normal, until you're back |
| **Settling in** — a new pet, a new baby monitor, a new device | `unusual`, gentle, 30 days |
| **Is this thing on?** — a device you don't trust yet | `stopped`, keen, 7 days |

A recipe is a starting point, not a rail — every field stays editable, and
a custom watch is the same object with nothing prefilled. **Flexibility is
preserved by making the model tiny, not by making the form big.**

## Why this fits SecuraCV specifically

Any home automation platform could build watches. This one has three
reasons to build them well:

1. **The invariants already say attention must be intentional.** Invariant
   VI forbids retroactive reprocessing; watches are its friendly face —
   you decide *now*, forward only.
2. **The vocabulary discipline is already built.** The spoken answers
   never overclaim; a watch summary inherits that (it says "still getting a
   feel for normal" instead of inventing confidence).
3. **The fleet already emits semantic events, not footage.** A watch on
   "litter box visits" is counting coarse events — it never needed video,
   which is exactly the thesis.

## What's built, and what's next

| Piece | Where | Status |
|---|---|---|
| Model, baseline, deviation, expiry, phrasing | `custom_components/securacv/watches.py` | **built, host-tested** |
| Duration parsing ("two weeks", "until October") | same | **built, host-tested** |
| Voice: start a watch, list watches | `intent.py` + sentences | **built** |
| Persistence across restarts | HA `Store` | next |
| Binding a watch to live sensor updates + notification lane | integration glue | next |
| Recipes in the UI, end-of-watch summary card | Lovelace | after that |

## Related

- [Talking to your fleet](../voice_control.md) — the voice surface
- [Whisper local voice](../research/whisper_local_voice.md) §3.1 — the contract this extends
- [Alert relay design](alert_relay.md) — how a fired watch reaches you
- [`spec/invariants.md`](../../spec/invariants.md) — Invariant VI, the reason watches only look forward
