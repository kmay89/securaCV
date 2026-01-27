# Privacy Witness Kernel — Invariants
Status: Draft v0.1
Intended Status: Foundational Specification
Author: The Witness Project
Last Updated: 2026-01-20

## 1. Purpose

This document defines the **non-negotiable invariants** of the Privacy Witness Kernel (PWK).

These invariants describe what the system **cannot do by construction**.
They are not policies, configurations, or user choices.
Any implementation that violates an invariant is **not a conforming implementation**.

The goal of these invariants is to ensure that a witness system cannot evolve—by accident, pressure, or convenience—into a system of bulk surveillance.

---

## 2. Definitions

- **Kernel**: The minimal trusted computing base responsible for ingestion, decision, sealing, retention enforcement, and export gating.
- **Raw Media**: Sensor data that allows reconstruction of identity, appearance, or continuous movement (e.g., video frames, audio waveforms).
- **Event**: A non-extractive claim produced by the kernel or an approved module.
- **Sealed Log**: An append-only, tamper-evident, locally owned record of events.
- **Evidence Vault**: An optional, separate subsystem for storing raw media under break-glass conditions.
- **Break-Glass**: A deliberate, auditable transition that allows access to sealed evidence under quorum rules.
- **Quorum**: A threshold of independent approvals required to perform a break-glass action.

---

## 3. Invariant I — No Raw Export by Design

**The system cannot export raw media during normal operation.**

- Raw media MAY be processed in-memory.
- Raw media MUST be discarded immediately after processing unless explicitly sealed.
- The kernel MUST NOT expose APIs that stream, mirror, or replay raw media externally.
- Any subsystem capable of raw media storage MUST be isolated behind break-glass gating.
- Derived artifacts that are reversible or approximately reversible to raw media
  (e.g., feature maps, embeddings, high-resolution masks) MUST be treated as raw
  media for the purposes of this invariant.

### 3.1 Pre-Event Buffering (Transient Only)

Pre-event raw media exists only as a short-lived, in-memory ring buffer used for
vault sealing. It is not part of the event log and is never exported without
break-glass authorization.

- **Buffer lifetime:** The pre-roll buffer is bounded by a build-time maximum
  duration and frame count; older frames are evicted once limits are reached.
- **Zeroization/discard:** Evicted or dropped frames must be immediately
  discarded and zeroized in memory to minimize exposure windows.
- **Audit expectations:** There MUST NOT be any non-break-glass export path for
  pre-event frames. Buffer drains for sealing must require a valid break-glass
  token, and ingestion components must not retain frames after handoff.

---

## 4. Invariant II — No Identity Substrate

**The system cannot produce, store, or export global identifiers.**

The kernel and its modules MUST NOT:
- Extract or emit license plate numbers, facial embeddings, biometric identifiers, or stable object identifiers.
- Emit hashes or tokens that remain comparable across devices, locations, or long time horizons.
- Provide query interfaces that accept identity-like selectors.

The system MAY produce short-lived, non-invertible correlation artifacts scoped strictly by time and device (see Event Contract).

---

## 5. Invariant III — Metadata Minimization Is Structural

**Metadata leakage is treated as a first-class attack surface.**

By design, the system:
- Cannot emit precise timestamps externally.
- Cannot emit absolute location data in event records.
- Cannot emit stable device identifiers in shared telemetry.
- Cannot vary network behavior in proportion to event occurrence unless explicitly configured for cover traffic.
In the absence of explicit cover traffic configuration, the system SHOULD batch
and delay exports to reduce event-correlated network signals.

---

## 6. Invariant IV — Local Ownership and Custody

**All authoritative logs and evidence are locally owned.**

- The kernel MUST NOT require centralized custody for correctness or verification.
- Third parties MUST NOT be able to query or index event logs remotely.
- Any export produces a tamper-evident receipt tied to a specific disclosure act.

---

## 7. Invariant V — Break-Glass by Quorum

**Access to sealed evidence requires distributed consent.**

- No single actor, credential, or process can unilaterally access sealed evidence.
- The kernel MUST support quorum-based authorization (e.g., N-of-M trustees).
- Each break-glass event MUST be logged immutably and be externally verifiable.
Quorum approvals MUST originate from independently controlled principals or
devices.
Vault confidentiality MUST rely on distinct, device-local key material or
quorum-derived secrets; identifiers are never treated as protective key
material.

---

## 8. Invariant VI — No Retroactive Capability Expansion

**New capabilities cannot be applied to historical data.**

- Events and evidence are permanently bound to the ruleset active at time of creation.
- A kernel upgrade MAY introduce new modules or claims, but MUST NOT reinterpret or reprocess previously sealed logs.
- Any attempt to reprocess sealed logs under new rules MUST be treated as a conformance violation and MUST fail with an auditable error.
`ruleset_hash` is an identifier for the active ruleset, not a secret, and MUST
NOT be treated as protective key material.

---

## 9. Invariant VII — Non-Queryability by Design

**The system cannot function as a searchable behavioral database.**

- There is no API for retrospective identity search.
- There is no API for bulk historical pattern mining.
- Any inspection of past events occurs through sequential, context-bound review.

---

## 10. Conformance

An implementation claiming conformance to the Privacy Witness Kernel MUST:
- Enforce all invariants in code, not configuration.
- Fail closed when invariants are threatened.
- Document any assumptions explicitly.

Failure to meet any invariant constitutes non-conformance.
