// ============================================================================
//  Canary — RADAR + CAMERA COMBO WITNESS  ⚠️ IN DEVELOPMENT (v0.2-dev)
//  One housing, two stacks: the Vision build (OV5647 + Grove Vision AI V2 +
//  stacked XIAO) beside the Sense build (MR60BHA2 + stacked XIAO C6).
//  Radar-confirmed camera events kill false positives — the classic
//  commercial-camera trick, here as two independent signed witnesses that
//  corroborate each other. Front face: lens + disc seat on one column,
//  RADOME window on the other (all radome rules apply — see the Sense case).
//  Three USB-C openings exit the bottom wall: the Vision module's port, the
//  XIAO under it, and the Sense stack's C6.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. This is a
//  geometry merge of two proven cases, but the combined layout is untested.
//
//  THE PISTON PLATE (canary_core_lib pl_*). The SHELL is one piece — face,
//  walls and posts — printed face-down as a cup; its walls run all the way
//  down to the back face. The PLATE nests inside the walls like a piston,
//  seats on a ledge the walls carry at z = floor_t, and is the chassis: the
//  rails, clips, pins and keyholes live on it, so service is the plate's
//  screws and both stacks come out together. The only seam is a hairline on
//  the back face. Short screws go through the plate (head recessed; an
//  O-ring gland under a pan head in seal mode) into blind pilots in the post
//  ends, which stop pl_relief() short of the ledge: the ledge is the datum.
//
//      face (z = base_d .. base_d + lid_t)        prints face-DOWN
//      |  post  post  ... posts hang from the face to pl_relief() above the ledge
//      |  cavity (cav_d), coved at the face and at the ledge
//      +-- ledge plane z = floor_t: the plate's front face seats here (gasket groove in seal mode)
//      |  bore (inner + 2*ledge_w): the plate's piston fit, tol_slide a side
//      +-- back face z = -mount_extra = floor_t - plate_t: the hairline seam
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
//  2026-09-24: ported to the PISTON PLATE (v0.2-dev; the Sense is the
//              reference). The front lid + back shell with a lip, the
//              screws-from-the-back bosses (bk_*), the drip skirt and the
//              side seam are gone: lip_h/lip_t, screw_from, skirt_h/skirt_t
//              removed; screw_size/screw_head added. All three USB openings
//              are cut with their chamfers toward the BACK — the opening's
//              roof in the face-down print. Sealed builds: the gasket clamp
//              span stays under 40 mm on every wall — a full post mid-way
//              along the +Y and both ±X walls, and on the -Y wall (where the
//              Sense carrier sits against the wall) a SHORT wall post under
//              the carrier with a 45° roof; the stacks lift so the lowest port
//              opening keeps a web over the gasket groove's top; the plate's
//              key notch stops short of the gasket's outer cheek. usb_hood
//              keeps port_hood's profile but is posed for the face-down
//              print (its 45° flank toward the bed). Fit gate: check="combo"
//              in canary_case_fitcheck.scad.
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, soft-edge front, the piston plate (pl_*) — the shared idiom
use <canary_mount_lib.scad>  // the stud/keyhole hanging standard — the blind pockets' one home
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_port_lib.scad>   // bridge-safe USB openings (the WAP's print-validated profile)
use <canary_board_lib.scad>  // board registry — the Grove/MR60 numbers the knobs cite
use <canary_rib_lib.scad>    // corner_gusset — the constant-width post web
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)

/* [What to render] */
part = "all";        // ["back","front","all","gasket","hood"]

/* [Options] */
opt_seal = false;    // perimeter TPU gasket in the shell's ledge + O-ring under every plate screw head (same system as the other cases)
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
wall_t = 2.0;  floor_t = 2.0;  lid_t = 2.0;  corner_r = 3.0;
floor_cove = 0.8;  // 45° cove where the ledge meets the walls, inside (canary_core_lib pl_cavity_cut); 0 = the old square corner  // [0:0.2:1.2]
                   // The sharp notch there was the crack-starter in every flat-printed shell — a corner drop
                   // hinges the floor about it along one layer boundary.
lid_key    = true; // poka-yoke: a rib on the +Y bore wall and a notch in the plate's edge — four corner posts fit
                   // a plate two ways and every face feature lines up one way; turned round the plate lands on the rib
