# Apple Home & the fleet — "automations without surveillance" — design

> **Status:** RFC with its core built. **Real today:** the projection core
> — the closed signal vocabulary and the cover-traffic pacer that keeps it
> inside Invariant III — ships as
> [`src/bridge/homekit.rs`](../../src/bridge/homekit.rs) behind the
> `bridge-homekit` feature, governed by
> [`witness_dictionary.json`](../../spec/witness_dictionary.json) →
> `homekit_projection` and gated in CI by `scripts/lint_dictionary_sync.py`
> (Rust ids, HAP characteristics, and the Swift mirror all fail the build on
> drift). [`HomeKitBridge.swift`](../../ios/Sources/SecuraCV/Native/HomeKitBridge.swift)
> is rewritten as the honest shepherd (§3.0) and mirrors that vocabulary; the
> iOS app already carries the HomeKit entitlement and usage string; the Home
> Assistant integration already publishes binary_sensors a HomeKit bridge can
> project. **Still design:** every lane that actually speaks HAP or Matter to
> a home (§3.2–§3.5) and the app UI that renders the shepherd. No *lane*
> ships until the open decisions at the end are settled, and **nothing here
> is a gate**:
> the fleet must remain fully usable with no Apple device in the house
> (the standing parity rule from
> [`apple_watch_and_notifications.md`](apple_watch_and_notifications.md) §5).
>
> **Scope:** how a Canary fleet becomes a first-class citizen of Apple Home —
> the trigger fabric for a home's automations — on iPhone, iPad, Apple Watch,
> Apple TV, Mac, and HomePod, without a single pixel or identity crossing the
> boundary. Plus the researched, argued verdict on HomeKit Secure Video.
>
> **Out of scope:** streaming video or audio into HomeKit in any form (§2 —
> argued once, with revisit triggers, per the
> [`lan_baby_monitor.md`](lan_baby_monitor.md) precedent), and the alert
> relay (owned by [`alert_relay.md`](alert_relay.md) — HomeKit changes
> nothing there).
>
> Grep tokens: `apple home`, `homekit bridge`, `hap accessory`, `hksv verdict`,
> `semantic projection`, `dumb-pir bar`, `bridge site`, `matter projection`.
>
> It builds directly on:
> [`iphone_companion_app.md`](iphone_companion_app.md) (the foundational RFC —
> especially §6, where Apple Home is the app we model *ourselves* after) ·
> [`apple_watch_and_notifications.md`](apple_watch_and_notifications.md) ·
> [the Witness Wall](../tvos/README.md) ·
> [`spec/invariants.md`](../../spec/invariants.md) ·
> [`docs/feature-flags.md`](../feature-flags.md) ·
> [`docs/FLIGHT_RULES.md`](../FLIGHT_RULES.md) (FR-4, FR-8, FR-10, FR-13).

---

## The through-line

Every camera company integrates with Apple Home the same way: they pipe video
in. We can't — and that is not a limitation, it is the design. What Apple Home
actually runs on is not video; it is **boolean truth**: *motion here,
occupancy there, tamper, alive*. That is exactly what a witness emits.

So the integration is this: **each Canary becomes the most trustworthy sensor
Apple Home has ever met, and Apple Home becomes the fleet's hands.** A person
crosses the driveway after dark → the path lights come on before anyone opens
an app. A Canary reports tamper → every light in the house goes to full and
the bedside HomePod plays the household's chosen sound. The witness stays a
witness; the *house* does the responding. HomeKit is read-out, not a new data
path — the sentence is already in the code
(`HomeKitBridge.swift`), and this epic is the plan that makes it true.

The bar we hold the **default projection** to, throughout this doc: **out of
the box, Apple Home learns no more from a Canary than it would learn from a
$20 dumb PIR sensor** — a present-tense boolean, and nothing else. The
witness intelligence (object classes, sealed chains, attestation, the record
itself) stays home. We call this the **dumb-PIR bar**. Exactly one opt-in
step past it exists — the coarse `ObjectClass` word (§3.1), never identity —
and the one signal that survives even booleans (state-change *timing*) is
named out loud and then **bounded by the pacer** (§5), which publishes on a
metronome rather than on events. Honesty by construction rather than by
promise.

Why this could genuinely be a showcase integration rather than a checkbox:
every other security accessory in the Home app asks the user to trade privacy
for automation. This is the one accessory line where the privacy is
structural — the surveillance code was never written — and the automations
work anyway. That story is Apple's own pitch for HomeKit, made literal.

---

## 1. The research — Apple Home in mid-2026, and what it means for us

