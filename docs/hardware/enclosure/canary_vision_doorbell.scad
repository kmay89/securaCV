// ============================================================================
//  SecuraCV Canary Vision — DOORBELL enclosure (parametric)  v0.3
// @env cer=2 ip="~IP54 (button ~IP65)"
//  A slim vertical unit in the Wyze/Ring video-doorbell form factor, holding
//  the stacked-XIAO Vision build: OV5647 camera (top) + Grove Vision AI V2
//  with a XIAO ESP32-C3/S3 seated in its socket (middle) + a 12 mm
//  illuminated momentary button (bottom, wired to the multifunction input).
//
//  Mounting follows the doorbell pattern, not the hinge: a thin WALL PLATE
//  screws to the door frame (flat, or a printable 5–15° WEDGE for angling
//  toward the approach); the body drops onto the plate's two T-studs (the
//  same blind seal-safe pockets as the keyhole system) and locks with a
//  hidden SECURITY SCREW driven up through the plate's bottom foot — the
//  body cannot be lifted off without a tool, Ring-style.
//
//  Power: USB-C cable from the stack's ports loops through the internal
//  cable well and exits through an oval in the BACK, through the matching
//  plate hole, into the wall/under trim (use a right-angle USB-C plug).
//
//  Units: millimeters.  CAD: OpenSCAD.  Weather sealing is ON by default
//  (a doorbell lives outside): TPU gasket + drip-edge face, ~IP54.
//
//  ⚠️ VERIFY BEFORE PRINTING. Measure your seated stack (stack_sock_h,
//     usb heights) and your button's body diameter/depth before printing.
//  Orientation: +Y up on the wall, +Z toward the face.
//
//  v0.3 (2026-08-23): canary_*_lib adoption (dedup, mesh-identical) — the
//  shared helpers, the pocket/stud drawings and the board clip now come from
//  the libraries; opt_mark debosses the house wordmark at the label spot.
//  v0.4 (2026-09-03): assembly review — every released mesh moves, for cause:
//    * the face's pan-head seat left 0.2 mm (one layer) under each head:
//      pads on the inside now carry a 1.0 mm floor (posts shorten to suit);
//    * the weep was BLIND — cut through the 2 mm floor of a back that is
//      5 mm thick — and a Ø1.5 capillary besides: it is Ø2 now, through the
//      bottom wall at the floor corner, beside the plate's foot;
//    * the T-studs sat 3.5 mm short of the pocket's slot end at the foot
//      stop, half of each head over its pass hole: the plate's studs moved
//      up by that travel so the heads park behind the web;
//    * the cable oval was half under the XIAO with 1.5 mm of headroom: it
//      sits wholly in the well now (stud_y 40, no offsets) and the XIAO
//      gets xiao_below of air for a plug — the well was 1.5 mm;
//    * the Pi-cam lens holder (8.5 sq, 5.5 tall) gets the post height it
//      needs; the bottom posts moved down 1.5 to clear a 14 AF button nut
//      (asserted); seal cheeks 1.2 (were 0.8); seal_mid_posts (ON — four
//      corner screws cannot hold a gasket over 97 mm of 2.2 mm face);
//      head_seal O-ring glands; selectable screw_size / screw_head.
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, soft_edge_plate, foot_chamfer_ring —
                             // the catalog's shared helpers
use <canary_mount_lib.scad>  // the stud/keyhole hanging standard — the plate's
                             // T-studs and the body's blind pockets, one home
use <canary_snap_lib.scad>   // snap-fit doctrine — snap_boardclip carries the
                             // WAP clip and its strain gate
use <canary_port_lib.scad>   // connector standards — the shell numbers the
                             // cable exit is sized against
use <canary_board_lib.scad>  // board registry — this file is the measured
                             // source for the seated-stack height
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)
use <canary_color_lib.scad>  // the colorway registry — assembled-preview spools

/* [What to render] */
part = "all";        // ["body","face","plate","gasket","all"]

/* [Options] */
opt_seal   = true;   // perimeter TPU gasket + drip-edge face (doorbells live outside)
opt_vent   = true;   // GORE vent cluster on the face — ON by default: a sealed outdoor
                     // unit with no pressure path pumps moist air past the seals on
                     // every day/night thermal cycle and the condensate never leaves
opt_led    = false;  // separate light-pipe port (the 12 mm button usually has its own LED ring)
opt_tamper = false;  // reed/Hall magnet pocket on the face underside
opt_weep   = true;   // Ø2 weep through the bottom wall at the floor corner (mounted button-down):
                     // condensate leaves beside the plate's foot
weep_d     = 2.0;    // weep bore  // [1.5:0.5:3]
seal_mid_posts = true; // one extra face screw per long wall, at the cable well: four corner screws
                       // cannot hold 20 % gasket squeeze across 97 mm of 2.2 mm face
head_seal  = false;  // O-ring under each face screw head (the posts stand INSIDE the gasket line, so a
                     // bare screw is a drip path down the thread) — needs screw_head = "pan"

/* [Boards] — the stacked-XIAO Vision build. Defaults measured from the
   committed vendor GLB (canary-local/boards/seeed_grove_vision_ai_v2.glb):
   the module PCB is 40 x 20 mm (Grove 1x2 form factor — NOT the 25 x 25 the
   v0.1 bay assumed), mounted VERTICALLY: 40 mm along Y, USB edge down. */
