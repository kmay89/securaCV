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
**id each of those files uses for one device**: no feature lists, no prose,
and — deliberately — no status. The one thing in it that is not an id is
`cad.params`, the board and module dimensions its case is built around, and
it holds those as the **owner**, not a mirror: a generator writes them into
the CAD — as numbers, or as references into the board registry that already
carries each board's evidence. Wave 1 ("Describe") of
[the roadmap's §4](../docs/IMPROVEMENT_ROADMAP.md) wrote the manifests and
the lint; wave 2 ("Consume") made the generators read
them — see "What the manifests drive" below for exactly which facts, per
generator; wave 3 ("Parametrize") put the manifest in front of the enclosure
chain — the `cad.params` rows below.

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
| `cad.params` | the literal Customizer knobs of `cad.scad` this device asserts — the board and module dimensions the case is built around (`board_l`, `vm_w`, `stack_sock_h`), as plain numbers, strings or booleans, or as **references into the board registry** (`{"brd": "xiao", "dim": "l"}` is `brd_l("xiao")`; `{"brd_fn": "brd_xiao_w_measured"}` is that measured fact) that the generator resolves from `docs/hardware/enclosure/canary_board_lib.scad` before it writes. Never a selector (`host`, `radar`, `preset`, `part` — chosen per printable set at render time), never a computed value, never a design-language knob (`wall_t`, `corner_r` — case-owned, with `deviates:` reasons, `lint_design_lang.py`) | [`gen_cad_params.py`](../docs/hardware/enclosure/gen_cad_params.py) **writes** each value into the `.scad`'s literal (the token on the knob's own line, nothing else) and its `--check`, which this linter also runs, proves the file still says it; the knob must be one `gen_builder_manifest.parse_scad` accepts; a reference must name a `BRD_REGISTRY` row and column or a `brd_*()` fact the library defines; manifests sharing one `.scad` may each assert a subset and must agree on shared keys |
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

**A knob that is a board fact references the registry; a knob that is a case
measurement is a number.** `docs/hardware/enclosure/canary_board_lib.scad`
already holds every board the catalog mounts, once, with its evidence rung
(`BRD_REGISTRY`: id, along-USB length, width, PCB thickness, status, note)
and the measured facts that refine a row, so a manifest does not retype
`21.0` — it names the entry:

```jsonc
"xiao_l": { "brd": "xiao", "dim": "l" },        // brd_l("xiao"); dim: l (along USB), w, t (PCB)
"xiao_w": { "brd_fn": "brd_xiao_w_measured" },   // a fact: function brd_xiao_w_measured() = 17.8;
"xiao_below": 5.5                                // a case measurement: no registry home, a number
```

