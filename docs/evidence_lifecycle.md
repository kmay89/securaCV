# Evidence Lifecycle (Plain Language)

This document describes how evidence moves through the Privacy Witness Kernel (PWK) without adding any new capabilities. It is a narrative view of the existing guarantees: events are claims, raw media stays sealed, and access is gated by quorum and receipts.

## 1) Creation (Event claims, not recordings)

- The kernel receives raw sensor input **in memory only**, and modules emit **event claims** that describe what happened in a coarse, non-identifying way.
- Each event is written into the **sealed log**, an append-only, tamper-evident record owned locally.
- Event records include only the contract-approved fields (event type, time bucket, zone ID, confidence, ruleset/kernel identifiers). No precise timestamps, identity fields, or raw media are stored in the event log.

**Ties to existing concepts:** Event Contract; Sealed Log; Metadata minimization.

## 2) Sealing (Commit to a tamper-evident record)

- Once an event is written, it cannot be changed, enriched, or retroactively reinterpreted.
- The sealed log binds events to the ruleset and kernel version active at creation time, preventing reprocessing under newer capabilities.

**Ties to existing concepts:** Sealed Log immutability; No retroactive capability expansion.

## 3) Access (Reading claims, not raw media)

- Normal access is limited to **event claims** via the event API. This is a read-only view of the sealed log.
- The system intentionally lacks APIs for raw playback, streaming, or bulk searching by identity. Review is sequential and context-bound.

**Ties to existing concepts:** Non-queryability by design; No raw export; Contract Enforcer.

## 4) Export (Explicit, receipted disclosure)

- Exports are **explicit artifacts** created by a deliberate act, not continuous feeds.
- Each export is accompanied by a **tamper-evident receipt** that records the disclosure act without expanding the data itself.
- Exported events keep coarse time buckets and zone identifiers; precise timestamps and identity fields remain forbidden.

**Ties to existing concepts:** Export receipts; Local ownership and custody; Metadata minimization.

## 5) Verification (Prove integrity without adding data)

- Verifiers can check that exported events match the sealed log and that the receipt corresponds to a specific disclosure.
- Verification confirms integrity and provenance without revealing raw media or identity data.

**Ties to existing concepts:** Sealed log integrity; Export receipts.

## 6) Dispute (Break-glass access with quorum)

- If a dispute requires raw evidence, the only path is **break-glass by quorum**.
- Break-glass access is auditable and produces receipts for each attempt.
- Raw media, if present in the Evidence Vault, is isolated and cannot be accessed without quorum authorization.

**Ties to existing concepts:** Evidence Vault; Break-glass quorum; Break-glass receipts.

## 7) Vault envelopes (v2 format + crypto modes)

- Vault envelopes use a **versioned header**. v2 carries the AEAD and KEM metadata alongside the sealed payload:
  `version`, `aead_alg`, `nonce`, `aad`, `ciphertext`, `kem_alg`, `kem_ct`, `kdf_info`, and optional `classical_wrap`.
- The **AAD encodes the envelope id and ruleset hash** (no precise timestamps or global identifiers are introduced).
- Each envelope uses a **per-object DEK**. The payload is AEAD-encrypted, and the DEK is protected by:
  - **classical**: local master-key wrap (`classical_wrap`)
  - **pq**: ML-KEM (FIPS 203) encapsulation + KDF-derived DEK
  - **hybrid**: both, enabling classical or PQ recovery
- The policy stored in the kernel database selects the mode via `vault.crypto_mode`.

---

## What Is *Not* Recorded

To make omissions explicit (especially for legal review), the following are **not** recorded in the sealed log or export artifacts:

- Raw frames, audio waveforms, or continuous sensor feeds.
- Precise timestamps (only coarse time buckets are used).
- Absolute locations (no GPS or address data; only local zone IDs).
- Stable identifiers, biometric data, or identity-derived metadata.
- Cross-device or long-term correlation identifiers.
- Free-form notes or annotations that could leak identity or context.

These omissions are structural guarantees, not configuration choices.

## How Gaps Are Represented

- **No data is stored for periods with no qualifying events.** There is no “heartbeat” log of continuous observation.
- **Missing evidence remains explicitly absent**—a gap means the kernel did not produce a conforming event, not that evidence was hidden or suppressed.
- If a break-glass request is denied or fails quorum, the **attempt is still recorded and receipted**; lack of raw media access is visible and auditable.

These representations make the system’s omissions visible and verifiable without adding new data collection pathways.
