// ============================================================================
//  SecuraCV Canary WAP — 3D-printable enclosure (parametric)
//  Board: Seeed XIAO ESP32-S3 Sense + optional LiPo (placed beside the board)
//  Features: light-pipe port, buzzer/pressure vent, camera/sensor window,
//            USB-C access, board standoffs, M2 screw lid, tamper-magnet pocket.
//
//  Units: millimetres.  CAD: OpenSCAD (https://openscad.org).
//
//  ⚠️ VERIFY BEFORE PRINTING. Defaults are nominal for the XIAO ESP32-S3 Sense
//     (PCB ~21.0 x 17.8 mm) plus a 503450-class LiPo. Measure YOUR board, battery,
//     camera-lens position and USB-C connector and adjust the parameters below.
//     Test-print the lid (it carries the fiddly features) before the full box.
//
//  Layout (top view):   [ board @ USB end ]  [ gap ]  [ battery ]
//                         -X  ............................  +X
//
//  Render a part:  set `part` then F6 (or use the CLI in the README).
// ============================================================================

/* [What to render] */
part = "all";  // ["base","lid","all"]

/* [Board] */
board_l        = 21.0;  // XIAO PCB length (USB end to far end, along X)
board_w        = 17.8;  // XIAO PCB width (along Y)
board_h        = 1.2;   // PCB thickness
board_stack_h  = 8.0;   // tallest thing above the PCB (camera / B2B stack)
board_clear    = 0.6;   // per-side clearance around the PCB

/* [Battery compartment] */
batt_enable    = true;
batt_l         = 50.0;  // LiPo length  (503450 ~ 50 x 34 x 5 mm)
batt_w         = 34.0;  // LiPo width
batt_h         = 6.0;   // LiPo thickness
batt_gap       = 2.5;   // gap between board zone and battery zone

/* [Shell] */
wall_t         = 2.0;   // side wall thickness
floor_t        = 2.0;   // base floor thickness
lid_t          = 2.0;   // lid top thickness
lip_h          = 4.0;   // how far the lid lip drops into the base
lip_t          = 1.2;   // lid lip wall thickness
fit_gap        = 0.20;  // lid-to-base sliding clearance (tune for your printer)
corner_r       = 3.0;   // outside corner radius

/* [Standoffs / screw posts] */
standoff_h     = 3.0;   // PCB sits this high off the floor (clearance for bottom parts)
standoff_d     = 4.0;
post_d         = 5.0;   // corner screw posts (lid screws thread into these)
screw_d        = 2.0;   // M2 self-tapping pilot
screw_head_d   = 4.0;   // countersink on lid
screw_head_h   = 2.0;

/* [USB-C port] — on the board's USB end (-X short wall) */
usb_w          = 9.5;
usb_h          = 4.5;
usb_z          = 0.0;   // extra lift relative to PCB-top centring

/* [Lid features] — offsets are measured FROM THE BOARD CENTRE (mm). Measure your board! */
// Camera / sensor window
cam_win_d      = 9.0;
cam_dx         = 0.0;
cam_dy         = 0.0;
// Light-pipe / status-LED port
lp_d           = 3.2;
lp_dx          = 5.0;
lp_dy          = 5.0;
// Buzzer + pressure vent (recess seats an adhesive GORE vent; ring of holes passes sound/pressure)
vent_pad_d     = 12.0;
vent_pad_depth = 0.8;
vent_hole_d    = 1.6;
vent_ring_d    = 6.0;
vent_holes     = 6;
vent_dx        = 7.0;
vent_dy        = -4.0;

/* [Tamper magnet] — blind pocket on the LID underside, over the board's reed/Hall switch */
mag_enable     = true;
mag_d          = 6.4;   // 6 mm magnet + fit
mag_h          = 3.2;
mag_dx         = -6.0;
mag_dy         = 5.0;

/* [Quality] */
$fn = 64;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
board_zone_l = board_l + 2*board_clear;
batt_zone_l  = batt_enable ? (batt_gap + batt_l) : 0;

inner_l = board_zone_l + batt_zone_l + 1.0;
inner_w = max(board_w + 2*board_clear, batt_enable ? batt_w : 0) + 1.0;
cav_h   = standoff_h + board_h + board_stack_h + 1.0;   // internal height above floor

out_l  = inner_l + 2*wall_t;
out_w  = inner_w + 2*wall_t;
base_h = floor_t + cav_h;

pcb_z   = floor_t + standoff_h;                 // absolute z of PCB underside
board_cx = -inner_l/2 + board_clear + board_l/2 + 0.5;   // board centre X
board_cy = 0;
batt_cx  = inner_l/2 - batt_l/2 - 0.5;          // battery centre X

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------
module rrect(l, w, r, h) {                       // rounded rectangular prism
    linear_extrude(height = h)
        offset(r = r) offset(r = -r)
            square([l, w], center = true);
}

