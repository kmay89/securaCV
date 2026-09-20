# The BUSY Bar surface — witness state on someone else's glass (design)

**Status:** the decision layer, the classing gate, the state machine and the
protocol mapping are **built** — the `busybar_surface` bin behind the
`surface-busybar` Cargo feature, with the pure core in
[`src/surface/busybar/`](../../src/surface/busybar/) and 68 unit tests plus six
source-level invariant tests
([`tests/busybar_surface_invariants.rs`](../../tests/busybar_surface_invariants.rs)).

**Nothing here has been run against a real BUSY Bar.** The protocol shapes
come from the firmware's own OpenAPI specification and the two official client
libraries, which is good evidence and not the same thing as a device on a
desk. **Bench validation is pending** and §9 is the list of what a bench
session has to settle. The physical controls are **designed and not
implemented**, for reasons given in §6; the control lane that works today is
MQTT.

Related: [the alert relay](alert_relay.md) (the other SecuraCV lane that draws
on this hardware), [the Busy Bar alert recipe](../integrations/busy-bar.md)
(that lane's worked setup), [Canary Cards](../standard/CANARY_CARDS.md) (the
schema this is a projection of), and [the Open Ambient Security Display
Standard](../standard/AMBIENT_DISPLAY_STANDARD.md) (the behavior contract
every SecuraCV surface answers to).

---

## 1 · What this is, and what it is not

A [BUSY Bar](https://busy.app) is a desk or door object from Flipper Devices
with a 72×16 RGB LED matrix on the front, a 160×80 monochrome display on the
back, a center button, a rotary dial, a mode switch, a top lever, and an open
local HTTP API. This is a SecuraCV **display surface** driving one from the
hub.

It is **not a Canary**. Canaries are SecuraCV's own witnessing devices; this
is third-party hardware rendering what the Canaries already witnessed. It gets
an honest name — the BUSY Bar surface, `busybar` in code — and BUSY Bar is
referred to as what it is, someone else's product ([`TRADEMARK.md`](../../TRADEMARK.md)
§1, truthful compatibility statements). It is not called "Canary Bar" and it
never will be.

The goal is not a status light. It is the calmest, legible physical readout of
witness state we can build: something that lets a person walking past the
front door understand *what the house knows*, without turning the hallway into
a surveillance console.

## 2 · The core idea: two displays, two audiences, one classing

**The device's two displays map onto the Canary Cards privacy classing.**

| display | audience | admits |
|---|---|---|
| front 72×16 RGB matrix | the room — anyone at the door | public phrases only |
| rear 160×80 mono | the operator, at arm's length | P0 detail, P1 by opt-in, never P2 |

Both are derived from the same card set the Canary already announces to Home
Assistant, through one classing gate
([`card.rs`](../../src/surface/busybar/card.rs)). There is no second
formatting path: the ingest builds the same cards, with the same ids and the
same privacy classes, that `canary-local/assets/canary-cards.js` builds for
the Sense Lab and that `fleet_cards.h` builds for the wall glass — and a test
pins ours to the reference renderer, because a drift in *classing* is a
privacy bug, not a cosmetic one.

Four rules, each enforced in code and each covered by a test:

1. **A card that cannot be classed is not rendered. Ever.** The schema makes
   `privacy` optional for forward compatibility. A surface rendering witness
   state does not inherit that latitude — unknown class fails closed, on both
   displays.
2. **P2 never renders anywhere.** P2 means the value never leaves the device
   and is seen only as its coarse derivative. A surface that rendered it would
   be the leak the class exists to name.
3. **P1 is operator-only and opt-in.** Wellbeing numerics are a person's
   vitals. Refused on the front unconditionally, and refused on the rear
   unless the operator turned them on — and turning them on **cannot** move
   them to the front.
4. **Only coarse kinds can become a public phrase.** `stat` and `sparkline`
   are numbers, and a number on a room-facing display is a reading about
   whoever is in the room. So `illuminance` and `frame_errors` are ordinary P0
   cards that reach the rear and stop there.

