# CI ground rules

The `.github/workflows/` tree is code, and it has invariants. They are
**machine-enforced**: `workflows-lint.yml` runs actionlint (syntax, shell,
expressions) *and* `.github/scripts/ci_policy_check.py` (this policy) on
every PR that touches CI. A new workflow that skips a rule fails its own
introducing PR — the rules below can't rot by forgetting.

## The rules

| # | Rule | Why |
|---|------|-----|
| R1 | Every workflow declares `permissions` (top-level or per-job) | Least-privilege `GITHUB_TOKEN`, always explicit — a workflow that needs `write` says so where reviewers see it |
| R2 | Every job sets `timeout-minutes` | A hung job otherwise burns the 360-minute default. Pick ~2–3× the healthy runtime — the timeout is a circuit breaker, not a race |
| R3 | Push/PR workflows declare a `concurrency` group; neither tag/release-triggered **nor branch-push** workflows may set a bare `cancel-in-progress: true` | PR groups cancel superseded runs (`cancel-in-progress: ${{ github.event_name == 'pull_request' }}`); anything that **publishes** (releases, GHCR, Pages-adjacent) queues with `cancel-in-progress: false` or a condition excluding the publish path — never kill a run mid-upload. A bare `true` also cancels **main**: a burst of merges then kills each build before it reports, and main's state goes unknown while merely looking busy. A canceled build reads the same as an unfinished one. Exemptions: `ci-policy.yml → publish_cancel_ok` / `branch_cancel_ok`, each with a reason |
| R4 | Action refs are pinned to a tag or SHA — never `@main`/`@master`, never docker `:latest` | A mutable ref can change under a release run; third-party actions with secrets access get SHAs |
| R5 | `pull_request` workflows are path-filtered | Unrelated PRs shouldn't pay for your workflow. Repo-wide checks that are unfiltered *on purpose* are listed in `ci-policy.yml → unfiltered_ok`, each with a reason |
| R6 | `push` and `pull_request` path lists are identical | Copy-paste drift between the two silently makes main verify different things than PRs |
| R7 | A paths filter includes the workflow's own file | Editing a workflow must run it — the classic "merged a broken workflow that never triggered" gap |
| R8 | Third-party actions (any owner outside `actions/` and `github/`) are pinned to a full commit SHA with a `# <version>` comment | A tag is a mutable ref in someone else's hands — a compromised or careless owner can move it under a run that holds secrets. SHA pins make the supply chain content-addressed; Dependabot bumps the pin and comment together. Exemptions: `ci-policy.yml → third_party_tag_ok`, each with a reason |

Exemptions live in `.github/ci-policy.yml`, never in the checker — each
one carries a comment saying why. Run the checker locally with
`python3 .github/scripts/ci_policy_check.py` (needs `pyyaml`).

## Speed & cost conventions

- **Arduino/ESP32 builds** go through the shared composite action
  `.github/actions/setup-arduino-esp32` — it caches the >1 GB ESP32
  toolchain (one cache entry shared by every latest-core job, weekly
  rotation so "latest" can't go stale) and installs libraries fresh
  (small on purpose, so lib changes never fragment the toolchain cache).
  Never hand-roll `arduino-cli core install` in a workflow.
- **PlatformIO** jobs cache `~/.platformio` keyed on the relevant
  `platformio.ini` set (see firmware.yml).
- **Rust** jobs use `Swatinem/rust-cache@v2`. Cargo *tools* built from
  source (`cargo install <tool>`) cache the built binary under a weekly
  key — pattern in audit.yml / sbom.yml.
- **Node** jobs installing from a lockfile use `setup-node`'s
  `cache: npm` with an explicit `cache-dependency-path`.
- **One-off big downloads** (Emscripten SDK, Playwright Chromium) get an
  `actions/cache` entry — pinned-version keys for pinned tools, weekly
  keys for floating ones (see canary-local.yml).
- **Expensive matrix legs skip PRs when a cheaper leg gives the signal**
  (e.g. addon-image builds amd64-only on PRs; the QEMU aarch64 build
  runs on main/tags where the multi-arch push happens).
- Main pushes are **never canceled** — full signal per main commit is a
  repo convention; only superseded PR runs get canceled.

## Adding a new workflow — checklist

1. `permissions:` block (usually `contents: read`).
2. `concurrency:` group named after the workflow +
   `${{ github.ref }}`; cancel PRs only, or `false` if it publishes.
3. `timeout-minutes` on every job.
4. Path-filter `push`/`pull_request` identically, and include
   `.github/workflows/<your-file>.yml` in the filter.
5. Pin `actions/`/`github/` actions to a major tag; pin every other
   action to its 40-hex commit SHA with a `# <version>` comment
   (resolve with `git ls-remote … 'refs/tags/<tag>^{}'`).
6. Reuse the caching patterns above instead of inventing new ones.

The policy check tells you about 1–5 on the PR if you forget; 6 is on
the reviewer.
