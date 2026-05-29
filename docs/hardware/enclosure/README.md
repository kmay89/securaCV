# Canary WAP — 3D-Printable Enclosure (v0.5)

Parametric, printable case for the Canary WAP (XIAO ESP32-S3 Sense), referenced
as a "future add" in the [Peripheral Build Plan](../canary_peripheral_build_plan.md)
(§6.6). Authored in [OpenSCAD](https://openscad.org), and built as a **configurator**:
tick the peripherals you fitted and the case rebuilds itself — adding the right
bays, ports and cutouts and resizing to suit.

## Configure it (you tick what you have)

Open `canary_wap_enclosure.scad` in the **OpenSCAD GUI** → the **Customizer**
panel shows your options as checkboxes. Tick what's on your device:

| Checkbox | When ticked, the case adds… |
|----------|------------------------------|
| `opt_camera` | sensor **window** on the lid + extra internal height (Sense camera). Off ⇒ shorter cavity for a plain XIAO |
| `opt_buzzer` | **vent** (hole-ring + GORE seat) on the lid |
| `opt_led` | **light-pipe** port on the lid |
| `opt_battery` | **LiPo bay** beside the board (enlarges the case) |
| `opt_gps` | internal **GPS module bay** (L76K) |
| `opt_tamper` | **magnet pocket** on the lid underside |
| `opt_touch` | thinned **touch window** on the lid (capacitive sensing through the wall) |
| `opt_antenna` | **bulkhead hole** in the side wall for a u.FL/SMA antenna |

Prefer a one-click starting point? Set **`preset`** to `battery_full` or
`compact_plain` (overrides the checkboxes), or leave it `custom` to use them.
Then set `part` to `base`, `lid`, `all`, or `coupon` and export.

The two committed example presets:

| Preset | Outer size | What's on it |
|--------|-----------|--------------|
| **battery_full** | ≈ **86 × 39 × 17 mm** | camera + buzzer + LED + LiPo bay + GPS bay + tamper |
| **compact_plain** | ≈ **38 × 36 × 17 mm** | plain board (no camera), buzzer + LED, USB-powered |

> Corner posts always sit in **true corners beside the board** (a 17.5 mm PCB +
> a post each side needs ≈ 30 mm) — that's why even the compact case isn't as
> tiny as the bare-board reference, which used snap clips instead of screws.

![battery_full preset — base and lid](./preview_all.png)
![compact_plain preset — base and lid](./preview_compact.png)

| | |
|---|---|
| **Source** | [`canary_wap_enclosure.scad`](./canary_wap_enclosure.scad) (configurator) |
| **Example STLs** | [`..._battery_base/lid.stl`](./canary_wap_enclosure_battery_base.stl), [`..._compact_base/lid.stl`](./canary_wap_enclosure_compact_base.stl) |
| **Clip test coupon** | [`..._clip_coupon.stl`](./canary_wap_enclosure_clip_coupon.stl) — print first to tune the snap fit |
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

**v0.5 — peripheral configurator + clip coupon:** the case is now driven by
**peripheral checkboxes** (`opt_camera/buzzer/led/battery/gps/tamper/touch/antenna`)
plus `preset`. Toggling a peripheral adds/removes its lid cutout or internal bay
and **resizes the box** (e.g. enabling GPS appends a module bay; disabling the
camera shortens the cavity). Added a **clip test coupon** (`part = "coupon"`) so
you can dial in the snap fit before printing the whole case.

**v0.6 — mechanical fixes (from review):** M2 self-tap pilot tightened
`2.0 → 1.6 mm` so threads actually bite; the broken battery cradle rim (its inner
cut deleted its own walls and the cell fouled the corner posts) is replaced with
**two transverse ribs** — the snug side walls cradle the cell and the case is
lengthened so the bay clears the +X screw posts; and the **lid lip is notched at
the USB end** so the cable plug/overmold can't jam it.

> ⚠️ **Still a reference — verify before printing.** Seeed publishes the PCB
> outline but not every component height; the camera-lens/LED/buzzer positions on
> the lid are nominal. **Print the clip coupon and test-fit, then measure your
> board and check the lid** before the full box. A printed case is **not IP-rated**
> on its own — see climate/IP guidance in build plan §9.

## What's in the box (features appear when their peripheral is ticked)

| Feature | Where | Toggle |
|---------|-------|--------|
| Board cradle + frame | base | always (standoffs + perimeter frame, PCB `standoff_h` off the floor) |
| **Board snap clips** | base, long edges | `board_clips` — 4 tabs hook over the PCB, **clicks in, no screws** |
| **USB-C port** | base, −X wall | always (10.5 × 6.5 mm, PCB-top height) |
| Battery bay | base | `opt_battery` |
| GPS module bay | base | `opt_gps` |
| Antenna bulkhead | base, +X wall | `opt_antenna` |
| **Camera/sensor window** | lid | `opt_camera` |
| **Light-pipe / LED port** | lid | `opt_led` |
| **Buzzer + pressure vent** | lid | `opt_buzzer` (GORE seat + hole-ring) |
| **Cap-touch window** | lid | `opt_touch` (thinned to `touch_wall`) |
| **Tamper-magnet pocket** | lid underside | `opt_tamper` |
| Screw lid | 4 corners | countersunk M2 into self-tapping posts; lip nests into base |

## Render / regenerate the STLs

Requires OpenSCAD (CLI). The helper renders both example presets + the coupon:

```bash
./render.sh
# or build your own config directly:
openscad --export-format binstl -o my_base.stl \
    -D 'preset="custom"' -D 'opt_gps=true' -D 'opt_camera=false' -D 'part="base"' \
    canary_wap_enclosure.scad
# or the clip test coupon:
openscad --export-format binstl -o coupon.stl -D 'part="coupon"' canary_wap_enclosure.scad
```

## Key parameters to check first

| Param | Default | Why you'd change it |
|-------|--------:|---------------------|
| `opt_*` | see table | **Tick the peripherals you fitted** — the case adapts |
| `preset` | `"custom"` | `battery_full` / `compact_plain` one-click configs (override the checkboxes) |
| `part` | `"all"` | `base` / `lid` / `all` / **`coupon`** (clip-fit tester) |
| `batt_l/w/h`, `gps_l/w/h` | — | Match **your** cell / GPS module |
| `cam_dx/dy`, `lp_dx/dy`, `vent_dx/dy`, `mag_dx/dy`, `touch_dx/dy` | — | Feature positions **from board centre** — set from a real measurement |
| `usb_w/h/z` | 10.5/6.5/0 | Align to your USB-C cable boot |
| `fit_gap` | 0.20 | Lid-to-base clearance; tune for your printer |
| `clip_t` / `clip_hook` | 1.5 / 0.8 | Tab flex vs. grip — tune on the **coupon** first |

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