// corner screw-post centres (just inside the cavity corners)
function post_xy() = [
    [ inner_l/2 - post_d/2 - 0.2,  inner_w/2 - post_d/2 - 0.2],
    [-inner_l/2 + post_d/2 + 0.2,  inner_w/2 - post_d/2 - 0.2],
    [ inner_l/2 - post_d/2 - 0.2, -inner_w/2 + post_d/2 + 0.2],
    [-inner_l/2 + post_d/2 + 0.2, -inner_w/2 + post_d/2 + 0.2],
];

// ----------------------------------------------------------------------------
//  BASE
// ----------------------------------------------------------------------------
module base() {
    union() {
        // hollow shell with the USB-C wall opening
        difference() {
            rrect(out_l, out_w, corner_r, base_h);
            // cavity
            translate([0, 0, floor_t])
                rrect(inner_l, inner_w, max(0.1, corner_r - wall_t), cav_h + 1);
            // USB-C cutout on the -X short wall, centred on PCB-top height
            translate([-out_l/2, board_cy, pcb_z + board_h + usb_h/2 + usb_z])
                cube([wall_t*3, usb_w, usb_h], center = true);
        }

        // PCB standoffs rise from the floor at the board corners
        bx = board_l/2 - standoff_d/2;
        by = board_w/2 - standoff_d/2;
        for (sx = [bx, -bx], sy = [by, -by])
            translate([board_cx + sx, board_cy + sy, floor_t])
                cylinder(d = standoff_d, h = standoff_h);

        // corner screw posts with self-tapping pilots
        difference() {
            for (p = post_xy())
                translate([p[0], p[1], floor_t]) cylinder(d = post_d, h = cav_h);
            for (p = post_xy())
                translate([p[0], p[1], floor_t + 2.0]) cylinder(d = screw_d, h = cav_h);
        }

        // battery cradle rim on the floor (kept strictly inside the cavity)
        if (batt_enable)
            translate([batt_cx, 0, floor_t])
                difference() {
                    rrect(min(batt_l + 2.4, batt_zone_l - 0.5),
                          min(batt_w + 2.4, inner_w - 0.5), 1.0, 1.4);
                    rrect(batt_l + 0.8, batt_w + 0.8, 0.5, 3);
                }
    }
}

// ----------------------------------------------------------------------------
//  LID
// ----------------------------------------------------------------------------
module vent_cluster(x, y) {
    translate([x, y, 0]) {
        // recessed seat for the adhesive GORE vent on the OUTER face
        translate([0, 0, lid_t - vent_pad_depth])
            cylinder(d = vent_pad_d, h = vent_pad_depth + 1);
        // ring of through-holes for sound + pressure equalisation
        for (i = [0 : vent_holes - 1])
            rotate([0, 0, i * 360 / vent_holes])
                translate([vent_ring_d/2, 0, -1])
                    cylinder(d = vent_hole_d, h = lid_t + 2);
    }
}

module lid() {
    cam = [board_cx + cam_dx, board_cy + cam_dy];
    lp  = [board_cx + lp_dx,  board_cy + lp_dy];
    vnt = [board_cx + vent_dx, board_cy + vent_dy];
    mag = [board_cx + mag_dx, board_cy + mag_dy];

    union() {
        difference() {
            rrect(out_l, out_w, corner_r, lid_t);

            translate([cam[0], cam[1], -1]) cylinder(d = cam_win_d, h = lid_t + 2);   // camera window
            translate([lp[0],  lp[1],  -1]) cylinder(d = lp_d,      h = lid_t + 2);   // light pipe
            vent_cluster(vnt[0], vnt[1]);                                             // buzzer vent

            // countersunk lid screws over the posts
            for (p = post_xy()) {
                translate([p[0], p[1], -1]) cylinder(d = screw_d + 0.6, h = lid_t + 2);
                translate([p[0], p[1], lid_t - screw_head_h])
                    cylinder(d1 = screw_d + 0.6, d2 = screw_head_d, h = screw_head_h + 0.1);
            }
        }

        // lid lip that nests into the base (with fit gap), cleared around the posts
        difference() {
            translate([0, 0, -lip_h])
                difference() {
                    rrect(inner_l - 2*fit_gap, inner_w - 2*fit_gap,
                          max(0.1, corner_r - wall_t - fit_gap), lip_h);
                    rrect(inner_l - 2*fit_gap - 2*lip_t, inner_w - 2*fit_gap - 2*lip_t,
                          0.1, lip_h + 1);
                }
            for (p = post_xy())
                translate([p[0], p[1], -lip_h - 0.1])
                    cylinder(d = post_d + 1.2, h = lip_h + 0.2);
        }

        // tamper magnet pocket (blind, opens downward)
        if (mag_enable)
            translate([mag[0], mag[1], -mag_h])
                difference() {
                    cylinder(d = mag_d + 2.4, h = mag_h);
                    translate([0, 0, -0.1]) cylinder(d = mag_d, h = mag_h + 0.2);
                }
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
if (part == "base") base();
else if (part == "lid") translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();   // printable orientation
else {
    base();
    translate([0, out_w + 8, 0]) translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();
}