vm_l     = 40.0;     // Grove Vision AI V2 long side (Y; USB edge down) — measured, brd_l("grove_v2")
vm_w     = 20.0;     // module short side (X) — measured, brd_w("grove_v2")
xiao_l   = 21.0;     // XIAO stacked on the module's socket, lower half, centered — brd_l("xiao")
xiao_w   = 17.8;     // the measured board, not the 17.5 spec — brd_xiao_w_measured()
stack_sock_h = 6.5;  // module underside -> XIAO underside when seated — THIS bench
                     // measurement (6.2, carried with margin) is the source behind
                     // brd_stack_sock_measured(); the registry serves it to the catalog
xiao_below   = 5.5;  // air under the XIAO's outward (USB) face: the shell (3.3) plus half a plug's
                     // overmold below the shell axis plus clearance — 1.5 could not pass a plug
vm_front_h   = 5.0;  // module front-side component height (measured)
cam_w    = 25.0;     // OV5647 carrier (Pi-cam v1.3 form) — brd_w("ov5647")
cam_h    = 24.0;     // carrier height (Y) — brd_l("ov5647")
pcb_t    = 1.0;      // brd_t("grove_v2") — the camera carrier matches
board_clear = 0.6;

/* [Camera] — Pi-cam v1.3 posts on the face */
cam_hole_x = 21.0;
cam_hole_y = 12.5;
cam_post_d = 3.6;
cam_post_h = 4.0;    // auto-raised to hold the lens holder behind the face (cam_lens_h)
cam_lens_h = 5.5;    // Pi-cam v1.3 lens holder height above the PCB face — MEASURE yours
cam_lens_sq = 8.5;   // the holder's square: its 12.0 diagonal cannot enter a Ø10 aperture
cam_screw_d = 1.6;
lens_dx  = 0.0;      // lens center offset from the camera-board center — MEASURE
lens_dy  = 2.5;
cam_ap_d   = 10.0;
cam_disc_d = 14.0;   // clear-disc seat (0 = bare aperture)
cam_disc_t = 1.0;

/* [Button] — 12 mm panel-mount illuminated momentary (short body, IP65) */
btn_d      = 12.0;   // button thread/body diameter (hole = btn_d + 2*tol_slide)
btn_bez_d  = 16.5;   // bezel seat diameter on the face (0 = no seat)
btn_bez_t  = 1.0;    // bezel seat depth
btn_body_l = 18.0;   // body + terminals depth behind the panel — checked against the cavity
btn_nut_ac = 16.2;   // panel nut across corners (14 AF M12 nut = 16.2; 16 AF = 18.5) — asserted
                     // against the bottom screw posts

/* [Doorbell shell] */
db_r     = 12.0;     // outside corner radius (pill look; <= half the width)
wall_t   = 2.0;      // auto-thickened in seal mode — catalog default, core_wall()
floor_t  = 2.0;      // back thickness
lid_t    = 2.2;      // face thickness (the visible surface)
lip_h    = 4.0;
lip_t    = 1.2;
cav_extra = 1.0;     // headroom over the tallest component
zone_top = 8.0;      // margin above the camera (top posts live here)
zone_gap = 2.0;      // camera <-> module gap
zone_well = 12.0;    // cable well between module and button (USB plugs live here)
zone_btn = 21.0;     // button zone height
usb_exit_w = 12.0;   // oval cable exit through the back (into the plate hole) — sized
                     // for a molded right-angle USB-C plug HEAD, not the 8.94 mm
                     // shell (port_usbc_shell_w()): molded heads run ~10-12 mm
usb_exit_h = 7.0;
usb_exit_dx = 0.0;   // exit offset from the centerline (0 keeps it between the mid-span posts)
usb_exit_dy = 0.0;   // exit offset from the well center: the oval sits wholly in the well, under
                     // nothing (at +6 it reached under the module and the XIAO)

/* [Print tolerances] — the catalog trio (canary_core_lib core_tol_*(),
   dialed on the fit coupon) */
tol_slide = 0.20;    // core_tol_slide()
tol_press = 0.10;    // core_tol_press()
tol_hole  = 0.30;    // core_tol_hole()

/* [Engineering] (see README "Engineering & materials") */
screw_insert = false;  // M2 brass heat-set inserts in the corner posts
insert_d     = 3.5;
insert_h     = 4.0;
lid_ribs     = true;   // perimeter rib ring under the face
lid_rib_w    = 2.5;
lid_rib_h    = 1.0;
foot_cham    = 0.5;    // 45° chamfer on the body's back edge

/* [Screw posts] (face screws — use black-oxide M2 for looks) */
post_d       = 5.0;
screw_size   = "m2";  // ["m2","m2.5","m3"] face screw — the core lib's registry sets pilot, clearance,
                      // head seat, insert bore and post floor; "m2" keeps the validated numbers below
screw_head   = "pan"; // ["pan","flat"] pan = flat-floored seat (the BOM's black-oxide pan heads; what
                      // head_seal needs), flat = 90° countersink
