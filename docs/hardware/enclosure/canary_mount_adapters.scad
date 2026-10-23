// ============================================================================
//  Canary — UNIVERSAL MOUNT ADAPTERS  ⚠️ IN DEVELOPMENT (v0.2-dev)
//  The T-stud/keyhole interface is the de-facto Canary mounting standard:
//  every case with blind keyhole pockets hangs on two Ø4 studs with Ø6.6
//  heads. These adapters carry that interface into three more scenarios:
//    part = "corner"   — 90° inside-corner wedge (two 45° wall faces)
//    part = "magnet"   — magnet plate (4 x Ø20x3 pockets + steel-surface note)
//    part = "pole"     — strap plate (hose-clamp/zip-tie channels)
//    part = "template" — 1 mm drill/hang template (stud + cable marks)
//
//  Set stud_gap to the target case's pocket spacing (see the README table:
//  Vision/Sense = 2*(inner/2 - kh_inset); WAP = 2*kh_x, preset-dependent).
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//
//  2026-10-23: v0.2-dev — the wall screws, the magnet pockets, the label:
//    * the wall-screw holes were Ø4.2 for a #8 (4.17 major): 0.0075 a side
//      at the 33-gon's flats, before the hole prints small — the screw
//      threads the adapter and jacks it off the wall instead of clamping it
//      (the Vision bracket's AUD-5 hole). Ø4.8 now, the #8 + 2 x tol_hole;
//    * the pole plate's pan-head seat was 2.2 deep: an ISO 7045 M4 head
//      (k 3.1) stood 0.9 proud of the stud face, and a hung case's face web
//      rides onto the stud cone from 0.5 of stand-off — the case could not
//      reach its parked position with an M4 pan head in the seat (probed: a
//      standard pocket parked on the plate is EMPTY to 0.45 of lift; with
//      that head seated it met it at every lift). 3.5 now, the registry's pan
//      rule (k + 0.4, flush, never proud); the plate grows 5.6 -> 6.9;
//    * the magnet plate's two back-open Ø20.2 pockets printed as flat 20 mm
//      bridges 3 mm over the bed (the catalog's printed ceiling is 7.0). It
//      now prints on its long edge (the selector) with a 45° teardrop crown
//      on each pocket, so every pocket roof self-supports;
//    * the template's "UP" ran 0.78 past the top edge and lost its tops.
// ============================================================================

use <canary_core_lib.scad>   // shared 2D primitives — rrect2d has one home now
use <canary_mount_lib.scad>  // the stud/keyhole standard this file carries around

/* [What to render] */
part = "corner";     // ["corner","magnet","pole","template"]

/* [Stud/keyhole interface] — the adapter's T-studs */
stud_gap = 30.0;     // stud spacing — match the target case's keyhole pockets
kh_face  = 1.0;      // target pocket's face web — canary_mount_lib mount_kh_face()

/* [Plate] */
ap_w = 36.0;         // adapter plate width
ap_t = 4.0;          // plate thickness
screw_d = 4.8;       // wall screw clearance (#8 / M4): the #8's 4.17 major + 2 x tol_hole (4.2 let the screw thread the adapter), counterbored

/* [Magnet plate] */
mag_d = 20.0;        // magnet pocket diameter (Ø20 x 3 neodymium discs)
mag_t = 3.0;
mag_n = 2;           // two Ø20 pockets fit the 56 mm plate (four overlapped into one
                     // slot); glue the magnets in — the open backs alone don't retain them

/* [Pole plate] */
strap_w = 9.0;
strap_t = 2.2;

/* [Template] */
cable_oval = true;   // mark the doorbell/relay-style cable oval too

/* [Print tolerances] */
tol_slide = 0.20;
tol_press = 0.10;
tol_hole  = 0.30;

/* [Quality] */
$fa = 3; $fs = 0.4;

ap_l = stud_gap + 26;
echo(str("Canary mount adapters v0.1-dev — ", part, ", stud_gap ", stud_gap, "  (IN DEVELOPMENT)"));

// T-stud from the mount library; this file's placement signature survives as
// a wrapper. The stem tracks the kh_face knob (stem = face web + 0.4 slide
// room) so a nonstandard pocket still gets a matching stud.
module tstud(yc, zbase) {
    translate([0, yc, zbase]) mount_tstud(stem = kh_face + 0.4);
}
// pan-head counterbore depth: an ISO 7045 M4 head (k 3.1) + the registry's
// 0.4 sink — flush, never proud. v0.1's 2.2 stood that head 0.9 above the
// face the hung case sits on, past the 0.45 a parked pocket tolerates
cb_h = 3.5;
module cb_screw(x, y, t) {           // through-hole + pan-head counterbore from the front
    translate([x, y, -0.1]) cylinder(d = screw_d, h = t + 3);
    // h tracks cb_h: a fixed 3 capped the 3.5 seat with a 0.5 skin
    translate([x, y, t - cb_h]) cylinder(d = screw_d + 4.4, h = cb_h + 1);
}

