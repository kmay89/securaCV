# SecuraCV — v1 Launch Readiness Capstone

> **Purpose:** a thin "start here for the v1 launch" layer that ties the existing canonical
> reviews together, records the decisions taken **this cycle**, and points to the two net-new
> operational docs. **It deliberately does _not_ restate** the per-subsystem audit, the flag
> report, the phased roadmap, the variant audit, or the market analysis — those already exist and
> are maintained (see the table below). If something here and a canonical doc disagree, the
> canonical doc wins and this file is the bug.
>
> **One-sentence verdict:** SecuraCV is *substantially v1-complete and CI-green*; the single hard
> blocker to a tagged v1 is **on-device hardware validation** (see the bench runbook). Nothing
> headline is vaporware.

---

## 1. Canonical sources — read these (this doc does not duplicate them)

| You want… | Read | Not |
|---|---|---|
| The binding v1 definition + stream status | [`../v1-roadmap.md`](../v1-roadmap.md) | here |
| Rebuild-from-scratch requirements, every item tagged Implemented/Partial/Stub w/ file evidence | [`review/00-requirements-spec.md`](review/00-requirements-spec.md) | here |
| Everything inconsistent/unfinished by severity (the `F-xx` flags) | [`review/01-flag-report.md`](review/01-flag-report.md) | here |
| The independent, reconciled, phased roadmap (P0–P4) + hardware plan + 2026 market scan | [`review/02-roadmap.md`](review/02-roadmap.md) | here |
| Feature-flag practice & engineering hygiene | [`review/03-feature-flags-and-hygiene.md`](review/03-feature-flags-and-hygiene.md) | here |
| Firmware variant lifecycle, risk, de-rot plan | [`../firmware/FIRMWARE_VARIANT_AUDIT.md`](../firmware/FIRMWARE_VARIANT_AUDIT.md) · [`../firmware/VARIANT_POLICY.md`](../firmware/VARIANT_POLICY.md) | here |
| Per-capability parity across variants (CI-guarded) | [`../firmware/FEATURES.md`](../firmware/FEATURES.md) | here |
| Product strategy / personas / market & cost | [`strategy/`](strategy/) | here |
| Threat model, audit-vs-security boundary | [`security/THREAT_MODEL.md`](security/THREAT_MODEL.md) | here |

The `review/` set is re-baselined to 2026-06-04 and already records that the three original
blockers (**F-01** detection labeling, **F-02** single v1 definition, **F-03** firmware privacy)
are **resolved**. This capstone does not re-litigate them.

---

## 2. Net-new deliverables this cycle (the only things this work added)

1. **CI: fixed the long-red `Detect Eval` check** (`.github/workflows/detect-eval.yml`). It was a
   YAML startup failure (unquoted `cargo test --lib eval::` → 0 jobs) on every commit since it was
   added; now it parses and runs green. (Supersedes the still-open PR #734.)
2. **Firmware guard: mesh requires flash encryption** (`firmware/scripts/regression_check.sh`).
   Asserts every mesh implementation file keeps its `esp_flash_encryption_enabled()` gate, so mesh
   can never silently ship without flash encryption — the static half of issue **#610**.
3. **Status corrections to the firmware picture** (verified against code, via PR #737 review):
   - Mesh **is wired** in the ACTIVE tree (`firmware/canary/src/main.cpp` inits
     `mesh_transport`/`mesh_session` under `FEATURE_MESH_NETWORK` and drives them from `loop()`);
     it ships `=0` everywhere except `[env:full]` and is unproven on hardware — the gap is
     enablement + proof, not wiring.
   - `canary-wap` **already serves HTTPS:443** (`httpd_ssl_start` + HTTP→443 redirect) when
     `SECURACV_HAS_HTTPS_SERVER` and a cert are present — cert/IDF-config-gated, not absent. (The
     `FEATURES.md` TLS row stays ❌ because default builds don't provision a cert; that's honest.)
4. **[`V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md)** — the unified driver for the one
   hardware gate (sequences the existing per-subsystem procedures; see §3).
5. **[`../firmware/PARITY_PLAN.md`](../firmware/PARITY_PLAN.md)** — captures the founder decision to
   bring **both** firmware trees to full bidirectional parity (see §4).

---

## 3. The launch gate: on-device hardware validation

This is the **only** open blocker in the canonical v1 definition (`v1-roadmap.md`; corresponds to
`review/02-roadmap.md` **P3**). Everything documented is green in CI but unproven on real ESP32
boards. The driver sheet — exact commands, expected results, artifact paths, sign-off matrix,
closing issue **#610** — is **[`V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md)**. It
references rather than duplicates the detailed procedures already in the repo:
[`audit/hardware_verification_checklist.md`](audit/hardware_verification_checklist.md) (mesh/chirp/
beacon), [`audit/v0.3_closeout.md`](audit/v0.3_closeout.md), [`hardware/bench_bringup.md`](hardware/bench_bringup.md)
(chirp/LED/button smoke), [`getting_started_canary.md`](getting_started_canary.md),
[`onboarding_multiple_canaries.md`](onboarding_multiple_canaries.md), and
`integrations/ha_frigate_mqtt/verify_pipeline.sh` (operator stack).

When its matrix is fully ✅, run the tag step: flip the README badge `v1-rc → v1.0`, date the
`CHANGELOG.md [1.0.0]` entry, bump the crate `0.5.0 → 1.0.0`. (Per **F-02**, the README/CHANGELOG/
roadmap status is *already* reconciled at `v1-rc`; the tag is the only remaining flip, not a
cleanup.)

---

## 4. Decisions taken this cycle

| Decision | Outcome | Lives in |
|---|---|---|
| Which firmware tree is the v1 image? | **Both**, brought to full bidirectional parity | [`../firmware/PARITY_PLAN.md`](../firmware/PARITY_PLAN.md) |
| Object-detection default (bundle a model)? | **Deferred** — consistent with **F-01**: in the primary Frigate deployment detection is Frigate's job; the direct-ingest path ships a one-command model fetch + a motion-only startup WARN | `review/01`, `review/02` P1 |
| Investor/advertising "what it can do" section | **Deferred** — the market/positioning material already lives in `review/02-roadmap.md §5` and `strategy/05`; the residual asset is the README screenshot (**F-12**, already tracked) | `strategy/`, `review/02` |

> **Parity vs. the variant audit — a tension to keep visible:** `FIRMWARE_VARIANT_AUDIT.md`'s
> de-rot plan item #6 recommends *convergence* (progressively migrate the Arduino monolith into the
> modular `firmware/canary`), whereas this cycle's decision is to *mirror* both at parity. Parity is
> the v1 bridge (lose no capability); convergence is the post-v1 question. `PARITY_PLAN.md` records
> both so the choice is deliberate, not drift.

---

## 5. For investors, in one paragraph (detail lives in `strategy/` + `review/02 §5`)

SecuraCV sells **proof, not pixels**: the only home camera whose record you can prove wasn't
altered, that by construction can't be turned into surveillance. The moat is a *combination
competitors cannot toggle on* — tamper-evident perception (hash-chain + Ed25519 + a standalone
dual Rust↔JS verifier) plus privacy-by-construction (seven invariants enforced in the type system).
It is demonstrable end to end today (`demo` → `tamper_demo` → `log_verify`), CI-gated across the
Frigate→HA pipeline, RTSP/file ingest, the adapter framework, and a 25/25-test device API. The
remaining work to a tagged, advertisable v1 is **validation, not invention** — run the bench gate
in §3 and tag. For the full thesis, personas, market scan, and the honest deferrals, read
`strategy/08-product-strategy.md` and `review/02-roadmap.md`.