screw_d      = 1.6;   // (m2)
screw_head_d = 4.0;   // (m2 pan)
screw_head_h = 2.0;   // (m2 pan) seat depth; the face carries a pad under it (head_pad)

/* [Weather sealing] */
gasket_w      = 1.6;
gasket_groove = 1.2;
gasket_proud  = 0.3;
skirt_h       = 3.0;
skirt_t       = 1.6;

/* [Wall plate + T-stud hooks + security screw] — the stud/pocket pair is the
   catalog's one hanging interface: canary_mount_lib owns the drawings and the
   numbers, these knobs stay for per-printer dialing */
plate_t     = 4.0;    // plate thickness at the THIN end
plate_wedge = 0;      // vertical wedge: camera tilts down the approach  // [0:5:15]
plate_wedge_x = 0;    // horizontal wedge: aims left/right (corner installs)  // [-15:5:15]
stud_y      = 40.0;   // T-stud/pocket centers at y = ±stud_y (clear of the cable exit and the well)
kh_head_d   = 7.0;    // pocket head pass (stud head 6.6) — mount_kh_head_d()
kh_shank_d  = 4.2;    // pocket slot (stud stem 4.0) — mount_kh_shank_d()
kh_slot_l   = 8.0;    // catalog standard — mount_kh_slot_l()
kh_head_h   = 3.5;    // pocket depth (face web + head cavity) — mount_kh_head_h()
kh_face     = 1.0;    // catalog standard — mount_kh_face()
kh_extra    = 3.0;    // body back thickening that hosts the pockets
sec_screw_d = 2.2;    // security screw (M2 self-tap; use a Torx/security drive)
plate_screw_d = 4.2;  // wall screws (#8 / M4 PAN head — the seats are flat counterbores)

/* [Aesthetics] */
colorway    = "graphite"; // ["graphite","canary","snow","forest","midnight"] assembled-preview spool set (canary_color_lib; single-part exports carry no color)
lid_edge    = 1.0;    // face edge chamfer
lid_edge2   = 0.8;    // second, steeper stage (~66°) — softens the face edge toward a roundover
// The wordmark sits where label_text would (label_dx/dy/rot/size/depth place
// and size it) and is gated by the mark library's measured type
opt_mark    = false;  // deboss the house wordmark instead of a custom label (exclusive with label_text)
                      // metrics, so a size that would print as a smudge or run off
                      // the face is refused before a print, not after
label_text  = "";     // debossed face label ("" = off)
label_size  = 4.5;
label_depth = 0.5;
label_dx    = 0.0;
label_dy    = -26.0;
label_rot   = 0;
label_font  = "Liberation Sans:style=Bold";

/* [Front-face features] — offsets from the MODULE center */
lp_d   = 3.0;
lp_dx  = 8.0;
lp_dy  = -8.0;
vent_pad_d     = 12.0;
vent_pad_depth = 0.8;
vent_hole_d    = 1.6;
vent_ring_d    = 6.0;
vent_holes     = 6;
vent_dx        = 0.0;    // vent/sound cluster ON the face's vertical axis — the
vent_dy        = -8.0;   // camera → grille → button rhythm of a real doorbell
                         // (off-axis it read as an accidental drill pattern)
mag_d  = 6.0;
mag_h  = 3.2;
mag_dx = 8.0;
mag_dy = 8.0;

/* [Board snap clips] — the WAP's print-proven numbers (the canary_snap_lib
   snap_boardclip defaults); the lib's strain gate holds them honest */
clip_w      = 6.0;
clip_t      = 1.0;
clip_hook   = 0.5;
clip_hook_h = 1.2;
clip_clear  = 0.25;

/* [Quality] */
// curve quality: $fa/$fs give smooth big arcs (pill corners, hood) without
// exploding tiny holes into thousands of facets like a large $fn would
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
e_seal   = opt_seal;
wall_eff = e_seal ? max(wall_t, gasket_w + 2*core_min_wall()) : wall_t;   // 1.2 mm cheek each side of the groove
scr_d   = (screw_size == "m2") ? screw_d : scr_pilot(screw_size);
scr_c   = max(scr_d + 2*tol_hole, scr_clear(screw_size));
head_d  = (screw_size == "m2" && screw_head == "pan") ? screw_head_d
        : (screw_head == "pan") ? scr_pan_d(screw_size) : scr_flat_d(screw_size);
head_h  = (screw_size == "m2" && screw_head == "pan") ? screw_head_h
        : (screw_head == "pan") ? scr_pan_h(screw_size) : scr_flat_h(screw_size);
ins_od  = (screw_size == "m2") ? insert_d : scr_insert_d(screw_size) + 0.3;
ins_h   = (screw_size == "m2") ? insert_h : scr_insert_h(screw_size);
pd = max(screw_insert ? max(post_d, ins_od + 3.0) : post_d, scr_post_min(screw_size));
head_pad = (screw_head == "pan") ? max(0, head_h + 1.0 - lid_t) : 0;
clip_stack  = clip_clear + clip_t;
vm_standoff = stack_sock_h + xiao_below;
cam_post_eff = (cam_ap_d >= cam_lens_sq*1.4142 + 0.6) ? cam_post_h : max(cam_post_h, cam_lens_h + 0.3);

