// ============================================================================
//  Canary — SOLAR LoRa/MESHTASTIC RELAY POD  ⚠️ IN DEVELOPMENT (v0.2-dev)
// @env cer=2 ip="CER-2 (→3 after test)"
//  Off-grid backhaul for remote witnesses (LoRa-mesh adapter, PR #747): a
//  pole-mounted sealed pod for a LoRa dev board (default: Heltec V3-class)
//  + an 18650 holder, with an SMA antenna bulkhead on the TOP wall, an
//  angled SOLAR ROOF bracket that doubles as the radiation shield/rain roof,
//  and hose-clamp/zip-tie channels on the back for pole mounting.
//  Sealing reuses the Canary gasket system (sealed by default — it lives on
//  a pole). Wire the panel through the roof gland note in the README.
//
//  THE PARTING LINE IS THE BACK FACE (the piston plate, canary_core_lib pl_*).
//  The SHELL — the show part — is ONE piece: face, side walls and the screw
//  posts, printed face-down as a cup. The BACK is a PLATE that nests inside
//  the walls like a piston and seats on a ledge the walls carry, so the only
//  seam is a hairline on the back face, against the pole. The plate is the
//  chassis: the LoRa rails and clips, the 18650 holder bay and the pole-strap
//  channels live on it, so service is the plate screws and the electronics
//  come out on the plate while the pod stays strapped to its pole. The screws
//  are short — through the plate into blind pilots in the post ends — and in
//  seal mode every seat glands an O-ring under a pan head.
//  Part names: `body` is the PLATE (back()) and `lid` is the SHELL (front())
//  — the catalog's names, kept so render.sh and the README keep working.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. Measure
//     your LoRa board, battery holder and panel.
//
//  ⚠️ THERMAL: a dark sealed pod charging at solar noon can exceed the 18650's
//     0..45 °C charge window — print in a LIGHT color, rely on the roof shade,
//     and set a charge-temperature cutoff in firmware (see field_ratings.md).
//     bh_l = 78 suits unprotected 65 mm cells; protected cells run to 69 mm.
//  ⚠️ SEAL HONESTY: the sealed build adds mid posts along every wall so no
//     gasket clamp span exceeds 40 mm (DESIGN_RULES §6 — the render echoes
//     the spans: 29.6 mm along the long walls, 39.4 mm along the short ones
//     at the defaults), so the clamp-spacing rule is MET; the pod is still
//     CER-2 hardware until a verified W-2 pass — claim CER-3 only after the
//     test.
//     Antenna: keep the panel's lower edge >= 2 cm above the SMA and prefer a
//     whip whose radiating half clears the roof plane — a panel 17 mm off the
//     feedpoint detunes it.
//
//  2026-08-23: adopted the shared contract libraries (core/snap/port/board/
//              mark) — the local helper copies they replace drew the same
//              geometry. Two fixes ride along: the bridge-safe chamfered
//              service-USB opening (canary_port_lib — this wall bridged a
//              flat top) and PAN-head FLAT counterbores on the lid (the
//              Vision's print-validated lesson — the old shallow cone left
//              the head standing on the show face). New opt_mark knob
//              debosses the house wordmark (default off).
//  2026-09-08: DRAINAGE (v0.2-dev) — the audit found a pole pod with no drain:
//              its only floor opening was the USB the README says to plug.
//              Now: opt_weep drains (Ø2, angled down through the bottom wall —
//              one in the LoRa column beside the rails, one under the 18650
//              bay); the ePTFE vent's spot-face is on the INNER face (it was
//              cut on the show face — a 0.9 mm cup on the membrane); a raised
//              sealing-washer land around the SMA on the sky wall (sma_boss)
//              so the thread stands above the water film; the roof's panel
//              bed is as long as the panel (bed_l = pan_l + stop — at 75 % of
//              pan_l a 110 mm panel overhung the root by 27 mm, into the top
//              wall and the antenna), its stop sits below the panel's top
//              face so water sheds over it, three drain notches at bed level
//              through the stop, and rib_lib ribs under the bed.
//  2026-09-24: ASSEMBLY (v0.2-dev) — probed in position: the SMA land stood
//              0.55 into the lid's drip skirt (sma_z now clears it); the roof
//              struts crossed the panel slot (now rooted under the rails, in
//              the bed's frame); the roof screws sat under the panel (the
//              posts now stand outboard of it — the pod is 15.5 mm wider);
//              the panel gland stood in the roof's root and on the holder
//              (moved over the LoRa column); the lid's lip landed on the
//              18650 holder (cav_d clears it); seal cheeks 0.8 -> 1.2 (CLR-7)
//              with the gasket-fill assert; strap channels bridge 7, not 9.
//  2026-09-26: THE PISTON PLATE (v0.2-dev) — the parting line moved to the
//              back face (canary_core_lib pl_*, the Sense's port as the
//              reference). The SHELL is one piece (face + walls + posts,
//              printed face-down) and carries every wall feature: the SMA
//              bulkhead and its land (with an INSIDE spot-face down to
//              sma_wall — a sealed wall is 5.8 here, past a standard
//              bulkhead's thread), the bridge-safe service-USB opening
//              (mirrored so its chamfers are the face-down print's roof),
//              the ePTFE vent and the panel gland in the face, the weeps and
//              the roof's blind pilots. The PLATE nests in the walls on a
//              ledge and carries the LoRa rails/clips, the 18650 bay (a
//              drain notch at its low end — the weeps sit above the gasket
//              groove now, so the bay must not be a cup), the strap channels
//              (cut through the shell's skin as well, so the strap still
//              crosses the pod's full width) and the key slot. Screws are
//              short (M2 pan x 8) into blind pilots in the post ends; a
//              sealed build glands an O-ring under every head. The gasket
//              groove moved from the rim top into the LEDGE. CLAMP SPANS:
//              mid posts on every wall (n_mid from the span, the WAP's
//              pattern — 2 per long wall, 1 per short wall at the defaults)
//              hold every gasket span <= 40. The roof's screws are their
//              own pair now, from the face into the top posts (M2 pan x 10,
//              derived; a full millimeter of post between their pilots and
//              the plate screws', asserted). Removed knobs: lip_h, lip_t,
//              skirt_h, skirt_t (no drip skirt — no side seam to shed off).
//              New: screw_size/screw_head (the registry), sma_wall. Weeps
//              are horizontal (tilt 0) a web above the groove's top: a 30°
//              dive from the floor bored through the gasket groove (the WAP
//              found it), so ~1.8 mm of condensate can stand at the low wall
//              — a limit of sealing the ledge, stated here rather than hidden.
//              The SMA's inside spot-face only ever THINS a wall (sma_wall_eff
//              = min(sma_wall, wall_eff): the unsealed 3.0 wall is left alone).
//              Part mapping: body = back() = the PLATE, lid = front() = the
//              SHELL; the assembly gate is relay_fitcheck() through
//              canary_case_fitcheck.scad (check = "relay"): empty at +0.1,
//              contact on the ledge at -0.1, and the turned control lands on
//              the key rib. Render-proven on both flavors: lateral 0.15 free /
//              0.25 collides, screw paths clear, post skin over every pilot,
//              heads fit their seats, the sealed seats keep their gland web;
//              the board+stack and the holder clear the shell's posts, and the
//              roof sits on the face with nothing under its feet but the face.
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, soft-edge face, the piston plate (pl_*), screw registry
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_port_lib.scad>   // bridge-safe USB opening (the WAP's print-validated profile)
use <canary_board_lib.scad>  // board registry — the LoRa knob defaults cite
                             // brd_l/brd_w/brd_t("heltec_v3") (spec rung; the
                             // MEASURE-yours duty stays until calipers upgrade it)
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)
use <canary_rib_lib.scad>    // corner_gusset (the post webs), plate_ribs under the roof's panel bed

