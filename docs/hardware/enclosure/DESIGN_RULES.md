# Enclosure design rules — the plastics-engineering checklist

Every shell in this folder is held to the rules below. This is the list a
packaging engineer runs down before a housing goes to tooling, translated
for FDM — where "tooling" is the print bed and the failure modes are layer
adhesion, overhangs and the first-layer squish rather than sink and draft.

Each rule names **where it is enforced**. A rule that lives only in this
document is a rule the next edit will break; the ones that matter are
asserts in the shared libraries (`canary_*_lib.scad`) or steps in
`.github/workflows/enclosure.yml`, and the catalog's own history is the
argument for that: the pan-head seat that shipped as a through-hole in four
cases was fine by every single-part check and was found only when a gate
put the two halves together.

Vocabulary: **base/back/body** is the half the boards live in, printed open
side up; **lid/front/face** is the show half, printed face-down; the **lip**
is the ring on the lid that nests into the base; **rim** is the base wall's
top face the lid seats on.

## 1. Walls and sections

| Rule | Number | Enforced by |
|---|---|---|
| Structural walls are whole extrusion counts, never thinner than three lines | 2.0 mm default, 1.2 mm floor | `core_wall()`, `core_min_wall()`, `core_selfcheck()`; `scripts/lint_design_lang.py` on every canonical default |
| Non-structural webs (a boss floor, a knockout web) are two lines minimum | 0.8 mm | `core_min_web()`; per-file asserts name the web |
| No feature narrower than one extrusion | 0.4 mm | `core_extrusion()`; the mark library refuses type below `mark_word_min_h()` |
| Seal-mode walls grow to host the groove with a full cheek each side | `gasket_w + 2 × 1.2` | `wall_eff` in every seal-capable file (CLR-7) |

## 2. Corners and stress risers

| Rule | Number | Enforced by |
|---|---|---|
| The floor-to-wall junction inside the cavity is coved, not square: a sharp inside corner along one layer boundary is where a flat-printed box hinges and cracks in a corner drop | 45° cove, 0.8 mm leg (`floor_cove`) | `cavity_cut()` in `canary_core_lib` — adopted by the WAP, Vision, Sense, doorbell, relay, combo and jbox shells; the WAP clears it along the battery bay and asserts the GPS pocket |
| Outside bottom edges get a chamfer (elephant-foot and the delamination site in compression) | 45° × 0.5 | `foot_chamfer_ring()`; `check_foot_relief.py` gates the C3/C6 bezels |
| Show-face edges are the house two-stage soft edge | 0.8 + 0.8 | `soft_edge_plate()`; `lint_design_lang.py` |
| Rib and rail roots are coved where a square root would read through the face or start a crack | 1.2 mm leg | `rib_root_fillet()`, `rib_root_fillet_rect()`, `board_rail(fil=)` |
| Corner screw posts are webbed into both walls with a constant-width gusset, not a flaring hull | landing width ≤ wall | `corner_gusset()`; adopters still on hand hulls are listed in the audit (AES rows) |

## 3. Ribs and bosses

| Rule | Number | Enforced by |
|---|---|---|
| Rib width is at most 0.8 × the wall it stands on, rounded to whole lines | `rib_t_max(wall)` | `wall_rib`, `board_rail`, `plate_ribs` asserts |
| Rib height is at most 9 × its width (slenderness) | `rib_h_max(t)` | same |
| A rib may reach down from a lid only as far as the headroom over the tallest component | `lid_rib_h ≤ headroom` | per-file asserts (WAP, Vision, doorbell, Sense) derived from each case's own `cav_*` arithmetic |
| A bare span longer than `rib_span_max` gets a rib row | `rib_count()` | `wall_rib_row()` |
| Boss tubes taller than 12 × their wall are tapered and webbed | `boss_h_max()` | `boss_tower()`; the DIN hub cover adopts it |
| A screw post is at least the pilot plus 3.0 in diameter, and an insert post the insert plus 1.2 a side | `scr_post_min()` | the screw registry; every screwed case derives `pd` from it |

## 4. Fasteners and service

| Rule | Number | Enforced by |
|---|---|---|
| One screw registry: pilot, nominal, clearance, both heads, insert, O-ring per size | M2 / M2.5 / M3 | `SCR_REGISTRY`, `core_selfcheck()` pins the print-validated M2 row |
| A head seat never goes as deep as the plate it is in; a pan head gets a floor, a flat head gets a 90° cone | ≥ 1.0 mm floor | `cb_head_pad()` + `cb_flat_cut()` / `cs_cone90_cut()`; the assembled fit check found the pad that overhung |
| Every screwed case offers a heat-set insert path, including the one screw undone at every service | `screw_insert` | WAP, Vision, doorbell (corner posts and the security boss), Sense |
| Snap-closed cases have a pry point | 0.6 in, 0.8 down | `pry_notch` on the watch drum, C6 and 1.69, positions derived and asserted off ports and corner radii |
| The lid fits one way (poka-yoke) — a symmetric post pattern gets a key | rib 3.0 × 0.8, slot +`tol_slide` | `lid_key_rib()` / `lid_key_slot()`; the fit check's `turned=true` control must **collide**, run in CI per family |
| The lip carries a lead-in so it finds the cavity blind | 45° × 0.4 on the tip | `lip_ring()` |
| Parting lines that are not designed seams get a shadow line | 0.6 deep × 0.8 tall | `seam_reveal_cut()` |