## 3 · The hard constraints

### 3.1 No control on this device may affect witnessing

Not the button, not the dial, not the switch, not the lever. They change what
the **surface shows**. The kernel keeps sealing, the log keeps growing, the
Canaries keep reporting.

A physical mute on a desk accessory that quiets a witness system is the
precise failure this project exists to prevent: it converts "the house is
witnessing" into "the house is witnessing unless someone reached over and
stopped it," and the second sentence is worth nothing in the moment it
matters. Guarantees here are `can't`, not `won't`, so the property is built
three ways:

- **The effect vocabulary is drawing.** `controls::Effect` has two variants,
  both about pixels. No `Publish`, no `Append`, no `Ack`, no `Suppress`. A
  handler cannot ask for anything else because there is nothing to ask for.
- **Witness state arrives immutably.** `controls::apply` takes `&WitnessView`,
  never `&mut`.
- **The surface has no write path.** The broker handle is `SubscribeOnly`,
  whose inner client is private; the kernel is reached with `GET` only. A
  source-level test greps for a publish that reached around it.

**The cost, named out loud:** you cannot acknowledge the fleet from the bar.
The center button clears the card *from this glass* and deliberately does not
publish to the household's shared `securacv/fleet/ack`. An ack made in Home
Assistant or on the wall glass still clears here, because the ingest
subscribes that topic — the flow is one-way. That is the price of having no
write path at all, and for a witness system it is the right price.

`POST /verify` is also not called. It does not alter the log, but it makes the
kernel work, and a dial detent must not be able to schedule kernel work.

### 3.2 No PII on the wire or on the glass — structurally

The front matrix is 72 pixels wide. A designer under that pressure
abbreviates, and abbreviation slides toward identity. The worked example is
`K. AT DOOR`: shorter, more useful, and exactly the thing this project exists
not to build.

You do not get that property by reviewing strings. You get it by making the
render path refuse one:

- The front renderer takes a `PublicPhrase`, never a `String`. No
  `From<String>`, no `FromStr`, no constructor taking free text. A test
  asserts none of those appear.
- The only variable part of any phrase is a `ZoneWord` — a **place**, not a
  person — read from the operator's own config table at load time and
  validated once.
- No MQTT field, kernel event, or HTTP response is ever interpolated into a
  front phrase. Notably, the retained `meta` name is **rear-only**: it is
  human-authored but it arrives over the broker, so anyone who can write there
  can set it.

The `ZoneWord` grammar then closes the abbreviation route specifically:
uppercase letters, digits and single spaces only, at most 16 characters, and
every space-separated word at least two characters. So `K. AT DOOR` is refused
for its punctuation and `K AT DOOR` is refused for its initial, both at config
load with a message naming the zone.

**What this does not claim.** An operator who types a housemate's name into
their own zone table has typed it themselves, and no grammar can tell `ROSE`
the room from `ROSE` the person. The honest guarantee is narrower and worth
more: *nothing the network carries can put a word on the front matrix*, and
the shape identity-abbreviation actually takes is refused.

### 3.3 No Beacon origination

The surface displays life-safety advisories; it never originates one, and
never from sensor state. Beacon invariant 1 is that sensors may prompt a human
and must not originate — a desk display is further from a human than a sensor
is. Since the surface has no publish path at all (§3.1) it structurally
cannot, and a test greps for the origination symbols anyway.

The advisory words are the ones the shipped Home Assistant blueprint already
puts on this hardware — SMOKE, CO ALARM — so a household running both lanes
reads one vocabulary.

### 3.4 Local-first

The default and the recommendation is the local HTTP path: USB virtual
Ethernet (`http://10.0.4.20`) or the LAN. `device::validate_url` refuses any
`busy.app` host, reusing the alert relay's rule rather than writing a second
one that could drift, and the daemon calls it **before** anything opens a
socket (a test asserts the ordering).

