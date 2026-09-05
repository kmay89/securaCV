// ============================================================================
//  Canary — COVERT JUNCTION-BOX SHELL  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Sometimes a witness should look like street furniture. This shell holds
//  the compact WAP stack (plain XIAO ESP32-S3) inside a housing styled as a
//  utility junction box: squared body, fake conduit bosses on the sides, and
//  the camera aperture hidden inside a mock "unpunched knockout" ring on the
//  lid. Utilitarian gray ASA/PETG completes the disguise.
//
//  NOTE: this is a legitimate-use disguise for a device you own on property
//  you monitor lawfully — pair with the witness signage plate where notice
//  is required.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//
//  v0.1-dev (2026-08-23): canary_*_lib adoption (dedup, mesh-identical);
//  opt_mark knob added — the mark rides the lid's INTERIOR face, default off.
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d — the catalog's shared helpers
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_port_lib.scad>   // connector standards — the USB slot centers on the shell axis
use <canary_board_lib.scad>  // board registry — the XIAO numbers the knobs cite
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)

/* [What to render] */
part = "all";        // ["body","lid","all"]

/* [Options] */
opt_camera = true;   // aperture hidden in the knockout ring (XIAO Sense)
opt_led    = false;  // pinhole light pipe (covert: usually off)
// A covert box stays street-plain outside, so the mark sits where only the
// installer sees it.
opt_mark   = false;  // deboss the house wordmark on the lid's INTERIOR face (covert: never outside)

/* [Board] — XIAO ESP32-S3 (Sense) */
board_l = 21.0;  board_w = 17.5;  board_h = 1.2;  board_clear = 0.6;
                     // 21 x 17.5 spec — brd_l/brd_w("xiao"), canary_board_lib; a
                     // real board mics 17.8 (brd_xiao_w_measured()) and the clips'
                     // clip_clear absorbs the difference, so the spec default stands
stack_h = 8.0;   standoff_h = 3.5;   // 3.5: the clip beam (standoff + PCB) keeps the measured
                                     // 17.8 board under the 4.5 % strain budget (3.0 ran 5.5 %)

/* [Shell] */
wall_t = 2.2;  floor_t = 2.2;  lid_t = 2.4;   // deviates: service-box duty build — chunkier walls to match its squared corners
inner_pad = 8.0;     // interior margin around the board (wiring room)
corner_r = 2.0;      // deviates: squared, utilitarian — the service box reads as gear, not decor
lip_h = 3.0;  lip_t = 1.2;
boss_d = 12.0;       // fake conduit boss diameter (fits the body height; a true 1/2" boss needs a taller shell)
boss_l = 6.0;        // boss protrusion
screw_d = 1.6;  screw_head_d = 4.0;  screw_head_h = 1.6;
post_d = 5.0;
usb_w = 10.5;  usb_h = 6.5;

/* [Camera knockout] */
cam_ap_d   = 9.0;
cam_dx     = 0.0;    // aperture offset from the board center — MEASURE: the XIAO
cam_dy     = 0.0;    // Sense camera sits toward the antenna end, not dead center
ko_ring_d  = 22.0;   // mock knockout ring (aperture recessed inside it)
ko_depth   = 0.8;

/* [Print tolerances] */
tol_slide = 0.20;  tol_hole = 0.30;

/* [Board snap clips] */
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
inner_l = board_l + 2*board_clear + 2*inner_pad;
inner_w = board_w + 2*board_clear + 2*inner_pad;
cav_h   = standoff_h + board_h + stack_h + 1.0;
out_l = inner_l + 2*wall_t;
out_w = inner_w + 2*wall_t;
base_h = floor_t + cav_h;
pd = post_d;
echo(str("Canary covert j-box v0.1-dev — ", out_l, " x ", out_w, " x ", base_h + lid_t,
         " mm  (IN DEVELOPMENT)"));
// the mark library's measured type metrics gate the interior wordmark the same
// way they gate an exterior one — a smudge inside the box is still a smudge
assert(!opt_mark || mark_word_ink_w("securaCV", 4.0) <= inner_l - 6,
       str("opt_mark: the wordmark draws ", mark_word_ink_w("securaCV", 4.0),
           " mm wide — more than the lid interior offers; shrink the cap height"));

// (rrect2d/rrect come from canary_core_lib — the local copies are gone)
function post_xy() = [
    [ inner_l/2 - pd/2 - 0.2,  inner_w/2 - pd/2 - 0.2],
    [-inner_l/2 + pd/2 + 0.2,  inner_w/2 - pd/2 - 0.2],
    [ inner_l/2 - pd/2 - 0.2, -inner_w/2 + pd/2 + 0.2],
    [-inner_l/2 + pd/2 + 0.2, -inner_w/2 + pd/2 + 0.2],
];
// Cantilever snap clip on a board long edge — the compact-WAP idiom, so the
// drawing now comes from canary_snap_lib like the WAP's own: the insertion-
// strain arithmetic runs as an assert on every render instead of trusting a
// copied number. `cx` = position along the edge; `sy` = +1/-1 picks the edge.
module boardclip(cx, sy) {
    snap_boardclip(cx, sy * board_w/2, sy,
                   floor_t, floor_t + standoff_h + board_h,
                   clip_w, clip_t, clip_hook, clip_hook_h, clip_clear,
                   over = (brd_xiao_w_measured() - board_w)/2);
}