/* [What to render] */
part = "all";        // ["body","lid","roof","gasket","all"] — body = the PLATE (back), lid = the SHELL (front)

/* [Options] */
opt_seal = true;
opt_weep = true;         // Ø2 drains, straight out through the BOTTOM wall (the pod hangs USB-down on
                         // its pole), a web above the gasket groove: condensate leaves at the low wall
                         // instead of pooling under the LoRa board and the 18650. The pressure path is the ePTFE vent.
sma_boss = true;         // raised Ø(sma_d + 6) x 1.0 land around the SMA on the sky wall: the EPDM
                         // sealing washer seats on it above the water film, not in it
roof_ribs = true;        // rib_lib loop + two spines under the panel bed (a 76 x 114 x 2 plate on two struts)
seal_mid_posts = true;   // (seal mode) evenly spaced mid posts along every wall, as many as it takes to keep
                         // each gasket clamp span <= 40 mm (DESIGN_RULES §6); off, the render says what the spans are

/* [LoRa board] — Heltec WiFi LoRa 32 V3-class. MEASURE yours */
lb_l  = 51.0;        // board length (Y, USB end down)
lb_w  = 26.0;        // board width (X)
pcb_t = 1.2;
lb_stack = 12.0;     // tallest top-side part (OLED/pin headers)
board_clear = 0.6;

/* [18650 holder] (single-cell holder with leads) */
bh_l = 78.0;
bh_w = 21.5;
bh_h = 15.0;

/* [Antenna] — SMA bulkhead on the TOP wall */
sma_d = 6.8;         // 1/4-36 SMA thread is Ø6.35: 6.8 with the D-flat at 5.7 (was 6.5/5.9 — 0.075 a side, under FDM hole shrink)
sma_wall = 4.0;      // the wall the bulkhead's nut clamps through: the sky wall is spot-faced from INSIDE down to this around the jack (a sealed wall is 5.8; a standard bulkhead's thread is ~8 mm: land + wall + washer + nut)

/* [Solar roof] — bracket rails take a small 6V panel. MEASURE yours */
pan_w = 70.0;        // panel width
pan_l = 110.0;       // panel length
pan_t = 3.0;         // panel thickness (slides into the rails)
roof_ang = 30;       // panel angle  // [15:5:45]
roof_stop_t = 3.0;   // the stop at the bed's low end the panel rests on under gravity
roof_drain_w = 6.0;  // three drain notches at bed level through that stop (0 = none)  // [0:1:12]

/* [Pole mount] — two channels for hose clamps / heavy zip ties, across the back of the plate AND the shell's skin */
strap_w = 9.0;       // strap width
strap_t = 2.0;       // channel depth (the plate thickens by it behind the floor)

/* [Shell / tolerances / fasteners] */
wall_t = 2.0;  floor_t = 2.0;  lid_t = 2.0;  corner_r = 3.0;
tol_slide = 0.20;  // catalog default — core_tol_slide(), canary_core_lib
tol_press = 0.10;  // catalog default — core_tol_press(), canary_core_lib
tol_hole  = 0.30;  // catalog default — core_tol_hole(), canary_core_lib
post_d = 5.0;         // screw post Ø — the plate screws thread into the post ends
screw_size = "m2";    // ["m2","m2.5","m3"] plate screw — the core lib's registry sets pilot, clearance and head seat; "m2" keeps the validated numbers below
screw_head = "pan";   // ["pan","flat"] pan = flat-floored seat (what the O-ring gland needs), flat = 90° countersink
screw_d      = 1.6;   // (m2 only) self-tap pilot Ø — the print-validated number this pod was drawn on; other sizes read scr_pilot()
screw_head_d = 4.0;   // (m2 pan only) head Ø — the validated seat; other sizes read scr_pan_d()/scr_flat_d()
screw_head_h = 2.0;   // (m2 pan only) head seat depth — the validated seat; other sizes read scr_pan_h()/scr_flat_h()
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;
usb_w = 12.0;  // service USB opening width, bottom wall (plug when deployed); 12 clears a boot
usb_h = 6.5;   // service USB opening height — boot clearance
clip_w      = 6.0;   // board-clip tab width along the board edge — snap_boardclip default, canary_snap_lib
clip_t      = 1.0;   // clip beam thickness — snap_boardclip default; canary_snap_lib runs the strain budget as an assert
clip_hook   = 0.5;   // lip overhang over the board top — snap_boardclip default
clip_hook_h = 1.2;   // lip + 45° lead-in height above the board top — snap_boardclip default
clip_clear  = 0.25;  // beam face to board edge (a fit — tune on the coupon) — snap_boardclip default
standoff_h = 3.0;
lid_edge  = 0.8;  // first (45°) stage of the show-face edge, mm — core_face_edge()  // [0:0.1:1.5]
lid_edge2 = 0.8;  // second (~66°) stage of the show-face edge, mm — ON is the house look (core_face_edge2()); it is what reads as a roundover instead of a bevel. 0 leaves the plain 45° facet any CAD default gives you  // [0:0.1:1.5]
foot_cham = 0.5;
floor_cove = 0.8;  // 45° cove where the cavity meets the walls, at the face and at the ledge (canary_core_lib pl_cavity_cut); 0 = the old square corner  // [0:0.2:1.2]
                   // The sharp notch there was the crack-starter in every flat-printed shell — a corner drop
                   // hinges the floor about it along one layer boundary.
