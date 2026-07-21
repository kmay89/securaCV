# Getting a great print — best-practice tips for the Canary parts

A slicer- and material-agnostic guide to the *why* behind a good print of these
enclosures: where strength actually comes from, how to get fits that click, and
what makes a part last outdoors. It's grounded in published FDM testing and
applied to the specific shapes in this folder — thin radome windows, snap clips,
cantilevered hinges, gasket grooves.

**Which page do you want?**

| You want… | Go to |
|---|---|
| **The reasoning** — why these settings, applied to these shapes (this page) | you're here |
| **Exact Cura values** to type in, plus an importable profile | [printing_petg_cura.md](./printing_petg_cura.md) |
| **Material choice + finish** (PETG vs ASA vs PC, consumer-look ladder) | [README → Engineering & materials](./README.md#engineering--materials-security-build) |
| **What a printed unit can survive** (IP/MIL, home tests) | [field_ratings.md](./field_ratings.md) |

> These are principles, not a printer. Every printer and filament spool is a
> little different — the point of this page is to teach you which knobs matter
> for *which outcome* so you can dial in your own machine with intent instead of
> copying numbers and hoping.

---

## First: decide what "best" means for *this* part

"Best" isn't one thing — the knobs that make a case strong are not the knobs
that make a gasket seal or a radome pass radar. Pick the dominant concern and
tune for it:

| Part | What "best" means | Tune mostly for |
|---|---|---|
| Cases, lids, brackets, mounts | **Strength & rigidity** (it's a security housing) | walls, temperature, orientation |
| Snap clips, hinge prongs | **Strength in flex** (they bend in service) | layer adhesion (temp + low cooling) |
| Lid lip, gasket groove, press pockets | **Dimensional fit** | flow calibration; *don't* over-extrude |
| Sense radome window | **Uniform thin wall + RF transparency** | flow; single unfilled material; no metallic |
| Weather / field builds | **Watertightness** | walls, flow, dry filament, hot |
| Any visible face | **Finish** | first layer on textured plate, seam, speed |

The rest of this page is those concerns in order.

---

## 1. Strength — where it actually comes from

In roughly descending order of how much they matter for a handled, pry-resistant
case. The first two are worth more than everything below them combined.

### Orientation is decision #1 — the Z axis is the weak one

FDM parts are **anisotropic**: bonds *within* a layer (the extruded plastic) are
strong, bonds *between* layers (Z) are only ~50–70 % as strong, because each new
layer welds to a cooling one below it. A part fails along layer lines first.

Every design in this folder is already oriented so the loads it sees in
service — a lid being pried, a wall taking an impact, a screw clamping — act
**in-plane**, not pulling layers apart, and the delamination-prone bottom edge
carries the modeled `foot_cham` chamfer. **So the single biggest strength
decision is already made for you: print each part in the orientation the
[cheat-sheet](./printing_petg_cura.md#per-model-cheat-sheet) gives.** The most
common way makers *weaken* these parts is re-orienting them to avoid a support
or fit the plate — which quietly turns an in-plane load into a peel load. Don't.

### Walls beat infill (this surprises people)

For a case loaded in bending and impact, **perimeters carry the load; infill
just stops the walls caving in.** By sandwich-panel behaviour, adding shells
raises flexural strength more than adding infill does, gram for gram — one test
set found going 1→3 perimeters gained ~51 % tensile strength, comparable to
pushing infill all the way from 40→100 % ([Markforged](https://markforged.com/resources/learn/design-for-additive-manufacturing-plastics-composites/understanding-3d-printing-strength/3d-printing-settings-impacting-part-strength),
[XDA](https://www.xda-developers.com/dont-need-100-infill-makes-prints-stronger/)).
That's why the spec here is **4 walls (1.6 mm of solid perimeter)** — bump to
**5 for sealed/weather builds** — with only moderate infill behind them. If you
want a tougher case, add a wall before you add infill.

### Infill: enough, isotropic, and no more

**30 % gyroid** is the sweet spot. Gyroid is near-**isotropic** — roughly equal
strength in every direction — which is what you want in a case that gets dropped
and pried from unpredictable angles, unlike grid/lines which are strong one way
and weak another ([BigRep](https://bigrep.com/posts/gyroid-infill-3d-printing/),
[Ultimaker](https://ultimaker.com/learn/mastering-3d-printing-infill-patterns-from-gyroid-to-lightning/)).
Past ~40 % you pay a lot of plastic and print time for very little extra
strength, and you trap heat that can warp thin walls. Gyroid also prints at
about the same speed as grid, so there's no reason not to use it here.

### Temperature is the layer glue — print hot for strength

Interlayer bond strength is a direct function of how completely each layer
re-melts the one below, so **hotter nozzle = stronger Z**, right up to where the
polymer starts degrading. CNC Kitchen's extrusion-temperature test is the classic
demonstration: PETG-style layer-adhesion samples nearly **doubled in strength**
across the usable temperature band as the nozzle went up ([CNC Kitchen](https://www.cnckitchen.com/blog/the-influence-of-extrusion-temperature-on-layer-adhesion)).
For PETG that means:

- **Print in the 235–245 °C range**, i.e. **+5–10 °C over the "safe" number** on
  the spool — deliberately, for adhesion. Weather/structural parts want the top
  of that.
- **Don't exceed ~250 °C** — above that PETG's glycol chains break down: yellowing,
  brittleness, more stringing, odour ([Raise3D](https://www.raise3d.com/blog/petg-3d-printing/)).
- **Run a temperature tower once per filament.** Spools vary; the tower shows you
  the highest temperature that still bridges and doesn't string, which is also
  your strongest. ASA/PC run hotter still and want an enclosure.

### Cooling is the strength ↔ finish dial — for these parts, favour strength

More part-cooling fan gives crisper overhangs and cleaner small features, but it
**freezes each layer before it fully welds to the next**, so it *lowers* Z
strength — the effect is strongest on PETG/ASA/PC. Because the overhangs here are
already handled in geometry (teardropped bores, self-supporting chamfers), you
**don't need aggressive cooling to print them**, so keep the fan modest (~40 %
on PETG, near-zero on ASA/PC) and keep the layer bond. The exception is genuinely
tiny features (bosses, the knob) where a minimum-layer-time slowdown does the
cooling for you.

### Annealing — real gains, but it *shrinks* your fits (read this before you bake a case)

Heat-treating a finished part relaxes print stresses and fuses layer boundaries,
raising heat resistance and impact toughness and moving the part away from its
anisotropic weakness — a fully annealed PETG part can hold shape past ~85 °C
instead of failing near 70 ([Unionfab](https://www.unionfab.com/blog/2025/06/annealing-3d-prints),
[Sovol](https://www.sovol3d.com/blogs/news/annealing-3d-printing-pla-petg)). The
catch that matters *here*: full annealing (80–90 °C) also causes **~1–2 % XY
shrinkage** (sometimes more) — on a 100 mm case that's 1–2 mm, which will wreck
the snap-clip, lid-lip and gasket fits this catalog spends so much effort
calibrating. So:

- **Parts where fit matters** (any case that snaps, seals, or presses): stick to
  the **mild 65 °C / ~1 h "stress-relief" regime** the README recommends — below
  PETG's glass transition, so you get creep/stiffness margin with negligible
  dimensional change. Do it **supported flat** so the part can't slump.
- **Parts where fit doesn't matter and heat does** (a sun-baked outdoor bracket):
  a full 80–90 °C anneal is worth it — but print it slightly oversize, or anneal
  *before* you rely on any mating dimension, and re-check the fit afterward.
- For real heat resistance without the shrinkage headache, the cleaner answer is
  **print it in ASA or PC** in the first place (see the [material table](./README.md#engineering--materials-security-build)).

---

## 2. Fit & dimensional accuracy — click, don't crack

These parts assemble by snap and press fit, so dimensional accuracy *is* the
product. Two ideas do 90 % of the work:

### Calibrate flow once — it's the biggest lever

**Flow (extrusion multiplier) is the number-one control on dimensional
accuracy** and on watertightness. Over-extrude and holes shrink, outer walls
bulge, and elephant-foot returns; under-extrude and walls get porous and leak.
Calibrate flow on a simple wall-thickness test for your filament, set it, and
leave it. Only *then* do fits behave predictably.

### The fit coupon is your calibration — not the slicer

The whole catalog shares one tolerance system baked into the models
(`tol_slide` 0.20 / `tol_press` 0.10 / `tol_hole` 0.30 / `clip_clear` 0.25).
Print the **[fit coupon](./canary_fit_coupon.scad) first**; if a station is
tight or loose it names the parameter to nudge in the `.scad`. Because the
tolerance lives in the geometry, **leave your slicer's XY / hole / horizontal-
expansion compensation at 0** — dialing in slicer compensation *on top* of the
model's tolerance double-corrects and breaks every fit at once. (Full reasoning:
[the one idea](./printing_petg_cura.md#the-one-idea-the-model-already-did-the-hard-part).)
Calibrate once and it holds for all ~60 parts.

### First-layer squish = elephant foot (fix it at the printer)

A bulging first layer (elephant foot) makes the base oversize so lids won't
seat. The models already remove it geometrically with the `foot_cham` chamfer,
so if you still see it, the cause is a first layer squished too hard: raise the
Z-offset a hair and/or drop the bed temperature a few degrees. **Don't** add
negative initial-layer horizontal expansion — you'd be correcting the same thing
the chamfer already handles, and the base would end up undersize.

### Remember big parts shrink as they cool

All thermoplastics contract on cooling, and hotter materials contract more —
ASA/PC shrink and warp noticeably more than PETG, which is one reason PETG is the
easy default here. On the longest parts (the doorbell pill, the field case) print
on a clean, level, adhesive plate and keep cooling gentle so one end doesn't lift
and pull the geometry out of true.

---

## 3. Surface & finish

The visible faces (lids, doorbell face) are modeled to print **face-down**, so
the *bed* makes your finish — a textured PEI plate stamps its texture into the
show surface and hides layer lines. Ironing the top does nothing for looks here
(the top is the hidden inside). The full "make it look store-bought" ladder —
fuzzy skin on outer walls, seam-to-rear, matte filament, two-tone TPU accents,
paint-filled debossed labels — is in the
[README finish section](./README.md#engineering--materials-security-build); it
gets ~90 % of the consumer look for ~0 % extra effort.

---

## 4. Weather & longevity

Three different enemies, three different answers — and none of them is a slicer
setting alone:

- **UV** embrittles plastic in sunlight. ASA holds up; **PETG slowly embrittles**;
  PLA fails fast. Sun-exposed part → ASA (light colour), or the solar shield.
- **Heat & creep.** PETG softens and *creeps* (slowly deforms) under sustained
  load — gasket preload, screw clamp, a hot dash. Fixes: heat-set inserts instead
  of self-tappers on serviced builds, re-torque sealed builds after 24 h, mild
  anneal for margin, or step up to ASA/PC for hot spots.
- **Water** gets through FDM parts via micro-gaps between extrusion lines, not
  over the wall. Watertightness is a *print-quality* property: **4–5 perimeters,
  +2–5 % flow, 0.2 mm layers, dry filament, hot, not over-cooled.** After adding
  flow, re-check the coupon — every fit tightens. What the result can honestly
  claim (splash, not submersion, unless it's the field case) is in
  [field_ratings.md](./field_ratings.md).

### Polycarbonate — yes, but only where you truly need it

You can print these parts in PC, and on paper it's the strongest choice: it tops
the shell ranking (`PC > PA-CF ≈ ASA > PETG > PLA` in [field_ratings.md](./field_ratings.md)),
has the highest heat-deflection temperature, and — unfilled — is RF-transparent
so it's fine for the Sense radome. **But it's a specialist pick, not a default,
and the reasons are about the print, not the polymer:**

- **It needs an enclosed, hot printer** (bed ~110–120 °C, warm chamber, nozzle
  ~260–290 °C). The big flat parts here — bases, lids, the doorbell pill, the
  field-case body — are exactly what **warp** in PC without a chamber, lifting
  corners and pulling the geometry out of true.
- **The baked-in fits don't transfer for free.** PC runs hotter and shrinks more
  than PETG, so the calibrated snap/press/gasket fits need the **fit coupon
  re-run in PC**, and warping makes them less consistent across a part.
- **PC is intensely hygroscopic** — more than PETG. Damp PC prints with bubbles
  and *weak layers*, which defeats the whole reason you reached for it.
- **Delamination is the catch.** PC is prone to weak interlayer adhesion and
  cracking if under-heated or over-cooled — and layer adhesion is the exact
  property you're buying PC for. A poorly-printed PC shell can be **worse** under
  impact than a well-printed ASA one.

The tell is in this catalog's own design: the **field case** — the one part
engineered for maximum survival (CER-4: IP67 + drop intent) — specs its shell as
**ASA or PETG plus a TPU impact boot**, *not* PC. A reliably-printed ASA shell
with an elastomer taking the hits beats a warped PC shell; PC's material ceiling
doesn't survive the printability tax on a sealed, drop-tested part.

**So use PC narrowly:** for vandal-prone or genuinely high-heat spots that
need HDT/impact beyond what ASA gives (a closed cabin well past +60 °C), *and*
only if you have an enclosed printer. Then: prefer a **PC blend** (PC-ABS,
PC-Max, PolyLite PC) over pure PC — far easier for ~90 % of the benefit — dry it
hard, print hot with minimal cooling, re-calibrate the coupon, keep gaskets and
the boot in TPU, and anneal (90 °C) only on **non-fitted** parts (same
shrinkage-wrecks-fits caveat as PETG). For most people, **ASA already covers
outdoor, UV and heat and is far easier to print — reach for it first.**

---

## 5. Reliability — the boring stuff that saves the print

- **Dry your filament.** PETG (and especially ASA/PC/nylon) absorbs water from
  the air; wet filament prints weak, hisses, strings, and leaks. Dry PETG ~60–65 °C
  for 4–6 h before any sealed or structural build. This one habit fixes more
  "bad prints" than any setting — and buying good filament in the first place is
  half the battle (see [§6](#6-the-filament-itself--judge-the-supplier-not-the-brand-name)).
- **Nail the first layer.** Clean the plate (isopropyl — skin oils kill
  adhesion), level it, slow layer 1 down, fatten it slightly, and print it hot
  with the fan off. PETG bonds *aggressively* to smooth PEI/glass and can tear a
  divot out — use **textured PEI** (also your best finish) or a thin release
  agent, and let the `foot_cham` chamfer help parts pop.
- **Brim the tippy parts.** Tall or small-footprint parts (hinge knob, bracket,
  doorbell pill, gaskets, field case) want a brim so they don't peel or topple
  mid-print.

---

## 6. The filament itself — judge the supplier, not the brand name

The best settings can't rescue bad plastic, and for a *security* housing the
stakes are real: off-spec or damp filament attacks the two things these cases
depend on — **watertightness** (porous, under-bonded walls leak) and
**layer-adhesion strength** (a clip or lid cracks in service). We deliberately
**don't** keep an "approved brands" list — quality drifts batch to batch, the
good names differ by region, and a stale list helps no one. Judge the spool
instead. The signals below are what separate a manufacturer with real QC from a
repackaged mystery roll.

**Before you buy — what a good supplier tells you:**

- **A stated diameter tolerance.** Reputable 1.75 mm filament is spec'd to
  **±0.02–0.03 mm**; ±0.05 is loose. This matters *here* specifically — loose
  tolerance swings your flow, and these parts' snap/gasket fits are calibrated,
  so an inconsistent diameter makes them inconsistent too.
- **A published technical data sheet** (print/bed temp range, sometimes a
  per-spool QC card with the measured diameter) and **lot/batch numbers** —
  traceability is a tell that a real QC process exists.
- **Sealed on arrival:** vacuum bag **+ desiccant**. A spool that shows up loose
  or in a leaky bag has already been drinking from the air.
- **Reviews about *consistency*, not just price** — batch-to-batch sameness and
  clean winding (no tangles/crossed wraps that snag mid-print) are the real
  signal. Be wary of no-name "PETG" with no TDS and no tolerance figure; some is
  an off-spec blend that won't hit the temps or strength you expect.

**How filament goes bad — the aging risks:**

- **Moisture is the number-one killer.** PETG is moderately hygroscopic;
  ASA/PC/TPU/nylon far more so. Wet filament flashes to steam at the nozzle:
  weak layers, stringing, popping/hissing, a hairy surface, and **porous walls
  that leak** — exactly the failure a sealed Canary can't afford. Signs you're
  printing wet: hiss/pop while printing, a fuzzy surface, or prints that snap
  along the layer lines.
- **Brittleness with age / bad storage.** Filament left out for months —
  especially in a humid shop — turns brittle and snaps in the feeder. Nylon and
  PC degrade fastest; PLA embrittles too.
- **UV & heat on the spool.** A roll parked in a sunny window ages like the parts
  would outdoors. Store cool and dark.
- **Shelf life is a storage question, not a calendar one.** Sealed with desiccant
  in a cool, dark box, most filament lasts *years*. Loose in humid air, it can be
  unusable in **weeks**.

**Store it, and test the spool you already have:**

- **Storage:** an airtight box with fresh desiccant (or a dry box); dry before any
  sealed or structural build (PETG ~60–65 °C / 4–6 h; PC and nylon hotter and
  longer), and print PC/nylon straight from a dry box.
- **A 5-minute quality check on any spool:** measure the diameter with calipers at
  several points (round, and on-tolerance?), and do a bend test — badly aged or
  wet filament cracks instead of flexing. Then let the **[fit coupon](./canary_fit_coupon.scad)
  plus a temperature tower double as your per-spool QC**: re-run them whenever you
  switch brand *or* spool, because tolerance and flow differ between them and
  that's exactly what your calibrated fits ride on.

---

## Per-geometry playbook

The features in this catalog and the one setting that most decides whether each
comes out right:

| Feature (where) | Why it's demanding | The lever that matters most |
|---|---|---|
| **Snap clips** (case edges) | They *flex* across layer lines every time you open the case — pure Z-load | Layer adhesion: print **hot, low fan**. A cold-printed clip snaps off. |
| **Radome window** (Sense front) | Thin uniform 1.0 mm membrane the 60 GHz beam crosses | **Flow calibration** for even thickness; **unfilled single material, no CF, no metallic paint/foil** in front of the antenna. |
| **Hinge prongs / bracket** (Vision, Sense) | Cantilevered — the highest bending stress in the catalog | **Walls + material**: print solid-ish, consider **CF-PETG/CF-Nylon**; keep the modeled orientation so the load is in-plane. |
| **Gasket groove & lid lip** (weather builds) | A precise seal that must stay dimensionally exact | **Flow, not over-extrusion**; **skip the full anneal** (shrinkage kills the seal). |
| **M2 screw posts** (corners) | Self-tappers in plastic strip easily | Enough **walls around the bore**; don't exceed ~0.3 N·m; use **heat-set inserts** for serviced/sealed fleets. |
| **Tall pill body** (doorbell, field case) | Long + narrow footprint warps and lifts | **Adhesion + brim + gentle even cooling** so one end doesn't curl. |
| **Field-case shell** (CER-4 intent) | Must be watertight *and* impact-tough | **4–5 walls, +flow, dry, hot**; let the **TPU boot** take impacts; anneal only with fit re-check. |

---

## The short version

If you remember five things:

1. **Print it in the orientation the cheat-sheet says** — that's your biggest
   strength decision, already made.
2. **Add walls before infill.** 4 (5 for weather) walls, 30 % gyroid.
3. **Print hot** (PETG 235–245, +5–10 over "safe") for strong layers; run one
   temperature tower per spool.
4. **Calibrate flow and the fit coupon once**, then leave slicer compensation at
   0 — the tolerance is in the model.
5. **Start with good, dry filament** (on-tolerance, sealed with desiccant) and
   **nail the first layer** on textured PEI.

Exact Cura numbers and the importable profile:
[printing_petg_cura.md](./printing_petg_cura.md).

---

## Sources

- [Markforged — 3D printing settings impacting part strength](https://markforged.com/resources/learn/design-for-additive-manufacturing-plastics-composites/understanding-3d-printing-strength/3d-printing-settings-impacting-part-strength) (perimeters/sandwich-panel behaviour)
- [XDA — you don't need 100 % infill](https://www.xda-developers.com/dont-need-100-infill-makes-prints-stronger/) (walls vs infill, quantified)
- [CNC Kitchen — the influence of extrusion temperature on layer adhesion](https://www.cnckitchen.com/blog/the-influence-of-extrusion-temperature-on-layer-adhesion) (temperature → Z strength)
- [Raise3D — PETG 3D printing guide](https://www.raise3d.com/blog/petg-3d-printing/) and [SigmaFilament — PETG print temp](https://sigmafilament.com/petg-print-temp-guide-2026/) (PETG temperature window + degradation)
- [BigRep](https://bigrep.com/posts/gyroid-infill-3d-printing/) and [Ultimaker](https://ultimaker.com/learn/mastering-3d-printing-infill-patterns-from-gyroid-to-lightning/) on gyroid isotropy
- [Unionfab](https://www.unionfab.com/blog/2025/06/annealing-3d-prints) and [Sovol](https://www.sovol3d.com/blogs/news/annealing-3d-printing-pla-petg) on annealing PETG (HDT gain vs. shrinkage)
