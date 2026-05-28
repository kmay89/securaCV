# Witness Mesh OS — A Vendor-Neutral, Rights-Respecting Unified Sensing Platform
Status: Draft v0.1
Intended Status: Informative (Positioning) + pointers to Normative specs
Last Updated: 2026-05-28

## 1. Why this exists

Commercial "Real-Time Crime Center" (RTCC) surveillance platforms market a "single pane of glass"
that unifies many systems — license-plate readers, video walls, drones, gunshot detection,
CAD/RMS/911, and many third-party vendors. Their power comes from a **central, queryable,
identity-centric data model**: plate/face search, cross-location person/vehicle linking, bulk
historical mining, and cloud custody. That model is what enables mass surveillance and what locks
customers into a proprietary data warehouse.

SecuraCV takes the **useful** part of that idea — broad, vendor-neutral integration of many
sensors into one local view — and **discards the rights-violating data model**. The result is a
*witness mesh*, not a surveillance database: it out-integrates the surveillance vendors on breadth
**because** it refuses the identity substrate that creates both the harm and the lock-in. With no
plate/face database to host, there is no proprietary moat, so any sensor that can emit a coarse
signal plugs in for free.

This document is the positioning + capability map. The binding rules are in
`spec/invariants.md`, `spec/event_contract.md`, and `spec/sensor_adapter_contract_v0.md`.

## 2. Who it is for

City councils, hospitals, schools, and small businesses that want **harm-reduction sensing without
surveillance**, using **equipment they already own or can cheaply source**, with **no vendor
lock-in** and **no per-seat cloud subscription**.

## 3. How breadth is achieved without new privilege

A single open contract — the [Sensor Adapter Contract](sensor_adapter_contract_v0.md) —
generalizes the existing `frigate_bridge` pattern. Any source becomes a witness source by emitting
coarse `Claim`s into the same unchanged `Kernel::append_event_checked` choke point. Adapters run
outside the trusted kernel and hold no privileged path; the kernel's three gates remain the
security boundary.

The "single pane of glass" is the **existing** read-only Event API plus the Home Assistant
integration. We add *producers*, never *query surface*: there is no new dashboard to build,
deploy, or secure, and no retrospective search is introduced.

## 4. Surveillance capability → securaCV equivalent

The pattern: keep the *harm-reduction signal*, drop the *identity / tracking / query*.

| Commercial RTCC capability | securaCV equivalent | Invariant forbidding the abusive version |
|---|---|---|
| ALPR plate read + plate search | **FORBIDDEN** → `vehicle_presence_after_hours` claim (no plate) | II, VII |
| "Vehicle seen at multiple locations" cross-camera linking | **FORBIDDEN** (no cross-zone/device linking) | II, VII |
| Face recognition / person search | **FORBIDDEN** → `presence_in_restricted_zone` (no identity) | II |
| Acoustic/gunshot detection + map pin (precise time/GPS) | `acoustic_impulse_in_zone` — coarse bucket, logical zone, no GPS/waveform | I, III |
| Drone live video feed | Drone-as-presence-sensor → `presence_in_restricted_zone`; raw video never crosses `Claim` | I |
| Live video wall / streaming pane of glass | Read-only **claims** pane (Event API + HA) — claims, not streams | I, VII |
| Bulk historical query / "search last 30 days" | **FORBIDDEN** — sequential, bounded-window review only | VII |
| Cloud custody / vendor data warehouse | Local sealed log, local keys, no remote indexing | IV |
| Door/contact/PIR alarm integration | `contact_state_change` / `presence_in_restricted_zone` (coarse) | allowed |
| BLE / device tracking | Existing BLE events (truncated pubkey, no MAC) per `event_contract.md §10` | II, III |
| Real-time per-event push to dispatch | **Batched** claim export only (HA notifications) | III; architecture.md §5 rejection-list #7 |
| CAD/RMS/911 enrichment | Out of scope — requires identity correlation | II, VII |
| Persistent object re-ID across days | Ephemeral `correlation_token` ≤15 min, per-device, key destroyed per bucket | II; `event_contract.md §6` |

## 5. Keeping it free / DIY / simple

- **Config-driven, zero-code adapters.** An operator adds a sensor by editing TOML
  (`adapter_host.example.toml`), not by writing code. A ~$15 PIR sensor plus an ESP32 publishing
  MQTT (firmware already in `firmware/`) becomes a witness source.
- **One binary, many adapters.** `adapter_host` registers every configured adapter and runs one
  shared host loop — operators run a single daemon, not N bespoke bridges.
- **No new heavy dependencies.** The reference adapters ride on `rumqttc`, already a dependency;
  ML stays optional behind `backend-tract`; there are no cloud SDKs.
- **Reuse existing gear.** Frigate (free NVR), ESP32, any MQTT-speaking sensor, cheap PIR/contact
  switches; ALPR cameras can be *down-reduced* to presence rather than discarded.
- **The pane of glass already exists.** Home Assistant (free) + the read-only Event API.

## 6. What this is NOT (guardrails)

The PR-rejection list in `kernel/architecture.md §5` still binds. An adapter that wants raw
frames, precise time, cloud egress, identity output, cross-location linking, or a new query
selector is building a different system and is non-conforming. The framework adds **breadth of
producers, never new privilege**. "Do it all" here means *integrate everything, surveil nothing.*

## 7. Reference implementation & specs

- Normative contract: `spec/sensor_adapter_contract_v0.md`
- Code: `src/adapter/` (`contract.rs`, `registry.rs`, `host.rs`, `frigate.rs`, `mqtt_sensor.rs`),
  binary `src/bin/adapter_host.rs`, example config `adapter_host.example.toml`.
- Conformance tests: `tests/adapter_contract.rs`, `tests/adapter_cannot_bypass_enforcer.rs`.
