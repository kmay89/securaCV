# Printing the Canary enclosures in PETG — a Cura guide

Everything in this folder is designed to print **in PETG, flat, open side up
or face-down, with no supports, ever.** This page is how you get a clean,
strong, dimensionally-correct result out of **Ultimaker Cura** (any 5.x; the
values port back to 4.x and forward to newer releases by hand).

Read it once, print the [fit coupon](./canary_fit_coupon.scad), and every part
in the [catalog](./README.md) prints the same way afterwards.

> Want the *reasoning* behind these numbers — where strength comes from, why
> hotter prints stronger, when annealing helps or hurts — rather than just the
> values? See [**Best-practice printing tips**](./printing_best_practices.md),
> the slicer-agnostic companion to this page.

- [Start here: profile vs. settings sheet (read this first)](#start-here-profile-vs-settings-sheet)
- [The one idea that makes these print well](#the-one-idea-the-model-already-did-the-hard-part)
- [The settings sheet (universal — type these into Custom mode)](#the-settings-sheet)
- [PETG-specific behaviour (stringing, cooling, sealing)](#petg-specific-behaviour)
- [Per-model cheat-sheet](#per-model-cheat-sheet)
- [The three things you actually calibrate](#the-three-things-you-actually-calibrate)
- [The importable profile](#the-importable-profile)

---

## Start here: profile vs. settings sheet

There are two ways to carry print settings, and they are **not** equally
durable — be honest with yourself about which you're using:

| | Importable `.curaprofile` | The settings sheet (below) |
|---|---|---|
| **Portable across Cura versions?** | Partly — it's stamped with a `setting_version`; a much newer or older Cura may upgrade it, warn, or refuse | **Yes, forever** — they're just numbers you type in |
| **Portable across printers?** | Partly — it imports onto any printer (it's built against the generic `fdmprinter` base), but the printer-specific knobs (retraction, bed adhesion) still need your values | **Yes** — the sheet tells you which knobs are yours to set |
| **Tells you *why*?** | No | Yes — every row has a reason |

> **The settings sheet is the source of truth.** The
> [importable profile](#the-importable-profile) is a convenience that gets you
> ~90 % of the way in one click for a **0.4 mm nozzle at 0.2 mm layers**, but
> it can't know your printer's retraction or first-layer squish. If the profile
> and this page ever disagree, this page wins.

---

## The one idea: the model already did the hard part

Most "PETG profiles you find online" spend their effort compensating for the
*model* — squishing holes bigger, shrinking outer walls, adding supports under
overhangs. **These models don't need any of that, and adding it will make fits
worse.** The geometry already accounts for it:

- **Tolerance is in the STL, not the slicer.** Every fit is driven by
  `tol_slide` (0.20), `tol_press` (0.10), `tol_hole` (0.30) and `clip_clear`
  (0.25), tuned once on the [fit coupon](./canary_fit_coupon.scad). So in Cura
  you set **Horizontal Expansion = 0, Hole Horizontal Expansion = 0, Initial
  Layer Horizontal Expansion = 0.** If you *also* dial in slicer XY
  compensation you double-correct, and the snap clips, lid lip and magnet
  pockets all fight you.
- **Elephant foot is a modeled chamfer.** The bottom edge carries a 45°
  `foot_cham` that removes elephant-foot *and* moves the delamination-prone
  bottom edge off the load path. So you **do not** need negative Initial Layer
  Horizontal Expansion. If the first layer still bulges, that's a squish/bed-temp
  problem — fix it at the printer (see [calibration](#the-three-things-you-actually-calibrate)),
  don't stack a second correction on top of the chamfer.
- **Overhangs self-support.** Horizontal hinge bores are **teardropped** so
  their crowns bridge cleanly; the doorbell pill, lens hood and soft face edges
  are drawn with `$fa/$fs` curve quality and land on the first layers when the
  part is oriented as intended. The snap-clip underside is a 45° chamfer sized
  for clean FDM bridging.
- **Therefore: supports OFF.** Not "usually off" — off. If Cura's overhang view
  lights up red, you have the part **oriented wrong**, not a part that needs
  supports. Re-seat it flat / face-down per the [cheat-sheet](#per-model-cheat-sheet).
  A support scar on a lid face or a radome window is a defect these parts were
  specifically designed to avoid.

Internalise that and the rest is just good PETG hygiene.

---

## The settings sheet

Cura → **Custom** mode → **Settings visibility: Expert**. Values are for a
**0.4 mm nozzle, 0.2 mm layers**, the security-build spec from the
[enclosure README](./README.md#engineering--materials-security-build). Rows
marked **[YOURS]** are printer-specific — start here and tune.

### Quality
| Setting | Value | Why |
|---|---|---|
| Layer Height | **0.20 mm** | The design datum. 0.12–0.16 only for the finest A-surfaces; 0.28 for tool/jig parts. |
| Initial Layer Height | **0.24 mm** | A fatter first layer bonds PETG to the plate and hides minor bed unevenness. |
| Line Width | **0.4 mm** (= nozzle) | Walls are modeled as whole multiples of a 0.4 line (2.0 mm = 5 lines). Keep them aligned. |

### Walls  *(the security-critical part — this is the physical attack surface)*
| Setting | Value | Why |
|---|---|---|
| Wall Line Count | **4** | 1.6 mm of solid perimeter. Rigidity against prying comes from perimeters, not infill. Bump to 5 for sealed/weather builds (watertightness). |
| Outer Wall Wipe Distance | 0 | Avoids a wipe blob on the visible face. |
| **Horizontal Expansion** | **0.0** | *Do not compensate.* Tolerance is in the model. |
| **Hole Horizontal Expansion** | **0.0** | Same — screw and light-pipe holes are already sized via `tol_hole`/`tol_press`. |
| **Initial Layer Horizontal Expansion** | **0.0** | Elephant-foot is the modeled `foot_cham` chamfer. Don't fight it. |
| Optimize Wall Printing Order | On | Fewer travels, cleaner seams. |
| Z Seam Alignment | **Rear** | Hides the seam on the back edge of every part (the faces point forward/down). |
| Seam Corner Preference | Hide Seam | Tucks it into a corner. |

### Top / Bottom
| Setting | Value | Why |
|---|---|---|
| Top/Bottom Thickness | **1.0 mm** (5 layers) | A solid, pry-resistant lid skin; watertight top on sealed builds. |
| Bottom Pattern (Initial) | Lines (or **Monotonic** if available) | On face-down parts this **is** the show surface — monotonic gives an even sheen. |
| Ironing | **Off** | The A-surface is the *bed* side (parts print face-down), so ironing the top does nothing for looks. The textured plate is your finish. |

### Infill
| Setting | Value | Why |
|---|---|---|
| Infill Density | **30 %** | Enough to back the walls and top skin without wasting PETG or trapping heat. |
| Infill Pattern | **Gyroid** | Isotropic — no weak direction in a case that gets handled and pried. |
| Infill Overlap | **30 %** | Fuses infill to walls so the shell acts as one piece (README spec). |
| Infill Before Walls | Off | Print walls first for a truer outer surface. |

### Material — PETG  **[YOURS: dial to your filament's label]**
| Setting | Value | Why |
|---|---|---|
| Printing Temperature | **240 °C** | ~+5–10 °C over a "safe" PETG number, deliberately — interlayer adhesion is what makes the case tough and (for weather builds) watertight. Range 230–250. |
| Printing Temperature Initial Layer | **245 °C** | Extra bite on layer 1. |
| Build Plate Temperature | **80 °C** | 70–85 typical for PETG. |
| Build Plate Temperature Initial Layer | **85 °C** | |
| Flow | **100 %** | Leave at 100 for dimensional fits. **Weather/sealed builds only:** +2–5 % for watertightness — then re-check the coupon, because more flow tightens every fit. |

### Cooling  *(PETG is not PLA — do not blast it)*
| Setting | Value | Why |
|---|---|---|
| Fan Enabled | On | |
| Fan Speed | **40 %** | Enough to hold small features; too much cooling embrittles PETG layer bonds — bad for a structural, sometimes-sealed case. |
| Initial Fan Speed | **0 %** | Let layer 1 weld to the plate. |
| Regular Fan Speed at Layer | **4** | Ramp in after the base is down. |
| Minimum Layer Time | **10 s** | Auto-slows tiny parts (coupon stations, bosses, the knob) so they don't melt. |
| Minimum Speed | 15 mm/s | Floor for the slow-down. |

> **ASA / PC / CF variants** (Vision bracket, sun-exposed outdoor): **minimal
> or no cooling**, print hotter, enclosed printer. Everything else on this page
> holds; just drop the fan toward 0 and follow the material table in the
> [README](./README.md#engineering--materials-security-build).

### Support & Adhesion
| Setting | Value | Why |
|---|---|---|
| **Generate Support** | **Off** | Non-negotiable — see [the one idea](#the-one-idea-the-model-already-did-the-hard-part). |
| Build Plate Adhesion Type | **Skirt** | Enough for wide, flat parts. |
| Skirt Line Count | 2 | Primes the nozzle. |
| *Switch to* **Brim** *for:* | 8 mm brim | Small-footprint / tall parts: the hinge **knob**, the **bracket** prongs, TPU **gaskets**, the **doorbell** pill, the **field case**. See the cheat-sheet. |

### Speed & Travel  *(PETG stringing lives here)* **[YOURS: retraction is printer-specific]**
| Setting | Value | Why |
|---|---|---|
| Print Speed | **45 mm/s** | Moderate — PETG welds better slow, and the A-surface is cleaner. |
| Outer Wall Speed | **30 mm/s** | The face you see. Slow it down. |
| Initial Layer Speed | **20 mm/s** | Adhesion + a flat foot. |
| **Retraction Distance** | **[YOURS]** — ~1.0–2.0 mm direct-drive, ~4–6 mm bowden | The #1 anti-stringing knob and the #1 thing a shared profile *can't* get right for you. |
| Retraction Speed | ~35 mm/s | |
| Combing Mode | **Not in Skin** | Keeps travel moves off visible faces so PETG ooze doesn't scar them. |
| Z Hop When Retracted | **On, 0.2 mm** | PETG leaves blobs; hopping stops the nozzle dragging through them. |

---

## PETG-specific behaviour

The things that bite people specifically on **PETG**, and the fix:

- **Stringing / whiskers between towers and across the USB cutout.** PETG oozes
  more than PLA. Fixes, in order: **dry the filament** (PETG absorbs water — wet
  PETG strings, pops and prints weak; 60–65 °C for 4–6 h before a sealed build),
  tune retraction ([YOURS], above), enable Z-hop, drop nozzle temp 5 °C at a
  time. A quick pass with a heat gun removes cosmetic strings after the fact.
- **PETG bonds *too* well to smooth PEI and glass** — it can tear a divot out of
  the plate. Print on **textured PEI** (which is also the finish these A-surfaces
  are designed around — the first layer takes the plate's texture) or use a thin
  release (glue stick / hairspray) on smooth sheets. The `foot_cham` chamfer also
  helps parts pop off.
- **Don't over-cool.** Too much fan is the classic PETG mistake: it looks fine
  and then a lid cracks along a layer line when you pry-test it, or a "sealed"
  case wicks water through under-bonded layers. 40 % is the ceiling for these
  structural parts; the modeled teardrops mean you don't need aggressive cooling
  for overhangs anyway.
- **Watertightness (weather / sealed presets)** is a print-quality property, not
  a geometry one: **5 walls, +2–5 % flow, 0.2 mm layers, dry filament, no
  over-cooling.** After adding flow, re-run the coupon — the gasket groove and
  lid lip get tighter. See [field_ratings.md](./field_ratings.md) for what the
  seal can honestly claim (splash, not submersion).
- **Creep under clamp.** PETG relaxes under the sustained load of gasket
  preload and screw torque. For serviced/sealed fleets, print with
  `screw_insert = true` and fit brass heat-set inserts, or **re-torque 24 h after
  assembly.** Optionally **anneal** finished parts (65 °C, 1 h, supported flat)
  for ~20 % more creep and stiffness margin.

---

## Per-model cheat-sheet

Thingiverse-style: orientation, brim, and the one thing that bites. **Supports
are off for every row** — the orientation is what makes that true.

| Part | Orient | Brim? | The one thing |
|---|---|---|---|
| **[Fit coupon](./canary_fit_coupon.scad)** | flat, as exported | no | **Print this first.** Every station is labeled with the parameter it tunes. Don't print a case until the clip/slide/press stations feel right. |
| **WAP base** | open side **up** | no | The USB-C cutout bridges fine at 240 °C; no support. |
| **WAP lid** | **face-down** | no | Face-down puts the chamfered edge + debossed label on layer 1 — a clean bed = a clean lid. |
| **WAP weather lid** | face-down | no | The 3 mm drip skirt is self-supporting; the gasket lands separately. |
| **TPU gasket** | flat | **yes** (thin ring) | TPU 90–95 A, **2 walls, 100 % infill, ~25 mm/s, fan low/off.** It's a 1.1 mm ring — brim keeps it from wandering. |
| **Vision back / front** | back open-up / front **face-down** | no | The GoPro hinge prongs print as part of the shell, fins self-supporting. |
| **Vision bracket** | prongs **up**, flat base down | **yes** | Small footprint + cantilever load — brim for adhesion, and print in **CF-PETG/CF-Nylon** if you can (it's the highest-stressed part). |
| **Vision knob** | flat | **yes** | Prints over an M5×25 bolt; small round footprint needs the brim. |
| **Sense radome front** | **face-down**, window flat on the plate | no | **Radome flatness is the whole game.** Uniform 1.0 mm membrane, **unfilled PETG/ASA only** — no CF, no metallic paint, no foil label in front of the antenna or you blind the 60 GHz radar. Textured PEI is fine; don't sand the window. |
| **Sense back** | open side up | no | — |
| **Doorbell body** | tall pill, back flat on plate | **yes** | Tall + narrow footprint — brim for stability. Sealed by default; treat like a weather build (5 walls). |
| **Doorbell face** | **face-down** | recommended | Two-stage soft edges + button bezel land on layer 1 face-down. |
| **Doorbell / wall plates** | flat | no | T-studs print upward, self-supporting. |
| **Field case** | body open-up, boot separate in **TPU** | **yes** | Top of the FDM ceiling: **4 mm walls, O-ring groove, no over-cooling.** Anneal it. The rating is *earned by test*, not assumed — [field_ratings.md](./field_ratings.md). |
| **Solar shield / desiccant tray** | flat | no | Print the shield **white/light ASA** — it's a sun part. |
| **Paper templates** | — | — | These are **SVGs you print on paper at 100 %** — no plastic. Check the 20 mm square. |

---

## The three things you actually calibrate

Ignore the hundreds of Cura settings. For *these* parts, three things decide
whether it works, and only these three:

1. **Fit — on the [coupon](./canary_fit_coupon.scad), not in the slicer.** If
   the lid binds or the clips are loose, adjust `tol_slide` / `clip_clear` in the
   `.scad` and re-export. Leave Cura's expansion settings at 0. Calibrate once;
   it holds for the whole catalog because everything shares the tolerance system.
2. **First-layer squish** (elephant foot, adhesion). Fix at the printer:
   Z-offset and bed temperature. The model's `foot_cham` already handles
   elephant-foot geometrically — if you still see a bulging foot, your nozzle is
   too close or the bed too hot. **Don't** reach for negative Initial Layer
   Horizontal Expansion; you'd be correcting the same thing twice.
3. **Retraction** (stringing). Printer-specific, [YOURS]. Dial it on a real
   print, then it's set for every PETG part you make.

Everything else on this page is a sensible default you can leave alone.

---

## The importable profile

`profiles/securacv_canary_petg_security_0.4n_0.2mm.curaprofile` is a
ready-to-import Cura quality profile that encodes the [settings
sheet](#the-settings-sheet) above.

**Import:** Cura → **Preferences → Profiles → Import**, pick the file, then
select it from the profile dropdown (top right). It's built against the generic
`fdmprinter` base, so it imports onto **any** printer.

**What it can and can't do for you:**

- ✅ Sets every geometry/quality/temperature value in the sheet in one click.
- ✅ Keeps all three compensation settings at 0 and supports off — the
  model-aware part you'd most likely get wrong by hand.
- ⚠️ **Built for Cura 5.x, 0.4 mm nozzle, 0.2 mm layers.** A very different Cura
  version may upgrade it on import (fine) or warn (re-enter from the sheet).
- ⚠️ **Does not set retraction for you** — that's printer-specific. After
  importing, set your Retraction Distance ([YOURS] above) or you'll get PETG
  strings.
- ⚠️ It's a *convenience*, not the spec. If it ever disagrees with this page,
  **this page is right** — it's the thing that survives version and printer
  changes.

For TPU gaskets and ASA/PC outdoor parts, don't import this — those are on the
[README material table](./README.md#engineering--materials-security-build) and
the gasket note in the cheat-sheet, because their cooling/temperature/flow are
different enough that a shared profile would mislead.