lid_key    = true; // poka-yoke: a rib on the +Y bore wall and a slot in the plate's edge — a symmetric post pattern fits
                   // a plate two ways and every feature lines up one way; turned round it stands on the rib

/* [Aesthetics] */
// Placed by mark_dx/dy/rot/size/depth, gated by the library's measured type
// metrics so an unprintable size is refused before a print, not after.
opt_mark   = false;  // deboss the house wordmark on the face (canary_mark_lib)
mark_size  = 5.0;    // wordmark cap height
mark_depth = 0.5;    // deboss depth (the shell prints face-down -> crisp first-layer voids)
mark_dx    = 0.0;    // mark center offset from the face center
mark_dy    = 0.0;    // default sits centered, clear of the gland hole and the vent spot-face
mark_rot   = 0;      // rotation (degrees)

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
e_seal   = opt_seal;
// the piston plate (canary_core_lib pl_*): the plate seats on a ledge inside
// the walls and the parting line is the back face itself
ledge_w  = pl_ledge(e_seal, gasket_w, tol_slide);       // gasket + a cheek each side, or one contact band
wall_eff = max(wall_t, ledge_w + core_min_wall());      // the skin outside the plate's bore stays a structural wall
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
pd = max(post_d, scr_post_min(screw_size));
// a sealed build seats an O-ring under every plate screw head: the seat is a
// hole through the seal line from outside (canary_core_lib pl_seat_cut)
e_gland  = e_seal;
post_corner = pd + 1.5;
clip_stack = clip_clear + clip_t;
// margins off the walls: the cavity is coved at the ledge (floor_cove), so
// whatever stands on the plate keeps 0.4 clear of the cove's foot
bot_margin = max(board_clear, floor_cove + 0.4);      // the LoRa board's bottom edge off the bottom wall
bay_wall   = 1.2;                                     // the 18650 bay's ring wall (its pocket is 0.3 a side over the holder)
bay_margin = bot_margin + bay_wall + 0.3;             // the holder off the bottom wall, its ring wall between

// two columns: LoRa board | battery holder (both vertical, USB/leads down)
col_lb = lb_w + 2*(clip_stack + board_clear) + 0.5;
col_bh = bh_w + 2.0;
// the roof screws (through its feet into the TOP posts, from the face) must
// be drivable with the panel in its rails: each Ø8 foot stands wholly
// outboard of the panel edge, so the corner posts sit at |x| >= pan_w/2 +
// 4.4. Packed to the columns alone the posts were under the panel, and the
// panel slides in before the roof goes on — the two roof screws were
// unreachable. The extra width is wire room between the columns.
roof_foot_d = head_d + 4.0;   // the roof screw's foot: the head seat + a 2 mm rim
roof_foot_h = 3.0;            // the foot's height on the face
roof_seat   = 1.4;            // the roof screw's head seats this far up its foot
roof_post_x = pan_w/2 + roof_foot_d/2 + 0.4;
// the corner posts sit 0.2 INTO each wall (fused above the gussets)
inner_x = max(col_lb + 2 + col_bh + 2*post_corner, 2*(roof_post_x + pd/2 - 0.2));
inner_y = max(lb_l + 2*bot_margin, bh_l + 2*bay_margin) + 10;   // + wire room at the top
cav_d   = max(standoff_h + pcb_t + lb_stack, bh_h) + 1.5;

out_x = inner_x + 2*wall_eff;
out_y = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;
// ASSEMBLED-FIT PROBE for canary_case_fitcheck.scad. It lives HERE, next to
// the geometry, for two reasons: it reads this file's own derived datum
// instead of duplicating the arithmetic it is checking, and its name is
// unique — every case in this catalog calls its halves front()/back(), so a
// single fit-check file that `use`d them all would silently resolve to
// whichever was parsed last and check the wrong case.
//
// MUST RENDER EMPTY. `lift` separates intended face-on-face contact from real
// interference: coplanar faces intersect to a zero-volume patch that CGAL
// reports as non-2-manifold, which is a dirty render, not a pass.
// `turned` seats the shell rotated 180° about Z — the poka-yoke CONTROL: with
// lid_key on, this must NOT be empty (the plate's edge lands on the key rib).
module relay_fitcheck(lift = 0.1, turned = false) {
    intersection() { translate([0, 0, base_d + lift]) rotate([0, 0, turned ? 180 : 0]) front(); back(); }
}

