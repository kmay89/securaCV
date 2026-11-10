# Improvement roadmap — the 2026-09 three-repo audit (closed)

The 2026-09-02 audit of this monorepo, the HACS mirror and the website
produced an ordered list of 60 items. **All 60 have landed**; `CHANGELOG.md`
records each, and the audit's full text, with its wave-by-wave account, last
stood at commit `018ee0e`. Open work now lives in
[`audit/repo_todo_sweep_2026-09.md`](audit/repo_todo_sweep_2026-09.md).

Two sections are kept because other docs point at them: the device package's
design as it landed (§4), and the Wi-Fi sensing path, whose bench steps are
still ahead (§5).

---

## 4 · The device package: one manifest, every surface

This is the largest single cohesion gap and the one the September audit was
asked about directly. A Canary product today is described in at least seven
places that nothing joins:

| Surface | Where the facts live today | How it is refreshed |
|---|---|---|
| Firmware build envs | `firmware/envs/*.ini`, `flavors.json`, `build_matrix.json` | by hand; `lint_build_matrix.py` checks two of the three agree |
| Emulator flavor | `canary-local/emulator/build.sh`, `dist/*.meta.json` | the pinned-emsdk rebuild workflow |
| Enclosure CAD | `hardware/enclosure/*.scad`, `gen_stamp.py`, `gen_builder_manifest.py` | by hand, gated by byte diff |
| 3D / AR model | website `scripts/make-*-glb.mjs`, `scad/cad-dims.json` | weekly carry of the CAD ledger only |
| Fleet figures | `canary-local/devices/figures.json`, `gen_figures.mjs` | reads STL bounding boxes; gated |
| Flasher catalog | `canary-local/tools/gen_flash.py` → `flash.json` | reads `dist/*.meta.json`; gated |
| Website copy | product pages, glossary, `llms-full.txt` | by hand |

Each row has its own generator and its own gate, and the gates are good. What
is missing is the **join**: the fact that the Doorbell is 92 × 38 × 24 mm, has
an S3 with 8 MB PSRAM, an ES7210 mic and a WS2812 ring, builds from
`canary-doorbell-s3` and is `confirmed` on the confidence ladder, is not
written down once. It is written down seven times, in seven schemas.

### Proposal — as it landed

A `devices/<slug>/device.json` per product, validated by one JSON Schema
(`devices/device.schema.json`; the how-to is `devices/README.md`). The
original proposal here carried a `status` and an `envelope_mm`; the schema
as it is has neither, on purpose, and the sample below is the WAP's manifest
as it stands, not a sketch:

```jsonc
{
  "slug": "canary-wap",
  "name": "Canary WAP",
  "family": "canary-wap",                        // the firmware/flavors.json product
  "board": { "mcu": "ESP32-S3", "psram_mb": 8, "flash_mb": 8,
             "board_id": "xiao-esp32s3-sense",   // the firmware/boards/<id>/ registry row
             "envs": ["canary-wap-default", "canary-wap-usbdrive"] },
  "peripherals": ["camera", "microphone", "sd_card", "gnss_uart", "tamper_input"],
  "figure": "device.canary-wap",                 // the fleet figure — and, through it, the envelope
  "cad": {
    "scad": "docs/hardware/enclosure/canary_wap_enclosure.scad",
    "enclosure_sets": ["wap-compact", "wap-battery", "wap-weather", "wap-clip-coupon", "thermal-outdoor-kit"],
    "params": {
      "board_l": { "brd": "xiao", "dim": "l" },  // a board fact: brd_l("xiao") in canary_board_lib.scad
      "board_w": { "brd": "xiao", "dim": "w" },  // the 17.5 spec width — the clips' decision, stated
      "board_h": 1.2,                            // a case measurement: a number
      "board_clear": 0.6, "stack_camera": 8.0, "stack_plain": 4.5
    }
  },
  "flasher": { "product": "securacv-canary-wap" }
  // "emulator": { "flavor": "watch" }           — display devices only (the browser twin)
  // "site":     { "model": "models/canary-vision.glb" } — declared, not verifiable from here
}
```

- **No `status`.** The confidence ladder is derived from evidence on disk by
  the figures generator and rejected by the schema (decided in wave 1;
  `scripts/lint_device_manifests.py` prints the derived verdict).
- **No `envelope_mm`.** The outer size is reached through `figure`,
  measured, never typed: every case derives it from board dimensions plus
  walls, `gen_assembled_dims.py` measures the assembled union off the
  STLs, and the ledger carries it (`figures.json`, `cad-dims.json`) — which
  is what the website's model tests already pin to. Wave 5 took this as a
  default; a linter-asserted mirror in the manifest is a reversible later
  step, and nobody has asked for one.
