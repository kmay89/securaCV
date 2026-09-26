// ============================================================================
//  SecuraCV Canary Vision — 3D-printable enclosure (parametric)  v0.5
// @env cer=2 ip="~IP54" basis="weather preset"
//  Stack: OV5647 camera (Pi-cam v1.3 form) + Grove Vision AI V2 (40 x 20)
//         + a selectable HOST:
//    host = "xiao"   — XIAO ESP32-C3/S3 seated in the module's stacking
//                      socket (recommended; zero wiring — device guide §3).
//                      Compact single-column case; the bottom wall carries
//                      BOTH USB-C ports (module "model port" + XIAO
//                      "firmware port", stacked vertically).
//    host = "devkit" — ESP32-C3-DevKitM-1 beside the module, joined by the
//                      Grove I2C cable (two-column case).
//
//  A wall/eave camera unit built as a PISTON PLATE (canary_core_lib pl_*):
//  the SHELL is one piece — the front face with the lens aperture (recessed
//  clear-disc seat, optional rain hood), the LED light pipe, the side walls
//  and the screw posts — printed face-down as a cup; the camera board screws
//  to posts under the face, so the lens is registered to its window by the
//  part that carries the window. The PLATE (part="back") nests inside the
//  walls like a piston, seats on a ledge they carry, and is the chassis: the
//  module rails and clips, the stacked XIAO's room, the DevKit pads, the
//  keyholes and the four (or six) short screw seats all live on it. The only
//  seam is a hairline on the back face. Mounting is a GoPro-compatible
//  two-prong hinge on the TOP wall (pitch-adjustable, locked with an M5
//  thumbscrew) plus a printable wall BRACKET (three prongs, screw/keyhole/
//  tripod-nut fixing) — an upgrade over friction-only fold-out stands:
//  optional radial DETENT TEETH on the mating faces make the set angle
//  sag-proof. Weather mode seals the plate on a printed TPU gasket in the
//  ledge, glands an O-ring under every plate screw head, and frames the USB
//  openings for a silicone plug.
//
//  Units: millimeters.  CAD: OpenSCAD (https://openscad.org).
//
//  ⚠️ VERIFY BEFORE PRINTING. Board dimensions are nominal (DevKitM-1
//     ~39.0 x 25.4, Vision AI V2 40 x 20, OV5647 carrier 25 x 24 with the
//     Pi-cam v1.3 21 x 12.5 mm M2 hole grid). Measure YOUR boards — DevKit
//     revisions differ, and soldered pin headers need a taller standoff.
//
//  Orientation: model +Y = up on the wall, USB opening on the BOTTOM (-Y)
//  wall, prongs on the TOP wall, +Z = toward the front face.
//
//  Assembled frame (what vision_fitcheck and the generators read):
//
//        z = base_d + lid_t   face, show side (the bed, face-down)
//        z = base_d           face underside; posts hang from here ...
//        z = floor_t + 0.2    ... to pl_relief() over the ledge plane
//        z = floor_t          THE LEDGE = the plate's front face = the seal line
//        z = -mount_extra     back face: the shell's walls end and the plate's
//                             back shows through — the only seam, a hairline
//
//  v0.3 (2026-08-23): canary_*_lib adoption (dedup, mesh-identical); both USB
//  cuts gain the WAP's bridge-safe chamfered top (print fix, same w x h);
//  opt_mark debosses the house mark on the front in place of label_text.
//  v0.4 (2026-09-03): assembly review — every released mesh moves, for cause:
//    * the front's pan-head counterbore was as deep as the front (2.0 in
//      2.0): a Ø4.6 through-hole the Ø4 head fell through, so the screws
//      clamped nothing and the gasket had no preload. A pad on the inside
//      now carries a 1.0 mm floor under each head (posts shorten to suit);
//    * USB openings center on the connector AXIS (shell/2 above the PCB),
//      not on PCB-top + h/2 — 1.6 mm of every cable boot landed in the wall;
//    * the XIAO's port is DERIVED from the seated stack (its outward face
//      carries the USB-C shell), the stack reads the registry's measured
//      6.5, and the XIAO gets xiao_below of air for a plug's overmold — the
//      1.5 mm it had could not pass a plug at all;
//    * the hinge fins start on the bed in the weather preset (they floated
//      3 mm over the keyhole slab); the skirt's hinge notch only opens when
//      the fins need it; the Pi-cam lens holder (8.5 sq, 5.5 tall) gets the
//      post height it needs instead of hitting the front at 4.0;
//    * seal mode: 1.2 mm cheeks either side of the groove (were 0.8), an
//      opt_weep drain (ON in the weather preset), seal_mid_posts for the
//      long walls, head_seal O-ring glands under the screw heads;
//    * selectable fastener (screw_size / screw_head — core lib registry),
//      hinge_clear on the bracket slots, cs 90° cone for the bracket's
//      flat-head wall screws (was a 98° cone), M5 nut pocket 8.4 AF, and
//      the tripod nut's 0.6 mm web is now 2.9.
//  v0.4 follow-up (2026-09-08): the rain hood is its OWN part (part="hood").
//      Grown on the front it had no printable pose — face-down it stood on
//      9 mm of hood, face-up the whole inner face was an unsupported ceiling
//      over four post tips (32.7 mm2 of first layer under a 16.8 mm part).
//      The front now carries a hood_seat groove on its show face and prints
//      face-down again in every preset; the collar presses in and bonds.
//  v0.5 (2026-09-24): the PISTON PLATE — the parting line moves to the back
//      face (canary_core_lib pl_*, the Sense is the reference port):
//    * the front lid + back shell + lip + side seam are gone. The shell is
//      face + walls + posts in one piece; the back is a plate that nests in
//      a bore inside the walls (bore = inner + 2*ledge_w, plate = bore -
//      2*tol_slide, so the WALLS carry lateral load) and seats on a ledge at
//      z = floor_t. The posts stop pl_relief() short of the ledge: the ledge
//      (the seal line) is the datum, the screws pull toward posts they never
//      reach. Screws are the shortest standard length that engages
//      hw_engage() in blind pilots (M2 x 8), through seats recessed into the
//      plate; in seal mode an O-ring glands under every pan head, always;
//    * knobs retired: lip_h, lip_t, screw_from (there is only the plate),
//      head_seal (a sealed plate always glands), skirt_h / skirt_t (no side
//      seam to shed water off). The seam reveal goes with the seam;
//    * the boards ride the plate: module rails, clips and corner pins, the
//      DevKit's pads and clips, the keyholes and knockouts. The camera board
//      stays on posts under the face (the lens is registered to its window
//      by the part that carries the window); the stack heights are the old
//      floor's, so nothing between the boards moved;
//    * the XIAO port's bottom edge keeps a web over the ledge — and in seal
//      mode over the gasket groove IN the ledge, which is now below every
//      opening instead of above them: xiao_below grows to suit (derived,
//      echoed) so the firmware port never opens into the seal line;
//    * the weep leaves the bottom wall straight (tilt 0) just above the
//      groove — a 30° dive from the old floor corner would now bore through
//      the gasket;
//    * mid posts are DERIVED from the clamp-spacing rule (≤ 40 mm between
//      screw centers along any wall in seal mode, DESIGN_RULES §6): each
//      wall gets as many as its span needs, evenly spaced. seal_mid_posts
//      keeps forcing one per ±X wall (the weather preset derives the same
//      post). A devkit + seal build gets one on each ±Y wall too, standing in
//      the column gap, which grows to seat it;
//    * the hinge fins print face-down as teardrops whose crown lands ON the
//      bed (the face plane) and whose root-to-crown edge climbs at 45° —
//      supportless in every preset, including the shallow devkit body;
//    * the USB openings' bridge chamfers face the BACK (the opening's roof in
//      the face-down print).
//  2026-09-26: inner_y is vision_inner_y(host) and the keyhole pockets
//              vision_kh_ys(host) — the same derivation as before, now callable
//              per host so the outlet cradle reads vision_kh_spread("xiao") /
//              ("devkit") through `use <>` instead of retyping them (its table
//              had the xiao spread 0.4 stale). No geometry moved.
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, tearbore_x, soft_edge_plate,
                             // foot_chamfer_ring, the pl_* plate — the catalog's shared helpers
use <canary_mount_lib.scad>  // the stud/keyhole hanging standard; the pocket
                             // is drawn natively per axis (no rotate)
use <canary_snap_lib.scad>   // snap-fit doctrine — snap_strain() gates edgeclip
use <canary_port_lib.scad>   // connector openings — the bridge-safe USB profile
use <canary_board_lib.scad>  // board registry — the knob defaults below cite it
use <canary_mark_lib.scad>   // THE BIRD — opt_mark's front deboss
use <canary_rib_lib.scad>    // corner_gusset — the constant-width post web
use <canary_color_lib.scad>  // the colorway registry — assembled-preview spools

/* [What to render] */
part   = "all";       // ["back","front","all","gasket","bracket","knob","hood"]

/* [Preset] — quick configs; choose "custom" to use the option checkboxes */
preset = "custom";    // ["custom","vision_indoor","vision_weather"]

/* [Options (applied when preset = custom)] */
opt_led    = true;    // status LED        -> light-pipe port on the front
opt_buzzer = false;   // piezo buzzer      -> shares the vent cluster (firmware: unpopulated on Vision)
opt_vent   = false;   // GORE vent cluster -> pressure equalization (recommended with opt_seal)
opt_tamper = false;   // reed/Hall + magnet -> magnet pocket on the front underside
opt_hood   = false;   // rain/glare hood over the lens window — its OWN part (part="hood"), pressed into a groove on the front and bonded
opt_seal   = false;   // perimeter TPU gasket under the plate + O-rings under the plate screws + USB plug recess (splash-resistant, NOT immersion)
opt_mount  = true;    // mounting features per mount_style
mount_style = "hinge"; // ["hinge","keyhole","both"]
opt_weep   = false;   // Ø2 weep at the cavity's low point (bottom wall, beside the USB): condensate leaves (ON in the weather preset)
seal_mid_posts = false; // (seal mode) one extra screw post mid-way along each long wall (ON in the weather preset): four corner
                        // screws cannot hold 20 % gasket squeeze across a 60 mm span — the 40 mm rule derives it anyway

