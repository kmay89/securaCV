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
//  2026-09-24: the rain hood is its OWN part (part="hood", the Vision's PRT-1):
//              grown on the front it had no printable pose. The front carries
//              a hood_seat groove and prints face-down with or without it.
//              Sealed builds get a pressure path — opt_weep drains the bottom
//              wall between the columns (asserted in seal mode, WTR-1/2) — and
//              usb_hood puts a port_hood awning over the Sense port. Corner
//              posts stand 0.2 INTO the walls (they stood 0.2 off them, webbed
//              only by the gussets), and the camera screws are called out at
//              the length their 3.1 mm pilots take (M2 x 4, not x 6).
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, soft-edge front, screw seats — the shared idiom
use <canary_mount_lib.scad>  // the stud/keyhole hanging standard — the blind pockets' one home
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_port_lib.scad>   // bridge-safe USB openings (the WAP's print-validated profile)
use <canary_board_lib.scad>  // board registry — the Grove/MR60 numbers the knobs cite
use <canary_rib_lib.scad>    // corner_gusset — the constant-width post web
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)

/* [What to render] */
part = "all";        // ["back","front","all","gasket","hood"]

/* [Options] */
opt_seal = false;    // gasket + drip skirt (same system as the other cases)
opt_hood = false;    // rain hood over the lens — its OWN part (part="hood"), pressed into a groove on the front and bonded
opt_weep = true;     // Ø2 drain through the bottom wall between the columns (canary_core_lib weep_cut); the sealed build's pressure path
usb_hood = false;    // drip awning (canary_core_lib port_hood) over the Sense USB opening; the Vision pair sits under the rim
opt_led  = true;     // one light pipe (wire to either stack's LED)

/* [Vision stack] — Grove Vision AI V2 + stacked XIAO. MEASURE */
vm_l = 40.0;                 // Grove Vision AI V2 length — the measured 1x2 form, brd_l("grove_v2"), canary_board_lib
vm_w = 20.0;                 // Grove Vision AI V2 width — the measured 1x2 form, brd_w("grove_v2")
                             // (was 25x25 — same fix as the Vision/doorbell cases; the registry
                             // pins the wrong square dead in board_selfcheck)
cam_w = 25.0;  cam_h = 24.0;
cam_hole_x = 21.0;  cam_hole_y = 12.5;  cam_post_d = 3.6;  cam_post_h = 4.0;  cam_screw_d = 1.6;
lens_dx = 0.0;  lens_dy = 2.5;
cam_ap_d = 10.0;  cam_disc_d = 14.0;  cam_disc_t = 1.0;
v_stack_sock = 6.5;    // brd_stack_sock_measured(), canary_board_lib — the XIAO ports are DERIVED
                       // from this, so the unmeasured 11.5 put them in the floor
v_front_h = 5.0;
xiao_w = 17.8;         // the measured XIAO — sets the corner pins that catch the module under it
xiao_below = 5.5;      // air under a stacked XIAO's USB face: shell + half a plug overmold + clearance

/* [Sense stack] — MR60BHA2 carrier + stacked XIAO C6. MEASURE */
sm_l = 44.0;                 // MR60 carrier length — brd_l("mr60"), canary_board_lib
sm_w = 36.0;                 // MR60 carrier width — brd_w("mr60"), canary_board_lib
s_stack_sock = 6.5;          // brd_stack_sock_measured() — same decision as v_stack_sock
s_front_h = 3.5;
ant_h = 1.2;
radome_t = 1.5;   // ≈ half-wave in PETG/ASA at 60 GHz (low-reflection optimum); AVOID 0.7–1.1 mm
rad_win_x = 24.0;  rad_win_y = 24.0;  rad_dx = 0.0;  rad_dy = 6.0;
s_usb_z = 0.0;       // extra lift of the C6 port relative to its DERIVED axis (a measured correction)
lux_d = 3.5;  lux_dx = -13.0;  lux_dy = -14.0;
lp_d = 3.0;   lp_dx = 13.0;   lp_dy = -14.0;