tol_slide = 0.20;  // catalog default — core_tol_slide(), canary_core_lib
tol_press = 0.10;  // catalog default — core_tol_press(), canary_core_lib
tol_hole  = 0.30;  // catalog default — core_tol_hole(), canary_core_lib
post_d = 5.0;  screw_d = 1.6;  screw_head_d = 4.0;  screw_head_h = 2.0;
screw_size = "m2";  // ["m2","m2.5","m3"] plate screw — the core lib's registry sets pilot, clearance and head seat; "m2" keeps the validated numbers on the line above
screw_head = "pan"; // ["pan","flat"] pan = flat-floored seat (what a sealed build's O-ring gland needs), flat = 90° countersink
mid_dx = -0.5;      // (seal mode) the bottom wall's short post, X off the case center: between the Sense rail and the seated XIAO's socket header (MEASURE yours); both clamp spans stay under 40
usb_w = 12.0;  // opening width — 12 clears rugged cable boots (the WAP's validated opening)
usb_h = 6.5;   // opening height — boot clearance (the WAP's validated opening)
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;
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

/* [Stud/keyhole interface] — blind keyholes in the plate */
kh_extra   = 3.0;   // plate thickening that hosts the keyhole pockets
kh_head_d  = 7.0;   // screw-head pass hole — catalog standard, mount_kh_head_d(), canary_mount_lib
kh_shank_d = 4.2;   // shank slot width — catalog standard, mount_kh_shank_d()
kh_slot_l  = 8.0;   // slot travel — catalog standard, mount_kh_slot_l()
kh_head_h  = 3.5;   // total pocket depth (face web + head cavity) — catalog standard, mount_kh_head_h()
kh_face    = 1.0;   // face web the screw head grips behind — catalog standard, mount_kh_face()
opt_mount = true;    // blind keyholes in the plate

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
// the piston plate (canary_core_lib pl_*): the plate seats on a ledge inside
// the walls and the parting line is the back face itself
ledge_w  = pl_ledge(e_seal, gasket_w, tol_slide);                   // gasket + a cheek each side, or one contact band
wall_eff = max(wall_t, ledge_w + core_min_wall());       // the skin outside the plate's bore stays a structural wall
assert(!e_seal || core_gasket_fill(gasket_w, gasket_groove, gasket_proud) <= core_gasket_fill_max(),
       str("the printed TPU ring would fill ", round(100*core_gasket_fill(gasket_w, gasket_groove, gasket_proud)),
           " % of its groove - past ", round(100*core_gasket_fill_max()),
           " % the incompressible gasket props the lid open instead of sealing; narrow gasket_w or deepen gasket_groove"));
scr_d   = (screw_size == "m2") ? screw_d : scr_pilot(screw_size);
scr_c   = max(scr_d + 2*tol_hole, scr_clear(screw_size));
head_d  = (screw_size == "m2" && screw_head == "pan") ? screw_head_d
        : (screw_head == "pan") ? scr_pan_d(screw_size) : scr_flat_d(screw_size);
head_h  = (screw_size == "m2" && screw_head == "pan") ? screw_head_h
        : (screw_head == "pan") ? scr_pan_h(screw_size) : scr_flat_h(screw_size);
pd = max(post_d, scr_post_min(screw_size));  post_corner = pd + 1.5;
clip_stack = clip_clear + clip_t;
// a sealed build seats an O-ring under every plate screw head: the seat is a
// hole through the seal line from outside (canary_core_lib pl_seat_cut)
e_gland  = e_seal;
// sealed builds break every wall's gasket clamp span under 40 mm with a post
// mid-way along it (DESIGN_RULES §6); the -Y wall's is a SHORT post (below)
mid_posts = e_seal;
// the lowest port opening keeps a web under it: over the ledge plane, or in
// seal mode over the gasket groove's TOP — an opening cut into the groove
// exposes the ring's outer flank to the plug. Both stacks lift by what that
// takes (0 unsealed: the layout is the validated one)
port_web   = e_seal ? gasket_groove + 0.4 : 0.6;
xiao_axis0 = xiao_below - port_usbc_shell_h()/2;       // a stacked XIAO's port axis above the floor, unlifted
stack_lift = max(0, port_web - (min(xiao_axis0, xiao_axis0 + s_usb_z) - usb_h/2));
v_standoff = v_stack_sock + xiao_below + stack_lift;
s_standoff = s_stack_sock + xiao_below + stack_lift;

