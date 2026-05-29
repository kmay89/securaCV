# Canary WAP — 3D-Printable Enclosure (v0.1)

A parametric, printable case for the Canary WAP (XIAO ESP32-S3 Sense + LiPo),
the enclosure referenced as a "future add" in the
[Peripheral Build Plan](../canary_peripheral_build_plan.md) (§6.6). Authored in
[OpenSCAD](https://openscad.org) so every dimension is a tweakable parameter.

![Enclosure preview — base and lid](./preview_all.png)

| | |
|---|---|
| **Source** | [`canary_wap_enclosure.scad`](./canary_wap_enclosure.scad) (parametric) |
| **Print-ready** | [`canary_wap_enclosure_base.stl`](./canary_wap_enclosure_base.stl), [`canary_wap_enclosure_lid.stl`](./canary_wap_enclosure_lid.stl) |
| **Outer size (defaults)** | ≈ **80 × 39 × 17 mm** (base 15 mm + lid 2 mm) |
| **Fasteners** | 4 × **M2** self-tapping screws (8–10 mm) into the corner posts |

> ⚠️ **This is a v0.1 reference — verify before printing.** Defaults are *nominal*
> for the XIAO ESP32-S3 Sense (PCB ≈ 21 × 17.8 mm) and a 503450-class LiPo, but
> the camera-lens position, LED location, USB-C connector height and your exact
> battery vary. **Measure your hardware, adjust the parameters, and test-print the
> lid first** (it carries the fiddly features). Treat it as a starting point, not
> a finished product. For weatherproofing, see the climate/IP guidance in the
> build plan §9 — a printed case is *not* IP-rated on its own.

## What's in the box (literally)

| Feature | Where | Notes |
|---------|-------|-------|
| Board cradle | base, USB end | 4 standoffs lift the PCB `standoff_h` (3 mm) off the floor |
| Battery bay | base, far end | low rim cradles a 503450 LiPo beside the board |
| **USB-C port** | base, −X wall | centred on the PCB-top height |
| **Camera/sensor window** | lid | `cam_win_d` (9 mm) aperture — keep optically clear |
| **Light-pipe / LED port** | lid | `lp_d` (3.2 mm) over the status LED |
| **Buzzer + pressure vent** | lid | recessed seat for a GORE adhesive vent + a ring of sound/pressure holes |
| **Tamper-magnet pocket** | lid underside | blind pocket holds a 6 mm magnet over the board's reed/Hall switch |
| Screw lid | 4 corners | countersunk M2 into self-tapping posts; lip nests into the base |

## Render / regenerate the STLs

Requires OpenSCAD (CLI). Use the helper script or call directly:

```bash
./render.sh                 # writes both STLs
# or:
openscad --export-format binstl -o canary_wap_enclosure_base.stl -D 'part="base"' canary_wap_enclosure.scad
openscad --export-format binstl -o canary_wap_enclosure_lid.stl  -D 'part="lid"'  canary_wap_enclosure.scad
```

Open `canary_wap_enclosure.scad` in the OpenSCAD GUI to tune parameters with the
Customizer (they're grouped: Board, Battery, Shell, Standoffs, Ports, Lid
features, Tamper magnet), then F6 → Export.

## Key parameters to check first

| Param | Default | Why you'd change it |
|-------|--------:|---------------------|
| `board_stack_h` | 8.0 | Height clearance above the PCB (camera/B2B stack). Too low → lid won't close |
| `batt_l/w/h` | 50/34/6 | Match **your** cell; set `batt_enable=false` for USB-only |
| `cam_dx/dy`, `lp_dx/dy`, `vent_dx/dy`, `mag_dx/dy` | — | Feature positions **from the board centre** — set from a real measurement |
| `usb_w/h/z` | 9.5/4.5/0 | Align the cutout to your USB-C connector |
| `fit_gap` | 0.20 | Lid-to-base clearance; loosen for a tighter printer, tighten for a sloppy one |
| `screw_d` | 2.0 | M2 self-tap; widen ~0.2 mm if you thread inserts instead |

## Suggested print settings

- **Material:** PETG or ASA for any heat/UV exposure (PLA only for indoor/bench).
- **Layer height:** 0.2 mm. **Walls:** 3 perimeters. **Infill:** 20–30 %.
- **Orientation:** print both parts flat, **open side up** (base cavity up, lid
  features up) — no supports needed.
- **Camera window:** keep it a clean circle; glue in a thin clear acrylic/glass
  disc if you need to seal it.

## Assembly

1. Drop the LiPo into the battery bay; route its lead to the XIAO BAT pads.
   **Read the battery safety notice in the build plan §6.5 first.**
2. Seat the PCB on the four standoffs, USB-C aligned to the wall cutout.
3. Press a 6 mm magnet into the lid pocket; stick a GORE vent over the vent seat
   if sealing; insert a light pipe in the LED port.
4. Close the lid (lip nests into the base) and drive 4 × M2 screws into the posts.

## Links
- [Peripheral Build Plan & BOM](../canary_peripheral_build_plan.md) — parts, wiring, climate/IP guidance
- [Bench bring-up](../bench_bringup.md) — get it chirping before you box it up