This is stricter than the relay's posture, and deliberately so. A surface
reaffirms its state on a cadence, so a cloud path would hand a third party a
continuous, timestamped record of when the household's witness state changes —
a finer timing oracle than the event log itself is allowed to expose
(Invariant III). The vendor cloud is therefore not an opt-in here; it is
refused. `TransportPath::Cloud` exists so the boundary has a name the rear
could print, and the config loader guarantees it never will.

### 3.5 Fail visible, not silent

The answer is a **dead-man's switch**, and it is the design's best load-bearing
idea.

Every drawing the surface makes carries a `timeout`, and the surface promises
to redraw well inside it (default: expiry at 6 s, redraw every 2 s). So if the
bridge process dies, the hub loses power, the LAN drops, or the broker goes
away, the last drawing ages off and the matrix goes **blank** — which is
honest — instead of holding a calm green pulse that means "everything is
proved" while nothing is proving anything.

No amount of error handling inside the process can give you this, because the
dangerous cases are the ones where the process is not running to handle
anything. Only the device expiring the drawing gives you this.

Losing an upstream is also its own front state, distinct from both healthy and
degraded: **NO LINK**, still, in a color that is neither the calm green nor
the degraded amber. Degraded means *the house knows something is wrong*.
Unknown means *this glass does not know anything*. Both unknown variants
(broker, kernel) render the same public word and differ only on the rear —
which is the two-audience split doing exactly its job.

## 4 · Animation: absence of motion is the alarm

The living-canary health cue on the shipped glass is a slow breath meaning
*the chain is intact and the witnesses are alive*
([display_living_canary.md](../hardware/display_living_canary.md)). This
surface keeps that grammar and sharpens it: **when verification fails or a
Canary drops out, the pulse stops and the color shifts. Stillness is the
alert. Motion is reassurance.**

This inverts the usual convention on purpose. An animated alarm trains a
household to feel watched by the very object whose job is to make watching
legible; a device that flashes at people in a hallway has become the thing it
was built to answer. **There are no flashing alert states here, and there will
not be.** A reviewer who asks for one should be pointed at this paragraph.

It is also the honest direction. A flashing alarm needs a live process to
flash it, so its absence is silence. A breathing calm state needs a live
process to breathe, so its absence is visible. Which unifies the two
mechanisms: the breath is driven by the same periodic redraw that arms the
device-side expiry in §3.5. One cadence, both jobs — the breath *is* the
liveness proof, and a bridge that dies stops breathing and then goes dark, in
that order, on the glass.

An advisory is made visually distinct from a red degraded state without adding
motion: it is the only render that also lights the bar's notification LED, a
second channel rather than a faster one.

**Device-side animation, and why it is not used yet.** The API does have one —
element `type: "animation"` with `loop: true`, plus a self-ticking `countdown`
and text scroll — and the design doc's instinct (define the animation
device-side, switch which is active) is right. But an `animation` element
needs an uploaded asset or a `stock_path`, and neither the asset format nor
the stock catalog is documented anywhere reachable. So the breath is currently
a low-rate redraw: four steps of 2 s, about 0.5 requests per second, a wash
across a 16-pixel-tall panel where the difference from a device-side loop is
invisible. The `countdown` primitive **is** used, for the quiet-window timer
on the rear, so that clock stays correct across a bridge restart. Moving the
breath device-side is §9's first item.

## 5 · The front states

| state | phrase | motion | color | priority |
|---|---|---|---|---|
| Calm | *(no text)* | breathing | green | 40 |
| Presence | the zone label, or `PRESENCE` | breathing | green | 40 |
| Degraded | `TAMPER` / `CHAIN FAIL` / `WITNESS LOST` / `WITNESS LATE` | still | red (tamper) or amber | 70 |

| Unknown | `NO LINK` | still | blue | 70 |
| Advisory | `SMOKE` / `CO ALARM` | still | red **+ LED** | 100 |
| On call | `ON CALL` | still | violet | 40 |

The calm state is the **absence** of a word — a pulse, not a caption. Color
never carries meaning alone (AD-Core §2.2): every state that means trouble
also has a word, and calm is distinguished by motion and by silence.

