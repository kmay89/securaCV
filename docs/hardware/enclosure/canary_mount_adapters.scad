// ============================================================================
//  Canary — UNIVERSAL MOUNT ADAPTERS  ⚠️ IN DEVELOPMENT (v0.1-dev)
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
// ============================================================================

use <canary_core_lib.scad>   // shared 2D primitives — rrect2d has one home now
use <canary_mount_lib.scad>  // the stud/keyhole standard this file carries around

/* [What to render] */
part = "corner";     // ["corner","magnet","pole","template"]

/* [T-stud interface] */
stud_gap = 30.0;     // stud spacing — match the target case's keyhole pockets
kh_face  = 1.0;      // target pocket's face web — canary_mount_lib mount_kh_face()

/* [Plate] */
ap_w = 36.0;         // adapter plate width
ap_t = 4.0;          // plate thickness
screw_d = 4.2;       // wall screws (#8 / M4), counterbored

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
module cb_screw(x, y, t) {           // through-hole + pan-head counterbore from the front
    translate([x, y, -0.1]) cylinder(d = screw_d, h = t + 3);
    translate([x, y, t - 2.2]) cylinder(d = screw_d + 4.4, h = 3);
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
        // wall screws through each wing (two per wing)
        for (w = [0, 90]) rotate([0, 0, 45])
            for (zz = [ap_l*0.25, ap_l*0.75])
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
mag_ap_t = mag_t + 3.0;
module magnet() {
    difference() {
        linear_extrude(mag_ap_t) rrect2d(ap_w, ap_l, 5);
        for (i = [0 : mag_n - 1])
            translate([0, -ap_l/2 + 10 + i*(ap_l - 20)/(mag_n - 1), -0.1])
                cylinder(d = mag_d + 2*tol_press, h = mag_t + 0.1);
    }
    tstud( stud_gap/2, mag_ap_t - 0.01);
    tstud(-stud_gap/2, mag_ap_t - 0.01);
}

// flat plate with strap channels across the back, studs on the front
module pole() {
    difference() {
        linear_extrude(ap_t) rrect2d(ap_w, ap_l, 5);
        for (sy = [1, -1]) translate([-ap_w/2 - 1, sy*ap_l/4 - strap_w/2, -0.1])
            cube([ap_w + 2, strap_w, strap_t + 0.1]);
        cb_screw(0, 0, ap_t);        // optional center wall screw as backup
    }
    tstud( stud_gap/2, ap_t - 0.01);
    tstud(-stud_gap/2, ap_t - 0.01);
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
        translate([0, ap_l/2 + 4, 0.4]) linear_extrude(0.7) text("UP", size = 5, halign = "center");  // deboss on the FRONT face (visible in use)
    }
}

if      (part == "corner")   corner();
else if (part == "magnet")   magnet();
else if (part == "pole")     pole();
else if (part == "template") template();
