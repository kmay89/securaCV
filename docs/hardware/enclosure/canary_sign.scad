// ============================================================================
//  Canary — WITNESS SIGNAGE PLATE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  SecuraCV is a privacy witness: it senses without storing raw video. Where
//  notice is appropriate (or legally required), this plate says so plainly —
//  debossed text prints crisply face-down, or pause at the text layer for a
//  contrast filament swap.
//
//  2026-08-23: rrect2d now comes from canary_core_lib — same geometry, one
//  home. The corner seats stay local: they are the 82° US flat-head cone,
//  not the lib's 90° cs_cone90_cut.
//
//  ⚠️ DEV STATUS: render/mesh-verified only. Check your local signage rules.
// ============================================================================

use <canary_core_lib.scad>   // rrect2d — the catalog's shared helpers
use <canary_rib_lib.scad>    // plate_ribs — the optional back section

/* [What to render] */
part = "sign";       // ["sign"]

/* [Plate] */
sign_w = 110.0;
sign_h = 70.0;
sign_t = 3.0;
edge_ch = 1.0;
screw_d = 4.2;       // countersunk corners; or use VHB and set screws=false
screws  = true;
back_ribs = false;   // rib_lib loop + two cross ribs on the BACK (rib_h tall): a 110 x 70 x 3 sheet stays
                     // flat and the screws pull on a frame, not a plate. The sign then exports FACE-DOWN
                     // (the text becomes first-layer voids — the catalog's crisp deboss). Leave OFF for a
                     // VHB mount, which wants the flat back
rib_h   = 2.0;       // back rib height  // [1:0.5:6]

/* [Text] — up to three lines */
line1 = "PRIVACY WITNESS";
line2 = "presence sensing in use";
line3 = "no video is recorded or stored";
size1 = 8.0;  size2 = 5.5;  size3 = 5.5;
text_depth = 0.8;
font_b = "Liberation Sans:style=Bold";
font_r = "Liberation Sans";

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary witness sign v0.1-dev — ", sign_w, " x ", sign_h, " mm",
         back_ribs ? " (back ribs — exports face-down)" : "", "  (IN DEVELOPMENT)"));

module sign() {
    difference() {
        union() {
            linear_extrude(sign_t - edge_ch) rrect2d(sign_w, sign_h, 6);
            translate([0, 0, sign_t - edge_ch - 0.01]) hull() {
                linear_extrude(0.01) rrect2d(sign_w, sign_h, 6);
                translate([0, 0, edge_ch]) linear_extrude(0.01)
                    rrect2d(sign_w - 2*edge_ch, sign_h - 2*edge_ch, 5);
            }
        }
        // border groove
        translate([0, 0, sign_t - 0.5]) linear_extrude(0.6) difference() {
            rrect2d(sign_w - 8, sign_h - 8, 4);
            rrect2d(sign_w - 11, sign_h - 11, 3);
        }
        translate([0, 14, sign_t - text_depth]) linear_extrude(text_depth + 0.1)
            text(line1, size = size1, font = font_b, halign = "center", valign = "center");
        translate([0, 0, sign_t - text_depth]) linear_extrude(text_depth + 0.1)
            text(line2, size = size2, font = font_r, halign = "center", valign = "center");
        translate([0, -12, sign_t - text_depth]) linear_extrude(text_depth + 0.1)
            text(line3, size = size3, font = font_r, halign = "center", valign = "center");
        if (screws) for (sx = [1, -1], sy = [1, -1]) {
            translate([sx*(sign_w/2 - 7), sy*(sign_h/2 - 7), -rib_h - 1]) cylinder(d = screw_d, h = sign_t + rib_h + 2);
            translate([sx*(sign_w/2 - 7), sy*(sign_h/2 - 7), sign_t - 1.6])
                cylinder(d1 = screw_d, d2 = screw_d + 2*2.3*tan(41), h = 2.3);   // #8 82° flat head (Ø8.2) seats flush —
                                                                       // the 82° US seat, deliberately NOT
                                                                       // cs_cone90_cut's metric 90°
        }
    }
}

// the back section: a loop inboard of the screw holes (inset 12 clears the
// Ø4.2 at 7 from the edge by 2.8) and two cross ribs. Screws pull the plate
// onto the loop, so the loop is the bearing face — no rocking on a bad wall
module sign_back_ribs() {
    mirror([0, 0, 1]) plate_ribs(sign_w, sign_h, sign_t, rib_h, r = 6, inset = 12, n = 2);
}
module sign_part() { sign(); if (back_ribs) sign_back_ribs(); }
if (back_ribs) translate([0, 0, sign_t]) rotate([180, 0, 0]) sign_part();   // face-down: ribs print as upstands
else sign_part();
