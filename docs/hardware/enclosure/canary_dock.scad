// ============================================================================
//  Canary — FLEET PROVISIONING DOCK  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  The v1 validation runbook flashes and validates fleets of boards. This
//  dock holds N bare XIAOs reclined side by side next to a USB hub, USB-C
//  ports up, each bay NUMBERED — flash, provision, verify, deploy, in order
//  (bay number ↔ device identity in your notes). Parametric bay count.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//
//  v0.1-dev (2026-08-23): canary_*_lib adoption (dedup, mesh-identical). The
//  17.8 lesson this file taught now lives in the board registry, cited below.
// ============================================================================

use <canary_core_lib.scad>   // rrect2d — the catalog's shared helpers
use <canary_board_lib.scad>  // board registry — where this file's 17.8 lesson lives now

/* [What to render] */
part = "dock";       // ["dock"]

/* [Bays] */
n_bays = 4;          // number of XIAO bays  // [2:1:10]
recline = 15;        // bay recline angle (degrees)

/* [Board] — XIAO (any variant); defaults measured from the committed vendor
   GLB (canary-local/boards/seeed_xiao_esp32s3_sense.glb): board 21 x 17.8 mm,
   bare thickness 4.4 incl the USB shell; a Sense with its camera folded flat
   stacks to 13.7 — pick the variant to size the pocket. */
variant = "bare";    // ["bare","sense"]
board_l = 21.0;      // board length (USB edge at the TOP of the bay) — brd_l("xiao")
board_w = 17.8;      // measured — brd_xiao_w_measured(), canary_board_lib (was 17.5:
                     // spec-sized pockets pinched the real board; this file is where
                     // that lesson was learned, and the registry is where it lives)
pocket_bare  = 7.0;  // pocket depth: bare XIAO (4.4 measured + finger room)
pocket_sense = 14.5; // pocket depth: Sense w/ camera folded on top (13.7 measured)
thumb_d = 12.0;      // thumb cutout for extraction

/* [Block] */
bay_pitch = 24.0;    // bay-to-bay spacing
base_d = 34.0;       // block depth (front-back)
label_depth = 0.6;
label_font = "Liberation Sans:style=Bold";
tol_slide = 0.20;

/* [Quality] */
$fa = 3; $fs = 0.4;

pocket_d = variant == "sense" ? pocket_sense : pocket_bare;
bw = n_bays * bay_pitch + 10;
bh = board_l * cos(recline) + 14;
echo(str("Canary provisioning dock v0.1-dev — ", n_bays, " bays, ", bw, " mm wide  (IN DEVELOPMENT)"));

// (rrect2d comes from canary_core_lib — the local copy is gone)

module dock() {
    difference() {
        // wedge block: tall back, low front (bays recline against it)
        hull() {
            translate([-bw/2, -base_d/2, 0]) cube([bw, base_d, 4]);
            translate([-bw/2, base_d/2 - 8, 0]) cube([bw, 8, bh]);
        }
        // reclined bays
        for (i = [0 : n_bays - 1]) {
            bx = -bw/2 + 5 + bay_pitch/2 + i*bay_pitch;
            translate([bx, base_d/2 - 4, bh - 2]) rotate([recline + 90, 0, 0]) {
                // board pocket (open at the top for the USB end)
                translate([0, 0, -1]) linear_extrude(board_l + 4)
                    rrect2d(board_w + 2*tol_slide, pocket_d, 1.5);
                // thumb cutout at the pocket front
                translate([0, -pocket_d/2, board_l*0.35]) rotate([90, 0, 0])
                    translate([0, 0, -6]) cylinder(d = thumb_d, h = 12);
            }
            // bay number, debossed into the RECLINED front face — the face is a
            // slope from (y=-base_d/2, z=4) to (y=base_d/2-8, z=bh), so the cut
            // must sit on that plane (a vertical cut at the base floated ~7 mm
            // in front of the surface and printed docks had no numbers)
            translate([bx,
                       -base_d/2 + (bh*0.35 - 4)*(base_d - 8)/(bh - 4),
                       bh*0.35])
                rotate([atan2(bh - 4, base_d - 8), 0, 0])
                    translate([0, 0, -label_depth]) linear_extrude(label_depth + 1)
                        text(str(i + 1), size = 7, font = label_font, halign = "center", valign = "center");
        }
        // cable channel along the back base (route to the hub)
        translate([0, base_d/2 - 3, -0.1]) linear_extrude(6) rrect2d(bw - 16, 5, 2);
        // rubber-feet dimples
        for (sx = [1, -1], sy = [1, -1])
            translate([sx*(bw/2 - 8), sy*(base_d/2 - 6), -0.1]) cylinder(d = 8, h = 0.8);
    }
}

dock();
