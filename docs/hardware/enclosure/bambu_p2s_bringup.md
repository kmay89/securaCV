# Bringing up a Bambu Lab P2S for the Canary catalog

You have a new **Bambu Lab P2S (AMS Combo)** and nothing printed yet. This page
is the ordered path from a sealed box to a part you trust, in the order that
finds problems cheapest first:

> **coupon → record your numbers → one small released case → the 7" gauge → the 7" slab**

Every step exists because skipping it costs more than doing it. Do them in
order and you will not be debugging a six-hour print.

**Related pages — this one sequences them, it doesn't replace them:**

| You want… | Go to |
|---|---|
| Exact slicer values for Bambu Studio / Orca | [`printing_petg_orca.md`](./printing_petg_orca.md) |
| The same values for Cura | [`printing_petg_cura.md`](./printing_petg_cura.md) |
| *Why* those values (any machine) | [`printing_best_practices.md`](./printing_best_practices.md) |
| Which machine to buy, and running costs | [`printer_selection.md`](./printer_selection.md) |
| The parts catalog itself | [`README.md`](./README.md) |

---

## 0 · About the AMS you bought

[`printer_selection.md`](./printer_selection.md) recommends **skipping the AMS
combo** — that advice is about buying *three production machines*, where the
AMS adds $250 each and buys nothing, because the catalog is single-material.
For **one bench machine that has to print, test and iterate**, the AMS is a
different proposition: it holds your PETG, your ASA and a contrast color for
debossed labels without a spool swap per print. Keeping it is reasonable. Two
rules apply either way:

- **TPU gaskets must NOT go through the AMS.** TPU 90–95 A — our gasket spec —
  buckles in the long PTFE path and jams the hub. Feed gaskets from an
  **external spool holder**, direct to the extruder. Only the stiffer
  "TPU for AMS" 68 D feeds through, and that is not our gasket material.
- **The AMS is not a dryer.** It slows moisture pickup; it does not remove it.
  See §2.

---

## 1 · Before the first print

Do the machine's own setup first — none of it is Canary-specific, and all of it
is load-bearing:

1. **Remove every piece of transit hardware.** Shipping screws under the bed,
   foam blocks in the gantry, tape on the toolhead. A P2S that grinds or skips
   on its first move almost always still has a transit screw in it.
2. **Run the built-in calibration wizard** and let it finish. It handles bed
   leveling, vibration compensation and motor characterization. Do not skip it
   to "just print something" — every dimension in this catalog is drawn to a
   tolerance system that assumes a calibrated machine.
3. **Fit the textured PEI plate**, and treat it as a *finish surface*, not a
   consumable. Every A-surface in this catalog prints **face-down**, so the
   plate's texture *is* the visible finish on lids, bezels and faces.
4. **Wash the plate with dish soap and water, then IPA.** Skin oil is the single
   most common adhesion failure on these machines. Handle the plate by its
   edges from here on.
5. **Check which nozzle shipped** and note it. The stock 0.4 mm hardened-capable
   assembly is fine for PETG/ASA/TPU. **Carbon-fiber filament will destroy a
   plain brass nozzle** — the Vision bracket is the only CF part in the catalog,
   and it is not on this bring-up path.
6. **Pin the firmware version** and write it down. Bambu firmware updates can
   change behavior mid-production; you want to know what you calibrated on.

---

## 2 · Filament

