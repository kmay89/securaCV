# `devices/` — one manifest per Canary, every fact joined

A Canary device is described in seven places, each with its own generator and
its own byte-diff gate: the firmware envs (`firmware/flavors.json`,
`firmware/build_matrix.json`, `firmware/envs/platformio/*.ini`), the board
registry (`firmware/boards/boards.json` + `boards/<id>/pins/pins.h`), the
emulator (`canary-local/emulator/build.sh` + `dist/*.meta.json`), the fleet
figures and their confidence ladder (`canary-local/devices/figures.json`), the
enclosure CAD (`docs/hardware/enclosure/*.scad` +
`canary-local/devices/enclosures.json`), the flasher catalog
(`canary-local/devices/flash.json`) and the website (its page and glTF
model). The gates are good. What was missing is the **join** — nothing said
that *this* env, *this* pins header, *this* figure, *this* flasher product and
*this* case are the same object.

`devices/<slug>/device.json` is that join, one file per device. It carries the
**id each of those files uses for one device** and nothing else: no dimensions,
no feature lists, no prose, and — deliberately — no status. This is wave 1
("Describe") of [the roadmap's §4](../docs/IMPROVEMENT_ROADMAP.md).

## The manifest

Validated by [`device.schema.json`](device.schema.json) (JSON Schema 2020-12;
the linter carries its own stdlib validator because CI has no `jsonschema`).

| Key | What it names | Proven against |
|---|---|---|
| `slug` | the directory name; where a `build_matrix.json` or `flash.json` product names this hardware, the same id | the directory; uniqueness |
| `name` | the product name as the flasher shows it | (copy — not linted) |
| `family` | the firmware product it is built from | `firmware/flavors.json` `name` |
| `board.mcu` · `psram_mb` · `flash_mb` | the silicon | `boards.json` row for `board_id` |
| `board.board_id` | the `firmware/boards/<id>/` registry row | `boards.json`; each env's resolved `board =` is that row's `pio_board`; each env's `-I boards/<id>/pins` include is this id (or a variant) |
| `board.variants` | SKU siblings of the same panel module whose pins headers some envs compile (the Dash 4.3 / 4.3B / 4.3C) | `boards.json`; same MCU |
| `board.envs` | every `[env:NAME]` that targets this hardware | the family's `platformio.ini` with `extra_configs` resolved; claimed by exactly one device; every `flavors.json` build env is claimed or explained in [`unclaimed.json`](unclaimed.json) |
| `peripherals` | lower-cased `HAS_<NAME>` flags | `#define HAS_<NAME> 1` in `boards/<board_id>/pins/pins.h` |
| `figure` | the fleet figure that draws this hardware | `figures.json`: role `device`, same family, agrees with its `hardware` map for `board_id` and with every `flash.json` product that draws this device |
| `cad.scad` · `cad.enclosure_sets` | the enclosure source and the printable sets | the file exists; sets exist in `enclosures.json`; the scad is the source of one of them |
| `emulator.flavor` | the browser twin | `build.sh` allowlist; `dist/canary-display-<flavor>.meta.json` exists; `build.sh` compiles this device's pins dir for that flavor; every dist display flavor is claimed |
| `flasher.product` · `flasher.variants` | the browser-flasher catalog entries that install onto this hardware | `flash.json`: exists, `chip` == `board.mcu`, `tier.board_id` is this board, `flash_mb` agrees; every catalog product is claimed exactly once |
| `site.page` · `site.model` | the website page and AR model | **shape only** — those files live in the `securacv_website` repository and cannot be verified from here |

Plus a cross-file rule: every `firmware/build_matrix.json` product has a
manifest (by id, else by `flavor` + env), and its `board`, `mcu` and env(s)
agree with it.

**The confidence ladder is never typed here.** `status`, `confidence` and
`tier` are rejected by the schema (`additionalProperties: false`). The
verdict for a device is `figures.json`'s `confidence` for its figure —
derived from evidence on disk (committed STLs, a firmware config, a released
catalog variant) — and `scripts/lint_device_manifests.py` prints it in its
table. A device with no figure yet shows `—`; that is the gap, honestly.

**A key whose value is unknown is omitted, not guessed.** No figure has been
drawn for the four `canary` boards or the Nightstand C6; the Glance AMOLED has
no enclosure yet; the C3 Super Mini has neither. Those keys are absent.

## Run the gate

```sh
python3 scripts/lint_device_manifests.py          # table, then errors; exit 1 on any
python3 -m unittest scripts/tests/test_device_manifests.py -v
```

CI runs the first in `.github/workflows/lint.yml` (Repo Lints) beside
`lint_build_matrix.py`, and the second through `unittest discover -s
scripts/tests`. The lint is grep-grade: it reads the ini files itself (no
PlatformIO) and the emulator's `build.sh` (no emsdk).

## Decisions the manifests encode

- **One device = one piece of hardware.** The three `canary` reach ports
  (ESP32-CAM, WROOM DevKit, Freenove S3) are separate devices, as
  `build_matrix.json` already treats them; so are the four Vision hosts (XIAO
  C3, C3 DevKit, C3 Super Mini, XIAO S3) — each has its own pins header and
  its own flasher product. `canary-vision` is the XIAO C3 host because that is
  the board `build_matrix.json` describes under that id; the flasher product
  literally named `securacv-canary-vision` is the DevKit build and belongs to
  `canary-vision-devkit`.
- **Feature-variant envs stay with their hardware.** The Dash's
  `dash-b/-rs485/-can/-vault/-sd/-rtc/-espnow/-modes/-mic/-ble5` envs are one
  device with `board.variants` naming the 4.3B and 4.3C pins headers they
  compile; `peripherals` are proven against the plain 4.3 only, so the
  mic-bearing 4.3C's microphone is deliberately not listed on the Dash. The
  same 7" board carries both `canary-display-dash7` and
  `canary-display-nightstand7` as distinct products (the OTA engine must never
  cross-grade them), so they are two manifests sharing a `board_id`, a figure
  and a case.
- **`board.envs` = what CI builds for this hardware plus what ships.** The
  `flavors.json` `build_envs` the device claims, plus an env
  `build_matrix.json` / `flash.json` name as the published image
  (`release_ha` on the flagship). Debug-only envs (`*-debug`, `minimal`,
  `standalone`) that CI does not build are not listed.
- **`peripherals` come from the registry's pins header**, even for the
  `canary` and `canary-wap` trees, whose builds carry their pins in build
  flags and the sketch rather than compiling `boards/<id>/pins/pins.h`. The
  header is still the declared capability map for that board
  (`firmware/scripts/check_board_registry.py` keeps it honest).
- **Website paths are declared, not verified.** Only the three glTF models
  that exist today (`canary-vision`, `canary-sense`, `canary-watch`) are
  named; no per-device page exists yet, so `site.page` is absent everywhere.

## What the manifests do NOT drive yet

Nothing reads them but the linter, so nothing existing can break. The
roadmap's next waves, one generator per PR with the byte gates proving the
output did not move:

- **Wave 2 — Consume.** `canary-local/tools/gen_flash.py` reads `board` and
  `flasher` instead of its own `PRODUCTS` table;
  `canary-local/tools/figures/gen_figures.mjs` reads `figure`; `scripts/lint_build_matrix.py` requires
  every env to be claimed here (today that rule lives in this linter).
- **Wave 3 — Parametrize.** `cad.params` and an `envelope_mm` threaded into
  the SCAD sources and the website's glTF generators, so one dimension edit
  re-renders the enclosure, the AR model and the figure together. That wave
  needs the render previews `AGENTS.md` requires with every SCAD change.

## Adding a device

1. Write `devices/<slug>/device.json` — copy the closest sibling, change only
   the ids. Every `flavors.json` build env for the new board goes in
   `board.envs`; a CI-only env goes in `unclaimed.json` with its reason.
2. `python3 scripts/lint_device_manifests.py` until it is green. Each error
   names the file that owns the fact it disagrees with.
3. Then the regeneration order [`CLAUDE.md`](../CLAUDE.md) already
   prescribes for the rest of the tree — `./setup.sh regen` → dispatch the
   emulator dist rebuild → pull it → run the `gen_*.py` catalogs → commit —
   because a new board usually moves `flash.json`, `figures.json` and the
   `dist/`, and the rebuild is upstream of the catalogs.
