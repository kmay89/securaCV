# Canary WAP — 3D-Printable Enclosure (v0.4)

Parametric, printable cases for the Canary WAP (XIAO ESP32-S3 Sense), referenced
as a "future add" in the [Peripheral Build Plan](../canary_peripheral_build_plan.md)
(§6.6). Authored in [OpenSCAD](https://openscad.org) so every dimension is a
tweakable parameter, with rendered print-ready STLs committed alongside.

Two variants from one source (`variant = "battery" | "compact"`):

| Variant | Outer size | Battery | Use |
|---------|-----------|---------|-----|
| **battery** | ≈ **80 × 39 × 17 mm** | LiPo bay beside the board | Standalone / portable Canary |
| **compact** | ≈ **38 × 36 × 17 mm** | none (USB-powered) | Smaller footprint, mains/USB powered |

> The compact case is sized so the **four M2 screw posts sit in true corners
> beside the board** (a 17.5 mm-wide PCB plus a post each side needs ≈ 30 mm) —
> that's why it isn't as tiny as the bare-board reference, which relied on
> snap-fit clips rather than corner screws.

![Battery variant — base and lid](./preview_all.png)
![Compact variant — base and lid](./preview_compact.png)

| | |
|---|---|
| **Source** | [`canary_wap_enclosure.scad`](./canary_wap_enclosure.scad) (parametric, both variants) |
| **Battery STLs** | [`..._battery_base.stl`](./canary_wap_enclosure_battery_base.stl), [`..._battery_lid.stl`](./canary_wap_enclosure_battery_lid.stl) |
| **Compact STLs** | [`..._compact_base.stl`](./canary_wap_enclosure_compact_base.stl), [`..._compact_lid.stl`](./canary_wap_enclosure_compact_lid.stl) |
| **Fasteners** | 4 × **M2** self-tapping screws (8–10 mm) into the corner posts |

## Dimensional basis (verified)

These dimensions were reconciled against **Seeed's official spec** and a
**reference enclosure STL** supplied for this work:

| Source | Figure | Used for |
|--------|--------|----------|
| Seeed XIAO ESP32-S3 ([p-5627](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html)) | PCB **21.0 × 17.5 mm**, 2.54 mm pitch, USB-C on a short edge | `board_l`, `board_w`, USB position |
| Reference case STL (`smallest_esp32_V2`) | bbox **20.0 × 35.1 × 9.8 mm**, internal cavity ≈ 9.8 mm deep, board sits 17.5 mm-width across, extra length = USB-C cable clearance | sanity check on cavity depth + USB-only footprint → the **compact** variant |

Changes from v0.1: `board_w` corrected 17.8 → **17.5 mm** (Seeed official); USB-C
opening widened to **10.5 × 6.5 mm** to clear a real cable boot (the connector
body is ~8.9 × 3.2 mm); added the **compact** variant.

**v0.3 — printability & connected structure:** the four corner **screw posts are
now fused to both adjacent walls** with gussets (no thin free-standing towers),
and the **board standoffs are joined into a perimeter frame and ribbed to the
screw posts** so the whole interior prints as one rigid piece with good bed
adhesion. The cavity is sized to guarantee the posts sit in real corners clear of
the PCB (this enlarged the compact variant).

**v0.4 — screwless board retention:** four **cantilever snap clips** along the
board's long edges hold the PCB down by clicking over its top — **no screws to
mount the board**. The board drops onto the standoffs and the clips snap over the
edge; a 45° lead-in cams them open on insertion. The screw posts still take the
**lid** screws. Tune `clip_t`/`clip_hook` to your material, or set
`board_clips = false` to omit them. (The lid screws remain optional too — the lip
already locates the lid; the clips are purely for the board.)

> ⚠️ **Still a reference — verify before printing.** Seeed publishes the PCB
> outline but not every component height; the camera-lens/LED/buzzer positions on
> the lid are nominal. **Measure your board and test-print the lid first.** A
> printed case is **not IP-rated** on its own — see climate/IP guidance in build
> plan §9. The compact variant assumes a **plain XIAO ESP32-S3** (lower profile);
> if you fit the **Sense** camera, keep `board_stack_h ≈ 8` or raise it.

## What's in the box (both variants)

| Feature | Where | Notes |
|---------|-------|-------|
| Board cradle | base | 4 standoffs + perimeter frame lift the PCB `standoff_h` (3 mm) off the floor |
| **Board snap clips** | base, board long edges | 4 cantilever tabs hook over the PCB — it **clicks in, held with no screws** |
| Battery bay | base (battery variant only) | low rim cradles a 503450 LiPo beside the board |
| **USB-C port** | base, −X wall | 10.5 × 6.5 mm, centred on PCB-top height |
| **Camera/sensor window** | lid | `cam_win_d` (9 mm) — keep optically clear |
| **Light-pipe / LED port** | lid | `lp_d` (3.2 mm) over the status LED |
| **Buzzer + pressure vent** | lid | recessed seat for a GORE adhesive vent + ring of sound/pressure holes |
| **Tamper-magnet pocket** | lid underside | blind pocket holds a 6 mm magnet over the reed/Hall switch |
| Screw lid | 4 corners | countersunk M2 into self-tapping posts; lip nests into the base |

## Render / regenerate the STLs

Requires OpenSCAD (CLI). The helper renders all four:

```bash
./render.sh
# or one part of one variant:
openscad --export-format binstl -o out.stl -D 'variant="compact"' -D 'part="lid"' canary_wap_enclosure.scad
```

Open the `.scad` in the OpenSCAD GUI to tune parameters with the Customizer
(grouped: What-to-render, Board, Battery, Shell, Standoffs, Ports, Lid features,
Tamper magnet), then F6 → Export.

## Key parameters to check first

| Param | Default | Why you'd change it |
|-------|--------:|---------------------|
| `variant` | `"battery"` | `"compact"` drops the battery bay and shrinks the box |
| `board_stack_h` | 8.0 | Clearance above the PCB. ~8 covers the Sense camera; a plain board needs ~4–5 |
| `batt_l/w/h` | 50/34/6 | Match **your** cell (battery variant) |
| `cam_dx/dy`, `lp_dx/dy`, `vent_dx/dy`, `mag_dx/dy` | — | Feature positions **from board centre** — set from a real measurement |
| `usb_w/h/z` | 10.5/6.5/0 | Align to your USB-C cable boot |
| `fit_gap` | 0.20 | Lid-to-base clearance; tune for your printer |
| `board_clips` | true | Snap tabs that retain the PCB without screws (set false to omit) |
| `clip_t` / `clip_hook` | 1.5 / 0.8 | Tab flex vs. grip — thin/less for brittle PLA, thicker for a firmer click |

## Suggested print settings

- **Material:** PETG or ASA for heat/UV exposure (PLA only for indoor/bench).
- **Layer height:** 0.2 mm. **Walls:** 3 perimeters. **Infill:** 20–30 %.
- **Orientation:** both parts flat, open side up — no supports.
- **Camera window:** keep it a clean circle; glue in a thin clear disc to seal.

## Assembly

1. (battery variant) Drop the LiPo in the bay; route to the XIAO BAT pads.
   **Read the battery safety notice in build plan §6.5 first.**
2. **Press the PCB straight down** until the four edge clips snap over it,
   USB-C aligned to the wall cutout — no screws needed for the board. (To remove,
   gently splay the clips outward and lift.)
3. Press a 6 mm magnet into the lid pocket; add a GORE vent over the seat if
   sealing; insert a light pipe in the LED port.
4. Close the lid (lip nests into the base) and drive 4 × M2 screws.

## Links
- [Peripheral Build Plan & BOM](../canary_peripheral_build_plan.md) — parts, wiring, climate/IP guidance
- [Bench bring-up](../bench_bringup.md) — get it chirping before you box it up