/* [Shared] */
pcb_t = 1.0;  board_clear = 0.6;  cav_extra = 1.0;
wall_t = 2.0;  floor_t = 2.0;  lid_t = 2.0;  lip_h = 4.0;  lip_t = 1.2;  corner_r = 3.0;
floor_cove = 0.8;  // 45° cove where the floor meets the walls, inside (canary_core_lib cavity_cut); 0 = the old square corner  // [0:0.2:1.2]
                   // The sharp notch there was the crack-starter in every flat-printed shell — a corner drop
                   // hinges the floor about it along one layer boundary.
lid_key    = true; // poka-yoke: a rib on the +Y cavity wall and a slot in the lid's lip — four corner posts fit
                   // a lid two ways and every lid feature lines up one way; turned round it stands lip_h proud
tol_slide = 0.20;  // catalog default — core_tol_slide(), canary_core_lib
tol_press = 0.10;  // catalog default — core_tol_press(), canary_core_lib
tol_hole  = 0.30;  // catalog default — core_tol_hole(), canary_core_lib
post_d = 5.0;  screw_d = 1.6;  screw_head_d = 4.0;  screw_head_h = 2.0;
screw_from = "back"; // ["back","face"] back = the face is unbroken: screws enter a seat in the back and thread into bosses under the front (the house default); face = pan heads on the front
usb_w = 12.0;  // opening width — 12 clears rugged cable boots (the WAP's validated opening)
usb_h = 6.5;   // opening height — boot clearance (the WAP's validated opening)
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;  skirt_h = 3.0;  skirt_t = 1.6;
clip_w      = 6.0;   // board-clip tab width along the board edge — snap_boardclip default, canary_snap_lib
clip_t      = 1.0;   // clip beam thickness — snap_boardclip default; canary_snap_lib runs the strain budget as an assert
clip_hook   = 0.5;   // lip overhang over the board top — snap_boardclip default
clip_hook_h = 1.2;   // lip + 45° lead-in height above the board top — snap_boardclip default
clip_clear  = 0.25;  // beam face to board edge (a fit — tune on the coupon) — snap_boardclip default
lid_edge  = 0.8;  // first (45°) stage of the show-face edge, mm — core_face_edge()  // [0:0.1:1.5]
lid_edge2 = 0.8;  // second (~66°) stage of the show-face edge, mm — ON is the house look (core_face_edge2()); it is what reads as a roundover instead of a bevel. 0 leaves the plain 45° facet any CAD default gives you  // [0:0.1:1.5]
hood_len = 9.0;  hood_t = 1.8;
hood_seat = 0.6;  // groove in the front's show face the hood's spigot presses into (tol_press); bond with neutral-cure silicone  // [0.4:0.1:1.0]
usb_hood_reach = 3.0;  // how far the USB awning's drip edge stands off the bottom wall  // [2:0.5:6]

/* [Stud/keyhole interface] — blind keyholes in the back */
kh_extra   = 3.0;   // back thickening that hosts the keyhole pockets
kh_head_d  = 7.0;   // screw-head pass hole — catalog standard, mount_kh_head_d(), canary_mount_lib
kh_shank_d = 4.2;   // shank slot width — catalog standard, mount_kh_shank_d()
kh_slot_l  = 8.0;   // slot travel — catalog standard, mount_kh_slot_l()
kh_head_h  = 3.5;   // total pocket depth (face web + head cavity) — catalog standard, mount_kh_head_h()
kh_face    = 1.0;   // face web the screw head grips behind — catalog standard, mount_kh_face()
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
// 1.2 mm cheek each side of the groove (2*core_min_wall) — this file carried
// the WAP's 0.8-cheek fork; the cheeks are the seal path's walls
wall_eff = e_seal ? max(wall_t, gasket_w + 2*core_min_wall()) : wall_t;
assert(!e_seal || core_gasket_fill(gasket_w, gasket_groove, gasket_proud) <= core_gasket_fill_max(),
       str("the printed TPU ring would fill ", round(100*core_gasket_fill(gasket_w, gasket_groove, gasket_proud)),
           " % of its groove - past ", round(100*core_gasket_fill_max()),
           " % the incompressible gasket props the lid open instead of sealing; narrow gasket_w or deepen gasket_groove"));
pd = post_d;  post_corner = pd + 1.5;
clip_stack = clip_clear + clip_t;
v_standoff = v_stack_sock + xiao_below;
s_standoff = s_stack_sock + xiao_below;
// pan-head seat: a 2.0 seat in a 2.0 front is a through-hole, so the front
// carries a pad under each head and the posts shorten by the same
e_back   = screw_from == "back";
head_pad = e_back ? 0 : max(0, screw_head_h + 1.0 - lid_t);