// effective flags (a preset overrides the checkboxes above)
function _pre(c, i, w) = (preset == "vision_indoor") ? i
                       : (preset == "vision_weather") ? w : c;
e_led    = _pre(opt_led,    true,  true);
e_buzzer = _pre(opt_buzzer, false, false);
e_vent   = _pre(opt_vent,   false, true);
e_tamper = _pre(opt_tamper, false, false);
e_hood   = _pre(opt_hood,   false, true);
e_seal   = _pre(opt_seal,   false, true);
e_mount  = _pre(opt_mount,  true,  true);
e_weep   = _pre(opt_weep,   false, true);
// the weather preset's gasket spans 66 mm between corner screws — past the
// 40 mm clamp-spacing rule (DESIGN_RULES §6) — so it carries the mid posts
e_midposts = _pre(seal_mid_posts, false, true);
m_style  = _pre(mount_style, "hinge", "both");

/* [Boards] — measure YOURS; these are nominal, and the registry
   (canary_board_lib) is where each number's evidence lives */
host     = "xiao"; // ["xiao","devkit"]  stacked XIAO (recommended) or Grove-cabled DevKitM-1
dk_l     = 39.0;   // ESP32-C3-DevKitM-1 length (Y, USB end down) — devkit host — brd_l("dk_c3")
dk_w     = 25.4;   // DevKitM-1 width (X) — brd_w("dk_c3")
vm_l     = 40.0;   // Grove Vision AI V2 long side (Y; USB edge down) — measured, brd_l("grove_v2")
vm_w     = 20.0;   // Grove Vision AI V2 short side (X) — measured (was 25x25: the
                   // module is the Grove 1x2 form, not a square — same fix as the
                   // doorbell; board_selfcheck() pins the old number dead)
xiao_l   = 21.0;   // stacked XIAO (Y) — xiao host — brd_l("xiao")
xiao_w   = 17.8;   // stacked XIAO (X) — the MEASURED board (brd_xiao_w_measured()); sets the corner pins
stack_sock_h = 6.5;  // module underside -> XIAO underside when seated (socket + headers):
                     // the registry's MEASURED 6.5 (brd_stack_sock_measured, doorbell bench).
                     // Was 11.5 "roomier headroom": the XIAO's port is derived from this
                     // number now, so a guess here is a port in the wrong wall — MEASURE yours
xiao_below   = 5.5;  // air under the XIAO's outward face: its USB-C shell (3.3) plus half a plug's
                     // overmold (6.5/2) below the shell axis plus clearance. The 1.5 it had could
                     // not pass a plug. Seal mode raises it as far as the gasket groove needs (echoed)
vm_front_h   = 5.0;  // module front-side component height (CSI connector etc.)
cam_w    = 25.0;   // OV5647 carrier width (X) — brd_w("ov5647")
cam_h    = 24.0;   // OV5647 carrier height (Y) — brd_l("ov5647")
pcb_t    = 1.0;    // PCB thickness (clips hook over this) — brd_t("grove_v2")
board_clear = 0.6; // per-side clearance around each PCB
stack_h  = 9.0;    // tallest top-side component over a PCB (Grove socket / USB boot) — devkit host

/* [Camera mounting] — Pi-cam v1.3 pattern on the FRONT face */
cam_hole_x = 21.0;  // hole grid (X)
cam_hole_y = 12.5;  // hole grid (Y)
cam_post_d = 3.6;   // camera post diameter
cam_post_h = 4.0;   // post height = lens-board standoff from the front face — auto-raised to
                    // clear the lens holder (see cam_lens_h)
cam_lens_h = 5.5;   // Pi-cam v1.3 lens holder height above the PCB face (drawing: 9.0 module
                    // - 1.0 PCB - 2.5 FPC connector) — MEASURE yours
cam_lens_sq = 8.5;  // the holder's square (its 12.0 diagonal cannot enter a Ø10 aperture, so
                    // the posts must hold the whole holder behind the front)
cam_screw_d = 1.6;  // M2 self-tap pilot in the posts
lens_dx   = 0.0;    // lens center offset from the camera-board center — MEASURE
lens_dy   = 2.5;    // (v1.3 lens sits ~2.5 mm above board center)
cam_ap_d  = 10.0;   // lens aperture in the front face — asserted against cam_fov
cam_fov   = 62;     // lens diagonal field of view (OV5647-62; the 160° fisheye needs no hood)  // [40:1:160]
cam_disc_d = 14.0;  // clear-disc seat diameter (12-16 mm disc; 0 = bare aperture)
cam_disc_t = 1.0;   // clear-disc thickness (disc sits 0.2 recessed)

/* [Shell] */
wall_t   = 2.0;    // side wall thickness (auto-thickened to host the plate's ledge, and the gasket in seal mode) — catalog default, core_wall()
floor_t  = 2.0;    // the plate's thickness under the boards (the plate grows below z = 0 for the screw heads and the keyhole slab)
lid_t    = 2.0;    // front face thickness
corner_r = 3.0;    // outside corner radius
floor_cove = 0.8;  // 45° cove where the walls meet the face inside, and where they meet the ledge (canary_core_lib pl_cavity_cut); 0 = square corners  // [0:0.2:1.2]
                   // The sharp notch there was the crack-starter in every flat-printed shell — a corner drop
                   // hinges the floor about it along one layer boundary.
lid_key    = true; // poka-yoke: a rib on the +Y bore wall and a slot in the plate's edge — four corner posts fit
                   // a plate two ways and every plate feature lines up one way; turned round it stands proud on the rib
standoff_h = 3.5;  // PCBs sit this high off the plate (RAISE if DevKit has soldered pin headers!)
                   // 3.5, not 3.0: the devkit clip beam is standoff + PCB, and at 4.5 mm it
                   // inserts at 3.7 % strain — 4.0 ran 4.7 % against the 4.5 % budget
cav_extra  = 1.0;  // headroom above the tallest component

/* [Print tolerances] — per-side clearances; tune once for your printer
   (defaults are the catalog trio — canary_core_lib core_tol_*(), dialed
   on the fit coupon) */
tol_slide = 0.20;  // sliding fits: the plate in its bore, the disc seat — core_tol_slide()
tol_press = 0.10;  // press fits: magnet, light pipe — core_tol_press()
tol_hole  = 0.30;  // clearance holes: plate screws, hinge bolt — core_tol_hole()

/* [Engineering — durability/rigidity options (see README "Engineering & materials")] */
screw_insert = false;   // M2 brass heat-set inserts in the post ends (service-grade threads)
insert_d     = 3.5;     // insert nominal OD (M2 short series: 3.5 x 4.0)
insert_h     = 4.0;     // insert length
lid_ribs     = true;    // perimeter rib ring under the front face (t³ stiffening against pry)
lid_rib_w    = 2.5;     // rib ring width
lid_rib_h    = 1.0;     // rib depth — keep <= cav_extra (the component headroom) or raise it
foot_cham    = 0.5;     // 45° chamfer on the shell's back edge: elephant-foot + delamination guard (0 = off)
kh_lock      = true;    // (keyhole mounts) anti-lift knockouts: 0.6 mm web, pierce with #4/M3 on install

/* [Screw posts] (the plate screws into the posts hanging from the face) */
post_d       = 5.0;   // corner screw post Ø — the plate screws thread into the post ends (auto-fattened for inserts and larger screws)
screw_size   = "m2";  // ["m2","m2.5","m3"] plate screw — the core lib's screw registry sets pilot, clearance,
                      // head seat, insert bore and post floor; "m2" keeps the validated numbers below
screw_head   = "pan"; // ["pan","flat"] the head in the bag: pan = flat-floored counterbore in the plate (the BOM's
                      // black-oxide M2 pan heads; what the seal-mode O-ring gland needs), flat = 90° countersink
screw_d      = 1.6;   // (m2) M2 self-tapping pilot
screw_head_d = 4.0;   // (m2 pan) head Ø
screw_head_h = 2.0;   // (m2 pan) head height; the plate is never thinner than a head and its 1.0 floor (pl_thick)

/* [USB-C ports] — on the BOTTOM (-Y) wall. devkit host: one opening (DevKit).
   xiao host: TWO stacked openings — module "model port" (upper) + XIAO
   "firmware port" (lower); both face the same edge (device guide §2).
   Both are cut with the WAP's bridge-safe profile (canary_port_lib). */
usb_w  = 12.0;     // opening width: clears rugged cable boots (USB-C body ~8.9 — port_usbc_shell_w())  // [9:0.5:14]
usb_h  = 6.5;      // opening height: boot clearance (the shell is 3.26)  // [4:0.5:8]
usb_z  = 0.0;          // extra lift relative to the connector AXIS (shell/2 above the PCB) — MEASURE
usb_dx = 0.0;          // upper/main port offset along the wall — MEASURE your boards (the module's
                       // USB-C sits beside its Grove socket on the same edge, so 0 is unlikely)
xiao_usb_dx   = 0.0;   // XIAO port offset along the wall
xiao_usb_z    = 0.0;   // extra lift of the XIAO port relative to its DERIVED axis (the XIAO's outward
                       // face minus half a shell) — a measured correction, not a position

