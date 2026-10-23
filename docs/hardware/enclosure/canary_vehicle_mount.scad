// ============================================================================
//  Canary — VEHICLE MOUNT KIT  ⚠️ IN DEVELOPMENT (v0.2-dev)
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
//  ⚠️ RETENTION (dash): the dash studs pair ACROSS the 10° slope (along Y),
//  and the catalog's slots run along the pair axis, so the slide-lock is
//  horizontal — gravity does not latch it, and braking or cornering pushes
//  the case straight back along its slide. Nothing on this plate holds it:
//  use a case with the click detent in its pockets (mount_keyhole_click)
//  AND lanyard it through the tether slot. The vent clip's studs pair
//  vertically, so there the case drops on and gravity latches as on a wall.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print- or road-validated.
//  The vent prongs print sideways by design (extruded profile: flex loads
//  stay in-plane); the studs on the vent clip therefore print sideways too
//  — fine at 04 stems, but deburr the head undersides.
//
//  2026-10-23: v0.2-dev — two things the print pose and the tape got wrong:
//    * the vent clip EXPORTED STANDING: vent() stood the extruded profile up
//      in XZ (rotate 90 about X), so the face was a 50 mm tower and both
//      prongs 18.5-22 mm cantilevers off it with nothing under them — the
//      sweep read 1363 mm² of ceiling (z 21.5 and 26.1, 22.0 and 18.5 of
//      span) on 128 mm² of bed. It now lies as this header always said: the
//      profile on the bed, extruded up vent_w — 32 tall, ceilings 3 mm²
//      (the stud cone lips), the same solid turned (volume unchanged);
//    * the dash plate's THREE VHB registers were 20 wide on 19 mm centers,
//      so they ran together into one 58 x 60 recess (a 58 mm bridge 0.6 over
//      the bed) with the tether slot inside the tape. Two registers now, one
//      per long edge, the slot on the bare land between them, and a cord
//      groove from the slot out under the front edge: a taped plate stands
//      only tape - vhb_rec off the dash (0.4-0.5 on 1.0-1.1 mm VHB), so the
//      lanyard the retention note relies on could not pass under the front bar.
// ============================================================================

use <canary_core_lib.scad>   // shared 2D primitives — rrect2d has one home now
use <canary_mount_lib.scad>  // the stud/keyhole standard this file carries around

/* [What to render] */
part = "all";        // ["dash","vent","all"]

/* [Stud/keyhole interface] — T-studs matching the target case's keyholes */
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
vhb_w = 20.0;        // 3M VHB strip register width
vhb_l = 60.0;        // 3M VHB strip register length
vhb_rec = 0.6;       // 3M VHB strip register recess depth
tether_w = 8.0;  tether_l = 4.0;

/* [Vent clip] */
vent_w   = 32.0;     // clip width (extrusion length)
face_h   = 50.0;     // front face height — a minimum: grown to stud_gap + stud_head + 4 so both heads land on the face
face_t   = 4.0;
prong_l  = 22.0;     // reach onto the louver blade
prong_t  = 2.4;
louver_gap = 2.2;    // louver BLADE thickness + play — MEASURE your vents (blades run 1.5-2.0; at
                     // 2.8 a thin blade passed with no deflection and the clip hung on its hook alone)

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary vehicle mount v0.2-dev — stud_gap ", stud_gap,
         "  (IN DEVELOPMENT — no battery builds on a hot dash!)"));
assert(stud_gap >= stud_head + 4, "studs overlap — raise stud_gap");

// every stud head sits whole on the face that carries it, with 2 mm of land
// beyond its rim at each end — the riser and the vent face grow with stud_gap
// (they were a fixed 46 / 50: at stud_gap 50 the dash heads overhung the
// riser by 5.3 and the vent heads the face by 3.3, with nothing to say so)
stud_span = stud_gap + stud_head + 4;
riser_l   = 50.0;                      // riser run along the slope
riser_w   = max(46.0, stud_span);      // 46 = the original riser; wider only when the studs need it
vent_fh   = max(face_h, stud_span);    // the vent face actually drawn
assert(riser_w <= dash_w - 4, "stud_gap too wide for the dash plate — the riser would overrun dash_w");
assert(vent_fh >= stud_span && riser_w >= stud_span, "a stud head overruns its face");
if (vent_fh > face_h)
    echo(str("vent face grown to ", vent_fh, " mm to carry stud_gap ", stud_gap));
echo("NOTE: the dash plate's slide-lock is horizontal — use a click-detent case and the tether lanyard (header)");
assert(louver_gap >= 1.5, "louver_gap < 1.5 mm — measure your vent blade");

// T-stud from the mount library, fed this file's knobs (+Z axis, base at the
// origin — the catalog standard); the cone stays the lib's print-support 1.2
module tstud() {
    mount_tstud(d = stud_d, head = stud_head,
                stem = stud_stem_h, cap = stud_head_t);
}

// ---- dash plate (prints flat, studs up on the wedge face) --------------------
module dash() {
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
        // VHB strip registers on the underside: TWO, 6 mm in from each long
        // edge (v0.2: a third on the centerline overlapped both — 20 wide on
        // 19 mm centers — into one 58 x 60 recess with the tether slot in the
        // tape); the 18 mm center land between them carries the slot
        for (yc = [-(dash_w/2 - 16), dash_w/2 - 16])
            translate([-dash_l/2 + 8, yc - vhb_w/2, -0.1]) cube([vhb_l, vhb_w, vhb_rec + 0.1]);
        // tether slot near the front edge
        translate([-dash_l/2 + 10, 0, -0.1]) linear_extrude(dash_t + 0.2)
            rrect2d(tether_l, tether_w, 1.5);
        // cord groove from the slot out under the front edge (v0.2): taped
        // down, the plate stands only tape - vhb_rec off the dash (0.4-0.5 on
        // 1.0-1.1 mm VHB), so a lanyard through the slot had no way back out
        // under the front bar.
        // 6 wide = a 6 mm bridge at z 1.5, under the catalog's 7.0 ceiling
        translate([-dash_l/2 - 1, -(tether_w - 2)/2, -0.1]) cube([11, tether_w - 2, 1.6]);
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
// edge, the lower tip up-ramp preloads the blade underside. Drawn in its
// PRINT POSE (v0.2): the profile lies in XY on the bed and extrudes up +Z by
// vent_w, so the profile's "up" is +Y here and the studs point -X sideways.
// v0.1 stood the profile up in XZ and exported both prongs as cantilevers.
module vent() {
    face_h = vent_fh;                  // the grown face (the knob is its minimum)
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
    linear_extrude(vent_w) polygon(pts);
    // studs on the front face, gap along the profile's up axis (vertical in
    // use), mid-width, pointing -X (sideways in print)
    for (s = [1, -1])
        translate([0.02, zm + s*stud_gap/2, vent_w/2]) rotate([0, -90, 0]) tstud();
}

// ---- selector -----------------------------------------------------------------
if      (part == "dash") dash();
else if (part == "vent") vent();
else { dash(); translate([0, dash_w/2 + 45, 0]) vent(); }
