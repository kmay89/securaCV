# SecuraCV — Independent Code Review & Roadmap (2026-05)

An audited, skeptical review of the entire `securaCV` repository, produced from a first-principles
reading of the code (not a restatement of the project's own marketing/strategy docs). Guiding
instruction: **do not assume anything is complete or finished.**

| Doc | What it is |
|-----|------------|
| [`00-requirements-spec.md`](00-requirements-spec.md) | **Rebuild-from-scratch requirements.** Layered (system → component → contracts), every requirement tagged Implemented / Partial / Stub / Spec-only / Config with file evidence. Precise enough to reconstruct the feature set without injecting errors. |
| [`01-flag-report.md`](01-flag-report.md) | **Everything inconsistent, unfinished, or poorly implemented**, by severity (Blocker / Major / Minor / Doc-debt), each with reproducible file evidence. Also lists what is genuinely solid. |
| [`02-roadmap.md`](02-roadmap.md) | **Independent roadmap** that reconciles & critiques the existing `v1-roadmap.md` and `docs/strategy/`, with a phased invariant-checked plan, an ESP32-S3-grounded hardware plan, and a current (2026) market comparison. |
| [`03-feature-flags-and-hygiene.md`](03-feature-flags-and-hygiene.md) | **Feature-flag practice & engineering-hygiene review.** Grades flag handling across all four layers and the repo's repeatable processes (CI/test/lint/deps/secrets), tagged Recommended-now / Keep / Follow-up. Ships a central flag registry + CI lint. |

## Relationship to existing docs
This review is deliberately **independent** of, and cross-checks, the authored strategy material in
[`../strategy/`](../strategy/) and the root [`../../v1-roadmap.md`](../../v1-roadmap.md). Where they
overclaim or are stale, the flag report says so (see F-01, F-02, F-13). The seven invariants in
[`../../spec/invariants.md`](../../spec/invariants.md) remain the binding constraint on every
recommendation.

## Top-line findings
- The integrity core (hash-chain + Ed25519 + dual Rust↔JS envelope verifier) is **real and tested**.
- The **default detection path is a frame-hash stub**; real ONNX detection is feature-gated and off
  by default — yet the roadmap marks it "Done" (Blocker F-01).
- "v1" is defined **three incompatible ways** across CHANGELOG / roadmap / README (Blocker F-02).
- Firmware still has open **privacy conformance bugs** (raw MAC / fine GPS over WAP APIs) that
  contradict the invariants (Blocker F-03).
- The strategic recommendation is **focus + productize, not expand**: finish the verified-timeline,
  one-click-install, and court-grade-export payoff; freeze unbuilt transports/mesh legs.
