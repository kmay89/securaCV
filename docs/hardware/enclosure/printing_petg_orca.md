# Printing the Canary enclosures in PETG — Bambu Studio / Orca

The companion to [`printing_petg_cura.md`](./printing_petg_cura.md) for the
slicer most people printing these parts actually open. **Bambu Studio and Orca
Slicer share this settings tree**, so one page covers both; where they differ,
the row says so.

Everything in this folder is designed to print **in PETG, flat, open side up or
face-down, with no supports, ever.** These are the values that get you there.

> **The reasoning lives elsewhere.** For *why* hotter prints stronger, where
> strength actually comes from, and when annealing helps, read
> [**Best-practice printing tips**](./printing_best_practices.md) — the
> slicer-agnostic companion. This page is just the numbers.
>
> **Setting up a new machine?** [`bambu_p2s_bringup.md`](./bambu_p2s_bringup.md)
> sequences this page into a first-print path.

- [The one idea](#the-one-idea-the-model-already-did-the-hard-part)
- [Porting from stock Bambu presets — the four rows that matter](#porting-from-a-stock-preset)
- [The settings sheet](#the-settings-sheet)
- [Cura ↔ Orca name map](#cura--orca-name-map)
- [Per-model cheat-sheet](#per-model-cheat-sheet)

---

## The one idea: the model already did the hard part

Identical to the Cura page, and it is the whole reason this catalog prints
cleanly:

- **Tolerance is in the STL, not the slicer.** Every fit runs off `tol_slide`,
  `tol_press`, `tol_hole` and `clip_clear`, tuned once on the
  [fit coupon](./canary_fit_coupon.scad). So **every XY compensation in the
  slicer stays at 0** — including the elephant-foot one Bambu ships enabled.
- **Elephant foot is a modeled chamfer.** The bottom edge carries a 45°
  `foot_cham`. If you *also* let the slicer shrink layer 1, you have corrected
  the same error twice and every press-fit is now loose.
- **Overhangs self-support.** Bores are teardropped, clip undersides are 45°.
- **Therefore: supports off.** If the overhang view lights up, the part is
  oriented wrong — re-seat it per the [cheat-sheet](#per-model-cheat-sheet).

---

## Porting from a stock preset

If you start from Bambu's **"Generic PETG"** filament and a stock quality
preset, these are the rows that are wrong *for this catalog* — fix these four
first, then work through the full sheet:

| Setting | Stock default | **Set to** | Why |
|---|---|---|---|
| **Elephant foot compensation** | 0.2 mm | **0** | The models carry a modeled chamfer. This is the double-correction that quietly loosens every press fit — and it is the one nobody thinks to check. |
| **X-Y hole compensation** | 0 | **0** (confirm) | Screw and magnet pockets are already sized by `tol_hole` / `tol_press`. |
| **X-Y contour compensation** | 0 | **0** (confirm) | Same, for outer contours. |
| **Enable support** | off | **off** (confirm) | Non-negotiable. |

---

## The settings sheet

For a **0.4 mm nozzle at 0.20 mm layers**, the security-build spec from the
[enclosure README](./README.md#engineering--materials-security-build). Rows
marked **[YOURS]** are printer- or filament-specific.

### Quality
| Orca / Bambu Studio | Value | Why |
|---|---|---|
| Layer height | **0.20 mm** | The design datum. 0.12–0.16 only for the finest A-surfaces. |
| Initial layer height | **0.24 mm** | Bonds PETG to the plate, hides minor unevenness. |
| Default line width | **0.42 mm** | Orca's Bambu profiles express line width slightly over nozzle Ø; walls are modeled as whole multiples of a 0.4 line, so keep every line width consistent rather than mixing. |
| **Elephant foot compensation** | **0** | See above. This is the important one. |
| **X-Y hole compensation** | **0** | Tolerance is in the model. |
| **X-Y contour compensation** | **0** | Tolerance is in the model. |
| Precise wall | On | Truer outer dimensions, which is what the fits depend on. |
| Seam position | **Back** | Hides the seam on the rear edge (faces point forward/down). |

### Strength — *the security-critical part*
| Orca / Bambu Studio | Value | Why |
|---|---|---|
| **Wall loops** | **4** | 1.6 mm of solid perimeter. Pry resistance comes from perimeters, not infill. **5** for sealed/weather builds. |
| Top shell layers | **5** (≈1.0 mm) | Pry-resistant lid skin. |
| Bottom shell layers | **5** (≈1.0 mm) | On face-down parts this is the show surface. |
| **Sparse infill density** | **30 %** | Backs the walls without wasting PETG or trapping heat. |
| **Sparse infill pattern** | **Gyroid** | Isotropic — no weak direction in a case that gets handled and pried. |
| Infill/wall overlap | **30 %** | Fuses infill to walls so the shell acts as one piece. |
| Infill wall order | Walls first | Truer outer surface. |

### Filament — PETG **[YOURS: dial to your spool's label]**
| Orca / Bambu Studio | Value | Why |
|---|---|---|
| Nozzle temperature | **240 °C** | Deliberately ~5–10 °C over a "safe" PETG number — interlayer adhesion is what makes the case tough. Range 230–250. |
| Nozzle temperature, initial layer | **245 °C** | Extra bite on layer 1. |
| Bed temperature | **80 °C** | 70–85 typical. |
| Bed temperature, initial layer | **85 °C** | |
| Flow ratio | **[YOURS]** — calibrate | Run the flow-dynamics calibration for the actual spool. Leave dimensional compensation out of it. |
| **Bed type** | **Textured PEI Plate** | Both the correct adhesion profile *and* our finish surface. |

### Cooling — *PETG is not PLA, do not blast it*
| Orca / Bambu Studio | Value | Why |
|---|---|---|
| Keep fan always on | On | |
| Fan speed (min / max) | **40 % / 40 %** | A flat 40 %. More cooling embrittles PETG layer bonds — bad in a structural, sometimes-sealed case. |
| Fan speed, initial layer | **0 %** | Let layer 1 weld to the plate. |
| Slow down for layer cooling | On, **10 s** | Auto-slows tiny features (coupon stations, bosses, the knob) so they don't melt. |
| Min print speed | 15 mm/s | Floor for that slow-down. |

> **ASA / PC / CF variants** (Vision bracket, sun-exposed outdoor): **close the
> door**, fan toward 0, print hotter. Everything else here holds.

### Speed **[YOURS: retraction is printer-specific]**
| Orca / Bambu Studio | Value | Why |
|---|---|---|
| Outer wall speed | **30 mm/s** | The face you see. |
| Inner wall speed | **45 mm/s** | |
| Sparse infill speed | 60–80 mm/s | Not load-bearing for our purposes. |
| Initial layer speed | **20 mm/s** | Adhesion and a flat foot. |
| **Retraction length** | **[YOURS]** — ~0.6–1.2 mm on Bambu's direct drive | The #1 anti-stringing knob and the #1 thing a shared profile can't get right for you. |
| Retraction speed | ~30–40 mm/s | |
| Z hop when retract | **On, 0.2 mm** | PETG blobs; hopping stops the nozzle dragging through them. |
| Avoid crossing walls | On | Keeps travel off visible faces. |

> **A P2S is fast, and these parts don't need it to be.** The stock speed
> presets are tuned for throughput on PLA. PETG welds better slower, and the
> A-surface is cleaner — the outer-wall row above is the one worth respecting.

### Support & adhesion
| Orca / Bambu Studio | Value | Why |
|---|---|---|
| **Enable support** | **Off** | Non-negotiable. |
| Brim type | **Outer brim only** | When you need it — see the cheat-sheet. |
| Brim width | **8 mm** (0 default) | Small-footprint / tall parts only. |
| Skirt loops | 2 | Primes the nozzle. |

---

## Cura ↔ Orca name map

Same idea, different label. Useful if you are translating the
[Cura sheet](./printing_petg_cura.md) yourself:

| Cura | Orca / Bambu Studio |
|---|---|
| Wall Line Count | Wall loops |
| Top/Bottom Thickness | Top / bottom shell layers |
| Infill Density / Pattern | Sparse infill density / pattern |
| Horizontal Expansion | X-Y contour compensation |
| Hole Horizontal Expansion | X-Y hole compensation |
| Initial Layer Horizontal Expansion | **Elephant foot compensation** |
| Z Seam Alignment = Rear | Seam position = Back |
| Combing Mode = Not in Skin | Avoid crossing walls |
| Minimum Layer Time | Slow down for layer cooling (layer time) |
| Build Plate Adhesion Type | Brim type / skirt loops |
| Generate Support | Enable support |

Note the third-from-last mapping in the compensation group: Cura's *Initial
Layer Horizontal Expansion* and Orca's *Elephant foot compensation* are the
same knob, and Cura ships it at 0 while Bambu ships it at 0.2 mm. That single
difference is why a profile that behaved in Cura can print loose fits here.

---

## Per-model cheat-sheet

Orientation, brim, and the one thing that bites. **Supports are off for every
row** — the orientation is what makes that true. The released parts here match
the [Cura page's table](./printing_petg_cura.md#per-model-cheat-sheet); this
adds the large in-development display cases, which are the parts where a Bambu's
bed size actually starts to matter.

| Part | Orient | Brim? | The one thing |
|---|---|---|---|
| **[Fit coupon](./canary_fit_coupon.scad)** | flat, as exported | no | **Print this first.** Render `base` and `mate` in PETG; the `strip` is **TPU** and must be a separate print from an external spool — don't plate `part="all"` as one job. |
| **WAP base / lid** | open-up / **face-down** | no | Face-down puts the chamfer and debossed label on layer 1. |
| **TPU gasket** | flat | **yes** | TPU 90–95 A, 2 walls, 100 % infill, ~25 mm/s, fan low. **External spool holder — never the AMS.** |
| **Sense radome front** | **face-down**, window flat | no | Uniform membrane, unfilled PETG/ASA only. No CF, no metallic paint, no foil label in front of the antenna. |
| **7" bezel** | **face-down** | no ¹ | ~208 mm across the ears — the largest part in the catalog. Clean plate; the window ledge prints on layer 1 and is the show surface. |
| **7" back tray** | outer face **down** | no ¹ | Keep the vent slots clear of any post-processing; they are the convection path, not decoration. |
| **7" corner gauge** | as exported | no | Print this **before** the two above. ~16.5 g vs ~158 g. |

¹ These are wide, flat, face-down parts — the shape most prone to lifting a
corner. Start without a brim on a clean plate; if a corner lifts, a 5 mm outer
brim is the fix, not a hotter bed.

---

## The importable profile

There isn't one for Orca yet — only the
[Cura profile](./printing_petg_cura.md#the-importable-profile) exists
(`profiles/securacv_canary_petg_security_0.4n_0.2mm.curaprofile`). The settings
sheet above is the source of truth in either slicer, and the values are few
enough to type in once.

If you build an Orca `.json` profile from this page and it prints well for you,
it would be a welcome contribution — with the same caveat the Cura profile
carries: **the sheet wins if the two ever disagree**, because the sheet is what
survives version and printer changes.
