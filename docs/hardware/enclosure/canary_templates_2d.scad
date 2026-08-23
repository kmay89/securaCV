// ============================================================================
//  Canary — 1:1 PAPER INSTALL TEMPLATES (2D)  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Renders 2D drawings that render.sh exports as SVG — print them ON PAPER at
//  100% scale, tape to the wall, and drill/mark. No plastic template needed;
//  works when the printer isn't where the install is.
//
//    mode = "studs"    — generic keyhole/T-stud pair (set stud_gap per case)
//    mode = "bracket"  — Vision/Sense wall bracket screw + keyhole pattern
//    mode = "doorbell" — doorbell plate wall screws + cable oval + outline
//
//  IMPORTANT: print at 100% / "actual size" — the 20 mm calibration square
//  in the corner must measure exactly 20 mm.
//
//  2026-08-23: the studs-mode drill number now reads mount_kh_shank_d() from
//  canary_mount_lib — same 4.2, but the paper can no longer drift from the
//  keyhole pockets it templates.
// ============================================================================

use <canary_mount_lib.scad>  // the stud/keyhole standard the studs template drills for

/* [What to render] */
mode = "studs";      // ["studs","bracket","doorbell"]

/* [Generic studs] */
stud_gap = 30.0;     // set per case (the fit coupon's POCKET pair prints at 30)

/* [Bracket] — mirror canary_vision_enclosure bracket defaults */
br_x = 46.0;  br_y = 34.0;  br_screw_d = 4.2;

/* [Doorbell] — mirror canary_vision_doorbell plate defaults */
db_out_x = 31.8;  db_out_y = 113.2;
db_screw_inset_x = 8.0;  db_screw_inset_y = 14.0;  db_screw_d = 4.2;
db_exit_dx = 5.0;  db_exit_cy = -20.8;  db_exit_w = 12.0;  db_exit_h = 7.0;

line = 0.5;          // drawn line weight

module ring(d) { difference() { circle(d = d + 2*line); circle(d = d); } }
module cross(l) { square([l, line], center = true); square([line, l], center = true); }
module cal() {   // 20 mm calibration square — MEASURE THIS after printing
    difference() { square(20, center = true); square(20 - 2*line, center = true); }
    text("20mm", size = 3, halign = "center", valign = "center");
}
module outline2d(l, w, r) {
    difference() { offset(r = r) offset(r = -r) square([l, w], center = true);
                   offset(r = r - line) offset(r = -r) square([l - 2*line, w - 2*line], center = true); }
}

module studs2d() {
    // the drill ring IS the mount interface: a wall screw whose shank must
    // ride the keyhole slot — mount_kh_shank_d() (4.2), so this paper and
    // the pockets can only ever change together
    for (s = [1, -1]) translate([0, s*stud_gap/2]) { ring(mount_kh_shank_d()); cross(9); }
    translate([0, 0]) cross(6);
    translate([26, stud_gap/2 + 14]) cal();
    translate([0, -stud_gap/2 - 12]) text(str("stud gap ", stud_gap, " — drill ",
                                              mount_kh_shank_d(), " + anchors"),
                                          size = 4, halign = "center");
}

module bracket2d() {
    outline2d(br_x, br_y, 3);
    for (sx = [1, -1], sy = [1, -1]) translate([sx*(br_x/2 - 5), sy*(br_y/2 - 5)]) { ring(br_screw_d); cross(8); }
    for (sx = [1, -1]) translate([sx*14, -3]) ring(7.5);   // the bracket's own through-plate keyhole
                                                           // (7.5) — not the blind-pocket standard
    translate([0, 0]) cross(6);
    translate([br_x/2 + 18, br_y/2 + 6]) cal();
    translate([0, -br_y/2 - 8]) text("Vision/Sense bracket — 4x #8 screws", size = 4, halign = "center");
}

module doorbell2d() {
    outline2d(db_out_x, db_out_y, 12);
    for (sy = [1, -1], sx = [1, -1])
        translate([sx*(db_out_x/2 - db_screw_inset_x), sy*(db_out_y/2 - db_screw_inset_y)])
            { ring(db_screw_d); cross(8); }
    // cable exit oval
    translate([db_exit_dx, db_exit_cy]) difference() {
        hull() for (s = [1, -1]) translate([s*(db_exit_w - db_exit_h)/2, 0]) circle(d = db_exit_h + 2*line);
        hull() for (s = [1, -1]) translate([s*(db_exit_w - db_exit_h)/2, 0]) circle(d = db_exit_h);
    }
    translate([0, 0]) cross(6);
    translate([db_out_x/2 + 18, db_out_y/2 - 4]) cal();
    translate([0, -db_out_y/2 - 8]) text("doorbell plate — UP is +Y", size = 4, halign = "center");
}

if      (mode == "studs")    studs2d();
else if (mode == "bracket")  bracket2d();
else if (mode == "doorbell") doorbell2d();
