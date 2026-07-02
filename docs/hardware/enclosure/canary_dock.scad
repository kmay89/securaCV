// ============================================================================
//  Canary — FLEET PROVISIONING DOCK  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  The v1 validation runbook flashes and validates fleets of boards. This
//  dock holds N bare XIAOs reclined side by side next to a USB hub, USB-C
//  ports up, each bay NUMBERED — flash, provision, verify, deploy, in order
//  (bay number ↔ device identity in your notes). Parametric bay count.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

/* [What to render] */
part = "dock";       // ["dock"]

/* [Bays] */
n_bays = 4;          // number of XIAO bays  // [2:1:10]
recline = 15;        // bay recline angle (degrees)

/* [Board] — bare XIAO (any variant) */
board_l = 21.0;      // board length (USB edge at the TOP of the bay)
board_w = 17.5;
pocket_d = 7.0;      // pocket depth into the block (board + both-side parts)
thumb_d = 12.0;      // thumb cutout for extraction

/* [Block] */
bay_pitch = 24.0;    // bay-to-bay spacing
base_d = 34.0;       // block depth (front-back)
label_depth = 0.6;
label_font = "Liberation Sans:style=Bold";
tol_slide = 0.20;

/* [Quality] */
$fa = 3; $fs = 0.4;

bw = n_bays * bay_pitch + 10;
bh = board_l * cos(recline) + 14;
echo(str("Canary provisioning dock v0.1-dev — ", n_bays, " bays, ", bw, " mm wide  (IN DEVELOPMENT)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }

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
            // bay number on the front face
            translate([bx, -base_d/2 + 0.01, bh*0.35]) rotate([90, 0, 0])
                translate([0, 0, -label_depth]) linear_extrude(label_depth + 0.1)
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