inner_x = max(cam_w + 2*board_clear, vm_w + 2*(clip_stack + board_clear) + 0.5);
inner_y = zone_btn + zone_well + (vm_l + board_clear) + zone_gap + cam_h + zone_top;
cav_d   = max(vm_standoff + pcb_t + vm_front_h + cav_extra, btn_body_l - lid_t + 1);

out_x  = inner_x + 2*wall_eff;
out_y  = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;
rr     = min(db_r, out_x/2 - 0.1);          // pill radius, clamped to the width

btn_cy  = -inner_y/2 + zone_btn/2;
well_cy = -inner_y/2 + zone_btn + zone_well/2;      // cable well / USB plug space
vm_cy   = -inner_y/2 + zone_btn + zone_well + board_clear + vm_l/2;
cam_cy  = vm_cy + vm_l/2 + zone_gap + cam_h/2;
vm_cx   = 0;  cam_cx = 0;
lens_x  = lens_dx;  lens_y = cam_cy + lens_dy;

// four face-screw posts in the top/bottom margins, inset from the pill corners
// (the bottom pair 1.0 from the wall, not 2.5: at 2.5 a 14 AF button nut
// touched them), plus the mid-span pair at the cable well when seal_mid_posts
function post_xy() = concat([
    [ inner_x/2 - pd/2 - 1.0,  inner_y/2 - pd/2 - 2.5],
    [-inner_x/2 + pd/2 + 1.0,  inner_y/2 - pd/2 - 2.5],
    [ inner_x/2 - pd/2 - 1.0, -inner_y/2 + pd/2 + 1.0],
    [-inner_x/2 + pd/2 + 1.0, -inner_y/2 + pd/2 + 1.0],
], (e_seal && seal_mid_posts) ? [[inner_x/2 - pd/2 + 0.2, well_cy], [-inner_x/2 + pd/2 - 0.2, well_cy]] : []);

skirt_gap = tol_slide + 0.2;
plate_x   = e_seal ? out_x + 2*(skirt_gap + skirt_t) : out_x;
plate_y   = e_seal ? out_y + 2*(skirt_gap + skirt_t) : out_y;
plate_r   = e_seal ? rr + skirt_gap + skirt_t : rr;

assert(btn_bez_d == 0 || btn_bez_d > btn_d + 2, "btn_bez_d must exceed the button hole");
assert(head_d > scr_c, "the screw head must be larger than its clearance hole, or it falls through the face");
assert(screw_head == "flat" || head_h + 1.0 - lid_t <= 1.5, "pan-head seat needs more than 1.5 mm of inside pad — thicken lid_t");
assert(!head_seal || screw_head == "pan", "head_seal seats an O-ring under a PAN head — set screw_head = \"pan\"");
assert(!head_seal || e_seal, "head_seal only means something in seal mode");
// the button's panel nut must clear the bottom posts (their inner edge vs the nut's corner radius)
assert(len([for (p = post_xy()) if (sqrt(pow(p[0], 2) + pow(p[1] - btn_cy, 2)) < btn_nut_ac/2 + pd/2 + 0.5) 1]) == 0,
       str("a ", btn_nut_ac, " mm across-corners button nut hits a face-screw post — smaller nut, or move the posts"));
// the cable oval must sit under the WELL: below the module's bottom edge and clear of the mid posts
assert(well_cy + usb_exit_dy + usb_exit_h/2 <= vm_cy - vm_l/2 - 0.5,
       "cable exit reaches under the module/XIAO — lower usb_exit_dy or lengthen zone_well");
assert(!(e_seal && seal_mid_posts) || abs(usb_exit_dx) + usb_exit_w/2 <= inner_x/2 - pd + 0.2 - 1.0,
       "cable exit runs into a mid-span post — center it (usb_exit_dx) or narrow it");
assert(btn_body_l - lid_t + 1 <= cav_d, "button too deep — raise btn_body_l budget or cavity");
assert(zone_btn >= btn_bez_d + 3, "zone_btn too short for the button bezel");
assert(lip_h < cav_d, "lip_h must be less than the cavity depth");
assert(!e_seal || gasket_groove <= lip_h - 0.5, "gasket_groove must stay below the lip");
assert(cam_disc_d == 0 || cam_disc_d > cam_ap_d, "cam_disc_d must exceed cam_ap_d");
assert(kh_head_h + 1.5 <= floor_t + kh_extra, "stud pocket too deep — raise kh_extra");
assert(lid_edge == 0 || (lid_edge >= 0.01 && lid_edge < lid_t), "lid_edge out of range");
assert(lid_edge2 >= 0 && (lid_edge > 0 || lid_edge2 == 0) && lid_edge + lid_edge2 < lid_t,
       "lid_edge2 requires lid_edge > 0, and their sum must stay below lid_t");
assert(abs(plate_wedge_x) <= 15 && plate_wedge <= 15, "keep wedge angles <= 15 degrees");
assert(well_cy + usb_exit_dy - usb_exit_h/2 >= -stud_y + kh_slot_l/2 + (kh_head_d + 0.6)/2 + 0.8,
       "cable exit overlaps the lower T-stud pocket — raise usb_exit_dy or stud_y");
assert(abs(usb_exit_dx) + usb_exit_w/2 <= inner_x/2 - 1, "cable exit too wide/offset for the cavity");
assert(!(opt_mark && label_text != ""),
       "opt_mark and label_text share the label spot — set one, not both");