col_v = max(cam_w + 2*board_clear, vm_w + 2*(clip_stack + board_clear) + 0.5);
col_s = sm_w + 2*(clip_stack + board_clear) + 0.5;
inner_x = col_v + 3 + col_s + 2*post_corner;
inner_y = max(board_clear + vm_l + 2 + cam_h + 3, board_clear + sm_l + 8);
cav_d = max(v_standoff + pcb_t + v_front_h, s_standoff + pcb_t + s_front_h,
            v_standoff + pcb_t + port_usbc_shell_h()/2 + usb_h/2 + 0.5) + cav_extra;   // keep the Vision USB opening fully inside the wall

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
// USB openings center on the connector AXIS (shell/2 above the board): the
// module's port on top of the module, the two XIAO ports hanging off the XIAO
// faces stack_sock below each carrier
v_usb_zc = floor_t + v_standoff + pcb_t + port_usbc_shell_h()/2;
v_usb_lo = floor_t + v_standoff - v_stack_sock - port_usbc_shell_h()/2;
s_usb_zc = floor_t + s_standoff - s_stack_sock - port_usbc_shell_h()/2 + s_usb_z;
assert(v_usb_zc - v_usb_lo >= usb_h + 1.2, "the Vision module and XIAO USB openings merge — no web between them");
assert(min(v_usb_lo, s_usb_zc) - usb_h/2 >= floor_t + 0.6, "a XIAO USB opening breaches the floor — raise xiao_below");
rad_gap = cav_d - s_standoff - pcb_t - ant_h + (lid_t - radome_t);  // true gap even when the Vision stack sets cav_d

mount_extra = opt_mount ? kh_extra : 0;
// screws from the back (canary_core_lib bk_*): length, the head's recess and
// the boss under the front, derived together; face_skin stays over the tip
face_skin = 1.0;
bk_L      = e_back ? bk_len("m2", "pan", mount_extra, base_d, lid_t, face_skin) : 0;
bk_r      = e_back ? bk_recess("m2", "pan", mount_extra, floor_t, base_d, lid_t, face_skin) : 0;
boss_h    = e_back ? bk_boss_h("m2", "pan", mount_extra, floor_t, base_d, lid_t, face_skin) : 0;
post_h    = cav_d - head_pad - boss_h;
assert(!e_back || post_h >= 2.0, "screws from the back: the boss leaves too short a post");
skirt_gap = tol_slide + 0.2;
plate_x = e_seal ? out_x + 2*(skirt_gap + skirt_t) : out_x;
plate_y = e_seal ? out_y + 2*(skirt_gap + skirt_t) : out_y;
plate_r = e_seal ? corner_r + skirt_gap + skirt_t : corner_r;

// corner posts stand 0.2 INTO both walls (the catalog's post seat — the
// pan-head pad is cropped to the cavity for exactly this); at -0.2 they stood
// 0.2 off both walls, tied in only by the gussets
function post_xy() = [
    [ inner_x/2 - pd/2 + 0.2,  inner_y/2 - pd/2 + 0.2],
    [-inner_x/2 + pd/2 - 0.2,  inner_y/2 - pd/2 + 0.2],
    [ inner_x/2 - pd/2 + 0.2, -inner_y/2 + pd/2 - 0.2],
    [-inner_x/2 + pd/2 - 0.2, -inner_y/2 + pd/2 - 0.2],
];
// the drain: bottom wall (the case hangs +Y up), in the gap between the columns
weep_x = (v_cx + col_v/2 + s_cx - col_s/2)/2;
assert(!opt_weep || (abs(weep_x - v_cx) >= usb_w/2 + weep_d()/2 + 1.0 && abs(weep_x - s_cx) >= usb_w/2 + weep_d()/2 + 1.0),
       "the weep merges with a USB opening");
