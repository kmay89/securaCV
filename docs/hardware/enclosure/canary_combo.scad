// ============================================================================
//  Canary — RADAR + CAMERA COMBO WITNESS  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  One housing, two stacks: the Vision build (OV5647 + Grove Vision AI V2 +
//  stacked XIAO) beside the Sense build (MR60BHA2 + stacked XIAO C6).
//  Radar-confirmed camera events kill false positives — the classic
//  commercial-camera trick, here as two independent signed witnesses that
//  corroborate each other. Front face: lens + disc seat on one column,
//  RADOME window on the other (all radome rules apply — see the Sense case).
//  Two USB-C ports exit the bottom wall (one per stack).
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. This is a
//  geometry merge of two proven cases, but the combined layout is untested.
//
//  2026-08-23: adopted the shared contract libraries (core/mount/snap/port/
//              board/mark) — the local helper copies they replace drew the
//              same geometry. Two fixes ride along: bridge-safe chamfered
//              USB openings (canary_port_lib — all three bottom-wall ports
//              bridged a flat top) and PAN-head FLAT counterbores on the
//              front (the Vision's print-validated lesson — the old shallow
//              cone left the head standing on the show face). New opt_mark
//              knob debosses the house wordmark (default off).
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, soft-edge front, screw seats — the shared idiom
use <canary_mount_lib.scad>  // the stud/keyhole hanging standard — the blind pockets' one home
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_port_lib.scad>   // bridge-safe USB openings (the WAP's print-validated profile)
use <canary_board_lib.scad>  // board registry — the Grove/MR60 numbers the knobs cite
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)

/* [What to render] */
part = "all";        // ["back","front","all","gasket"]

/* [Options] */
opt_seal = false;    // gasket + drip skirt (same system as the other cases)
opt_hood = false;    // rain hood over the lens
opt_led  = true;     // one light pipe (wire to either stack's LED)

/* [Vision stack] — Grove Vision AI V2 + stacked XIAO. MEASURE */
vm_l = 40.0;  vm_w = 20.0;   // measured Grove 1x2 form — brd_l/brd_w("grove_v2"), canary_board_lib
                             // (was 25x25 — same fix as the Vision/doorbell cases; the registry
                             // pins the wrong square dead in board_selfcheck)
cam_w = 25.0;  cam_h = 24.0;
cam_hole_x = 21.0;  cam_hole_y = 12.5;  cam_post_d = 3.6;  cam_post_h = 4.0;  cam_screw_d = 1.6;
lens_dx = 0.0;  lens_dy = 2.5;
cam_ap_d = 10.0;  cam_disc_d = 14.0;  cam_disc_t = 1.0;
v_stack_sock = 11.5;   // brd_stack_sock_unmeasured(), canary_board_lib — the roomier legacy
                       // guess the Vision build keeps (bench-measured 6.5 = brd_stack_sock_measured)
v_front_h = 5.0;
v_usb_drop = 10.0;   // XIAO port below the module port (see Vision case)

/* [Sense stack] — MR60BHA2 carrier + stacked XIAO C6. MEASURE */
sm_l = 44.0;  sm_w = 36.0;   // brd_l/brd_w("mr60"), canary_board_lib
s_stack_sock = 11.5;         // brd_stack_sock_unmeasured() — same decision as v_stack_sock
s_front_h = 3.5;
ant_h = 1.2;
radome_t = 1.5;   // ≈ half-wave in PETG/ASA at 60 GHz (low-reflection optimum); AVOID 0.7–1.1 mm
rad_win_x = 24.0;  rad_win_y = 24.0;  rad_dx = 0.0;  rad_dy = 6.0;
s_usb_z = 4.0;       // C6 USB center above the floor
lux_d = 3.5;  lux_dx = -13.0;  lux_dy = -14.0;
lp_d = 3.0;   lp_dx = 13.0;   lp_dy = -14.0;

/* [Shared] */
pcb_t = 1.0;  board_clear = 0.6;  cav_extra = 1.0;
wall_t = 2.0;  floor_t = 2.0;  lid_t = 2.0;  lip_h = 4.0;  lip_t = 1.2;  corner_r = 3.0;
tol_slide = 0.20;  tol_press = 0.10;  tol_hole = 0.30;   // the catalog trio — core_tol_*(), canary_core_lib
post_d = 5.0;  screw_d = 1.6;  screw_head_d = 4.0;  screw_head_h = 2.0;
usb_w = 10.5;  usb_h = 6.5;
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;  skirt_h = 3.0;  skirt_t = 1.6;
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;   // snap_boardclip defaults — canary_snap_lib runs the strain budget as an assert
lid_edge = 0.8;  lid_edge2 = 0.0;
hood_len = 9.0;  hood_t = 1.8;
kh_extra = 3.0;  kh_head_d = 7.0;  kh_shank_d = 4.2;  kh_slot_l = 8.0;  kh_head_h = 3.5;  kh_face = 1.0;   // catalog standard — mount_kh_*(), canary_mount_lib
opt_mount = true;    // blind keyholes in the back