/* [Hinge — GoPro-compatible two prongs on the TOP wall] */
prong_t     = 3.0;   // fin thickness (GoPro standard)
prong_pitch = 6.35;  // fin center spacing (GoPro standard)
fin_r       = 7.5;   // fin end radius
hinge_off   = 13.0;  // hinge axis stand-off from the top wall face
hinge_bolt_d = 5.0;  // M5 thumbscrew (GoPro standard)
hinge_clear = 0.15;  // extra slot width on the bracket for printed fins that run +0.1 oversize  // [0:0.05:0.4]
hinge_teeth = true;  // radial detent teeth on the mating faces — SAG-PROOF angle.
                     // Set false for smooth faces (full GoPro accessory compatibility).
teeth_n     = 24;    // castellation steps around the hinge, a tooth on every other one: it detents every 720 / teeth_n degrees (24 -> 12 positions, 30 degrees apart); keep it even
teeth_h     = 0.6;   // tooth height

/* [Bracket] — wall plate with three prongs (mates the case hinge) */
br_x        = 46.0;  // plate width (along the hinge axis)
br_y        = 34.0;  // plate height
br_t        = 4.0;   // plate thickness
br_screw_d  = 4.2;   // countersunk wall screws (#8 / M4)
bracket_tripod = true; // captive 1/4-20 nut pocket behind the center fin (tripod mount)

/* [Stud/keyhole interface] — blind keyhole pockets in a thickened plate (seal-safe) */
// the catalog's one hanging interface — canary_mount_lib owns the drawing and the numbers
kh_extra   = 3.0;    // plate thickening below z = 0 that hosts the keyhole pockets
kh_head_d  = 7.0;    // catalog standard — mount_kh_head_d()
kh_shank_d = 4.2;    // catalog standard — mount_kh_shank_d()
kh_slot_l  = 8.0;    // catalog standard — mount_kh_slot_l()
kh_head_h  = 3.5;    // total pocket depth (face web + head cavity) — mount_kh_head_h()
kh_face    = 1.0;    // catalog standard — mount_kh_face()
kh_inset   = 12.0;   // pocket centers at y = ±(inner_y/2 − kh_inset), on the X centerline

/* [Weather sealing] */
gasket_w      = 1.6;  // gasket groove width in the shell's ledge (the printed gasket is 0.5 narrower)
gasket_groove = 1.2;  // groove depth into the ledge
gasket_proud  = 0.3;  // how far the printed gasket stands proud of its groove, uncompressed — what the plate screws squeeze
usb_cover     = true; // (seal mode) shallow recess framing the USB opening(s) for a flanged silicone plug; the xiao host's one recess spans both ports
usb_cov_pad   = 2.0;  // recess margin around the USB opening(s)
usb_cov_dep   = 1.0;  // recess depth into the outer wall face
weep_d        = 2.0;  // weep bore (canary_core_lib weep_d)  // [1.5:0.5:3]
hood_len      = 9.0;  // rain-hood protrusion from the front face  // [5:0.5:15]
hood_t        = 1.8;  // hood wall thickness
hood_seat     = 0.6;  // groove in the front's show face the hood's spigot presses into (tol_press); bond with neutral-cure silicone  // [0.4:0.1:1.0]
                      // The hood is its OWN part: a hood grown on the front had no printable orientation
                      // (face-down it stood on 9 mm of hood; face-up the whole 4,000 mm2 inner face was an
                      // unsupported ceiling over four post tips)

/* [Front-face features] — offsets are measured FROM THE MODULE CENTER so they
   stay valid for both hosts. Measure your build! */
lp_d   = 3.0;      // light-pipe diameter (hole = lp_d + 2*tol_press)
lp_dx  = 8.0;      // light-pipe port center X, from the module center
lp_dy  = -8.0;     // light-pipe port center Y, from the module center
vent_pad_d     = 12.0;  // GORE seat (outer face)
vent_pad_depth = 0.8;   // GORE-seat recess depth into the front's outer face — core_vent_pad_depth()
vent_hole_d    = 1.0;   // fine holes — insect-resistant (the README's outdoor rule: <= 1.0 mm)
vent_ring_d    = 6.0;   // Ø of the ring the vent holes sit on — core_vent_ring_d()
vent_holes     = 10;    // more, smaller holes recover the open area at 1.0 mm
vent_dx        = -8.0;  // vent cluster center X, from the module center
vent_dy        = -8.0;  // vent cluster center Y, from the module center
mag_d  = 6.0;      // tamper MAGNET diameter (pocket = mag_d + 2*tol_press)
mag_h  = 3.2;      // magnet thickness, and the pocket ring's height off the front's inside face
mag_dx = 8.0;      // magnet pocket center X, from the module center
mag_dy = 8.0;      // magnet pocket center Y, from the module center

/* [Board snap clips] — the WAP's print-proven numbers (the canary_snap_lib
   snap_boardclip defaults); the strain gate in edgeclip() holds them honest */
clip_w      = 6.0;   // board-clip tab width along the board edge — snap_boardclip default
clip_t      = 1.0;   // clip beam thickness — snap_boardclip default; edgeclip() asserts its insertion strain
clip_hook   = 0.5;   // lip overhang over the board top — snap_boardclip default
clip_hook_h = 1.2;   // lip + 45° lead-in height above the board top — snap_boardclip default
clip_clear  = 0.25;  // beam face to board edge (a fit — tune on the coupon) — snap_boardclip default

/* [Aesthetics] */
colorway    = "graphite"; // ["graphite","canary","snow","forest","midnight"] assembled-preview spool set (canary_color_lib; single-part exports carry no color)
lid_edge    = 0.8;   // 45° chamfer around the front's top edge  // [0:0.1:1.5]
lid_edge2   = 0.8;   // second (~66°) stage of the show-face edge, mm — ON is the house look (core_face_edge2()); it is what reads as a roundover instead of a bevel. 0 leaves the plain 45° facet any CAD default gives you  // [0:0.1:1.5]
label_text  = "";    // debossed front label ("" = off; needs the font installed)
label_size  = 5.0;   // label text height
label_depth = 0.5;   // deboss depth (prints as crisp first-layer voids — the front prints face-down)
label_dx    = 0.0;   // label center X offset from the FRONT's center (not the module center)
label_dy    = -14.0; // label center Y offset from the FRONT's center
label_rot   = 0;     // label rotation (degrees)
label_font  = "Liberation Sans:style=Bold";  // the font label_text is set in (it must be installed)
// The bird lands at the label position; exclusive with label_text — the
// front carries one identity, not two.
opt_mark    = false; // deboss the house mark (the canary_mark_lib bird) instead of a custom label
mark_h      = 16.0;  // bird height; the lib floors it at mark_min_h(mark_rib)
                     // rather than let the mark print as mush
mark_rib    = 0.7;   // mark stroke width

/* [Quality] */
// curve quality: $fa/$fs give smooth big arcs (pill corners, hood) without
// exploding tiny holes into thousands of facets like a large $fn would
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
// the piston plate (canary_core_lib pl_*): the plate seats on a ledge inside
// the walls and the parting line is the back face itself
ledge_w  = pl_ledge(e_seal, gasket_w, tol_slide);                   // gasket + a cheek each side, or one contact band
wall_eff = max(wall_t, ledge_w + core_min_wall());       // the skin outside the plate's bore stays a structural wall
clip_stack = clip_clear + clip_t;
// the fastener, resolved once from the core lib's registry (the M2 rows are
// this file's validated numbers, so a default render reads the knobs)
scr_d   = (screw_size == "m2") ? screw_d : scr_pilot(screw_size);
scr_c   = max(scr_d + 2*tol_hole, scr_clear(screw_size));
head_d  = (screw_size == "m2" && screw_head == "pan") ? screw_head_d
        : (screw_head == "pan") ? scr_pan_d(screw_size) : scr_flat_d(screw_size);
head_h  = (screw_size == "m2" && screw_head == "pan") ? screw_head_h
        : (screw_head == "pan") ? scr_pan_h(screw_size) : scr_flat_h(screw_size);
ins_od  = (screw_size == "m2") ? insert_d : scr_insert_d(screw_size) + 0.3;
ins_h   = (screw_size == "m2") ? insert_h : scr_insert_h(screw_size);
pd = max(screw_insert ? max(post_d, ins_od + 3.0) : post_d,   // >=1.5 mm wall around an insert
         scr_post_min(screw_size));
// a sealed build seats an O-ring under every plate screw head: the seat is a
// hole through the seal line from outside (canary_core_lib pl_seat_cut)
e_gland  = e_seal;
post_corner = pd + 1.5;
post_in     = pd/2 - 0.2;          // a post center's inset from the cavity wall (the post sits 0.2 INTO it)
has_dk = (host == "devkit");

// xiao host: the module rides tall rails so the stacked XIAO hangs beneath it,
// with xiao_below of air under the XIAO's outward (USB) face. The port's
// bottom edge keeps a web over the plate's front face — and in seal mode over
// the gasket groove in the ledge below it, which is the seal line now: an
// opening that broke into it would drain straight onto the gasket. So
// xiao_below grows to what the groove needs (echoed below when it does)
xiao_port_web  = e_seal ? gasket_groove + core_min_web() : 0.6;
xiao_below_eff = max(xiao_below, port_usbc_shell_h()/2 + usb_h/2 + xiao_port_web - xiao_usb_z);
vm_standoff = has_dk ? standoff_h : stack_sock_h + xiao_below_eff;
// the bottom margin: the ledge's cove must not land on the board's edge strip
bot_margin  = max(board_clear, floor_cove + 0.4);

