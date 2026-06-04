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
> **Re-baselined 2026-06-04.** Findings below were the 2026-06-01 audit state; the 🟢/🟡 markers
> reflect the tree after the 2026-06 fix wave (#660–#680). Per-flag detail and closing PRs live in
> [`01-flag-report.md`](01-flag-report.md); phase status in [`02-roadmap.md`](02-roadmap.md). **All
> three original Blockers (F-01/F-02/F-03) are now resolved.**

- 🟢 The integrity core (hash-chain + Ed25519 + dual Rust↔JS envelope verifier) is **real and tested**.
- 🟢 **F-01 resolved** — the default detection path is still motion (not ONNX) *by design*, but is now
  **honestly labeled** (startup WARN), the confidence threshold is configurable, and a one-command
  model fetch enables real detection (#660/#665/#667).
- 🟢 **F-02 resolved** — "v1" now has a single canonical definition; README badge / CHANGELOG /
  roadmap agree (`v1-rc`, on-device validation pending) (#673).
- 🟢 **F-03 resolved** — firmware routes identity through a salted pseudonym and coarsens GPS in
  *every* tree, with a `regression_check.sh` guardrail that hard-fails on raw MAC / fine GPS
  (#662/#669).
- 🟡 The strategic recommendation is **focus + productize, not expand**: P0/P1, one-click install, and
  the **HA verified-✓ timeline card** are **done**; what remains of the payoff arc is the README
  screenshot (F-12, deferred) and the **break-glass/trustee setup UI** (P2). Unbuilt transports/mesh
  legs stay frozen.