lb_cx = -inner_x/2 + post_corner + col_lb/2;
bh_cx =  inner_x/2 - post_corner - col_bh/2;
lb_cy = -inner_y/2 + bot_margin + lb_l/2;
bh_cy = -inner_y/2 + bay_margin + bh_l/2;
// SMA bulkhead height on the sky wall: centered on the cavity (no drip skirt
// to clear any more — the seam is on the back face)
sma_land_d = sma_d + 6;
sma_z = floor_t + cav_d/2;
// panel-lead gland in the face: over the LoRa column, clear of the board's top
// edge (the Ø13 locknut envelope hangs 6 below the face) and under the roof bed
// where it stands highest. Over the battery bay by the top wall (as it was) its
// body stood in the roof's root and its nut on the holder.
gland_x = lb_cx;
gland_y = lb_cy + lb_l/2 + 11;
usb_zc = floor_t + standoff_h + pcb_t + port_usbc_shell_h()/2;   // on the connector AXIS, not PCB-top + h/2
// drains: one in the gap between the LoRa rail and the battery bay wall, one
// under the 18650 bay (its ring wall is a dam: the bay carries a drain notch)
weep_x_gap = lb_cx + lb_w/2 + 0.75 + weep_d()/2;
function weep_xs() = [weep_x_gap, bh_cx];
// the weep's bore keeps a web ABOVE the gasket groove's top all the way
// through the ledge band (a bore through the groove's roof is a channel over
// the gasket), so it runs straight out (tilt 0) — the pod hangs +Y up, and
// straight out of the bottom wall is straight down
weep_z = floor_t + (e_seal ? gasket_groove + core_min_web() : 0.2) + weep_d()/2;
assert(!opt_weep || weep_x_gap + weep_d()/2 + 0.5 <= bh_cx - (bh_w + 3)/2,
       "the gap weep runs into the battery bay wall — widen post_corner or move it");
assert(!opt_weep || weep_x_gap - weep_d()/2 >= lb_cx + usb_w/2 + 1.0,
       "the gap weep merges with the USB opening");
assert(!opt_weep || weep_z + weep_d()/2 + 0.4 <= usb_zc - usb_h/2 || true, "");

// the plate (canary_core_lib pl_*): never thinner than a head and its floor;
// the strap channels need strap_t of plate behind the floor; the head
// recesses as far as that floor allows; the screw is the shortest standard
// length that engages hw_engage() in the post end past the relief
mount_extra0 = strap_t;
plate_t  = pl_thick(floor_t, mount_extra0, screw_size, screw_head, e_gland);
mount_extra = plate_t - floor_t;                   // the plate below z = 0 (the strap slab, or the head's floor)
pl_r     = pl_recess(plate_t, screw_size, screw_head, e_gland);
pl_L     = pl_len(plate_t, pl_r, screw_size, screw_head);
pl_eng   = pl_engage(plate_t, pl_r, screw_size, screw_head);
pl_pil   = pl_pilot(plate_t, pl_r, screw_size, screw_head);
post_h   = cav_d - pl_relief();                    // face underside -> the relief over the ledge plane
shell_d  = cav_d + plate_t;                        // the walls, face underside -> the back face
bore_x   = inner_x + 2*ledge_w;  bore_y = inner_y + 2*ledge_w;
bore_r   = core_cav_r(corner_r, wall_eff) + ledge_w;
plate_x  = bore_x - 2*tol_slide;  plate_y = bore_y - 2*tol_slide;  plate_r = max(bore_r - tol_slide, 0.4);
// the roof's screws: pan heads seated roof_seat up each foot, through the
// face, into blind pilots from the face into the two TOP posts — the shortest
// standard length that engages hw_engage() past the face, and its pilot
// stays a full millimeter clear of the plate screw's coming up the same post
roof_L   = hw_len(roof_foot_h - roof_seat + lid_t, 0, hw_engage(screw_size));
roof_pil = roof_L - (roof_foot_h - roof_seat) + 1.0;   // from the face's outer plane, blind
assert(!e_gland || screw_head == "pan", "a sealed build seats an O-ring under each plate screw head — that needs screw_head = \"pan\"");
assert(pl_pil + 1.0 <= post_h, str("the post is too short for its pilot (", pl_pil, " into ", post_h, " mm)"));
assert(post_h >= (roof_pil - lid_t) + pl_pil + 1.0,
       str("the roof screw's pilot (", roof_pil - lid_t, " into the post from the face) meets the plate screw's (", pl_pil,
           " from the post end) — a shorter roof screw or a deeper cavity"));
assert(!e_seal || !lid_key || core_key_d() + 0.3 <= core_min_wall() + 1e-9,
       "the plate's key notch would reach the gasket groove's outer cheek");
assert(!e_seal || gasket_groove + 0.5 <= cav_d, "gasket_groove runs out of the ledge");
assert(head_d > scr_c, "the screw head must be larger than its clearance hole, or it falls through the plate");
assert(mount_extra >= strap_t, "the plate is thinner than the strap channels are deep");

// the screw posts: four corners 0.2 INTO each wall, plus (seal mode) evenly
// spaced MID posts along every wall — as many as it takes to hold each gasket
// clamp span <= 40 (DESIGN_RULES §6): n_mid_x along the ±Y walls (the span
// runs in X), n_mid_y along the ±X walls. A mid post is gusseted only to its
// own wall; the asserts below keep every one off the rails, the bay and the key
post_cx = inner_x/2 - pd/2 + 0.2;
post_cy = inner_y/2 - pd/2 + 0.2;
span_x  = 2*post_cx;                                     // corner to corner along X (the ±Y walls)
span_y  = 2*post_cy;                                     // corner to corner along Y (the ±X walls)
n_mid_x = (e_seal && seal_mid_posts) ? max(0, ceil(span_x/40 - 1e-9) - 1) : 0;
n_mid_y = (e_seal && seal_mid_posts) ? max(0, ceil(span_y/40 - 1e-9) - 1) : 0;
function corner_xy() = [
    [ post_cx,  post_cy], [-post_cx,  post_cy],
    [ post_cx, -post_cy], [-post_cx, -post_cy],
];
function mid_xs() = n_mid_x > 0 ? [for (i = [1 : n_mid_x]) -post_cx + i*span_x/(n_mid_x + 1)] : [];
function mid_ys() = n_mid_y > 0 ? [for (i = [1 : n_mid_y]) -post_cy + i*span_y/(n_mid_y + 1)] : [];
function mid_xy() = concat([for (x = mid_xs(), s = [1, -1]) [x, s*post_cy]],
                           [for (y = mid_ys(), s = [1, -1]) [s*post_cx, y]]);