- **`cad.params` in two forms.** A board fact is a reference into the board
  registry that already carries each board's evidence rung; a case
  measurement with no registry home is a number. Never a wall or a
  tolerance (the design-language canon stays case-owned, with `deviates:`
  reasons), never a selector (`preset`, `host`, `part` — chosen per
  printable set at render time).

What the generators do with it — what actually landed, per wave, against
the original list:

- `gen_flash.py` reads `board` and `flasher` and stopped inferring chips
  (wave 2, as proposed);
- `gen_figures.mjs` builds the hardware→figure map from the manifests
  (wave 2) — it never reads a status or an envelope from them: the ladder
  stays derived and the envelopes stay measured (`docs/design/FLEET_FIGURES.md`
  §5);
- `lint_build_matrix.py` resolves every matrix lane to one manifest through
  the shared `scripts/_device_join.py` (wave 2, as proposed);
- `gen_cad_params.py` **writes** `cad.params` into the case `.scad` and
  `--check`s it (wave 3) — **not** the proposed "the SCAD is parametric from
  the manifest": the file stays a literal Customizer knob, nothing reads a
  manifest at render time, and every consumer of that literal-knob contract
  (`gen_builder_manifest.py`, `gen_stamp.py`, `gen_enclosures.py`,
  `render.sh`, the fit check, `lint_design_lang.py`) is untouched;
- `gen_builder_manifest.py --site` carries the resolved knobs, the measured
  seams and the board registry into the website's `cad-dims.json` beside the
  envelopes it always carried (wave 3); the glTF generators pin to the
  measured envelope and, since website PR #197 (2026-09-20), place seams and
  modules from `seams_mm` and `knobs` and pin page copy to `board_registry`
  (the website's `scripts/lib/cad-dims.mjs`, held by its
  `tests/models.test.mjs` and `tests/ar-dims.test.mjs`);
- the emulator `build.sh` still types its flavor allowlist — the manifests
  **prove** it (every `emulator.flavor` is in the allowlist and every dist
  flavor is claimed) rather than drive it; the boot probe loops over the
  dist metadata (item 43, landed).

The gates stay exactly where they are; they have a common upstream. A new
device is: write one manifest, `python3 scripts/regen_cad.py --previews
<dir>` (the manifest into the `.scad`, the STLs, the envelopes, the figures
and their mirrors, the flashers' models, the sketch mirror — then it
**stops**), dispatch the dist rebuild and pull it, `python3
scripts/regen_cad.py --from gen_flash` for the catalogs, commit — the order
`CLAUDE.md` prescribes, now runnable, with one file at the front instead of
five.

### Migration in three waves

The counter here is the device package's own (`devices/README.md` uses it);
the PR waves in §3 are a different count.

1. **Describe** (S, **landed** — the follow-up wave, PR #1635: `devices/` and
   `scripts/lint_device_manifests.py`): write the manifests for the five
   shipping and confirmed devices from the facts as they stand; add the
   schema and a lint that every manifest validates and every `flavors.json`
   entry has one. Nothing consumes them yet, so nothing can break.