module body() {
    union() {
        difference() {
            union() {
                rrect(out_l, out_w, corner_r, base_h);
                // fake conduit bosses: two per long wall, half-cylinders lying on
                // the wall, CENTERD on the body height so they stay inside the
                // print envelope (a Ø21 boss poked 1.7 mm below the bed plane).
                // Horizontal cylinders on vertical walls: print with a dab of
                // support or accept a rough underside on the lower quarter.
                for (sy = [1, -1], i = [-1, 1])
                    translate([i*out_l/4, sy*(out_w/2 - 0.01), base_h/2])
                        rotate([-sy*90, 0, 0]) cylinder(d = boss_d, h = boss_l);
            }
            translate([0, 0, floor_t]) rrect(inner_l, inner_w, max(0.1, corner_r - wall_t), cav_h + 1);
            // USB pass-through slot, low on the -X short wall: route a pigtail
            // through it (a plug cannot reach the recessed connector) and dress
            // the cable as conduit. Sheltered mounting only - the slot is open.
            // centered on the connector AXIS (shell/2 above the PCB), not PCB-top + h/2:
            // the pigtail's lower half landed in the wall
            translate([-out_l/2, 0, floor_t + standoff_h + board_h + port_usbc_shell_h()/2])
                cube([wall_t*3, usb_w, usb_h], center = true);
        }
        // posts + board clips (compact-WAP idiom)
        difference() {
            for (p = post_xy()) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_h);
            for (p = post_xy()) translate([p[0], p[1], floor_t + 2]) cylinder(d = screw_d, h = cav_h);
        }
        // support rails under the board's short edges (clips alone don't set the
        // PCB height — review catch); board rests at standoff_h, clips retain it
        for (s = [1, -1])
            translate([s*(board_l/2 - 1.5) - 1.5, -(board_w - 1)/2, floor_t - 0.01])
                cube([3, board_w - 1, standoff_h + 0.01]);
        for (s = [1, -1]) for (cx = [-board_l/4, board_l/4]) boardclip(cx, s);
    }
}

module lid() {
    union() {
        difference() {
            rrect(out_l, out_w, corner_r, lid_t);
            // mock knockout: shallow circular groove ring; aperture recessed inside it
            translate([0, 0, lid_t - ko_depth]) difference() {
                cylinder(d = ko_ring_d + 1.6, h = ko_depth + 0.1);
                translate([0, 0, -0.1]) cylinder(d = ko_ring_d, h = ko_depth + 0.3);
            }
            if (opt_camera) translate([cam_dx, cam_dy, -1]) cylinder(d = cam_ap_d, h = lid_t + 2);
            if (opt_led) translate([out_l/4, 0, -1]) cylinder(d = 1.8, h = lid_t + 2);
            // the house wordmark (opt_mark), debossed into the INTERIOR face (z=0):
            // the disguise forbids exterior branding, but a marked interior still
            // identifies the device to whoever opens it. 0.5 deep at cap 4.0 —
            // above mark_word_min_h(), clear of the aperture (y), the post
            // counterbores (y) and the lip ring (which lives below z=0).
            if (opt_mark) translate([0, -9, -0.1])
                linear_extrude(0.6) mark_wordmark(4.0);
            for (p = post_xy()) {
                translate([p[0], p[1], -1]) cylinder(d = screw_d + 2*tol_hole, h = lid_t + 2);
                translate([p[0], p[1], lid_t - screw_head_h])
                    cylinder(d1 = screw_d + 2*tol_hole, d2 = screw_head_d, h = screw_head_h + 0.1);
            }
        }
        difference() {   // lip
            translate([0, 0, -lip_h]) difference() {
                rrect(inner_l - 2*tol_slide, inner_w - 2*tol_slide, 0.5, lip_h);
                rrect(inner_l - 2*tol_slide - 2*lip_t, inner_w - 2*tol_slide - 2*lip_t, 0.3, lip_h + 1);
            }
            for (p = post_xy()) translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
            translate([-inner_l/2, 0, -lip_h/2]) cube([lip_t*4, usb_w + 4, lip_h + 0.2], center = true);
        }
    }
}

if      (part == "body") body();
else if (part == "lid")  translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();
else { body(); translate([out_l + 16, 0, lid_t]) rotate([180, 0, 0]) lid(); }
