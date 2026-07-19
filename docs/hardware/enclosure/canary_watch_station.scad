// ============================================================================
//  SecuraCV Canary Watch — monitoring-station puck  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Hardware: Seeed Round Display for XIAO (1.28" 240x240 GC9A01 touch, 39 mm
//  disc, RTC/SD/charger) with a XIAO ESP32-S3 seated in its back socket.
//  See docs/hardware/display_research.md for the selection rationale.
//
//  SenseCAP-Watcher-style desk puck: a straight DRUM holds the display disc
//  behind a screwed BEZEL (XIAO stack rides the display's own socket); the
//  drum rests in a separate 25° tilted STAND with a rear cable channel, or
//  wall-mounts via its blind keyhole pockets without the stand.
//  All parts print flat, no supports.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. Measure
//     your display disc (disc_d, stack_t) and XIAO USB position first.
// ============================================================================

/* [What to render] */
part = "all";        // ["drum","bezel","stand","all"]

/* [Display stack] — Round Display for XIAO + seated XIAO. MEASURE */
disc_d   = 39.0;     // display PCB disc diameter
disc_t   = 6.2;      // display module thickness (glass to socket face)
stack_t  = 14.0;     // full stack: glass front to XIAO back (incl. USB boot room)
usb_ang  = 270;      // XIAO USB direction, degrees (0 = +X; 270 = -Y = rear/down on the stand)
usb_w    = 10.5;
usb_h    = 6.5;

/* [Battery] — optional LiPo laid in the drum back (display has JST + charger) */
opt_batt = false;
batt_l = 30.0;  batt_w = 20.0;  batt_h = 3.4;   // 302030-class cell — MEASURE

/* [Puck] */
drum_d   = 52.0;     // drum outer diameter (posts fully clear of the 39 mm disc)
wall_t   = 2.5;
back_t   = 2.5;
bez_t    = 7.0;      // bezel thickness (holds the display seat)
bez_ap_d = 34.0;     // face aperture (1.28" active circle + margin)
seat_dep = 5.2;      // display seat depth in the bezel
tilt     = 25;       // stand angle  // [15:5:40]

/* [Print tolerances] */
tol_slide = 0.20;
tol_press = 0.10;
tol_hole  = 0.30;

/* [Fasteners + mounting] */
screw_d      = 1.6;  // M2 self-tap into the drum posts
screw_head_d = 4.0;
screw_head_h = 1.6;
kh_head_d  = 7.0;    // blind keyhole pockets in the drum back (wall mount)
kh_shank_d = 4.2;
kh_slot_l  = 7.0;
kh_head_h  = 3.0;
kh_face    = 1.0;
kh_extra   = 2.5;    // drum back thickening hosting the pockets

/* [Aesthetics] */
lid_edge  = 1.0;     // bezel face chamfer
label_text = "";
label_size = 4.0;  label_depth = 0.5;  label_font = "Liberation Sans:style=Bold";

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
cav_d   = drum_d - 2*wall_t;                 // internal bore
drum_h  = back_t + kh_extra + (stack_t - seat_dep) + 1.0;   // depth behind the bezel
seat_d  = disc_d + 2*tol_slide;
post_r  = cav_d/2 - 1.0;                     // bezel screw posts: outboard of the 39 mm disc
post_h  = drum_h - back_t - kh_extra - 1.5;  // posts stop short of the rim (the display's
                                             // back protrudes ~1 mm past the bezel seat)

assert(seat_d < cav_d - 1, "display disc too large for the drum bore — grow drum_d");
assert(bez_ap_d < disc_d - 3, "aperture must land on the display bezel ring");
assert(kh_head_h + 1.2 <= back_t + kh_extra, "keyhole pocket too deep — raise kh_extra");
assert(!opt_batt || sqrt(pow(batt_l/2,2) + pow(batt_w/2 + 2,2)) < cav_d/2, "battery too large for the drum bore");
echo(str("Canary Watch station v0.1-dev — drum ", drum_d, " x ", drum_h + bez_t,
         " mm, stand tilt ", tilt, " deg  (IN DEVELOPMENT)"));