Precedence, highest first: **Advisory > Degraded > Unknown > on-call >
Presence > Calm.** An advisory overrides every mode and every window, Dark
included — a display that hid a smoke advisory because someone set it to Dark
would be indefensible. Degraded overrides the quiet window and Dark too: the
lever quiets *cards*, not the honesty about whether the witness system works.
Unknown overrides the quiet window but not Dark, where a blank glass is
already the honest answer.

**A Canary going dark reaches the room.** Liveness is not a card — no entity
announces "I am missing" — so it is folded into the front resolver directly
rather than through the classing gate. A Canary past `witness_late_secs` (180 s
by default, AD-Core §2.1's reference deadline) is `WITNESS LATE`; past
`witness_lost_secs` (600 s) it is `WITNESS LOST`. Either stops the breath. A
device that has never been heard from at all is deliberately not counted: that
is a config entry, not a witness that went dark, and crying wolf on first boot
teaches a household to ignore the one state that matters.

**A blank front is not a blank device.** Dark and the quiet window silence the
room-facing matrix while the rear keeps talking — Dark in particular has to
keep saying that witnessing continues. So a blank front produces a rear-only
drawing rather than nothing at all, and the previously drawn front element goes
away by ageing off its own expiry, the same mechanism that blanks the glass
when this process dies.

**Presence debouncing.** A presence card dwells 8 s and then decays back to the
breath. A retrigger for the same Canary inside 30 s is swallowed, and
critically it **extends nothing** — extending would let a chattering sensor
hold the matrix indefinitely, which is a strobe with extra steps. Debouncing is
per Canary, so the back gate is not silenced by the porch. A **retained**
`state` publish never lights the matrix at all: the broker replaying what it
already held proves the state, not a transition, the same replay reasoning
`PeerTable` applies to a retained chain publish.

## 6 · The controls

| control | does |
|---|---|
| center button | acknowledge the card on the glass, advance to the next waiting one |
| center long-press | enter or leave the timeline scrubber |
| rotary dial | scrub the recent verified timeline, one event per detent |
| mode switch | cycle Live → Timeline → Chain health → Dark |
| top lever | open a quiet-display window; the rear shows the countdown |

**Dark blanks the glass, not the witnessing.** The rear says so in as many
words whenever Dark is entered, because a blank security display that does not
explain itself is indistinguishable from a broken one — and because a
household must never acquire the belief that the bar on the desk is a mute
button.

**The dial reads the log through the one read-only route.** Every kernel route
that returns events — `/events`, `/events/latest`, `/digest`, `/export/bundle`
— runs `export_events_for_api`, which **appends a signed export receipt**
(Invariant IV: an export leaves a receipt tied to a specific disclosure act).
Correct for an export; disastrous for a polling display, which at one refresh a
minute would forge 1,440 disclosure receipts a day and bury the record of who
actually looked at the evidence under acts nobody performed.
`GET /api/sealed-log` appends nothing, and a source-level test keeps the
surface on it — the difference is invisible at the call site, since all of them
are plain `GET`s returning JSON.

What the surface checks is the entry-hash walk: `SHA256(prev_hash || payload)`
across the served tail, linking to its checkpoint anchor. That proves the rows
are internally consistent and proves **nothing about who wrote them**, so the
verdict is `self-consistent` and the rear says exactly that. It never says
"verified events".

**The dial is the best idea in the design.** It turns the witness log into
something you can physically scroll through at the door: each detent steps one
verified event, the front shows that event's public card, the rear shows what
was actually verified about it. The first detent out of live lands on the
newest event; past the oldest the cursor stops rather than wrapping, because a
witness log has a beginning and pretending otherwise would be a small lie
about the record. Scrubbing is a read over a snapshot — it cannot alter,
export, or unseal anything, and no function reachable from it could.

What the rear says about a scrubbed event matters, and it is the place this
design most easily could have overclaimed. Sealed rows carry no per-event
signature a reader can check in isolation, so the rear names two things
honestly: the event's own `attestation` tier (`device` / `adapter` /
`ha-bridged`) and the log verdict above. A test asserts that no line about the
timeline ever contains the word "verified" — the per-device `chain N verified`
line may, because there the peer table really did check an Ed25519 signature
against a pinned key.