// devkit host: two columns (camera+module | DevKit); xiao host: one column.
// Posts always sit in true X-margins beside the boards.
col_cam = max(cam_w + 2*board_clear, vm_w + 2*(clip_stack + board_clear) + 0.5);
col_dk  = dk_w + 2*(clip_stack + board_clear) + 0.5;
// the clamp-spacing rule (DESIGN_RULES §6): in seal mode no more than 40 mm
// between screw centers along any wall. A devkit body's ±Y walls are ~66 mm
// between the corner screws, so they get a mid post each — standing in the
// gap between the two board columns, which grows from 2.0 to seat it
span_max = 40;
inner_x0 = col_cam + 2.0 + col_dk + 2*post_corner;     // the devkit body at the plain 2.0 column gap
dk_mid   = has_dk && e_seal && (inner_x0 - 2*post_in > span_max);
mid_gap  = dk_mid ? pd + 1.0 : 2.0;   // devkit host: gap between the camera + module column and the DevKit column
inner_x = has_dk ? col_cam + mid_gap + col_dk + 2*post_corner
                 : col_cam + 2*post_corner;
// the cavity height is a function of the host so a fitment can read either
// host's keyhole spread through `use <>` (the outlet cradle) — this file's
// own inner_y is the same function at its own host, never a second derivation
function vision_inner_y(h = host) =
    (h == "devkit") ? max(3 + cam_h + 2 + vm_l + 1.5 + 1.5, dk_l + bot_margin + board_clear + 6.0)
                     : bot_margin + vm_l + 2 + cam_h + 3;   // module at the USB wall, camera above
inner_y = vision_inner_y(host);

// cavity depth: xiao host is driven by the rail height + module front parts;
// devkit host by the tallest top-side component
cav_d_min = has_dk ? standoff_h + pcb_t + stack_h + cav_extra
                   : vm_standoff + pcb_t + vm_front_h + cav_extra;
usb_soff  = has_dk ? standoff_h : vm_standoff;            // standoff of the board that owns the USB wall
usb_axis = usb_soff + pcb_t + port_usbc_shell_h()/2 + usb_z;         // connector axis above the plate's front face
// wall guaranteed above the opening: 1.5 in every mode; seal mode adds the plug recess frame
usb_over = 1.5 + (e_seal && usb_cover ? usb_cov_pad : 0);
cav_d   = max(cav_d_min, usb_axis + usb_h/2 + usb_over);
// the Pi-cam lens holder: a square that cannot enter the round aperture must
// sit wholly behind the front, so the posts grow to hold it there
cam_post_eff = (cam_ap_d >= cam_lens_sq*1.4142 + 0.6) ? cam_post_h : max(cam_post_h, cam_lens_h + 0.3);

out_x  = inner_x + 2*wall_eff;
out_y  = inner_y + 2*wall_eff;
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
// A gate that can only pass has proved nothing; this is the case it must fail.
module vision_fitcheck(lift = 0.1, turned = false) {
    intersection() { translate([0, 0, base_d + lift]) rotate([0, 0, turned ? 180 : 0]) front(); back(); }
}

cam_cx = has_dk ? -inner_x/2 + post_corner + col_cam/2 : 0;
dk_cx  =  inner_x/2 - post_corner - col_dk/2;                 // devkit host only
gap_cx = -inner_x/2 + post_corner + col_cam + mid_gap/2;      // devkit host: the column gap's center
vm_cx  = cam_cx;
vm_cy_x = -inner_y/2 + bot_margin + vm_l/2;                   // xiao host: module at the bottom wall
cam_cy = has_dk ? inner_y/2 - 3 - cam_h/2                     // camera at the top (3 mm margin)
                : vm_cy_x + vm_l/2 + 2 + cam_h/2;
vm_cy  = has_dk ? cam_cy - cam_h/2 - 2 - vm_l/2 : vm_cy_x;
dk_cy  = -inner_y/2 + bot_margin + dk_l/2;                    // DevKit parked at the USB (bottom) wall
lens_x = cam_cx + lens_dx;
lens_y = cam_cy + lens_dy;
usb_cx = (has_dk ? dk_cx : vm_cx) + usb_dx;                   // main/upper USB opening center (X)
usb_zc = floor_t + usb_axis;                                  // opening centered on the connector axis
// xiao host: the XIAO's USB-C hangs off its outward face, which is stack_sock_h
// below the module — its axis is half a shell below that face
xiao_usb_zc = floor_t + vm_standoff - stack_sock_h - port_usbc_shell_h()/2 + xiao_usb_z;
xiao_usb_cx = vm_cx + xiao_usb_dx;

mount_extra0 = (e_mount && (m_style == "keyhole" || m_style == "both")) ? kh_extra : 0;
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
assert(!screw_insert || ins_h + 1.0 <= post_h, "the post is shorter than the insert it must hold");
function vision_kh_ys(h = host) = let(k = vision_inner_y(h)/2 - kh_inset)
    (k >= kh_slot_l/2 + kh_head_d/2 + 2) ? [-k, k] : [0];
kh_y  = inner_y/2 - kh_inset;
kh_ys = vision_kh_ys(host);
// the pocket spread a fitment hangs this case by (0 = one centered pocket)
function vision_kh_spread(h = host) = let(ks = vision_kh_ys(h)) len(ks) == 2 ? ks[1] - ks[0] : 0;
hinge_hole = hinge_bolt_d + tol_hole + 0.1;       // ~5.4 for M5: free pivot

// mid posts, DERIVED from the clamp-spacing rule: each wall gets as many as
// its span between corner screw centers needs, evenly spaced; seal_mid_posts
// forces one per ±X wall (the weather preset's setting — its 66 mm span
// derives the same post). The ±Y walls only ever need one, and only on a
// devkit body, where it stands in the column gap (dk_mid)
span_x  = inner_y - 2*post_in;                    // along a ±X wall
span_y  = inner_x - 2*post_in;                    // along a ±Y wall
n_mid_x = e_seal ? max(e_midposts ? 1 : 0, ceil(span_x/span_max) - 1) : 0;
n_mid_y = !e_seal ? 0 : has_dk ? (dk_mid ? 1 : 0) : ceil(span_y/span_max) - 1;
function _mids(n, half) = n <= 0 ? [] : [for (i = [1 : n]) -half + i*(2*half)/(n + 1)];
mid_ys = _mids(n_mid_x, inner_y/2 - post_in);
mid_xs = has_dk ? (dk_mid ? [gap_cx] : []) : _mids(n_mid_y, inner_x/2 - post_in);
// screw centers: the four corners, then the ±X wall mids, then the ±Y wall mids
function post_xy() = concat([
    [ inner_x/2 - post_in,  inner_y/2 - post_in],
    [-inner_x/2 + post_in,  inner_y/2 - post_in],
    [ inner_x/2 - post_in, -inner_y/2 + post_in],
    [-inner_x/2 + post_in, -inner_y/2 + post_in],
], [for (sx = [1, -1], y = mid_ys) [sx*(inner_x/2 - post_in), y]],
   [for (sy = [1, -1], x = mid_xs) [x, sy*(inner_y/2 - post_in)]]);
// which wall(s) a post stands on — a corner gussets to both, a mid post to its own
function _on_x(p) = abs(abs(p[0]) - (inner_x/2 - post_in)) < 0.01;
function _on_y(p) = abs(abs(p[1]) - (inner_y/2 - post_in)) < 0.01;
// the longest gasket span the derived posts leave along each wall
function _max_gap(v) = max([for (i = [1 : len(v) - 1]) v[i] - v[i - 1]]);
_stations_x = concat([-(inner_y/2 - post_in)], mid_ys, [inner_y/2 - post_in]);
_stations_y = concat([-(inner_x/2 - post_in)], mid_xs, [inner_x/2 - post_in]);
gasket_span = max(_max_gap(_stations_x), _max_gap(_stations_y));
assert(!e_seal || gasket_span <= span_max + 1e-9,
       str("seal mode: ", gasket_span, " mm between screw centers along a wall — past the ", span_max,
           " mm clamp-spacing rule (DESIGN_RULES §6)"));
// a ±Y mid post shares the bottom wall with the USB opening(s) and the top
// wall with the key rib and the hinge root: it must clear them
assert(!e_seal || len(mid_xs) == 0
       || min([for (x = mid_xs) min(abs(x - usb_cx), has_dk ? 99 : abs(x - xiao_usb_cx))]) >= usb_w/2 + pd/2 + 1.0,
       "a ±Y mid post lands on a USB opening — move usb_dx / xiao_usb_dx or widen the body");
key_x = inner_x/2 - post_corner - 2.5;   // plate key: on the +Y bore wall, inboard of the +X corner post
assert(!e_seal || len(mid_xs) == 0 || min([for (x = mid_xs) abs(x - key_x)]) >= pd/2 + core_key_w()/2 + 1.0,
       "a +Y mid post lands on the plate key rib");

// the hinge fin prints on the shell, face-down: whatever faces the face is a
// bed-facing overhang. The fin is a teardrop knuckle whose crown lands ON
// the bed (the face's show plane), and the root stays low enough (fin_root)
// that the edge from the root to the crown climbs at 45° — supportless. The
// far flank, from the crown down to the knuckle, is steeper still. The root
// is what carries the case: a 3 x fin_root PETG section sees under 1 MPa
// from the case's weight on the bolt's lever
fin_pt   = base_d + lid_t;                        // the crown, on the bed
fin_root = fin_pt - hinge_off;                    // the 45° line from the crown back to the wall
assert(fin_pt - fin_r >= fin_r*sqrt(2) - 1e-9,
       str("the body is too shallow (", fin_pt, " mm) for the hinge fin's 45° crown over a ", fin_r,
           " mm knuckle — lower fin_r"));
assert(fin_root >= 2.0, "the hinge fin's root is too low for its stand-off — lower hinge_off");

