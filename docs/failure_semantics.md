# Failure Semantics (Fail-Closed)

The witnessing kernel is **fail-closed**: when a safety or integrity invariant is threatened, the system must stop producing conforming evidence and emit explicit failure events instead. This matches the existing invariant language that serialization and invariant enforcement must “fail closed” (see [`spec/invariants.md`](../spec/invariants.md)).

## Required Failure Events

The following conditions MUST produce explicit failure events (not silent gaps or partial records):

- **Storage full** or write failure.
- **Cryptographic failure**, including key unavailability or signature/verification errors.
- **Clock desynchronization** beyond allowable tolerance.
- **Sensor disagreement** that exceeds configured consensus thresholds.
- **Power loss** or brownout conditions detected by the platform.
- **Firmware integrity failure**, including signature or attestation mismatch.

## Missing Data is a Gap Artifact

When fail-closed behavior prevents evidence creation, the absence of evidence must be recorded as a **gap artifact**. This keeps missing data explicit and auditable—never ambiguous—and aligns with the documented rule that missing evidence remains explicitly absent rather than hidden or suppressed.

## Implementation Mapping

How each required failure is detected and recorded today. "Once per X" means
the detector latches: one sealed record per condition transition, never one
per frame/loop iteration.

| Required failure | `FailureType` | Detector | Trigger / cadence |
|---|---|---|---|
| Storage full | `StorageFull` | witnessd disk preflight (`statvfs` on the DB filesystem, every `storage.check_interval_s`) | Once per drop below `storage.min_free_mb`; latched until space recovers |
| Storage write failure | `StorageWriteFailed` | `append_event_with_failure_semantics` + retention/heartbeat append fallbacks | Once per failed write, with alarm-table → stderr degradation chain |
| Cryptographic failure | `CryptoFailure` | error classification in the append path | Once per failed signing/verification operation |
| Clock desynchronization | `ClockSkew` | witnessd clock monitor: monotonic-vs-wallclock drift beyond `clock.skew_tolerance_s`, or coarse time-bucket regression | Once per excursion (monitor re-baselines after recording) |
| Power loss | `PowerLoss` | lifecycle records: boot finds a trailing `start` with no `shutdown_clean` | Once per unclean restart. **This is a proxy**: it cannot distinguish power loss from a crash or `kill -9`; the record's details say `unclean_shutdown` |
| Sensor silent / camera outage | `GapMissingData` | witnessd ingest supervisor: source unhealthy for `ingest.failure_threshold_s` | Once per outage (details: `ingest_stalled backend=<name>`; backend name only, never URLs). Recovery is visible in the next heartbeat's `ingest_healthy=true` |
| Conformance rejection | `GapMissingData` | kernel contract/allowlist/zone-policy enforcement | Once per rejected candidate event |
| Sensor disagreement | `SensorDisagreement` | **Not emitted.** No multi-sensor consensus layer exists; adapters are independent. Deferred until a consensus mechanism lands — emitting it without one would be fabrication |
| Firmware integrity failure | `FirmwareIntegrity` | **Not emitted.** No firmware attestation infrastructure exists on the host kernel. Deferred to the firmware-attestation work |

### System trace records (not failures)

- `lifecycle` (`start` / `shutdown_clean`): every daemon start and clean
  shutdown is sealed into the chain, making restarts auditable.
- `heartbeat`: one record per 10-minute bucket with per-bucket counter deltas
  and the ingest-health flag. Anchors the chain tail (deleting the newest
  records becomes a detectable missing-heartbeat gap) and provides the
  timeline auditors use to reconstruct system health.

`log_verify` cross-checks these (stale tail, missing heartbeat buckets,
timestamp regressions, checkpoint back-dating) and reports warnings;
`--strict` makes warnings fail verification.

### Known gap (tracked follow-up)

ESP32 Canary firmware tracks physical-tamper counters, battery, and subsystem
health (`SystemHealth` in `securacv_witness.h`) and publishes BLE lifecycle
events over MQTT — none of which reach a sealed chain. Bridging device-side
tamper/health claims through the adapter contract into the kernel's sealed log
is a planned follow-up; until then, firmware health is observable on MQTT but
not cryptographically witnessed.