assert((label_text == "" && !opt_mark) || (label_depth > 0 && label_depth < lid_t),
       "label_depth must be between 0 and lid_t");
// the wordmark's two gates, from the mark library's measured type metrics —
// the render looks perfect either side of them, which is why they are asserts
assert(!opt_mark || label_size >= mark_word_min_h(),
       str("opt_mark at label_size ", label_size, " mm is under the ",
           mark_word_min_h(), " mm cap height where a 0.4 mm bead still ",
           "reaches the letterforms — raise label_size"));
assert(!opt_mark || mark_word_ink_w("securaCV", label_size) <= plate_x - 4.0,
       str("the wordmark draws ", mark_word_ink_w("securaCV", label_size),
           " mm at label_size ", label_size, " on a ", plate_x,
           " mm face (2 mm margin per side) — shrink label_size"));
echo(str("Canary Vision DOORBELL v0.4 — body ", out_x, " x ", out_y, " x ", base_d + lid_t + kh_extra,
         " mm + plate ", plate_t, " mm (wedge ", plate_wedge, " deg, seal=", e_seal, ")"));

// ----------------------------------------------------------------------------
//  Helpers — the idiom once shared by copy with the other Canary enclosures
//  now comes from the canary_*_lib set; what stays local is doorbell-specific
//  (the rim ring, the vent cluster, the wedge plate — and the T-stud, for the
//  mesh-stability reason at its definition)
// ----------------------------------------------------------------------------
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, rr - wall_eff/2));
        offset(r = -w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, rr - wall_eff/2));
    }
}
// The WAP cantilever clip, routed through canary_snap_lib so the strain gate
// runs on every render (the beam here rises 9 mm off the floor — 0.9 %,
// nothing near the budget). The lib places clips across a Y edge line; these
// stand on ±X board edges, so the wrapper keeps this file's original
// rotate-into-place transform — identity ops only, the released mesh stays put.
module edgeclip(px, py, ang, soff) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, floor_t, floor_t + soff + pcb_t,
                       w = clip_w, t = clip_t, hook = clip_hook,
                       hook_h = clip_hook_h, clear = clip_clear);
}