// a sealed box needs a pressure path or it pumps air past the gasket on every
// thermal cycle (field_ratings.md, WTR-2); this front has no vent seat, so the
// path is the weep
assert(!e_seal || opt_weep, "seal mode with no pressure path — enable opt_weep");
// the USB awning over the Sense port stays under the front's rim (and its
// drip skirt in seal mode); the Vision pair cannot take one — its module
// port's top sits under the rim, and an awning over the XIAO port below it
// would stand in the module plug's overmold
function usb_hood_top() = s_usb_zc + usb_h/2 + 0.5 + usb_hood_reach*1.5;
assert(!usb_hood || usb_hood_top() <= base_d - (e_seal ? skirt_h + 0.4 : 0.4),
       "the USB awning reaches the front's rim — shorten usb_hood_reach");
// the hood's groove leaves a floor, and stays off the clear-disc seat
assert(!opt_hood || hood_seat + 0.8 <= lid_t, "hood_seat leaves under 0.8 mm of front beneath the hood groove");
assert(!opt_hood || cam_disc_d/2 + 2.5 >= cam_disc_d/2 + tol_slide + 0.5 + 1.0,
       "the hood groove runs into the clear-disc seat's lead-in");
// the camera screws: the longest standard length the board + the drawn
// pilot (cam_post_h - 0.9 deep) takes — an M2 x 6 through a 1.0 board runs
// 5.0 into a 3.1 pilot and bottoms before it clamps
cam_pilot = cam_post_h - 0.9;
cam_scr_l = max([for (l = hw_std_lens()) if (l <= pcb_t + cam_pilot + 1e-9) l]);
assert(radome_t >= 0.6 && radome_t < lid_t, "radome_t out of range");
assert(rad_gap >= 3.0, "antenna-to-radome gap < 3 mm — raise cav_extra");
assert(rad_win_x + 2*abs(rad_dx) <= col_s && rad_win_y + 2*abs(rad_dy) <= sm_l,
       "radome window exceeds the sense column — shrink rad_win/rad_dx/rad_dy");