// ----------------------------------------------------------------------------
//  DRUM — straight cup, prints open-face-up; keyhole pockets in the back
// ----------------------------------------------------------------------------
module keyhole_pocket_r(ang) {                // radial blind pocket, slot toward drum top
    rotate([0, 0, ang]) translate([0, 6, 0]) union() {
        translate([0, -kh_slot_l/2, -0.1]) linear_extrude(kh_face + 0.1) {
            circle(d = kh_head_d);
            hull() { circle(d = kh_shank_d); translate([0, kh_slot_l]) circle(d = kh_shank_d); }
        }
        translate([0, -kh_slot_l/2, kh_face]) linear_extrude(kh_head_h - kh_face)
            hull() { circle(d = kh_head_d + 0.6); translate([0, kh_slot_l]) circle(d = kh_head_d + 0.6); }
    }
}
module drum() {
    union() {
        difference() {
            cylinder(d = drum_d, h = drum_h);
            translate([0, 0, back_t + kh_extra]) cylinder(d = cav_d, h = drum_h);
            // XIAO USB slot through the wall (drum sits stack-first; slot at the rim)
            rotate([0, 0, usb_ang]) translate([drum_d/2 - wall_t/2, 0, drum_h - usb_h/2])
                cube([wall_t*3, usb_w, usb_h + 1], center = true);
            // blind keyhole pocket (single, centred) for stand-less wall mount
            keyhole_pocket_r(0);
            // bottom-edge chamfer
            difference() {
                translate([0, 0, -0.01]) cylinder(d = drum_d + 0.1, h = 0.5);
                translate([0, 0, -0.02]) cylinder(d1 = drum_d - 1.0, d2 = drum_d + 0.12, h = 0.53);
            }
        }
        // LiPo fence rails on the drum back floor (cell strapped with foam tape)
        if (opt_batt) for (s2 = [1, -1])
            translate([-batt_l/2, s2*(batt_w/2 + tol_slide + 1.0) - 1.0, back_t + kh_extra - 0.01])
                cube([batt_l, 2.0, 2.0]);
        // bezel screw posts INSIDE the bore, fused to the wall — added AFTER the
        // cavity cut so the bore cannot carve them away (review catch)
        for (a = [0, 180]) rotate([0, 0, a])
            translate([0, 0, back_t + kh_extra]) hull() {
                translate([post_r, 0, 0]) cylinder(d = 5, h = post_h);
                translate([cav_d/2 - 0.3, 0, 0]) cylinder(d = 2, h = post_h);
            }
    }
}
module drum_final() {
    difference() {
        drum();
        for (a = [0, 180]) rotate([0, 0, a])
            translate([post_r, 0, back_t + kh_extra + 1.5]) cylinder(d = screw_d, h = drum_h);
    }
}

// ----------------------------------------------------------------------------
//  BEZEL — flat ring, prints face-down; seats the display disc
// ----------------------------------------------------------------------------
module bezel() {
    difference() {
        union() {
            cylinder(d = drum_d, h = bez_t - lid_edge);
            translate([0, 0, bez_t - lid_edge - 0.01])
                cylinder(d1 = drum_d, d2 = drum_d - 2*lid_edge, h = lid_edge);
        }
        // display seat (from the back) + face aperture
        translate([0, 0, -0.1]) cylinder(d = seat_d, h = seat_dep + 0.1);
        translate([0, 0, -0.1]) cylinder(d = bez_ap_d, h = bez_t + 1);
        // aperture lead-in on the face
        translate([0, 0, bez_t - 0.5]) cylinder(d1 = bez_ap_d, d2 = bez_ap_d + 1.2, h = 0.51);
        // bezel screws into the drum posts
        for (a = [0, 180]) rotate([0, 0, a]) translate([post_r, 0, 0]) {
            translate([0, 0, -0.1]) cylinder(d = screw_d + 2*tol_hole, h = bez_t + 1);
            translate([0, 0, bez_t - screw_head_h]) cylinder(d1 = screw_d + 2*tol_hole, d2 = screw_head_d, h = screw_head_h + 0.1);
        }
        if (label_text != "")
            translate([0, -drum_d/2 + 5, bez_t - label_depth]) linear_extrude(label_depth + 1)
                text(label_text, size = label_size, font = label_font, halign = "center", valign = "center");
    }
}

// ----------------------------------------------------------------------------
//  STAND — tilted cradle pocket + rear cable channel; prints upright
// ----------------------------------------------------------------------------
module stand() {
    sw = drum_d + 12;                          // stand width
    sd = drum_d * 0.9 + 14;                    // depth
    sh = drum_d * sin(tilt) + 22;              // height
    difference() {
        // wedge body
        hull() {
            translate([-sw/2, -sd/2, 0]) cube([sw, sd, 4]);
            translate([-sw/2, sd/2 - 10, 0]) cube([sw, 10, sh]);
        }
        // tilted drum pocket (leans back against the tall rear wall).
        // rotate([90 - tilt]) points the pocket axis (0, -cos(tilt), sin(tilt)):
        // trough open up-front, glass reclined `tilt` from vertical. The
        // previous rotate([tilt + 90]) mirrored the axis in Z and carved the
        // cradle tipping the drum face-down 65° — the drum could never seat.
        // ⚠️ regenerate enclosures/preview/canary_watch_station_stand.stl.
        translate([0, 6, sh - 4]) rotate([90 - tilt, 0, 0])
            cylinder(d = drum_d + 2*tol_slide + 0.2, h = drum_d);
        // rear cable channel down the back face
        translate([0, sd/2 - 3, -0.1]) rotate([-tilt, 0, 0])
            translate([-6, -6, 0]) cube([12, 20, sh + 10]);
    }
}

// ----------------------------------------------------------------------------
if      (part == "drum")  drum_final();
else if (part == "bezel") bezel();
else if (part == "stand") stand();
else {
    drum_final();
    translate([drum_d + 12, 0, 0]) bezel();
    translate([-(drum_d + 30), 0, 0]) stand();
}
