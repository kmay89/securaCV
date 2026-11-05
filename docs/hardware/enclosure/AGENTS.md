# Enclosure CAD — the brief for this tree and for `devices/`

Loads when you touch a file here; the root `AGENTS.md` still applies. The
human guide is [`README.md`](README.md) (151 KB: open the section you need,
"Preview renders" above all).

## Three rules

1. **A board knob lives in the manifest, not the file.** The board and module
   dimensions a case is cut around (`board_l`, `vm_w`, `stack_sock_h`, …) are
   owned by `devices/<slug>/device.json` `cad.params`, and `gen_cad_params.py`
   writes them into the `.scad` (`--dry-run <slug>:<knob>=<value>` prints the
   one-line diff first). A board fact is a reference into
   `canary_board_lib.scad`'s registry: correct the registry row and every
   owned case follows. Walls, tolerances and the `preset` / `host` / `part`
   selectors stay in the file (`scripts/lint_design_lang.py`). `cad.also` lets
   one manifest own a second case of the same build (the Vision's doorbell),
   so a Vision knob edit writes both. How-to: `devices/README.md`.
2. **Every `.scad` change ships PNG previews of every affected part**, shared
   with the requester, never committed. `regen_cad.py --previews <dir>`
   renders them; the recipe is the README's "Preview renders".
3. **A part whose printed shape moved gets a revision.** `gen_revisions.py`
   (after `render.sh`) bumps `rN` and records what moved in
   `canary-local/devices/revisions.json`; its `--check` refuses an unrecorded
   move in `lint.yml` and `enclosure.yml`. A comment-only edit does not move
   it; a post moved 1 mm does.

## The chain, as one command

`python3 scripts/regen_cad.py --previews <dir>` runs every step in the order
derived from what each generator reads (`--check` names the first stale step,
`--from <step>` resumes, `--list` prints them):

```
device.json cad.params → gen_cad_params.py → lint_design_lang.py → render.sh --no-png
  → gen_assembled_dims.py → gen_hardware.py → gen_assembly_poses.py → gen_enclosures.py
  → gen_figures.mjs → gen_device_glbs.mjs
  → firmware/projects/canary-display/setup.sh regen (when fleet_figures*.h moved) — STOP
  → dispatch the emulator dist rebuild, pull it
  → gen_flash.py → gen_builder_manifest.py [--site <website>] → gen_print_files.py --site <website>
  → gen_stamp.py --check, gen_mark_svg.py --check (report only; a STAMP_REV bump is a human's call)
```

OpenSCAD is 2021.01. STL bytes are not deterministic, so nothing byte-gates
them: the two steps after `render.sh` gate bounding boxes and seams, and
`enclosure.yml` re-renders and greps its log. No Actions button renders STLs
and pushes them back.

A re-exported STL moves the fleet figures (`docs/design/FLEET_FIGURES.md`).
The generator refuses a figure that drifted from its part, or one that puts
two materials on one plane; both are real defects. An idea renders as a
dashed ghost by construction, because the confidence ladder in
`canary-local/devices/figures.json` is derived, never hand-written.

**A generator that cannot spell a value must refuse, not pass.** When you
write one with a `--check`, enumerate what the write path cannot express (an
exponent-form float, NaN, a string holding a comment opener, a two-statement
line) and make the check fail on it. The one that once passed green:
[`docs/GENERATED_FILES.md`](../../GENERATED_FILES.md).
