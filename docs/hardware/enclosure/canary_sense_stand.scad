// ============================================================================
//  Canary Sense — BEDSIDE STAND  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  The wellbeing/breathing channel wants the Sense unit BEDSIDE at <= 1.5 m
//  (design doc §2), but the radome case only wall/ceiling-mounts. This is a
//  weighted freestanding base with a tilted stalk ending in the standard
//  three-prong hinge head — the Sense case (or any Canary with the GoPro
//  two-prong hinge) clips on and locks with the usual M5 thumbscrew.
//
//  Ballast: the underside has pockets for 4 x M10 washers / US quarters
//  (~25 mm discs); cover with the ballast lid (glue or tape). Stick-on
//  rubber feet recommended.
//
//  2026-08-23: the teardrop bore now comes from canary_core_lib — same
//  geometry (the local copy was verbatim), one home.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

use <canary_core_lib.scad>   // tearbore_x — the teardrop bore the hinge bolt rides

/* [What to render] */
part = "all";        // ["base","ballast_lid","all"]

/* [Stand] */
base_d   = 92.0;     // base disc diameter
base_t   = 12.0;     // base thickness
stalk_h  = 70.0;     // stalk height to the hinge axis region — 70, not 55: hung plumb (radome
                     // horizontal, the bedside pose) the case's far edge swings case_reach below
                     // the axis and needs the base clear; asserted below
case_reach = 70.0;   // the Sense case's far bottom edge from its hinge axis (out_y/2 + hinge_off + ... = 69.6)
stalk_d  = 16.0;     // stalk diameter
stalk_tilt = 12;     // stalk lean (degrees, toward the front)  // [0:2:20]

/* [Ballast pockets] (coin / washer discs, glued in) */
bal_d    = 25.6;     // disc diameter (US quarter 24.26 + fit; M10 washer ~ 20-30)
bal_t    = 4.0;      // pocket depth ABOVE the lid recess — fits two stacked quarters (2 x 1.75)
                     // per pocket (~45 g total); a cantilevered case + USB cable needs the mass
bal_n    = 4;        // pocket count on a ring
bal_r    = 28.0;     // pocket ring radius

/* [Hinge head] — GoPro-compatible three prongs (mates the Canary two-prong) */
prong_t     = 3.0;
prong_pitch = 6.35;
fin_r       = 7.5;
hinge_bolt_d = 5.0;
hinge_teeth = true;
teeth_n     = 24;
teeth_h     = 0.6;

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide(), canary_core_lib
tol_hole  = 0.30;    // catalog default — core_tol_hole(), canary_core_lib

/* [Quality] */
$fa = 3; $fs = 0.4;

hinge_hole = hinge_bolt_d + 0.4;
assert(base_t - 2 + stalk_h*cos(stalk_tilt) + 7.5 >= base_t + case_reach + 2,
       "the case hung plumb hits the base — raise stalk_h (axis must clear base_t + case_reach + 2)");
assert(bal_r + bal_d/2 < base_d/2 - 3, "ballast ring exceeds the base — shrink bal_r/bal_d");
echo(str("Canary Sense bedside stand v0.1-dev — base ", base_d, " mm, head axis at ~",
         base_t - 2 + stalk_h*cos(stalk_tilt) + head_off, " mm  (IN DEVELOPMENT)"));

module teeth2d() {
    step = 360 / teeth_n;
    for (i = [0 : 2 : teeth_n - 1]) rotate([0, 0, i * step])
        intersection() {
            difference() { circle(r = fin_r - 0.5); circle(r = 3.5); }
            polygon([[0, 0], [2*fin_r, 0], [2*fin_r*cos(step), 2*fin_r*sin(step)]]);
        }
}
// three-prong head: fins ⊥ X rise from a base plate at local z=0 (which embeds
// into the stalk top); bolt axis along X at z = head_off
head_off = 10.0;
module head_fin(xc) {
    hull() {
        translate([xc - prong_t/2, -fin_r, -0.5]) cube([prong_t, 2*fin_r, 0.5]);
        translate([xc - prong_t/2, 0, head_off]) rotate([0, 90, 0]) cylinder(r = fin_r, h = prong_t);
    }
}
module hinge_head() {
    difference() {
        union() {
            head_fin(0); head_fin(-prong_pitch); head_fin(prong_pitch);
        }
        tearbore_x(-prong_pitch - prong_t, 0, head_off, 2*(prong_pitch + prong_t), hinge_hole);
        // detent tooth POCKETS in the outer fins' inner faces — the case fins
        // carry the male teeth (same female convention as the Vision bracket)
        if (hinge_teeth) {
            xi = prong_pitch - prong_t/2;
            translate([ xi - 0.05, 0, head_off]) rotate([0,  90, 0])
                linear_extrude(teeth_h + 0.15) offset(delta = 0.12) teeth2d();
            translate([-xi + 0.05, 0, head_off]) rotate([0, -90, 0])
                linear_extrude(teeth_h + 0.15) offset(delta = 0.12) teeth2d();
        }
    }
}

module base() {
    union() {
        difference() {
            // base disc with a soft top edge
            union() {
                cylinder(d = base_d, h = base_t - 2);
                translate([0, 0, base_t - 2 - 0.01]) cylinder(d1 = base_d, d2 = base_d - 6, h = 2);
            }
            // ballast pockets, from below, with a retaining rim for the lid
            // pocket depth is referenced to the RECESS floor (z = 1.21), not z = 0 —
            // otherwise the lid recess swallows the bottom 1.2 mm of every pocket
            for (i = [0 : bal_n - 1]) rotate([0, 0, i * 360 / bal_n])
                translate([bal_r, 0, -0.1]) cylinder(d = bal_d, h = bal_t + 1.41);
            translate([0, 0, -0.1]) cylinder(d = 2*bal_r + bal_d + 2, h = 1.31);  // 1.2 lid recess covers the pockets fully
        }
        // tilted stalk + hinge head (fins rise off the stalk top; bolt axis along X)
        translate([0, base_d*0.05, base_t - 2]) rotate([-stalk_tilt, 0, 0]) {
            cylinder(d = stalk_d, h = stalk_h);
            translate([0, 0, stalk_h - 0.2]) hinge_head();
        }
    }
}

module ballast_lid() {
    cylinder(d = 2*bal_r + bal_d + 2 - 2*tol_slide, h = 1.2);
}

if      (part == "base")        base();
else if (part == "ballast_lid") ballast_lid();
else { base(); translate([base_d/2 + bal_r + 12, 0, 0]) ballast_lid(); }