col_v = max(cam_w + 2*board_clear, vm_w + 2*(clip_stack + board_clear) + 0.5);
col_s = sm_w + 2*(clip_stack + board_clear) + 0.5;
inner_x = col_v + 3 + col_s + 2*post_corner;
inner_y = max(board_clear + vm_l + 2 + cam_h + 3, board_clear + sm_l + 8);
cav_d = max(v_standoff + pcb_t + v_front_h, s_standoff + pcb_t + s_front_h,
            v_standoff + pcb_t + port_usbc_shell_h()/2 + usb_h/2 + 0.5) + cav_extra;   // keep the Vision USB opening fully inside the wall

out_x = inner_x + 2*wall_eff;
out_y = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;
// ASSEMBLED-FIT PROBE for canary_case_fitcheck.scad (check="combo"). It lives
// HERE, next to the geometry, so it reads this file's own derived datum, and
// its name is unique — every case calls its halves front()/back().
//
// MUST RENDER EMPTY. `lift` separates intended face-on-face contact from real
// interference (coplanar faces intersect to a zero-volume patch CGAL reports
// as non-2-manifold — a dirty render, not a pass). `turned` seats the shell
// rotated 180° about Z — the poka-yoke CONTROL: with lid_key on this must NOT
// be empty (the plate's edge lands on the key rib).
module combo_fitcheck(lift = 0.1, turned = false) {
    intersection() { translate([0, 0, base_d + lift]) rotate([0, 0, turned ? 180 : 0]) front(); back(); }
}

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
assert(min(v_usb_lo, s_usb_zc) - usb_h/2 >= floor_t + port_web - 1e-9,
       "a XIAO USB opening breaches the ledge (or the gasket groove) — raise xiao_below");
rad_gap = cav_d - s_standoff - pcb_t - ant_h + (lid_t - radome_t);  // true gap even when the Vision stack sets cav_d

mount_extra0 = opt_mount ? kh_extra : 0;
// the plate (canary_core_lib pl_*): never thinner than a head and its floor;
// the head recesses as far as that floor allows; the screw is the shortest
// standard length that engages hw_engage() in the post end past the relief
plate_t  = pl_thick(floor_t, mount_extra0, screw_size, screw_head, e_gland);
mount_extra = plate_t - floor_t;                   // the plate below z = 0 (the keyhole slab, or the head's floor)
pl_r     = pl_recess(plate_t, screw_size, screw_head, e_gland);
pl_L     = pl_len(plate_t, pl_r, screw_size, screw_head);
pl_eng   = pl_engage(plate_t, pl_r, screw_size, screw_head);
pl_pil   = pl_pilot(plate_t, pl_r, screw_size, screw_head);
post_h   = cav_d - pl_relief();                    // face underside -> the relief over the ledge plane
shell_d  = cav_d + plate_t;                        // the walls, face underside -> the back face
bore_x   = inner_x + 2*ledge_w;  bore_y = inner_y + 2*ledge_w;
bore_r   = core_cav_r(corner_r, wall_eff) + ledge_w;
plate_x  = bore_x - 2*tol_slide;  plate_y = bore_y - 2*tol_slide;  plate_r = max(bore_r - tol_slide, 0.4);
assert(!e_gland || screw_head == "pan", "a sealed build seats an O-ring under each plate screw head — that needs screw_head = \"pan\"");
assert(pl_pil + 1.0 <= post_h, str("the post is too short for its pilot (", pl_pil, " into ", post_h, " mm)"));
assert(head_d > scr_c, "the screw head must be larger than its clearance hole, or it falls through the plate");
assert(mount_extra0 == 0 || kh_head_h + 1.5 <= plate_t, "keyhole pocket too deep for the plate");

