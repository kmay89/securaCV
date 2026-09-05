# Privacy Witness Kernel — Event Contract
Status: Draft v0.1
Intended Status: Normative
Last Updated: 2026-01-20

## 1. Purpose

This document defines the **only permissible output** of a conforming Privacy Witness Kernel: the Event.

Events are *claims*, not recordings.
They assert that something meaningful occurred without revealing identity, trajectory, or continuous behavior.
Events are not intended to support replay, reconstruction, or simulation of past behavior.

---

## 2. Event Structure (Normative)

Each event MUST contain:

- `event_type` (string, from a constrained vocabulary)
- `time_bucket` (coarse, non-precise temporal window)
- `zone_id` (local logical zone, not absolute location)
- `confidence` (normalized float or ordinal class)
- `correlation_token` (optional, ephemeral; see §6)
- `kernel_version`
- `ruleset_id`

Each event MAY contain:
- `replication_status` (enum or boolean marker; see §2.1)

Each event MUST NOT contain:
- Raw sensor data
- Precise timestamps
- Absolute coordinates
- Stable identifiers
- Free-form text fields

Additional constraints:
- `confidence` MUST represent the kernel’s internal assessment of rule satisfaction, not a
  probability of real-world truth.
- `correlation_token`, if present, MUST be short-lived, non-stable, and MUST NOT be usable to
  reconstruct sequences across multiple time buckets.

---

## 2.1 Export Replication & Offline Markers (Normative)

Export artifacts MAY include minimal replication metadata to indicate whether an event was
broadcast/replicated beyond the local system. These markers MUST NOT disclose raw timestamps
and MUST adhere to the same bucket granularity as `time_bucket`.

### 2.1.1 Replication Status

`replication_status` is a boolean or enum marker that indicates export visibility:

- **Boolean form**: `replication_status: true | false`
- **Enum form**: `replication_status: "local_only" | "replicated" | "unknown"`

Derivation guidance:
- The marker SHOULD be derived from export/bridge state (e.g., MQTT bridge connectivity,
  replication queue success, or outbound delivery acknowledgement).
- `replication_status` MUST NOT be used as a proxy for delivery confirmation, recipient identity,
  or audience size.
- Implementations MUST NOT encode raw timestamps or identifiers in this field.
- If status cannot be determined, it MUST be handled as follows: if using the enum form, the
  value MUST be `unknown`; if using the boolean form, the field MUST be omitted.

### 2.1.2 Offline Interval Records

Export artifacts MAY include `offline_intervals`, a list of local-only operation windows that
indicate the system was unable to broadcast/replicate events.

Each interval MUST be expressed as coarse buckets, not raw timestamps, for example:

```json
{
  "offline_intervals": [
    { "start_bucket": "2026-01-20T10:00Z", "end_bucket": "2026-01-20T10:10Z" }
  ]
}
```

Constraints:
- `start_bucket` and `end_bucket` MUST be bucketed (same granularity as §3).
- Intervals MUST NOT imply precise outage times; buckets MAY include jitter in export.
- Intervals SHOULD be derived from bridge status signals (e.g., MQTT disconnected, outbound
  queue paused, or replication pipeline unhealthy).
- Interval records MUST NOT include network identifiers, broker addresses, or per-message IDs.

---

## 3. Temporal Granularity

Time MUST be expressed as a **bucket**, not a timestamp.

Recommended defaults:
- Minimum bucket: 5 minutes
- Typical bucket: 10–15 minutes
- Buckets MAY include random jitter when exported
- Export artifacts MUST preserve bucketed time (no precise timestamps) and SHOULD be batched.

### Conformance-Critical Parameters

Bucket size and export jitter parameters are **conformance-critical**. They MUST NOT be narrowed
(e.g., smaller buckets or reduced jitter) without a ruleset change. The defaults defined in
`TimeBucket::now_10min` and `ExportOptions::default` are normative baselines for conformance.

---

## 4. Spatial Granularity

Events reference space only via **local zone identifiers**.

- Zones are defined by the operator at configuration time.
- Zone identifiers have no meaning outside the local deployment.
- Absolute addresses, GPS coordinates, or map references are forbidden.

---

## 5. Good vs Forbidden Claims (Examples)

### Permitted Claims (Good)

