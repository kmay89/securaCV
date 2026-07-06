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

/* [What to render] */
part = "all";        // ["clip","molle","all"]

/* [Stud interface] — match the target case's keyholes (36 = field case) */
stud_gap  = 36.0;
stud_d    = 4.0;  stud_head = 6.6;  stud_head_t = 0.8;
stud_stem_h = 2.6;

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
slot_w = 27.0;  slot_h = 3.5;  slot_pitch = 32.0;   // 1" PALS webbing

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary wear clip v0.1-dev — stud_gap ", stud_gap, "  (IN DEVELOPMENT)"));
assert(belt_gap >= 3.0, "belt_gap < 3 mm won't take a belt");
assert(stud_gap + stud_head + 4 <= plate_l, "studs don't fit the clip plate — grow plate_l");
assert(stud_gap + stud_head + 4 <= mp_l, "studs don't fit the MOLLE plate");

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }

module tstud() {                        // +Z axis; base at z0
    cylinder(d = stud_d, h = stud_stem_h + 0.01);
    translate([0, 0, stud_stem_h]) {
        cylinder(d1 = stud_d, d2 = stud_head, h = (stud_head - stud_d)/2);
        translate([0, 0, (stud_head - stud_d)/2]) cylinder(d = stud_head, h = stud_head_t);
    }
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
        [plate_t + belt_gap + leaf_t + 2.5, plate_l - leaf_l - 0.5],  // flared entry
        [plate_t + belt_gap + leaf_t, plate_l - leaf_l + 4],
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
    // studs up, on the vertical centreline (clear of the slot rows)
    for (s = [1, -1])
        translate([0, s*stud_gap/2, mp_t - 0.01]) tstud();
}

// ---- selector -------------------------------------------------------------------
if      (part == "clip")  clip();
else if (part == "molle") molle();
else { clip(); translate([mp_w/2 + belt_gap + 30, 0, 0]) molle(); }