// POSTS. Corner posts stand 0.2 INTO both walls (the catalog's post seat).
// Sealed builds add a full post mid-way along the +Y wall and each ±X wall,
// 0.2 into its own wall like the corners. The -Y wall's mid post cannot hang
// from the face — the Sense carrier sits board_clear off that wall with its
// XIAO beneath it — so it is a SHORT wall post: a boss half-buried in the
// wall, from the relief up just far enough to hold the pilot and a 1.0 skin,
// capped by a 45° roof back to the wall (face-down, that roof is the
// overhang). Its pilot sits 0.3 into the wall, 0.9 clear of the gasket
// groove's inner cheek.
function corner_xy() = [
    [ inner_x/2 - pd/2 + 0.2,  inner_y/2 - pd/2 + 0.2],
    [-inner_x/2 + pd/2 - 0.2,  inner_y/2 - pd/2 + 0.2],
    [ inner_x/2 - pd/2 + 0.2, -inner_y/2 + pd/2 - 0.2],
    [-inner_x/2 + pd/2 - 0.2, -inner_y/2 + pd/2 - 0.2],
];
function midx_xy() = mid_posts ? [[ inner_x/2 - pd/2 + 0.2, 0], [-inner_x/2 + pd/2 - 0.2, 0]] : [];
function midy_xy() = mid_posts ? [[0, inner_y/2 - pd/2 + 0.2]] : [];
wp_y     = -inner_y/2 + 0.5;                       // the wall post's center: 0.5 inside the -Y cavity wall
wp_reach = wp_y + pd/2 + inner_y/2;                // how far it stands into the cavity
wp_h     = pl_pil + 1.0;                           // its pilot plus the skin over the tip
function wall_xy() = mid_posts ? [[mid_dx, wp_y]] : [];
function post_xy() = concat(corner_xy(), midx_xy(), midy_xy(), wall_xy());
// the roof climbs at 45° from the wall post's far edge to the wall; where the
// carrier's edge sits (board_clear off the wall) it must stay under the board
assert(!mid_posts || floor_t + pl_relief() + wp_h + wp_reach - board_clear <= floor_t + s_standoff - 0.5,
       "the -Y wall post's roof reaches the Sense carrier — raise xiao_below");
// the gasket's clamp spans, post to post along each wall (DESIGN_RULES §6)
function _sp(a, b) = norm(a - b);
clamp_spans = !mid_posts ? [] :
    let (c = corner_xy(), mx = midx_xy(), my = midy_xy()[0], w = wall_xy()[0])
    [_sp(c[1], my), _sp(my, c[0]),                        // +Y wall
     _sp(c[3], w),  _sp(w, c[2]),                         // -Y wall
     _sp(c[0], mx[0]), _sp(mx[0], c[2]),                  // +X wall
     _sp(c[1], mx[1]), _sp(mx[1], c[3])];                 // -X wall
assert(!mid_posts || max(clamp_spans) <= 40.0 + 1e-9,
       str("a gasket clamp span exceeds 40 mm: ", clamp_spans, " — move mid_dx toward the center"));
// the drain: bottom wall (the case hangs +Y up), in the gap between the columns
weep_x = (v_cx + col_v/2 + s_cx - col_s/2)/2;
assert(!opt_weep || (abs(weep_x - v_cx) >= usb_w/2 + weep_d()/2 + 1.0 && abs(weep_x - s_cx) >= usb_w/2 + weep_d()/2 + 1.0),
       "the weep merges with a USB opening");
assert(!mid_posts || abs(weep_x - mid_dx) >= pd/2 + weep_d()/2 + 1.0, "the weep runs into the -Y wall post");
// a sealed box needs a pressure path or it pumps air past the gasket on every
// thermal cycle (field_ratings.md, WTR-2); this front has no vent seat, so the
// path is the weep
assert(!e_seal || opt_weep, "seal mode with no pressure path — enable opt_weep");
// the USB awning over the Sense port stays under the face's edge; the Vision
// pair cannot take one — its module port's top sits under the face, and an
// awning over the XIAO port below it would stand in the module plug's overmold
function usb_hood_top() = s_usb_zc + usb_h/2 + 0.5 + usb_hood_reach*1.5;
assert(!usb_hood || usb_hood_top() <= base_d - 0.4,
       "the USB awning reaches the face's edge — shorten usb_hood_reach");
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
assert(!e_seal || gasket_groove + 0.5 <= cav_d, "gasket_groove runs out of the ledge");
key_x = inner_x/2 - post_corner - 2.5;   // plate key: on the +Y bore wall, inboard of the +X corner post
// the plate's key notch reaches 0.3 past the rib's tip and no further: in seal
// mode the gasket's outer cheek (core_min_wall) is all the plate has outboard
// of the ring, and a notch into the groove's footprint leaves the ring unbacked
assert(!e_seal || core_key_d() + 0.3 <= core_min_wall(), "the key notch reaches the gasket groove");
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
assert(!opt_mark || mark_word_ink_w("securaCV", mark_size) <= out_x - 4.0,
       str("the wordmark draws ", mark_word_ink_w("securaCV", mark_size),
           " mm at mark_size ", mark_size, " on a ", out_x,
           " mm face (2 mm margin per side) — shrink mark_size"));
