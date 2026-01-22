# Privacy Witness Kernel — Event Contract
Status: Draft v0.1
Intended Status: Normative
Last Updated: 2026-01-20

## 1. Purpose

This document defines the **only permissible output** of a conforming Privacy Witness Kernel: the Event.

Events are *claims*, not recordings.
They assert that something meaningful occurred without revealing identity, trajectory, or continuous behavior.

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

Each event MUST NOT contain:
- Raw sensor data
- Precise timestamps
- Absolute coordinates
- Stable identifiers
- Free-form text fields

---

## 3. Temporal Granularity

Time MUST be expressed as a **bucket**, not a timestamp.

Recommended defaults:
- Minimum bucket: 5 minutes
- Typical bucket: 10–15 minutes
- Buckets MAY include random jitter when exported

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