assert(lip_h < cav_d, "lip_h vs cavity");
key_x = inner_x/2 - post_corner - 2.5;   // lid key: +Y wall, inboard of the +X corner post
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
// the hardware, DERIVED from the same knobs that draw the holes (canary_core_lib)
hw_echo("Combo witness", [
    e_back ? hw_item(len(post_xy()), str(hw_screw("m2", "pan", bk_L, "self-tap"), " from the back"))
           : hw_item(len(post_xy()), hw_screw("m2", "pan", hw_len(lid_t, head_pad, 6), "self-tap")),
    hw_item(4, str(hw_screw("m2", "pan", cam_scr_l, "self-tap"), " (OV5647 to the front posts)")),
    e_seal ? hw_item(1, "TPU gasket (print part=\"gasket\")") : "",
    opt_hood ? hw_item(1, "hood (print part=\"hood\"; bond into the front's groove)") : "",
    opt_led ? hw_item(1, str("Ø", lp_d, " light pipe")) : "",
    opt_mount ? hw_item(2, "#6 pan wall screw (keyholes)") : "",
]);
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
module rails_half(cx, cy, w, l, soff) {   // rails beside the camera-end half only
    for (s = [1, -1]) {
        rail_l = l/2 - 4;
        difference() {
            translate([cx + s*(w/2 - 1.5) - 1.5, cy + 2, floor_t]) cube([3, rail_l, soff]);
            translate([cx + s*(w/2 - 1.5), cy + 2 + rail_l/2, floor_t + soff/2]) cube([5, clip_w + 2, soff + 1], center = true);
        }
        edgeclip(cx + s*w/2, cy + 2 + rail_l/2, s > 0 ? 0 : 180, soff);
    }
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

// screws from the back: seats + bores cut through the whole back, and the
// bosses they thread into hanging from the front onto the post tops
module back() {
    difference() {
        back_body();
        if (e_back) for (p = post_xy())
            bk_seat_cut(p[0], p[1], mount_extra, floor_t + post_h, "m2", "pan", bk_r,
                        screw_d + 2*tol_hole, tol_hole);
    }
}
module front() {
    difference() {
        union() {
            front_body();
            if (e_back) for (p = post_xy())
                cb_head_pad(p[0], p[1], boss_h, pd, inner_x, inner_y, core_cav_r(corner_r, wall_eff));
        }
        if (e_back) for (p = post_xy())
            bk_boss_bore(p[0], p[1], boss_h, lid_t, face_skin, screw_d);
    }
}

module back_body() {
    posts = post_xy();
    union() {
        difference() {
            union() {
                rrect(out_x, out_y, corner_r, base_d);
                if (mount_extra > 0) translate([0, 0, -mount_extra]) rrect(out_x, out_y, corner_r, mount_extra);
            }
            translate([0, 0, floor_t])   // the cavity, floor cove left standing (canary_core_lib)
                cavity_cut(inner_x, inner_y, max(0.1, corner_r - wall_eff), cav_d + 1, floor_cove);
            // Vision stack: module port + XIAO port (stacked); Sense stack: C6
            // port. All three: 45°-chamfered top corners halve the unsupported
            // bridge in the upright-printed wall and keep any droop out of the
            // plug envelope — canary_port_lib (the WAP's print-validated
            // profile; these walls used to bridge a flat top)
            for (pz = [[v_cx, v_usb_zc], [v_cx, v_usb_lo], [s_cx, s_usb_zc]])
                translate([pz[0], -out_y/2 + wall_eff*1.5, pz[1]])
                    rotate([90, 0, 0]) linear_extrude(wall_eff*3)
                        port_bridge_profile2d(usb_w, usb_h);
            if (e_seal)
                translate([0, 0, base_d - gasket_groove]) linear_extrude(gasket_groove + 1) rim_ring2d(gasket_w);
            if (mount_extra > 0) { keyhole_pocket(-inner_x/4); keyhole_pocket(inner_x/4); }
            // drain at the low point: the bottom wall's floor corner, angled
            // down and out (canary_core_lib weep_cut)
            if (opt_weep)
                weep_cut(weep_x, -inner_y/2, floor_t + weep_d()/2 + 0.2, "-y", wall_eff, weep_d());
        }
        // drip awning over the Sense USB opening, printed with the wall (its
        // underside rises at 45°; the wall frame's up is the front side)
        if (usb_hood)
            translate([s_cx, -out_y/2, s_usb_zc]) rotate([90, 0, 0])
                port_hood(usb_w, usb_h, usb_hood_reach, usb_h/2);
        difference() {
            union() {
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = post_h);
                // the corner behind each post filled solid into both walls: a
                // round post set into a square (or tight-radius) corner leaves
                // a closed sliver there, a crevice that traps dirt and prints
                // as a pinhole column
                for (p = posts) let (sx = sign(p[0]), sy = sign(p[1]))
                    translate([min(p[0], sx*(inner_x/2 + 0.3)), min(p[1], sy*(inner_y/2 + 0.3)), floor_t])
                        cube([abs(sx*(inner_x/2 + 0.3) - p[0]), abs(sy*(inner_y/2 + 0.3) - p[1]), post_h]);
                // constant-width webs (canary_rib_lib corner_gusset) — no hull flare
                for (p = posts) translate([0, 0, floor_t]) {
                    sx = sign(p[0]); sy = sign(p[1]);
                    gw = min(2.0, rib_t_max(wall_eff));
                    corner_gusset(p[0], p[1], sx*(inner_x/2 + 0.5), p[1], min(cav_d - lip_h - 1, post_h - 0.5), wall_eff, pd, gw);
                    corner_gusset(p[0], p[1], p[0], sy*(inner_y/2 + 0.5), min(cav_d - lip_h - 1, post_h - 0.5), wall_eff, pd, gw);
                }
            }
            if (!e_back) for (p = posts) translate([p[0], p[1], floor_t + 2]) cylinder(d = screw_d, h = cav_d);
        }
        // lid key (canary_core_lib): a rib on the +Y wall inside the lip zone
        if (lid_key) lid_key_rib(key_x, inner_y/2, 270, base_d, lip_h);
        // Vision column: rails on the TOP HALF only — the stacked XIAO (17.8 wide
        // under the 20 mm module) hangs beneath the lower half, so full-length
        // rails ran straight through it (the Vision case's fix, ported); two
        // corner pins catch the module's lower edge outboard of the XIAO
        rails_half(v_cx, vm_cy, vm_w, vm_l, v_standoff);
        for (s = [1, -1])
            // 0.3 off the XIAO's edge (at + 1.1 it was 0.1 — less than a Ø2 pin prints oversize)
            translate([v_cx + s*(xiao_w/2 + 1.3), vm_cy - vm_l/2 + 1.2, floor_t])
                cylinder(d = 2.0, h = v_standoff);
        // Sense column: the 36 mm carrier overhangs its 17.8 XIAO by 9 a side —
        // full-length rails at the carrier's edges clear it
        rails(s_cx, sm_cy, sm_w, sm_l, s_standoff);
    }
}

