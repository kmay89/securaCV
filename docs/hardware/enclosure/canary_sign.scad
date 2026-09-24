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
cs_head_d = 8.2;     // #8 82° flat-head Ø — the seat's rim at the face, so the head lands flush
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
size1 = 7.5;         // line 1 cap size — 8.0 measured 98.68 wide and touched the border groove (inner edge ±49.5)
size2 = 5.2;         // line 2 size — matches line 3
size3 = 5.2;         // line 3 size — 5.5 measured 97.79 wide, 0.55 off the groove
text_depth = 0.8;
font_b = "Liberation Sans:style=Bold";
font_r = "Liberation Sans";

/* [Quality] */
$fa = 3; $fs = 0.4;

// the countersink: an 82° cone whose rim is cs_head_d AT the face, so it starts
// cs_h below it. It started at a fixed 1.6 down (Ø6.98 at the face), which left
// an Ø8.2 head standing 0.70 proud of a seat the comment called flush
cs_h = (cs_head_d - screw_d) / (2*tan(41));        // 2.30 for #8 in a Ø4.2 hole
assert(cs_h < sign_t, "the countersink is deeper than the plate — thicken sign_t or use a smaller head");

// text vs the border groove: the lines must stay >= 1 mm inside its inner edge.
// OpenSCAD 2021 cannot measure text, so the default strings carry their
// Liberation Sans advance measured off a DXF export (width per unit size);
// a custom string gets a generous per-character estimate and is only warned about
text_room = sign_w - 11 - 2*1.0;                   // 97: groove inner width less 1 mm a side
function _tw(t, sz, meas, k) = (meas > 0 ? meas : k*len(t)) * sz;
_w1 = _tw(line1, size1, line1 == "PRIVACY WITNESS" ? 12.335 : 0, 0.84);
_w2 = _tw(line2, size2, line2 == "presence sensing in use" ? 14.495 : 0, 0.66);
_w3 = _tw(line3, size3, line3 == "no video is recorded or stored" ? 17.780 : 0, 0.66);
assert(line1 != "PRIVACY WITNESS" || _w1 <= text_room, "line 1 runs into the border groove — lower size1");
assert(line2 != "presence sensing in use" || _w2 <= text_room, "line 2 runs into the border groove — lower size2");
assert(line3 != "no video is recorded or stored" || _w3 <= text_room, "line 3 runs into the border groove — lower size3");
if (max(_w1, _w2, _w3) > text_room)
    echo(str("NOTE: a custom line may reach the border groove (estimate ", max(_w1, _w2, _w3),
             " mm vs ", text_room, " mm of room) — check the preview and lower its size"));

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
            translate([sx*(sign_w/2 - 7), sy*(sign_h/2 - 7), sign_t - cs_h])  // #8 82° flat head seats flush —
                cylinder(d1 = screw_d, d2 = cs_head_d + 2*0.1*tan(41),            // the 82° US seat, deliberately NOT
                         h = cs_h + 0.1);                                         // cs_cone90_cut's metric 90°
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
