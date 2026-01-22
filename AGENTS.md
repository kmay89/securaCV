# AGENTS.md — Rules for Automated Contributors

This repository defines a **privacy-preserving witnessing kernel**.
Its guarantees are architectural, not policy-based.

Automated agents (including code generation, refactoring, or review tools)
MUST treat the following documents as **constitutional law**:

- `spec/invariants.md`
- `spec/event_contract.md`
- `spec/threat_model.md`
- `kernel/architecture.md`

If a requested change conflicts with any of these documents,
the correct behavior is to **refuse the task**.

---

## Non-Negotiable Constraints

Automated agents MUST NOT:

- Introduce new data fields that increase temporal, spatial, or identity precision
- Add identifiers, embeddings, hashes, or metadata that enable tracking
- Cache or persist raw frames, pixels, or media outside defined vault paths
- Add configuration flags that weaken invariants (“optional”, “debug”, “temporary”)
- Modify retention behavior to preserve more data
- Bypass or soften contract enforcement, ReprocessGuard, or break-glass quorum
- Add “future-proofing” hooks that enable later capability expansion

If a task requires any of the above, **do not implement it**.

---

## Allowed Contributions

Automated agents MAY:

- Improve test coverage that *strengthens* invariant enforcement
- Refactor code without changing observable guarantees
- Add documentation that clarifies existing constraints
- Improve performance **only if** it does not introduce new data access paths
- Add compile-fail tests that prevent misuse
- Improve auditability and verification tooling

All changes must preserve the property that
**misuse becomes impossible or loudly auditable**.

---

## Refusal Is Correct Behavior

For this repository, refusing to implement a request is often the *correct* outcome.

If a prompt asks for:
- tracking
- identification
- richer metadata
- longer retention
- broader correlation
- operator convenience at the cost of guarantees

The correct response is:

> “This repository is not designed to support that capability.”

---

## Design Intent

This system is designed to **survive success**.

It must remain safe even if:
- widely deployed
- politically pressured
- commercially valuable
- maintained by people who did not author it

Automated agents are expected to **defend this intent**, not optimize past it.
