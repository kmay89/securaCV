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
| Power loss | `PowerLoss` | lifecycle records: boot finds a trailing `start` with no `shutdown_clean` | Once per unclean restart. **This is a proxy**: it cannot distinguish power loss from a crash or `kill -9`; the record's details say `unclean_shutdown`. **Hub only:** a Canary logs its reset reason (brownout, panic, watchdog) in the health log and status JSON and seals nothing for it |
| Sensor silent / camera outage | `GapMissingData` | witnessd ingest supervisor: source unhealthy for `ingest.failure_threshold_s` | Once per outage (details: `ingest_stalled backend=<name>`; backend name only, never URLs). Recovery is visible in the next heartbeat's `ingest_healthy=true` |
| Conformance rejection | `GapMissingData` | kernel contract/allowlist/zone-policy enforcement | Once per rejected candidate event |
| Sensor disagreement | `SensorDisagreement` | **Not emitted.** No multi-sensor consensus layer exists; adapters are independent. Deferred until a consensus mechanism lands — emitting it without one would be fabrication |
| Firmware integrity failure | `FirmwareIntegrity` | **Not emitted.** No firmware attestation infrastructure exists on the host kernel. Deferred to the firmware-attestation work |

### Retention under clock faults

Retention ages a sealed row by its `created_at` stamp — the wall clock at the
moment of sealing — so a wall clock that moves is a way to lose evidence that
no adversary needs. Two bounds hold regardless of the wall clock:

- **The prune is a prefix.** The pass finds the first row whose stamp is still
  inside retention and prunes only the rows *before* it. A clock that stepped
  back between two appends leaves a later row with an older stamp; that row
  stops the prefix instead of dragging the live rows before it out with it.
- **A forward step cannot expire what was just sealed.** The store keeps a
  bounded ring of `(monotonic instant, wall time, first rowid)` samples, one
  per 10-minute interval over the last 14 days of this process. When the wall
  clock has run ahead of the monotonic clock across a sample by more than the
  step tolerance (30 s, the clock monitor's default skew tolerance, so every
  step that seals a `ClockSkew` also engages the floor) — an RTC-less hub
  taking its first NTP sync after boot, an operator correcting a clock that
  was years behind — the rows after that sample are
  aged by the monotonic clock, not the stamp: a row appended less than
  `retention − 10 min` ago is never pruned. When the two clocks agree the
  stamps are trusted as before.
- **A declared excursion holds the pass.** `witnessd` skips one retention pass
  after the clock monitor seals a `ClockSkew` record, so pruning resumes on
  the settled clock; a clock that steps every interval holds retention every
  interval (loud, by design — the ClockSkew records say why).

What this does **not** cover: rows sealed by an earlier process have no
monotonic history, so a clock that was already wrong at boot governs them by
stamp alone; and a forward step larger than 14 days outruns the ring. Both
are the host-clock trust assumption, stated rather than hidden.

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

### Physical tamper (bridged)

ESP32 Canary firmware detects enclosure tamper (touch pad), thermal-attack
temperature drift, and camera tamper/blinding. Each is signed into the
device-side witness chain (`RECORD_TAMPER_ALERT`) **and** published once per
event to `securacv/<device_id>/tamper`
(`{"state":"on","confidence":0..1,"kind":"enclosure_tamper"|"temp_drift"|"camera_tamper"}`).
A `tamper_detected` route on the host's `mqtt_sensor` adapter (see
`adapter_host.example.toml`) seals it into the kernel's log as an
`EventType::TamperDetected` event through the standard
`Kernel::append_event_checked` gates. Tamper is an *event* (something the
device witnessed happening to itself), not a `FailureType` — the device is
still functioning when it reports it.

### Known gap (tracked follow-up)

Firmware bulk health counters (battery, heap, subsystem flags in
`SystemHealth`) and BLE lifecycle events remain MQTT-only and are not sealed
into any chain. They are operational telemetry rather than evidence; sealing
them is deliberately deferred until a concrete audit need emerges.