// sanity checks + a measurable size echo for scripted verification
assert(wall_t > 0 && floor_t > 0 && lid_t > 0, "shell thicknesses must be positive");
assert(standoff_h > 0, "standoff_h must be positive");
assert(screw_head_d > screw_d, "screw_head_d must be larger than screw_d");
assert(head_d > scr_c, "the screw head must be larger than its clearance hole, or it falls through the plate");
lid_headroom = cav_extra + (cav_d - cav_d_min);
assert(!lid_ribs || lid_rib_h <= lid_headroom,
       str("lid_rib_h ", lid_rib_h, " reaches past the ", lid_headroom, " mm headroom over the stack — the ribs land on a component"));
assert(!lid_ribs || lid_rib_w >= core_min_wall(), "lid_rib_w is under the structural wall floor");
// a sealed box needs a pressure path (a vent membrane or a weep) or it pumps
// air past the gasket on every thermal cycle — field_ratings.md
assert(!e_seal || e_vent || e_buzzer || e_weep,
       "seal mode with no pressure path — enable opt_vent (GORE seat) or opt_weep");
assert(!e_seal || gasket_groove + 0.5 <= cav_d, "gasket_groove runs out of the ledge");
assert(!e_seal || core_gasket_fill(gasket_w, gasket_groove, gasket_proud) <= core_gasket_fill_max(),
       str("the printed TPU ring would fill ", round(100*core_gasket_fill(gasket_w, gasket_groove, gasket_proud)),
           " % of its groove - past ", round(100*core_gasket_fill_max()),
           " % the incompressible gasket props the plate open instead of sealing; narrow gasket_w or deepen gasket_groove"));
assert(cam_disc_t == 0 || cam_disc_t + 0.2 < lid_t, "cam_disc_t too thick for lid_t");
assert(cam_disc_d == 0 || cam_disc_d > cam_ap_d, "cam_disc_d must be larger than cam_ap_d");
// the aperture at the disc's inner plane must pass the lens's field of view
// (lens front sits cam_post_eff - cam_lens_h behind the front's inner face)
cam_throw = (cam_post_eff - cam_lens_h) + lid_t - (cam_disc_t > 0 ? cam_disc_t + 0.2 : 0);
cam_need  = 2*max(0, cam_throw)*tan(cam_fov/2) + 4.0;
assert(cam_ap_d >= cam_need,
       str("lens aperture ", cam_ap_d, " mm vignettes a ", cam_fov, "° lens ", cam_throw,
           " mm behind the disc — needs ", round(cam_need*10)/10, " mm"));
assert(!e_hood || atan((cam_disc_d/2 + 2.5)/(hood_len + cam_throw)) > cam_fov/2 + 3,
       str("the rain hood clips a ", cam_fov, "° lens — shorten hood_len or widen cam_disc_d"));
// the hood's groove must leave a floor under it (it sits over the rib ring and the
// disc seat's land) and must not run into the disc seat cut from the same face
assert(!e_hood || hood_seat + 0.8 <= lid_t, "hood_seat leaves under 0.8 mm of front beneath the hood groove — thicken lid_t");
assert(!e_hood || cam_disc_d/2 + 2.5 - hood_t/2 >= cam_disc_d/2 + tol_slide + 1.0,
       "the hood groove runs into the clear-disc seat");
// the camera board hangs under the face; the module's front parts stand up
// from the plate — the two must never share a column of air. They are apart
// in Y by construction (camera above the module), but the sum is the gate
assert(base_d - cam_post_eff - pcb_t >= floor_t + vm_standoff + pcb_t + vm_front_h - 1e-9
       || cam_cy - cam_h/2 - board_clear >= vm_cy + vm_l/2 + board_clear - 1e-9,
       "the camera board and the module's front parts overlap in Z and in Y — raise cav_extra");
assert(host == "devkit" || usb_zc - xiao_usb_zc >= usb_h + 1.2,
       "the module and XIAO USB openings merge — no web left between them (stack_sock_h / xiao_below)");
assert(host == "devkit" || xiao_usb_zc - usb_h/2 >= floor_t + xiao_port_web - 1e-9,
       "the XIAO port opening breaches the web over the ledge — raise xiao_below");
assert(has_dk || !e_seal || usb_zc - usb_h/2 >= floor_t + gasket_groove + core_min_web() - 1e-9,
       "the USB opening breaches the web over the gasket groove — raise standoff_h");
assert(mount_extra0 == 0 || kh_head_h + 1.5 <= floor_t + kh_extra, "keyhole pocket too deep — raise kh_extra");
assert(mount_extra0 == 0 || kh_head_h > kh_face, "kh_head_h must exceed kh_face");
assert(mount_extra0 == 0 || kh_head_d > kh_shank_d, "kh_head_d must be larger than kh_shank_d");
assert(lid_edge == 0 || (lid_edge >= 0.01 && lid_edge < lid_t), "lid_edge must be 0, or between 0.01 and lid_t");
assert(lid_edge2 >= 0 && (lid_edge > 0 || lid_edge2 == 0) && lid_edge + lid_edge2 < lid_t,
       "lid_edge2 requires lid_edge > 0, and their sum must stay below lid_t");
assert(!(opt_mark && label_text != ""),
       "opt_mark and label_text are exclusive — the front carries the mark or a label, not both");
assert((label_text == "" && !opt_mark) || (label_depth > 0 && label_depth < lid_t),
       "label_depth must be between 0 and lid_t");
// the hardware, DERIVED from the same knobs that draw the holes (canary_core_lib)
hw_thread = screw_insert ? "machine (into the inserts)" : "self-tap";
hw_echo(str("Vision ", host), [
    hw_item(len(post_xy()), str(hw_screw(screw_size, screw_head, pl_L, hw_thread), " (plate to the post ends)")),
    hw_item(4, "M2 pan x 6 self-tap (OV5647 to the front posts)"),
    screw_insert ? hw_item(len(post_xy()), str(hw_size_name(screw_size), " heat-set insert ", ins_od, " OD x ", ins_h)) : "",
    e_gland      ? hw_item(len(post_xy()), hw_oring(screw_size)) : "",
    e_seal       ? hw_item(1, "TPU gasket (print part=\"gasket\")") : "",
    e_vent       ? hw_item(1, str("Ø", vent_pad_d, " adhesive ePTFE/GORE vent patch")) : "",
    e_led        ? hw_item(1, str("Ø", lp_d, " light pipe")) : "",
    cam_disc_t > 0 && cam_disc_d > 0 ? hw_item(1, str("Ø", cam_disc_d, " x ", cam_disc_t, " clear disc (neutral-cure silicone)")) : "",
    e_hood       ? hw_item(1, "hood (print part=\"hood\"; bond into the front's groove)") : "",
    e_tamper     ? hw_item(1, str("Ø", mag_d, " x ", mag_h, " disc magnet (press + glue)")) : "",
    e_mount && (m_style == "hinge" || m_style == "both") ? hw_item(1, str("M", hinge_bolt_d, " x 25 bolt + nut (hinge; knob part=\"knob\")")) : "",
    e_mount && (m_style == "hinge" || m_style == "both") ? hw_item(4, "#6 pan wall screw (bracket)") : "",
    e_mount && (m_style == "keyhole" || m_style == "both") ? hw_item(2, "#6 pan wall screw (keyholes)") : "",
]);
echo(str("Canary Vision enclosure v0.5 — outer ", out_x, " x ", out_y, " x ",
         base_d + lid_t + mount_extra, " mm (+", hinge_off + fin_r,
         " mm prongs)  (host=", host, ", preset=", preset, ", seal=", e_seal, ", mount=", m_style, ")"));
echo(str("piston plate: ", plate_t, " mm plate in a ", bore_x, " x ", bore_y, " bore, ledge ", ledge_w,
         " wide, posts ", post_h, " tall with ", pl_pil, " mm pilots; ", len(post_xy()), " screws, longest span ",
         gasket_span, " mm"));
if (wall_eff > wall_t)
    echo(str("walls auto-thickened ", wall_t, " -> ", wall_eff, " mm to host the plate's ledge",
             e_seal ? " and the gasket groove" : ""));
if (!has_dk)
    echo(str("USB openings (bottom wall): module port axis ", usb_zc, " mm, XIAO port axis ", xiao_usb_zc,
             " mm above the back face, at x ", usb_cx, " / ", xiao_usb_cx, " — MEASURE both"));
if (!has_dk && xiao_below_eff > xiao_below)
    echo(str("xiao_below raised ", xiao_below, " -> ", xiao_below_eff,
             " mm so the XIAO port keeps a web over the gasket groove in the ledge"));
if (dk_mid)
    echo(str("seal mode, devkit host: the column gap grows 2.0 -> ", mid_gap, " mm to seat a mid post on each ±Y wall"));
if (e_hood)
    echo("opt_hood: the hood is its own part — render part=\"hood\", press its spigot into the front's groove and bond it (the front still prints face-down)");
// ----------------------------------------------------------------------------
//  Helpers — the idiom once shared by copy with canary_wap_enclosure.scad now
//  comes from the canary_*_lib set; what stays local is Vision-specific
//  (rim ring, cradles, the hinge, the clip)
// ----------------------------------------------------------------------------
// a ring of width w centered on the ledge band (the gasket's home)
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + ledge_w, inner_y + ledge_w, core_cav_r(corner_r, wall_eff) + ledge_w/2);
        offset(r = -w/2) rrect2d(inner_x + ledge_w, inner_y + ledge_w, core_cav_r(corner_r, wall_eff) + ledge_w/2);
    }
}

// ring pedestal that supports a PCB's underside along its perimeter
module ringped(cx, cy, l, w) {
    translate([cx, cy, floor_t - 0.01]) difference() {
        rrect(l - 1.5, w - 1.5, 1.5, standoff_h + 0.01);
        translate([0, 0, -0.5]) rrect(l - 7.5, w - 7.5, 1.0, standoff_h + 1);
    }
}

