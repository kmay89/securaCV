// ============================================================================
//  Canary — VEHICLE MOUNT KIT  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Puts any keyhole-pocket Canary (WAP weather, field case, Vision back)
//  in a vehicle cabin, on the shared two-T-stud interface:
//
//    dash — low-profile dashboard plate: VHB tape recesses underneath,
//           10 deg riser wedge so the case faces the cabin, tether slot
//    vent — air-vent louver clip: extruded spring prongs straddle one
//           horizontal louver blade; case hangs on the front-face studs
//
//  ⚠️ HEAT: a closed car exceeds +60 C — over the LiPo's discharge limit
//  and near PETG's comfort zone. For vehicle duty: NO battery (USB power),
//  print the mount in ASA/PC, use a light-colored case, and never leave a
//  battery build on a sun-baked dash (esp32s3_power_battery_guide.md).
//  Set stud_gap to YOUR case's keyhole spacing (36 = field case default).
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print- or road-validated.
//  The vent prongs print sideways by design (extruded profile: flex loads
//  stay in-plane); the studs on the vent clip therefore print sideways too
//  — fine at 04 stems, but deburr the head undersides.
// ============================================================================

use <canary_core_lib.scad>   // shared 2D primitives — rrect2d has one home now
use <canary_mount_lib.scad>  // the stud/keyhole standard this file carries around

/* [What to render] */
part = "all";        // ["dash","vent","all"]

/* [Stud interface] — match the target case's keyholes */
stud_gap  = 36.0;    // center-to-center of the two T-studs
// ecosystem-standard T-stud (canary_mount_lib, the interface's one home):
// stem 1.4 (= kh_face 1.0 + 0.4 slide) + cone 1.2 + head 0.8 = 3.4 total,
// fitting the catalog's 3.5 mm-deep blind keyhole pockets
stud_d    = 4.0;     // stem — canary_mount_lib mount_stud_d()
stud_head = 6.6;     // head disc — canary_mount_lib mount_stud_head()
stud_head_t = 0.8;   // head thickness — canary_mount_lib mount_stud_cap()
stud_stem_h = 1.4;   // stem height — canary_mount_lib mount_stud_stem()

/* [Print tolerances] */
tol_slide = 0.20;  tol_press = 0.10;

/* [Dash plate] */
dash_l = 110.0;  dash_w = 70.0;  dash_t = 4.0;
dash_tilt = 10;      // riser angle (case leans back toward the cabin)
vhb_w = 20.0;  vhb_l = 60.0;  vhb_rec = 0.6;   // 3M VHB strip registers
tether_w = 8.0;  tether_l = 4.0;

/* [Vent clip] */
vent_w   = 32.0;     // clip width (extrusion length)
face_h   = 50.0;     // front face height
face_t   = 4.0;
prong_l  = 22.0;     // reach onto the louver blade
prong_t  = 2.4;
louver_gap = 2.8;    // louver BLADE thickness + play — MEASURE your vents

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary vehicle mount v0.1-dev — stud_gap ", stud_gap,
         "  (IN DEVELOPMENT — no battery builds on a hot dash!)"));
assert(stud_gap >= stud_head + 4, "studs overlap — raise stud_gap");
assert(louver_gap >= 1.5, "louver_gap < 1.5 mm — measure your vent blade");

// T-stud from the mount library, fed this file's knobs (+Z axis, base at the
// origin — the catalog standard); the cone stays the lib's print-support 1.2
module tstud() {
    mount_tstud(d = stud_d, head = stud_head,
                stem = stud_stem_h, cap = stud_head_t);
}

// ---- dash plate (prints flat, studs up on the wedge face) --------------------
module dash() {
    riser_l = 50.0;  riser_w = 46.0;
    riser_h = riser_l * tan(dash_tilt);
    difference() {
        union() {
            linear_extrude(dash_t) rrect2d(dash_l, dash_w, 8);
            // riser wedge on the rear half
            translate([dash_l/2 - riser_l - 6, -riser_w/2, dash_t - 0.01])
                rotate([0, -dash_tilt, 0])
                    cube([riser_l / cos(dash_tilt), riser_w, 2]);
            // wedge fill below the tilted face
            translate([dash_l/2 - riser_l - 6, -riser_w/2, dash_t - 0.01]) hull() {
                cube([0.1, riser_w, 0.1]);
                translate([riser_l, 0, 0]) cube([0.1, riser_w, riser_h + 0.1]);
            }
        }
        // VHB strip registers on the underside
        for (yc = [-(dash_w/2 - 16), 0, dash_w/2 - 16])
            translate([-dash_l/2 + 8, yc - vhb_w/2, -0.1]) cube([vhb_l, vhb_w, vhb_rec + 0.1]);
        // tether slot near the front edge
        translate([-dash_l/2 + 10, 0, -0.1]) linear_extrude(dash_t + 0.2)
            rrect2d(tether_l, tether_w, 1.5);
    }
    // studs on the tilted face (gap along Y so both sit at the same height)
    for (s = [1, -1])
        translate([dash_l/2 - 6 - riser_l/2, s*stud_gap/2,
                   dash_t + (riser_l/2)*tan(dash_tilt) + 2*cos(dash_tilt) - 0.02])
            rotate([0, -dash_tilt, 0]) tstud();
}

// ---- vent clip (extruded side profile; prints on its side) -------------------
// The two prongs straddle ONE louver blade (gap = louver_gap); the upper
// prong's tip hook dips 1.0 into the gap and snaps behind the blade's rear
// edge, the lower tip up-ramp preloads the blade underside.
module vent() {
    zm = face_h/2;  g = louver_gap/2;
    pts = [
        [0, 0], [face_t, 0],
        [face_t, zm - g - prong_t],
        // lower prong: underside out, tip up-ramp, top back to the face
        [face_t + prong_l, zm - g - prong_t],
        [face_t + prong_l, zm - g - 0.4],
        [face_t + prong_l - 2.5, zm - g],
        [face_t + 0.01, zm - g],
        // inner wall between the prongs (the blade seats against the face)
        [face_t + 0.01, zm + g],
        // upper prong: underside out, hook down 1.0 into the gap, ramp up, top back
        [face_t + prong_l - 3.5, zm + g],
        [face_t + prong_l - 3.5, zm + g - 1.0],
        [face_t + prong_l - 1.5, zm + g - 1.0],
        [face_t + prong_l, zm + g + 0.8],
        [face_t + prong_l, zm + g + prong_t],
        [face_t, zm + g + prong_t],
        [face_t, face_h], [0, face_h],
    ];
    rotate([90, 0, 0]) translate([0, 0, -vent_w/2]) linear_extrude(vent_w) polygon(pts);
    // studs on the front face, vertical gap, pointing -X (sideways in print)
    for (s = [1, -1])
        translate([0.02, 0, zm + s*stud_gap/2]) rotate([0, -90, 0]) tstud();
}

// ---- selector -----------------------------------------------------------------
if      (part == "dash") dash();
else if (part == "vent") vent();
else { dash(); translate([0, dash_w/2 + 45, 0]) vent(); }
