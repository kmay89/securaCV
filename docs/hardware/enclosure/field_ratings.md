# Field & environmental ratings — what a printed Canary can honestly claim

Every "IP67" and "MIL-SPEC" sticker in a store means *a specimen of that
exact assembly passed a specific lab test*. A 3D-printed case can genuinely
reach serious ratings — but only per printed unit, verified by test, because
**your printer, filament and seal assembly are part of the product**. This
page defines what the Canary hardware can survive at all (the *hardware
ceiling*), what FDM printing can deliver (the *process ceiling*), and a
simple honest rating ladder — the **Canary Environmental Rating (CER)** —
with home test protocols mapped to the real standards.

> TL;DR — no design in this folder is "GoPro durable" until *your print* of
> it passes the tests below. The [field case](./canary_field_case.scad) is
> the only design *engineered* to the top of the ceiling (CER‑4: IP67 +
> transit-drop intent); everything else tops out at splash-resistant.

## 1. The hardware ceiling (the enclosure can't fix these)

| Component | Limit | Consequence |
|-----------|-------|-------------|
| XIAO ESP32‑S3 (Sense) board | **−20 … +65 °C** working ([Seeed spec](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)) | the system envelope, regardless of case |
| OV2640 camera | stable image **0 … +50 °C**, functional ≈ −20 … +70 °C ([datasheet](https://www.uctronics.com/download/cam_module/OV2640DS.pdf)) | image quality degrades outside 0–50 even though the device runs |
| LiPo pouch cell | discharge −20 … +60 °C, **charge 0 … +45 °C only** | the true weak link: charging below freezing plates lithium (permanent damage / hazard). Winter field use = charge indoors, or power without a battery |
| microSD (consumer) | typically 0 … +70 °C | buy industrial (−40 … +85) for outdoor units |
| Solder joints, connectors, antenna | fine to high shock — the boards weigh grams | drops kill via **battery crush, FPC ribbon chafe and connector walk-out**, not the silicon. Foam-bed the pack, dress the ribbon |

**System envelope: operate −20 … +50 °C; charge only 0 … +45 °C; storage
−20 … +60 °C.** A matte-black case in direct summer sun exceeds +50 °C
internally — use light colors or the [solar shield](./README.md#weather-mode-opt_seal)
outdoors. Sealed cases also *need* a vent membrane: a 40 °C day/night swing
pumps air (and eventually moisture) past any static seal unless pressure can
equalize through ePTFE.

## 2. The process ceiling (what FDM can and can't deliver)

- **Walls are not watertight by default.** FDM leaves micro-channels between
  extrusion lines; leak paths form wherever layers bond poorly
  ([Formlabs](https://formlabs.com/blog/watertight-3d-printing/),
  [Forge Labs](https://forgelabs.com/blog/ingress-protection-3d-printing-ip-rated-parts)).
  Fixes that work: **≥4–5 perimeters, ≥3–4 mm walls, +2–5 % extrusion**,
  0.2 mm layers, dry filament. An interior brush coat of epoxy is the
  belt-and-braces upgrade for submersion.
- **The seam is what fails, not the wall.** Corner-screwed printed-gasket
  lids (our weather presets) are splash-tight, not dunk-tight. Reaching IP67
  needs a **real O-ring cord in a groove, compressed 15–35 %, with clamp
  points ≤ ~40 mm apart** — that's exactly why the field case has six screw
  lobes, not four corner screws.
- **Impacts break layer adhesion first.** Z-tensile is 50–70 % of in-plane
  strength; a hard corner drop starts a delamination crack. The answer is
  the GoPro answer: **let an elastomer take the hit** (the TPU boot) and
  radius everything. Material ranking for shells: PC > PA‑CF ≈ ASA > PETG >
  PLA (PLA also creeps and dies in a hot car — don't use it outdoors).
- **UV**: ASA keeps its toughness outdoors; PETG embrittles slowly; PLA fast.
- What FDM will **not** reach: GoPro's 10 m/IP68 (injection-molded PC,
  overmolded TPE, glass window, factory leak-tested every unit) or any
  *certified* MIL‑STD badge — those are lab programs, not features.

## 3. How the real ratings work (so ours map honestly)

- **IP code** (IEC 60529): first digit = solids (5 dust-protected / 6
  dust-tight), second = water (4 splash / 5 jets / 7 = **1 m submersion,
  30 min**). Digits are independent tests on a specific assembly.
- **MIL‑STD‑810H** is a *menu of test methods*, not a certification — any
  marketer can say "MIL-SPEC design". The one method that matters for a
  bag-carry device is **Method 516.8 Procedure IV, transit drop: 26 drops
  from 1.22 m onto 5 cm plywood over concrete**, hitting every face, edge
  and corner ([method text](https://cvgstrategy.com/wp-content/uploads/2019/08/MIL-STD-810H-Method-516.8-Shock.pdf)) —
  and it's completely reproducible at home.
- **IK code** (IEC 62262) rates impact on the *installed* housing
  (IK08 ≈ 5 J). Informative for wall-mounted units; the drop matrix covers
  the field case better.

## 4. The Canary Environmental Rating (CER)

A CER level = **design features** + **a pass on that level's test protocol,
on your printed unit**. The SCADs echo their *design intent* at render time;
the verified rating is earned in §5 and belongs to one physical unit.

| Level | Means | Maps to | Design prerequisites | Who can reach it |
|-------|-------|---------|----------------------|-------------------|
| **CER‑1 · Indoor** | dust-protected desk/wall duty | ~IP20–40 | any case as printed | every design |
| **CER‑2 · Sheltered outdoor** | rain-splash under an eave/porch | ~IP53–54 target | weather preset: printed TPU gasket, drip skirt, USB plug, vent, mounted ports-down | WAP/Vision weather, doorbell (plate-sealed) |
| **CER‑3 · Exposed outdoor** | direct rain + hose-down washes | ~IP55 target | CER‑2 + cable gland on any wire exit + solar shield + 4–5 perimeter walls | WAP/Vision weather (careful print), relay pod (dev) |
| **CER‑4 · Field** | 1 m submersion 30 min + 1.22 m transit drops, bag-carry | ~IP67 + 810H 516.8 IV target | **field case only**: O-ring cord, 6-lobe clamp, no external ports, ePTFE vent, 4 mm walls, TPU boot, foam-bedded battery | [`canary_field_case.scad`](./canary_field_case.scad) |

Current honest status (2026‑07): **every level is *target/intent* — no unit
has been through §5 yet.** Log your results (printer, filament, settings,
pass/fail per test) in a PR so designs graduate on evidence.

| Design | Design intent | Verified |
|--------|---------------|----------|
| WAP compact/battery, Vision indoor, Sense, station/watch tools | CER‑1 | untested |
| WAP weather, Vision weather, Doorbell | CER‑2 (CER‑3 with gland+shield, good print) | untested |
| Relay/solar pod (dev) | CER‑3 | untested |
| **Field case (dev)** | **CER‑4** | untested |

## 5. The home test protocols (run in order; stop at first failure)

Prep for any water test: fit fresh seals; put a **desiccant indicator card
+ a paper towel** inside; weigh the closed unit on a 0.1 g scale.
Pass = indicator unchanged, towel dry, weight delta < 0.1 g, device boots
and produces a **signed, verified event** end-to-end.

- **W‑1 spray (CER‑2)** — 10 min garden sprayer "rain" from ≥ 15° above
  horizontal, all sides, mounted in service orientation.
- **W‑2 jet (CER‑3)** — garden hose, nozzle ~3 m, ~12 L/min, 1 min per side
  (an honest IPX5 *proxy* — say "hose-tested", not "certified IPX5").
- **W‑3 submersion (CER‑4)** — **lowest point of the case 1 m below the
  surface, 30 min**, water 15–25 °C (IEC 60529's small-enclosure condition —
  a 21 mm case in a shallow bucket sees almost none of the intended
  pressure). No 1 m vessel? A rain barrel, a capped 1 m pipe section, or a
  weighted drop into a pool all work. A shallower dunk only earns
  "CER‑4 (depth-limited, *X* m)" — record the depth, don't claim IP67.
  Do it once more after the drop matrix: *sealed after drops* is the real
  field claim.
- **D‑1 dust (any level)** — zip-bag of flour/talc, agitate 5 min, tap off,
  open and inspect (proxy for IP5X — call it "dust-tested").
- **S‑1 transit drop (CER‑4)** — boot fitted, battery in, logging: **26
  drops from 1.22 m onto 5 cm plywood over concrete** — each of 6 faces, 12
  edges, 8 corners once. Function-check every 5 drops; inspect for
  delamination cracks at the end.
- **T‑1 cold (all outdoor levels)** — 4 h in a −18 °C domestic freezer,
  power on inside the freezer (battery pre-charged — never charge below
  0 °C), verify events; watch for condensation at the window on removal
  (that's what the desiccant tray is for).
- **T‑2 heat** — 4 h at +50 ± 5 °C (closed car in sun with a thermometer, or
  a filament dryer); verify events; check the case hasn't warped (PETG will
  be near its comfort limit — this test is why ASA is the outdoor pick).

## 6. Field-case shopping list (the parts that make CER‑4 possible)

Ø1.5 mm nitrile/silicone O-ring cord (~200 mm cut, or a cord kit) ·
6× M3 heat-set inserts + M3×8 pan (or torx security) screws ·
Ø12×2 mm polycarbonate disc + neutral-cure silicone ·
adhesive ePTFE vent patch (≥ Ø10) · closed-cell foam sheet 2 mm ·
1 g desiccant sachet · TPU 95A for the boot · ASA (or PETG) for the shell.

*Sources: [Seeed XIAO ESP32‑S3 spec](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) ·
[OV2640 datasheet](https://www.uctronics.com/download/cam_module/OV2640DS.pdf) ·
[MIL‑STD‑810H Method 516.8](https://cvgstrategy.com/wp-content/uploads/2019/08/MIL-STD-810H-Method-516.8-Shock.pdf) ·
[Formlabs watertight printing](https://formlabs.com/blog/watertight-3d-printing/) ·
[Forge Labs IP-rated FDM design](https://forgelabs.com/blog/ingress-protection-3d-printing-ip-rated-parts) ·
[GoPro HERO12 specs](https://gopro.com/en/us/shop/cameras/hero12-black/CHDHX-121-master.html)*