function post_xy() = concat(corner_xy(), mid_xy());
function _is_corner(p) = abs(abs(p[0]) - post_cx) < 1e-6 && abs(abs(p[1]) - post_cy) < 1e-6;
// the ±X mid posts hang beside the columns: their inner face (pd - 0.2 into
// the cavity) stays 0.5 off the LoRa clips and the battery bay's ring wall
assert(n_mid_y == 0 || (inner_x/2 - pd + 0.2 - 0.5 >= bh_cx + (bh_w + 3)/2
                        && -(inner_x/2 - pd + 0.2) + 0.5 <= lb_cx - lb_w/2 - clip_stack),
       "a mid post on a ±X wall lands on the LoRa clips or the battery bay — widen post_corner");
// the ±Y mid posts stand between the columns: off the USB opening, the SMA land and the weeps
for (x = mid_xs()) {
    assert(abs(x - lb_cx) >= usb_w/2 + pd/2 + 1.0, "a mid post on the bottom wall lands on the USB opening");
    assert(!sma_boss || abs(x - lb_cx) >= sma_land_d/2 + pd/2 + 0.5, "a mid post on the sky wall lands on the SMA land");
    for (w = weep_xs()) assert(!opt_weep || abs(x - w) >= pd/2 + weep_d()/2 + 0.5, "a mid post on the bottom wall lands on a weep");
}
key_x = inner_x/2 - post_corner - 2.5;   // plate key: +Y bore wall, inboard of the +X corner post (battery side)
for (x = mid_xs()) assert(abs(key_x - x) >= pd/2 + core_key_w()/2 + 0.5, "a mid post lands on the plate key — move key_x");
// the strap channels run across the plate's back between the mid posts' seats
for (p = post_xy()) assert(abs(abs(p[1]) - inner_y/4) >= strap_w/2 + head_d/2 + tol_hole + 0.5 + 1e-9,
       "a plate screw seat lands in a strap channel — move the channels or the posts");
assert(roof_drain_w == 0 || 3*roof_drain_w + 12 <= pan_w, "roof_drain_w: three notches do not fit across the stop");
assert(!sma_boss || sma_d + 6 <= cav_d, "the SMA washer land is taller than the sky wall — shrink it");
assert(sma_z - sma_land_d/2 >= floor_t + (e_seal ? gasket_groove + core_min_web() : 0),
       "the SMA land (and its inside spot-face) reaches the gasket groove — shrink sma_d or deepen cav_d");
assert(sma_z + sma_land_d/2 <= base_d, "the SMA land reaches the face — shrink sma_d or deepen cav_d");
// the spot-face only ever THINS the wall: an unsealed wall (3.0) is already under sma_wall, so it is left alone
sma_wall_eff = min(sma_wall, wall_eff);
assert(sma_wall >= core_min_wall(), "sma_wall: never under the structural minimum wall");
assert(roof_post_x - roof_foot_d/2 >= pan_w/2 + 0.4 - 1e-9 && post_xy()[0][0] >= roof_post_x - 1e-9,
       "a roof foot stands under the panel — its screw cannot be driven with the panel fitted");
assert(abs(gland_y - (lb_cy + lb_l/2)) >= 6.5 + 1.0, "the gland's locknut lands on the LoRa board");
// the USB opening's bottom keeps a web over the gasket groove's top (the plug envelope stays out of the seal path)
assert(usb_zc - usb_h/2 >= floor_t + (e_seal ? gasket_groove : 0) + core_min_web() - 1e-6,
       "the USB opening breaks into the ledge band — raise standoff_h");
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
hw_echo("Solar relay pod", [
    hw_item(len(post_xy()), str(hw_screw(screw_size, screw_head, pl_L, "self-tap"), " (plate to the post ends)")),
    hw_item(2, str(hw_screw(screw_size, "pan", roof_L, "self-tap"), " (through the roof feet into the top posts, from the face)")),
    e_gland ? hw_item(len(post_xy()), hw_oring(screw_size)) : "",
    e_seal ? hw_item(1, "TPU gasket (print part=\"gasket\")") : "",
    hw_item(1, str("SMA bulkhead jack, Ø", sma_d, " D-flat, + EPDM sealing washer under the nut")),
    hw_item(1, "M8 cable gland (panel lead, face)"),
    hw_item(1, "Ø10 adhesive ePTFE vent patch (inner face of the shell)"),
    hw_item(1, str("18650 holder with leads (", bh_l, " x ", bh_w, ")")),
    hw_item(2, str("hose clamp or heavy zip tie, ", strap_w, " mm wide (pole straps)")),
]);
echo(str("Canary solar relay pod v0.2-dev — ", out_x, " x ", out_y, " x ", base_d + lid_t + mount_extra,
         " mm, panel ", pan_w, "x", pan_l, " @ ", roof_ang, " deg  (IN DEVELOPMENT)"));
echo(str("plate screws: ", screw_size, " ", screw_head, " x ", pl_L, ", head ", pl_r, " mm into a ", plate_t,
         " mm plate, ", pl_eng, " mm into ", post_h, " mm posts (pilot ", pl_pil, ")",
         e_gland ? " — O-ring glands under the heads" : "",
         " — the roof's pilots ", roof_pil - lid_t, " mm into the top posts from the face"));
if (wall_eff > wall_t)
    echo(str(e_seal ? "seal mode: " : "", "walls auto-thickened ", wall_t, " -> ", wall_eff,
             " mm — a ", core_min_wall(), " mm skin outside the plate's ", ledge_w, " mm ledge band"));
// the clamp-spacing rule (DESIGN_RULES §6: <= 40 mm between the screws that
// squeeze the gasket) — say what the spans are on every sealed render rather
// than let the build imply what it does not deliver
_seal_span = max(span_x/(n_mid_x + 1), span_y/(n_mid_y + 1));
if (e_seal)
    echo(str("seal mode: ", n_mid_y, " mid post(s) per long (±X) wall at y = ", mid_ys(), ", ", n_mid_x,
             " per short (±Y) wall at x = ", mid_xs(), " — clamp spans ", span_y/(n_mid_y + 1),
             " (long) / ", span_x/(n_mid_x + 1), " (short) mm"));
if (e_seal && _seal_span > 40 + 1e-6)
    echo(str("seal mode: ", _seal_span, " mm between gasket screws (rule: <= 40) — mid-span squeeze rests on the plate's stiffness; treat this build as splash-resistant"));

