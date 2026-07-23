# Canary Car Mode — a stock Meshtastic node in your car's USB port

**Status:** concept — real, tested Rust support (`ClaimKind::VehicleArrivalDeparture` is now
allowed through `src/adapter/meshtastic.rs`), zero custom firmware or circuitry required, **no
bench validation yet**. Same honesty tier as every other unbuilt hardware idea in this repo: the
design is sound and sourced, nothing has touched a real car.

**The one-sentence version:** plug a stock, unmodified [Meshtastic](https://meshtastic.org) node
into your car's ignition-switched USB port. It boots when the car does, tells the mesh "I'm here,"
and goes silent — with nothing left to configure wrong — the instant the car turns off.

---

## 1 · Why this needs no firmware and no circuit

Every other Canary variant in this repo trades off some combination of custom firmware, custom
wiring, or bench-unverified timing. Car Mode doesn't have to, because of one structural fact:
**the node's own power supply already *is* the ignition signal.**

- The node is powered directly from the car's ignition/ACC-switched USB port — not a battery, not
  a separate sensing circuit off the CAN bus. When the car is off, the node has no power, full
  stop.
- Its Detection Sensor Module — [`meshtastic_integration.md`](../meshtastic_integration.md)'s
  already-shipped inbound path — needs a GPIO to monitor. Configure it with `use_pullup = true`,
  leave the pin **unconnected**: it reads HIGH the instant the chip has power, with no external
  resistor, no soldering, nothing to wire wrong. "The node exists" becomes "the monitored pin is
  triggered," for free.
- Result: **zero custom firmware** (stock Meshtastic, actively maintained upstream — it cannot rot
  the way a bespoke sketch would) and **zero custom circuitry** (one off-the-shelf USB cable). The
  entire "build" is a config screen and a cable.

This is a genuine repurposing of the Detection Sensor Module — it's designed for a real switch
that can go either way, not a permanently-pulled-high "I exist" flag — so it's marked concept, not
confirmed, until someone bench-tests that the first boot-time transition actually fires a
detection alert the way a real switch closing would.

---

## 2 · Arrival: the easy half

On boot, the pinned-high GPIO reads as a fresh not-triggered→triggered transition — the Detection
Sensor Module's normal "something happened" case — and fires a detection alert immediately (or
within `minimum_broadcast_interval`, tunable; see §4's firmware-behavior caveat). Configure
`state_broadcast_interval` (e.g. every 120 s) so the node also re-affirms "still running"
periodically for the whole drive — cheap insurance against a missed single packet, and it's what
makes departure detection possible at all (§3).

`src/adapter/meshtastic.rs` already turns an active detection or an active heartbeat into a
[`vehicle_arrival_departure`](../spec/event_contract.md) claim — the exact same claim kind
[Canary Vehicle](./canary_vehicle_can.md)'s CAN adapter emits, so the rest of your fleet reacts
identically regardless of which sensing method produced it.

---

## 3 · Departure: inferred from silence, on purpose

This is the design decision worth explaining, because the obvious approach is wrong. The obvious
idea — detect the GPIO going LOW as the car turns off, fire one last packet before the node
dies — was the original plan for [Canary Vehicle's CAN adapter](./canary_vehicle_profiles.md), and
it doesn't survive contact with real Meshtastic firmware:

- [meshtastic/firmware#8977](https://github.com/meshtastic/firmware/issues/8977): the Detection
  Sensor Module has shipped with a documented bug where it "repeatedly sends ON and does not send
  OFF." Building a departure signal on top of an open bug in someone else's firmware is exactly
  the kind of thing that looks like it works on the bench and silently fails in the field.
- Even setting the bug aside: the node loses power the same instant the transition would need to
  fire. There's no time budget for "detect the drop, then transmit" without a supercap holding the
  rail up — timing-critical hardware this design deliberately avoids (see §1).

**The fix: don't try to detect departure at all — infer it from silence.** The node sends active
heartbeats every `state_broadcast_interval` while it has power (§2); `state_broadcast_state()`
(the adapter code already shipped for every Meshtastic-sourced claim, not just this one) already
never asserts on an inactive state — presence in this codebase has always meant "an active signal
keeps arriving," never "an explicit inactive signal announced it stopped." Silence for longer than
a couple of heartbeat intervals means the car lost power, full stop, no separate mechanism needed.

**What this gets you today, no new code:** set a Home Assistant presence/`binary_sensor`'s
availability timeout to ~2× your `state_broadcast_interval`. The entity goes "away" when the
heartbeats stop — the same pattern HA already uses for any presence integration. That's a complete
arrival/departure story with the adapter as it ships right now.

**What it doesn't get you: a departure event in the *sealed* witness log.** HA's inferred
"away" state is real and useful, but it's not a signed claim in the tamper-evident chain the way
the arrival packet is. Getting that would mean a stale-node watchdog — something in
`AdapterHost` or the meshtastic adapter that tracks last-active-per-node and emits one
`ObjectRemovedFromZone` claim after a configured silence window. That's a real, scoped, buildable
follow-up (not vaporware — the shape is clear), just not built yet. Flag it if you want it; it's a
clean addition to an already-tested adapter, not a redesign.

---

## 4 · Setup

1. **Read [`meshtastic_integration.md`](../meshtastic_integration.md) first** — this doc doesn't
   repeat how Meshtastic/the gateway/the adapter work in general, only what's specific to Car
   Mode. You need a gateway node at home (MQTT module, `mqtt.enabled = true`,
   `mqtt.json_enabled = true`) before any of this does anything.
2. **Hardware:** the same kit already researched for
   [Canary Fence Guard](./canary_fence_guard_research.md) — Seeed XIAO ESP32S3 + Wio-SX1262 kit,
   SKU 102010611, ~$9.90, Meshtastic pre-flashed on units shipped after Oct 2024. No case needed
   for a glovebox/console deployment; the case-kit variant (SKU 113110064) if you want one.
3. **Check your car's USB port is actually ignition-switched** before trusting any of this — not
   all are. Many vehicles keep at least one USB port "always hot" for phone charging even with the
   engine off, specifically because owners expect that. Test it: plug in any USB device, turn the
   car off, wait a few minutes (some vehicles have a delayed cutoff, not instant), and see if it
   loses power. If your only ports are always-hot, this design doesn't work on this vehicle without
   finding a genuinely switched circuit (a wired ACC-tap add-a-fuse, out of scope for "no
   soldering," but a legitimate fallback).
4. **Join the private channel** — same non-default PSK discipline as every Meshtastic node in
   this repo; see `meshtastic_integration.md`'s node-side runbook §1.
5. **Configure the Detection Sensor Module:**
   - `enabled = true`
   - `monitor_pin` — any free GPIO not claimed by the radio's B2B connector (D0–D3 are free per
     [Fence Guard's pin research](./canary_fence_guard_research.md#4)); leave it physically
     unconnected.
   - `use_pullup = true`, `detection_triggered_high = true`
   - `name = "Car Mode"` (or similar — used only for matching if you ever fall back to the
     `text`-frame path; not required for the primary `detection` path)
   - `minimum_broadcast_interval` — tune for responsiveness vs. spam; a car doesn't arrive/depart
     rapidly, so 10-30 s is reasonable (contrast Fence Guard's 45 s+ spam-guard floor for a
     twitchier vibration sensor)
   - `state_broadcast_interval` — e.g. 120 s; this is what makes departure inference (§3) work at
     all — don't leave it at 0 (change-only)
6. **`adapter_host.toml`:** one `[[adapter.node]]` block,
   `kind = "vehicle_arrival_departure"` — worked example in
   [`adapter_host.example.toml`](../../adapter_host.example.toml).
7. **Home Assistant:** set the resulting presence entity's availability timeout per §3.

---

## 5 · Never let it rot

- **No custom firmware, ever.** Stock Meshtastic gets upstream's own maintenance, bug fixes, and
  security patches — including, eventually, a fix for the OFF-transition bug this design was
  written to route around entirely rather than depend on.
- **No custom circuit to go stale, drift, or need re-tuning.** One resistor's worth of complexity
  (zero, actually — the internal pull-up needs none) can't rot.
- **One canonical Meshtastic doc.** Every mechanic — gateway setup, channel PSK discipline, the
  adapter's parsing rules, the firmware-bug caveat — lives in
  [`meshtastic_integration.md`](../meshtastic_integration.md); this doc only adds what's specific
  to reading a car's power state through it. Fence Guard and Car Mode both point at the same
  source instead of each carrying their own copy to drift out of sync.
- **Same hardware research as Fence Guard, not a second dossier.** Kit specs, pin map, and
  Meshtastic firmware notes are cited from
  [`canary_fence_guard_research.md`](./canary_fence_guard_research.md), not re-derived.