// ----------------------------------------------------------------------------
//  BODY (back shell: board rails, cable exit, stud pockets, security boss)
// ----------------------------------------------------------------------------
module body() {
    posts = post_xy();
    gusset_h = max(2, cav_d - lip_h - 1.0);
    difference() {
    union() {
        difference() {
            union() {
                rrect(out_x, out_y, rr, base_d);
                translate([0, 0, -kh_extra]) rrect(out_x, out_y, rr, kh_extra);  // stud-pocket back
            }
            translate([0, 0, floor_t]) rrect(inner_x, inner_y, max(0.1, rr - wall_eff), cav_d + 1);
            // oval cable exit through the back (aligns with the plate hole; offset
            // sideways/up so it clears the lower T-stud pocket)
            translate([usb_exit_dx, well_cy + usb_exit_dy, 0]) hull()
                for (s = [1, -1]) translate([s*(usb_exit_w - usb_exit_h)/2, 0, -kh_extra - 1])
                    cylinder(d = usb_exit_h, h = kh_extra + floor_t + 2);
            if (e_seal)
                translate([0, 0, base_d - gasket_groove])
                    linear_extrude(gasket_groove + 1) rim_ring2d(gasket_w);
            // blind T-stud pockets, slot toward +Y (the body drops onto the
            // plate's studs) — canary_mount_lib draws the pocket natively per
            // axis, no rotate, so the released mesh stays put
            for (yc = [-stud_y, stud_y])
                mount_keyhole_pocket(yc, -kh_extra, "y",
                                     head_d = kh_head_d, shank_d = kh_shank_d,
                                     slot_l = kh_slot_l, head_h = kh_head_h,
                                     face = kh_face);
            if (foot_cham > 0) foot_chamfer_ring(out_x, out_y, rr, foot_cham, -kh_extra);
            // weep at the cavity's lowest point (mounted button-down): through the
            // BOTTOM wall at the floor corner, angled down, beside the plate's foot
            // (x ±6) and inboard of the bottom posts' gussets. The v0.3 weep went
            // through the 2 mm floor of a 5 mm back — it was blind. Too small to
            // matter for ingress; the pressure path is the vent membrane.
            if (opt_weep)
                weep_cut(7.0, -inner_y/2, floor_t + weep_d/2 + 0.2, "-y", wall_eff, weep_d);
        }
        // internal boss backing the security screw (~7 mm thread engagement);
        // kept below z=5 so it clears the button body's tip
        translate([-4, -inner_y/2 - 0.1, 0]) cube([8, 4.1, 5]);
        // face-screw posts, gusseted into the nearest walls (a mid-span post only
        // to its own wall); shortened by the face's head pads
        difference() {
            union() {
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - head_pad);
                for (p = posts) {
                    sx = sign(p[0]); sy = abs(p[1]) > 1 ? sign(p[1]) : 0;
                    hull() {
                        translate([p[0], p[1], floor_t]) cylinder(d = pd, h = gusset_h);
                        translate([sx*(inner_x/2 - 0.3), p[1], floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                    if (sy != 0 && abs(p[1]) > inner_y/2 - 10) hull() {
                        translate([p[0], p[1], floor_t]) cylinder(d = pd, h = gusset_h);
                        translate([p[0], sy*(inner_y/2 - 2.3), floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                }
            }
            for (p = posts) translate([p[0], p[1], floor_t + 2.0])
                cylinder(d = screw_insert ? scr_nominal(screw_size) + 0.3 : scr_d, h = cav_d);
            if (screw_insert)
                for (p = posts) translate([p[0], p[1], floor_t + cav_d - head_pad - ins_h - 0.5])
                    cylinder(d = ins_od - 0.3, h = ins_h + 1);
        }
        // module rails + clips, TOP HALF ONLY — the stacked XIAO (17.8 wide on
        // the 20 mm module) hangs beneath the LOWER half, so full-length side
        // rails would collide with it. Two bottom-corner pins catch the module's
        // lower edge outboard of the XIAO and the down-facing USB ports.
        for (s = [1, -1]) {
            rail_l = vm_l/2 - 4;
            difference() {
                translate([vm_cx + s*(vm_w/2 - 1.5) - 1.5, vm_cy + 2, floor_t])
                    cube([3, rail_l, vm_standoff]);
                translate([vm_cx + s*(vm_w/2 - 1.5), vm_cy + 2 + rail_l/2, floor_t + vm_standoff/2])
                    cube([5, clip_w + 2, vm_standoff + 1], center = true);
            }
            edgeclip(vm_cx + s*vm_w/2, vm_cy + 2 + rail_l/2, s > 0 ? 0 : 180, vm_standoff);
            translate([vm_cx + s*(xiao_w/2 + 1.0 + 0.1), vm_cy - vm_l/2 + 1.2, floor_t])
                cylinder(d = 2.0, h = vm_standoff);   // pin inner edge 0.1 outboard of the measured
                                                      // XIAO, still catches the 20 mm module
        }
    }
    // security-screw pilot, drilled LAST so it passes through wall AND boss,
    // ending blind inside the boss (the seal envelope stays intact)
    translate([0, -out_y/2 - 0.1, 3.0]) rotate([-90, 0, 0]) cylinder(d = sec_screw_d - 0.5, h = 6.5);
    }
}

// ----------------------------------------------------------------------------
//  FACE (lens + disc seat, button bezel, optional vent/LED/label, lip, skirt)
// ----------------------------------------------------------------------------
module vent_cluster(x, y) {
    translate([x, y, 0]) {
        translate([0, 0, lid_t - vent_pad_depth]) cylinder(d = vent_pad_d, h = vent_pad_depth + 1);
        for (i = [0 : vent_holes - 1]) rotate([0, 0, i * 360 / vent_holes])
            translate([vent_ring_d/2, 0, -1]) cylinder(d = vent_hole_d, h = lid_t + 2);
    }
}
module face() {
    union() {
        difference() {
            union() {
                // the catalog's two-stage face-down soft edge (canary_core_lib) —
                // this file's face_plate copy, retired into the library
                soft_edge_plate(plate_x, plate_y, plate_r, lid_t, lid_edge, lid_edge2);
                // pan-head pads: the 1.0 mm floor under each head the 2.2 face cannot spare
                if (head_pad > 0) for (p = post_xy())
                    translate([p[0], p[1], -head_pad]) cylinder(d = head_d + 2*tol_hole + 3.2, h = head_pad + 0.1);
            }
            translate([lens_x, lens_y, -1]) cylinder(d = cam_ap_d, h = lid_t + 2);
            if (cam_disc_t > 0 && cam_disc_d > 0) {
                translate([lens_x, lens_y, lid_t - (cam_disc_t + 0.2)])
                    cylinder(d = cam_disc_d + 2*tol_slide, h = cam_disc_t + 1);
                // cosmetic 45° lead-in around the seat rim (cleaner edge, easier disc entry)
                translate([lens_x, lens_y, lid_t - 0.4])
                    cylinder(d1 = cam_disc_d + 2*tol_slide, d2 = cam_disc_d + 2*tol_slide + 1.0, h = 0.41);
            }
            // button hole + bezel seat (+ matching lead-in rim)
            translate([0, btn_cy, -1]) cylinder(d = btn_d + 2*tol_slide, h = lid_t + 2);
            if (btn_bez_d > 0) {
                translate([0, btn_cy, lid_t - btn_bez_t])
                    cylinder(d = btn_bez_d + 2*tol_slide, h = btn_bez_t + 1);
                translate([0, btn_cy, lid_t - 0.4])
                    cylinder(d1 = btn_bez_d + 2*tol_slide, d2 = btn_bez_d + 2*tol_slide + 1.0, h = 0.41);
            }
            if (opt_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -1]) cylinder(d = lp_d + 2*tol_press, h = lid_t + 2);
            if (opt_vent) vent_cluster(vm_cx + vent_dx, vm_cy + vent_dy);
            // screw seats by the head in the bag (canary_core_lib): flat floor for
            // PAN heads (on the pad), 90° cone for FLAT, O-ring gland with head_seal
            for (p = post_xy()) translate([0, 0, -head_pad]) {
                if (screw_head == "flat")
                    cs_cone90_cut(p[0], p[1], lid_t, scr_c, head_h);
                else if (head_seal)
                    cb_oring_cut(p[0], p[1], lid_t + head_pad, scr_c,
                                 scr_oring_id(screw_size), scr_oring_cs(screw_size), head_d);
                else
                    cb_flat_cut(p[0], p[1], lid_t + head_pad, scr_c, head_d + 2*tol_hole, head_h);
            }
            if (label_text != "")
                translate([label_dx, label_dy, lid_t - label_depth])
                    linear_extrude(label_depth + 1) rotate(label_rot)
                        text(label_text, size = label_size, font = label_font,
                             halign = "center", valign = "center");
            // the house wordmark (opt_mark), debossed where the label would sit
            // and by the same first-layer machinery — canary_mark_lib owns the
            // word and its face, this file only places it
            if (opt_mark)
                translate([label_dx, label_dy, lid_t - label_depth])
                    linear_extrude(label_depth + 1) rotate(label_rot)
                        mark_wordmark(label_size);
        }
        // perimeter rib ring (t³ stiffening), cleared around features and posts;
        // fused to the lip (it stopped 0.4 short — a one-nozzle slot)
        if (lid_ribs) {
            ro_x = inner_x - 2*tol_slide - 2*lip_t + 0.4;
            ro_y = inner_y - 2*tol_slide - 2*lip_t + 0.4;
            difference() {
                translate([0, 0, -lid_rib_h]) linear_extrude(lid_rib_h + 0.1)
                    difference() {
                        rrect2d(ro_x, ro_y, max(0.1, rr - wall_eff - lip_t));
                        rrect2d(ro_x - 2*lid_rib_w, ro_y - 2*lid_rib_w, 0.1);
                    }
                for (p = post_xy())
                    translate([p[0], p[1], -lid_rib_h - 0.1]) cylinder(d = pd + 1.6, h = lid_rib_h + 0.2);
                translate([lens_x, lens_y, -lid_rib_h - 0.1])
                    cylinder(d = max(cam_ap_d, cam_disc_d) + 3, h = lid_rib_h + 0.2);
                for (sx = [1, -1], sy = [1, -1])
                    translate([cam_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, -lid_rib_h - 0.1])
                        cylinder(d = cam_post_d + 2, h = lid_rib_h + 0.2);
                translate([0, btn_cy, -lid_rib_h - 0.1])
                    cylinder(d = max(btn_bez_d, btn_d) + 4, h = lid_rib_h + 0.2);
                if (opt_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -lid_rib_h - 0.1])
                    cylinder(d = lp_d + 4, h = lid_rib_h + 0.2);
                if (opt_vent) translate([vm_cx + vent_dx, vm_cy + vent_dy, -lid_rib_h - 0.1])
                    cylinder(d = vent_pad_d + 3, h = lid_rib_h + 0.2);
                if (opt_tamper) translate([vm_cx + mag_dx, vm_cy + mag_dy, -lid_rib_h - 0.1])
                    cylinder(d = mag_d + 2*tol_press + 4.8, h = lid_rib_h + 0.2);
            }
        }
        // camera posts (Pi-cam v1.3 grid), tall enough to hold the lens holder behind the face
        for (sx = [1, -1], sy = [1, -1])
            translate([cam_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, -cam_post_eff])
                difference() {
                    cylinder(d = cam_post_d, h = cam_post_eff + 0.1);
                    translate([0, 0, -0.1]) cylinder(d = cam_screw_d, h = cam_post_eff - 0.8);
                }
        // lip, cleared at posts
        difference() {
            translate([0, 0, -lip_h])
                difference() {
                    rrect(inner_x - 2*tol_slide, inner_y - 2*tol_slide,
                          max(0.1, rr - wall_eff - tol_slide), lip_h);
                    rrect(inner_x - 2*tol_slide - 2*lip_t, inner_y - 2*tol_slide - 2*lip_t, 0.1, lip_h + 1);
                }
            for (p = post_xy())
                translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
        }
        // drip-edge skirt (no notch needed — no wall ports on a doorbell)
        if (e_seal)
            translate([0, 0, -skirt_h]) linear_extrude(skirt_h)
                difference() {
                    rrect2d(plate_x, plate_y, plate_r);
                    rrect2d(out_x + 2*skirt_gap, out_y + 2*skirt_gap, rr + skirt_gap);
                }
        if (opt_tamper)
            translate([vm_cx + mag_dx, vm_cy + mag_dy, -mag_h]) difference() {
                cylinder(d = mag_d + 2*tol_press + 2.4, h = mag_h + 0.1);
                translate([0, 0, -0.1]) cylinder(d = mag_d + 2*tol_press, h = mag_h + 0.1);
            }
    }
}

// ----------------------------------------------------------------------------
//  GASKET
// ----------------------------------------------------------------------------
module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5); }

// ----------------------------------------------------------------------------
//  WALL PLATE — flat or wedge; T-studs the body drops onto; countersunk wall
//  screws; cable pass; bottom L-foot with the security-screw hole.
//  Modeled in print orientation (back on the bed).
// ----------------------------------------------------------------------------
// The catalog T-stud, kept as THIS FILE'S drawing rather than routed through
// canary_mount_lib's mount_tstud(): the lib overlaps its stem 0.01 into the
// cone (the catalog's CGAL hygiene rule), this drawing joins them on an exact
// shared plane, and CGAL tessellates the two joints differently — same
// envelope, different mesh, and the released plate STL must not move under a
// dedup. The numbers ARE the standard (mount_stud_* in canary_mount_lib);
// adopting the lib's overlap is a deliberate re-release, not a dedup.
module tstud(yc, zbase) {
    translate([0, yc, zbase]) {
        cylinder(d = 4.0, h = kh_face + 0.4);                       // stem (rides the 4.2 slot)
        translate([0, 0, kh_face + 0.4]) cylinder(d1 = 4.0, d2 = 6.6, h = 1.2);  // 45° under-head
        translate([0, 0, kh_face + 1.6]) cylinder(d = 6.6, h = 0.8);             // head
    }
}
// top-surface height of the (possibly wedged) plate at a given y —
// thin at the bottom (-Y), thick at the top: the camera tilts DOWN toward
// the walk-up, the usual doorbell wedge direction
function plate_z(y) = plate_t + (plate_wedge > 0 ? (y + out_y/2) * tan(plate_wedge) : 0);
// extra height the horizontal wedge adds at the plate edge
function plate_zx() = out_x/2 * tan(abs(plate_wedge_x));

module plate() {
    hmax = plate_z(out_y/2) + 2*plate_zx() + 0.1;   // covers the HIGH side of the x-wedge too
    foot_z = plate_t + kh_extra + 3.0;         // security bore height = body pilot height
    difference() {
        union() {
            // slab with a sloped top: straight prism cut by the wedge plane
            difference() {
                rrect(out_x, out_y, rr, hmax);
                translate([0, -out_y/2, plate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0])
                    translate([-out_x - 1, -1, 0]) cube([2*out_x + 2, out_y + rr + 2, hmax + out_y + out_x]);
            }
            // studs, L-foot and (below) the security bore all live in the TILTED
            // frame so they stay aligned with the body resting on the wedge face
            // the body rests on the foot 0.5 above the plate's bottom edge, so a stud
            // drawn AT the pocket center parks 0.5 short of it — and 3.5 short of the
            // slot's far end, half its head over the pass hole. Drawn (slot/2 - 0.5)
            // higher, the head parks at the slot end, wholly behind the 1.0 mm web:
            // offer the pass holes over the studs, drop the body 7.5, it stops on the foot
            translate([0, -out_y/2, plate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0]) {
                tstud(out_y/2 - stud_y + (kh_slot_l/2 - 0.5), -0.2);
                tstud(out_y/2 + stud_y + (kh_slot_l/2 - 0.5), -0.2);
                // bottom L-foot: sits under the body's bottom wall, carries the security screw
                translate([-6, -4, -plate_t - plate_zx()]) cube([12, 4.5, foot_z + plate_zx() + 4]);
            }
        }
        // wall screws: through-holes + flat counterbores at a CONSTANT 3 mm from
        // the wall side, so standard-length screws work at any wedge angle
        // (7.5 from the side edge, not 8: the top stud's head now reaches y = stud_y + 6.8
        // and the counterbores must stay 1 mm clear of it in x)
        for (sy = [1, -1], sx = [1, -1]) {
            translate([sx*(out_x/2 - 7.5), sy*(out_y/2 - 14), -0.1])
                cylinder(d = plate_screw_d, h = hmax + 1);
            translate([sx*(out_x/2 - 7.5), sy*(out_y/2 - 14), 3.0])
                cylinder(d = plate_screw_d + 4.4, h = hmax + 1);
        }
        // cable pass (a roomier match for the body's oval exit)
        translate([usb_exit_dx, well_cy + usb_exit_dy, -0.1]) hull()
            for (s = [1, -1]) translate([s*(usb_exit_w - usb_exit_h)/2, 0, 0])
                cylinder(d = usb_exit_h + 4, h = hmax + 1);
        // security-screw bore: up through the foot into the body's pilot (tilted frame)
        translate([0, -out_y/2, plate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0])
            translate([0, -4.1, foot_z - plate_t]) rotate([-90, 0, 0])
                cylinder(d = sec_screw_d + 0.4, h = 5);
        // flatten anything the compound wedge tips below the wall plane (z < 0)
        translate([-out_x, -out_y/2 - 12, -10]) cube([2*out_x, out_y + 24, 10]);
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
if      (part == "body")   body();
else if (part == "face")   translate([0, 0, lid_t]) rotate([180, 0, 0]) face();
else if (part == "gasket") { assert(e_seal, "gasket needs opt_seal=true"); gasket(); }
else if (part == "plate")  plate();
else {
    // assembled preview wears the chosen colorway (canary_color_lib);
    // color() is preview-only — single-part exports are byte-identical
    color(cw_body(colorway)) body();
    color(cw_body(colorway)) translate([0, 0, 0]) translate([out_x/2 + plate_x/2 + 10, 0, lid_t]) rotate([180, 0, 0]) face();
    color(cw_body(colorway)) translate([-(out_x + 14), 0, 0]) plate();
    if (e_seal) color(cw_light(colorway)) translate([0, out_y + 12, 0]) gasket();
}
