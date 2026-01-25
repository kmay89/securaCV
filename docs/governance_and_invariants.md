# Governance and Invariants

Status: Draft v0.1  
Intended Status: Normative (Governance)  
Last Updated: 2026-01-20

## Purpose

This document defines how the Privacy Witness Kernel protects its constitutional
invariants against erosion. The governance process exists to make weakening
changes *impossible by default* and *auditable when proposed*. This is not a
policy overlay; it is a structural safeguard in service of the system’s
non-negotiable guarantees.

This document is binding alongside:
- `spec/invariants.md`
- `spec/event_contract.md`
- `spec/threat_model.md`
- `kernel/architecture.md`

Any change that conflicts with those documents is non-conforming.

---

## 1. Invariants Are Locked by Construction

The invariants in `spec/invariants.md` are **constitutional law** for this
repository. They are not configurable, optional, or feature-dependent. The
implementation MUST enforce them in code and fail closed if enforcement is not
possible.

Consequences:
- A change that introduces new identity, temporal, or spatial precision is
  prohibited.
- A change that creates new tracking, correlation, or queryability paths is
  prohibited.
- A change that increases retention or allows retroactive capability expansion
  is prohibited.
- A change that weakens break-glass quorum or auditability is prohibited.

If a proposal violates any of the above, it must be rejected.

---

## 2. Versioning and Spec Updates

### 2.1 Spec Version Scope

Spec versioning applies to **normative documents** and the **rulesets** they
anchor. This includes:
- `spec/invariants.md`
- `spec/event_contract.md`
- `kernel/architecture.md`

Normative updates MUST be explicit and versioned. Informative or explanatory
documents may evolve without changing conformance requirements, but MUST NOT
smuggle in new capabilities or loosen constraints.

### 2.2 Acceptable Changes

Acceptable updates include:
- Clarifying language that reduces ambiguity without altering meaning.
- Strengthening constraints or enforcement guidance.
- Adding tests, audit hooks, or verification tooling that make misuse harder or
  more detectable.
- Documenting stricter defaults or guardrails already enforced by code.

### 2.3 Prohibited Changes

Prohibited updates include:
- Narrowing time buckets or jitter requirements.
- Expanding event schema fields or adding new identifiers.
- Introducing fields that enable identity, trajectory, or cross-device linking.
- Allowing export of raw media in normal operation.
- Weakening break-glass quorum or retention enforcement.
- Making any invariant “optional,” “debug,” or “temporary.”

If a change proposal falls into a prohibited category, it MUST be rejected.

---

## 3. Governance Process for Changes

### 3.1 Default Stance: Reject Weakening Changes

Any change that weakens a constitutional guarantee is **rejected by default**.
This includes changes that *appear minor* (e.g., a smaller time bucket or longer
correlation window). These are treated as violations of the threat model and
must not be merged.

### 3.2 Required Path for Potentially Weakening Changes

If a contributor believes a change that could weaken guarantees is necessary,
the only acceptable process is a **formal, explicit RFC** that is publicly
reviewed and approved by the community and maintainers.

Such an RFC MUST:
- Clearly identify every invariant affected.
- Provide a threat-model analysis explaining the new risks.
- Demonstrate why a strengthening alternative is not viable.
- Document the expected public review period and approval threshold.

A change MUST NOT be merged without clear, recorded community approval and
maintainer sign-off. Silent or implicit approval is invalid.

### 3.3 Enforcement Expectations

Even with approval, code changes MUST:
- Preserve structural enforcement (no “policy-only” exemptions).
- Provide explicit audit artifacts for any new or modified capability.
- Maintain the “fail closed” posture if enforcement mechanisms are absent.

---

## 4. Rejection Criteria (Non-Exhaustive)

Reject any change that:
- Adds fields that increase temporal, spatial, or identity precision.
- Enables correlation across devices or long time windows.
- Permits raw media export outside the break-glass path.
- Creates new identifiers, hashes, embeddings, or metadata that enable tracking.
- Weakens contract enforcement, retention enforcement, or break-glass quorum.
- Adds “future-proofing” hooks that expand capability surface.

If in doubt, treat the change as a weakening change and reject it.

---

## 5. Auditability and Public Record

All approved changes to normative specs MUST be:
- Documented in a public changelog with rationale.
- Linked to the RFC or review record.
- Traceable to specific versions of the normative documents.

This ensures that any deviation from the original guarantees is visible,
contestable, and historically auditable.

---

## 6. Summary

This repository exists to make misuse structurally impossible. Governance is the
mechanism that prevents incremental erosion. The invariants are locked, the
specs are versioned with care, and any weakening change requires explicit
community approval and formal review. Anything less is non-conforming.
