// ============================================================================
//  Canary Sense — IN-WALL SINGLE-GANG FLUSH MOUNT  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Radar works through plastic, so the Sense witness can live INSIDE the wall:
//  a one-piece faceplate that mounts to a US single-gang old-work box on the
//  standard 6-32 device screws (83.3 mm spacing). The MR60BHA2 carrier +
//  stacked XIAO ride clip rails on the BACK of the plate; the plate's center
//  is the RADOME (thin flat membrane). Looks like a smart switch/keypad —
//  zero visual footprint. Power: in-box USB (Class-2 low voltage rules apply;
//  do NOT share a box with mains conductors — check local code).
//
//  2026-08-23: rrect2d and the board clip now come from the shared libraries
//  (canary_core_lib / canary_snap_lib) — same mesh, one home; the clip's
//  insertion strain is now gated on every render instead of trusted.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. Measure
//     your box, carrier, and stack; radome rules as in canary_sense_enclosure.
// ============================================================================

use <canary_core_lib.scad>   // rrect2d — the catalog's shared helpers
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_board_lib.scad>  // board registry — the MR60 numbers the knobs cite

/* [What to render] */
part = "plate";      // ["plate"]

/* [Wallplate] — standard US single-gang footprint */
plate_w  = 70.0;     // wallplate width
plate_h  = 115.0;    // wallplate height
plate_t  = 3.2;      // plate thickness outside the radome
screw_gap = 83.3;    // 6-32 device-screw spacing (single-gang standard)
dev_screw_d = 3.7;   // 6-32 clearance
lid_edge = 1.0;      // face edge chamfer

/* [Boards] — MR60BHA2 carrier + stacked XIAO ESP32-C6. MEASURE */
vm_l     = 44.0;      // brd_l("mr60") — canary_board_lib
vm_w     = 36.0;      // brd_w("mr60") — canary_board_lib
stack_sock_h = 11.5;  // brd_stack_sock_unmeasured() — the doorbell bench measured 6.5 (canary_board_lib)
vm_front_h   = 3.5;   // carrier front-side TALLEST part (connectors etc.) — MEASURE
pcb_t    = 1.0;
board_clear = 0.6;

/* [Radome window] */
radome_t  = 1.5;   // ≈ half-wave in PETG/ASA at 60 GHz — the low-reflection optimum;
                   // AVOID 0.7–1.1 mm (quarter-wave band, ~20 % reflection)
rad_win_x = 24.0;
rad_win_y = 24.0;
rad_dx    = 0.0;
rad_dy    = 6.0;

/* [Face features] — offsets from the board center */
lp_d   = 3.0;  lp_dx  = 13.0;  lp_dy  = -14.0;   // WS2812 light pipe
lux_d  = 3.5;  lux_dx = -13.0; lux_dy = -14.0;   // BH1750 aperture

/* [Board snap clips] */
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;  // the WAP's proven 1.0/0.5 — canary_snap_lib gates the strain

/* [Print tolerances] */
tol_slide = 0.20;  tol_press = 0.10;  tol_hole = 0.30;  // catalog trio — core_tol_*(), canary_core_lib

/* [Quality] */
$fa = 3; $fs = 0.4;

vm_standoff = 4.5;                    // rails hold the carrier's front parts (vm_front_h) clear of the
rail_h  = vm_standoff;                // solid plate back; the XIAO hangs deeper into the box
assert(vm_standoff > vm_front_h, "vm_standoff must clear the carrier's front-side parts (vm_front_h)");
assert(radome_t >= 0.6 && radome_t < plate_t, "radome_t must be printable and thinner than plate_t");
assert(vm_w + 2*(clip_clear + clip_t) + 2 < plate_w, "carrier too wide for the plate");
echo(str("Canary Sense single-gang plate v0.1-dev — ", plate_w, " x ", plate_h,
         " mm, radome ", radome_t, " mm  (IN DEVELOPMENT)"));

module plate() {
    union() {
        // face plate with chamfered edge (prints face-down: back features rise)
        difference() {
            union() {
                linear_extrude(plate_t - lid_edge) rrect2d(plate_w, plate_h, 4);
                translate([0, 0, plate_t - lid_edge - 0.01]) hull() {
                    linear_extrude(0.01) rrect2d(plate_w, plate_h, 4);
                    translate([0, 0, lid_edge]) linear_extrude(0.01)
                        rrect2d(plate_w - 2*lid_edge, plate_h - 2*lid_edge, max(0.5, 4 - lid_edge));
                }
            }
            // NOTE: model z=0 is the BACK of the plate; face at z=plate_t
            // radome window: blind thinning from the back, flat membrane at the face
            translate([rad_dx, rad_dy, -0.1])
                linear_extrude(plate_t - radome_t + 0.1) rrect2d(rad_win_x, rad_win_y, 3);
            // 6-32 device screws (vertical slots for old-work box tolerance)
            for (sy = [1, -1]) hull() for (dy = [-1.5, 1.5])
                translate([0, sy*screw_gap/2 + dy, -0.1]) cylinder(d = dev_screw_d, h = plate_t + 1);
            for (sy = [1, -1])                                     // head seats on the face
                hull() for (dy = [-1.5, 1.5])
                    translate([0, sy*screw_gap/2 + dy, plate_t - 1.6])
                        cylinder(d1 = dev_screw_d, d2 = dev_screw_d + 3.6, h = 1.7);
            // light pipe + lux apertures (board center = plate center)
            translate([lp_dx, lp_dy, -0.1]) cylinder(d = lp_d + 2*tol_press, h = plate_t + 1);
            translate([lux_dx, lux_dy, -0.1]) cylinder(d = lux_d, h = plate_t + 1);
        }
        // carrier rails + clips on the BACK (single-gang box device opening is ~45 x 65:
        // the carrier + XIAO recess into the box; rails only need to clear the clips)
        for (s = [1, -1]) {
            difference() {
                translate([s*(vm_w/2 - 1.5) - 1.5, -(vm_l - 1)/2, -rail_h])
                    cube([3, vm_l - 1, rail_h + 0.01]);
                translate([s*(vm_w/2 - 1.5), 0, -rail_h/2])
                    cube([5, clip_w + 2, rail_h + 1], center = true);
            }
            // the clip is canary_snap_lib's WAP cantilever, strain-gated on
            // every render; turned 90° so the beam stands on this plate's
            // left/right edges (90° turns are exact in OpenSCAD's degree
            // trig, so the mesh cannot move), then mirrored into the box
            mirror([0, 0, 1]) rotate([0, 0, -s*90])
                snap_boardclip(0, vm_w/2, 1, 0, rail_h + pcb_t,
                               w = clip_w, t = clip_t, hook = clip_hook,
                               hook_h = clip_hook_h, clear = clip_clear);
        }
    }
}

// export face-down so the radome membrane prints flat on the bed in one
// uniform stack of layers (same convention as the sense front)
if (part == "plate") translate([0, 0, plate_t]) rotate([180, 0, 0]) plate();
else translate([0, 0, plate_t]) rotate([180, 0, 0]) plate();
