// ============================================================================
//  Canary — HUB ENCLOSURE (Raspberry Pi 5, DIN rail)  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  The server side of SecuraCV (compose stack: broker, verifier, dashboards)
//  lives on a Pi-class box. This is a vented tray + cover for a Raspberry
//  Pi 5 that clips onto a standard 35 mm top-hat DIN rail (or screws flat),
//  with all port faces open, chimney venting over the SoC, and an M.2/SSD
//  HAT height budget.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. The DIN
//     clip is a printed spring — PETG minimum, verify engagement on your rail.
// ============================================================================

/* [What to render] */
part = "all";        // ["tray","cover","all"]

/* [Board] — Raspberry Pi 5 */
pi_l = 85.0;         // X
pi_w = 56.0;         // Y
hole_dx = 58.0;      // mount hole grid (X)
hole_dy = 49.0;      // mount hole grid (Y)
hole_off_x = 3.5;    // grid corner from board corner
hole_off_y = 3.5;
pcb_t = 1.4;
standoff_h = 6.0;    // clearance under the board (PoE HAT pins / SD access)
hat_h = 22.0;        // headroom above the PCB (HAT/M.2 + fan)

/* [Shell] */
wall_t = 2.0;  floor_t = 2.5;  lid_t = 2.0;  corner_r = 3.0;
board_clear = 1.0;
tol_slide = 0.20;  tol_hole = 0.30;
screw_d = 2.2;       // M2.5 self-tap into the standoffs
screw_head_d = 5.0;
lid_screw_d = 2.6;   // M3 self-tap into corner posts
post_d = 7.0;
lid_edge = 0.8;

/* [DIN rail] — 35 mm top-hat (EN 50022) */
din = true;
din_w = 35.2;        // rail width across the flanges
din_lip = 5.0;       // flange depth engaged by the hooks
din_t = 1.5;         // rail metal + spring clearance

