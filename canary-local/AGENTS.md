# canary-local — the brief for the Lab

Loads when you touch a file here; the root `AGENTS.md` still applies. The Lab
is real firmware compiled to WebAssembly plus hand-written pages. The human
guide is [`README.md`](README.md) (80 KB: open a section, not the file).

## Tests

- One suite: `node --test canary-local/tests/<name>.test.js` from the repo
  root. There are 62; `canary-local.yml`'s "page logic tests" job runs them
  all, and `scripts/check_path_filter_reads.py` fails it when a test reads a
  file outside the workflow's `paths:` filter. Add the path to **both** the
  `push` and `pull_request` lists.
- `LAB_CORES=native node --test canary-local/tests/vision.test.js
  canary-local/tests/eyes.test.js canary-local/tests/audio.test.js` builds the
  Vision and audio cores with g++ from the sources `build.sh` hands em++ and
  runs the same tests on them. Native green and dist red after a core change
  means the sources are right and the dist needs the rebuild below.
  `vision_wasm32.test.js` needs neither and catches 32-bit overflow from the
  sources. The rest: `tests/native/README.md`.
- The Chromium probes (`vision_probe.mjs`, `eyes_probe.mjs`,
  `audio_probe.mjs`) take the same variable; the display flavors boot only in
  Chromium.

## The committed dist, and the two gates that catch it stale

`emulator/dist/*.js` is compiled from `firmware/projects/canary-display` and
the Vision sources (exactly which files: that tree's `AGENTS.md`). Two CI
jobs catch a stale one, and only one is named for it:

1. "Dist drift check" compares the bundle bytes and ignores `meta.json`.
2. The **page logic tests** catch the version stamp (`canary_local.test.js`
   "artifact fw matches registry train"; `vision.test.js` "generated data is
   stale against its firmware core"). A version bump goes red in a job whose
   name says nothing about the emulator.

Rebuild with Actions → "Rebuild emulator dist (pinned emsdk)", dispatched on
your branch; emsdk 6.0.3 is not installable in most environments. Then:

- It pushes to the branch it was dispatched on. Dispatch on a feature branch.
- Its push does not retrigger CI (default `GITHUB_TOKEN`), and re-running the
  failed job checks out the original commit. Push again yourself, and that
  push must touch a path `canary-local.yml` or `firmware.yml` watches, or the
  PR sits at **zero** check runs and looks queued.
- Runs parked at `action_required` are waiting for a human's Approve, not for
  a commit. Zero runs anywhere is the path-filter problem; runs at
  `action_required` is the approval problem. Check which before pushing again.
- **The rebuild is upstream of the catalog generators.** `gen_flash.py` reads
  `fw_version` from `dist/*.meta.json`: run the `tools/gen_*.py` catalogs
  after pulling the rebuilt dist, never before, or `flash.json` fails as "a
  board's chip changed".

## Generators here

`tools/gen_*.py` write the pages' data and the catalogs; each has `--check`
and CI runs it. Two are chained: a captive-portal change is `gen_wap.py`
**then** `gen_csp.py`; a display-portal change is `gen_display_portal.py`
**then** `gen_csp.py` (every Lab page's CSP hashes the embedded page).
`tools/figures/gen_figures.mjs` and `gen_device_glbs.mjs` read the committed
STLs and the device manifests; the confidence ladder in `devices/figures.json`
is derived from evidence on disk, never typed. The full account:
[`docs/GENERATED_FILES.md`](../docs/GENERATED_FILES.md).

## Two flashers

`assets/` is the browser flasher and `desktop/src/` the desktop Flasher. They
share no UI code: a user-facing diagnostic lands in both.
