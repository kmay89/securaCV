# .github/ — the brief for workflows and releases

Loads when you touch a file here; the root `AGENTS.md` still applies.

## Workflows

- Every workflow follows [`CI.md`](CI.md) (`push` paths equal `pull_request`
  paths, one Python floor from `pyproject.toml`, and the rest), and
  `scripts/ci_policy_check.py` enforces it: run it after editing YAML.
- Release targets are declared in [`release-targets.yml`](release-targets.yml),
  never in workflow YAML; the decision engine is unit-tested in
  `scripts/test_release_plan.py`.
- Display env lists in a matrix are derived from `firmware/flavors.json`
  (`firmware/scripts/flavor_envs.py --check-workflows`), never typed.
- CodeQL on a PR analyzes only the languages the diff touches (its `changes`
  job reads the compare); a push to main and the weekly schedule analyze all five.
- `python3 scripts/gates.py --paths <file>` says which workflows a path
  triggers; check it after editing a `paths:` filter.
- A filtered workflow's tests may read only files inside its `paths:` filter
  (`scripts/check_path_filter_reads.py`, in `canary-local.yml`).
- "Rebuild emulator dist (pinned emsdk)" pushes to the branch it was
  dispatched on with the default token, so its push retriggers nothing and
  can park runs at `action_required`; the playbook is `canary-local/AGENTS.md`.

## Releasing

- To ship anything, or to answer "why didn't this ship?", start from
  [`docs/RELEASE_BUTTONS.md`](../docs/RELEASE_BUTTONS.md). The default button
  is Actions → "Update everything (only what needs it)": it releases only what
  is ahead, explains what it skipped, and cannot re-ship a new tree under a
  published version. The three failures that cost real time: no OTA signing
  key; a flasher pinned to a release nobody cut; an app version already
  published, where "publishing" silently overwrites.
- Before touching an app build or release workflow (Flasher, Lab, iPhone,
  iPad, tvOS, Mac), grep [`RELEASE_LESSONS.md`](RELEASE_LESSONS.md) for your
  target (`rg <target> .github/RELEASE_LESSONS.md`); it holds 93 dated entries
  and is not read whole. What holds on every platform: copy bundled payloads
  with `cp -RL` (a dangling symlink makes the bundler abort with "resource …
  doesn't exist"); pin or log every upstream ref; prove the bundle with the
  build-only / `dry_run` path before publishing; verify bundled resources
  exist in the copy step.
- When you fix a release or packaging bug, **append a dated entry there**
  (symptom → cause → fix → applies-to) and apply the fix to every app target,
  not just the one that broke (`scripts/asc_publish.sh` is one shared result).
- Three version files per app: `tauri.conf.json` (names the tag),
  `package.json`, `Cargo.toml`. `desktop/scripts/check_app_versions.py` holds
  them together, and `RELEASE_NOTES.md` must say what the version changes.
- Two flashers, two frontends (root rule 7): a diagnostic lands in
  `canary-local/assets/` and `desktop/src/` both.
