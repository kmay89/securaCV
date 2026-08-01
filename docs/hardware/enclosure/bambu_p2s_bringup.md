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
different proposition: it holds your PETG, your ASA and a contrast colour for
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
   levelling, vibration compensation and motor characterisation. Do not skip it
   to "just print something" — every dimension in this catalog is drawn to a
   tolerance system that assumes a calibrated machine.
3. **Fit the textured PEI plate**, and treat it as a *finish surface*, not a
   consumable. Every A-surface in this catalog prints **face-down**, so the
   plate's texture *is* the visible finish on lids, bezels and faces.
4. **Wash the plate with dish soap and water, then IPA.** Skin oil is the single
   most common adhesion failure on these machines. Handle the plate by its
   edges from here on.
5. **Check which nozzle shipped** and note it. The stock 0.4 mm hardened-capable
   assembly is fine for PETG/ASA/TPU. **Carbon-fibre filament will destroy a
   plain brass nozzle** — the Vision bracket is the only CF part in the catalog,
   and it is not on this bring-up path.
6. **Pin the firmware version** and write it down. Bambu firmware updates can
   change behaviour mid-production; you want to know what you calibrated on.

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

## 7 · Print #3 — the 7" dashboard

> ⚠️ **Status: in development.** `canary_s3_lcd7.scad` is render-, mesh- and
> assembly-checked in CI, but **not print-validated** — nobody has yet held one.
> There is no committed STL for it, by the same convention that covers every
> in-development design. You are printing the first one; expect to iterate, and
> please record what you find.

### 7a · Measure your panel first

Most of the model's dimensions come from a vendor drawing, not from a board on
our bench. The one that was **actively disputed** — published summaries
disagreed about a "PCB height" of 97.60 mm vs 126.20 mm — is now resolved:
**126.20 mm is the M3 mount-hole X span**, measured off a reference case print
that fits the real panel; a summary had recorded the hole span as an outline
height. v0.3 ships the measured mount pattern (126.20 × 65.65 mm, centred
1.5 mm right / 0.9 mm up of the glass centre with the panel mounted
buttons-down). Still put calipers on your actual board and check these against
the values echoed when you render:

| Measure | Parameter | Model default |
|---|---|---|
| Touch-glass width × height | `glass_w` / `glass_h` | 192.96 × 110.76 |
| Glass thickness at the edge | `glass_t` | 4.0 |
| Glass corner radius | `glass_r` | 2.0 (a fitting case's r≈2.7 cavity still gaps — the slab is sharper) |
| Active (lit) area | `aa_w` / `aa_h` | 154.88 × 86.72 |
| PCB outline | `pcb_w` / `pcb_h` | 165.72 × 97.60 |
| Tallest rear-side component | `comp_h` | 11.0 |
| Glass back → PCB front | `pcb_standoff` | 5.0 |
| M3 mount-hole spacing | `m3_dx` / `m3_dy` | 126.20 × 65.65 (measured) |
| M3 pattern offset from glass centre | `m3_ox` / `m3_oy` | −1.5 / −0.9 (measured, native mounting — verify signs) |
| Panel's own standoffs, PCB back → tip | `standoff_len` | 6.9 (frame only) |
| USB-C / UART / CAN / RS485 / battery centres | `bottom_open_*`, `side_open_*` | nominal ⚠️ |

The model asserts on the fatal combinations — a mount pattern that falls outside
the board, a lip too narrow to retain the glass, no air gap left above the PCB —
so a bad measurement usually fails the render with a message rather than wasting
filament. Two parameters deserve individual attention:

- **`pcb_standoff`** alone decides whether the bezel lip reaches the glass or
  leaves it rattling. Get this one right.
- **`m3_ox` / `m3_oy`** — the mount pattern is **not centred on the glass**,
  and that offset is why a panel cannot simply be rotated 180° inside a case
  drawn for the other orientation: the holes stop lining up. Every part in
  v0.3 assumes the panel's **native** mounting — no image rotation in
  firmware — which puts BOOT/RESET at the **top** edge in use; the frame's
  window, labels and keyhole wall mounts are all drawn for that. Verify the
  offset signs on your board; a sign error moves every boss by twice the
  offset.

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

v0.3 adds a **`part="frame"`** alternative to the bezel + tray pair, based on a
case layout print-proven against the real panel: the slab drops in face-first
through the front opening, the board hangs on the panel's **own white M3
standoffs**, and **4 × M3×8–10 driven from the back** thread into those
standoffs — the screws, not a ledge, pull the glass flush with the front rim.
It carries a bevelled BOOT/RESET window in the top wall — the button edge in
native mounting — with debossed labels (back view: **BOOT left, RESET
right**), gill vents on the side walls, intake/exhaust slot rows, a back
grille, two **keyhole wall mounts** near the top corners (hang the case over
two pan-head screws and slide it down; the catches point up at the button
edge), and **paired bevelled service windows in each short wall**, placed
from the Rev1.2 board photos: microSD + UART1 and the two USB-C ports on the
right (back view), the CAN/I²C/sensor cluster and battery/RS-485 terminals
on the left. The SD socket sits ~14 mm inboard, so the card window is sized
for a fingertip to reach in and work the push-push socket — an "SD" deboss
marks it. Window positions are photo-derived: **measure your board** before
printing. The shell carries the catalog's modelled foot chamfer (slicer
elephant-foot compensation stays 0) and a matching back-rim chamfer.