// the hardware, DERIVED from the same knobs that draw the holes (canary_core_lib)
hw_echo("Combo witness", [
    hw_item(len(post_xy()), str(hw_screw(screw_size, screw_head, pl_L, "self-tap"), " (plate to the post ends)")),
    e_gland ? hw_item(len(post_xy()), hw_oring(screw_size)) : "",
    hw_item(4, str(hw_screw("m2", "pan", cam_scr_l, "self-tap"), " (OV5647 to the front posts)")),
    e_seal ? hw_item(1, "TPU gasket (print part=\"gasket\")") : "",
    opt_hood ? hw_item(1, "hood (print part=\"hood\"; bond into the front's groove)") : "",
    opt_led ? hw_item(1, str("Ø", lp_d, " light pipe")) : "",
    opt_mount ? hw_item(2, "#6 pan wall screw (keyholes)") : "",
]);
echo(str("Canary COMBO witness v0.2-dev — ", out_x, " x ", out_y, " x ", base_d + lid_t + mount_extra,
         " mm, radar gap ", rad_gap, " mm  (IN DEVELOPMENT; plate ", plate_t, ", seal=", e_seal, ")"));
if (mid_posts) echo(str("gasket clamp spans (mm, +Y/-Y/+X/-X walls): ", clamp_spans));