For the whole bring-up path you need **one spool of PETG**. That is not an
understatement of the catalog's needs — the entire released set of 34 STLs is
**under a kilogram** ([printer_selection.md](./printer_selection.md#running-cost--per-device-computed-from-the-meshes)).

| Material | For | Notes |
|---|---|---|
| **PETG** | Everything on this page | The default. Buy a known brand; generic PETG varies wildly. |
| ASA | Outdoor / sun-exposed parts | Door closed, minimal cooling. Not needed for bring-up. |
| TPU 90–95 A | Gaskets only | **External spool holder — never the AMS.** |
| CF-PETG / CF-Nylon | Vision bracket only | Hardened nozzle required. |

**Dry the PETG before you trust a result.** PETG absorbs water; wet PETG
strings, pops audibly, and prints weak — and it will make you chase slicer
settings for a filament problem. 60–65 °C for 4–6 h. If a print that worked
last week fails today and nothing changed, suspect moisture first.

**Run the printer's flow-dynamics / pressure-advance calibration for the exact
spool you are using**, once. It is the difference between corners that are
square and corners that bulge, and it is per-filament, not per-material.
**§2b below is the procedure** — and note the ordering trap it opens with:
dry the spool *before* you calibrate it, or the numbers encode the moisture.

---

## 2b · Calibrate — machine once, then every spool

Two different things are called "calibration" and they are not
interchangeable. The machine one makes the printer move correctly. The
filament one makes *this spool* extrude correctly. A new P2S needs both, in
this order, and the order is the part people get wrong.

### Sequence — dry, then calibrate, then print

**Dry the filament BEFORE you calibrate it, not after.** This is the one
that silently wastes a day. Calibration measures how the filament behaves
*right now*, so calibrating a damp spool bakes the moisture into your K-factor
and flow ratio. Dry it later and the numbers you carefully measured are now
wrong, in a way that looks like the printer drifting. 60–65 °C for 4–6 h
first (§2), then calibrate.

### Step 1 — the machine, from the printer's own screen

Do this on the printer, not in the slicer. **Calibration → Full/Auto
calibration**, and let all of it finish:

| Routine | What it fixes | Re-run when |
|---|---|---|
| Motor noise cancellation | stepper resonance | after firmware updates |
| **Vibration compensation** (input shaping) | ringing/ghosting on corners | printer moved or re-sited |
| **Auto bed leveling** | first-layer squash across the plate | plate swapped, printer moved |

Ten to fifteen minutes, once. Re-run it after you move the machine — including
across a room. Anything that shook in transit is why step 1 exists.

### Step 2 — Flow Dynamics (pressure advance / K-factor), per spool

In **Bambu Studio → Calibration → Flow Dynamics Calibration**. This is the one
that decides whether corners come out square or bulge, and it is the single
highest-value calibration for this catalog, because every fit in it is a
corner or an edge.

Bambu ships two paths and which you get depends on the machine's sensing
hardware. **Open the dialog and look:**

- If it offers **Auto** — the printer prints a pattern, reads it itself, and
  writes the K-factor back. Take it. Minutes, no judgment required.
- If it only offers **Manual / Complete** — it prints a numbered pattern and
  you pick the cleanest line by eye. Pick the number where the corners stop
  bulging and *before* they start looking gappy; if two look equal, take the
  lower.

I'm not going to tell you which one a P2S has — I don't have that confirmed,
and sending you to a menu that isn't there wastes more time than looking.
Whichever appears is the right one.

### Step 3 — Flow Rate, per spool

**Calibration → Flow Rate.** Two passes: coarse, then fine on the winner.
You are looking for a top surface that is smooth and continuous — not
pin-holed (under), not ridged and rough (over). Under-extrusion is the one
that hurts here: it thins walls and every clearance in the catalog opens up.

### Step 4 — record it, per slot

Write the numbers down against **the spool**, not the material. Brand, color
and even batch change them — a black PETG and a white PETG from the same
maker will not share a flow ratio, which matters directly for the 7″ frame's
three-filament print. If you are running the AMS, each slot's filament needs
its own numbers. §5's record card is there for exactly this.

**Max Volumetric Speed** is the one you can skip. It only binds when you are
pushing speed, and [`printing_petg_orca.md`](./printing_petg_orca.md) already
argues these parts do not need the P2S to be fast.

### Step 5 — prove it on a part, not a test pattern

A passing calibration pattern is not a passing fit. Print, in this order:

1. **§4's fit coupon** — the tolerance system's acceptance test. For the 7″
   dashboard the stations that matter are **SLIDE** and **SCREW**.
2. **The ring gauge** (§7b″) — five grams, and it checks the whole display
   outline against the real panel. If flow is off, this is where you see it
   as a contour that will not sit down.

**Do not fix a fit by changing calibration, and do not fix calibration by
changing the slicer's XY compensation.** They are three separate errors and
correcting one with another leaves you with two. Calibration belongs to the
spool, tolerance belongs to the `.scad`, and XY compensation stays at 0.

---

## 3 · Slicer

Install **Bambu Studio** (ships with the machine) or **Orca Slicer**, add the
P2S, and set the plate type to **Textured PEI**.

Then type in the values from
[**`printing_petg_orca.md`**](./printing_petg_orca.md). Do not start from a
stock "PETG Strength" preset and hope — the two settings that matter most for
this catalog are ones stock presets get wrong for us:

- **All XY compensation at 0.** Tolerance is in the model, not the slicer. Set
  it in both places and every fit fights you.
- **Supports off. Not "usually off" — off.** Every part self-supports in its
  intended orientation. If the overhang view lights up red, the part is
  oriented wrong.

---

## 4 · Print #1 — the fit coupon

**This is the first thing the machine prints.** One plate calibrates every fit
in the catalog, because all of the cases share one tolerance system.

```sh
cd docs/hardware/enclosure
openscad --export-format binstl -o coupon_base.stl  -D 'part="base"'  canary_fit_coupon.scad
openscad --export-format binstl -o coupon_mate.stl  -D 'part="mate"'  canary_fit_coupon.scad
openscad --export-format binstl -o coupon_strip.stl -D 'part="strip"' canary_fit_coupon.scad
```

> ⚠️ **Do not render `part="all"` and print it as one plate.** The `all` layout
> includes the **STRIP**, which is a **TPU** gasket bar — a different material,
> temperature and fan profile from the PETG base and mate. Print
> **base + mate in PETG together**, and the **strip in TPU separately** from the
> external spool holder. This is the most common way to waste the first plate.

Print flat, as exported, no brim, no supports.

### Reading the coupon

Each station is debossed with its name and maps to one parameter. Test it with
the real hardware, not by eye:

| Station | Test it with | Too tight → | Too loose → | Parameter |
|---|---|---|---|---|
| **CLIP** | a 1.2 mm PCB or scrap | clip won't seat / snaps | board rattles out | `clip_clear` |
| **POCKET** | the MATE's T-studs | studs won't enter | hangs sloppily | `kh_shank_d` / `kh_head_d` |
| **SLIDE** | the MATE's rib | rib binds in the channel | rib falls through | **`tol_slide`** |
| **GROOVE** | the TPU STRIP | strip won't seat flat | strip lifts out | `gasket_w` / `gasket_groove` |
| **PRESS** | a 6 × 2 mm magnet, 3 mm rod | needs force / cracks | magnet drops out | **`tol_press`** |
| **SCREW** | an M2 screw, ~0.3 N·m | strips the post | spins free | `screw_d` / **`tol_hole`** |
| **INSERT** | an M3 heat-set insert | insert won't sink | insert spins | `insert_d` |

**Adjust the `.scad` parameter and re-export — never the slicer's XY
compensation.** Fixing fit in the slicer corrects the same error twice and
throws off every other fit in the catalog.

You only need to care about the stations a given part uses. **For the 7"
dashboard that is SLIDE and SCREW** — `tol_slide` sizes the glass and board
pockets, `tol_hole` sizes the M3 clearance through the bezel ears. The case has
**no press fit at all** (no magnet pocket, no light pipe), so a PRESS result
changes nothing about it — don't reprint the bezel chasing one.

---

## 5 · Record your numbers

Fill this in once and it holds for the whole catalog. Keep it with the machine.

```
Machine: Bambu Lab P2S          Firmware: ____________  Date: __________
Plate: textured PEI             Nozzle: 0.4 mm  Layer: 0.20 mm
Filament: ______________________  dried? ____  flow-calibrated? ____

Coupon results
  tol_slide  default 0.20  ->  mine ______   (SLIDE binds / slides / drops)
  tol_press  default 0.10  ->  mine ______   (PRESS  needs force / snug / loose)
  tol_hole   default 0.30  ->  mine ______   (SCREW  strips / bites / spins)
  clip_clear default 0.25  ->  mine ______
Retraction distance (anti-stringing): ______ mm
```

> The catalog's defaults are for a well-calibrated machine and behave
> conservatively. **The 7" dashboard deliberately runs looser** —
> `tol_slide 0.25`, `tol_hole 0.35` — because a 200 mm part accumulates more
> thermal contraction across its span than a 70 mm one. If your coupon says you
> print tight, that is the part that most wants the extra. It declares no
> `tol_press`, because it has no press fit for one to tune.

---

## 6 · Print #2 — one small released case

Before the big one, print something **print-validated**: the WAP compact base +
lid. It is two parts, ~11 g, and it is in the committed, validated set — so if
it comes out wrong, the problem is your machine or filament, not the model.

```sh
./render.sh --no-png    # or just take the two committed STLs
```
Print `canary_wap_enclosure_compact_base.stl` (open side **up**) and
`canary_wap_enclosure_compact_lid.stl` (**face-down**). Snap them together. The
lid should close with a positive click and no rattle.

**If this part is good, the machine is good.** Everything after this is about
the 7" model, not the printer.

---

## 6b · Print #2½ — the hallway stick (your first AMS job)

> ⚠️ **Status: in development.** No committed STL, same convention as every
> in-development design.

Do this one before the 7" frame if you have the AMS loaded. It is the cheapest
three-filament print in the catalog — both halves of the
[S3 hallway stick](./canary_s3_lcd147.scad) on one plate, about **20 g** — and
it rehearses everything the frame will ask of you: an inlay registered inside
a recess, a tool change on a small part, and a purge tower that outweighs the
job.

```sh
python3 gen_3mf.py stick     # writes stick_case.3mf
```

Load the slots **before** you open the file. With one slot loaded there is
nothing for parts 2 and 3 to point at, and it reads as "the parts are
missing":

| slot | filament |
|---|---|
| 1 | Black PETG |
| 2 | Signal Yellow PETG (RAL 1003) |
| 3 | **Unfilled** white PETG — natural/translucent |

Slot 3 is the one to get right, and it is not a color choice. Those two strips
down the long walls are the light pipe that carries the WS2812 out of the gap
between the LCD module and the PCB. Carbon-filled, glitter or "matte white"
filament is opaque: the case will look correct and the seam will be dead.

Open `stick_case.3mf` directly — both objects are already positioned, already
registered, and already on their filaments. Slice as-is. Expect the purge
tower to dwarf the parts; that is normal for a 20 g three-filament plate and
the plate leaves it the whole right half of the bed on purpose.

**What it proves.** Press the bezel and the back together: they should click
positively and come apart only under a deliberate thumb flick at the far end.
Look down the side in the light — the white band should read as one unbroken
line from snap to snap, with three faint shadows where the internal ribs sit
behind it. If the band is a row of dashes rather than a line, you are printing
an older revision of the `.scad`.

**No AMS?** Print `part="bezel"` and `part="back"` in black, then
`part="fil_light"` alone in white and press the two strips in — that is what
the default `band_clear = 0.10` is for.

---

## 7 · Print #3 — the 7" dashboard

> ⚠️ **Status: in development — FIRST REAL PRINT DONE (2026-08).** That print
> corrected three things the drawings could not: the M3 offset **signs** (the
> pattern matched only with the panel upside down), the glass **corner
> radius** (r2.0 was the wrong direction — now 3.2, bracketed by the
> `radius_gauge`), and the SD cover's recess (an unprintable cantilever — now
> a 45° countersink). The model is still **not fully print-validated**; there
> is no committed STL, by the same convention that covers every
> in-development design. Expect to iterate, and keep recording what you find —
> it is exactly what turned v0.4 into v0.5.

### 7a · Measure your panel first

Most of the model's dimensions come from a vendor drawing, not from a board on
our bench. The one that was **actively disputed** — published summaries
disagreed about a "PCB height" of 97.60 mm vs 126.20 mm — is now resolved:
**126.20 mm is the M3 mount-hole X span**, measured off a reference case print
that fits the real panel; a summary had recorded the hole span as an outline
height. v0.3 ships the measured mount pattern (126.20 × 65.65 mm, centered
1.5 mm right / 0.9 mm up of the glass center with the panel mounted
buttons-down). Still put calipers on your actual board and check these against
the values echoed when you render:

| Measure | Parameter | Model default |
|---|---|---|
| Touch-glass width × height | `glass_w` / `glass_h` | 192.96 × 110.76 |
| Full panel thickness where the LCD module reaches — glass front → module can back | `glass_t` | 4.0 |
| Bare-glass BORDER thickness at the adhesive band — glass front → glass back, calipers on the edge, NOT over the module | `glass_edge_t` | 1.2 ✅ MEASURED — these are **two different measurements**: the border is just glass, the middle is glass + module. `glass_edge_t` sets the adhesive-ledge depth (`glass_edge_t + adh_t` behind the front face); `glass_t` sets the cavity and the whole rear stack. Assigning the thin edge reading to `glass_t` shortens the rear stack ~3.2 mm and the frame cannot seat the module |
| Glass corner radius | `glass_r` | **8.2 — measured**, not printed for. Three earlier revisions (2.0 → 3.0 → 3.2) each stepped rounder by 0.2 because that was the only increment the gauge could resolve, while the truth sat 5 mm away. **Measure it directly**: sit a square into the corner to find where the sharp bounding-box corner would be, measure the shortest distance `d` to the glass, `r = 2.414 × d` (8.2 → d 3.40). Then confirm on `part="radius_gauge"`, which now sweeps `rg_center` ± 1.5·`rg_step` — hunt coarse (`-D rg_center=8 -D rg_step=1`) before fine. Print to confirm, never to search. |
| Active (lit) area | `aa_w` / `aa_h` | 154.88 × 86.72 |
| LCD module can outline + center offset | `panel_core_w` / `panel_core_h` / `panel_core_dy` | 165.72 × 97.60, dy −1.435 ✅ MEASURED (19.76 + 126.20 + 19.76 = 165.72 exactly) — the `frame_glass` CI gate and the insertion asserts check the ledge ring against THIS outline, so measure the can, not the ledge |
| PCB outline | `pcb_w` / `pcb_h` | 165.72 × 97.60 |
| Tallest rear-side component | `comp_h` | 11.0 |
| Glass back → PCB front | `pcb_standoff` | 5.0 |
| M3 mount-hole spacing | `m3_dx` / `m3_dy` | 126.20 × 65.65 (measured) |
| M3 pattern offset from glass center | `m3_ox` / `m3_oy` | +1.5 / +0.9 — signs SETTLED by the first real print (the earlier −/− pattern matched only with the panel upside down) |
| Panel's own standoffs, PCB back → tip | `standoff_len` | 6.9 (frame only) |
| USB-C / UART / CAN / RS485 / battery centers | `bottom_open_*`, `side_open_*` | nominal ⚠️ |

The model asserts on the fatal combinations — a mount pattern that falls outside
the board, a lip too narrow to retain the glass, no air gap left above the PCB —
so a bad measurement usually fails the render with a message rather than wasting
filament. Two parameters deserve individual attention:

- **`pcb_standoff`** alone decides whether the bezel lip reaches the glass or
  leaves it rattling. Get this one right.
- **`m3_ox` / `m3_oy`** — the mount pattern is **not centered on the glass**,
  and that offset is why a panel cannot simply be rotated 180° inside a case
  drawn for the other orientation: the holes stop lining up. The signs are
  now **settled by the first real print** — v0.4's −1.5/−0.9 matched the
  panel only upside down, exactly the failure a sign error produces (every
  boss moves by twice the offset) — v0.5 ships +1.5/+0.9. Every part still
  assumes the panel's **native** mounting — no image rotation in firmware —
  which puts BOOT/RESET at the **top** edge in use; the frame's window,
  labels and keyhole wall mounts are all drawn for that.

### 7b · Print the corner gauge — not the case

```sh
openscad --export-format binstl -o lcd7_gauge_tray.stl  -D 'part="gauge_tray"'  canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_gauge_bezel.stl -D 'part="gauge_bezel"' canary_s3_lcd7.scad
```

These are one real corner of each part, cut out of the actual geometry by
intersection — they cannot drift from what you are about to print. Together
they are **≈16.5 g against the case pair's ≈158 g: about 10 %.**

Assemble the pair around your panel's own corner with one M3 and check, in
order:

1. The glass corner **drops into the cavity** without forcing.
2. The lip **lands on the black border** and the window **clears the lit area**.
3. The M3 **threads into the post** and the PCB boss meets its mount hole.
4. Calipers across the closed pair read the **echoed closed height** (27.6 mm at
   the defaults — the render prints the exact figure).

Anything wrong here is a 16 g mistake. The same thing wrong on the slab is 158 g
and most of a day.

### 7b′ · Or: the one-piece frame

v0.3 added a **`part="frame"`** alternative to the bezel + tray pair, based on
a case layout print-proven against the real panel: the slab drops in face-first
through the front opening, the board hangs on the panel's **own white M3
standoffs**, and **4 × M3×8–10 driven from the back** thread into those
standoffs — the screws, not a ledge, set the glass depth: it lands
`glass_guard` below the front rim, which is **−0.2 today — the rim sits one
layer BELOW the glass** so a cleaning cloth cannot catch on it (positive
values trade that for a drop-protection recess).
It carries a beveled BOOT/RESET window in the top wall — the button edge in
native mounting — with debossed labels (back view: **BOOT left, RESET
right**), gill vents on the side walls, exhaust slots flanking the button
window, a back grille, **keyhole wall mounts in all four corners** (hang the
case over the top pair — or all four — and slide it down; every catch points
up at the button edge, and the bottom pair pins the case flat against the
wall: first-print feedback), and a **microSD opening through the back plate**
covering the socket, the card's downward slide travel and room for a
fingertip — so the card goes in and out without ever being dropped inside the
case. An "SD" deboss marks it; the first print corrected its position 6.35 mm
toward the plate's center — still confirm against **your** board. The shell
carries chamfers at both the plate edge and the opposite rim.

v0.4 gives the frame its power story and closes v0.3's "USB-C has no external
access" gap. Centered on the bottom wall is a **USB pass-through**: a beveled
stadium opening sized to pass the power cable's **overmold head**
(`usb_head_w`/`usb_head_h` — **measure your cable**, overmolds vary), and the
brand lettering is now **cut through the wall as slat-stencil intake vents**
flanking it — the bottom edge breathes through its own name, and the old
debossed edge brand and pill intakes are gone. Horizontal tie bands interrupt
every glyph, so letter counters stay attached and nothing needs bridging.

Three **TPU fitments** finish the openings (TPU 90–95 A, **external spool —
never the AMS**, §0):

- **`grommet_usb`** — a slit wire grommet for the port. Assembly order
  matters: with the case still empty, feed the head in through the port,
  plug it into the panel, **leave a service loop**, seat the panel, then open
  the grommet at its slit, wrap it around the wire and press it in. Its inner
  flange bears flat on the wall's inner face, so a yank on the cable loads
  the frame — not the board's connector. Tune `usb_wire_d` to your cable
  jacket. Note the trade: once the panel is adhered, replacing the *cable*
  means pulling the panel — treat the cable as semi-permanent and the
  grommet as the serviceable part.
- **`plug_buttons`** — a captive plug for the BOOT/RESET window: its cap
  nests the window's bevel, a 45° lip snaps in behind the wall (it cannot
  fall out or be pushed inside), and the buttons are pressed **through** the
  plug via two pips under the face dimples — it never needs to come out.
  Set `btn_reach` to your measured wall-to-button gap minus ~0.5.