// Cantilever snap clip at edge point (px,py); `ang` = outward normal (degrees,
// pointing AWAY from the board); `soff` = board standoff (the rail/pedestal
// height the PCB sits on). Same proven profile as the WAP enclosure:
// flat retention seat over the board + 45° under-chamfer across the gap.
// Kept LOCAL rather than routed through canary_snap_lib's snap_boardclip:
// that module places clips by ±Y mirror only, and these stand on ±X board
// edges at an arbitrary normal. The lib still does the engineering —
// snap_strain() gates every instantiation. The xiao host's rail-top clips
// ride a 14 mm beam (0.4 % — nothing). The devkit host runs the profile on
// a 4.0 mm rise (standoff 3.0 + pcb_t 1.0; the WAP's 1.2 mm board makes its
// beam 4.2) and evaluates to 4.7 % — a hair past the catalog's 4.5 % once
// budget, far below the ~10 % that cracks vertical-print PETG. Released
// geometry a dedup must not move, so the gate pins today's worst case
// (+0.2 pp) and stops any edit that pushes past it; raising standoff_h to
// 3.5 (beam 4.5 -> 3.7 %) clears the budget properly on a reprint.
module edgeclip(px, py, ang, soff = standoff_h) {
    // the devkit's 4.0 mm beam ran 4.7 % (grandfathered +0.2 pp); the 3.5
    // standoff makes it 4.5 mm and 3.7 %, so the budget binds again
    eps = snap_strain(clip_t, clip_hook, soff + pcb_t);
    assert(eps <= snap_budget_once(),
           str("edgeclip: insertion strain ", round(eps*1000)/10,
               " % exceeds the ", round(snap_budget_once()*1000)/10,
               " % budget — thin clip_t, shorten clip_hook, or raise the standoff"));
    bt = floor_t + soff + pcb_t;
    tp = bt + clip_hook_h;
    pts = [ [clip_clear, floor_t - 0.01], [clip_clear + clip_t, floor_t - 0.01],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
}

// 12-of-24 radial castellation: interlocks with an identical facing ring
module teeth2d() {
    step = 360 / teeth_n;
    for (i = [0 : 2 : teeth_n - 1]) rotate([0, 0, i * step])
        intersection() {
            difference() { circle(r = fin_r - 0.5); circle(r = 3.5); }
            polygon([[0, 0], [2*fin_r, 0],
                     [2*fin_r*cos(step), 2*fin_r*sin(step)]]);
        }
}

// (the teardrop bore the hinge and bracket ride is canary_core_lib's
// tearbore_x — this file's drawing, promoted verbatim)

// one hinge fin on the case top wall, centered at x = xc: a teardrop knuckle,
// its root on the back face (-mount_extra, under a keyhole slab too), its
// crown on the face's show plane — the two surfaces that face the bed in
// the face-down print are the bed itself and a 45° (or steeper) flank
module case_fin(xc) {
    hull() {
        translate([xc - prong_t/2, out_y/2 - 1, -mount_extra]) cube([prong_t, 1, fin_root + mount_extra]);
        translate([xc - prong_t/2, out_y/2 + hinge_off, fin_r])
            rotate([0, 90, 0]) cylinder(r = fin_r, h = prong_t);
        translate([xc - prong_t/2, out_y/2 + hinge_off - 0.01, fin_pt - 0.01]) cube([prong_t, 0.02, 0.01]);
    }
}

// case-side hinge: two fins + root web, detent teeth on the OUTER faces,
// M5 bolt hole through. Prints with the shell, face-down.
module case_hinge() {
    ax = [0, out_y/2 + hinge_off, fin_r];        // hinge axis point
    difference() {
        union() {
            case_fin(-prong_pitch/2);
            case_fin( prong_pitch/2);
            // root web between/behind the fins (clears the bracket fins:
            // their swing circle never comes closer than hinge_off - fin_r);
            // its top is the fins' root height, a short bridge between them
            translate([-(prong_pitch/2 + prong_t/2), out_y/2 - 1, -mount_extra])
                cube([prong_pitch + prong_t, 1 + max(1, hinge_off - fin_r - 0.5), fin_root + mount_extra]);
            if (hinge_teeth) {
                xo = prong_pitch/2 + prong_t/2;  // fin outer faces
                translate([ xo, ax[1], ax[2]]) rotate([0,  90, 0]) linear_extrude(teeth_h) teeth2d();
                translate([-xo, ax[1], ax[2]]) rotate([0, -90, 0]) linear_extrude(teeth_h) teeth2d();
            }
        }
        tearbore_x(-out_x/2, ax[1], ax[2], out_x, hinge_hole);
        // With the keyhole slab under the plate (mount_style "both", the
        // weather preset) the root web reached down to z = -mount_extra, and
        // hanging straight under the bracket (0°, lens level) that slab-level
        // web landed on the bracket's tripod boss: 46 mm³ of case in bracket,
        // so the weather Vision could only hang tilted 30°. A 45° relief from
        // the wall's foot takes the web back to z = -0.6 — what the indoor
        // body already had — and faces away from the bed in the face-down print.
        if (mount_extra > 0)
            translate([-out_x, 0, 0]) rotate([90, 0, 90]) linear_extrude(2*out_x)
                polygon([[out_y/2 + 0.5, -mount_extra - 1], [out_y/2 + 40, -mount_extra - 1],
                         [out_y/2 + 40, -0.6], [out_y/2 + 0.5 + mount_extra + 0.4, -0.6]]);
    }
}

// ----------------------------------------------------------------------------
//  The PLATE (part="back") — the chassis: the floor with its slab, the board
//  cradles and clips, the keyholes, and the seats the screws enter through.
//  Drawn in the assembled frame: front face at z = floor_t, body down to
//  -mount_extra. Prints back-face down.
// ----------------------------------------------------------------------------
module back() {
    difference() {
        union() {
            translate([0, 0, floor_t]) pl_plate(plate_x, plate_y, plate_r, plate_t);
            // board cradles + snap clips on the X edges.
            // devkit host: pads for the DevKit, a perimeter pedestal for the module.
            // xiao host: two tall side rails under the module's X edges — the rail
            // gap clears the stacked XIAO hanging beneath it.
            if (has_dk) {
                // DevKit: four corner pads, not a perimeter ring — the DevKitM-1's
                // header rows run along its long edges and a ring there sat under
                // the solder stubs; the clips hook the pad-free corners
                for (sx = [1, -1], sy = [1, -1])
                    translate([dk_cx + sx*(dk_w/2 - 3.5), dk_cy + sy*(dk_l/2 - 3.5), floor_t - 0.01])
                        cylinder(d = 5.0, h = standoff_h + 0.01);
                ringped(vm_cx, vm_cy, vm_w, vm_l);
                for (s = [1, -1]) {
                    for (dy = [-(dk_l/2 - 4.5), dk_l/2 - 4.5])
                        edgeclip(dk_cx + s*dk_w/2, dk_cy + dy, s > 0 ? 0 : 180);
                    edgeclip(vm_cx + s*vm_w/2, vm_cy, s > 0 ? 0 : 180);
                }
            } else {
                // module rails + clips, TOP HALF ONLY — the stacked XIAO (17.5 wide
                // on the 20 mm module) hangs beneath the LOWER half, so full-length
                // side rails would collide with it (same fix as the doorbell). Two
                // bottom-corner pins catch the module's lower edge outboard of the
                // XIAO and the down-facing USB ports. Rails are NOTCHED at the clip
                // so the clip can flex.
                for (s = [1, -1]) {
                    rail_l = vm_l/2 - 4;
                    difference() {
                        translate([vm_cx + s*(vm_w/2 - 1.5) - 1.5, vm_cy + 2, floor_t - 0.01])
                            cube([3, rail_l, vm_standoff + 0.01]);
                        translate([vm_cx + s*(vm_w/2 - 1.5), vm_cy + 2 + rail_l/2, floor_t + vm_standoff/2])
                            cube([5, clip_w + 2, vm_standoff + 1], center = true);
                    }
                    edgeclip(vm_cx + s*vm_w/2, vm_cy + 2 + rail_l/2, s > 0 ? 0 : 180, vm_standoff);
                    // corner pin: its inner edge clears the measured XIAO, still catches the module
                    translate([vm_cx + s*(xiao_w/2 + 1.0 + 0.1), vm_cy - vm_l/2 + 1.2, floor_t - 0.01])
                        cylinder(d = 2.0, h = vm_standoff + 0.01);
                }
            }
        }
        // the screws: seats through the plate, pan heads over their glands in seal mode
        for (p = post_xy())
            pl_seat_cut(p[0], p[1], floor_t, plate_t, screw_size, screw_head, pl_r, scr_c, tol_hole, e_gland);
        // the key slot in the plate's edge (the rib is on the shell's bore wall)
        if (lid_key) translate([0, 0, floor_t]) lid_key_slot(key_x, bore_y/2 - tol_slide, 270, plate_t + 0.2, ledge_w + 0.4);
        // blind keyhole pockets (never reach the cavity — seal-safe); the
        // vision hangs by its back with +Y up, so the slot runs native-Y —
        // canary_mount_lib draws it without a rotate
        if (mount_extra0 > 0)
            for (yc = kh_ys)
                mount_keyhole_pocket(yc, -mount_extra, "y",
                                     head_d = kh_head_d, shank_d = kh_shank_d,
                                     slot_l = kh_slot_l, head_h = kh_head_h,
                                     face = kh_face);
        // anti-lift knockouts (0.6 mm web at the back face): after hanging, pierce
        // with #4/M3 screws into the wall so the case can't be lifted off the
        // keyholes. Placed in the empty top region, clear of pockets and cradles.
        if (mount_extra0 > 0 && kh_lock)
            for (sx = [1, -1]) translate([sx*10, inner_y/2 - 5, 0]) {
                translate([0, 0, -mount_extra + 0.6]) cylinder(d = 3.2, h = mount_extra + floor_t);
                translate([0, 0, floor_t - 1.2]) cylinder(d1 = 3.2, d2 = 6.0, h = 1.21);  // head seat, inside
            }
    }
}

// ----------------------------------------------------------------------------
//  The SHELL (part="front") — face, walls and posts, one piece: the lens
//  aperture + disc seat + hood groove, LED, vent, label on the face, the
//  camera board on posts under it, the walls hanging below to the back face.
//  Drawn with the face at z = 0..lid_t; everything below z = 0 is inside or
//  the walls. Prints face-down.
// ----------------------------------------------------------------------------
// The vent cluster and light-pipe bore are canary_core_lib's now
// (core_vent_cluster / core_lightpipe_bore) — this file used to carry its
// own copy of both, and the four copies across the weather shells had
// forked. The knobs above still ride in as arguments.
module front() { translate([0, 0, -base_d]) shell_asm(); }

// the shell in the ASSEMBLED frame (back face at -mount_extra, face at base_d..base_d + lid_t)
module shell_asm() {
    posts = post_xy();
    difference() {
        shell_solid();
        // the posts' blind pilots (or insert bores), up from their end faces —
        // cut LAST, through the posts the union below adds
        for (p = posts)
            pl_post_pilot(p[0], p[1], floor_t + pl_relief(), pl_pil,
                          screw_insert ? scr_nominal(screw_size) + 0.3 : scr_d,
                          screw_insert, ins_od - 0.3, ins_h);
    }
}
module shell_solid() {
    posts = post_xy();
    gusset_h = max(2, post_h - 0.5);
    gusset_w = min(2.0, rib_t_max(wall_eff));   // the landing width, capped at the old 2.0 target
    union() {
        difference() {
            union() {
                // the catalog's two-stage face-down soft edge (canary_core_lib)
                translate([0, 0, base_d]) soft_edge_plate(out_x, out_y, corner_r, lid_t, lid_edge, lid_edge2);
                translate([0, 0, -mount_extra]) rrect(out_x, out_y, corner_r, shell_d + 0.01);
                if (e_mount && (m_style == "hinge" || m_style == "both"))
                    case_hinge();
            }
            // the cavity, coved at the face and at the ledge (canary_core_lib)
            translate([0, 0, base_d]) pl_cavity_cut(inner_x, inner_y, core_cav_r(corner_r, wall_eff), cav_d, floor_cove);
            // the plate's bore below the ledge
            pl_bore_cut(bore_x, bore_y, bore_r, floor_t, plate_t);
            // the gasket groove, cut into the ledge
            if (e_seal)
                translate([0, 0, floor_t - 0.01]) linear_extrude(gasket_groove + 0.01) rim_ring2d(gasket_w);
            translate([0, 0, base_d]) {
                // lens aperture + recessed clear-disc seat
                translate([lens_x, lens_y, -1]) cylinder(d = cam_ap_d, h = lid_t + 2);
                if (cam_disc_t > 0 && cam_disc_d > 0)
                    translate([lens_x, lens_y, lid_t - (cam_disc_t + 0.2)])
                        cylinder(d = cam_disc_d + 2*tol_slide, h = cam_disc_t + 1);
                // the hood's seat: a hood_seat-deep groove in the show face on the
                // collar's own footprint (a first-layer void on the face-down print)
                if (e_hood)
                    translate([lens_x, lens_y, lid_t - hood_seat]) linear_extrude(hood_seat + 1) hood_ring2d();
                if (e_led) core_lightpipe_bore(vm_cx + lp_dx, vm_cy + lp_dy, lid_t, lp_d, tol_press);
                if (e_vent || e_buzzer) core_vent_cluster(vm_cx + vent_dx, vm_cy + vent_dy, lid_t,
                                                  vent_pad_d, vent_pad_depth, vent_ring_d, vent_hole_d, vent_holes);
                if (label_text != "")
                    translate([label_dx, label_dy, lid_t - label_depth])
                        linear_extrude(label_depth + 1) rotate(label_rot)
                            text(label_text, size = label_size, font = label_font,
                                 halign = "center", valign = "center");
                // the house mark instead of a label (debossed, so it prints as
                // crisp first-layer voids like everything else on this face)
                if (opt_mark)
                    translate([label_dx, label_dy, lid_t - label_depth])
                        linear_extrude(label_depth + 1) rotate(label_rot)
                            mark_bird(mark_h, mark_rib);
            }
            // USB-C opening(s), bottom wall: DevKit port, or module "model port".
            // The WAP's bridge-safe profile (canary_port_lib), its 45° chamfers
            // on the BACK side of the opening — in the face-down print that is
            // the opening's roof, and the chamfers halve the flat bridge
            translate([usb_cx, -out_y/2 + wall_eff*1.5, usb_zc])
                rotate([90, 0, 0]) linear_extrude(wall_eff*3)
                    mirror([0, 1, 0]) port_bridge_profile2d(usb_w, usb_h);
            // xiao host: second opening below it for the XIAO "firmware port"
            if (!has_dk)
                translate([xiao_usb_cx, -out_y/2 + wall_eff*1.5, xiao_usb_zc])
                    rotate([90, 0, 0]) linear_extrude(wall_eff*3)
                        mirror([0, 1, 0]) port_bridge_profile2d(usb_w, usb_h);
            // recess framing the USB opening(s) for flanged silicone plugs
            // (xiao host: one tall recess spans both stacked ports, including
            // any measured X offset between them); its floor never dips below
            // the ledge into the skin over the plate
            if (e_seal && usb_cover) {
                uz0 = max((has_dk ? usb_zc : xiao_usb_zc) - usb_h/2 - usb_cov_pad, floor_t + 0.3);
                uz1 = usb_zc + usb_h/2 + usb_cov_pad;
                ux0 = min(usb_cx, has_dk ? usb_cx : xiao_usb_cx) - (usb_w/2 + usb_cov_pad);
                ux1 = max(usb_cx, has_dk ? usb_cx : xiao_usb_cx) + (usb_w/2 + usb_cov_pad);
                translate([ux0, -out_y/2 - 1, uz0])
                    cube([ux1 - ux0, 1 + usb_cov_dep, uz1 - uz0]);
            }
            // 45° back-edge chamfer (elephant-foot + first-layer delamination
            // guard) — bounded to the footprint, so the hinge-fin roots lose
            // only a 0.5 mm nick
            if (foot_cham > 0)
                foot_chamfer_ring(out_x, out_y, corner_r, foot_cham, -mount_extra);
            // weep at the low point (bottom wall, hung +Y up), beside the USB
            // opening and outside its plug recess — canary_core_lib weep_cut.
            // It leaves the wall STRAIGHT (tilt 0 — straight down when hung),
            // a web above the gasket groove in the ledge: the old 30° dive
            // from the floor corner would bore through the seal line now
            if (e_weep)
                weep_cut(usb_cx + usb_w/2 + usb_cov_pad + weep_d + 1.0, -inner_y/2,
                         floor_t + (e_seal ? gasket_groove + core_min_web() : 0.2) + weep_d/2,
                         "-y", wall_eff, weep_d, 0);
        }
        // screw posts from the face's underside to the relief over the ledge,
        // gusseted to their walls (a mid-span post only to its own) — the
        // gussets root at the face and taper toward the ledge. A corner post
        // also fills the pocket between its two gussets and the cavity's
        // corner: with the cavity coved at the ledge too, that pocket would
        // otherwise close into a sealed void (the mesh gate counts it a part)
        for (p = posts) translate([p[0], p[1], floor_t + pl_relief()]) cylinder(d = pd, h = post_h);
        for (p = posts) translate([0, 0, base_d]) mirror([0, 0, 1]) {
            if (_on_x(p)) corner_gusset(p[0], p[1], sign(p[0])*(inner_x/2 + 0.5), p[1], gusset_h, wall_eff, pd, gusset_w);
            if (_on_y(p)) corner_gusset(p[0], p[1], p[0], sign(p[1])*(inner_y/2 + 0.5), gusset_h, wall_eff, pd, gusset_w);
        }
        for (p = posts) if (_on_x(p) && _on_y(p))
            translate([min(p[0], sign(p[0])*(inner_x/2 + 0.5)), min(p[1], sign(p[1])*(inner_y/2 + 0.5)), floor_t + pl_relief()])
                cube([inner_x/2 + 0.5 - abs(p[0]), inner_y/2 + 0.5 - abs(p[1]), post_h]);
        // the plate key (canary_core_lib): a rib on the +Y bore wall
        if (lid_key) lid_key_rib(key_x, bore_y/2, 270, floor_t, plate_t + 0.5);
        translate([0, 0, base_d]) {
            // camera-board posts on the inner face (Pi-cam v1.3 21 x 12.5 grid), tall
            // enough that the lens holder sits wholly behind the front
            for (sx = [1, -1], sy = [1, -1])
                translate([cam_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, -cam_post_eff])
                    difference() {
                        cylinder(d = cam_post_d, h = cam_post_eff + 0.1);  // 0.1 embeds into the face
                        translate([0, 0, -0.1]) cylinder(d = cam_screw_d, h = cam_post_eff - 0.8);
                    }
            // perimeter rib ring under the front face: t³ stiffening against pry/flex,
            // cleared around every feature and the screw posts (≈1 g of material);
            // fused 0.2 into the walls
            if (lid_ribs) {
                ro_x = inner_x + 0.4;
                ro_y = inner_y + 0.4;
                difference() {
                    translate([0, 0, -lid_rib_h]) linear_extrude(lid_rib_h + 0.1)
                        difference() {
                            rrect2d(ro_x, ro_y, core_cav_r(corner_r, wall_eff) + 0.2);
                            rrect2d(ro_x - 2*lid_rib_w, ro_y - 2*lid_rib_w, 0.1);
                        }
                    for (p = post_xy())
                        translate([p[0], p[1], -lid_rib_h - 0.1]) cylinder(d = pd + 1.6, h = lid_rib_h + 0.2);
                    // keep-outs: lens/disc seat, camera posts, LED, vent, magnet
                    translate([lens_x, lens_y, -lid_rib_h - 0.1])
                        cylinder(d = max(cam_ap_d, cam_disc_d) + 3, h = lid_rib_h + 0.2);
                    for (sx = [1, -1], sy = [1, -1])
                        translate([cam_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, -lid_rib_h - 0.1])
                            cylinder(d = cam_post_d + 2, h = lid_rib_h + 0.2);
                    if (e_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -lid_rib_h - 0.1])
                        cylinder(d = lp_d + 4, h = lid_rib_h + 0.2);
                    if (e_vent || e_buzzer) translate([vm_cx + vent_dx, vm_cy + vent_dy, -lid_rib_h - 0.1])
                        cylinder(d = vent_pad_d + 3, h = lid_rib_h + 0.2);
                    if (e_tamper) translate([vm_cx + mag_dx, vm_cy + mag_dy, -lid_rib_h - 0.1])
                        cylinder(d = mag_d + 2*tol_press + 4.8, h = lid_rib_h + 0.2);
                    // keep the USB cable path clear
                    translate([usb_cx, -inner_y/2, 0]) cube([usb_w + 4, 14, 3*lid_rib_h], center = true);
                }
            }
            // tamper magnet pocket (press fit; embedded 0.1 so the export is one shell)
            if (e_tamper)
                translate([vm_cx + mag_dx, vm_cy + mag_dy, -mag_h]) difference() {
                    cylinder(d = mag_d + 2*tol_press + 2.4, h = mag_h + 0.1);
                    translate([0, 0, -0.1]) cylinder(d = mag_d + 2*tol_press, h = mag_h + 0.1);
                }
        }
    }
}