The glass lands on an **adhesive ledge** matched to the panel's own adhesive
strips (10 mm down each side, 6 mm along the button edge, 2 mm over the FPC
at the bottom): peel the liners, drop the panel in, press — the screws
become backup rather than the only thing setting the glass depth. **Before
the panel goes in, snap out the dashed ribs** standing in the glass pocket:
they exist only to hold the wide ledge up during printing (a 10 mm shelf
cannot print over air), and they break out cleanly from the front opening.
The frame is branded on the back plate and on the visible bottom edge.

```sh
openscad --export-format binstl -o lcd7_frame_gauge.stl -D 'part="frame_gauge"' canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_frame.stl       -D 'part="frame"'       canary_s3_lcd7.scad
```

Same doctrine as 7b: **print the gauge first.** It is one corner containing a
boss and a wall keyhole; assembled on the panel's corner with one screw it
proves the glass corner radius (`glass_r`), the mount-offset **signs**
(`m3_ox`/`m3_oy` — the screw only threads home if they're right), and
`standoff_len` (the glass sits flush with the rim only if that's right). The
frame prints **face-down**, no supports, no brim unless a corner lifts.

### 7c · Print the case

```sh
openscad --export-format binstl -o lcd7_bezel.stl -D 'part="bezel"' canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_back.stl  -D 'part="back"'  canary_s3_lcd7.scad
openscad --export-format binstl -o lcd7_stand.stl -D 'part="stand"' canary_s3_lcd7.scad   # optional
```

| Part | Footprint | Solid volume | Mass (upper bound) | Orient | Brim |
|---|---|---|---|---|---|
| **bezel** | 208.5 × 126.3 × 7.0 | 37.8 cm³ | ~48 g | **face-down** | no ² |
| **back** | 208.5 × 126.3 × 20.6 | 86.5 cm³ | ~110 g | outer face **down** | no ² |
| stand (optional) | 210 × 131.2 × 63.1 | 238.2 cm³ | ~303 g ¹ | flat | no |

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

**Keep the vents clear** when you mount it. The convection path is real and
directional: **intake along the bottom wall, exhaust along the top wall**, back
grille radiating in between. Mounting it flat against a wall with the top slots
blocked converts a ventilated case into an oven.

### 7d · Assembly order

1. Seat the PCB on the four moulded bosses; drive **M3 into the boss pilots**.
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
