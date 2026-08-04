# Canary Guardian — worn duress witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**.
Same honesty tier as Fence Guard and the Vision concepts: the design is grounded in real
hardware and real field data, nothing has been built or worn yet. Where a number below is
someone else's measurement, it's cited; where it's a design intention, it says so.

**The one-sentence version:** a small, screenless LoRa node you *choose to wear*, asleep
almost always, that emits a signed **"I need help"** — or a quiet **"I'm OK"** — to your own
people over an off-grid mesh, and is architecturally incapable of telling anyone where you've
been.

This is the deliberate inverse of a tracking tag. A tracker answers *"where is this person?"*
for whoever holds it. Guardian answers *"is this person alright?"* — and only the wearer can
ask it, arm it, or silence it.

---

## 1 · Why worn, and why it isn't just a smaller wall Canary

Every other Canary witnesses a *place*. Guardian witnesses a *person's situation*, and four
things are only possible when the sensor moves with the body:

- **The wearer is the sensor.** "I'm in trouble," "I'm OK," "I made it home" are facts about a
  person, untethered from any fixed spot. A wall node fundamentally cannot assert *"**I** am in
  trouble"* — only "something happened *here*."
- **Off-grid reach at the point of need.** The emergency is wherever the person is — a trailhead,
  a parking structure, a blackout, a protest with saturated cell towers. Battery + LoRa mesh puts
  the witness *at the incident*, with no cell and no mains.
- **Mobility across zones.** "Arrived home / left the safe zone" requires the subject to carry the
  emitter; a fixed sensor only ever sees who passes *it*.
- **Consenting agency.** A worn beacon is armed, pressed, and canceled *by the wearer*. That's an
  ethical posture a fixed surveillance sensor can't occupy — and it's the whole reason this device
  is allowed to exist in a privacy-first fleet.

If a use case doesn't need one of those four, it should be a fixed node, not a worn one. That's
the scope filter for everything below.

---

## 2 · The hardware call: nRF52840, not ESP32 — and why

Every other Canary host is a XIAO **ESP32**. Guardian is the one place we deliberately step off
that line, to the **Seeed XIAO nRF52840 + Wio-SX1262 Kit** — the pin-incompatible sibling of the
exact ESP32-S3 Meshtastic kit [Fence Guard](./canary_fence_guard_research.md) specs. The reason is
battery life, and it's not marginal:

- The best-carried Meshtastic wearable in the wild, the **LILYGO T-Echo**, gets *days* of standby
  from an **850 mAh** cell — and it runs an **nRF52840**. A comparable **ESP32** node on a *bigger*
  2600 mAh cell lasts roughly **a day**. The nRF52 sleeps in the **single-digit µA** range with its
  BLE stack resident; a real XIAO-class ESP32-S3 measures **~14 µA and up** in *deep* sleep — CPU and
  radio fully down, not idling.
- A worn safety device that needs frequent charging is a worn safety device that's dead on the hook
  when you need it. The commercial ceiling makes the point: dedicated LoRaWAN panic buttons
  (TEKTELIC FINCH, MOKO LW014) last **5+ years** because they do exactly one thing — sleep, then send
  one packet on a press.

The nRF52840 kit keeps everything else familiar: same Wio-SX1262 (Semtech SX1262, 862–930 MHz, up
to +22 dBm), same Meshtastic firmware family, same LoRa antenna ecosystem. It trades the ESP32's
WiFi/price for the battery life a body-worn witness actually lives or dies by. (The ESP32-S3 variant
remains the right host for mains-or-big-battery *fixed* Canaries — this is a worn-line-specific call.)

**The honest caveat that drove the choice:** an ESP32 worn node is only viable if it is *TX-mostly*
— asleep by default, waking to shout, never listening continuously. The moment you want a worn node
that *listens* all day (continuous LoRa RX for two-way messaging), average draw jumps toward
~4–5 mA and you're back to daily charging. nRF52840 is what lets Guardian both shout **and** keep a
usable standby. See §6 for the power budget.

