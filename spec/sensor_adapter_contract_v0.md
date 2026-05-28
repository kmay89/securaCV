# Privacy Witness Kernel — Sensor Adapter Contract
Status: Draft v0.1
Intended Status: Normative
Last Updated: 2026-05-28

## 1. Purpose

This document defines the **Sensor Adapter Contract**: the open, vendor-neutral interface by which
any external sensing source feeds claims into a conforming Privacy Witness Kernel (PWK).

It generalizes the one-off `frigate_bridge` pattern (parse an external source → strip identity →
coarsen → map to a kernel event → seal) so that *many* sources — acoustic/impulse sensors,
PIR/contact switches, drones reduced to presence, ALPR cameras down-reduced to non-identifying
presence, BLE gateways, other NVRs, generic MQTT/webhook devices — can plug in **without granting
any new privileged path** and **without vendor lock-in**.

This contract is binding on adapters. It does not relax any invariant in `spec/invariants.md`; it
constrains adapters so the invariants hold structurally at the integration boundary.

---

## 2. The `Claim` Type (Normative)

An adapter's only output is a list of **Claims**. A `Claim` is deliberately *narrower* than the
kernel's `CandidateEvent` so that an adapter cannot express a forbidden field even by mistake.

Each `Claim` MUST contain exactly:

