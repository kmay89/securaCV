// ============================================================================
//  Canary Sense — BEDSIDE STAND  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  The wellbeing/breathing channel wants the Sense unit BEDSIDE at <= 1.5 m
//  (design doc §2), but the radome case only wall/ceiling-mounts. This is a
//  weighted freestanding base with a tilted stalk ending in the standard
//  three-prong hinge head — the Sense case (or any Canary with the GoPro
//  two-prong hinge) clips on and locks with the usual M5 thumbscrew.
//
//  POSE: the case stands UP on the head — radome level and facing the bed is
//  the bedside pose — and tips forward from there down to 15° below
//  horizontal. It does NOT hang plumb below the head: a GoPro joint's center
//  fin points down the stalk, and the case's hinge root collides with it
//  (plumb measures 2,445 mm³ of case inside the head and stalk, the joint
//  itself aside). Re-measured 2026-09-26 against the piston-plate shell
//  (canary_sense_front.stl — the fins are on the shell now, teardrop roots)
//  hung on the head's bolt axis by sense_hinge_axis_stl() and swept in 5°
//  steps with the joint (fin_r + 0.4 round the axis) excluded: zero overlap
//  from 45° past horizontal toward the back (the sweep's end), through
//  upright, to 20° below horizontal toward the front; 22° hits (15 mm³),
//  25° hits (57 mm³). pose_low = 15 keeps 5° in hand.
//
//  Ballast: the underside has pockets for 4 x M10 washers / US quarters
//  (~25 mm discs); cover with the ballast lid (glue or tape). Stick-on
//  rubber feet recommended.
//
//  2026-08-23: the teardrop bore now comes from canary_core_lib — same
//  geometry (the local copy was verbatim), one home.
//  2026-09-26: case_reach / case_face are read off canary_sense_enclosure.scad
//  (sense_hinge_reach()/_face(), through `use <>`) — the knobs here had the
//  reach 1.6 short of the ported shell (71.2 for 72.8); the pose sweep above
//  re-run against that shell.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

use <canary_core_lib.scad>   // tearbore_x — the teardrop bore the hinge bolt rides
use <canary_sense_enclosure.scad>   // sense_hinge_reach()/_face()/_back() — the case's reach from its hinge axis, read live

/* [What to render] */
part = "all";        // ["base","ballast_lid","all"]

/* [Stand] */
base_d   = 92.0;     // base disc diameter
base_t   = 12.0;     // base thickness
stalk_h  = 70.0;     // stalk height to the hinge axis region — puts the standing radome ~130 mm over the nightstand
pose_low   = 15;     // the forward tip limit below horizontal — where the case root meets the center fin (header)
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
// the case's reach from its hinge axis — read off canary_sense_enclosure.scad's own
// derivation (out_y + hinge_off to the far wall; base_d + lid_t - fin_r to the radome
// face), never retyped here: a case that grows moves this stand's pose limits with it
case_reach = sense_hinge_reach();
case_face  = sense_hinge_face();
head_off = 10.0;                     // bolt axis above the head's base plate
// the hinge axis as base() places it: the stalk top plus head_off, both along the tilted stalk
axis_z = base_t - 2 + (stalk_h - 0.2 + head_off)*cos(stalk_tilt);
// the same point, for a probe that hangs the case on it (the pose sweep in the header)
function sense_stand_axis() = [0, base_d*0.05 + (stalk_h - 0.2 + head_off)*sin(stalk_tilt), axis_z];
function sense_stand_head_x() = prong_pitch + prong_t;   // the head's half-width along the bolt
// the lowest free pose (tipped pose_low below horizontal, radome down-forward) keeps the
// case's leading corner 2 mm over the base; the plumb pose is not free, so not asserted
assert(axis_z - case_reach*sin(pose_low) - case_face*cos(pose_low) >= base_t + 2,
       "the case tipped fully forward hits the base — raise stalk_h");
assert(bal_r + bal_d/2 < base_d/2 - 3, "ballast ring exceeds the base — shrink bal_r/bal_d");
echo(str("Canary Sense bedside stand v0.1-dev — base ", base_d, " mm, head axis at ",
         axis_z, " mm; case stands up, tips to ", pose_low, "° below level  (IN DEVELOPMENT)"));

module teeth2d() {
    step = 360 / teeth_n;
    for (i = [0 : 2 : teeth_n - 1]) rotate([0, 0, i * step])
        intersection() {
            difference() { circle(r = fin_r - 0.5); circle(r = 3.5); }
            polygon([[0, 0], [2*fin_r, 0], [2*fin_r*cos(step), 2*fin_r*sin(step)]]);
        }
}
// three-prong head: fins ⊥ X rise from a base plate at local z=0 (which embeds
// into the stalk top); bolt axis along X at z = head_off (defined with the knobs' derived values)
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