// rrect2d/rrect come from canary_core_lib; only file-specific geometry stays local
// a ring of width w centered on the ledge band (the gasket's home)
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + ledge_w, inner_y + ledge_w, core_cav_r(corner_r, wall_eff) + ledge_w/2);
        offset(r = -w/2) rrect2d(inner_x + ledge_w, inner_y + ledge_w, core_cav_r(corner_r, wall_eff) + ledge_w/2);
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
            translate([cx + s*(w/2 - 1.5) - 1.5, cy + 2, floor_t - 0.01]) cube([3, rail_l, soff + 0.01]);
            translate([cx + s*(w/2 - 1.5), cy + 2 + rail_l/2, floor_t + soff/2]) cube([5, clip_w + 2, soff + 1], center = true);
        }
        edgeclip(cx + s*w/2, cy + 2 + rail_l/2, s > 0 ? 0 : 180, soff);
    }
}
// full-length rails; y0 trims their -Y ends (the -Y wall post stands where
// the Sense rail's end was)
module rails(cx, cy, w, l, soff, y0 = -1e9) {
    for (s = [1, -1]) {
        ya = max(cy - (l - 1)/2, y0);
        difference() {
            translate([cx + s*(w/2 - 1.5) - 1.5, ya, floor_t - 0.01]) cube([3, cy + (l - 1)/2 - ya, soff + 0.01]);
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

// ----------------------------------------------------------------------------
//  The PLATE (part="back") — the chassis: the floor with its slab, both
//  columns' rails, clips and pins, the keyholes, and the seats the screws
//  enter through. Drawn in the assembled frame: front face at z = floor_t,
//  body down to -mount_extra. Prints back-face down.
// ----------------------------------------------------------------------------
module back() {
    difference() {
        union() {
            translate([0, 0, floor_t]) pl_plate(plate_x, plate_y, plate_r, plate_t);
            // Vision column: rails on the TOP HALF only — the stacked XIAO (17.8 wide
            // under the 20 mm module) hangs beneath the lower half, so full-length
            // rails ran straight through it (the Vision case's fix, ported); two
            // corner pins catch the module's lower edge outboard of the XIAO
            rails_half(v_cx, vm_cy, vm_w, vm_l, v_standoff);
            for (s = [1, -1])
                // 0.3 off the XIAO's edge (at + 1.1 it was 0.1 — less than a Ø2 pin prints oversize)
                translate([v_cx + s*(xiao_w/2 + 1.3), vm_cy - vm_l/2 + 1.2, floor_t - 0.01])
                    cylinder(d = 2.0, h = v_standoff + 0.01);
            // Sense column: the 36 mm carrier overhangs its 17.8 XIAO by 9 a side —
            // full-length rails at the carrier's edges clear it; in seal mode their
            // -Y ends stop 0.5 short of the wall post
            rails(s_cx, sm_cy, sm_w, sm_l, s_standoff, mid_posts ? wp_y + pd/2 + 0.5 : -1e9);
        }
        // the screws: seats through the plate, pan heads over their glands in seal mode
        for (p = post_xy())
            pl_seat_cut(p[0], p[1], floor_t, plate_t, screw_size, screw_head, pl_r, scr_c, tol_hole, e_gland);
        // the key notch in the plate's edge (the rib is on the shell's bore wall):
        // core_key_d() + 0.3 deep from the bore wall
        if (lid_key) translate([0, 0, floor_t]) lid_key_slot(key_x, bore_y/2, 270, plate_t + 0.2, core_key_d() - 0.7);
        if (mount_extra0 > 0) { keyhole_pocket(-inner_x/4); keyhole_pocket(inner_x/4); }
    }
}

// ----------------------------------------------------------------------------
//  The SHELL (part="front") — face, walls and posts, one piece: the lens
//  column and the RADOME column on the face, the walls hanging below to the
//  back face. Drawn with the face at z = 0..lid_t; everything below z = 0 is
//  inside or the walls. Prints face-down.
// ----------------------------------------------------------------------------
module front() { translate([0, 0, -base_d]) shell_asm(); }

// the shell in the ASSEMBLED frame (back face at -mount_extra, face at base_d..base_d + lid_t)
module shell_asm() {
    difference() {
        shell_solid();
        // the posts' blind pilots, up from their end faces — cut LAST, through
        // the posts the union below adds
        for (p = concat(corner_xy(), midx_xy(), midy_xy()))
            pl_post_pilot(p[0], p[1], floor_t + pl_relief(), pl_pil, scr_d);
        // the wall post's pilot is cut from the LEDGE PLANE to the same tip
        // depth: the ledge cove's toe stands 0.3 above the post's end inside
        // the pilot's footprint (the post is only 0.5 off the wall), and the
        // screw's path must be clear from the plate to the pilot
        for (p = wall_xy())
            pl_post_pilot(p[0], p[1], floor_t, pl_pil + pl_relief(), scr_d);
    }
}
module shell_solid() {
    gusset_h = max(2, post_h - 0.5);
    gusset_w = min(2.0, rib_t_max(wall_eff));
    union() {
        difference() {
            union() {
                translate([0, 0, base_d]) soft_edge_plate(out_x, out_y, corner_r, lid_t, lid_edge, lid_edge2);
                translate([0, 0, -mount_extra]) rrect(out_x, out_y, corner_r, shell_d + 0.01);
                // drip awning over the Sense USB opening (canary_core_lib port_hood).
                // The shell prints FACE-DOWN, so the awning's bed-facing flank is the
                // one toward the face: the library's profile is turned end for end
                // and re-seated above the opening, which puts its 45° flank toward
                // the bed and its steeper flank toward the port (no cheeks: turned,
                // they would climb the wall instead of coming down beside the plug)
                if (usb_hood)
                    translate([s_cx, -out_y/2, s_usb_zc]) rotate([90, 0, 0])
                        translate([0, 2*(usb_h/2 + 0.5) + 1.5*usb_hood_reach, 0]) rotate([0, 0, 180])
                            port_hood(usb_w, usb_h, usb_hood_reach, 0);
            }
            // the cavity, coved at the face and at the ledge (canary_core_lib)
            translate([0, 0, base_d]) pl_cavity_cut(inner_x, inner_y, core_cav_r(corner_r, wall_eff), cav_d, floor_cove);
            // the plate's bore below the ledge
            pl_bore_cut(bore_x, bore_y, bore_r, floor_t, plate_t);
            // the gasket groove, cut into the ledge
            if (e_seal)
                translate([0, 0, floor_t - 0.01]) linear_extrude(gasket_groove + 0.01) rim_ring2d(gasket_w);
            translate([0, 0, base_d]) {
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
                // the house wordmark (opt_mark), debossed on the show face by the
                // first-layer machinery — canary_mark_lib owns the word and its
                // metrics, this file only places it (mark_dx/dy/rot/size/depth)
                if (opt_mark)
                    translate([mark_dx, mark_dy, lid_t - mark_depth])
                        linear_extrude(mark_depth + 1) rotate(mark_rot)
                            mark_wordmark(mark_size);
            }
            // Vision stack: module port + XIAO port (stacked); Sense stack: C6
            // port. All three: 45° chamfers on the BACK side of the opening —
            // in the face-down print that is the opening's roof, and the
            // chamfers halve the flat bridge (canary_port_lib)
            for (pz = [[v_cx, v_usb_zc], [v_cx, v_usb_lo], [s_cx, s_usb_zc]])
                translate([pz[0], -out_y/2 + wall_eff*1.5, pz[1]])
                    rotate([90, 0, 0]) linear_extrude(wall_eff*3)
                        mirror([0, 1, 0]) port_bridge_profile2d(usb_w, usb_h);
            // drain at the low point: the bottom wall (hung +Y up: straight out
            // is straight down), its bore a web ABOVE the gasket groove's top —
            // a dive from the ledge plane bores through the groove (canary_core_lib weep_cut)
            if (opt_weep)
                weep_cut(weep_x, -inner_y/2, floor_t + gasket_groove + core_min_web() + weep_d()/2,
                         "-y", wall_eff, weep_d(), tilt = 0);
        }
        // screw posts from the face's underside to the relief over the ledge,
        // gusseted to their walls (a mid-span post only to its own) — the
        // gussets root at the face and taper toward the ledge. A corner post
        // also fills the pocket between its two gussets and the cavity's
        // corner: with the cavity coved at the ledge too, that pocket would
        // otherwise close into a sealed void (the mesh gate counts it a part)
        full = concat(corner_xy(), midx_xy(), midy_xy());
        for (p = full) translate([p[0], p[1], floor_t + pl_relief()]) cylinder(d = pd, h = post_h);
        for (p = full) translate([0, 0, base_d]) mirror([0, 0, 1]) {
            sx = sign(p[0]); sy = sign(p[1]);
            if (sx != 0) corner_gusset(p[0], p[1], sx*(inner_x/2 + 0.5), p[1], gusset_h, wall_eff, pd, gusset_w);
            if (sy != 0) corner_gusset(p[0], p[1], p[0], sy*(inner_y/2 + 0.5), gusset_h, wall_eff, pd, gusset_w);
        }
        for (p = corner_xy())
            translate([min(p[0], sign(p[0])*(inner_x/2 + 0.5)), min(p[1], sign(p[1])*(inner_y/2 + 0.5)), floor_t + pl_relief()])
                cube([inner_x/2 + 0.5 - abs(p[0]), inner_y/2 + 0.5 - abs(p[1]), post_h]);
        // the -Y wall post: the short boss under the Sense carrier, roofed at 45°
        // back into the wall so the face-down print has no flat overhang
        for (p = wall_xy()) {
            translate([p[0], p[1], floor_t + pl_relief()]) cylinder(d = pd, h = wp_h);
            hull() {
                translate([p[0], p[1], floor_t + pl_relief() + wp_h - 0.01]) cylinder(d = pd, h = 0.01);
                translate([p[0] - pd/2, -inner_y/2 - 0.5, floor_t + pl_relief() + wp_h + wp_reach + 0.5 - 0.01])
                    cube([pd, 0.5, 0.01]);
            }
        }
        // the plate key (canary_core_lib): a rib on the +Y bore wall
        if (lid_key) lid_key_rib(key_x, bore_y/2, 270, floor_t, plate_t + 0.5);
        // camera posts, hanging from the face's underside
        for (sx = [1, -1], sy = [1, -1])
            translate([v_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, base_d - cam_post_h])
                difference() {
                    cylinder(d = cam_post_d, h = cam_post_h + 0.1);
                    translate([0, 0, -0.1]) cylinder(d = cam_screw_d, h = cam_post_h - 0.8);
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
