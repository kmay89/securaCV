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
//
//  2026-10-23 (v0.1-dev): FIT + PRINT REVIEW. THE CLIP PRINTS ON ITS SIDE
//  AT LAST: the profile was swung into XZ, so the clip stood 58 tall on its
//  3 x 45 plate end with the leaf hanging from the bridge — its tip started
//  8 mm over the bed on nothing (a 117 mm2 ceiling) and its flex ran across
//  the layers, the opposite of what this header promised. The profile now
//  lies on the bed (contact 135 -> ~330 mm2). STUD RISERS: with the field
//  boot fitted (its drop configuration) the boot's 2.5 floor sits between a
//  carrier plate and the case, and the 1.4 stem left the stud's head inside
//  the 1.0 face web (25.5 mm3 when slid home): no carrier could hang a
//  booted case. Each stud stands on a stud_riser pad that passes the boot's
//  floor window.
// ============================================================================

use <canary_core_lib.scad>   // shared 2D primitives — rrect2d has one home now
use <canary_mount_lib.scad>  // the stud/keyhole standard this file carries around
use <canary_snap_lib.scad>   // beam-strain arithmetic for the belt leaf spring

/* [What to render] */
part = "all";        // ["clip","molle","all"]

/* [Stud/keyhole interface] — T-studs matching the target case's keyholes (36 = field case) */
stud_gap  = 36.0;    // center-to-center of the two T-studs: set it to your case's keyhole spacing. Each plate must carry stud_gap + stud_head + 4, and the MOLLE slots must clear the heads (asserted)
// ecosystem-standard T-stud (canary_mount_lib): stem 1.4 + cone 1.2 + head
// 0.8 = 3.4 total — mount_stud_d/head/cap/stem() are the knob defaults
stud_d    = 4.0;     // stem diameter — mount_stud_d()
stud_head = 6.6;     // head disc diameter — mount_stud_head()
stud_head_t = 0.8;   // head disc thickness — mount_stud_cap()
stud_stem_h = 1.4;   // stem height — mount_stud_stem()
stud_riser = 2.4;    // pad under each stud: the field boot's floor (boot_floor 2.5) less 0.1, so a stud reaches its keyhole through the boot's floor window; 0 = studs on the bare plate
stud_riser_d = 10.0; // riser pad diameter, teardrop-pointed 45° so it prints sideways on the belt clip. It passes the boot's 24 x 16 floor window over the whole 8 mm slide

/* [Belt clip] */
leaf_w    = 45.0;    // belt-clip width (extrusion length) — the plate and the leaf are one profile
plate_l   = 58.0;    // plate height along the belt direction
plate_t   = 3.0;     // clip plate thickness — the T-studs stand on its outer face, and the belt gap and the leaf stack behind its inner face
leaf_l    = 48.0;    // leaf spring length, from the clip's top to the start of its flared entry — the grip nub sits 2 above that end, and the leaf's strain over this length is asserted under the cycle budget
leaf_t    = 2.6;     // leaf spring thickness, held to the tip (the flared entry runs parallel) — the strain assert reads it: thinner flexes easier
belt_gap  = 4.5;     // leaf-to-plate clearance (belt/waistband thickness)
nub_h     = 1.2;     // grip nub at the leaf tip

/* [MOLLE plate] */
mp_w = 55.0;         // MOLLE plate width (X) — the three slot_w webbing slots run across it
mp_l = 90.0;         // MOLLE plate length (Y) — the slots sit on slot_pitch along it and the stud pair on its center line; it must carry stud_gap + stud_head + 4 (asserted)
mp_t = 3.0;          // MOLLE plate thickness — the webbing slots go through it and the studs stand on its face
slot_w = 27.0;       // webbing slot long side — clears 1" (25.4 mm) PALS webbing
slot_h = 3.5;        // webbing slot short side
slot_pitch = 38.1;   // PALS grid: 1" webbing rows spaced 1" apart = 38.1 mm
                     // center-to-center (25.4 let only one row pass)

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary wear clip v0.1-dev — stud_gap ", stud_gap, "  (IN DEVELOPMENT)"));
assert(belt_gap >= 3.0, "belt_gap < 3 mm won't take a belt");
// a stud's footprint on its plate: the head, or the riser pad when there is one
stud_fp = max(stud_head, stud_riser > 0 ? stud_riser_d : 0);
assert(stud_gap + stud_fp + 4 <= plate_l, "studs don't fit the clip plate — grow plate_l");
assert(stud_gap + stud_fp + 4 <= mp_l, "studs don't fit the MOLLE plate");
assert(abs(stud_gap/2 - slot_pitch) > stud_fp/2 + slot_h/2 + 0.6 && stud_gap/2 > slot_h/2 + stud_fp/2 + 0.6,
       "a MOLLE slot runs under a stud head or its riser pad — move stud_gap or slot_pitch");
assert(stud_riser_d/2*sqrt(2) <= leaf_w/2 - 1.0, "the riser pad's point runs off the belt clip's plate");
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

// the stud on its riser pad (+Z axis, base at the origin). The pad's 45°
// point is local -X — DOWN on the side-printed belt clip — and the stud's
// base stands stud_riser up, so with the boot fitted the case's 1.0 face web
// rides on the stem as it does on a bare plate: 0.3 of play under the cone
// (0.4 bare); with the boot off the case rests on the pads instead
module carrier_stud() {
    if (stud_riser > 0) linear_extrude(stud_riser + 0.01) hull() {
        circle(d = stud_riser_d);
        translate([-stud_riser_d/2*sqrt(2), 0]) square(0.01, center = true);
    }
    translate([0, 0, stud_riser]) tstud();
}

// ---- belt clip (extruded profile in XY; prints on its side) ------------------
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
    // the profile lies ON THE BED, extruded up by leaf_w: every layer is the
    // whole profile, so the leaf bends in the layer plane. (Swung into XZ it
    // stood on the plate's end and the leaf's tip started 8 mm up on air.)
    linear_extrude(leaf_w) polygon(pts);
    // studs on the plate outer face, pointing -X (sideways in print)
    for (s = [1, -1])
        translate([0.02, plate_l/2 + s*stud_gap/2, leaf_w/2]) rotate([0, -90, 0]) carrier_stud();
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
        translate([0, s*stud_gap/2, mp_t - 0.01]) carrier_stud();
}

// ---- selector -------------------------------------------------------------------
if      (part == "clip")  clip();
else if (part == "molle") molle();
else { clip(); translate([mp_w/2 + belt_gap + 30, 0, 0]) molle(); }