---

## 3 · What it does, ranked by honest usefulness

### 3.1 · Duress beacon — the flagship (build first)

Press-and-hold the single physical button → sign a **`duress_asserted @ <coarse zone>`** claim →
burst it over the LoRa mesh to your people → haptic confirm → back to sleep. This is the use case
the hardware is *actually good at*: the radio is asleep 99.9% of the time, one deliberate human
action wakes it, it transmits a tiny signed burst, done. It maps 1:1 onto the sealed-log model — a
discrete signed claim, not a stream.

**How well does Meshtastic serve this — honestly?**

- **Range:** a direct link runs **a few hundred meters to ~1–2 km in urban** clutter without a
  repeater, **3–5 km suburban**, **10–15 km line-of-sight**. A body-worn antenna near flesh performs
  *worse* than these figures — plan for the low end and lean on a **fleet of fixed relay nodes** (your
  home Canaries, a Fence Guard) to catch the shout.
- **Latency:** **~900–3000 ms** end-to-end even on the fastest preset, more per hop. Fine for "help"
  (seconds don't matter); useless for anything real-time.
- **Reliability:** Meshtastic retransmits up to 3× and treats any rebroadcast as an implicit ack, so
  a duress packet has decent odds of reaching *somewhere* — but it is **not guaranteed delivery**, and
  dense meshes degrade past ~50–80 nodes on default presets.

**So we frame it truthfully:** a Meshtastic duress beacon is a **best-effort, off-grid, no-cell-needed
shout your own people can hear** — genuinely valuable exactly where cellular fails and a family/neighbor
fleet exists to listen. It is **not** a 911-grade guaranteed alarm, and the docs must never imply it is.
**Hedge it:** pair the LoRa burst with a **BLE-to-phone fallback** — if a paired phone is in range, it
relays over cell too. That's what the commercial dual-mode buttons do, and it's the right belt-and-braces.

### 3.2 · Solo check-in / dead-man's-switch (build second)

A periodic "I'm OK" heartbeat; **absence past a timeout raises the alert.** This reuses the idiom the
fleet already trusts — *presence via repeated active signal, absence via consumer-side timeout* — the
same shape `state_broadcast_state()` uses for every Meshtastic claim today. Power-wise it's ideal:
wake on an RTC timer every N minutes, TX a tiny signed heartbeat, sleep.

Two honest caveats designed into the feature, not papered over:

1. **The failure mode is inverted.** A dead battery, a mesh gap, and "the person is down" all look
   identical — silence. So the heartbeat carries a **coarse battery-state field** so the consumer can
   distinguish "signal lost / low battery" from "distress," and re-check-in must be one button press.
2. **It's most credible in a bounded context** — a solo shift, a day hike — where "we should have
   heard from them by now" is meaningful. It's a check-in, not an always-on life alarm; the docs say so.

### 3.3 · Coarse zone presence — the anti-AirTag (build third)

Kids or elders carry a Guardian; home base sees **"arrived home" / "left the safe zone"** as coarse
zone-crossing *events*, **never GPS breadcrumbs**. Zones are implemented by **proximity to fixed anchor
nodes** (heard the home anchor → "home"; stopped hearing it → "left"), so the device **never computes or
transmits fine location** — it only ever emits "entered/left zone Z." This is where the privacy stance
*becomes the product*: it's the thing AirTag-style trackers structurally can't offer.

This extends cleanly into a niche but uniquely-ours fourth use: **proximity witness / two-person
integrity** — a worn node attesting "person X was present in zone Y at coarse time T" into the sealed log
(lone-worker check-ins, caregiver-visited-client attestation). Same hardware, same signing model, no new
sensing.

### 3.4 · Fall / impact detection — a *prompt*, never a *claim* ⚠️

Build the IMU in, but be ruthless about what it's allowed to do. The literature is a warning:

- Lab numbers seduce: ESP32-accelerometer fall systems report **95–97% accuracy**.
- **Real-world collapses them.** Validated against *actual* older-adult falls: **~57% average
  sensitivity**, and **3 to 85 false alarms in a single day**; another study caught only **8 of 10 real
  falls** even at good sensor placement. Sitting down hard, jumping, and running transiently look like
  falls.

A false "fall occurred" written into a **tamper-evident** log is worse than useless — it's permanent
garbage in the one place that's supposed to be trustworthy. So: a suspected impact **starts a local "are
you OK?" countdown the wearer can cancel** (a wake-on-IMU-interrupt is a great *power* trick regardless).
Only if the countdown expires uncanceled does it escalate — and even then it escalates as a *check-in
failure*, not an asserted "fall." Raw IMU heuristics never write an unqualified claim.

---

## 4 · Claim vocabulary — what this adds to the dictionary

Guardian needs **one clearly-new** claim kind and reuses the rest. Each new kind is drift-gated across
`spec/witness_dictionary.json`, the Rust vocabulary, and the two Home Assistant mirrors, so we add the
minimum:

| Signal | Proposed claim kind | New? | Notes |
|---|---|---|---|
| Duress press | **`duress_asserted`** | **new** | Coarse zone + coarse time. The one genuinely new concept — "a person deliberately called for help." Deserves to read as itself in the timeline, not as a generic alarm. |
| "I'm OK" heartbeat | *(liveness, not a sealed claim)* | no | The heartbeat is a presence signal; the *alert* is absence-inferred consumer-side, exactly like Car-Mode's departure. No new kind — silence is the event. |
| Entered/left safe zone | reuse `presence_in_restricted_zone` or a future `zone_presence_change` | tbd | Settle in design: person-scoped zone crossing may fit an existing kind or want its own. Not blocking the duress flagship. |
| Suspected impact (uncanceled) | escalates via the check-in path | no | Never its own "fall" claim (§3.4). |

**Decision to lock during firmware design, not now:** whether zone-presence reuses an existing kind.
`duress_asserted` is the only addition the flagship needs.

---

## 5 · The privacy line — built in, not bolted on

A body-worn always-on radio is *structurally* the same object as a stalking tool. The cautionary tale is
explicit: reported GPS-tracker misuse in coercive-control cases **rose 317% (2018→2023)**, and the core
critique of AirTags was that they were "designed without a thought for privacy or physical security."
Guardian's defense is to be *architecturally incapable* of being that thing.

**Deliberately does NOT:**

- **No continuous GPS breadcrumbing.** Emit zone-crossing *events*, never a location stream. Prefer
  proximity-to-anchor so the device never even computes fine coordinates. GPS-equipped variants ship with
  GPS **off by default**.
- **No audio, no video, ever** — the same rule as the rest of the fleet, no exceptions for "safety."
- **No covert mode.** The device that can't be hidden can't be a stalking tool — Guardian *announces
  itself* (a visible/haptic "I'm on"), the deliberate inverse of AirTag's silence.
- **No remote arming.** Only the **wearer** arms, presses, and cancels. A witness someone *else* can
  turn on from afar is surveillance, full stop.

**Deliberately DOES:**

- **Wearer-owned keys; wearer-chosen destination** — the wearer picks which fleet hears them.
- **Coarse everything** — coarse zones, coarse time buckets, minimal signed claims. Privacy and the
  sealed-log architecture happen to point the same way here.
- **Legible on-body affordance** — the wearer can always see/feel that it's on, and cancel a pending
  alert.

**The line in one sentence:** *a personal safety beacon is a witness the wearer summons about their own
situation and can always silence; a tracking device is a witness someone else summons about the wearer,
which they can't turn off.* Everything Guardian does sits on the first side.

---

## 6 · Firmware shape & power budget

**Default state: deep sleep.** Everything is event-driven — the mental model is a 5-year commercial panic
button, not a smartwatch.

```
BOOT → provision keys, zone anchors, fleet id (flash) → DEEP SLEEP
   ├─ wake: BUTTON (press-and-hold)   → sign "duress @ zone"  → LoRa TX burst ×retry → BLE-phone fallback → haptic → sleep
   ├─ wake: RTC timer (heartbeat)     → sign "OK @ zone" (+ coarse battery) → LoRa TX burst → sleep
   ├─ wake: IMU INT (motion/impact)   → start local "are you OK?" countdown (NOT an auto-claim) → sleep / await cancel
   └─ wake: BLE anchor seen/lost      → sign "entered/left zone Z" event → sleep
```

**Realistic power budget (nRF52840 + SX1262):**

- Deep sleep: **single-digit µA** (nRF52840 with BLE resident) + IMU in low-power motion-wake ≈ a µA or
  two more.
- SX1262 **RX ≈ 4.2 mA**; a TX burst is tens–100+ mA but only for **tens of ms per packet**.
- **Rule of thumb:** kept **TX-mostly** (asleep, a handful of bursts per hour), average draw lands around
  **150–300 µA**, so a **500 mAh** LiPo gives **weeks** and a **1000 mAh** puck clears a week with generous
  heartbeats. Turn LoRa RX **continuously on** and you're at **~1 day per 100 mAh** — which is why RX is
  event-gated, not always-on.
- **Charging:** single-cell LiPo + USB-C (the nRF52840 XIAO's onboard charger). Expose battery voltage as
  a coarse telemetry field so "low battery" ≠ "wearer down" (critical for §3.2).

**Form factor:** a **screenless pendant/pocket puck** is the honest match — fewer things to leak, longest
life, cheapest, and no display to argue with the "coarse only" rule. (A wrist variant is possible later but
costs battery for convenience.)

---

## 7 · Never let it rot

- **Stock Meshtastic transport where possible** — the same upstream-maintained radio stack Fence Guard and
  Car Mode ride; no bespoke LoRa MAC to bit-rot.
- **One canonical Meshtastic doc.** Gateway setup, channel PSK discipline, the adapter's parsing rules, the
  known firmware caveats — all live in [`meshtastic_integration.md`](../meshtastic_integration.md); this
  doc only adds what's specific to a *worn* node.
- **Shared kit lineage with Fence Guard.** Same Wio-SX1262 module, sibling XIAO host — radio facts, antenna
  guidance, and Meshtastic notes are cited from
  [`canary_fence_guard_research.md`](./canary_fence_guard_research.md), not re-derived.
- **Honest tiering.** Every capability above is `concept` until a worn unit is built and carried; the fall
  path stays a *prompt* until (if ever) a personalized on-device model earns a stronger claim.

---

## 8 · Open items before this is more than a concept

- **No bench unit, no worn unit.** Every power and range figure is someone else's measurement (cited),
  not ours on this build.
- **The `duress_asserted` claim kind** isn't in the dictionary yet — adding it touches the kernel vocabulary
  and both HA mirrors (the drift-linter enforces all three stay in sync).
- **BLE-to-phone fallback** (§3.1) needs a companion path defined — the honest belt-and-braces for a
  best-effort mesh, not yet designed.
- **Zone-presence claim mapping** (§4) is an open dictionary decision.
- **nRF52840 firmware** for the witness stack is a real port, not a config change — the flagship duress
  path could start as stock Meshtastic + Detection Sensor Module (button on a GPIO), the same
  zero-custom-code Phase 0 Fence Guard uses, before any bespoke firmware.

---

*Sources for the facts above: LILYGO T-Echo (nRF52840) and T-Watch (ESP32) battery comparisons; Seeed XIAO
+ Wio-SX1262 kit documentation; Meshtastic range/latency/mesh-algorithm field data; peer-reviewed
real-world fall-detection sensitivity and false-alarm studies (PMC12431052, PMC5498034); commercial LoRaWAN
panic-button (TEKTELIC, MOKO) battery specs; and reporting on GPS-tracker/AirTag stalking. Specific links are
collected in the concept card's `sources` block and in
[`canary_fence_guard_research.md`](./canary_fence_guard_research.md) for the shared kit.*
