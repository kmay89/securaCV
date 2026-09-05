# The Canary design language

One grammar for every case in the catalog and every surface that draws one.
This document is the index to that grammar — **every rule here names the code
that states it and the gate that enforces it**, because a rule that lives
only in prose is a rule the audit found broken fifteen ways
([`../hardware/enclosure/AUDIT_2026_09.md`](../hardware/enclosure/AUDIT_2026_09.md):
"rules were written and not enforced").

## The doctrine

A product line reads as one product line when it repeats a **small
vocabulary** — one radius family, one edge treatment, one parting-line
gesture, one fastener philosophy, one place the accent color is spent — and
when every departure from that vocabulary is *chosen*, visibly, with a
reason. So the grammar lives as **data the build checks**, not as taste:

- The house constants are functions in
  [`canary_core_lib.scad`](../hardware/enclosure/canary_core_lib.scad)
  (`core_corner_r()`, `core_face_edge()`/`core_face_edge2()`,
  `core_foot_cham()`, `core_wall()`, the `core_vent_*()` /
  `core_lightpipe_d()` feature vocabulary, the `core_tol_*()` fits).
- A case's Customizer knob stays a **literal** (a computed default would
  vanish from the Customizer and the website builder), so conformance is a
  lint: [`scripts/lint_design_lang.py`](../../scripts/lint_design_lang.py)
  fails the build on any canonical default that neither equals the house
  value nor carries a `deviates: <reason>` comment on its line — and on any
  case that redefines a module the libraries own. The deviation ledger
  therefore lives *in the case files, next to the numbers*.
- Run it from the repo root: `python3 scripts/lint_design_lang.py`. CI runs
  it in `enclosure.yml`.

## Form

| Gesture | The house value | Stated in | Gated by |
|---|---|---|---|
| Outside corner radius | 3.0 (palm shells); display frames scale up (4.3″ → 5.0, 7″ → 6.0), the doorbell is a deliberate 12 mm pill, the j-box a deliberate 2.0 | `core_corner_r()` | `lint_design_lang.py` |
| Show-face edge | two-stage 0.8 / 0.8 taper, both stages ON | `core_face_edge()`/`core_face_edge2()`, drawn only by `soft_edge_plate()` | lint + the taper-by-construction module (a square rabbet cannot be expressed) |
| Foot | 0.5 mm 45° relief on the bottom edge | `core_foot_cham()`, `foot_chamfer_ring()` | `check_foot_relief.py` hunts stepped feet |
| Parting line | a designed shadow: the drip skirt on weather builds, `seam_reveal_cut()` on indoor builds — never a raw Ø-for-Ø butt joint | `core_reveal_d()`/`core_reveal_h()` | the fit gate (`canary_case_fitcheck.scad`) proves the seam still closes |
| Walls | 2.0, with declared classes above it (snap shells 2.2, panel-scale 3.0, drop case 4.0) | `core_wall()` | lint |

## The feature vocabulary

The small repeated gestures that make the fleet read as a set. Geometry
lives in the library **once**; the knobs stay per-case:

- **Status light pipe** — Ø `core_lightpipe_d()` press-fit bore,
  `core_lightpipe_bore()`.
- **Vent/buzzer cluster** — GORE seat Ø `core_vent_pad_d()` over a ring of
  through-holes, `core_vent_cluster()`. Outdoor rule: holes ≤
  `core_vent_hole_d()` = 1.0 (insects), count recovers the open area.
- **Connector openings** — `canary_port_lib.scad` owns the numbers and the
  bridge-safe profiles; the insertion-length gate is part of the language
  (a case that blocks its own plug is not designed, whatever it looks like).
- **Hanging** — `canary_mount_lib.scad` owns the T-stud/keyhole standard;
  the GoPro hinge is Vision/Sense's shared gesture.
- **Fasteners** — `SCR_REGISTRY` in the core lib; heads seat in designed
  seats (`cs_cone90_cut` / `cb_head_pad`), never on bare face.

## Color

- **The registry is `CW_REGISTRY`** in
  [`canary_color_lib.scad`](../hardware/enclosure/canary_color_lib.scad),
  with its own executable doctrine (`color_selfcheck()`): one accent spend,
  ink contrasts, graphite first. It is carried to the website as
  `scad/colorways.json`, sha256-pinned.
- **There is one canary yellow** — the registry ink. The fleet-figure
  palette (`iso.mjs`) draws a deliberate *drawing* style (neutral body — a
  technical drawing is not a product render, see
  [`FLEET_FIGURES.md`](FLEET_FIGURES.md) §3), but its accent is pinned to
  the registry ink by `canary-local/tests/figures.test.js`.
- Color values cross surfaces as **sRGB hex, converted to linear
  explicitly per consumer** — the flasher GLBs learned this the hard way
  (`gen_device_glbs.mjs` stored sRGB in glTF's linear `baseColorFactor`
  and every preview rendered washed).

## Ecosystem surfaces

The same device is drawn by four renderers. Their jobs differ on purpose —
the drawings stay drawings — but where two surfaces claim the *same
physical thing* they must use the same numbers:

| Surface | Source of geometry | Source of color/material |
|---|---|---|
| Fleet figures (SVG, firmware glyphs, Swift) | massings pinned to committed STL boxes + measured assembled dims | `iso.mjs` MATERIALS (drawing palette; accent pinned to registry) |
| Flasher device GLBs (both trees) | same massings | same palette, linearized |
| Website AR models | CAD ledger (`cad-dims.json`) | colorway registry variants + the shared material roles (website repo, `scripts/lib/`) |
| Website showroom | live three.js builds | registry `shell`; stage materials aligned to the AR roles |

## Amending the grammar

Change the function in the library, run the lint, and fix or annotate what
it names — the same loop as any other gate here. A new case starts from the
house values and earns its deviations one `deviates:` line at a time.
