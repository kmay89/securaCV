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
no feature lists, no prose, and — deliberately — no status. Wave 1
("Describe") of [the roadmap's §4](../docs/IMPROVEMENT_ROADMAP.md) wrote the
manifests and the lint; wave 2 ("Consume") made the generators read them —
see "What the manifests drive" below for exactly which facts, per generator.

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
PlatformIO) and the emulator's `build.sh` (no emsdk). The ini resolver and
the build-matrix→manifest resolution live once, in
[`scripts/_device_join.py`](../scripts/_device_join.py), and both lints
import them — `lint_build_matrix.py` applies the matrix's side of the join
(every lane resolves to one manifest; every manifest env is a real
`[env:NAME]`) so the two gates cannot disagree about which device a lane is.

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

## What the manifests drive

Wave 2 ("Consume") pointed the generators at the manifests one at a time,
with each generator's byte gate proving its output did not move. Honestly,
per generator — what is read from here, and what is still typed elsewhere:

| Consumer | Reads from the manifest | Still its own |
|---|---|---|
| `canary-local/tools/gen_flash.py` (→ `flash.json`) | which device each flasher product installs onto (`flasher.product` / `variants` — a product no manifest claims refuses to generate); that device's `board.mcu` (the chip guard), `board.flash_mb`, `board.board_id` (the support tier, re-checked against `boards.json`'s `mcu`), and its `family` → the PlatformIO project. The build's pins header must be the manifest's `board_id` or a listed variant, and a resolved figure must equal the manifest's `figure`. Loader: [`canary-local/tools/_devices.py`](../canary-local/tools/_devices.py). | the human copy, `asset_stem`, `env`, the flasher's own `family` grouping, the PlatformIO `board` (still re-derived from the firmware tree and cross-checked); one `board_id` override for a product that installs onto a `board.variants` sibling (the modes build on the 4.3B); the WAP's `figure`, declared because its sketch compiles no pins header (kept in the table so `figure.via` stays a true sentence — checked against the manifest). |
| `canary-local/tools/figures/gen_figures.mjs` (→ `figures.json`, `fleet_figures.h`, `FleetFigures.swift`, …) | the **hardware→figure map** (`hardware.mapped`, `figure_for_hardware()`): a manifest's `figure` draws its `board.board_id` — and only that; `variants` stay unmapped as different housings; two manifests drawing one board differently fail the build. | the coarse config→device-type map (`CONFIG_FIGURE`, feeding `device_types` / `configs_audit`), because a config directory is not one board and the WAP's envs compile no `configs/` include. The manifests **validate** it in both directions (every row backed by a device with that figure; every typed config a drawn device compiles has a row) and record its one **dispute**: `canary-vision/default` is compiled by the DevKit (own figure since its housing was traced) and the two XIAO hosts (the stacked-XIAO figure), all publishing device type `canary-vision`; the row keeps the XIAO figure pending a decision (unmapping the type moves the firmware and Swift tables). |
| `scripts/lint_build_matrix.py` | every `build_matrix.json` lane resolves to one manifest (by id, else flavor + env — the same function this linter uses); every manifest's `board.envs` is an `[env:NAME]` of its family's project | the feature-flag cells, which are `platformio.ini` / `canary_config.h` facts, not device facts |
| `.github/workflows/firmware-release.yml`, `flasher-release.yml` | (not the manifests — `firmware/flavors.json` `release_envs`, via `flavor_envs.py --release --json`; roadmap item 23, landed in the same wave: neither workflow types a display env any more) | — |

**Still typed, deliberately:** the confidence ladder (derived from evidence
by the figures generator, never here), the emulator twin aliases in
`gen_flash.py` (`TWIN_ALIASES` — a per-product judgment that a sibling's
build shows the same face, not a device fact), and `firmware/build_matrix.json`'s
`board` / `mcu` cells (mirrored, and proven equal to the manifest by the
linter; making the matrix generator read them is a wave of its own).

## What comes next

- **Wave 3 — Parametrize.** `cad.params` and an `envelope_mm` threaded into
  the SCAD sources and the website's glTF generators, so one dimension edit
  re-renders the enclosure, the AR model and the figure together. That wave
  needs the render previews `AGENTS.md` requires with every SCAD change.

## Adding a device

1. Write `devices/<slug>/device.json` — copy the closest sibling, change only
   the ids. Every `flavors.json` build env for the new board goes in
   `board.envs`; a CI-only env goes in `unclaimed.json` with its reason.
   A flasher product needs `board.flash_mb` (the flasher names the module
   from chip + flash size; `gen_flash.py` refuses a manifest without it).
2. `python3 scripts/lint_device_manifests.py` until it is green. Each error
   names the file that owns the fact it disagrees with.
3. Then the regeneration order [`CLAUDE.md`](../CLAUDE.md) already
   prescribes for the rest of the tree — `./setup.sh regen` → dispatch the
   emulator dist rebuild → pull it → run the `gen_*.py` catalogs → commit —
   because a new board usually moves `flash.json`, `figures.json` and the
   `dist/`, and the rebuild is upstream of the catalogs.