/* [Aesthetics] */
// Placed by mark_dx/dy/rot/size/depth, gated by the library's measured type
// metrics so a size that would print as a smudge or run off the face is refused
opt_mark   = false;  // deboss the house wordmark on the front (canary_mark_lib)
                     // before a print, not after. Keep it OUT of the radome window.
mark_size  = 5.0;    // wordmark cap height
mark_depth = 0.5;    // deboss depth (the front prints face-down -> crisp first-layer voids)
mark_dx    = 0.0;    // mark center offset from the FRONT-plate center
mark_dy    = -31.5;  // default rides the bottom margin, clear of the lux/light-pipe holes
mark_rot   = 0;      // rotation (degrees)

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
e_seal = opt_seal;
wall_eff = e_seal ? max(wall_t, gasket_w + 1.6) : wall_t;
pd = post_d;  post_corner = pd + 1.5;
clip_stack = clip_clear + clip_t;
v_standoff = v_stack_sock + 1.5;
s_standoff = s_stack_sock + 1.5;

col_v = max(cam_w + 2*board_clear, vm_w + 2*(clip_stack + board_clear) + 0.5);
col_s = sm_w + 2*(clip_stack + board_clear) + 0.5;
inner_x = col_v + 3 + col_s + 2*post_corner;
inner_y = max(board_clear + vm_l + 2 + cam_h + 3, board_clear + sm_l + 8);
cav_d = max(v_standoff + pcb_t + v_front_h, s_standoff + pcb_t + s_front_h,
            v_standoff + pcb_t + usb_h) + cav_extra;   // keep the Vision USB opening fully inside the wall

out_x = inner_x + 2*wall_eff;
out_y = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;

v_cx = -inner_x/2 + post_corner + col_v/2;
s_cx =  inner_x/2 - post_corner - col_s/2;
vm_cy  = -inner_y/2 + board_clear + vm_l/2;
cam_cy = vm_cy + vm_l/2 + 2 + cam_h/2;
sm_cy  = -inner_y/2 + board_clear + sm_l/2;
lens_x = v_cx + lens_dx;  lens_y = cam_cy + lens_dy;
rad_cx = s_cx + rad_dx;   rad_cy = sm_cy + rad_dy;
v_usb_zc = floor_t + v_standoff + pcb_t + usb_h/2;
rad_gap = cav_d - s_standoff - pcb_t - ant_h + (lid_t - radome_t);  // true gap even when the Vision stack sets cav_d

mount_extra = opt_mount ? kh_extra : 0;
skirt_gap = tol_slide + 0.2;
plate_x = e_seal ? out_x + 2*(skirt_gap + skirt_t) : out_x;
plate_y = e_seal ? out_y + 2*(skirt_gap + skirt_t) : out_y;
plate_r = e_seal ? corner_r + skirt_gap + skirt_t : corner_r;

function post_xy() = [
    [ inner_x/2 - pd/2 - 0.2,  inner_y/2 - pd/2 - 0.2],
    [-inner_x/2 + pd/2 + 0.2,  inner_y/2 - pd/2 - 0.2],
    [ inner_x/2 - pd/2 - 0.2, -inner_y/2 + pd/2 + 0.2],
    [-inner_x/2 + pd/2 + 0.2, -inner_y/2 + pd/2 + 0.2],
];
assert(radome_t >= 0.6 && radome_t < lid_t, "radome_t out of range");
assert(rad_gap >= 3.0, "antenna-to-radome gap < 3 mm — raise cav_extra");
assert(rad_win_x + 2*abs(rad_dx) <= col_s && rad_win_y + 2*abs(rad_dy) <= sm_l,
       "radome window exceeds the sense column — shrink rad_win/rad_dx/rad_dy");
assert(lip_h < cav_d, "lip_h vs cavity");
assert(!opt_mark || (mark_depth > 0 && mark_depth < lid_t),
       "mark_depth must be between 0 and lid_t");
// the wordmark's two gates, from the mark library's measured type metrics:
// below mark_word_min_h() a 0.4 mm bead no longer reaches the letterforms and
// the deboss prints as a smudge with the rhythm of type — the render looks
// perfect either way, which is why this is an assert and not an eyeball
assert(!opt_mark || mark_size >= mark_word_min_h(),
       str("opt_mark at mark_size ", mark_size, " mm is under the ",
           mark_word_min_h(), " mm cap height where a 0.4 mm bead still ",
           "reaches the letterforms — raise mark_size"));
