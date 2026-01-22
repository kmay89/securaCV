# Module Spec: zone_crossing
Status: Draft v0.1
Type: Conforming Module Template
Last Updated: 2026-01-20

## 1. Purpose
`zone_crossing` detects when an object crosses from outside a configured boundary into a restricted zone.

It is intentionally minimal:
- It is the “Hello, World” of conforming witness modules.
- It demonstrates the event contract and forbidden outputs.

## 2. Inputs
The module receives:
- Frame handle (read-only)
- Zone mask (local logical zone geometry)
- Time bucket (coarse, supplied by kernel)
- Sensor class (camera type, lens role; no unique IDs)

The module MUST NOT receive:
- precise timestamps
- device identifiers usable outside the box
- location/GPS/address
- persistence handles
- network access

## 3. Outputs
Permitted event types (examples):
- `boundary_crossing_object_large`
- `boundary_crossing_object_small`
- `human_presence_in_restricted_zone` (only if implemented without identity)

Required fields:
- `event_type`
- `time_bucket`
- `zone_id`
- `confidence`
- `kernel_version`
- `ruleset_id`

Optional:
- `correlation_token` (ephemeral, bucket-scoped; see Event Contract §6)

Forbidden:
- raw media
- plate strings
- face embeddings
- stable object IDs
- precise timestamps
- GPS/address

## 4. Detection approach (MVP)
MVP is allowed to be simple:
- background subtraction / frame differencing
- zone mask intersection
- blob size threshold (large vs small)

The first goal is conformance, not accuracy.

## 5. Privacy constraints
- Never export pixel data
- Never persist frames
- Never emit “same object as yesterday”
- Correlation tokens (if used) must be derived via kernel-provided bucket key and must not survive the time bucket.

## 6. Conformance tests
The module MUST be tested to prove it cannot:
- emit forbidden fields
- emit event types outside the allowlist
- produce correlation tokens that compare across buckets