2. **Consume** (M, **landed** — PR wave 3, #1664): `lint_build_matrix.py`,
   `gen_flash.py` and `gen_figures.mjs` read the manifests, with the byte
   gates proving no generated output moved (`flash.json` and `figures.json`
   byte-identical); the leftovers are listed on row 21.
3. **Parametrize** (L, **landed in part** — PR wave 5, #1682): the
   manifest owns the board knobs its case is cut around, and a dimension
   edit is one manifest line that `gen_cad_params.py` writes into the
   `.scad`, from which the chain that already exists re-renders the
   enclosure, re-measures the envelope, redraws the figure and re-carries
   the ledger the AR model is pinned to — `scripts/regen_cad.py` is that
   chain as one command. It landed with **zero `.scad` bytes moved**, so
   no previews were owed; the rehearsal (one WAP knob, 46 files, 26
   previews, reverted) is the proof the obligation is automated
   (`--previews DIR`), and the first real edit is the one that pays it.
   What is left, honestly (wave 6 closed three of these): the **doorbell**
   is owned through `cad.also` on `devices/canary-vision` (one manifest, two
   case files of the same build; zero doorbell bytes moved); the
   **Nightstand C6**'s `model` ternaries were dead branches behind
   `assert(model == "1.47")` and became the 1.47 literals its manifest owns
   as `ws147` references; the **Touch 1.69**'s two-knob line is split and
   both offsets owned; the **7" frame** still reads its panel record from
   `canary_panel_lib.scad` through `panel_variant` — computed `pnl_*(PANEL)`
   reads of a positional list literal in `gen_stamp`-hashed files, outside
   the writer's grammar (`devices/README.md` "What a manifest cannot yet
   own"; the smallest follow-up is a `cad.panel` join, a separate decision);
   the **selectors** (`host`, `preset`, `radar`,
   `headers`, `port`, `model`, `panel_variant`, `part`) are chosen per
   printable set in `render.sh` — making them manifest-owned is the
   optional **render-plan** package's selector half (option A), which
   rewrites `render.sh`, `gen_assembled_dims.py` and enclosure CI and was
   not built because it needs the maintainer's yes; its Lab-card half
   landed in PR #1703 as option B (maintainer to confirm): the manifests'
   `cad.enclosure_sets` home every enclosure set, so the five display
   cards the name regex had put on the wrong page moved to the devices
   whose manifests list them; **walls and
   tolerances** stay case-owned by the design-language canon; the
   **sketch envelopes** of devices with no committed CAD stay in
   `massing.mjs` (the Watch's figure left them in PR #1703, maintainer to
   confirm: it is measured off its assembled CAD, `dims_source: assembled-cad`, as the Dash's now
   is, so a knob edit reaches either figure through
   `gen_assembled_dims.py`); and the **website's
   half** — its glTF generators and copy reading `knobs`, `seams_mm`,
   `board_registry` and `board_facts` from the carried ledger — is the
   website repo's change, merged as website PR #197 on 2026-09-20 (its
   `make-*-glb.mjs` and `cad-dims.mjs` read `seamsMeters`, `knobMeters` and
   `boardMm`; the models are byte-identical).

---

## 5 · Wi-Fi sensing: from compiles to senses

The September pass made the CSI stack portable and correct in its indexing;
it did not make it *validated*. The path from here, in order:

1. **Transmitter filtering** (P0 item 2) — **landed** 2026-09-08, host-tested:
   one link per window (the associated router's BSSID, plus registered peers
   on the WAP), with `frames_dropped_foreign` saying how much of the
   neighborhood was kept out. Step 3 is what shows the false-positive floor
   actually moved.
2. **AGC-aware envelope and fixed-cadence Goertzel** (items 3 and 4) — **landed** in the follow-up wave, host-tested on synthetic frames; step 3 is what makes it a claim about a room.
3. **Bench pass on three boards** — S3, C3 and C6 (the HE path is compile-only
   today), following [`csi_quickstart.md`](csi_quickstart.md), with the
   `supply` object confirming which traffic source fed each window. Record
   thresholds in `docs/csi_modules.md` per board, since the C6's HE-LTF
   changes the noise floor.
4. **Wander and jitter features** in the style of espressif/esp-radar —
   **host half landed** (wave 8, PR #1722). `csi_wander_jitter.h` gives our
   own within-window definitions (standard deviation of the amplitude
   centroid → `v[28]`, mean frame-to-frame |Δ| of the normalized row →
   `v[29]`) behind `CSI_WANDER_JITTER`, which is 0 in every shipped build.
   CI compiles the flag-on path for the S3 on the canary-wap's second
   Arduino pass. Host-tested on synthetic frames and read by no module;
   both rise with front-end noise as well as with motion, and both
   still-room floors rise on a weak link, so neither is a motion-only
   reading. esp-radar ships its metric math as a binary, so equivalence,
   thresholds and any cost comparison with the variance stack are the bench
   half, after step 3 (recipe in
   [`csi_modules.md`](csi_modules.md#the-second-extractor-wander-and-jitter)).
5. **Multi-device fusion** — `core.multilink_fusion` exists and is
   host-tested; feeding it real peers still waits on item 2: nothing fills
   the probe's peer table yet, so the WAP probes broadcast-only at 10 Hz,
   ~0.8 % by the airtime governor's framed estimate. The airtime accounting
   has landed: every probe send reserves through `airtime_governor` (sweep
   F4, #1696) against a window that holds every send at any rate, and the
   probe stops at 1.60 % of it so the heartbeat and presence keep the rest
   of the 2 % routine cap (sweep F51, PR #1722; host-tested). When the table
   fills, an over-budget probe arrives in bursts (host-measured about 1.3 s
   of frames per 10 s window at 8 peers) unless `aggregate_cap_hz` is set
   from the budget — that is this step's call.
6. **802.11bf** stays a watch item: no IDF release exposes it, and the research
   note says why the project should not build on a draft.

Everything in this section stays `compile-tested` until step 3 is written up
in [`V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md) with the board, the
firmware sha and the numbers.

---