- **`plug_sd`** — a peel-open cover for the SD opening, **countersunk
  flush**: the plate carries a 45° rim and the cap a matching tapered edge,
  so both print clean back-plate-down and the taper self-centers the cap.
  (The v0.4 flat-floored recess left a cantilevered ring hanging over the
  opening — it drooped on the first real print; this replaces it.)
  Since v0.6 the cover is **leashed**: push its arrowhead barb through the
  plate's anchor hole with a thumb (it mushrooms inside — captive from that
  moment), lay the strap in its skin channel, press the cap home. To open,
  a fingernail scoop bites the rim at the card end — the nail lands
  directly under the cap's tapered edge; peel, and the cover dangles on its
  leash, attached. A deliberate yank cams the barb free for service.

TPU fits are tuned by `tpu_squeeze` (waist interference) and `tpu_grip`
(grommet bore vs jacket) — TPU seats by squeeze, so its knobs are
interferences, not clearances; your PETG coupon numbers do not transfer.

The glass lands on an **adhesive ledge** matched to the panel's own adhesive
strips (10 mm down each side, 6 mm along the button edge, 2 mm over the FPC
at the bottom): peel the liners, drop the panel in, press — the screws
become backup rather than the only thing setting the glass depth. Every
ledge carries a solid 45° wedge down to its wall, which is why **the frame
prints back-plate-down** (the exported STL is already in that orientation):
that way up, the ledges are fully self-supporting — no slicer supports, no
sacrificial geometry. The frame is branded debossed on the back plate and on
the visible bottom edge (crisp deboss since v0.6 — see below; v0.4–v0.5
cut the words through the wall as slat-stencil vents).

