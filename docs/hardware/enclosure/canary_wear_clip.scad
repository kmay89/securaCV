// ============================================================================
//  Canary — BODY-WORN CARRY CLIPS  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Wearable carry for any keyhole-pocket Canary — designed around the FIELD
//  CASE (its floor keyholes are 36 mm apart) but stud_gap is parametric:
//
//    clip  — belt clip: extruded side profile (plate + leaf spring joined by
//            a rounded bridge, 4.5 mm belt gap, flared entry + grip nub).
//            Prints on its side so the leaf's flex stays in-plane — the
//            strong direction. Studs therefore print sideways (fine at 04).
//    molle — PALS/MOLLE adapter: flat plate, weaves through vest webbing
//            (three 27 mm slots), studs print upright.
//
//  Wearing a witness device has social and legal weight — pair with the
//  signage plate where required and check local recording law.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print- or wear-validated.
// ============================================================================

use <canary_core_lib.scad>   // shared 2D primitives — rrect2d has one home now
use <canary_mount_lib.scad>  // the stud/keyhole standard this file carries around
use <canary_snap_lib.scad>   // beam-strain arithmetic for the belt leaf spring

/* [What to render] */
part = "all";        // ["clip","molle","all"]

/* [Stud interface] — match the target case's keyholes (36 = field case) */
stud_gap  = 36.0;
// ecosystem-standard T-stud (canary_mount_lib): stem 1.4 + cone 1.2 + head
// 0.8 = 3.4 total — mount_stud_d/head/cap/stem() are the knob defaults
stud_d    = 4.0;  stud_head = 6.6;  stud_head_t = 0.8;
stud_stem_h = 1.4;

/* [Belt clip] */
clip_w    = 45.0;    // width (extrusion length)
plate_l   = 58.0;    // plate height along the belt direction
plate_t   = 3.0;
leaf_l    = 48.0;
leaf_t    = 2.6;
belt_gap  = 4.5;     // leaf-to-plate clearance (belt/waistband thickness)
nub_h     = 1.2;     // grip nub at the leaf tip

/* [MOLLE plate] */
mp_w = 55.0;  mp_l = 90.0;  mp_t = 3.0;
slot_w = 27.0;  slot_h = 3.5;  slot_pitch = 38.1;   // PALS grid: 1" webbing rows spaced 1" apart = 38.1 mm
                                                     // center-to-center (25.4 let only one row pass)

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary wear clip v0.1-dev — stud_gap ", stud_gap, "  (IN DEVELOPMENT)"));
assert(belt_gap >= 3.0, "belt_gap < 3 mm won't take a belt");
assert(stud_gap + stud_head + 4 <= plate_l, "studs don't fit the clip plate — grow plate_l");
assert(stud_gap + stud_head + 4 <= mp_l, "studs don't fit the MOLLE plate");
assert(abs(stud_gap/2 - slot_pitch) > stud_head/2 + slot_h/2 + 0.6 && stud_gap/2 > slot_h/2 + stud_head/2 + 0.6,
       "a MOLLE slot runs under a stud head — move stud_gap or slot_pitch");
// The belt leaf is a designed spring on repeated duty (every clip-on cams the
// grip nub over the belt), so it documents itself against the cycle budget.
// Printed on its side the flex stays in-plane with the layers — roughly twice
// the cross-layer strength the budget assumes — so a pass here carries that
// whole factor as margin.
assert(snap_strain(leaf_t, nub_h, leaf_l) <= snap_budget_cycle(),
       str("belt leaf strain ", round(snap_strain(leaf_t, nub_h, leaf_l)*1000)/10,
           " % exceeds the cycle budget — thin the leaf, shrink the nub, ",
           "or lengthen the leaf"));

// T-stud from the mount library, fed this file's knobs (+Z axis, base at the
// origin — the catalog standard); the cone stays the lib's print-support 1.2
module tstud() {
    mount_tstud(d = stud_d, head = stud_head,
                stem = stud_stem_h, cap = stud_head_t);
}

// ---- belt clip (extruded profile in XZ; prints on its side) ------------------
module clip() {
    bridge_r = plate_t + belt_gap + leaf_t;   // rounded top bridge
    pts = [
        // plate outer face (studs side) at x = 0, top at z = plate_l
        [0, 0], [plate_t, 0],
        // inner face up to where the bridge starts
        [plate_t, plate_l - 6],
        // bridge inner curve approximated by the polygon top
        [plate_t + belt_gap, plate_l - 6],
        // leaf inner face down, ending at the grip nub
        [plate_t + belt_gap, plate_l - leaf_l + 4],
        [plate_t + belt_gap - nub_h, plate_l - leaf_l + 2],     // grip nub
        [plate_t + belt_gap, plate_l - leaf_l],
        // flared entry as a PARALLEL path so the leaf keeps leaf_t thickness
        // to the tip (a wedge here made the spring rigid where it must flex)
        [plate_t + belt_gap + 2.5, plate_l - leaf_l - 2],
        [plate_t + belt_gap + leaf_t + 2.5, plate_l - leaf_l - 2],
        [plate_t + belt_gap + leaf_t, plate_l - leaf_l],
        // leaf outer face back up to the bridge, then the chamfered bridge top
        [bridge_r, plate_l - 6],
        [bridge_r, plate_l - 2], [bridge_r - 2, plate_l], [2, plate_l], [0, plate_l - 2],
    ];
    rotate([90, 0, 0]) translate([0, 0, -clip_w/2]) linear_extrude(clip_w) polygon(pts);
    // studs on the plate outer face, pointing -X (sideways in print)
    for (s = [1, -1])
        translate([0.02, 0, plate_l/2 + s*stud_gap/2]) rotate([0, -90, 0]) tstud();
}

// ---- MOLLE plate (prints flat, studs up) --------------------------------------
module molle() {
    difference() {
        linear_extrude(mp_t) rrect2d(mp_w, mp_l, 6);
        // webbing slots (weave through 2 PALS rows)
        for (yc = [-slot_pitch, 0, slot_pitch])
            translate([0, yc, -0.1]) linear_extrude(mp_t + 0.2)
                rrect2d(slot_w, slot_h, 1.5);
    }
    // studs up, on the vertical centerline (clear of the slot rows)
    for (s = [1, -1])
        translate([0, s*stud_gap/2, mp_t - 0.01]) tstud();
}

// ---- selector -------------------------------------------------------------------
if      (part == "clip")  clip();
else if (part == "molle") molle();
else { clip(); translate([mp_w/2 + belt_gap + 30, 0, 0]) molle(); }