### The physical inputs are design-only — here is exactly why

The bar does expose its inputs. `GET /api/status/ws` upgrades to a WebSocket
that pushes `InputEvent { key, state, timestamp_ms }` with the key enum `up,
down, ok, back, start, busy, custom, off, apps, settings`, and it is confirmed
local-only. Two things stop it being wired today, and neither is effort:

1. **The frames are protobuf and the `BSB_State.State` schema is not published
   with field numbers anywhere reachable.** Decoding it would mean guessing a
   wire format. Guessing is worse than waiting.
2. **Which physical control each key name denotes is not stated anywhere.**
   `up`/`down` are plausibly the dial and `busy`/`start` plausibly the lever.
   "Plausibly" is not a thing to build a mapping on.

So the decision layer for those events is complete and tested, and it is
driven today by a `ControlSource` that needs neither answer: **MQTT**, from
Home Assistant or anything else that can publish to the broker. The Home
Assistant side is a drop-in package —
[`homeassistant/packages/securacv_busybar_surface.yaml`](../../homeassistant/packages/securacv_busybar_surface.yaml)
— giving the button, the mode switch, the dial and the lever as ordinary MQTT
button, select and number entities. Anyone who can
write to your broker can change what this glass shows — which is true, and
bounded by §3.1: the only thing a command can produce is a drawing. One bench
session settles both questions, and the physical lane is then one file.

### Camera-active passthrough

The bar has a native on-call state, and it is **not exposed by the device's
HTTP API** — nothing in the firmware's OpenAPI mentions a camera or a
microphone, and the feature is implemented host-side in the vendor's desktop
app, which watches the host machine and pushes a status to the bar.

The closest achievable design is a signal the household already owns: an
operator-configured MQTT topic carrying the boolean, off by default. The front
then shows `ON CALL` while it is true. It is deliberately not a card and not a
witness claim — it says nothing about SecuraCV, whose Canaries do not stream,
and a capture indicator about *them* would read false in the one direction
that matters. The word borrows the vendor's own so nobody misreads it.

A device that announces when a camera is running is squarely on this project's
side of the argument, which is why it is worth carrying even in this reduced
form.

## 7 · Where the code lives, and why there

`src/surface/busybar/`, under a new `src/surface/` module — a third thing next
to `adapter` (claims come in) and `bridge` (coarse state goes out to a
home-automation ecosystem).

It is not filed under `bridge` for a reason with teeth. The bridge rule is a
**fixed publication cadence** that does not vary with event occurrence,
because a bridge's consumer is a party whose logs the owner does not hold, and
a publication rate tracking the event rate hands them a timing oracle. A
surface's consumer is a human standing in the room. Holding a presence card
for a readable dwell is the entire job, and metering it to a metronome would
make the surface useless without protecting anyone. Filing it under `bridge`
would have meant either breaking that module's stated invariant or quietly
weakening its doc.

**It is not a `devices/` manifest.** `devices/<slug>/device.json` requires
`family` (a `firmware/flavors.json` product) and `board` (a
`firmware/boards/boards.json` row), and its schema is
`additionalProperties: false`. A BUSY Bar has neither: no SecuraCV firmware, no
board in our registry, no enclosure, no flasher catalog entry, no emulator
twin. `devices/README.md` is explicit that a manifest is the join across those
seven files for one Canary. Third-party hardware we render onto is not a
Canary, and forcing it into that shape would break the one thing the manifests
are for. It gets no fleet figure either — the confidence ladder is derived
from committed CAD and firmware evidence, and we have neither for someone
else's product.

Reuse, rather than parallel paths (FR-13):

- **`fleet_peers::PeerTable`** does the TOFU key pinning, the Ed25519 chain
  verification, the replay rejection and the online verdict. The surface feeds
  it the same messages the kernel's own `/api/fleet` does. Writing a second
  verifier here would have been the parallel path, and the second one would
  have been the one that got the replay rule wrong.