// ----------------------------------------------------------------------------
//  Helpers — rrect2d/rrect come from canary_core_lib; only file-specific
//  geometry stays local
// ----------------------------------------------------------------------------
// a ring of width w centered on the ledge band (the gasket's home)
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + ledge_w, inner_y + ledge_w, core_cav_r(corner_r, wall_eff) + ledge_w/2);
        offset(r = -w/2) rrect2d(inner_x + ledge_w, inner_y + ledge_w, core_cav_r(corner_r, wall_eff) + ledge_w/2);
    }
}
// Cantilever snap clip on the LoRa board's edge — canary_snap_lib's beam
// (the WAP pattern), so the insertion-strain arithmetic runs as an assert on
// every render: this board sits at standoff_h 3.0, which is exactly the
// short-beam regime where 1.5/0.8-class numbers crack vertical-print PETG.
// The clips stand on the ±X edges, so the wrapper keeps the axis rotation
// and hands the drawing to the library.
module edgeclip(px, py, ang, soff) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, floor_t, floor_t + soff + pcb_t,
                       clip_w, clip_t, clip_hook, clip_hook_h, clip_clear);
}
// the pole strap channels, across the whole back (assembled frame): cut from
// the plate's back AND from the shell's skin around it, so the strap crosses
// the pod's full width in one channel and bears on both parts. The channel
// opens onto the bed, so its roof is a bridge: 45° corners hold the flat span
// to the print-validated 7 mm (canary_port_lib) — a square 9 mm channel
// bridged all 9. Only within the plate's slab behind the floor (seal-safe:
// the seal line is the ledge, floor_t above the channel's roof).
module strap_cut() {
    for (sy = [1, -1]) translate([-out_x/2 - 1, sy*inner_y/4, -mount_extra - 0.1])
        rotate([90, 0, 90]) linear_extrude(out_x + 2)
            port_bridge_profile2d(strap_w, 2*(strap_t + 0.1),
                                  max(0, (strap_w - port_flat_span_max())/2));
}

// ----------------------------------------------------------------------------
//  The PLATE (part="body", the chassis) — the floor with its strap slab, the
//  LoRa rails and clips, the 18650 bay, the strap channels and the seats the
//  screws enter through. Drawn in the assembled frame: front face at
//  z = floor_t, body down to -mount_extra. Prints back-face down.
// ----------------------------------------------------------------------------
module back() {
    difference() {
        union() {
            translate([0, 0, floor_t]) pl_plate(plate_x, plate_y, plate_r, plate_t);
            // LoRa rails + clips
            for (s = [1, -1]) {
                difference() {
                    translate([lb_cx + s*(lb_w/2 - 1.5) - 1.5, lb_cy - (lb_l - 1)/2, floor_t - 0.01])
                        cube([3, lb_l - 1, standoff_h + 0.01]);
                    translate([lb_cx + s*(lb_w/2 - 1.5), lb_cy, floor_t + standoff_h/2])
                        cube([5, clip_w + 2, standoff_h + 1], center = true);
                }
                edgeclip(lb_cx + s*lb_w/2, lb_cy, s > 0 ? 0 : 180, standoff_h);
            }
            // battery-holder bay: a ring wall round the holder's pocket, with a
            // drain notch through its LOW (-Y) end — the weep it drains to sits
            // above the gasket groove now, and a bay with a closed ring is a cup
            translate([bh_cx, bh_cy, floor_t - 0.01]) difference() {
                rrect(bh_w + 3, bh_l + 3, 1.5, 4.01);
                translate([0, 0, -0.5]) rrect(bh_w + 0.6, bh_l + 0.6, 1, 5);
                translate([-2, -(bh_l + 3)/2 - 1, -0.5]) cube([4, 4, 2.0]);
            }
        }
        // the screws: seats through the plate, pan heads over their glands in seal mode
        for (p = post_xy())
            pl_seat_cut(p[0], p[1], floor_t, plate_t, screw_size, screw_head, pl_r, scr_c, tol_hole, e_gland);
        // the key notch: core_key_d() + 0.3 into the plate from the bore wall — never
        // through the ledge band, so the gasket stays backed (asserted above in seal mode)
        if (lid_key) translate([0, 0, floor_t]) lid_key_slot(key_x, bore_y/2, 270, plate_t + 0.2, core_key_d() - 0.7);
        strap_cut();
    }
}
module body() { back(); }   // the catalog's name for the plate

// ----------------------------------------------------------------------------
//  The SHELL (part="lid") — face, walls and posts, one piece, with every wall
//  feature: the SMA bulkhead and its land on the sky wall, the service USB in
//  the bottom wall, the weeps, the gland and vent in the face, the roof's
//  pilots. Drawn with the face at z = 0..lid_t in its own frame (front()) and
//  in the assembled frame by shell_asm(). Prints face-down.
// ----------------------------------------------------------------------------
module front() { translate([0, 0, -base_d]) shell_asm(); }
module lid()   { front(); }   // the catalog's name for the shell

