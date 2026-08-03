# Which 3D printer — to support, and to buy for production

This catalog is deliberately **printer-agnostic** ([best practices](./printing_best_practices.md)
teaches knobs, not machines), and that doesn't change. But two questions have
concrete answers worth writing down:

1. **Which printer should the docs target?** — most people printing these parts
   own one particular machine, and our only slicer guide is for a slicer they
   probably don't open.
2. **What should *we* buy to produce these in volume?** — with real upfront,
   running and maintenance numbers rather than vibes.

> **The costs below are computed from the committed STLs in this folder**, not
> estimated from feel. Where a number is an estimate (print time, wear intervals)
> it says so and states its assumption.

**Which page do you want?**

| You want… | Go to |
|---|---|
| **Which machine to buy / support** (this page) | you're here |
| The *why* behind good print settings, any machine | [printing_best_practices.md](./printing_best_practices.md) |
| Exact slicer values to type in | [printing_petg_cura.md](./printing_petg_cura.md) |
| Material choice + finish ladder | [README → Engineering & materials](./README.md#engineering--materials-security-build) |
| What a printed unit can survive | [field_ratings.md](./field_ratings.md) |

---

## First: what these parts actually demand

### Scope of these measurements

**This analysis covers the 34 committed, print-validated STLs in this folder**
(`git ls-files '*.stl'`) — the released WAP, Vision, Doorbell and Sense parts
plus the workshop coupons. That is the set we would actually put into
production, and it is exactly reproducible.

It is **not** the whole catalog. Around 60 parts render from the `.scad`
sources, including the [in-development](./README.md#in-development) designs that
have no committed STL, and **those are not costed here**. One of them matters
for printer choice and is called out below: [`canary_s3_lcd7.scad`](./canary_s3_lcd7.scad),
the 7" wall dashboard, is dimensioned around a **192.96 × 110.76 mm** bonded
glass slab, so its bezel lands near **200 mm** wide — bigger than anything in
the released set.

### The constraints

| Constraint | Measured reality | What it rules in / out |
|---|---|---|
| **Largest released part** | **120.5 × 78.0 × 43.7 mm** (`canary_dash_display_stand`) | Build volume is **not a constraint for the released set** — it fits any current machine with room to spare. Do not pay for bed size. |
| **Largest part in the catalog** | ~**200 mm** wide (7" dashboard bezel, in development, no committed STL) | Sets the real floor: a **~220 mm bed or larger** if that design ships. Comfortably inside a 256 mm machine; it rules out the 180 mm mini class. |
| **Tallest print** | ~44 mm in the modeled orientation | Warping scales with height and footprint. At this size, **an active heated chamber is a nice-to-have, not a requirement** — this is the single biggest cost lever. |
| **Supports** | Never — geometry self-supports | No soluble material, no second nozzle needed for support interface. |
| **Materials** | PETG default · ASA outdoor · **TPU 90–95 A** gaskets · **CF-PETG/CF-Nylon** bracket · PC narrowly | Needs: **enclosure** (ASA), **direct drive** (TPU), **hardened nozzle** (CF). |
| **Finish surface** | The **bed** — visible faces print face-down | A **textured PEI plate is mandatory**, not an upgrade. |
| **Nozzle / layer** | 0.4 mm / 0.20 mm | Stock hardware on everything. |
| **Catalog size** | 34 committed STLs (~60 parts render from source) | Throughput comes from **parallel machines**, not one fast one. |

**The conclusion this forces:** the expensive tier of the market sells large
build volume, active chambers and dual extrusion. This catalog needs **none of
the three** — a stock 256 mm bed swallows even the 7" dashboard. What it
actually needs is an enclosure, direct drive, a hardened nozzle option, a
textured PEI plate, and *many cheap reliable units*.

---

## Question 1 — the most popular machine (what to support)

**Bambu Lab, decisively.** In the first four months of 2026 roughly **every
second desktop printer sold** was a Bambu, against one in four for Creality
([3Druck](https://3druck.com/en/industry-2/3d-printers-boom-bambu-lab-displaces-creality-from-top-spot-04157946/),
[Tom's Hardware](https://www.tomshardware.com/3d-printing/bambu-lab-overtakes-creality-as-the-worlds-top-selling-budget-3d-printer-brand)).
They hold **40 %+ of the $1,000–5,000 segment** and took the sub-$2,500 desktop
FDM lead from Creality ([PrintVX](https://printvx.com/blog/3d-printing-news-bambu-lab-expansion-2026.html)).
Prusa's CORE One narrowed the speed gap; it has not narrowed the unit-share gap.

### The gap this exposes in our own docs

Our only slicer guide is **Cura**. Cura still has the largest total install base
and the best legacy-printer support, but its development pace has visibly slowed
in 2026, and **the market-leading machine ships Bambu Studio**, with **Orca
Slicer** as the enthusiast default ([slicer comparison](https://3dprinting.com/software-guides/best-3d-printer-slicers/)).

So the median person printing a Canary case today opens a slicer we have never
documented. That is a **documentation** fix, not a change of stance:

- **Keep** [`printing_petg_cura.md`](./printing_petg_cura.md) — it is the
  settings *sheet*, and the sheet is the source of truth.
- ✅ **Done:** [`printing_petg_orca.md`](./printing_petg_orca.md) is the
  Orca/Bambu Studio companion. The values port directly; the machine-specific
  rows (retraction, first-layer squish) are marked `[YOURS]`. It also carries a
  Cura↔Orca name map and, more usefully, the **four stock-preset rows that are
  wrong for this catalog** — chief among them **elephant-foot compensation,
  which Cura ships at 0 and Bambu ships at 0.2 mm**. Our parts carry a modeled
  chamfer, so Bambu's default silently double-corrects and loosens every
  press fit.
- ✅ **Also added:** [`bambu_p2s_bringup.md`](./bambu_p2s_bringup.md) — the
  ordered first-print path for a new P2S, since "which printer" and "it arrived,
  now what" are different questions.
- **Keep the agnostic framing.** "These are principles, not a printer" stays
  true — we are adding the slicer most readers actually run, not blessing a
  vendor.

---

## Question 2 — what to buy for production

### Upfront cost

Prices are US, mid-2026, base unit unless noted.

| Machine | Unit | Chamber | Build vol | Verdict for *these* parts |
|---|---|---|---|---|
| **Bambu Lab P2S** | **$549** ($799 w/ AMS) | Passive, 40–50 °C | 256³ | **Best value.** Passive is sufficient at our ≤44 mm heights |
| Bambu Lab P1S | ~$449–549 | Passive | 256³ | Proven farm workhorse; buy only if steeply discounted |
| Bambu Lab X2D | $649 ($899 w/ AMS) | **Active 65 °C**, dual nozzle | 256×256×260 | Capability we would almost never use |
| QIDI Q2 | ~$499 | **Active 65 °C** | — | Cheapest true active chamber; smaller ecosystem |
| Prusa CORE One | $1,099–1,199 | Active 55 °C | 250×220×270 | Open source; ~2× cost for less throughput per dollar |

**Skip the AMS combos.** That saves **$250 per machine** and costs us nothing:
these parts are single-material, and critically **TPU 95 A is incompatible with
AMS / AMS 2 Pro** — soft filament buckles in the long PTFE path and jams the hub
([Bambu TPU guide](https://filamentpicks.com/how-to-print-tpu-on-bambu-lab/)).
Gaskets must run from an **external spool holder** regardless of what we buy.
Only the stiffer TPU-for-AMS 68 D feeds through, and that is not our gasket spec.

> **This is advice about buying a production fleet, not about a bench machine.**
> If you already have an AMS on a single development printer, keeping it is
> reasonable — it holds PETG, ASA and a contrast color for debossed labels
> without a spool swap per print. The TPU rule is the part that still binds:
> gaskets run from an external spool holder regardless. See
> [`bambu_p2s_bringup.md` §0](./bambu_p2s_bringup.md#0--about-the-ams-you-bought).

**A three-machine fleet, all-in:**

| Item | Qty | Each | Total |
|---|---|---|---|
| Bambu Lab P2S (base, no AMS) | 3 | $549 | $1,647 |
| Spare textured PEI plate | 3 | $35 | $105 |
| Hardened-steel hotend (for CF bracket) | 1 | $21 | $21 |
| Spare hotend assembly | 1 | $21 | $21 |
| External spool holder (TPU gaskets) | 1 | ~$15 | $15 |
| **Total** | | | **≈ $1,809** |

For comparison: **one** Prusa CORE One is $1,099–1,199, and three would be
~$3,300–3,600 before spares.

### Running cost — per device, computed from the meshes

Volumes are exact: signed-tetrahedron integration over each committed STL, which
yields the **solid material volume of the modeled part**. Because these
enclosures are modeled hollow, that volume is the *wall material only* — the
interior cavity is not in it.

**Mass below is quoted at 100 % of solid volume**, i.e. an **upper bound**, at
PETG 1.27 g/cm³. That is deliberate, and worth explaining: our spec is 4 walls
(1.6 mm) with 1.0 mm solid top/bottom skins against a 2.0 mm modeled wall, so a
typical wall section is ~1.6 mm solid perimeter plus ~0.4 mm at 30 % gyroid —
roughly **85–90 % dense**, not 30 %. Gaskets are TPU at 100 % infill, so they
are fully solid. Only the few bulky parts (the watch-station stand) sit
meaningfully below the bound. **Quoting the upper bound means these figures never
understate cost**; the slicer's own estimate is the authority for any real run.

PETG at $20/kg, ASA at $41/kg ([price tracker](https://filamentpricetracker.com/material/petg)).

| Device set | Parts | Solid volume | Mass (upper bound) | PETG | ASA |
|---|---|---|---|---|---|
| WAP (compact) | 2 | 9.0 cm³ | 11.5 g | **$0.23** | $0.47 |
| WAP (battery) | 2 | 26.5 cm³ | 33.7 g | **$0.67** | $1.38 |
| Sense radome | 2 | 27.6 cm³ | 35.1 g | **$0.70** | $1.44 |
| Vision (xiao, indoor) | 4 | 36.1 cm³ | 45.9 g | **$0.92** | $1.88 |
| Vision (xiao, weather) | 5 | 57.2 cm³ | 72.6 g | **$1.45** | $2.98 |
| WAP (weather) | 4 | 57.8 cm³ | 73.4 g | **$1.47** | $3.01 |
| Doorbell | 4 | 57.9 cm³ | 73.5 g | **$1.47** | $3.01 |
| **All 34 committed STLs** | 34 | 673.2 cm³ | **855 g** | **$17.10** | $35.05 |

**The headline: the entire released catalog is under a kilogram of PETG —
roughly one spool, about $17.** Filament is not the cost of this operation, and
that conclusion is robust: it holds even at the upper bound, and the true figure
is at or below it.

**Power** is smaller still. Measured draw on a P1S-class machine is ~105 W
printing PLA; call it **~120 W** for PETG with an 80 °C bed
([Filamino](https://filamino.com/blog/3d-printer-electricity-cost)). At US-average
~$0.17/kWh that is **~$0.02 per print-hour**. Note the "700 W"/"1000 W" figures
on spec sheets are **PSU peak ratings, not draw** — a common way these estimates
get inflated 5×.

**Everything that isn't filament, per print-hour:**

| Component | Per print-hour |
|---|---|
| Electricity (~120 W at $0.17/kWh) | ~$0.02 |
| Consumables (from $60–100/machine-year, below) | ~$0.04 |
| Machine amortization ($549 over 3 yr, below) | ~$0.09 |
| **Total non-material** | **~$0.15** |

**These are deliberately left per-hour rather than per-device.** Converting them
needs a print time, and we have not benchmarked one — see the note below. What
the table does establish is the shape of the cost: **material is $0.23–$1.47 per
device and everything else is cents per hour on top.**

**Labor is in neither table, and it dominates both.** Plate changes, part
removal, gasket fitting, QC against the fit coupon — at any realistic wage,
human handling costs several times the ~$1.50 of material, power and machine
wear. That single fact drives the decision at the bottom of this page.

> **Not benchmarked: print time and daily capacity.** Per
> [AGENTS.md](../../../AGENTS.md) rule 4 — *no performance claim without a
> benchmark* — this page does **not** publish a parts-per-day figure. Nothing
> here has been sliced or timed, and a real number would also have to absorb
> plate changes, material swaps and failures. **Before planning capacity or
> committing to a delivery date, slice and time a representative plate**
> (a PETG enclosure set, a TPU gasket, a CF bracket) on the machine actually
> bought, and record it here. The recommendation below does not depend on that
> number.

### Reliability & maintenance cost

Bambu's published schedule and community farm practice
([Bambu wiki](https://wiki.bambulab.com/en/P2S/maintenance),
[Printago farm guide](https://printago.io/blog/bambu-lab-print-farm-guide-2026)):

| Interval | Task | Cost |
|---|---|---|
| Every print | Clean plate with IPA — **skin oil is the #1 adhesion failure** on these machines | ~$0 |
| Monthly | Inspect belts, fans, nozzle, cutter, camera | Labor only |
| Every 3 months | Clean + lubricate XY axis | ~$0 (grease lasts) |
| Every 5 months | Clean + lubricate Z axis | ~$0 |
| As needed | Hotend assembly (nozzle + heater + thermistor + fan) | **$21–25** |
| As needed | Silicone sock, PTFE tube, filters | ~$5–15 |
| ~Annually | Textured PEI plate (it *is* our finish surface — replace on wear) | **$35** |

**Estimated consumables per machine-year: $60–100**, assuming steady PETG duty
with occasional ASA. Two caveats specific to us:

- **PETG chews PTFE.** The extruder-to-hotend PTFE degrades faster with PETG and
  higher-temp materials than with PLA. Cheap part, easy swap, but put it on the
  schedule.
- **CF filament is abrasive.** The Vision bracket in CF-PETG/CF-Nylon will
  destroy a brass nozzle. Either dedicate one machine to CF with a hardened
  hotend, or accept a hotend swap each time. **Dedicating one is cheaper than
  swapping** once you count the recalibration.

**Amortization:** a $549 P2S over 3 years is $183/yr. At a conservative 2,000
print-hours/yr that is **~$0.09 per print-hour**. Machines are cheap per part;
this is why buying more of them wins.

**Known reliability caveats, stated honestly:**

- Firmware updates can change behavior mid-production. Pin firmware across the
  fleet and update deliberately, not automatically.
- Bambu's tooling is built for individual users, not production. Fleet
  orchestration is third-party (e.g. Printago).
- Cloud dependency: their cloud ran **99.79 %** over a recent 30-day window
  ([status](https://status.bambulab.com/)). **LAN-only mode removes this
  dependency entirely** — see the caveat section below, which for this project
  is not optional.

### Pros and cons

**Bambu Lab P2S — the recommendation**

| Pros | Cons |
|---|---|
| Cheapest path to enclosed + direct drive + textured PEI | **Cloud-tethered by default** (LAN-only mode exists and works) |
| Largest install base → our docs match what readers own | Relatively closed ecosystem; parts are first-party |
| Passive 40–50 °C chamber is *sufficient* for our ≤44 mm parts | Not enough chamber for tall ASA or serious PC — irrelevant here |
| $21 user-serviceable hotend; guided swap on-screen | Firmware updates can shift behavior under a production run |
| $549 means three machines beat one premium machine | Vendor concentration risk in the fleet |

**Prusa CORE One — the values-aligned alternative**

| Pros | Cons |
|---|---|
| **Open source**, philosophically consistent with SecuraCV | ~2× the price per machine |
| Active 55 °C chamber; genuinely better for tall ASA/PC | Buys capability this catalog does not use |
| No cloud dependency; excellent long-term parts support | Smaller build volume than half-price rivals |
| Strong repairability and documentation | One CORE One ≈ two P2S of throughput forgone |

**Bambu X2D / QIDI Q2 — the active-chamber tier**

| Pros | Cons |
|---|---|
| Active 65 °C chamber; real headroom for PC and tall ASA | We have **no tall parts** — the feature idles |
| X2D adds dual nozzle, 31 sensors, stainless rails | Dual nozzle is unused: our parts need no supports |
| QIDI Q2 is the cheapest true active chamber (~$499) | QIDI: smaller community, thinner profile ecosystem |

---

## The decision

**Buy three Bambu Lab P2S base units (no AMS), ~$1,809 all-in.**

The reasoning, in order of weight:

1. **The parts are small.** No released part exceeds 120 mm or prints taller than
   ~44 mm, and the largest thing the catalog could ship (the 7" bezel, ~200 mm)
   still sits well inside a stock 256 mm bed. That deletes large build volume
   *and* active chamber heating from the requirements list — most of what the
   premium tier sells.
2. **Throughput is parallel, not serial.** A 34-part catalog of small, repetitive
   parts is embarrassingly parallel. Three machines beat one machine that is
   30 % faster, for a third of the price premium — and this holds whatever the
   per-part time turns out to be, because it is a ratio, not a rate.
3. **Redundancy is the real reliability feature.** With three units, a failed
   hotend costs a third of capacity for the price of a $21 part. With one
   premium unit, it costs all of it.
4. **Labor dominates consumables ~4:1.** Since human handling is the expensive
   input, the right optimization is more machines finishing plates in parallel —
   not a better machine finishing them slightly sooner.
5. **It matches what our readers own.** The machine we produce on is the machine
   we can write first-hand documentation for, and it is already the one most
   users have.

**Throughput: benchmark it, don't estimate it.** Slice and time a representative
plate on the machine you buy before quoting anyone a parts-per-day figure, and
record the result here. Note that all five reasons above are *structural* — part
size, parallelism, redundancy, labor share, reader overlap — so none of them
moves when that benchmark lands.

**Buy alongside:** one hardened hotend (dedicate a machine to the CF bracket),
one external spool holder (TPU gaskets — they cannot use an AMS), a spare
textured PEI plate per machine, one spare hotend.

**Do not buy:** AMS units (saves $750 across three; TPU can't use them and
nothing here is multi-material), or bed size beyond stock.

**Revisit this if** the catalog grows a part over ~180 mm tall, or a sealed
product line moves to PC — either would make the active-chamber tier worth its
premium. Neither is true today.

### The caveat that matters for *this* project

Bambu is cloud-tethered and relatively closed — an awkward default for a
privacy-first project whose devices are built on local-only operation. This is a
real tension, not a footnote, and it has a concrete mitigation:

**Run the fleet in LAN-only mode.** It works, it removes the cloud dependency,
and it keeps production consistent with what we tell users about their own
devices. If we are not willing to do that, the honest choice is the **Prusa
CORE One** at roughly double the per-machine cost — open source, no cloud, and
consistent with the project's stated values.

That is a values call rather than a technical one, so it belongs to the
maintainers, not to this analysis. The technical recommendation is unchanged
either way: **buy several cheap enclosed machines, not one expensive one.**

---

## Confidence and staleness

- **Part measurements** (dimensions, volumes) are computed by signed-tetrahedron
  integration over the 34 STLs returned by `git ls-files '*.stl'` and are exact
  for this tree. Re-run them if the CAD changes materially.
- **Masses are upper bounds** (100 % of solid volume), not slicer output. True
  printed mass is at or modestly below them — see the reasoning in the running-cost
  section. For a real production run, take the figure from the slicer.
- **Print time and daily capacity are deliberately absent.** Nothing has been
  sliced or timed; publishing a rate would breach AGENTS.md rule 4. Benchmark
  before planning capacity.
- **In-development parts are out of scope** — only the released, print-validated
  set is costed. The 7" dashboard is noted because it changes the minimum bed
  size, not because it has been costed.
- **Prices and specs** are mid-2026 and come partly from secondary review sites;
  two primary sources were unreachable when this was written. **Confirm current
  pricing and the P2S chamber spec on the vendor's own store page before
  purchasing** — the passive-vs-active distinction was reported inconsistently
  across sources and is resolved here as **passive, ~40–50 °C**.
- Consumables and maintenance figures are steady-duty estimates, not a measured
  service history. Revisit after a year of real production.

## Sources

- [3Druck](https://3druck.com/en/industry-2/3d-printers-boom-bambu-lab-displaces-creality-from-top-spot-04157946/) and [Tom's Hardware](https://www.tomshardware.com/3d-printing/bambu-lab-overtakes-creality-as-the-worlds-top-selling-budget-3d-printer-brand) — 2026 unit-share shift
- [PrintVX](https://printvx.com/blog/3d-printing-news-bambu-lab-expansion-2026.html) — segment share
- [Filamino — P2S review](https://filamino.com/blog/bambu-lab-p2s-review-bambu-s-mid-range-flagship-tested) and [P1S vs P2S](https://makers101.com/bambulab-p2s-vs-p1s/) — specs, pricing, chamber
- [Makers101 — X2D](https://makers101.com/bambu-lab-x2d-specs-release-date/) — active chamber, dual nozzle, pricing
- [Prusa — CORE One announcement](https://blog.prusa3d.com/introducing-prusa-core-one-fully-enclosed-corexy-3d-printer-with-active-temperature-control_105477/) — active chamber CoreXY
- [3DPrinting.com — QIDI Q1 Pro](https://3dprinting.com/news/qidi-tech-unveils-low-cost-q1-pro-with-heated-chamber/) — low-cost active chamber
- [Bambu Lab wiki — P2S maintenance](https://wiki.bambulab.com/en/P2S/maintenance) — service schedule and wear parts
- [Printago — Bambu print farm guide 2026](https://printago.io/blog/bambu-lab-print-farm-guide-2026) — production practice, firmware/tooling caveats
- [Filamino — 3D printer electricity cost](https://filamino.com/blog/3d-printer-electricity-cost) — measured draw vs PSU rating
- [Filament price tracker — PETG](https://filamentpricetracker.com/material/petg) — filament pricing
- [FilamentPicks — TPU on Bambu](https://filamentpicks.com/how-to-print-tpu-on-bambu-lab/) — TPU/AMS incompatibility
- [3DPrinting.com — best slicers 2026](https://3dprinting.com/software-guides/best-3d-printer-slicers/) — Cura vs Orca vs Bambu Studio