Unlike the forbidden half below, **this list is exhaustive, not a sample.** It
is exactly the `event_type` set of §11's table, mirrored in
`spec/witness_dictionary.json` (`event_types`) and enforced by the kernel —
anything else is refused at `Kernel::append_event_checked` with *"conformance:
event_type not in allowed vocabulary"*. `scripts/lint_dictionary_sync.py`
pins this list, §11's table and the dictionary to each other. Adding a kind
means a ruleset change plus the dictionary and every copy that linter names,
in that order (FR-13). (§10's BLE-discovery records are a separate,
firmware-level family and are not adapter claims.)

- `vehicle_presence_after_hours`
- `boundary_crossing_object_large`
- `boundary_crossing_object_small`
- `acoustic_impulse_in_zone`
- `presence_in_restricted_zone`
- `contact_state_change`
- `object_removed_from_zone`
- `tamper_detected`
- `vehicle_arrival_departure`

> This list carried a tenth entry, `forced_entry_detected`, until 2026-09-05.
> No such kind ever existed — not in the dictionary, not as a `ClaimKind` or
> an `EventType`, not in the integration, the timeline card or the Apple
> vocabulary — so an adapter author following §5 emitted a claim the kernel
> refused. It was also the wrong shape for this contract twice over: "forced"
> is an inference about intent, and "entry" names an act rather than an
> observation. The observable fact is a `contact_state_change` (a door or
> window opened) or a `tamper_detected` (the device itself was interfered
> with); what it *meant* is the operator's call, not the witness's.

### Forbidden Claims (Non-Conforming)

- `license_plate_detected`
- `person_identified`
- `same_vehicle_as_yesterday`
- `vehicle_seen_at_multiple_locations`
- `face_match_score`

---

## 6. Ephemeral Correlation Tokens

The system MAY emit a `correlation_token` to support short-term reasoning such as:
> “This is likely the same object as a moment ago.”

Constraints:
- Tokens MUST be derived from non-invertible features.
- Tokens MUST be scoped to a single device.
- Tokens MUST expire within a short window (e.g., ≤15 minutes).
- Tokens MUST rotate automatically.
- Tokens MUST be derived using a key that rotates per time-bucket and is destroyed after the bucket expires.
- Tokens MUST NOT be comparable across devices or time windows.

Formally:
> A correlation token MUST NOT function as an identifier.

---

## 7. Required vs Forbidden Metadata

### Required
- Kernel version
- Ruleset identifier
- Event type
- Coarse time bucket
- Zone ID
- Confidence estimate

### Forbidden
- Network identifiers
- Operator identifiers
- User account references
- External correlation hints
- Sequential event numbers that imply continuity

---

## 8. Event Immutability

Once written to the sealed log:
- Events cannot be modified.
- Events cannot be reclassified.
- Events cannot be enriched retroactively.

---

## 9. Forward Compatibility

New event types MAY be introduced via new rulesets.
Older events remain governed by their original contract and semantics.
Backward reinterpretation is forbidden.

---

## 10. BLE Discovery Semantic Events

The following semantic events are emitted by the BLE Discovery subsystem
(Opera/Chirp/Nearby) and fed into the witness chain as new records:

| Event Type | Category | Description |
|-----------|----------|-------------|
| `ble_initialized` | BLUETOOTH | BLE subsystem started successfully |
| `ble_init_failed` | BLUETOOTH | BLE failed to start (no antenna, hardware fault) |
| `ble_client_connected` | BLUETOOTH | A BLE client connected to Opera server |
| `ble_client_disconnected` | BLUETOOTH | A BLE client disconnected from Opera server |
| `chirp_sent` | BLUETOOTH | A chirp broadcast was sent (includes type) |
| `chirp_received` | BLUETOOTH | A chirp was received from another Canary |
| `canary_discovered` | BLUETOOTH | A new Canary device appeared in scan |
| `canary_lost` | BLUETOOTH | A previously visible Canary dropped off |

### Privacy Constraints for BLE Events

- BLE events MUST NOT include MAC addresses or stable hardware identifiers
- Device identification uses truncated Ed25519 pubkey hash only
- Non-Canary device counts are aggregate only (no individual entries)
- Chirp received events include the sender's device ID prefix (from pubkey hash)
- RSSI values MAY be included for proximity context but MUST NOT be stored
  with enough precision to enable tracking

---