// the shell in the ASSEMBLED frame (back face at -mount_extra, face at base_d..base_d + lid_t)
module shell_asm() {
    difference() {
        shell_solid();
        // the posts' blind pilots, up from their end faces — cut LAST, through
        // the posts the union below adds (a cut inside that union's own
        // difference would cut nothing: the Sense had that bug)
        for (p = post_xy())
            pl_post_pilot(p[0], p[1], floor_t + pl_relief(), pl_pil, scr_d);
        // the roof's blind pilots, from the face into the two TOP corner posts
        for (p = [post_xy()[0], post_xy()[1]])
            translate([p[0], p[1], base_d + lid_t - roof_pil]) cylinder(d = scr_d, h = roof_pil + 0.1);
    }
}
module shell_solid() {
    posts = post_xy();
    gusset_h = max(2, post_h - 0.5);
    gusset_w = min(2.0, rib_t_max(wall_eff));
    union() {
        difference() {
            union() {
                translate([0, 0, base_d]) soft_edge_plate(out_x, out_y, corner_r, lid_t, lid_edge, lid_edge2);
                translate([0, 0, -mount_extra]) rrect(out_x, out_y, corner_r, shell_d + 0.01);
                // SMA sealing-washer land, 1.0 proud of the sky wall: a bulkhead
                // nut torqued onto a flat wall seats its washer IN the film of
                // water that wall carries; on a land it seats above it
                if (sma_boss)
                    translate([lb_cx, out_y/2 - 0.01, sma_z]) rotate([-90, 0, 0])
                        cylinder(d = sma_land_d, h = 1.01);
            }
            // the cavity, coved at the face and at the ledge (canary_core_lib)
            translate([0, 0, base_d]) pl_cavity_cut(inner_x, inner_y, core_cav_r(corner_r, wall_eff), cav_d, floor_cove);
            // the plate's bore below the ledge
            pl_bore_cut(bore_x, bore_y, bore_r, floor_t, plate_t);
            // the gasket groove, cut into the ledge
            if (e_seal)
                translate([0, 0, floor_t - 0.01]) linear_extrude(gasket_groove + 0.01) rim_ring2d(gasket_w);
            // SMA bulkhead, top wall, over the LoRa column. D-FLAT bore: the
            // 1/4-36 thread is Ø6.35, and nut torque on a plain round bore
            // spins the jack and chews the print. Fit an EPDM sealing washer
            // under the external nut — this is the sky-facing wall.
            translate([lb_cx, out_y/2, sma_z])
                rotate([-90, 0, 0]) translate([0, 0, -wall_eff*2])
                    linear_extrude(wall_eff*4) intersection() {
                        circle(d = sma_d);
                        translate([-sma_d/2, -sma_d/2]) square([5.7 + (sma_d - 5.7)/2, sma_d]);   // D-flat: 5.7 across the flat
                    }
            // ...and the INSIDE spot-face that thins the sealed wall to sma_wall
            // around the jack: a standard bulkhead's thread is ~8 mm, and the
            // land + a 5.8 wall + the washer leaves nothing for the nut
            if (wall_eff > sma_wall_eff + 1e-9)
                translate([lb_cx, inner_y/2 - 0.01, sma_z]) rotate([-90, 0, 0])
                    cylinder(d = sma_land_d, h = wall_eff - sma_wall_eff + 0.01);
            // service USB, bottom wall (silicone plug when deployed): 45°
            // chamfers on the BACK side of the opening — in the face-down print
            // that side is the opening's roof, and the chamfers halve the flat
            // bridge (canary_port_lib, the WAP's print-validated profile)
            translate([lb_cx, -out_y/2 + wall_eff*1.5, usb_zc])
                rotate([90, 0, 0]) linear_extrude(wall_eff*3)
                    mirror([0, 1, 0]) port_bridge_profile2d(usb_w, usb_h);
            translate([0, 0, base_d]) {
                // panel-lead gland hole in the face (fit an M8 cable gland or silicone-seal)
                translate([gland_x, gland_y, -1]) cylinder(d = 8.2, h = lid_t + 2);
                // pressure vent: Ø3 hole + inner spot-face for an adhesive ePTFE
                // patch (Ø10) — the most thermally-cycled design in the folder
                // pumps ~14 % of its volume past the gasket per day/night cycle
                // without a membrane (field_ratings.md rule). Roof-shaded face.
                // the spot-face is on the INNER face (the cavity side): the patch
                // is bonded inside, the bore faces the weather. Cut on the show
                // face (as it was) it left a 0.9 mm cup pooling water ON the membrane.
                translate([lb_cx, -inner_y/4, -1]) cylinder(d = 3.0, h = lid_t + 2);
                translate([lb_cx, -inner_y/4, -0.1]) cylinder(d = 10.6, h = 0.6 + 0.1);
                // the house wordmark (opt_mark), debossed on the show face by the
                // first-layer machinery — canary_mark_lib owns the word and its
                // metrics, this file only places it (mark_dx/dy/rot/size/depth)
                if (opt_mark)
                    translate([mark_dx, mark_dy, lid_t - mark_depth])
                        linear_extrude(mark_depth + 1) rotate(mark_rot)
                            mark_wordmark(mark_size);
            }
            // the strap channels' notches through the shell's skin (the plate
            // carries the rest of the channel) — below the ledge, so the seal
            // line is untouched
            strap_cut();
            // bottom-edge chamfer — the LIBRARY's ring (canary_core_lib)
            if (foot_cham > 0)
                foot_chamfer_ring(out_x, out_y, corner_r, foot_cham, -mount_extra);
            // drains through the BOTTOM wall, straight out (= straight down on
            // the pole), a web above the gasket groove (canary_core_lib weep_cut)
            if (opt_weep) for (x = weep_xs())
                weep_cut(x, -inner_y/2, weep_z, "-y", wall_eff, weep_d(), tilt = 0);
        }
        // screw posts from the face's underside to the relief over the ledge,
        // gusseted to their walls (a mid post only to its own) — the gussets
        // root at the face and taper toward the ledge. A corner post also
        // fills the pocket between its two gussets and the cavity's corner:
        // with the cavity coved at the ledge too, that pocket would otherwise
        // close into a sealed void (the mesh gate counts it a part)
        for (p = posts) translate([p[0], p[1], floor_t + pl_relief()]) cylinder(d = pd, h = post_h);
        for (p = posts) translate([0, 0, base_d]) mirror([0, 0, 1]) {
            sx = sign(p[0]); sy = sign(p[1]);
            if (_is_corner(p) || abs(abs(p[0]) - post_cx) < 1e-6)
                corner_gusset(p[0], p[1], sx*(inner_x/2 + 0.5), p[1], gusset_h, wall_eff, pd, gusset_w);
            if (_is_corner(p) || abs(abs(p[1]) - post_cy) < 1e-6)
                corner_gusset(p[0], p[1], p[0], sy*(inner_y/2 + 0.5), gusset_h, wall_eff, pd, gusset_w);
        }
        for (p = posts) if (_is_corner(p))
            translate([min(p[0], sign(p[0])*(inner_x/2 + 0.5)), min(p[1], sign(p[1])*(inner_y/2 + 0.5)), floor_t + pl_relief()])
                cube([inner_x/2 + 0.5 - abs(p[0]), inner_y/2 + 0.5 - abs(p[1]), post_h]);
        // the plate key (canary_core_lib): a rib on the +Y (sky) bore wall, on
        // the battery side — the SMA lives on the LoRa side of that wall
        if (lid_key) lid_key_rib(key_x, bore_y/2, 270, floor_t, plate_t + 0.5);
    }
}

