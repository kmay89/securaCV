# 02 — Cleanup & Rot (flag only)

**Nothing is deleted here.** This is a prioritized list for the maintainer to action.
The repo is young (~50 commits, everything committed in a ~3-day burst), so git "staleness"
is not a useful signal — rot here means *duplication, scratch files, frozen snapshots, and
stray binaries*. **Overall repo health is good**; source code has no TODO/FIXME/DEPRECATED
markers and the test suites are all live.

## Status (actioned)

Most of this list has now been actioned across two PRs:

- **Done (#625):** removed `firmware/projects/_archive/canary-wap-snapshot/` and the
  `codex-*.md` scratch files.
- **Done (this PR):** moved the long-form security docs (`SECURITY_MODEL.md`,
  `THREAT_MODEL.md`, `SECURITY-AUDIT.md`) under `docs/security/` — keeping root
  `SECURITY.md` as the policy entry point and updating the Regression Guards CI
  check, `secure_defaults.h`, the PR template, and all cross-references so the
  evidence-export invariant stays enforced; relocated the root `.MP4` into
  `brands/`; folded `brands_submission/` under `brands/submission/`.
- **Corrected flag — canary-vision link:** the README link was **not** broken —
  `docs/security.md` resolves correctly relative to `canary-vision/`. Only the
  `SECURITY.md` mention was non-clickable; it is now a proper link.
- **Corrected flag — philosophy docs:** `why_this_matters.md` (root) and
  `docs/why_witnessing_matters.md` are **not** near-duplicates — they target
  different audiences (plain-language explainer vs. systems-level rationale).
  Both are kept, now reciprocally cross-linked instead of stubbed.
- **Deferred:** root `spec.md` consolidation, whitepaper "derived" marker, and
  `AGENTS.md` review remain open (low priority).

## Flagged candidates

| Path | What it is | Why flagged | Confidence | Recommendation |
|------|-----------|-------------|------------|----------------|
| `firmware/projects/_archive/canary-wap-snapshot/` | Frozen firmware snapshot (~936K), archived 2026-02-20, build-gated with `#error` + CI `[archive-edit]` label | Intentionally dead; bloats the tree | **Very high** | Remove if no legal/historical need; else move out of `projects/` and document the policy |
| `codex-plan.md`, `codex-prompt.md` (root) | AI agent scratch / planning notes | Not product docs; clutter the root | **High** | Remove (or move to a `.dev/` ignored path) |
| `canary-vision/SECURITY.md` & `canary-vision/README.md` | Reference `docs/security.md` | Broken relative link — actual file is `canary-vision/docs/security.md` | **High** | Fix the path (1-line) |
| `securaCV_logo_animation.MP4` (1.4 MB, root) | Logo animation video | Large binary at repo root; a GIF already lives in `docs/` | **Medium** | Move to `brands/` or a release asset / Git LFS |
| Root security/threat doc sprawl | `SECURITY.md`, `SECURITY_MODEL.md`, `SECURITY-AUDIT.md`, `THREAT_MODEL.md` **+** `spec/threat_model.md` | 4–5 overlapping docs; readers don't know which is canonical | **Medium** | Keep `SECURITY.md` (policy) + `spec/threat_model.md` (canonical); move the long-form ones under `docs/security/` and link |
| `why_this_matters.md` (root) vs `docs/why_witnessing_matters.md` | Two philosophy docs (249 vs 209 lines) | Near-duplicate intent | **Medium** | Keep one canonical (`docs/why_witnessing_matters.md`); make the root a stub link |
| `spec.md` (root) vs `spec/` | Root file overlaps the canonical spec set | Ambiguous source of truth | **Medium** | Fold into `spec/` or make it an index pointing into `spec/` |
| `securaCV_whitepaper.md` (root) | Consolidates other markdown into one paper | Hand-editing risks drift from sources | **Low** | Keep, but mark as *derived* / generated so edits go to the source docs |
| `brands/` + `brands_submission/` | Logo assets + one-time HA brands-repo PR package | Few references; submission is a one-shot artifact | **Low** | After the brands PR merges, fold `brands_submission/` under `brands/submission/` or archive |
| `AGENTS.md` (root) | Agent harness instructions | Confirm it's still used by current tooling | **Low** | Keep if used; remove if stale |

## Confirmed NOT rot — leave alone

The deep audit cleared these as legitimate and referenced:
`kernel/` (canonical architecture, linked from README), `sbom/`, `examples/`,
`integrations/ha_frigate_mqtt/`, `homeassistant/`, all `tests/` directories, and the
committed lockfiles (`Cargo.lock`, `canary-vision/package-lock.json` — correct practice).

## Suggested execution order (for later)

1. Fix the broken `canary-vision` doc link (trivial, user-facing).
2. Remove the two `codex-*.md` scratch files.
3. Decide the fate of `firmware/projects/_archive/canary-wap-snapshot/` (biggest single win).
4. Consolidate the security/threat/philosophy docs behind canonical files.
5. Relocate the root `.MP4` and the brands submission package.

Net: this is light cleanup, not a rescue. The structure is sound; it mostly needs a tidier
root and a single canonical home for security/philosophy docs.
