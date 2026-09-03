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
| R3 | Push/PR workflows declare a `concurrency` group. **Tests:** `group: <name>-${{ github.ref }}-${{ github.event_name == 'pull_request' && 'pr' \|\| github.sha }}` + `cancel-in-progress: ${{ github.event_name == 'pull_request' }}` — PR runs collapse per branch, every main commit gets its own group. **Publishers** (releases, GHCR, Pages, label sync, the HACS mirror push): `group: <name>-${{ github.ref }}` + `cancel-in-progress: false` (or a condition that excludes the publish path), listed in `ci-policy.yml → main_queue_ok` — publishes queue in order and are never killed mid-upload. Never a bare `cancel-in-progress: true` on anything that runs on a branch push | Two failure modes, both silent. A bare `true` cancels **main**: a burst of merges kills each build before it reports, and main's state goes unknown while merely looking busy (firmware.yml, 2026-07-24). And `false` only protects the *running* run: GitHub keeps one **pending** run per group and a third arrival evicts the one waiting, so with a shared per-ref group a main commit can go unverified while looking merely queued. Per-commit groups close that gap; a queued *publish* being superseded by a newer publish is fine, which is why publishers stay per-ref. A canceled build reads the same as an unfinished one. Exemptions: `ci-policy.yml → publish_cancel_ok` / `branch_cancel_ok` / `main_queue_ok`, each with a reason |
| R4 | Action refs are pinned to a tag or SHA — never `@main`/`@master`, never docker `:latest` | A mutable ref can change under a release run; third-party actions with secrets access get SHAs |
| R5 | `pull_request` workflows are path-filtered | Unrelated PRs shouldn't pay for your workflow. Repo-wide checks that are unfiltered *on purpose* are listed in `ci-policy.yml → unfiltered_ok`, each with a reason |
| R6 | `push` and `pull_request` path lists are identical | Copy-paste drift between the two silently makes main verify different things than PRs |
| R7 | A paths filter includes the workflow's own file | Editing a workflow must run it — the classic "merged a broken workflow that never triggered" gap |
| R8 | Third-party actions (any owner outside `actions/` and `github/`) are pinned to a full commit SHA with a `# <version>` comment | A tag is a mutable ref in someone else's hands — a compromised or careless owner can move it under a run that holds secrets. SHA pins make the supply chain content-addressed; Dependabot bumps the pin and comment together. Exemptions: `ci-policy.yml → third_party_tag_ok`, each with a reason |
| R9 | A job that runs Python — `python3`, `pip`, `ruff`, `mypy`, `pytest`, directly or through a script — has an `actions/setup-python` step, normally `python-version-file: pyproject.toml`, placed right after checkout | The runner image's `python3` is whatever `ubuntu-latest` ships this month and it moves under you (24.04 went to 3.12 while `pyproject.toml` targets 3.11), and packages that merely happen to be preinstalled there (PyYAML) are not on the setup-python interpreter — so a job `pip install`s what it imports. `pyproject.toml`'s `requires-python` is the ONE floor for the repo's tooling: raise it there and every job follows. A job that needs a specific interpreter (PlatformIO, the Vela compiler) may pin `python-version` explicitly and says why in a comment. The checker sees commands in `run:` blocks; a Python call hidden inside a shell script is on the reviewer. Exemptions: `ci-policy.yml → system_python_ok` (`<workflow>.yml:<job>`) |

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
  (e.g. addon-image builds amd64-only on PRs; the aarch64 build runs on
  main/tags where the multi-arch push happens).
- **Multi-arch images build each arch natively** — `runs-on:
  ${{ matrix.runner }}` with `ubuntu-24.04-arm` for arm64, never QEMU on an
  x86 runner. A Rust build under emulation is an order of magnitude slower:
  the add-on's aarch64 leg hit its 90-minute timeout on six consecutive main
  runs before it moved to the native runner.
- Main pushes are **never canceled, and never evicted from a queue** — full
  signal per main commit is a repo convention; only superseded PR runs get
  canceled, and test workflows give each main commit its own concurrency
  group (R3) so a burst of merges cannot drop one.
- **Python jobs bring their own interpreter** (R9): `actions/setup-python@v7`
  with `python-version-file: pyproject.toml` right after checkout, then
  `pip install` whatever the job imports beyond the stdlib. Nothing runs on
  the runner image's `python3`.

## Adding a new workflow — checklist

1. `permissions:` block (usually `contents: read`).
2. `concurrency:` — a test: `group: <name>-${{ github.ref }}-${{
   github.event_name == 'pull_request' && 'pr' || github.sha }}` and
   `cancel-in-progress: ${{ github.event_name == 'pull_request' }}`; a
   publisher: `group: <name>-${{ github.ref }}`, `cancel-in-progress:
   false`, plus a `main_queue_ok` entry saying why order matters.
3. `timeout-minutes` on every job.
4. Path-filter `push`/`pull_request` identically, and include
   `.github/workflows/<your-file>.yml` in the filter.
5. Pin `actions/`/`github/` actions to a major tag; pin every other
   action to its 40-hex commit SHA with a `# <version>` comment
   (resolve with `git ls-remote … 'refs/tags/<tag>^{}'`).
6. Runs Python? `actions/setup-python@v7` with `python-version-file:
   pyproject.toml` right after checkout, and `pip install` what it imports.
7. Reuse the caching patterns above instead of inventing new ones.

The policy check tells you about 1–6 on the PR if you forget; 7 is on
the reviewer.
