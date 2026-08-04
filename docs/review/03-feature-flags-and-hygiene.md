# SecuraCV — Feature-Flag Practice & Engineering Hygiene Review

> Companion to [`00-requirements-spec.md`](00-requirements-spec.md),
> [`01-flag-report.md`](01-flag-report.md), and [`02-roadmap.md`](02-roadmap.md).
> Question answered: *"Are we implementing feature flags properly, and what other
> repeatable processes should we adopt at this stage?"* Same skeptical lens as the
> flag report — evidence-backed, severity-tagged, with a counterbalance of what's
> already solid. Severity: **Major** (will bite a re-implementer or let a regression
> through) · **Minor** (rot/cleanup) · **Process** (a repeatable practice to adopt).

This review ships with concrete fixes (see [§3](#3-what-shipped-with-this-review)).
The central flag registry it recommends now exists at
[`docs/feature-flags.md`](../feature-flags.md).

---

## Part A — Feature flags

**Verdict: the mechanisms are good and layer-appropriate; the *coordination* was
missing.** SecuraCV gates behavior at four layers, each internally consistent,
but until this review there was no single index, no written lifecycle, and the
one excellent "don't advertise unbuilt work" pattern was applied by hand with no
guard.

### The four layers (evidence)

| Layer | Mechanism | Source of truth |
|---|---|---|
| Rust compile-time | Cargo `[features]` + `#[cfg(feature=…)]` / `required-features` | `Cargo.toml:55-71` (91 `cfg(feature)` sites across 16 files) |
| Firmware compile-time | `FEATURE_*` C macros, per-board | `firmware/FEATURES.md` |
| Runtime config | TOML + `WITNESS_*` env (kernel); JSON device-state (canary-vision) | `src/config.rs`, `config.example.toml`, `canary-vision/device-api/lib/device-state.js` |
| Capability gating | `ALL_*` vs `FUTURE_*` lists | `custom_components/securacv/const.py:49-65` |

### What's genuinely solid (counterbalance)
- **Invariant flags have teeth.** `privacy.camera_peek_enabled` is in
  `IMMUTABLE_PRIVACY_KEYS` and is stripped from every API write
  (`canary-vision/device-api/routes/config.js:12,84-99`), with a regression test
  (`canary-vision/tests/security/camera-peek.test.js`). This is the right way to
  make a privacy invariant *non*-optional — exactly what `CONTRIBUTING.md` §"What
  We Do Not Accept" demands.
- **The capability-gate pattern is correct.** `FUTURE_TRANSPORTS` keeps LoRa/audio
  out of `ALL_TRANSPORTS` so the HA surface never advertises a transport no device
  can report on (`const.py:49-65`) — this already resolved flag-report **F-07**.
- **CI exercises feature combinations**, not just the default build:
  `--no-default-features`, `pqc-*`, and per-ingest jobs (`.github/workflows/rust.yml`).
- **Firmware flags are inventoried** in a per-variant parity matrix
  (`firmware/FEATURES.md`).

### Findings

**FF-01 (Major) — No single source of truth across layers.** Each layer documented
its own flags; nothing stated *what flags exist, their default, and their lifecycle*
in one place, so the Cargo/firmware/runtime/capability stories drifted independently
(cf. flag-report **F-13** stale roadmap). *Fixed:* central registry at
[`docs/feature-flags.md`](../feature-flags.md), lint-enforced for completeness.

**FF-02 (Major) — The "never advertise unbuilt" rule wasn't enforced.** The
`FUTURE_*`/`ALL_*` split is correct but was a hand-maintained convention; nothing
stopped a future edit from re-introducing F-07. *Fixed:*
`scripts/lint_feature_flags.sh` fails CI if any `FUTURE_*` transport appears in
`ALL_TRANSPORTS`.

**FF-03 (Minor) — No documented lifecycle / removal criteria.** Flags had no
experimental→stable→deprecated→removed convention and no written exit conditions,
so stale flags accumulate (cf. flag-report **F-11**, two RTSP impls that "risk
divergence"). *Fixed:* lifecycle + removal-criteria columns are now mandatory in
the registry; convention written in its "Conventions" section.

**FF-04 (Minor) — "Feature flag" conflated with "config."** Cargo features
(build-time optionality) and runtime config toggles were both loosely called
"flags." *Fixed:* the registry's decision rule states which layer to use when.

**FF-05 (Process) — Default detection is flag-gated off, but that's underspecified
to users.** `backend-tract` (real ONNX) is off by default; the default build is the
frame-diff stub (flag-report **F-01**). That's a legitimate *experimental* flag —
now labeled as such in the registry — but the user-facing "what does the default
build actually detect" gap is F-01's to close, not solved here.

**Orphaned-flag guard.** `scripts/lint_feature_flags.sh` also fails if a Cargo
`[features]` key is declared but never referenced in `src/`/`tests/` — catching
features left behind after their code is deleted.

---

## Part B — Engineering hygiene (broad)

**Verdict: ~9/10. Production-grade, security-first discipline with a few gaps that
are mostly deliberate trade-offs.** Each item below is tagged **Recommended now**,
**Keep (deliberate trade-off)**, or **Follow-up**.

### Already strong (keep doing)
- **CI breadth:** 12 workflows — Rust `fmt`/`clippy -D warnings`/test across feature
  variants, firmware PlatformIO (dev/release/full), CodeQL SAST (Rust/Py/JS/C++),
  gitleaks (`secrets-scan.yml`), CycloneDX SBOM (`sbom.yml`), release gates.
- **Multi-language e2e tests** against live brokers (RTSP via MediaMTX, MQTT via
  mosquitto) with `SECURACV_RTSP_E2E=1` so a missing server can't pass vacuously.
- **Governance:** strict `CONTRIBUTING.md` (invariants-as-law), security-checklist
  `PULL_REQUEST_TEMPLATE.md`, `CODEOWNERS`, structured issue forms.
- **Secrets/deps:** comprehensive `.gitignore` (`*.pem`, `*.key`, `.env`,
  `secrets.h`, `.device_key_seed`), pinned `Cargo.lock` + `package-lock.json`,
  advisory tracking in `SECURITY.md`.
- **Git practice:** conventional commits with scope + PR refs, no fixup/revert noise.

### Gaps & recommendations

| ID | Sev / tag | Gap | Recommendation |
|----|-----------|-----|----------------|
| HY-01 | **Recommended now** *(shipped)* | The two `scripts/lint_*.sh` (CAP-mapping, non-impersonation) ran nowhere in CI — they were dead guards. | New `.github/workflows/lint.yml` runs all `scripts/lint_*.sh` on push/PR. **Done in this PR.** |
| HY-02 | **Recommended now** *(shipped)* | No `.editorconfig`; cross-editor drift in a 4-language repo. | Added repo-root `.editorconfig`. **Done in this PR.** |
| HY-03 | Follow-up | No JS lint/format. `canary-vision` (Node) relies on human review; CSP/token bugs are easy to miss. | Add ESLint flat config + `npm run lint`; gate in `viewer.yml`/canary-vision CI. Prefer `eslint` core + a small ruleset over Prettier-everything to limit churn. |
| HY-04 | Follow-up | No Python type-checking for the HA integration (`custom_components/securacv/`). | Add `mypy` (or `ruff check`) as a CI job; the existing `conftest.py` HA stubs make this tractable. |
| HY-05 | Follow-up | HA integration deps aren't pinned (no `requirements.txt`/`pyproject.toml`); only an implicit HA version lock via `manifest.json`. | Pin a dev/test requirements file so CI is reproducible. |
| HY-06 | Follow-up | Dependency-advisory tracking is **manual** in `SECURITY.md`. | Enable Dependabot (or a scheduled `cargo audit` + `npm audit` job) to automate what's now hand-curated. |
| HY-07 | Follow-up | `firmware/FEATURES.md:33` describes a CI contract (fail on ✅→⚠️/❌ dashboard regression unless the PR cites an issue) that **isn't wired yet**. | Implement the dashboard-parser guard the doc already promises; this is the firmware analogue of `lint_feature_flags.sh`. |
| HY-08 | **Keep** | No pre-commit hooks (husky/pre-commit). | **Deliberate trade-off — keep.** The project favors CI gates + an upfront PR checklist over local hooks. Only add if contributors want faster local feedback; don't make it a merge gate. |

### Priority at this stage
1. **Shipped:** wire existing lints into CI (HY-01), `.editorconfig` (HY-02),
   flag registry + lint (Part A).
2. **Next (cheap, high-signal):** Dependabot/`cargo audit` (HY-06) and the
   `FEATURES.md` regression guard (HY-07) — both close "a guarantee exists on
   paper but nothing enforces it" gaps, matching this repo's enforce-in-code ethos.
3. **Then:** JS lint (HY-03), Python types + dep pinning (HY-04/05).

---

## 3. What shipped with this review
- [`docs/feature-flags.md`](../feature-flags.md) — central flag registry + conventions (FF-01/03/04).
- `scripts/lint_feature_flags.sh` — orphaned-feature, no-advertise-unbuilt, and registry-completeness checks (FF-01/02/03).
- `.github/workflows/lint.yml` — runs all `scripts/lint_*.sh` in CI (HY-01).
- `.editorconfig` — cross-language formatting baseline (HY-02).
- `CONTRIBUTING.md` — feature-flag rules + lint added to the pre-submit checklist.