- `kind` — a value from the closed [`ClaimKind`](#6-claimkind--eventtype-mapping-normative) vocabulary.
- `zone_label` — a raw, human-friendly local label (e.g. `"Front Door"`). The host sanitizes it.
- `confidence` — a float in `0.0..=1.0`. Passed through to the kernel, which bounds-checks it.

Each `Claim` MAY contain:

- `dedup_hint` — an opaque string used **only** for in-bucket deduplication.

A `Claim` MUST NOT contain (and the type provides no field for):

- Raw sensor data, media, waveforms, images, feature maps, or embeddings.
- Any timestamp. The host stamps a coarse `TimeBucket`; the kernel re-coarsens to 10 minutes.
- A pre-built `zone:` identifier, absolute coordinates, or addresses.
- Stable identifiers (plates, faces, MACs, device serials) or correlation tokens.
- Free-form descriptive text.

`dedup_hint` MUST NOT be written to the sealed log, included in any export, or used as anything
other than a transient deduplication key. It MUST NOT carry identity or precise time.

---

## 3. The `SensorAdapter` Trait (Normative)

An adapter implements a single behavior: produce a batch of pre-sanitized `Claim`s on demand.

```
trait SensorAdapter {
    fn name(&self) -> &'static str;
    fn descriptor(&self) -> &'static AdapterDescriptor;
    fn poll(&mut self) -> Result<Vec<Claim>>;   // no disk / network / log; no raw media
    fn warm_up(&mut self) -> Result<()> { Ok(()) }
}
```

Conformance requirements for `poll`:

- MUST return only `Claim`s permitted by the adapter's `AdapterDescriptor`.
- MUST NOT block indefinitely; returning an empty batch is normal.
- MUST NOT write to disk, open network sockets, or write to the sealed log. Any I/O an adapter
  needs (e.g. receiving MQTT) MUST be performed by an external feeder, not by the adapter holding
  a privileged handle. Reference adapters receive `(topic, payload)` pairs over an in-process
  channel and own no connection themselves.
- MUST NOT retain raw media after constructing a `Claim`; anything reversible to raw media MUST be
  discarded before `poll` returns (Invariant I).

### 3.1 `AdapterDescriptor`

A static descriptor, mirroring `ModuleDescriptor`:

```
struct AdapterDescriptor {
    id: &'static str,
    allowed_claim_kinds: &'static [ClaimKind],
    allowed_event_types: &'static [EventType],
    requested_capabilities: &'static [ModuleCapability],
}
```

- `allowed_claim_kinds` and `allowed_event_types` MUST be consistent: every kind in the former
  MUST map (via §6) into the latter.
- An adapter requesting `Filesystem` or `Network` capabilities MUST be refused by the host, exactly
  as `CapabilityBoundaryRuntime` refuses such modules.
- The host translates `AdapterDescriptor` into a `ModuleDescriptor` so the **unchanged** kernel
  allowlist gate (`enforce_module_event_allowlist`) governs adapter output identically to module
  output.

---

## 4. The Adapter Boundary: Audit, Not Security (Normative)

The `SensorAdapter` trait is an **AUDIT BOUNDARY**, not a security boundary — identical in spirit
to `detect::backend::DetectorBackend`. Adapters run **outside the kernel TCB** and are treated by
the threat model as careless or malicious.

The **security boundary** is, and remains, the three fail-closed gates inside
`Kernel::append_event_checked`:

1. **Event-type allowlist** — the adapter (via its `ModuleDescriptor`) may emit only its declared
   `EventType`s; anything else is rejected and a `FailureEvent` is recorded.
2. **Contract Enforcer** — confidence bounds, 10-minute time-bucket coarsening, strict
   `^zone:[a-z0-9_-]{1,64}$` zone allowlist, correlation-token constraints.
3. **Zone policy** — operator-designated sensitive zones are rejected.

The host (the trusted orchestrator) provides only convenience plumbing — bucketing, dedup, zone
sanitization — lifted out of `frigate_bridge` so every adapter shares it. **The host exposes no
method that writes an event to the log bypassing the Contract Enforcer.** A claim is the *only*
egress, and every claim passes all three gates.

---

## 5. Per-Invariant Preservation at the Adapter Boundary (Normative)

| Invariant | How it holds at the adapter boundary |
|-----------|--------------------------------------|
| **I — No Raw Export** | `Claim` has no byte/blob/image field; raw media is structurally incapable of crossing. Adapters that touch media MUST discard it before producing a `Claim`. |
| **II — No Identity Substrate** | `ClaimKind` is a closed vocabulary with no plate/face/embedding/stable-ID variant; `zone_label` is forced through `sanitize_zone_name` + the kernel's zone regex; no correlation token is adapter-settable. |
| **III — Metadata Minimization** | Adapters cannot set time; the host stamps a coarse bucket and the enforcer re-coarsens to 10 min. No network/device identifiers are representable in `Claim`. |
| **IV — Local Ownership** | Adapters never open the database; only the host's single `Kernel` handle writes. No remote query path is added. |
| **V — Break-Glass by Quorum** | Untouched; adapters produce only claims, never vault material. |
| **VI — No Retroactive Expansion** | Each adapter writes under the *current* `ruleset_id`/`ruleset_hash`. New adapters = new ruleset, never reprocessing of sealed logs. |
| **VII — Non-Queryability** | Adapters add *producers*, never *query surface*. Review remains the existing read-only, bounded-window Event API. |

---

## 6. `ClaimKind` → `EventType` Mapping (Normative)

The mapping lives in the trusted host (`adapter::contract::claim_kind_to_event_type`), never in an
adapter. The vocabulary is closed and coarse:

| `ClaimKind` | `EventType` |
|-------------|-------------|
| `LargeObjectBoundaryCrossing` | `BoundaryCrossingObjectLarge` |
| `SmallObjectBoundaryCrossing` | `BoundaryCrossingObjectSmall` |
| `AcousticImpulseInZone` | `AcousticImpulseInZone` |
| `PresenceInRestrictedZone` | `PresenceInRestrictedZone` |
| `VehiclePresenceAfterHours` | `VehiclePresenceAfterHours` |
| `ContactStateChange` | `ContactStateChange` |
| `ObjectRemovedFromZone` | `ObjectRemovedFromZone` |

New kinds MAY be introduced only via a ruleset change and only if they remain coarse,
non-identifying claims (see `spec/event_contract.md §5` for permitted vs forbidden claims).

---

## 7. Conformance

An adapter is conforming only if:

- Every claim it produces passes `Kernel::append_event_checked` (i.e. it never relies on a
  privileged write path — none exists).
- It declares an `AdapterDescriptor` whose `allowed_claim_kinds`/`allowed_event_types` are
  consistent and minimal for its purpose.
- It requests no forbidden capabilities.
- It never retains, emits, or logs raw media, identity, or precise time/location.

The reference implementation lives in `src/adapter/` (`contract.rs`, `registry.rs`, `host.rs`,
`frigate.rs`, `mqtt_sensor.rs`) with conformance tests in `tests/adapter_contract.rs` and
`tests/adapter_cannot_bypass_enforcer.rs`. The positioning and capability-mapping rationale is in
`spec/witness_mesh_os_v0.md`.
