# firmware/ — the brief for this tree

Loads when you touch a file here; the root `AGENTS.md` still applies. The
human guide is [`README.md`](README.md); each project has its own brief where
it needs one (`projects/canary-wap/`, `projects/canary-display/`).

## Which copy is the source

- `canary/` (PlatformIO) and `common/` are canonical. The canary-wap Arduino
  sketch carries byte-identical staged copies of `common/` code so a zip
  download compiles; `scripts/check_*_sync.sh` (eighteen of them) fail CI when
  a copy drifts. Edit the source under `common/`, then re-stage the copy the
  check names. Never edit a copy alone.
- The canary-display Arduino sketch (`projects/canary-display/arduino/`) is
  generated from its PlatformIO tree by that project's `setup.sh regen`;
  never edit it. The canary-wap sketch is the opposite: it **is** the source
  (`platformio.ini` points `src_dir` at it).
- `build_matrix.json` and `flavors.json` are generated truth
  (`scripts/lint_build_matrix.py`, `scripts/flavor_envs.py --check-workflows`);
  `FEATURES.md` is narrative and may lag. `boards/boards.json` says
  `verified` vs `compile-tested`; a board is not promoted by hand.
- Every `FEATURE_*` flag must be read by code
  (`scripts/check_config_feature_flags.py`).

## Checks

- Host tests: `make -C firmware/tests_host` runs all; one test is its binary,
  `make -C firmware/tests_host build/test_<name>` then run it. Projects with
  their own suite have a `tests_host/` beside their sources. Build into
  `build/` only: a binary beside its source trips `scripts/lint_no_binaries.py`.
- `firmware.yml` builds every flavor and env. `canary-local.yml` compiles the
  display and Vision sources to WebAssembly, so an edit to those or to
  `common/` is also a Lab change with a committed dist to keep current
  (`projects/canary-display/AGENTS.md`).
- A version bump (`include/canary/version.h` in each project) moves the
  emulator dist, the firmware SBOM (`python3 scripts/gen_firmware_sbom.py`)
  and the six version strings `scripts/lint_fw_version_sync.sh` holds
  together. A release commit is always a dist commit.

## Lessons

`LESSONS_LEARNED.md` holds 169 dated entries. Grep it for the subsystem you
are touching (`rg <subsystem> firmware/LESSONS_LEARNED.md`); do not read it
whole. Append an entry when you pay for a new one.