v0.6 is the production-hardening pass — **no fit knob moved**, three
durability/finish features landed:

- **Keyhole doublers.** Every wall keyhole now bears on a pad on the plate's
  inner face: the screw head clamps **5 mm of material** (3 mm plate + 2 mm
  doubler, `khm_pad_t`) instead of 3, the slide's catch shears a wider
  section, and each mouth carries a lead-in chamfer so the case slips over
  the screw heads without catching an elephant-footed rim. The frame's M3
  bosses and the tray's PCB bosses also gained 45° root fillets — a boss
  fails by shearing at its root, so that section now spreads into the plate.
- **Adhesive rails.** Two outlined zones on the back plate (17 × 74 mm at
  x ±12) are guaranteed **smooth and uninterrupted** — no grille slot,
  keyhole, boss pocket or deboss ever lands inside one, and a CI fit gate
  (`frame_adh_rail`) fails any change that cuts a zone. They fit 15.9 × 70 mm
  **interlocking picture-hanging strip pairs** (e.g. Command Medium) — pairs,
  not single stretch-release foam strips, deliberately: the mounted case
  fully covers its strips, so a single strip's pull tab would be sealed
  behind it, unreachable. With pairs, removal follows the product's own
  doctrine: pull the case straight off its wall halves (grip it by the side
  gills / bottom port), and every wall tab is then exposed for its stretch
  release. Stick inside the moat outlines, strips vertical, tabs down, and
  wipe the zone with IPA first. The zone's finish is the build plate's
  finish — a smooth sheet gives the best bond, but the foam bonds through
  light texture too. No screws, no drill: the renter's wall mount.
  **The trade, stated plainly:** on a build with the back grille switched on
  (`plate_grille`), the rails' keepouts cost it several columns — the render
  echo computes the exact count and area for your config from the same
  predicate that cuts the slots. At stock dims the plate carries no grille at
  all, so today the rails cost it nothing; what they do cost is the plate's
  composition, since a rail moat crosses the field the badge and the help
  symbol sit on. The convection path proper — bottom-wall
  intake → top-wall exhaust — is untouched, and there is no better spot:
  the SD zone, boss pockets and keyhole pads own every other clear column.
  Screw-mount builds should set `adh_rails=false` and reclaim every slot.
