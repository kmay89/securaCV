# Capture provenance — what the kernel records about how an event was captured

This note is deliverable E of roadmap item 7: what SecuraCV records today
about the path from a sensor to a sealed event, what it deliberately does not
record, and what moving the signature toward the sensor would cost. It
builds nothing. Its tracker is [`maps/capture_provenance.json`](maps/capture_provenance.json),
rendered as [`capture_provenance.md`](capture_provenance.md). It is also the
procedure the Part 11 row 11.10(h) and the BS 10008 row `bs.capture` point
at: the organization validates its own input devices, and this note says
what the kernel gives it to do that with.

## 1. What the kernel records about the capture path today

Per event:

- `attestation` — the input tier. Absent means the event was produced in
  this kernel; `adapter` means an out-of-process adapter produced the claim
  and the kernel signed it at ingest; `ha-bridged` means it also transited
  Home Assistant first (`pub enum Attestation` in `src/lib.rs`; the
  dictionary's `attestation_tiers`);
- `kernel_version`, `ruleset_id` and `ruleset_hash`;
- `zone_id` (`pub struct ExportEvent` in `src/lib.rs`).

Invariant VI binds history to the ruleset label: events sealed under one
ruleset are never reprocessed under another (`pub struct ReprocessGuard`).

**What it does not record:** a camera, a source, an adapter binary, a module
hash or a sandbox profile. This is deliberate. Events must not contain stable
identifiers ([`spec/event_contract.md`](../../spec/event_contract.md) §2), and
the module template forbids device identifiers usable outside the box
([`spec/modules/zone_crossing.md`](../../spec/modules/zone_crossing.md)).

The tier is a recorded label, not a check of the source: the kernel does not
authenticate the camera.

## 2. What `ruleset_hash` binds

`ruleset_hash` is SHA-256 of the ruleset id **string**
(`KernelConfig::ruleset_hash_from_id` in `src/lib.rs`). It binds a label. It
does not bind the rules, a model or a configuration. A configuration changed
without changing its id is caught only procedurally: the operator pack's
[OP-9](BS10008_OPERATOR_PACK.md#op-9-change-control-of-analytics) tells the
operator to change `ruleset_id` whenever the detection configuration
changes.

## 3. No per-file acquisition sheet yet

`court_export` writes no per-file acquisition sheet today (a search of
`src/bin/court_export.rs` for "acquisition" finds nothing), while
[`spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md) §5 lists such
sheets as design. The smallest honest step is item 7 CP3's capture-path
summary: event counts by tier, kernel version and ruleset, taken from the
already-verified bundle ([`VALIDATION_SHEETS.md`](VALIDATION_SHEETS.md)).

## 4. A Canary

The firmware already signs its own witness records on the device, under the
domain `securacv:fw:chain:v1` (`src/crypto/signatures.rs`;
[`spec/witness_api_v1.md`](../../spec/witness_api_v1.md)). The hub does not
verify that signature or carry it into a sealed event: no source-signature
check exists under `src/ingest`, `src/adapter` or `src/relay`.

Sensor-proximate signing would mean the hub verifying a Canary record
against a pinned Canary key before sealing it, and recording a **pin
reference** (the hub roster index), never the key bytes. That is item 3 K's
direction, and the tracker row `canary-record-binding` is `designed` against
it. Its costs:

- a new attestation tier and event field: a format change and a dictionary
  change;
- a stable source identifier, which Invariant III rules out unless the
  reference stays inside the box;
- a source time, which must be dropped to the bucket.

## 5. A Frigate-fed hub

Frigate's detections arrive unsigned over MQTT, so the honest ceiling is the
`adapter` tier. Signing at the camera needs camera firmware (in-camera C2PA).
[`PROVENANCE_INTEROP.md`](../security/PROVENANCE_INTEROP.md) §1.3 records the
industry's lesson: the capture pipeline, not the signature, is the hard part.

## 6. Invariant tensions

- **Invariant II:** a per-sensor key is an identity substrate if it leaves
  the box, and a signature over raw frames is a content-comparable value
  (the tracker row `frame-signing-at-capture` is `not-pursued`).
- **Invariant III:** stable identifiers and precise capture times.
- **Invariant VI:** binding to labels, not rules.

## 7. The tracker

[`capture_provenance.md`](capture_provenance.md), generated from
[`maps/capture_provenance.json`](maps/capture_provenance.json).