## 5. Snap fits

| Rule | Number | Enforced by |
|---|---|---|
| Insertion strain is computed from the beam's real numbers, with the measured board overwidth, and budgeted | 4.5 % once, lower for cycled snaps | `snap_boardclip(over=)`, `snap_strain()`, `snap_budget_once()` / `snap_budget_cycle()` — asserted on every render |
| The window derives from the ridge that parks in it, never typed twice | `snap_window()` | the C3/C6/1.69 files; `snap_selfcheck()` |
| Nothing roots under a clip beam (a floor rib there shortens the beam to ~1 mm) | — | the WAP's `_rib_hits_clip()`; the floor cove lies wholly inside the beam's own section, so it does not shorten it |
| Nub tips land where the strain assert says they do | `skirt_od/2 + snap_proud` | the watch station's tip cube (REV-1) |

## 6. Sealing, drainage and pressure

| Rule | Number | Enforced by |
|---|---|---|
| The gasket fills its groove between 80 and 95 % | `core_gasket_fill()` | asserted in every seal-capable file (CLR-8) |
| Gasket clamp spans stay under the clamp-spacing rule, or mid-span posts are added | ≤ 40 mm | `seal_mid_posts`; the relay and doorbell echo the span |
| A screw inside the gasket line is a drip path unless it is O-ring sealed | `head_seal` | `cb_oring_cut()`, `oring_gland_h()` squeezes 25 %; asserted pan-head only |
| A sealed box has a pressure path — a membrane seat or a weep — or it pumps past its gasket on every thermal cycle | — | asserted in seal mode in the WAP, Vision, doorbell and Sense (WTR-2) |
| Every cavity has a drain at its mounted low point, angled down and out | Ø2 weep | `weep_cut()`; `opt_weep` on every outdoor shell, the relay included (WTR-1) |
| Membrane seats are on the inner face; a spot-face on the show face is a cup | — | the relay lid (WTR-1) |
| Upward-facing penetrations stand on a washer land above the water film | 1.0 mm | `sma_boss` on the relay |
| Wall openings on an outdoor face get a drip awning that prints self-supported | 45° | `port_hood()`; `usb_hood` option |
| Panel beds and gutters drain over their stop, not against it | stop below the panel's top face; drain notches | the relay roof |

## 7. Openings

| Rule | Number | Enforced by |
|---|---|---|
| Openings center on the connector's axis, not on PCB-top plus half an opening | `port_usbc_shell_h()/2` | every USB opening (the assembly review) |
| A plug's overmold has the room it needs behind the wall | `port_usbc_overmold_*()`, `xiao_below` | the 1.69 (OPN-2), the stacked-XIAO cases |
| Bridges in an upright wall are chamfered to hold the print-validated flat span | ≤ 7.0 mm flat | `port_bridge_profile2d()`, `port_bridge_cham_for()` (OPN-1) |
| Horizontal bores print with a teardrop crown | — | `tearbore_x()` |
| Outdoor vent holes are insect-proof | ≤ 1.0 mm | `vent_hole_d` defaults; the README's outdoor rule |
| A radome is never in the quarter-wave band | avoid 0.7–1.1 mm | `radome_t` assert in the Sense and gang plate |

## 8. Print pose and first layers

| Rule | Number | Enforced by |
|---|---|---|
| Every part is modeled in its print pose, z = 0 the bed, and prints without support | — | `render.sh`; the catalog generator's print notes read the pose |
| Any grown feature that has no printable pose becomes its own part | — | the Vision hood (PRT-1) |
| Debossed marks and chamfers land on the first layers of a face-down part | — | `soft_edge_plate()`, `mark_*` |
| Coves and lead-ins are 45° so they self-support in either pose | — | `inner_cove_ring()`, `lip_ring()`, `lid_key_rib()` |
| Two different-material faces never share a plane (z-fight in the AR models) | — | the website's coplanar-guard test on the generated glTF |

## 9. Fit and clearance

| Rule | Number | Enforced by |
|---|---|---|
| One tolerance trio, tuned on the coupon | slide 0.20 / press 0.10 / hole 0.30 | `core_tol_*()`, `canary_fit_coupon.scad` |
| The two halves of every released case are intersected in their assembled position and must come out empty | `seat_lift` 0.1 | `canary_case_fitcheck.scad`, nine variants in CI |
| Board dimensions come from the registry, measured where measured | `brd_*()` | `canary_board_lib`, `board_selfcheck()` |
| Envelopes published to the website and the figures are measured off the committed meshes, never typed | — | `gen_assembled_dims.py`, `gen_figures.mjs --check`, the website's `ar-dims` test |

## 10. What is still open

- **Lid rib proportions.** Every lid's rib ring is pinned by a 1.0 mm
  headroom, now asserted; making the ribs taller means growing `cav_extra`
  on each case, which moves the released envelopes. A per-case decision.
- **Gusset adoption.** The released cases still draw their post gussets as
  hulls; `corner_gusset()` is the constant-width form. Mesh-moving on
  every released STL, so it waits for a release that re-cuts them anyway.
- **Hardware counts.** No file counts its screws and inserts; the one BOM
  quantity in a header has already drifted.
- **Customizer help text and naming collisions** — the audit's parametric
  UX section.