*(Snapshot dated 2026-08-03; the moving parts below are pinned with links so
this section can be re-verified instead of re-researched.)*

### 1.1 The protocol landscape

- **HAP (HomeKit Accessory Protocol) is stable and open enough.** The
  accessory protocol has a
  [non-commercial specification](https://www.cnx-software.com/2017/06/09/apple-opens-homekit-accessory-protocol-specification-to-non-commercial-projects/)
  (Release R2, essentially frozen since 2019) and an Apache-licensed
  [open-source ADK](https://github.com/apple/HomeKitADK). Uncertified
  accessories pair with a one-time "Add Anyway" confirmation — a UX that
  Homebridge and [Scrypted](https://docs.scrypted.app/homekit.html) have
  normalized for millions of users. Commercial *sale* of HomeKit-enabled
  hardware is what requires the Works with Apple Home (MFi) program — see
  open decision #2.
- **[HomeSpan](https://github.com/HomeSpan/HomeSpan)** is the mature
  Arduino-ESP32 HAP-R2 implementation — actively maintained, ESP32-S3
  supported. Our WAP-class boards are Arduino-framework ESP32-S3: the
  device-native bridge site (§3.4) is real, not hypothetical.
- **Matter 1.5 (November 2025) added cameras** —
  [finalized by the CSA](https://www.macrumors.com/2025/11/20/matter-1-5-camera-support/)
  — but as of mid-2026
  [Apple has not shipped Matter camera support](https://appleinsider.com/articles/26/03/17/the-first-matter-camera-has-arrived-but-apple-users-wont-notice)
  (SmartThings is first). Matter *sensor* device types (occupancy, contact)
  commission into Apple Home today, from ESP32 silicon, via
  [esp-matter](https://docs.espressif.com/projects/arduino-esp32/en/latest/matter/matter.html).
  Matter is the multi-ecosystem projection (§3.5), not the first move.
- **HomeKit framework availability, per platform** (for the app-side work in
  §4): iOS/iPadOS — full read/control plus creating scenes and automations;
  watchOS and tvOS — read and control, **no home management and no adding
  accessories**; Mac — Catalyst only, which our iOS app explicitly disables
  (`SUPPORTS_MACCATALYST: NO`). Third-party apps get **no access to HKSV
  footage or camera streams** on any platform — Apple keeps that surface to
  itself, which for us is convenient: there is nothing to be tempted by.

### 1.2 HomeKit Secure Video, as of iOS 27

WWDC 2026 made HKSV bigger, not smaller:
[4K recording, Apple Intelligence clip descriptions and notification
summarizing](https://www.matteralpha.com/industry-news/ios-27-apple-home-thread-1-4-4k-energy),
Thread 1.4 in tvOS 27. And on June 3, 2026, Apple published something
genuinely new: the
[**HomeKit Secure Video Open Source Compatibility Guide**](https://developer.apple.com/download/files/HomeKit-Secure-Video-Open-Source-Compatibility-Guide.pdf)
(Developer Preview) — an official path for open-source camera firmware to
speak HKSV. We read all of it. The technical shape, for the record:

- Layered on **HAP R17**; live view moves from legacy SRTP to **WebRTC**
  (SDP offer/answer and ICE over HAP characteristics, ≥6 concurrent
  sessions, optional SFrame end-to-end media encryption), with a multi-tier
  RTP path (≥5 sessions) retained.
- Recording is **CMAF ingest** to a publishing point on the home hub, with a
  CSR/client-certificate provisioning flow, driven by buffer
  upload/activity/event commands over HAP.
- **Three simultaneous encodings per sensor** minimum (1080p cameras:
  1080p30 + 720p30 + 360p15), **HEVC mandatory**, **Opus mandatory**
  (16 kHz capture minimum).
- Motion is first-class: an enhanced Motion Sensor service with a
  **Contributing Sensors** characteristic (which sensors triggered), and a
  **Camera Motion Zones** service (user-drawn polygons, normal/inverted).

### 1.3 What we take, and what we refuse

| From the 2026 landscape | Verdict |
|---|---|
| HAP sensor services (motion/occupancy/contact + status characteristics) | **Take.** This *is* our data model, spoken natively (§3). |
| HKSV WebRTC live view, CMAF recording ingest | **Refuse** — Invariant I, argued once in §2. |
| HKSV familiar-faces recognition | **Refuse, permanently** — Invariant II. Not deferred: absent. |
| The OSS guide's motion design language (contributing sensors, zone polygons) | **Take as design language** — multi-sensor corroboration and local zones are things the fleet already does better; the guide confirms Apple's own vocabulary for them. |
| Matter sensor device types | **Take, later** — the second projection of the same table (§3.5). |
| Matter 1.5 cameras | **Watch.** Apple hasn't shipped it, and the video half falls to the same §2 verdict anyway. |
| App Intents / Siri, HomeKit framework on tvOS | **Take** (§4.4, §4.3). |

---

## 2. The HKSV verdict — argued once, recorded, with revisit triggers

Per the [`lan_baby_monitor.md`](lan_baby_monitor.md) precedent: when a "no"
is real, argue it honestly in one place so a future revisit starts from the
argument instead of re-litigating from memory.

**HKSV's core loop is: on motion, stream the raw clip (video + audio) off
the device, automatically, to the home hub and then iCloud.** However good
Apple's encryption story is — and it is genuinely good; footage is
end-to-end encrypted to the user's own account — the *device-side* behavior
is exactly what our constitution forbids:

| HKSV behavior | Invariant it breaks |
|---|---|
| Continuous automatic export of raw video + audio on every motion trigger | **I — No Raw Export by Design.** "The kernel MUST NOT expose APIs that stream, mirror, or replay raw media externally." Raw media leaves only via break-glass, a quorum-approved disclosure act — architecturally incompatible with an automatic per-motion upload. |
| Familiar-faces recognition on recorded clips | **II — No Identity Substrate.** Not ours to enable even indirectly by shipping the pixels that feed it. |
| A clip upload fired per event, timestamped to the second | **III — Metadata Minimization.** Event-correlated network behavior and precise external timestamps are both structurally suppressed here. |
| Footage custody in a cloud, however encrypted | **IV — Local Ownership.** Our record stays home. (Invariant IV would tolerate a user-owned encrypted copy — the CloudKit precedent — but I and III fall first, so this never gets reached.) |

There is also a plainer, non-constitutional fact: **no Canary has a media
plane to offer HKSV.** The device API has no snapshot, stream, or RTSP
endpoint; the mic pipeline reduces audio to three scalars per window and
zeroes its buffer in the callback; camera Peek is a single-viewer,
physically-confirmed, thermally-throttled *setup tool*. The HKSV minimums —
three simultaneous encodings, HEVC, Opus — are also simply beyond an
ESP32-S3. Meeting HKSV would mean building the surveillance camera we exist
to refuse, then apologizing for it in hardware we don't have.

The Witness Wall doc already names HKSV as the anti-pattern
([`docs/tvos/README.md`](../tvos/README.md) §1: "a grid of live video feeds…
exactly what SecuraCV exists to refuse"). This section makes that stance the
recorded verdict.

**What the refusal does *not* close off** — the entire rest of this doc. The
event half of HKSV (motion services, status, zones, automations) is fully
open to us, and it's the half users actually automate on.

**Revisit triggers.** Reopen this section — don't restart the debate — if:

1. A written, accepted **Invariant I carve-out** for session-scoped live
   viewing exists (the constitutional work `lan_baby_monitor.md` §2.1 also
   waits on — one carve-out debate, not two).
2. Apple ships an accessory class for **event/sensor-only security devices**
   with recording explicitly absent — a "semantic camera" profile. (The OSS
   guide's Contributing Sensors and Motion Zones services are the closest
   thing yet; today they still ride a camera accessory.)
3. A future device line ships silicon with a real media plane **and** the
   break-glass story for a quorum-approved, receipted clip disclosure is
   extended to cover an HKSV-shaped destination. (Note recorded now: HKSV
   has no "ingest one adjudicated clip" API — this trigger is unlikely to
   fire in Apple's current model.)

Until then: researched properly, and the answer is *not the video — ever;
the events — absolutely.*

---

## 3. The architecture — one projection, three bridge sites

### 3.0 First, the correction this epic makes

`HomeKitBridge.swift:9` says "a pure software accessory is exposed via HAP,"
but the class holds an `HMHomeManager` — and the HomeKit *framework* can
only read and control accessories that already exist in the home; **it
cannot publish new ones.** No iOS API turns an app into an accessory. So the
existing stub, as wired, could never put a Canary in the Home app.

The honest architecture separates two roles that the stub conflates:

- **Publishing accessories** is a *bridge's* job — a HAP (or Matter) server
  running where the fleet lives: on the hub, or on the Canary itself (§3.2–§3.5).
- **The app** (§4.2) is the *shepherd and concierge*: it guides pairing,
  reads back what Apple Home sees, builds the automations, and shows the
  Doctor card when the two worlds disagree. `HomeKitBridge.swift` becomes
  this — its doctrine comment survives; its architecture note does not.

### 3.1 The semantic projection — one table, dictionary-governed

Everything every bridge site publishes derives from **one mapping**, which
lives conceptually beside the witness dictionary and physically *in* it once
implementation starts (FR-13: vocabulary first, then every mirror; the
mapping below is design, deliberately not yet in
[`witness_dictionary.json`](../../spec/witness_dictionary.json) — "never
advertise unbuilt" applies to machine-readable claims too).

| Fleet signal (already in `fleet_model.h` / `Witness`) | HAP service / characteristic | Notes |
|---|---|---|
| liveness (`!link.isDark`) | `StatusActive` on every service | The dead-man's-switch, projected. A dark Canary reads "No Response" in the Home app — Apple's UI does the honest thing for free. |
| `tamper` | `StatusTampered` | Mute punch-through preserved: tamper projects even when a witness is muted, same as everywhere else. |
| battery (where the board has one) | `StatusLowBattery` | |
| radar presence (`radarPresent` — Canary Sense) | **Occupancy Sensor** service | The single best fit in all of HAP: presence without a camera, meets the dumb-PIR bar exactly. |
| vision motion event (Canary Vision / WAP-with-camera) | **Motion Sensor** service | Present-tense boolean with a hold time (§5); the event record itself never crosses. |
| `contact_state_change` (contact modality, Fence Guard) | **Contact Sensor** service | |
| per-class motion (`Person`/`Vehicle`/`Animal`/`Package`) | optional named child Motion Sensor services ("Porch — Person") | **Opt-in, default off.** Exceeds the dumb-PIR bar by exactly one word — the sanctioned `ObjectClass` vocabulary, never identity. Users who enable it get class-conditional automations ("porch light only for a person, not the neighbor's cat"). |
| doorbell press (Vision in the doorbell enclosure) | **Doorbell** service (Stateless Programmable Switch) | Cameraless doorbells are legal HAP and chime HomePods. Exploration-grade — open decision #7. |

**What is deliberately not in the table:** event history (HomeKit
characteristics are current-state only — there is nothing for Apple Home to
query, so Invariant VII survives by construction); zone names and zone
geometry (local-only, Invariant III); timestamps (Apple Home sees "now," we
keep the coarse bucket); attestation tiers and chain state (trust vocabulary
stays on surfaces that can render it honestly — the app, the Wall, HA);
anything Peek-shaped.

### 3.2 Bridge site A — the hub you already run (Home Assistant) · works first

Households running the Pi hub already have a HomeKit bridge installed: Home
Assistant's own `homekit` integration projects entities into Apple Home. Our
[integration](../../custom_components/securacv) publishes per-Canary
binary_sensors today (connectivity, tamper, problem classes). Two
deliverables make this lane real:

- **A0 — a worked recipe doc** in [`docs/integrations/`](../README.md), in
  the style of
  [`home-assistant-frigate-mqtt.md`](../integrations/home-assistant-frigate-mqtt.md):
  the HA HomeKit Bridge include-list, plus template binary_sensors
  (`device_class: motion` / `occupancy`) derived from securacv MQTT events
  for the sensors the integration doesn't yet publish natively. Works with
  zero new code in this repo.
- **A1 — native motion/occupancy binary_sensors** in
  `custom_components/securacv`, fed by the events/presence topics it already
  subscribes to, so the recipe sheds its template section. Dictionary-checked
  like every other mirror.

Cost: near zero. Requires: the hub. Attestation honesty: what Apple Home
sees through HA is `ha-bridged` tier by definition — the app and the Wall
keep saying so; Apple Home doesn't need to know what it can't render.

### 3.3 Bridge site B — the kernel speaks HAP (hub without Home Assistant)

A HAP accessory server inside `witnessd` (Rust, beside `src/api/mod.rs`),
projecting the same table for kernel-attached sources — including the venue
case: the Witness Board over existing RTSP/ONVIF cameras, where "the
registers' witness layer shows up in the owner's Home app" is a genuinely
new thing no NVR vendor offers without a subscription. Cargo feature
`bridge-homekit` (new prefix — these are egress, not adapters; registered in
[`feature-flags.md`](../feature-flags.md) the day it exists), default off,
Class B code: FR-2 no-panic, FR-4 bounded session/pairing tables, FR-8
explicit modes, FR-10 observables. Crate selection (`hap` vs alternatives)
passes the FR-14 dependency gate before a line lands.

### 3.4 Bridge site C — the Canary itself is the accessory (no hub at all)

The magic tier: a WAP-class or Sense Canary that pairs straight into Apple
Home from the Lab's QR code, no hub, no bridge, nothing else powered on.
[HomeSpan](https://github.com/HomeSpan/HomeSpan) (HAP-R2, Arduino-ESP32,
S3-supported) behind **`FEATURE_HOMEKIT`**, default off, with the standing
firmware rules applied:

- A `feature_sanity.h` gate (e.g. `FEATURE_HOMEKIT` requires
  `FEATURE_WIFI`; motion service requires a motion-capable build) and a
  `build_matrix.json` row per flavor that carries it.
- **Measured flash/RAM budget before any promise.** HAP + mDNS + the
  existing Opera/MQTT/HTTP stack on one S3 is plausible, not proven — and a
  performance claim without a benchmark is a rejected PR here. The phase
  gate is a measured build on a `verified`-tier board.
- **Pairing keys are secrets.** HomeKit long-term pairing keys persist in
  NVS; they get the `opera_secret` treatment — refuse to persist on
  flash-encryption-off devices (the tiered story from
  [`hardware_root_of_trust.md`](hardware_root_of_trust.md)).
- Pairing UX ships in **both flashers** (browser Lab and desktop Flasher) in
  the same phase — the two-flashers rule is repo law, and the setup QR is a
  user-facing diagnostic if anything is.

### 3.5 Bridge site D — the Matter projection (later, and multi-ecosystem)

The same table, spoken as Matter occupancy/contact sensor device types
(esp-matter; the ESP32-C6 boards in
[`board_market_research.md`](../board_market_research.md) carry Thread
radios). One projection layer, two dialects — and Matter buys Google, Alexa,
and SmartThings alongside Apple for the same table. Deliberately after HAP:
HAP's status characteristics (tamper, active) map our signals more richly
today, HomeSpan is more proven on our silicon than esp-matter, and the Apple
experience is the one this epic is for. Nothing in §3.1 is HAP-specific by
construction — that is the point of the table.

---

## 4. What it feels like — the use cases, per platform

### 4.1 The marquee scenes (the payoff loop)

1. **Lights before eyes.** Person in the driveway after dark → path lights
   on, before any app opens. The automation runs on Apple's home hub —
   phone asleep, app not running, still works.
2. **The porch knows.** Package detected (opt-in class service) → the porch
   light pulses once, the HomePod plays a soft chime. No doorbell camera, no
   cloud clip — the *house* acknowledges, the record seals locally.
3. **Tamper is a house-wide event.** A Canary reports tamper → every light
   to 100%, shades up, HomePods play the household's chosen sound. The
   response a security company sells as a monitoring plan, authored by the
   household in the Home app, triggered by a witness that can't be quietly
   unplugged (silence itself is the alarm — the dead-man's-switch already
   built into the fleet, §5).
4. **Quiet when home** *(the privacy trick nobody else can do)*: Apple
   Home's "arrive home" → indoor witnesses drop to tamper-only posture. An
   integration that uses home automation to *reduce* sensing — the inbound
   control half is open decision #4, because inbound is a new trust surface
   and gets designed as one.
5. **Goodnight, witnesses.** The "Good Night" scene arms the after-hours
   posture — contingent on a posture vocabulary existing at all
   (dictionary-first; open decision #3).

### 4.2 iPhone & iPad — the shepherd and the concierge

The app's HomeKit role, replacing the stub's ambitions with buildable ones:

- **Pairing shepherd.** Finds the Canary (mDNS — already built), shows the
  setup QR, hands off to Apple's pairing flow, then *confirms the result*:
  "Porch Canary is in Apple Home — automations available." (Confirms, not
  "verifies" — that word stays reserved for an Ed25519 signature checked
  against a pinned key, and a pairing observation is nothing of the sort.) The
  [onboarding wizard doc](../onboarding_unified_wizard.md) already studied
  HomeKit's own setup ritual as precedent; now we join it.
- **Automation concierge.** A "tell the house" screen: pick a witness signal
  → pick a scene. Written as real HomeKit automations (`HMEventTrigger` on
  our accessories' characteristics), so they run on the home hub forever
  after, app closed. The app is the *author*, never the runtime — that is
  what makes it reliable and what keeps the app thin.
- **The Doctor card.** One card when the two worlds disagree: accessory
  unpaired, home hub missing (no hub = no automations — say so), Apple Home
  seeing a Canary the fleet says is dark. Calm, actionable, in the standing
  Doctor idiom.
- Opt-in remains the law: `isEnabled = false` until a human turns it on,
  per-signal consent for the class services.

### 4.3 Apple TV — cause and response on one timeline

The Wall gains the read-only home context tvOS's HomeKit framework allows:
interleave *"person at the porch (verified ✓)"* with *"porch light on —
Home automation"* on the same timeline. The payoff loop, visible on the
shared screen: the household watches the house answer the witness. Strictly
read-only, strictly optional, degrades to today's Wall when HomeKit is
absent or unauthorized.

### 4.4 Watch & Siri

The wrist already has the glance, the heartbeat, and mirrored ack/mute. What
HomeKit adds arrives free (a Home-app automation fires regardless of which
screen you're near). The increment worth building is **App Intents** — the
scaffold exists (`FleetFocusFilter` already links the framework): "Hey Siri,
is the fleet OK?" answered from the on-phone `FleetStore` on every surface
Siri lives, including the wrist and CarPlay. No new privacy surface: the
answer is the fleet ladder, computed locally.

### 4.5 Mac — Apple already built our Mac app

`SUPPORTS_MACCATALYST: NO` stays. The Mac story: once Canaries are
accessories, **Apple's own Home app on the Mac renders the fleet, the
automations, and the notifications** — maintained by Apple, forever, for
free. The Tauri apps remain birth tools; the witness console on Mac remains
an open question owned by the iPhone RFC, not this one.

### 4.6 HomePod

Two roles, both already in-idiom: scene sounds authored by the household
(§4.1 — never our voice, never alarm-tone impersonation; the
`lint_no_impersonation.sh` red lines bind Beacon, and we stay far inside
them here too), and the existing AirPlay chime route
(`MediaRoute.swift`) for the app's own critical alert — unchanged.

---

## 5. Self-healing, and working when Apple isn't there

**The degradation table (FR-8: every mode explicit, every transition an
observable):**

| Condition | Behavior |
|---|---|
| No Apple devices at all | Feature dormant. Zero runtime cost (compile-gated on firmware, opt-in flag elsewhere). The fleet, HA, the Wall, the PWA: unchanged. This column is the product; Apple is enhancement. |
| Apple Home present, no home hub | Accessories pair, states render live in the Home app; automations don't run (Apple's rule). The app's Doctor card says exactly that, once. |
| Bridge restarts / Canary reboots | HAP state is **re-derived, never restored**: re-advertise mDNS, republish current signals. Pairings persist (encrypted NVS / hub keystore); nothing else is worth keeping. Stateless projection = nothing to corrupt. |
| Canary goes dark | `StatusActive` false → Home app shows "No Response" — Apple's UI carries our dead-man's-switch semantics natively. Fleet-side, the existing silence-is-the-alarm machinery (sentinel, mesh mutual watch) is untouched: HomeKit is an *additional* renderer of darkness, never the detector. |
| mDNS lost / network partition | Bounded reconnect with full jitter — the `Backoff` pattern from the Wall's `FleetClient` is the house template (base 2 s, cap 60 s, reset on success), and firmware backlog #921 (reconnect jitter) inherits this consumer. |
| Motion pulse discipline | Motion projects with a hold time (HKSV's own guide models motion the same way), FR-4-bounded: a characteristic is a current-state cell, not a queue — the projection *coalesces by construction*, and a flapping sensor cannot flood anything. |
| User unpairs / revokes in Apple Home | We notice (accessory removed), the app reflects it, nothing breaks: HomeKit was read-out, so losing it loses nothing but read-out. |

The invariant echo, one line per number, in the
[`iphone_companion_app.md`](iphone_companion_app.md) §5 style: no raw export
(**I** — booleans only, no media plane exists); no identity substrate
(**II** — `ObjectClass` at most, opt-in, never identity); metadata
minimization (**III** — present-tense state, no zones, no precise history
externally; the timing residual argued honestly below); local ownership
(**IV** — HAP and Matter are LAN protocols; accessory state syncing to
*the user's own* Apple account is the CloudKit-private-DB precedent, not a
SecuraCV cloud); break-glass
(**V** — untouched, no media path exists here at all); no retroactive
expansion (**VI** — a stateless present-tense projection has no archive to
reprocess); non-queryability (**VII** — no history characteristic exists;
there is nothing to ask).

**The Invariant III timing residual, argued honestly instead of waved at.**
A characteristic that flips on motion is event-correlated behavior, and
Invariant III suppresses event-correlated *external* signals and precise
external timestamps. Two different scopes hide in that sentence, and this
design refuses to blur them:

- **On the LAN**, real-time state change is established, shipped practice —
  the MQTT status plane, the SSE witness stream, and the BLE beacon all
  deliver live truth to household surfaces today, and the HA integration is
  `local_push` by design. A HAP characteristic notify to a home hub on the
  same network is the same class of signal: content-free, boolean,
  household-internal.
- **Off the LAN is the new part.** An Apple home hub syncs accessory state
  changes to the user's other devices through Apple's infrastructure. The
  content is end-to-end encrypted to the user's own account (the CloudKit
  precedent covers custody) — but a network observer of the household's
  uplink could see *that* traffic left *when* it left: an event-correlated
  timing signal carrying no content. This is the same residual the alert
  relay accepts with eyes open ("a compromised relay leaks *'this household
  got some alerts,'* never *what*"), and it must be accepted — or shaped —
  with the same honesty here, not asserted away.

**How this is settled — the pacer (built: `bridge-homekit`).** Invariant III
names its own conforming path: network behavior may not vary with event
occurrence *"unless explicitly configured for cover traffic."* So the
projection **is** that configuration, and the mechanism is the whole answer:

- **Nothing publishes on an event.** Events only mark state pending;
  publication happens on a **metronome** (`Projection::tick`) that emits on
  every tick whether or not anything happened.
  `publishes_every_tick_even_when_nothing_ever_happens` and
  `publication_rate_does_not_vary_with_event_rate` are tests, not intentions.
- **The tick is the external time resolution.** Nothing downstream can place
  an event more precisely than the tick it fell in, because a finer time
  never reached the wire. `tick_ms` is therefore a **privacy/latency dial**
  (default 1 s, bounded 200 ms–10 min, and an out-of-range value is *refused,
  never clamped*) — the log's 10-minute coarsening instinct, applied to
  egress.
- **Count doesn't leak either.** A thousand events inside one tick publish
  exactly like one: the pacer coalesces by construction, not via a rate
  limiter someone could tune off.

What this honestly does **not** do: cover traffic is ours only for the hop we
own (kernel → controller). A downstream Apple home hub relaying changes
onward has its own traffic pattern we do not control. The guarantee is the
**bound**, not anonymity — and the bound is real, because a finer time never
existed on our wire to relay. Decision #8 is therefore resolved by
construction rather than by amending the constitution; what remains is
per-lane defaults, which is what #8 now asks.

---

## 6. Never let it rot

- **One table, one dictionary, N mirrors.** When implementation starts, the
  §3.1 projection enters `spec/witness_dictionary.json` (a `homekit`
  projection block), and `scripts/lint_dictionary_sync.py` grows the new
  mirrors — HA entity classes, Rust bridge constants, firmware service
  definitions, and the Swift signal enum that already exists. Drift becomes
  a red X, exactly like every other vocabulary here (FR-13).
- **We build on the frozen layer, deliberately.** HAP R2 has been stable
  since 2019; `HMHomeManager` is a 2014-era API; Matter is versioned by a
  standards body. The churny layer (HKSV, Apple Intelligence summaries) is
  the layer we refused for constitutional reasons anyway — the refusal and
  the rot-resistance are the same decision. The one Developer Preview
  surface we cite (the OSS guide, version strings "17.99") we cite as
  research, not as a dependency.
- **Existing gates carry the new weight**: `ios-selfheal.yml` compiles any
  Swift the epic touches nightly; `lint_feature_flags.sh` refuses an
  unregistered `bridge-homekit` or `FEATURE_HOMEKIT`; `lint_build_matrix.py`
  pins the firmware flavor truth; `lint_docs_index.py` already forced this
  doc onto the map. New CI surface needed: none until code exists —
  then golden pairing/projection tests ride the same linter that guards
  every other mirror.
- **"Never advertise unbuilt"** binds the apps too: no HomeKit UI renders
  until a bridge site actually ships; the concierge appears with the
  accessories, not before. Capability-gated, like the HA transports.

## 7. What this integration will deliberately never do

- Stream, relay, or transcode video or audio into HomeKit, HKSV, or a
  Matter camera cluster (**I** — and §2 is the argued verdict, not a mood).
- Feed familiar-faces or any identity feature, on any side of the bridge
  (**II** — absent, not disabled).
- Publish zone names/geometry, precise timestamps, or event history to
  Apple Home (**III**, **VII** — present-tense booleans; the dumb-PIR bar
  by default, the four sanctioned `ObjectClass` words at most, ever).
- Require Apple anything: no feature of the fleet gates on HomeKit, and the
  non-Apple path (HA + the Verified Timeline card + the PWA) keeps full
  parity, per the standing rule.
- Let Apple Home *originate* witness assertions: HomeKit inbound (postures,
  quiet-when-home) may adjust *sensing posture* if decision #4 lands — it
  never writes to a witness chain and never originates a Beacon (that
  channel's invariants are untouched by this epic).
- Ship a "Works with Apple Home" logo we haven't earned: certification
  claims wait for the program, and "verified" keeps meaning an Ed25519
  signature checked against a pinned key — nothing looser, including here.

## 8. Phasing summary

| Phase | Depends on | Deliverable | State |
|---|---|---|---|
| **P0** | — | The projection core: closed signal vocabulary + the cover-traffic pacer, dictionary-governed and linter-gated (`src/bridge/homekit.rs`, `bridge-homekit`) | **built** — 26 tests, incl. the Invariant III properties |
| **A0** | P0 | HA HomeKit Bridge worked recipe (`docs/integrations/`), template sensors included | open — docs only, works with zero new code (un-paced; see decision #8) |
| **A1** | A0 | native motion/occupancy binary_sensors in `custom_components/securacv` | open |
| **A2** | P0 | app as shepherd + concierge + Doctor card; per-signal consent | **partly built** — `HomeKitBridge.swift` rewritten honestly (vocabulary mirror, `HomeKitStanding` + Doctor notes, per-signal consent, tamper refusal); the UI that renders it is open |
| **B1** | P0 · decision #1 | first accessory lane on top of P0: a HAP server in witnessd **or** `FEATURE_HOMEKIT` on one verified board, measured budget first | open |
| **B2** | B1 | the other §3 site, if #1 says both | open |
| **C1** | A2 | App Intents ("is the fleet OK?"), Wall home-context timeline | open |
| **D1** | B-lane stable | Matter projection of the same table | open |
| **—** | never | HKSV video lanes, familiar faces | refused (§2), revisit triggers recorded |

## 9. Open decisions (settle before building past A1)

1. **Bridge-site order.** A-only first (prove demand on hub households),
   then witnessd-vs-firmware for the first native lane? Firmware is the
   magic but carries the budget risk; witnessd covers the venue story.
2. **Commercial licensing.** Self-flashed DIY Canaries ride the
   non-commercial HAP spec exactly as Homebridge/HomeSpan users do
   ("Add Anyway" is the honest, normalized UX). *Selling pre-flashed kits*
   with HomeKit enabled likely crosses into Works with Apple Home program
   territory — needs a real licensing review (Errer Labs, not this doc)
   before `FEATURE_HOMEKIT` defaults on in any shipped flavor. Matter
   certification (CSA, no Apple-specific program) may be the cleaner
   commercial path — feeds decision #1.
3. **Postures.** "Good Night arms the fleet" needs a posture vocabulary
   that doesn't exist yet — dictionary-first, its own small RFC, shared
   with the Business edition's open-hours/after-hours need.
4. **Inbound.** Quiet-when-home is the best trick in the epic and the only
   inbound control path — who may write it (admin-only characteristic?),
   what it may touch (sensing posture only, never chain/Beacon/config), and
   whether it demands the physical-confirm idiom. Design as a trust
   surface, not a toggle.
5. **Per-class services default.** Off (dumb-PIR bar) — confirm, and decide
   whether "Package" alone earns a friendlier default given how loved the
   use case is.
6. **The Home-app face of a Canary.** One bridged accessory per Canary vs
   one per signal; naming ("Porch Canary" vs "Porch — Person"); which
   accessory category the setup flow claims (the wizard doc's "category
   drives naming" lesson applies).
7. **Doorbell service.** Cameraless HAP doorbell (HomePod chime, no video)
   for the Vision doorbell enclosure — exploration, needs a bench test
   before it's promised anywhere.
8. **The shipped tick, per lane** (§5). The mechanism is settled and built —
   the pacer publishes on a metronome, so Invariant III's cover-traffic path
   is satisfied by construction rather than by amendment. What is still open
   is the *default* each lane ships: 1 s reads as instant and bounds timing
   to the second, but a hub lane serving a venue may want coarser, and the
   HA lane (A0) inherits Home Assistant's own change-driven publishing rather
   than ours — so the recipe must say plainly that it is the un-paced path
   until A1 routes it through the projection.

---

## Trademarks

Apple, Apple Home, HomeKit, HomeKit Secure Video, HomePod, Apple TV, tvOS,
iPhone, iPad, Apple Watch, watchOS, Mac, macOS, Siri, and AirPlay are
trademarks of Apple Inc., registered in the U.S. and other countries and
regions. Matter and the Matter logo are trademarks of the Connectivity
Standards Alliance. SecuraCV is an independent project by Errer Labs and is
**not affiliated with, endorsed, sponsored, or certified by Apple Inc. or
the Connectivity Standards Alliance.** References are nominative — for
identification and interoperability only.