- **`relay::acoustic_advisory`** is the smoke/CO decision, factored out of the
  relay's `sensing` arm so both lanes read one rule — including the trap that
  the cumulative `t3_detected`/`t4_detected` counters stay nonzero forever and
  must never be the gate.
- **`relay::busybar::validate_url`** is the cloud refusal.
- The card ids and classes are pinned to `canary-cards.js`.

**No Lab bench, and that is the considered answer rather than an omission.**
The Lab (`canary-local/`) is real firmware compiled to WebAssembly plus the
benches around flashing it; this surface is hub Rust driving hardware we have
no firmware for and cannot emulate. What *is* expected of a card-rendering
surface is card-model parity with the reference renderer, and that is
implemented and tested the same way the wall glass's model is
(`tests_host/test_fleet_cards.cpp` pins the firmware's; a unit test here reads
`canary-local/assets/canary-cards.js` and pins ours). If someone later wants a
teaching bench that draws the two displays in a browser, the pure core is
already the right shape for it — it is a projection with no I/O — and
`build-line.json` is where it would have to be registered.

The two lanes draw under **different** application names (`securacv` for the
relay, `securacv-surface` here). One name would mean each silently wiping the
other's content off the same matrix; two means the bar's own `priority` field
arbitrates, and the surface's ladder sits entirely below the relay's, so a
real alert always wins the glass.

## 8 · Two defects this work found in the shipped lane

Both are fixed in the same change, in all three places the product touches
this hardware (the relay, the blueprint, the recipe doc).

- **Colors were six hex digits; the device wants eight.** The firmware types
  both `led_notification_color` and an element `color` with the pattern
  `^#[a-fA-F0-9]{8}$` — RGBA. A six-digit CSS color does not match it, so on
  firmware that enforces the pattern the draw was rejected and the bar stayed
  dark with no error an owner could see. `Rgba` in this module can only render
  the eight-digit form.
- **The troubleshooting doc named the wrong status code.** An unauthenticated
  Wi-Fi request gets **403**, not 401, and USB and loopback bypass the access
  key entirely — so it is a Wi-Fi-only symptom.

One more finding is recorded and **not** acted on, because acting on it would
be guessing: the spec documents `scroll_rate` in **pixels per minute**, and the
relay has shipped `scroll_rate: 20` since it was written, which under that
reading is about three and a half minutes to cross the matrix. Either the
units differ on shipped firmware or that value is far too low. This surface
scrolls nothing — its whole vocabulary is short enough not to need it — and the
question goes to the bench.

## 9 · Open items — what a bench session settles

1. **Move the breath device-side.** Needs the animation asset format and the
   `stock_path` catalog. The dead-man's expiry stays either way; it just stops
   being the same mechanism as the motion.
2. **`DELETE /api/display/draw` parameter location.** The spec names a
   `DeletionParameters` schema with `application_name` and `element_ids` but
   does not pin query-vs-body. We build the query form, matching the shipped
   HA recipe. A one-function change if a real unit disagrees.
3. **`scroll_rate` units.** §8.
4. **The input WebSocket.** The `BSB_State.State` protobuf schema, and the
   mapping from `up`/`down`/`ok`/`back`/`start`/`busy` to the physical dial,
   button, switch and lever. Both are needed before the physical control lane
   is anything but a guess.
5. **Rear rendering.** Whether a text element's `color` is honored or reduced
   to gray on the monochrome back display, and how many `tiny` lines actually
   fit in 160×80. `MAX_REAR_LINES` is a bound, not a measurement.
6. **Priority arbitration in practice.** That a `409` really is what a
   lower-priority draw gets while another application owns the glass, and that
   the relay's alert reliably preempts the surface.
7. **Draw cadence.** No rate limit is documented. ~0.5 requests per second is
   modest, but it is continuous, and a real unit should be watched for thermal
   or battery cost before the default is blessed.

Until items 4 through 7 are answered on real hardware, this surface is
**design-complete and bench-unvalidated**, and the README says exactly that.