- **Two-color, one extruder.** The frame prints back-plate-down, and two
  z-bands are deliberately isolated so plain filament-change pauses (or AMS
  layer swaps — §0) give a finished two-tone part with no painting:
  - **Accent back skin** — start in the accent color and swap to the body
    color at **z = 0.8 mm** (the rim-chamfer band). The back face and its
    edge chamfer print in the accent; every back deboss floor sits at
    1.2 mm (`label_back_depth`), so BOOT/RESET, SD, the brand line and the
    rail moats all show through in the body color.
  - **Accent front ring** — swap back to the accent at **z = 22.9 mm**
    (`fr_depth − 0.6` — the render echo prints your exact number if you
    changed the stack). The last 0.6 mm of the print is only the front rim
    and its glass entry chamfer, so the swap paints a clean accent ring
    around the glass and nothing else.

  Use either band alone or both; skipping both swaps prints the ordinary
  single-color part.
- **Crisp edge lettering.** The bottom-edge brand is now a clean 1.0 mm
  **deboss** — the slat-stencil vents are gone. Print feedback drove this:
  the tie bands every through-cut glyph needed (or its counters fall out)
  read as horizontal scan lines across the letters in the flesh. A deboss
  stays attached everywhere by the wall web behind it — no ties, no lines,
  no islands. The intake the stencil carried moved to a **shadow gill row**
  (16 small pills, ≈1 cm² — the stencil's open area) tucked into the wall
  band's last few millimeters before the back plate: invisible against a
  wall and over the dock's well, still feeding the same bottom-in → top-out
  convection path, and asserted clear of the grommet flange and the plate.
- **The SD cover is leashed.** The v0.5 hinge tongue only *hooked* under the
  plate edge — a full peel could slide it out and the cover is gone. It is
  replaced by a strap ending in an **arrowhead barb** that pushes through a
  small anchor hole in the plate and mushrooms inside: the peeled cover
  dangles, captive, and only a deliberate yank (45° cam faces) frees it.
  **The frame carries the hole and the strap channel — re-export the frame
  before printing**, or the barb has nowhere to go; two CI gates
  (`sd_tether_hole`, `sd_tether_barb`) keep hole and barb aligned forever.
  The other fitments already pass the stays-attached test: the button plug
  is snap-captive behind its wall and never needs to come out, and the
  grommet lives wrapped around the cable itself — pulled from the port, it
  stays on the cord.

**With an AMS** (§0 — rigid filaments only; TPU still prints from the
external spool), the whole two-tone story runs itself, no pauses:

- **The two z-bands** — in Bambu Studio right-click the layer slider at the
  two echoed heights (0.8 mm and 22.9 mm at stock dims) and *Add color
  change*; the AMS swaps automatically. Accent back skin, body-color words
  in the debosses, accent front ring.
- **The lettering** — *Color Painting → Smart Fill* on the frame body: one
  click per debossed letter (the bottom-edge words, and BOOT/RESET/SD/brand
  on the back plate if you like) floods that recess with the accent
  filament. Expect a prime tower and purge waste on every color-change
  layer — that is the AMS working, not a mis-slice.
- **Filament picks**: the case must stay **PETG** (it runs hot — never the
  PLA slot). Load body and accent in any two AMS slots and map them in the
  slicer's filament list.
- **⚠️ On THIS z-band recipe, if the part carries the help QR the BODY
  filament must be the DARK one.** Every deboss floor on the back — labels,
  brand lockup and the QR's modules — prints in the **body** color, inside a
  skin that prints in the **accent**. A QR reader wants dark modules on a
  light field, so the old house pairing (white body + canary-yellow accent)
  produces **white modules on yellow: unscannable**, and leaves every other
  back label barely legible into the bargain. Pair a **dark body** (graphite
  or black) with a **light accent skin** and the whole back reads
  dark-on-light — labels included. The shipped palette already has the dark
  body (`pal_body` is Black), so this path is satisfied by default. A
  single-color print is fine as-is: the 1.2 mm floors read dark by shadow.

  This rule is specific to the z-band fallback, where the modules have no
  filament of their own and must inherit the body. **The AMS path below is
  different** — there the modules get their own filament, and on the shipped
  palette (`qr_style = "bare"`, `accent_groups` carrying `"qr"`) that is
  YELLOW ON BLACK: light modules on a dark field, inverted from the spec on
  purpose, for looks. See the README's palette section. Either way: **scan the
  part before you print nine more.**

```sh
# ~6 g, print FIRST after any radius doubt: four corner sockets bracketing
# glass_r — the one that hugs the panel's corner with no daylight and no
# bind is your radius
openscad --export-format binstl -o lcd7_radius_gauge.stl -D 'part="radius_gauge"' canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_frame_gauge.stl -D 'part="frame_gauge"' canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_frame.stl       -D 'part="frame"'       canary_s3_lcd7.scad
# TPU fitments — print these on their OWN plate, TPU from the external spool
openscad --export-format binstl -o lcd7_grommet_usb.stl  -D 'part="grommet_usb"'  canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_plug_buttons.stl -D 'part="plug_buttons"' canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_plug_sd.stl      -D 'part="plug_sd"'      canary_s3_lcd7.scad
```

Same doctrine as 7b: **gauges before the slab, smallest first.** The
`radius_gauge` (~6 g) settles the corner **shape**; the `frame_gauge` (one
corner containing a boss and a wall keyhole, ~10 %) then proves the whole
corner — assembled on the panel's corner with one screw it checks `glass_r`
in context, the mount-offset **signs** (`m3_ox`/`m3_oy` — the screw only
threads home if they're right), and `standoff_len` (the glass sits exactly
`glass_guard` **below** the rim only if that's right). **Read that pass
criterion off `glass_guard` in the `.scad`, not off this page.** It is `0`
today, so **a step DOWN onto the case is the pass**: run a finger off the
glass outward and it must drop onto the case, never climb into an edge. A rim
you can feel as a ridge is a fail — that is exactly what the first fitting
print showed. The criterion moves with the sign of `glass_guard`: positive
means a recess is the pass, zero means flush, negative means the step down. The
frame and gauge print
**back-plate-down, as exported** — no supports, no brim unless a corner
lifts.

### 7c · Print the case

```sh
openscad --export-format binstl -o lcd7_bezel.stl -D 'part="bezel"' canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_back.stl  -D 'part="back"'  canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_stand.stl -D 'part="stand"' canary_s3_lcd7.scad   # optional
openscad --export-format binstl -o lcd7_stand_gauge.stl -D 'part="stand_gauge"' canary_s3_lcd7.scad  # print BEFORE the stand
```

| Part | Footprint | Solid volume | Mass (upper bound) | Orient | Brim |
|---|---|---|---|---|---|
| **bezel** | 208.5 × 126.3 × 7.0 | 37.8 cm³ | ~48 g | **face-down** | no ² |
| **back** | 208.5 × 126.3 × 20.6 | 86.5 cm³ | ~110 g | outer face **down** | no ² |
| stand (optional) | 174 × 126 × 94.2 | 265.4 cm³ | ~338 g ¹ | flat (base down) | no |
| stand_gauge | 22 × 126 × 94.2 | 44.0 cm³ | ~56 g ¹ | flat (base down) | no |

¹ Quoted at 100 % of solid volume, the same upper-bound convention as
[printer_selection.md](./printer_selection.md#running-cost--per-device-computed-from-the-meshes).
The stand is a bulky part, so its real mass at 30 % gyroid is *well* below this
bound; the slicer's own estimate is the authority. Print times are deliberately
not quoted — nothing here has been benchmarked.

² Start without a brim on a clean plate. These are the widest, flattest parts in
the catalog, which is the shape most prone to lifting a corner — if one lifts,
a 5 mm outer brim is the fix, not a hotter bed.

**Both case parts fit a 256 mm bed with room to spare, but they are the largest
things in the catalog** — the bezel is ~208 mm across the corner ears against
the released set's largest part at 120.5 mm. Two consequences:

- **Bed adhesion matters more than it ever has here.** A wide, thin, face-down
  part is exactly the shape that lifts at a corner. Clean plate, no skipping §1.4.
- **Print in PETG or ASA — never PLA.** This panel's backlight, the ESP32-S3 and
  its regulators all run hot in a closed box. PLA creeps at temperatures this
  case will reach.
- **The material call is also the drop call.** A handling drop loads exactly
  the places PLA fails brittle: the wall corners and the plate rim. PETG bends
  where PLA snaps; ASA adds UV life for a sunny room. Give the case **4 wall
  loops** (the 2 mm walls then print as solid perimeters — stronger and cleaner
  than loops + gap fill) and don't lower the layer height below 0.2 to "add
  strength": more, hotter-bonded layers beat many cold thin ones for impact.
  The geometry does its part (a 45° fillet ring ties the walls
  into the back plate; the SD leash carries root fillets) — but note
  `glass_guard` is **−0.2**, so the rim sits below the glass and a face-down
  drop lands on the panel — which now stands slightly proud, so it lands a
  little harder. That is the deliberate price of a screen you can wipe; raise
  `glass_guard` to buy the protection back — the slicer settings
  are the other half of the deal.

**Keep the vents clear** when you mount it. The convection path is real and
directional: **intake along the bottom wall, exhaust along the top wall**.
Mounting it flat against a wall with the top slots blocked converts a
ventilated case into an oven — and that matters more on the one-piece `frame`
than it used to, because its back plate is now a composed face with no grille,
no vent eggs and no radio window: the only openings in it are the card window
and the four keyholes. Nothing in this repo measures the requirement. If a
built case runs warm the recovery is **two** knobs, not one: `plate_grille =
true` alone restores the field with the badge's column still reserved (~5 cm²),
and `badge_column = false` alongside it is the whole field (~21 cm²). The
render echoes both figures so neither has to be quoted from memory.

### 7b″ · The ring gauge — five grams, and it gates the 110 g print

The cheapest part in the catalog, and the one to print first:

```sh
openscad --export-format binstl -o lcd7_ring_gauge.stl \
         -D 'part="ring_gauge"' canary_s3_lcd7.scad
```

**4.42 cm³, ~5.6 g, flat, four layers at 0.3** — no supports, no brim.

Its inner edge *is* the frame's glass opening: 193.26 × 111.06 at r8.35, which
is the slab plus `frame_reveal` (0.15) per side. Lay the panel into it, or hold
it over the panel, and every outline error shows at once:

| what you see | what it means |
|---|---|
| will not drop in, or rattles | `glass_w` / `glass_h` wrong |
| daylight at the arcs, straights touching | `glass_r` wrong |
| uniform slop or uniform bind all round | `frame_reveal` wrong |

The radius gauge answers *what is the corner*; the frame gauge answers *does
one corner assemble*. Neither can show a wrong overall size, because neither
has an opposite edge to measure against. This does — and it costs five grams.

It debosses its own dimensions, so a loose ring on a bench is never a mystery.

### 7c″ · The color + fit coupon — print this before the frame

One part answers two questions at **20.9 cm³ — a fifth of a frame** (body 20.56, ink 0.31, accent 0.02):

```sh
for f in body accent; do   # add 'ink' if you have put a group back on it
  openscad --export-format binstl -o lcd7_coupon_$f.stl \
           -D "part=\"coupon_$f\"" canary_s3_lcd7.scad
done
```

It is cut from the bottom edge, running from the center lockup **out through a
corner**, at full depth — so it carries the glass pocket, the front bezel band
and the back plate all on one piece.

| end | what it proves |
|---|---|
| center | the lockup: INK product name over the ACCENT company line, and the USB port for a grommet trial-fit |
| corner | the glass pocket's radius and the front bezel — offer the panel's own corner into it |

**Do not hand-assemble these from the STLs** — use the packager, which writes
one 3MF whose parts are already registered and already on filaments 1/2/3:

```sh
python3 gen_3mf.py tests      # writes BOTH pre-flight plates, in order
```

The manual path (load `coupon_body`, **Add part → Load** the other two) still
works, but it is one misclick from a silently wrong print: any centering or
drop-to-bed on an added part separates the inlays from their recesses, and the
result slices cleanly and looks fine in preview. An instruction that must be
obeyed for the output to be right is a defect, not a doc problem — so the
packager exists and this is the path to take. Add the filament **slots** in
Bambu Studio first, or parts 2 and 3 have nothing to point at and read as
missing.

Read it for: black bleeding into white on the deboss floors (raise the flush
volume), the lockup sitting flush rather than proud or sunk, where the bezel
swap landed, and whether the panel corner drops into the pocket with the glass
face landing where `glass_guard` says it should — at its current value of
**−0.2** the case sits one layer below the glass, so a finger run off the
glass steps down onto the case rather than up into an edge.

Do **not** judge bed adhesion from it — it is a slice out of a larger part, so
its sawn edges behave nothing like the frame's continuous perimeter.

### 7c‴ · The QR plaque — the one test only a phone can run

Every other gate in this project is geometry, and geometry cannot tell you
whether a printed QR **decodes**. `qr_back_cell` is 1.3 mm — about three line
widths at a 0.42 mm extrusion — and whether that survives a real first layer,
a real purge and a real phone camera is a question with no CAD answer. So
print it and scan it.

```sh
python3 gen_3mf.py color   # the QR plaque rides plate 2 with the coupon
```

It is the **back plate only** — 3 mm, not the frame's 23.5 — cut from the real
frame at the real QR, so the deboss depth, module shape and first layer are the
ones the case will actually print. Two filaments:

| slot | gets | why |
|---|---|---|
| 1 | body (**Black**) — the field | on the shipped palette the field *is* the case |
| 2 | ink (**White**) — the modules | the modules always get their own filament |

⚠️ **That is white-on-black — INVERTED from the spec**, and it is the shipped
default (`qr_style = "bare"`), chosen so the symbol sits *in* the black case
instead of inside a bright rectangle on it. Readers are specified to want dark
modules on a light field. Many current phone cameras decode inverted symbols;
plenty of older and embedded readers refuse, and nothing in this repo can tell
you which yours is — **which is exactly why this coupon exists.** If it will
not scan, set `qr_style = "plaque"`: the white prints as a light plaque with
the modules punched through it in black, and the conforming polarity is back.

There is deliberately **no accent slot**. Yellow on a finder pattern is how you
make a symbol that looks finished and never scans.

The plaque stops exactly at the quiet zone (`qr_back_reach`) and not a
millimeter further — that radius is what the model's asserts guarantee is clear
of rail moats, grille slots and keyholes. A 1.5 mm "nicer border" was tried
first and clipped an adhesive-rail moat, leaving a detached 0.8 mm hairline of
black at the plaque edge.

Read it for: **does it scan**, first, from about the distance someone would
actually hold a phone — and on this polarity try more than one phone, because
inverted support is the thing that varies between them. Then look at the module
edges for the two filaments bleeding into each other — the QR is where purge
contamination costs you contrast rather than just looking untidy, and it is the
first place you will see it.

If it does not scan, raise `qr_back_cell` before anything else; the symbol is
21×21, so every 0.1 mm of cell is 2.1 mm of field, and the asserts will tell
you when it stops fitting its keepout.

### 7c′ · The two-color case (AMS)

The one-piece frame prints as a **black case with everything the back plate
says — the mark, the lockup and the help QR's modules — in yellow**. It is a
two-filament print (it used to be three; a white QR beside a yellow mark made
the plate a three-way argument), and it is laid out so the AMS barely works:
color changes are confined to two thin bands and the ~105 layers in between
never change tools.

> **⚠️ If your case came into Bambu Studio in ONE color, this is the step you
> want.** There are only two ways that happens, and both are easy to hit:
> exporting `part="frame"` (that mesh is the whole case as a single solid — it
> has no color information in it, and it is *correct* for the single-extruder
> recipe in §7b′), or exporting the filament parts and opening them with
> **File → Open**, which loads them as independent OBJECTS. Studio then
> auto-arranges objects across the plate, which is right for objects and fatal
> for parts of one thing. The parts are only meaningful in the SAME coordinate
> frame: the mark sits in a recess cut into the body, so moving one a
> millimeter puts the bird beside its own hole rather than in it.

**Run the packager. Do not hand-assemble it.**

```sh
python3 gen_3mf.py frame      # writes lcd7_frame.3mf
```

That writes ONE 3MF: one object, its volumes already registered to each other
and already assigned to filament slots. Open it directly — **File → Open**, no
importing of parts, no re-centering. The packager prints what it packed; read
that line rather than trusting this one, because the slot map is derived from
`ink_groups` / `accent_groups` in the case file and follows them.

1. **Add the filament slots in Bambu Studio first.** With one slot loaded there
   is nothing for the other volumes to point at, and it looks like the parts
   are missing.
2. Load them **by SLOT NUMBER** — the 3MF assigns slots, not colors, so a spool
   in the wrong slot prints a different case:

   | slot | filament | what it prints |
   |---|---|---|
   | 1 | `pal_body` — **Black** | the case, the bezel ring, the vent mouths |
   | 2 | — | **unused on this palette.** `ink_groups` is empty, so the packager reports the ink volume as EMPTY and packs nothing onto slot 2 |
   | 3 | `pal_accent` — **Signal Yellow** (RAL 1003) | everything the plate says: the mark, the lockup (SECURACV / CANARY / ERRERlabs), and the help QR's modules |

   Slot 2 keeps its number rather than being closed up, so a three-filament
   build is still `ink_groups = ["qr"]` and nothing else — and so a plate
   sliced from an older 3MF cannot land the accent on the wrong tool.

3. Open `lcd7_frame.3mf` and slice.

The **"not from Bambu Lab, load geometry and color data only"** dialog on open
is expected and is the good outcome — it means the filament assignment was
read. If you never see that dialog, you opened an STL.

**Rehearse the colors on the coupon first** (§7c): `gen_3mf.py color` writes a
plate with the color coupon and the QR plaque on it, and it is a few grams
against the frame's ~160.

<details><summary>Exporting the STLs by hand (only if you are not using the packager)</summary>

```sh
for f in body ink accent; do
  openscad --export-format binstl -o lcd7_fil_$f.stl -D "part=\"fil_$f\"" canary_s3_lcd7.scad
done
```

Then in Studio: load `lcd7_fil_body.stl`, right-click it → **Add part → Load**,
and add the other two — **Add part**, never File → Open. **Do not re-center,
rotate, or drop-to-bed the added parts.** All three are exported in the same
coordinate frame as `part="frame"`, so they arrive already registered.

This is the path the packager exists to replace: an instruction that must be
obeyed for the output to be correct is a design defect, not a documentation
problem. It got the first real attempt wrong.

</details>

| Band (print z) | What is there | Filaments in play |
|---|---|---|
| 0 – 1.2 | back skin, every deboss floor | body + ink + accent |
| 1.2 – 22.7 | the shell — nothing but wall | body only |
| 22.7 – 23.3 | front bezel ring + edge chamfer | body only, unless `bezel_color` says otherwise |

That middle band is the whole point: it is 21.5 mm of a 23.3 mm part with **zero**
tool changes, so the purge tower stays short. The bezel is the full ring rather
than "top and bottom bands" for the same reason — the front rim is a uniform
2 mm all the way round, so bands would not be a visible distinction, they would
just add a tool change to every one of those last layers.

The render echoes the exact bands and the exact group→filament assignment for
*your* config — read that, not this table, if you have changed anything.

**Why this needs the AMS at all.** The single-extruder recipe (§7b′, color
swaps at fixed heights) cannot produce a case with a QR whose modules differ
from its field at all. The QR's modules are the deboss *floors*, and on a
z-swap every deboss floor prints in whichever filament is running at floor
height — so the modules and the plate around them come out the SAME color, and
there is no swap height that fixes it, because the field and the modules are at
different heights but the wrong way round. Giving the modules their own
filament is what makes the symbol possible. For the same reason, **the modules
must stay in the ink filament** — yellow against either body color has nowhere
near enough contrast to decode.

**The AMS carries the case's three rigid filaments and nothing else.** The TPU
fitments still come off the external spool (§0), and they print on their own
plate regardless.

**If you have no AMS, print `part="frame"` and follow §7b′ — not `fil_body`.**
`fil_body` is the body's *share* of the three-way split: the final 0.6 mm bezel
band has been subtracted out of it, because that material belongs to `fil_ink`.
Printed on its own it is a case with the front bezel missing, not a
single-color case. Only the full `frame` mesh is the whole part.

### 7d · Assembly order

1. Seat the PCB on the four molded bosses; drive **M3 into the boss pilots**.
2. Drop the panel in — the glass back sits flush with the tray's wall top.
3. Lay the bezel on; the lip closes onto the glass border.
4. **M3 × 16–20 from the front**, through the bezel's counterbored ears, self-
   tapping into the full-height corner posts. Snug, not tight — see below.

> **PETG creeps under sustained clamp load.** If this is going to live screwed
> together for months, either re-torque 24 h after assembly, or fit brass
> heat-set inserts rather than self-tapping into plastic.

---

## 8 · Keeping it running

From Bambu's schedule and farm practice, with the two items specific to us:

| Interval | Task |
|---|---|
| Every print | **Wipe the plate with IPA.** Skin oil is the #1 adhesion failure. |
| Monthly | Inspect belts, fans, nozzle, cutter, camera |
| Every 3 months | Clean + lubricate XY |
| Every 5 months | Clean + lubricate Z |
| As needed | Hotend assembly (~$21–25); silicone sock, PTFE, filters |
| ~Annually | **Textured PEI plate (~$35)** — it *is* our finish surface; replace it on wear |

- **PETG chews PTFE** faster than PLA does. Cheap part, easy swap, put it on the
  schedule.
- **CF filament is abrasive** and will destroy a brass nozzle. If you ever print
  the Vision bracket, dedicate a hardened hotend to it.

---

## 9 · When something goes wrong

| Symptom | Most likely cause | Fix |
|---|---|---|
| First layer won't stick | Skin oil on the plate | Soap + water, then IPA. Not just IPA over a fingerprint. |
| Wide part lifts at a corner | Draft / cold plate / dirty plate | Close the door, verify plate temp, re-clean |
| Strings and whiskers | **Wet filament**, then retraction | Dry 60–65 °C 4–6 h; then tune retraction |
| Audible popping while printing | Wet filament | Dry it. Do not chase temperature. |
| Bulging first layer ("elephant foot") | Nozzle too close / bed too hot | Fix at the printer. The model already carries a chamfer — do **not** add negative XY compensation |
| Fits are tight everywhere | Over-extrusion, or double-corrected | Check flow calibration; confirm slicer XY compensation is **0** |
| Fits are tight only on the 7" | Span-scale contraction | This is what the looser `tol_*` are for; re-check on the gauge |
| Part tears the plate | PETG on **smooth** PEI | Use textured, or a thin release layer |
| Overhang view is red | Wrong orientation | Re-seat per the [cheat-sheet](./printing_petg_cura.md#per-model-cheat-sheet). Do not enable supports |

---

## What to send back

This catalog's 7" case has never been printed. If you print it, the useful
things to record are: your measured panel dimensions (§7a), whether the gauge
passed on the first try, your coupon numbers (§5), and photographs of anything
that did not fit. That is what turns this design from **in development** into a
committed, print-validated STL.