// 90° inside-corner wedge: two 45° wall wings, studs on the outward face
module corner() {
    difference() {
        union() {
            // prism body: right-isosceles cross-section, studs face the hypotenuse
            rotate([0, 0, 45]) linear_extrude(ap_l, center = false)
                polygon([[0, 0], [ap_w*0.9, 0], [0, ap_w*0.9]]);
            // stud face pad ON the hypotenuse plane (y = hyp after the 45°
            // rotation — the old double-rotate composed to 90° and buried the
            // pad inside the prism with the studs at the inside-corner vertex)
            translate([-ap_w/2, ap_w*0.9/sqrt(2) - 1.0, 0]) cube([ap_w, 1.01, ap_l]);
        }
        // wall screws through each wing — ONE per wing, mid-height between the
        // studs: at 0.25/0.75 of ap_l the Ø8.6 counterbores exited the hypotenuse
        // under the stud heads and left each stud on a hollowed base
        for (w = [0, 90]) rotate([0, 0, 45])
            for (zz = [ap_l/2])
                if (w == 0)
                    translate([ap_w*0.55, 3.5, zz]) rotate([90, 0, 0])
                        { cylinder(d = screw_d, h = 8); translate([0,0,-14]) cylinder(d = screw_d + 4.4, h = 14.6); }
                else
                    translate([3.5, ap_w*0.55, zz]) rotate([0, -90, 0])
                        { cylinder(d = screw_d, h = 8); translate([0,0,-14]) cylinder(d = screw_d + 4.4, h = 14.6); }
    }
    // studs OUT of the hypotenuse face, spaced across it (stud_gap apart, so a
    // case's keyhole pair drops straight on)
    translate([0, ap_w*0.9/sqrt(2) - 0.1, ap_l/2]) rotate([-90, 0, 0])
        { tstud( stud_gap/2, 0); tstud(-stud_gap/2, 0); }
}

// flat plate with magnet pockets on the back, studs on the front.
// Thicker than the other plates: the stud bases sit over the pockets, so a
// >= 3 mm floor is kept between pocket bottom and stud (review catch).
// PRINTS ON ITS LONG EDGE (v0.2, the selector turns it): back-down, each
// back-open pocket's flat Ø20.2 roof was a 20 mm bridge 3 mm over the bed.
// On edge the pockets stand as vertical circles, each with a 45° teardrop
// crown toward +X (up in print) so the roof self-supports; the crown is a
// void on the steel side, the disc still locates on the circle, and the
// studs print sideways like the corner wedge's.
mag_ap_t = mag_t + 3.0;
module magnet() {
    mr = mag_d/2 + tol_press;        // pocket radius
    difference() {
        linear_extrude(mag_ap_t) rrect2d(ap_w, ap_l, 5);
        // pockets inboard of the plate end by 2 mm of wall (at ±18 a Ø20 pocket
        // breached a 56 mm plate's end); mag_n = 1 would divide by zero
        assert(mag_n >= 2, "mag_n: two pockets minimum (one divides the spacing by zero)");
        for (i = [0 : mag_n - 1])
            translate([0, -(ap_l/2 - mag_d/2 - 2) + i*(ap_l - mag_d - 4)/(mag_n - 1), -0.1])
                linear_extrude(mag_t + 0.1) union() {
                    circle(r = mr);
                    polygon([[mr*cos(45), mr*sin(45)], [mr*sqrt(2), 0], [mr*cos(45), -mr*sin(45)]]);
                }
    }
    tstud( stud_gap/2, mag_ap_t - 0.01);
    tstud(-stud_gap/2, mag_ap_t - 0.01);
}

// flat plate with strap channels across the back, studs on the front.
// Thicker than ap_t: the center screw's counterbore sits right over the strap
// channel, and at 4.0 its floor (1.8) was below the channel roof (2.2) — the
// Ø8.6 bore broke into the channel and the head fell through. The plate is
// channel + a 1.2 seat floor + the counterbore, so the head bears on plastic.
// The screw is a backup: it passes through the channel, so it is used with a
// strap that straddles it (two zip ties) or instead of the strap
pole_t = max(ap_t, strap_t + 1.2 + cb_h);         // 6.9 at the defaults (5.6 before the v0.2 seat)
assert(pole_t - cb_h - strap_t >= 1.0,
       "pole plate: the center screw's counterbore breaks into the strap channel — raise ap_t");
module pole() {
    difference() {
        linear_extrude(pole_t) rrect2d(ap_w, ap_l, 5);
        // ONE strap channel, centered between the studs: at ±ap_l/4 the channels
        // ran directly under the stud heads and left 1.8 mm of floor in the load path
        translate([-ap_w/2 - 1, -strap_w/2, -0.1])
            cube([ap_w + 2, strap_w, strap_t + 0.1]);
        cb_screw(0, 0, pole_t);      // optional center wall screw as backup
    }
    tstud( stud_gap/2, pole_t - 0.01);
    tstud(-stud_gap/2, pole_t - 0.01);
}

// 1 mm drill/hang template: hold to the wall, mark through the holes
module template() {
    difference() {
        linear_extrude(1.0) rrect2d(ap_w, ap_l + 16, 4);
        for (s = [1, -1]) translate([0, s*stud_gap/2, -0.1]) cylinder(d = 4.4, h = 1.2);
        translate([0, 0, -0.1]) cylinder(d = 2.5, h = 1.2);   // center mark
        if (cable_oval) translate([5, 6, -0.1]) hull()
            for (s = [1, -1]) translate([s*2.5, 0, 0]) cylinder(d = 7, h = 1.2);
        // level line slots
        for (s = [1, -1]) translate([s*(ap_w/2 - 5), 0, -0.1])
            linear_extrude(1.2) rrect2d(6, 1.4, 0.6);
        // deboss on the FRONT face (visible in use). The baseline sits at
        // ap_l/2 + 2, not + 4: at + 4 the 5 mm letters ran 0.78 past the top
        // edge and lost their tops (v0.2) — now 1.2 inside it
        translate([0, ap_l/2 + 2, 0.4]) linear_extrude(0.7) text("UP", size = 5, halign = "center");
    }
}

if      (part == "corner")   corner();
else if (part == "magnet")   translate([0, 0, ap_w/2]) rotate([0, -90, 0]) magnet();  // on its long edge (v0.2)
else if (part == "pole")     pole();
else if (part == "template") template();