`gen_cad_params.py` resolves a reference to the registry's number before it
writes, so the `.scad` still receives a literal and nothing at render time
changes; what changes is that a registry correction now reaches every
manifest-owned case — `--check` reports it as "registry says X, file says Y"
and a write carries it into the file. A reference is declared only where the
knob's help comment already cites the registry; a knob that merely equals a
row by coincidence (the WAP's `board_h` 1.2, the Sense's uncommented `pcb_t`)
stays a number. **A design decision is which registry entry the manifest
names:** the WAP and Sense clips name `brd_w("xiao")` (the 17.5 spec width —
the clips absorb the measured board) and the Vision pins name
`brd_xiao_w_measured()` (17.8); the registry states the truth, the manifest
states the decision, as the library's header asks each file to. The rows and
facts no manifest references (the doorbell's, the gang plate's, the display
cases') are printed by `--check` as INFO, never an error — those cases cite
the registry by comment and have no manifest yet. The library is read, never
written: correcting a board dimension is still an edit to the registry.

## Run the gate

```sh
python3 scripts/lint_device_manifests.py          # table, then errors; exit 1 on any
python3 docs/hardware/enclosure/gen_cad_params.py --check   # the cad.params half alone; a
                                                  # failure prints the regen order to follow
python3 docs/hardware/enclosure/gen_cad_params.py --dry-run canary-wap:board_w=17.8
                                                  # the diff a manifest edit WOULD write; nothing written
python3 scripts/regen_cad.py --check              # every generator downstream of a knob, in order
python3 -m unittest scripts/tests/test_device_manifests.py scripts/tests/test_gen_cad_params.py \
    scripts/tests/test_regen_cad.py -v
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
- **A shared case is owned as a union of subsets.** Several manifests may
  name one `cad.scad` (the three Vision hosts; the 7" Dash and Nightstand).
  Each asserts the knobs about *its* hardware in `cad.params` —
  `canary-vision` the stacked-XIAO, module and camera knobs,
  `canary-vision-devkit` its `dk_l` / `dk_w` / `stack_h` — the union is
  written, and a key two manifests both assert must agree or the gate names
  both. `canary-vision-xiao-s3` asserts nothing: same case, same host, so the
  union already covers it. Per-case design decisions stay per case, stated
  as which registry entry is named: `xiao_w` is `brd_w("xiao")` (the 17.5
  spec width) in the WAP and Sense clips and `brd_xiao_w_measured()` (17.8)
  in the Vision pins, and each manifest says exactly what its case does.

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
| [`docs/hardware/enclosure/gen_cad_params.py`](../docs/hardware/enclosure/gen_cad_params.py) (→ the case `.scad` literals) | `cad.params`: the board/module knobs of `cad.scad` — a number, or a reference the generator resolves from the board registry (`canary_board_lib.scad`, read and never written). The generator **writes** them into the file — the literal token on the knob's own line, nothing else — so the Customizer, `render.sh`, the fit check, the web builder's manifest and `lint_design_lang.py` all keep reading the same literal knob they always did; `--check` proves equality, and the first run over the committed tree changed zero bytes. A knob the builder's parser does not accept (computed, module-local, `[Hidden]`), a selector, a type mismatch, a two-knob line or a reference the registry does not define is refused by name. | walls, tolerances and every feature knob (the design-language canon — case-owned, explained with `deviates:`); the enum selectors (`host`, `radar`, `preset`, `part` — per printable set, in `render.sh` and the fit check); the doorbell (`canary_vision_doorbell.scad`: no manifest names it); computed knobs (`board_stack_h`) and knobs read from a registry (`canary_s3_lcd7.scad`'s panel record); and `envelope_mm`, which is never an input — every case derives its outer size from board dims + walls, the ledger measures it off the STL (`gen_assembled_dims.py`), and the manifest reaches it through `figure` |

**Still typed, deliberately:** the confidence ladder (derived from evidence
by the figures generator, never here), the emulator twin aliases in
`gen_flash.py` (`TWIN_ALIASES` — a per-product judgment that a sibling's
build shows the same face, not a device fact), and `firmware/build_matrix.json`'s
`board` / `mcu` cells (mirrored, and proven equal to the manifest by the
linter; making the matrix generator read them is a wave of its own).

## What comes next

- **Wave 3 — Parametrize (landed in part).** The manifest owns the board
  knobs of the released cases (`cad.params`, above): a dimension edit in a
  manifest is written into the `.scad` by `gen_cad_params.py`, and from
  there the chain that already exists re-renders the enclosure
  (`render.sh`), re-measures the envelope (`gen_assembled_dims.py`), redraws
  the figure (`gen_figures.mjs`) and re-carries the CAD ledger the website's
  AR models are pinned to (`gen_builder_manifest.py --site`). That order is
  one command — [`scripts/regen_cad.py`](../scripts/regen_cad.py) runs the
  twelve generators and gates in the order each one's inputs dictate, stops
  before the emulator dist rebuild when the figure headers moved (the
  rebuild is upstream of the catalogs; `--from gen_flash` resumes), renders
  the owed PNG previews of every part of every changed case with
  `--previews DIR`, and `--check` runs every step's check form and names the
  first stale one — and `gen_cad_params.py --dry-run SLUG:KNOB=VALUE` shows
  the one-line diff an edit would write before anything moves.
  `envelope_mm` is deliberately **not** an
  input — every case derives its outer size from board dims + walls and the
  ledger measures it — so `figure` stays the join to the AR model. A knob
  that is a board fact is a reference into the board registry
  (`canary_board_lib.scad`), so a registry correction flows to every owned
  case through the same generator. Still open in this wave: the display
  cases, whose `model` ternary and two-knob lines the generator refuses
  today; and the ledger keys the website derives its copy from. A real
  dimension edit still owes the render previews `AGENTS.md` requires with
  every `.scad` change — the generator lists the changed lines, and
  `regen_cad.py --previews` renders them, so that obligation is a list and
  a directory, not a memory.

## Adding a device

1. Write `devices/<slug>/device.json` — copy the closest sibling, change only
   the ids. Every `flavors.json` build env for the new board goes in
   `board.envs`; a CI-only env goes in `unclaimed.json` with its reason.
   A flasher product needs `board.flash_mb` (the flasher names the module
   from chip + flash size; `gen_flash.py` refuses a manifest without it).
   If the device has a case, `cad.params` may assert the board knobs it is
   built around — copied from the `.scad` **as they are today**, because the
   generator writes the manifest into the file: asserting a different number
   is a geometry change, and owes previews. A knob whose help comment cites
   the registry is written as the reference it cites (`{"brd": …, "dim": …}`
   or `{"brd_fn": …}`), which resolves to the same literal; a case
   measurement is copied as the number.
2. `python3 scripts/lint_device_manifests.py` until it is green. Each error
   names the file that owns the fact it disagrees with.
3. Then `python3 scripts/regen_cad.py --previews <dir>` — the regeneration
   order [`CLAUDE.md`](../CLAUDE.md) prescribes for the rest of the tree,
   as one command: the manifest-owned knobs into the `.scad`, the STLs, the
   assembled envelopes, the figures and their firmware / Swift mirrors, the
   two flashers' models, then — because a new board usually moves
   `flash.json`, `figures.json` and the `dist/`, and the rebuild is upstream
   of the catalogs — it regenerates the display sketch mirror
   (`firmware/projects/canary-display/setup.sh regen`) and **stops**: push
   the branch, dispatch Actions → "Rebuild emulator dist (pinned emsdk)" on
   it, pull, and `python3 scripts/regen_cad.py --from gen_flash` finishes
   the catalogs. Commit what moved; the PNGs in `<dir>` go to the requester,
   never into the tree. Before any of that, `gen_cad_params.py --dry-run
   <slug>:<knob>=<value>` shows the exact line a `cad.params` edit would
   move.