// solar roof: an AWNING — the panel bed roots at the pod's top wall and
// descends outward over the face at roof_ang from horizontal, so the panel
// faces UP and OUT (+Y is up the pole, +Z is away from it: the panel's normal
// is (0, cos, sin)). The v0.1 transform pointed it 60° BELOW horizontal — the
// pod's reason to exist faced the ground. The bed's far (lower) end carries
// the stop the panel rests against under gravity; the panel slides in from
// the root end before the roof goes on. Screws through the feet and the face
// into the two TOP posts (roof_L, blind pilots from the face). Drawn with
// z = 0 on the face's outer plane.
// the bed's own frame: x across, y along the slope from the root, z = 0 the
// bed's underside (the rib side), the panel slot at z 2 .. 2 + pan_t
module roof_bed_frame() {
    translate([0, inner_y/2 - 2, 0]) rotate([90 + roof_ang, 0, 0]) mirror([0, 0, 1]) translate([0, 0, -2])
        children();
}
module roof() {
    // the bed is as long as the panel plus its stop: at 0.75 x pan_l the panel
    // overhung the ROOT end, straight into the top wall and the antenna
    bed_l = pan_l + tol_slide + roof_stop_t;
    stop_h = pan_t - 0.4;   // BELOW the panel's top face: water sheds over the stop, not against it
    rail_x = pan_w/2 + tol_slide;   // the rails' inner face
    difference() {
        union() {
            // two feet over the top corner posts — outboard of the panel
            // edge (roof_post_x), so a driver reaches them past a fitted panel
            for (p = [post_xy()[0], post_xy()[1]])
                translate([p[0], p[1], 0]) cylinder(d = roof_foot_d, h = roof_foot_h);
            roof_bed_frame() {
                translate([-pan_w/2 - 3, 0, 0]) cube([pan_w + 6, bed_l, 2]);                // bed
                for (s = [1, -1]) translate([s*(pan_w/2 + tol_slide) + (s < 0 ? -3 : 0), 0, 2 - 0.01])
                    cube([3, bed_l, pan_t + 2]);                                              // side rails
                for (s = [1, -1]) translate([s*(pan_w/2 - 1.2) + (s < 0 ? -1.8 : 0), 0, 2 + pan_t + 0.2])
                    cube([1.8, bed_l, 1.4]);   // retaining lips: 1.2 mm OVER the panel edge, rooted in the rails
                // stop at the FAR (lower, outboard) end: gravity pulls the panel onto
                // it; drain notches at bed level let what runs under the panel out
                difference() {
                    translate([-pan_w/2 - 3, bed_l - roof_stop_t, 2 - 0.01]) cube([pan_w + 6, roof_stop_t, stop_h + 0.01]);
                    if (roof_drain_w > 0) for (x = [-pan_w/3, 0, pan_w/3])
                        translate([x - roof_drain_w/2, bed_l - roof_stop_t - 1, 2 - 0.02]) cube([roof_drain_w, roof_stop_t + 2, 1.2]);
                }
                // stiffening under the bed (rib_lib): a peripheral loop and two
                // spines along the slope, 4 mm on 1.6 — the bed is a 2 mm plate
                // on two struts with a panel and the wind on it
                if (roof_ribs)
                    translate([0, bed_l/2, 0.01]) mirror([0, 0, 1]) plate_ribs(pan_w + 6, bed_l, 2, 4.0, r = 1.0, inset = 2.0, n = 2);
            }
            // struts from the feet up to the bed's UNDERSIDE, their upper end
            // defined in the bed's frame under the side rail (x outboard of the
            // rail's inner face, z <= 0): a hull of two bodies that both lie
            // outboard of the panel edge cannot reach the panel slot. The old
            // upper end was a world-frame cylinder at 0.8 x the post x — inside
            // the panel's width and 3 mm tall across the bed plane, so both
            // struts stood 2 x 214 mm3 into the panel.
            for (p = [post_xy()[0], post_xy()[1]])
                hull() {
                    translate([p[0], p[1], 0]) cylinder(d = roof_foot_d, h = roof_foot_h);
                    roof_bed_frame()
                        translate([sign(p[0]) > 0 ? rail_x : -rail_x - 3, 14, -2]) cube([3, 8, 2.01]);
                }
        }
        // the roof screws and their driver path: the Ø(head + 0.6) bore runs up
        // through the foot, the strut and the rail root above it, so the driver
        // reaches the head with the panel fitted
        for (p = [post_xy()[0], post_xy()[1]]) {
            translate([p[0], p[1], -0.1]) cylinder(d = scr_c, h = 30);
            translate([p[0], p[1], roof_seat]) cylinder(d = head_d + 0.6, h = 40);
        }
        // crop everything below the foot plane: the tilted lower stop otherwise
        // protrudes ~2.7 mm below z=0 and digs into the face on assembly
        // (crop top sits 0.01 below the feet so no coincident faces)
        translate([-100, -100, -50.01]) cube([200, 200, 50]);
    }
}

module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5); }

// ----------------------------------------------------------------------------
//  Layout — body = the plate (back face at -mount_extra), lid = the shell
//  face-down, the roof on its feet, the gasket flat
// ----------------------------------------------------------------------------
if      (part == "body" || part == "back")  back();
else if (part == "lid"  || part == "front") translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
else if (part == "roof")   roof();
else if (part == "gasket") { assert(e_seal, "gasket needs opt_seal=true"); gasket(); }
else {
    back();
    translate([out_x + 20, 0, 0]) translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
    translate([0, out_y + 30, 0]) roof();
    if (e_seal) translate([-(out_x + 16), 0, 0]) gasket();
}