assert(!opt_mark || mark_word_ink_w("securaCV", mark_size) <= plate_x - 4.0,
       str("the wordmark draws ", mark_word_ink_w("securaCV", mark_size),
           " mm at mark_size ", mark_size, " on a ", plate_x,
           " mm face (2 mm margin per side) — shrink mark_size"));
echo(str("Canary COMBO witness v0.1-dev — ", out_x, " x ", out_y, " x ", base_d + lid_t + mount_extra,
         " mm, radar gap ", rad_gap, " mm  (IN DEVELOPMENT)"));

// rrect2d/rrect come from canary_core_lib; only file-specific geometry stays local
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
        offset(r = -w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
    }
}
// Cantilever snap clip on a board-column edge — canary_snap_lib's beam (the
// WAP pattern), so the insertion-strain arithmetic runs as an assert on every
// render instead of living in a comment. The clips stand on the ±X edges of
// each column, so the wrapper keeps the axis rotation and hands the drawing
// to the library.
module edgeclip(px, py, ang, soff) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, floor_t, floor_t + soff + pcb_t,
                       clip_w, clip_t, clip_hook, clip_hook_h, clip_clear);
}
module rails(cx, cy, w, l, soff) {
    for (s = [1, -1]) {
        difference() {
            translate([cx + s*(w/2 - 1.5) - 1.5, cy - (l - 1)/2, floor_t]) cube([3, l - 1, soff]);
            translate([cx + s*(w/2 - 1.5), cy, floor_t + soff/2]) cube([5, clip_w + 2, soff + 1], center = true);
        }
        edgeclip(cx + s*w/2, cy, s > 0 ? 0 : 180, soff);
    }
}
// blind keyhole pocket (xc = center along X; slot toward +Y = UP on the wall,
// so the case slides down to seat — gravity is the latch); canary_mount_lib
// draws it natively along the axis — no rotate — so the interface has one
// home and this mesh stays put
module keyhole_pocket(xc) {
    translate([xc, inner_y/2 - 14, 0])
        mount_keyhole_pocket(0, -mount_extra, "y",
                             kh_head_d, kh_shank_d, kh_slot_l, kh_head_h, kh_face);
}

module back() {
    posts = post_xy();
    union() {
        difference() {
            union() {
                rrect(out_x, out_y, corner_r, base_d);
                if (mount_extra > 0) translate([0, 0, -mount_extra]) rrect(out_x, out_y, corner_r, mount_extra);
            }
            translate([0, 0, floor_t]) rrect(inner_x, inner_y, max(0.1, corner_r - wall_eff), cav_d + 1);
            // Vision stack: module port + XIAO port (stacked); Sense stack: C6
            // port. All three: 45°-chamfered top corners halve the unsupported
            // bridge in the upright-printed wall and keep any droop out of the
            // plug envelope — canary_port_lib (the WAP's print-validated
            // profile; these walls used to bridge a flat top)
            for (pz = [[v_cx, v_usb_zc], [v_cx, v_usb_zc - v_usb_drop],
                       [s_cx, floor_t + s_usb_z]])
                translate([pz[0], -out_y/2 + wall_eff*1.5, pz[1]])
                    rotate([90, 0, 0]) linear_extrude(wall_eff*3)
                        port_bridge_profile2d(usb_w, usb_h);
            if (e_seal)
                translate([0, 0, base_d - gasket_groove]) linear_extrude(gasket_groove + 1) rim_ring2d(gasket_w);
            if (mount_extra > 0) { keyhole_pocket(-inner_x/4); keyhole_pocket(inner_x/4); }
        }
        difference() {
            union() {
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d);
                for (p = posts) {
                    sx = sign(p[0]); sy = sign(p[1]);
                    hull() { translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - lip_h - 1);
                             translate([sx*(inner_x/2 - 0.3), p[1], floor_t]) cylinder(d = 2, h = cav_d - lip_h - 1); }
                    hull() { translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - lip_h - 1);
                             translate([p[0], sy*(inner_y/2 - 0.3), floor_t]) cylinder(d = 2, h = cav_d - lip_h - 1); }
                }
            }
            for (p = posts) translate([p[0], p[1], floor_t + 2]) cylinder(d = screw_d, h = cav_d);
        }
        rails(v_cx, vm_cy, vm_w, vm_l, v_standoff);
        rails(s_cx, sm_cy, sm_w, sm_l, s_standoff);
    }
}

