# Failure Semantics (Fail-Closed)

The witnessing kernel is **fail-closed**: when a safety or integrity invariant is threatened, the system must stop producing conforming evidence and emit explicit failure events instead. This matches the existing invariant language that serialization and invariant enforcement must “fail closed” (see `spec/invariants.md`).

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
