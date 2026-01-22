# Privacy Witness Kernel — Threat Model
Status: Draft v0.1
Intended Status: Informative but Binding
Last Updated: 2026-01-20

## 1. Design Philosophy

The Privacy Witness Kernel is **adversarial-first**, not feature-first.

This document enumerates the threats the system is explicitly designed to resist.
Threats not listed here are out of scope.

---

## 2. Adversaries

### 2.1 Bulk Government Access
Threat:
- Mass querying, fishing expeditions, warrantless pattern analysis.

Mitigations:
- No centralized access
- No identity substrate
- Break-glass by quorum
- Non-queryable logs

### 2.2 Wholesale Aggregators
Threat:
- Purchase or consolidation of behavioral data across deployments.

Mitigations:
- Local ownership
- No global identifiers
- No exportable raw feeds
- Event semantics prevent resale value

### 2.3 Insider Exfiltration
Threat:
- Authorized users abusing access.

Mitigations:
- Quorum requirements
- Immutable audit logs
- Explicit break-glass receipts

### 2.4 Gradual Feature Creep
Threat:
- “Just one more feature” leading to surveillance.

Mitigations:
- Hard invariants
- No retroactive capability expansion
- Constrained event vocabulary

### 2.5 Anonymous Metadata Correlation
Threat:
- Reconstruction of movement or identity via timestamps, network traffic, or zone correlation.

Mitigations:
- Coarse time buckets
- Jitter
- Optional cover traffic
- Event export batching (multiple events MAY be exported together to prevent single-event timing analysis)
- No cross-device comparability

---

## 3. Non-Goals

The system does NOT attempt to:
- Prevent all inference by external observers
- Replace legal or social accountability
- Function as a general-purpose analytics platform

It exists to **raise the structural cost of abuse beyond viability**.

---

## 4. Security Posture Summary

The kernel assumes:
- The network is hostile
- The cloud is curious
- Users may be coerced
- Future maintainers may be tempted

The design responds by making certain abuses **structurally impossible**, not merely disallowed.