## 11. Sensor Adapter Event Vocabulary

The following `event_type` values are emitted by vendor-neutral sensor adapters
(see `spec/sensor_adapter_contract_v0.md`) and fed into the witness chain as new records.
They are coarse, non-identifying claims and obey every constraint above.

| Event Type | Description |
|-----------|-------------|
| `boundary_crossing_object_large` | A vehicle-sized object crossed a zone boundary |
| `boundary_crossing_object_small` | A small object (animal, package, bicycle) crossed a zone boundary |
| `acoustic_impulse_in_zone` | A coarse acoustic impulse was sensed in a zone (no waveform, no direction) |
| `presence_in_restricted_zone` | A presence was sensed in an operator-designated restricted zone (no identity) |
| `vehicle_presence_after_hours` | A vehicle-sized presence during operator-configured "after hours" (no plate/make/model) |
| `contact_state_change` | A binary contact/open-close change (door, gate, window, enclosure) |
| `object_removed_from_zone` | An object previously present in a zone is no longer present |
| `tamper_detected` | Tampering with the witnessing device itself: enclosure opened, camera covered/blinded, or thermal-attack temperature drift. The zone names the device location, never the actor |
| `vehicle_arrival_departure` | A vehicle arrived at or departed a zone (e.g. ignition on/off sensed passively off a vehicle's own CAN bus). No plate, make, model, trip data, or GPS trail — a binary state change scoped to a zone |

### Claim → Event Flow

Adapters do not emit events directly. They emit a narrower, pre-sanitized `Claim` (kind +
raw zone label + confidence). The trusted Adapter Host stamps a coarse `TimeBucket`, sanitizes
the zone label into a `zone_id`, maps the claim kind to one of the event types above, and submits
the result through the **same** `Kernel::append_event_checked` choke point used by every other
producer. The kernel's contract enforcement is authoritative; an adapter cannot bypass it.

New adapter event types MAY be added only via a ruleset change and only if they remain coarse and
non-identifying. Forbidden claims (§5) remain forbidden regardless of source.

---

## 12. System Trace Records (Normative)

The sealed log additionally carries two **system trace** record types. They are
NOT Events: they describe the witnessing system itself, never the observed
scene, and they are excluded from event exports.

| `record_type` | Cadence | Purpose |
|---|---|---|
| `heartbeat` | One per 10-minute time bucket while the daemon runs | Anchors the tail of the hash chain (tail truncation becomes a detectable missing-heartbeat gap) and records coarse system health |
| `lifecycle` | One `start` per daemon boot; one `shutdown_clean` per deliberate stop | Makes restarts auditable; a boot that finds a trailing `start` seals a `PowerLoss` failure record (unclean-shutdown proxy) |

### Permitted content

- Coarse `time_bucket` (same granularity rules as §3).
- Kernel version, ruleset identifier, ruleset hash (same binding as Events).
- `heartbeat` only: a boolean ingest-health flag and **per-bucket aggregate
  deltas** (frames captured, events appended, failure records appended).
- `lifecycle` only: the phase (`start` / `shutdown_clean`).

### Forbidden content

Everything §7 forbids, plus:

- Cumulative counters or sequence numbers spanning buckets (deltas only —
  cumulative values imply continuity across the retention boundary).
- Source URLs, device paths, or any network identifier (failure details may
  name the backend kind, e.g. `rtsp`, never the endpoint).

Retention and pruning apply to system trace records exactly as to Events.
Verifiers MAY use heartbeat cadence and lifecycle pairing to flag missing
buckets, stale tails, and timestamp regressions; such findings are warnings
about coverage, not hash-chain failures.

### On-Device Vision Contributions

The Canary firmware's on-device vision cascade (`securacv_vision`) contributes coarse witness
records for camera **motion**, **person presence**, **camera tamper/blinding**, and
**object removal** — each carrying only a coarse `time_bucket` and a local zone, never frames,
boxes, or identity. These reuse existing semantics: object removal corresponds to
`object_removed_from_zone` (already listed above); camera tamper is recorded device-side under
the firmware's tamper-alert record class and bridged to the kernel's sealed log as
`tamper_detected` (listed above) via the MQTT adapter route on `securacv/<device_id>/tamper`.
The vision module emits at most one record per detected transition (single-fire latches), so it
cannot flood the chain.
