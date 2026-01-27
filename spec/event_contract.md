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

- **Boolean form**: `replicated: true | false`
- **Enum form**: `replication_status: local_only | replicated | unknown`

Derivation guidance:
- The marker SHOULD be derived from export/bridge state (e.g., MQTT bridge connectivity,
  replication queue success, or outbound delivery acknowledgement).
- `replication_status` MUST NOT be used as a proxy for delivery confirmation, recipient identity,
  or audience size.
- Implementations MUST NOT encode raw timestamps or identifiers in this field.
- If status cannot be determined, use `unknown` (enum form) or omit the field.

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

- `vehicle_presence_after_hours`
- `boundary_crossing_object_large`
- `human_presence_in_restricted_zone`
- `object_removed_from_zone`
- `forced_entry_detected`

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