// ----------------------------------------------------------------------------
//  GASKET — TPU seal ring matching the ledge groove (seal mode)
// ----------------------------------------------------------------------------
module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5); }

// ----------------------------------------------------------------------------
//  BRACKET — wall plate with three prongs; countersunk screws, keyhole slots,
//  optional captive 1/4-20 nut for tripods. Modeled in print orientation.
// ----------------------------------------------------------------------------
module bracket_fin(xc) {
    hull() {
        translate([xc - prong_t/2, -fin_r, br_t - 0.5]) cube([prong_t, 2*fin_r, 0.5]);
        translate([xc - prong_t/2, 0, br_t + hinge_off])
            rotate([0, 90, 0]) cylinder(r = fin_r, h = prong_t);
    }
}

module bracket() {
    az = br_t + hinge_off;                  // hinge axis height above the bed
    xo = prong_pitch + hinge_clear;         // outer fins stand hinge_clear wider than the GoPro pitch
    difference() {
        union() {
            rrect(br_x, br_y, 3, br_t);
            bracket_fin(0);
            bracket_fin(-xo);
            bracket_fin( xo);
            // tripod boss merges into the center-fin root; 5.0 tall so the nut
            // pocket leaves a 2.9 mm web under the fin (2.6 left 0.6 — under the
            // catalog's 0.8 web floor, pierced by the stud bore, carrying the camera)
            if (bracket_tripod)
                translate([0, 0, br_t - 0.1]) rrect(18, 18, 2, 5.0);
        }
        // M5 bolt bore through all three fins (teardrop roof — no crown sag)
        tearbore_x(-br_x/2, 0, az, br_x, hinge_hole);
        // detent tooth POCKETS cut into the outer fins' inner faces — the case
        // fins carry the male teeth. One side must be female: two protruding
        // rings can't nest in the 0.175 mm fin gap, so male/male meshing would
        // permanently spring the fins apart (creep -> detents loosen).
        if (hinge_teeth) {
            xi = xo - prong_t/2;            // outer fins' inner faces
            translate([ xi - 0.05, 0, az]) rotate([0,  90, 0])
                linear_extrude(teeth_h + 0.15) offset(delta = 0.12) teeth2d();
            translate([-xi + 0.05, 0, az]) rotate([0, -90, 0])
                linear_extrude(teeth_h + 0.15) offset(delta = 0.12) teeth2d();
        }
        // countersunk wall screws at the corners — a true 90° cone (canary_core_lib)
        // for a #8 flat head (Ø8.3): the old Ø4.2 -> Ø8.8 over 2.0 was a 98° cone
        // that bore on the head's rim only
        for (sx = [1, -1], sy = [1, -1])
            cs_cone90_cut(sx*(br_x/2 - 5), sy*(br_y/2 - 5), br_t, br_screw_d, (8.3 - br_screw_d)/2);
        // keyhole slots (through-plate; hang-and-slide-down)
        for (sx = [1, -1]) translate([sx*14, 0, 0]) {
            translate([0, -3, -0.1]) cylinder(d = 7.5, h = br_t + 0.2);
            translate([-2.1, -3, -0.1]) cube([4.2, 9, br_t + 0.2]);
            translate([0, 6, -0.1]) cylinder(d = 4.2, h = br_t + 0.2);
        }
        // captive 1/4-20 nut pocket (insert from the wall side) + stud bore
        if (bracket_tripod) {
            translate([0, 0, -0.1]) rotate([0, 0, 30]) cylinder(d = 11.4/cos(30), h = 6.0, $fn = 6);  // full-height 1/4-20 nut (5.56) sits sub-flush
            translate([0, 0, -0.1]) cylinder(d = 6.8, h = br_t + 3);
        }
    }
}

