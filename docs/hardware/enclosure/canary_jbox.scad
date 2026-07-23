// ============================================================================
//  Canary — COVERT JUNCTION-BOX SHELL  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Sometimes a witness should look like street furniture. This shell holds
//  the compact WAP stack (plain XIAO ESP32-S3) inside a housing styled as a
//  utility junction box: squared body, fake conduit bosses on the sides, and
//  the camera aperture hidden inside a mock "unpunched knockout" ring on the
//  lid. Utilitarian grey ASA/PETG completes the disguise.
//
//  NOTE: this is a legitimate-use disguise for a device you own on property
//  you monitor lawfully — pair with the witness signage plate where notice
//  is required.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

/* [What to render] */
part = "all";        // ["body","lid","all"]

/* [Options] */
opt_camera = true;   // aperture hidden in the knockout ring (XIAO Sense)
opt_led    = false;  // pinhole light pipe (covert: usually off)

/* [Board] — XIAO ESP32-S3 (Sense) */
board_l = 21.0;  board_w = 17.5;  board_h = 1.2;  board_clear = 0.6;
stack_h = 8.0;   standoff_h = 3.0;

/* [Shell] */
wall_t = 2.2;  floor_t = 2.2;  lid_t = 2.4;
inner_pad = 8.0;     // interior margin around the board (wiring room)
corner_r = 2.0;      // squared, utilitarian
lip_h = 3.0;  lip_t = 1.2;
boss_d = 12.0;       // fake conduit boss diameter (fits the body height; a true 1/2" boss needs a taller shell)
boss_l = 6.0;        // boss protrusion
screw_d = 1.6;  screw_head_d = 4.0;  screw_head_h = 1.6;
post_d = 5.0;
usb_w = 10.5;  usb_h = 6.5;

/* [Camera knockout] */
cam_ap_d   = 9.0;
cam_dx     = 0.0;    // aperture offset from the board centre — MEASURE: the XIAO
cam_dy     = 0.0;    // Sense camera sits toward the antenna end, not dead centre
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

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module rrect(l, w, r, h) { linear_extrude(h) rrect2d(l, w, r); }
function post_xy() = [
    [ inner_l/2 - pd/2 - 0.2,  inner_w/2 - pd/2 - 0.2],
    [-inner_l/2 + pd/2 + 0.2,  inner_w/2 - pd/2 - 0.2],
    [ inner_l/2 - pd/2 - 0.2, -inner_w/2 + pd/2 + 0.2],
    [-inner_l/2 + pd/2 + 0.2, -inner_w/2 + pd/2 + 0.2],
];
module edgeclip(px, py, ang) {
    bt = floor_t + standoff_h + board_h;  tp = bt + clip_hook_h;
    pts = [ [clip_clear, floor_t], [clip_clear + clip_t, floor_t],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
}

module body() {
    union() {
        difference() {
            union() {
                rrect(out_l, out_w, corner_r, base_h);
                // fake conduit bosses: two per long wall, half-cylinders lying on
                // the wall, CENTRED on the body height so they stay inside the
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
            translate([-out_l/2, 0, floor_t + standoff_h + board_h + usb_h/2])
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
        for (s = [1, -1]) for (cx = [-board_l/4, board_l/4]) edgeclip(cx, s*board_w/2, s > 0 ? 90 : 270);
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