module front_body() {
    union() {
        difference() {
            union() {
                // the plate with the catalog's two-stage soft edge — canary_core_lib
                // (this also wires up the previously inert lid_edge2 knob)
                soft_edge_plate(plate_x, plate_y, plate_r, lid_t, lid_edge, lid_edge2);
                if (head_pad > 0) for (p = post_xy())   // the floor under each pan head
                // pan-head pads: the floor under each head the front cannot
                // spare — CROPPED to the cavity (canary_core_lib). Drawn as a
                // bare cylinder the pad overhung its post and landed on the
                // shell wall rim, holding the front proud so the lip never
                // entered the cavity and the screws clamped nothing.
                    cb_head_pad(p[0], p[1], head_pad,
                                cb_pad_d(screw_head_d, tol_hole),
                                inner_x, inner_y, core_cav_r(corner_r, wall_eff),
                                screw_head_d + 2*tol_hole);
            }
            // the hood's seat: a hood_seat-deep groove in the show face on the
            // collar's footprint (the hood is its own part)
            if (opt_hood)
                translate([lens_x, lens_y, lid_t - hood_seat]) linear_extrude(hood_seat + 1) hood_ring2d();
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
            if (!e_back) for (p = post_xy()) translate([0, 0, -head_pad])
                cb_flat_cut(p[0], p[1], lid_t + head_pad, screw_d + 2*tol_hole,
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
            lip_ring(inner_x - 2*tol_slide, inner_y - 2*tol_slide, max(0.1, corner_r - wall_eff - tol_slide), lip_h, lip_t);
            for (p = post_xy()) translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
            for (cx = [v_cx, s_cx]) translate([cx, -inner_y/2, -lip_h/2]) cube([usb_w + 4, lip_t*4, lip_h + 0.2], center = true);
            if (lid_key) lid_key_slot(key_x, inner_y/2, 270, lip_h, lip_t);
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

// the rain hood — its OWN part (the Vision's PRT-1): grown on the front it
// stood 9 mm off the show face, so face-down the front stood on the hood and
// face-up its whole inner face was a ceiling. A C-ring, open at the bottom so
// water drains, pressed into the front's groove at tol_press and bonded; it
// prints drip-edge-down, spigot up — an extrusion with no overhang
module hood_ring2d(inset = 0) {
    difference() {
        circle(d = cam_disc_d + 5 + 2*hood_t - 2*inset);
        circle(d = cam_disc_d + 5 + 2*inset);   // +5 (was +3): the Vision's margin that keeps
                                                // the OV5647's diagonal FOV off the hood
        translate([-(cam_disc_d/2 + hood_t + 3), -2*(cam_disc_d + hood_t)])
            square([cam_disc_d + 2*hood_t + 6, 2*(cam_disc_d + hood_t) - cam_disc_d*0.18 + inset]);
    }
}
module hood() {
    assert(opt_hood, "the hood needs opt_hood=true so the front carries its groove");
    union() {
        linear_extrude(hood_len) hood_ring2d();
        // spigot: 0.1 shy of the groove's floor so the collar's root seats on the face
        translate([0, 0, -(hood_seat - 0.1)]) linear_extrude(hood_seat - 0.1 + 0.01) hood_ring2d(tol_press);
    }
}

if      (part == "back")   back();
else if (part == "front")  translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
else if (part == "gasket") { assert(e_seal, "gasket needs opt_seal"); gasket(); }
else if (part == "hood")   translate([0, 0, hood_len]) rotate([180, 0, 0]) hood();   // drip edge on the bed, spigot up
else {
    back();
    translate([0, -(out_y/2 + plate_y/2 + 12), 0]) translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
    if (e_seal) translate([-(out_x + 16), 0, 0]) gasket();
    if (opt_hood) translate([out_x/2 + 30, 0, hood_len]) rotate([180, 0, 0]) hood();
}