// ----------------------------------------------------------------------------
//  KNOB — printable M5 thumbscrew head (captive hex nut), or buy a GoPro
//  M5 knurled thumbscrew. Pair with an M5 x 25 bolt.
// ----------------------------------------------------------------------------
//  HOOD — the rain/glare collar over the lens window, as its OWN part.
//  ~220° arc open at the bottom, a spigot on its root that presses into the
//  front's groove (tol_press a side; bond with neutral-cure silicone). It
//  exports drip-edge-DOWN, spigot up: a C-shaped extrusion with no overhang.
//  Grown on the front it could not be printed in either pose (audit 2026-09).
// ----------------------------------------------------------------------------
module hood_ring2d(inset = 0) {
    difference() {
        circle(d = cam_disc_d + 5 + 2*hood_t - 2*inset);
        circle(d = cam_disc_d + 5 + 2*inset);   // +5 (was +3): keeps the hood outside the
                                                // OV5647's 72° diagonal FOV even with ±0.7 mm
                                                // lens decentration — no corner vignette
        translate([-(cam_disc_d/2 + hood_t + 2), -2*(cam_disc_d + hood_t)])
            square([cam_disc_d + 2*hood_t + 4, 2*(cam_disc_d + hood_t) - cam_disc_d*0.18 + inset]);
    }
}
module hood() {
    assert(e_hood, "the hood needs opt_hood=true (or preset=vision_weather) so its collar matches the front's groove");
    union() {
        linear_extrude(hood_len) hood_ring2d();
        // spigot: 0.1 shy of the groove's floor so the collar's root seats on the face
        translate([0, 0, -(hood_seat - 0.1)]) linear_extrude(hood_seat - 0.1 + 0.01) hood_ring2d(tol_press);
    }
}

// ----------------------------------------------------------------------------
module knob() {
    difference() {
        cylinder(d = 22, h = 8);
        for (i = [0 : 11]) rotate([0, 0, i*30])
            translate([12.6, 0, -1]) cylinder(d = 5, h = 10);          // grip scallops
        translate([0, 0, -0.1]) rotate([0, 0, 30])
            cylinder(d = 8.4/cos(30), h = 5.0, $fn = 6);               // M5 nut pocket, 8.4 AF: printed hexes
                                                                       // come out ~0.2 small (ISO 4032 m=4.7 fits)
        translate([0, 0, -0.1]) cylinder(d = hinge_bolt_d + 0.4, h = 10);
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
if      (part == "back")    back();
else if (part == "front") translate([0, 0, lid_t]) rotate([180, 0, 0]) front();   // face-down, hood or not
else if (part == "hood")  translate([0, 0, hood_len]) rotate([180, 0, 0]) hood();   // drip edge on the bed, spigot up
else if (part == "gasket") {
    assert(e_seal, "the gasket needs opt_seal=true (or preset=vision_weather) so its ring matches the groove");
    gasket();
}
else if (part == "bracket") bracket();
else if (part == "knob")    knob();
else {
    // assembled preview wears the chosen colorway (canary_color_lib);
    // color() is preview-only — single-part exports are byte-identical
    color(cw_body(colorway)) back();
    color(cw_body(colorway)) translate([0, -(out_y + hinge_off + fin_r + 10), 0])
        translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
    if (e_hood) color(cw_body(colorway))
        translate([-(out_x/2 + br_x/2 + 16), -(out_y + hinge_off + fin_r + 10), hood_seat - 0.1]) hood();
    color(cw_body(colorway)) translate([out_x/2 + br_x/2 + 14, 0, 0]) bracket();
    color(cw_ink(colorway))  translate([out_x/2 + br_x/2 + 14, br_y/2 + 22, 0]) knob();
    if (e_seal) color(cw_light(colorway)) translate([-(out_x/2 + br_x/2 + 16), 0, 0]) gasket();
}