/* [Venting] */
vent_slot_w = 2.0;
vent_slot_p = 5.0;   // pitch

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
inner_l = pi_l + 2*board_clear;
inner_w = pi_w + 2*board_clear;
out_l = inner_l + 2*wall_t;
out_w = inner_w + 2*wall_t;
tray_h = floor_t + standoff_h + pcb_t + 6;     // tray walls stop above the PCB
total_h = floor_t + standoff_h + pcb_t + hat_h + lid_t;
echo(str("Canary hub (Pi 5, DIN) v0.1-dev — ", out_l, " x ", out_w, " x ", total_h,
         " mm  (IN DEVELOPMENT)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module rrect(l, w, r, h) { linear_extrude(h) rrect2d(l, w, r); }

function holes() = [
    [-pi_l/2 + hole_off_x,           -pi_w/2 + hole_off_y],
    [-pi_l/2 + hole_off_x + hole_dx, -pi_w/2 + hole_off_y],
    [-pi_l/2 + hole_off_x,           -pi_w/2 + hole_off_y + hole_dy],
    [-pi_l/2 + hole_off_x + hole_dx, -pi_w/2 + hole_off_y + hole_dy],
];

// DIN clip on the tray underside: fixed hook (top) + printed spring hook (bottom)
module din_clip() {
    cw = 42;                                    // clip block width along the rail
    translate([-cw/2, 0, 0]) {
        // riser pad
        translate([0, -din_w/2 - 8, -6]) cube([cw, din_w + 16, 6.01]);
        // fixed hook over the top flange (lip points INWARD over the rail)
        translate([0, din_w/2 + din_t - 4, -6]) cube([cw, 4 + 1.6, 3]);
        translate([0, din_w/2 + din_t, -6]) cube([cw, 1.6, 6.01]);
        // spring arm + hook under the bottom flange (flexes -Y to snap on)
        translate([0, -din_w/2 - din_t - 1.6, -6]) cube([cw, 1.6, 6.01]);
        translate([0, -din_w/2 - din_t - 1.6, -6]) cube([cw, 4 + 1.6, 1.8]);
        // release tab
        translate([0, -din_w/2 - din_t - 10, -6]) cube([cw, 8.5, 1.8]);
    }
}

module tray() {
    union() {
        difference() {
            rrect(out_l, out_w, corner_r, tray_h);
            translate([0, 0, floor_t]) rrect(inner_l, inner_w, max(0.1, corner_r - wall_t), tray_h);
            // port faces fully open above the standoff line on both short walls
            for (s = [1, -1]) translate([s*(inner_l/2 + wall_t/2), 0, floor_t + standoff_h])
                cube([wall_t*2, inner_w - 6, tray_h], center = true);
            // USB/eth face also open on one long wall (Pi 5 ports are on one long edge)
            translate([0, -(inner_w/2 + wall_t/2), floor_t + standoff_h])
                cube([inner_l - 10, wall_t*2, tray_h], center = true);
            // floor vent slots
            for (x = [-inner_l/2 + 8 : vent_slot_p : inner_l/2 - 8])
                translate([x, 0, -0.1]) linear_extrude(floor_t + 0.2)
                    rrect2d(vent_slot_w, inner_w - 16, 1);
        }
        // Pi standoffs (M2.5 self-tap)
        for (h = holes()) translate([h[0], h[1], floor_t])
            difference() {
                cylinder(d = 7, h = standoff_h);
                translate([0, 0, 1]) cylinder(d = screw_d, h = standoff_h + 1);
            }
        // cover posts at the corners
        for (sx = [1, -1], sy = [1, -1])
            translate([sx*(inner_l/2 - post_d/2 - 0.2), sy*(inner_w/2 - post_d/2 - 0.2), floor_t])
                difference() {
                    cylinder(d = post_d, h = tray_h - floor_t);
                    translate([0, 0, 2]) cylinder(d = lid_screw_d, h = tray_h);
                }
        if (din) din_clip();
    }
}

// cover: slides over the tray walls; chimney vents over the SoC zone; screwed
// at the four posts with M3 x 25 through tubes
module cover() {
    ch = total_h - tray_h + 4;                  // cover skirt height (4 mm overlap)
    union() {
        difference() {
            union() {
                rrect(out_l + 2*tol_slide + 3.2, out_w + 2*tol_slide + 3.2, corner_r + 1.6, lid_t);
                translate([0, 0, lid_t - 0.01]) linear_extrude(ch)
                    difference() {
                        rrect2d(out_l + 2*tol_slide + 3.2, out_w + 2*tol_slide + 3.2, corner_r + 1.6);
                        rrect2d(out_l + 2*tol_slide, out_w + 2*tol_slide, corner_r + tol_slide);
                    }
            }
            // top chimney vents
            for (x = [-25 : vent_slot_p : 25])
                translate([x, 0, -0.1]) linear_extrude(lid_t + 0.2) rrect2d(vent_slot_w, out_w - 24, 1);
            // side louvres on the skirt (rear long wall)
            for (x = [-out_l/2 + 12 : 8 : out_l/2 - 12])
                translate([x, (out_w + 3.2)/2 + 1, lid_t + 6]) rotate([90, 0, 0])
                    linear_extrude(6) rrect2d(3, ch - 10, 1.4);
            // lid screw tubes' holes
            for (sx = [1, -1], sy = [1, -1])
                translate([sx*(inner_l/2 - post_d/2 - 0.2), sy*(inner_w/2 - post_d/2 - 0.2), -0.1]) {
                    cylinder(d = lid_screw_d + 2*tol_hole, h = lid_t + ch + 1);
                    translate([0, 0, 0.1]) cylinder(d1 = screw_head_d + 1, d2 = lid_screw_d + 2*tol_hole, h = 1.2);
                }
        }
        // internal screw tubes guiding the long M3s down to the posts
        for (sx = [1, -1], sy = [1, -1])
            translate([sx*(inner_l/2 - post_d/2 - 0.2), sy*(inner_w/2 - post_d/2 - 0.2), lid_t - 0.01])
                difference() {
                    cylinder(d = lid_screw_d + 2*tol_hole + 2.4, h = ch - 2);
                    translate([0, 0, -1]) cylinder(d = lid_screw_d + 2*tol_hole, h = ch + 2);
                }
    }
}

if      (part == "tray")  tray();
else if (part == "cover") cover();
else { tray(); translate([out_l + 20, 0, 0]) cover(); }