module front() {
    union() {
        difference() {
            // the plate with the catalog's two-stage soft edge — canary_core_lib
            // (this also wires up the previously inert lid_edge2 knob)
            soft_edge_plate(plate_x, plate_y, plate_r, lid_t, lid_edge, lid_edge2);
            // lens + disc seat + lead-in
            translate([lens_x, lens_y, -1]) cylinder(d = cam_ap_d, h = lid_t + 2);
            translate([lens_x, lens_y, lid_t - (cam_disc_t + 0.2)])
                cylinder(d = cam_disc_d + 2*tol_slide, h = cam_disc_t + 1);
            translate([lens_x, lens_y, lid_t - 0.4])
                cylinder(d1 = cam_disc_d + 2*tol_slide, d2 = cam_disc_d + 2*tol_slide + 1, h = 0.41);
            // radome window (blind thinning from inside)
            translate([rad_cx, rad_cy, -1]) linear_extrude(lid_t - radome_t + 1) rrect2d(rad_win_x, rad_win_y, 3);
            // lux + light pipe on the sense column
            translate([s_cx + lux_dx, sm_cy + lux_dy, -1]) cylinder(d = lux_d, h = lid_t + 2);
            if (opt_led) translate([s_cx + lp_dx, sm_cy + lp_dy, -1]) cylinder(d = lp_d + 2*tol_press, h = lid_t + 2);
            // flat counterbores: the BOM's PAN-head screws seat flush — the
            // canary_core_lib seat, per the Vision's print-validated lesson
            // (a cone this shallow left the head standing on the show face)
            for (p = post_xy())
                cb_flat_cut(p[0], p[1], lid_t, screw_d + 2*tol_hole,
                            screw_head_d + 2*tol_hole, screw_head_h);
            // the house wordmark (opt_mark), debossed on the show face by the
            // first-layer machinery — canary_mark_lib owns the word and its
            // metrics, this file only places it (mark_dx/dy/rot/size/depth)
            if (opt_mark)
                translate([mark_dx, mark_dy, lid_t - mark_depth])
                    linear_extrude(mark_depth + 1) rotate(mark_rot)
                        mark_wordmark(mark_size);
        }
        // camera posts
        for (sx = [1, -1], sy = [1, -1])
            translate([v_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, -cam_post_h])
                difference() {
                    cylinder(d = cam_post_d, h = cam_post_h + 0.1);
                    translate([0, 0, -0.1]) cylinder(d = cam_screw_d, h = cam_post_h - 0.8);
                }
        // lip
        difference() {
            translate([0, 0, -lip_h]) difference() {
                rrect(inner_x - 2*tol_slide, inner_y - 2*tol_slide, max(0.1, corner_r - wall_eff - tol_slide), lip_h);
                rrect(inner_x - 2*tol_slide - 2*lip_t, inner_y - 2*tol_slide - 2*lip_t, 0.1, lip_h + 1);
            }
            for (p = post_xy()) translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
            for (cx = [v_cx, s_cx]) translate([cx, -inner_y/2, -lip_h/2]) cube([usb_w + 4, lip_t*4, lip_h + 0.2], center = true);
        }
        // hood over the lens
        if (opt_hood)
            translate([lens_x, lens_y, lid_t - 0.1]) linear_extrude(hood_len + 0.1)
                difference() {
                    circle(d = cam_disc_d + 3 + 2*hood_t);
                    circle(d = cam_disc_d + 3);
                    translate([-(cam_disc_d/2 + hood_t + 2), -2*(cam_disc_d + hood_t)])
                        square([cam_disc_d + 2*hood_t + 4, 2*(cam_disc_d + hood_t) - cam_disc_d*0.18]);
                }
        // drip skirt
        if (e_seal) difference() {
            translate([0, 0, -skirt_h]) linear_extrude(skirt_h) difference() {
                rrect2d(plate_x, plate_y, plate_r);
                rrect2d(out_x + 2*skirt_gap, out_y + 2*skirt_gap, corner_r + skirt_gap);
            }
            for (cx = [v_cx, s_cx]) translate([cx, -(out_y/2 + skirt_gap + skirt_t/2), -skirt_h/2])
                cube([usb_w + 6, skirt_t*3, skirt_h + 0.4], center = true);
        }
    }
}

module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5); }

if      (part == "back")   back();
else if (part == "front")  translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
else if (part == "gasket") { assert(e_seal, "gasket needs opt_seal"); gasket(); }
else {
    back();
    translate([0, -(out_y/2 + plate_y/2 + 12), 0]) translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
    if (e_seal) translate([-(out_x + 16), 0, 0]) gasket();
}
