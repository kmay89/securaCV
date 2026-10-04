// ============================================================================
//  SecuraCV Canary Vision — DOORBELL enclosure (parametric)  v0.6
// @env cer=2 ip="~IP54 (button ~IP65)"
//  A slim vertical unit in the Wyze/Ring video-doorbell form factor, holding
//  the stacked-XIAO Vision build: OV5647 camera (top) + Grove Vision AI V2
//  with a XIAO ESP32-C3/S3 seated in its socket (middle) + a 12 mm
//  illuminated momentary button (bottom, wired to the multifunction input).
//
//  THE PARTING LINE IS THE BACK FACE (canary_core_lib pl_*). The SHELL is one
//  piece — face, side walls and screw posts, printed face-down as a cup — and
//  the BACK is a PLATE that nests inside the walls like a piston, seats on a
//  ledge the walls carry (the gasket lives in that ledge) and is pulled home
//  by short screws driven from the back into blind pilots in the post ends.
//  The plate is the chassis: module rails and clips, the cable exit, the
//  lug pockets and the screw seats all live on it. The only seam is a
//  hairline on the back face — which the wall plate covers when mounted, so
//  the wall plate is also the tamper cover: the plate screws sit under it.
//
//  Mounting follows the doorbell pattern, not the hinge: a thin WALL PLATE
//  screws to the door frame (flat, or a printable 5–15° WEDGE for angling
//  toward the approach); the body drops onto the plate's three dovetail lugs
//  (blind, seal-safe pockets — canary_mount_lib's drop-durable hanger), lands
//  in a collar wrapping its bottom end, and locks with a hidden SECURITY
//  SCREW driven up through the collar — the body cannot be lifted off without
//  a tool, Ring-style.
//
//  Power: USB-C cable from the stack's ports loops through the internal
//  cable well and exits through an oval in the BACK plate, through the
//  matching wall-plate hole, into the wall/under trim (use a right-angle
//  USB-C plug).
//
//  Units: millimeters.  CAD: OpenSCAD.  Weather sealing is ON by default
//  (a doorbell lives outside): TPU gasket in the ledge, ~IP54.
//
//  ⚠️ VERIFY BEFORE PRINTING. Measure your seated stack (stack_sock_h,
//     usb heights) and your button's body diameter/depth before printing.
//  Orientation: +Y up on the wall, +Z toward the face.
//
//  Assembled frame (the fit check's): the plate's front face at z = floor_t,
//  its back face at z = -mount_extra (= the shell's back edge); the shell's
//  face underside at z = base_d = floor_t + cav_d, the ledge plane at
//  z = floor_t, the posts stopping pl_relief() above it.
//
//      face  ______________________________  z = base_d + lid_t
//            |  post   post  (hang)        |
//       wall |   |      |     cavity       | wall
//            |   |      |                  |
//            |  _|_    _|_  relief 0.2     |
//     ledge  |‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|  z = floor_t   (gasket groove up into the ledge)
//       skin | [ plate — the chassis ]     | skin   (bore = inner + 2*ledge_w)
//            |_[_screw_seats_from_back_]___|  z = -mount_extra   (back face: the hairline seam)
//                 wall plate under it
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
//  v0.5 (2026-09-24): the piston-plate port — the parting line moves to the
//  back face (canary_core_lib pl_*; the Sense is the reference). Every mesh
//  moves:
//    * the face + walls + posts are ONE shell (part="face"), printed
//      face-down; the body (part="body") is a plate nested inside the walls
//      on a ledge, bore = inner + 2*ledge_w, so the walls carry the lateral
//      load and the only seam is the hairline on the back face. The lip,
//      the drip skirt, the seam reveal and the face-side screw path are gone
//      (knobs lip_h, lip_t, skirt_h, skirt_t, screw_from, head_seal removed);
//    * the screws are short (pl_len: M2 x 8 here), from the back through the
//      plate into blind pilots in the post ends, every seat glanding an
//      O-ring under a pan head in seal mode; the heads recess flush, and the
//      wall plate covers them when mounted — the wall mount is the tamper
//      cover;
//    * the gasket clamp span was 75.6 mm (DESIGN_RULES §6: <= 40): the mid
//      posts are now as many per long wall as the rule needs (two, evenly
//      spaced) and the cavity widens so they clear the module's clips
//      (inner_x 26.2 -> 33.1); the outer body is 43.5 x 118.0 (was 34.2 x
//      115.6, and 38.2 x 119.6 over the skirt);
//    * the security boss hangs from the bottom wall above the gasket groove
//      (sec_z derived: the bore never breaks into the groove), tapered 45°
//      toward the face so it prints supportless; the wall plate's foot rises
//      to meet it;
//    * the T-stud pockets, the cable oval and the poka-yoke key slot move to
//      the plate; the key rib is on the +Y bore wall; the wall plate's
//      thickness knob is wplate_t (plate_t is the piston plate's, derived).
//  v0.6 (2026-10-02): DROP-DURABLE — every feature that could break off and
//  take the part's function with it is designed out, not thickened. Every
//  mesh moves:
//    * the wall plate's two Ø4 T-studs (each held by one 12.6 mm² layer line
//      at its root) become THREE dovetail lugs (canary_mount_lib
//      mount_dovelug: no neck, a 42 mm² root under a 45° root chamfer, 3.3x
//      the stem each) in blind dovetail pockets on the body's back; lose any
//      one and the other two plus the collar still hang the body;
//    * the security screw's L-FOOT — a 12 x 4.5 tab standing ~13 mm off the
//      plate with a Ø2.6 bore leaving ~1 mm of wall either side, loaded in
//      peel across its layers — is gone. A COLLAR wraps the body's bottom
//      end instead: a continuous U whose side returns are deep beams in the
//      prying direction, fused along ~95 mm of root, and the body's resting
//      stop as well. The security screw passes up through the collar's
//      3.5 mm bottom wall into the same boss;
//    * the module's two Ø2 x 12 corner pins become CORNER SHOES (seat +
//      edge guide + a -Y stop the module never had — mounted, gravity pulls
//      it down the plate — on a buttress), top guides join the rails, and a
//      second clip pair goes on the lower half: four clips, root-filleted
//      (snap_boardclip root_r). The guides stand taller than the headroom
//      over the stack, so even with every clip broken the closed shell
//      keeps the module seated (asserted);
//    * THE CAMERA HOLE WAS IN THE WRONG PLACE. The posts sat on the carrier's
//      center and the aperture 2.5 above it, but the vendor CAD (the "RPi cam
//      Rev 1.3" in boards/vendor/seeed_grove_vision_ai_v2.step.gz) puts the
//      21 x 12.5 hole grid 4.05 ABOVE the carrier's center and the lens 1.7
//      BELOW it (toward the ribbon edge) — mounted on its posts, the real
//      lens sat 8.3 mm off the Ø10 hole. The posts now take the grid's
//      offset, the lens its own, and the hole is cut to the lens: a
//      Ø7.0 + 2*tol_slide bore the OV5647-62's barrel nests 0.6 into (it
//      registers the lens and blocks cavity light — the button's LED ring —
//      from flaring the disc), then a cone at the datasheet's 62° diagonal
//      FOV + cam_fov_margin, so nothing vignettes (asserted).
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, soft_edge_plate, foot_chamfer_ring, the
                             // piston plate (pl_*) — the catalog's shared helpers
use <canary_mount_lib.scad>  // the catalog's hangers — the wall plate's dovetail
                             // lugs and the body's blind pockets, one home
use <canary_snap_lib.scad>   // snap-fit doctrine — snap_boardclip carries the
                             // WAP clip and its strain gate
use <canary_port_lib.scad>   // connector standards — the shell numbers the
                             // cable exit is sized against
use <canary_board_lib.scad>  // board registry — this file is the measured
                             // source for the seated-stack height
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)
use <canary_rib_lib.scad>    // corner_gusset — the constant-width post web
use <canary_color_lib.scad>  // the colorway registry — assembled-preview spools

/* [What to render] */
part = "all";        // ["body","face","plate","gasket","all"]

/* [Preset] — quick configs; choose "custom" to use the option checkboxes */
preset = "custom";   // ["custom","doorbell_weather"]

/* [Options (applied when preset = custom)] */
opt_seal   = true;   // perimeter TPU gasket in the shell's ledge (doorbells live outside)
opt_vent   = true;   // GORE vent cluster on the face — ON by default: a sealed outdoor
                     // unit with no pressure path pumps moist air past the seals on
                     // every day/night thermal cycle and the condensate never leaves
opt_led    = false;  // separate light-pipe port (the 12 mm button usually has its own LED ring)
opt_tamper = false;  // reed/Hall magnet pocket on the face underside
opt_weep   = true;   // Ø2 weep through the bottom wall just above the plate's face (mounted button-down):
                     // condensate leaves through a drain slot in the wall plate's collar
weep_d     = 2.0;    // weep bore  // [1.5:0.5:3]
seal_mid_posts = true; // (seal mode) mid posts along each long wall, as many as keep every gasket
                       // clamp span <= 40 mm (DESIGN_RULES §6) — four corner screws cannot hold
                       // 20 % gasket squeeze across 99 mm of plate

// effective flags (a preset overrides the checkboxes above). doorbell_weather is
// the committed build — the file's defaults: sealed, vented, weep through the
// bottom wall, no separate light pipe (the button usually has its own LED ring), no
// tamper magnet. A doorbell lives outside, so the one preset is the sealed one.
function _pre(c, w) = (preset == "doorbell_weather") ? w : c;
e_seal   = _pre(opt_seal,   true);
e_vent   = _pre(opt_vent,   true);
e_led    = _pre(opt_led,    false);
e_tamper = _pre(opt_tamper, false);
e_weep   = _pre(opt_weep,   true);

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
board_clear = 0.6;   // clearance around the camera carrier and the module (per side across the case; once below the module)

/* [Camera] — the OV5647-62 (the Grove Vision AI V2 kit's camera, Pi-cam v1.3 form) on posts under the face */
// Geometry measured off the vendor CAD ("RPi cam Rev 1.3" in boards/vendor/
// seeed_grove_vision_ai_v2.step.gz); optics from Seeed's OV5647-62 datasheet
// (62° FOV, EFL 3.2, F2.8, 25 x 24 x 7±0.2 module). The carrier hangs
// ribbon-edge DOWN: its FPC runs to the module below.
cam_hole_x = 21.0;   // camera post grid (X) — brd_ov5647_hole_x(), the vendor CAD
cam_hole_y = 12.5;   // camera post grid (Y) — brd_ov5647_hole_y(), the vendor CAD
cam_grid_dy = 4.05;  // hole-grid center ABOVE the carrier's center (toward its top edge) — brd_ov5647_grid_dy(), the vendor CAD
cam_post_d = 3.6;    // camera post diameter
cam_lens_h = 5.0;    // lens barrel top above the PCB face — brd_ov5647_lens_h(), the vendor CAD
cam_holder_h = 3.65; // the square holder's top above the PCB face — brd_ov5647_holder_h(), the vendor CAD
cam_lens_sq = 8.8;   // the holder's square — brd_ov5647_holder_sq(), the vendor CAD
cam_barrel_d = 7.0;  // the round lens barrel — brd_ov5647_barrel_d(), the vendor CAD
cam_barrel_in = 0.6; // how far the barrel stands into that bore; the rest under the disc is focus travel
cam_fov    = 62;     // diagonal field of view — the OV5647-62 datasheet  // [40:1:160]
cam_fov_margin = 4;  // degrees added to each side of the FOV cone the hole must clear
cam_screw_d = 1.6;   // pilot bored down each camera post for its screw
lens_dx  = 0.0;      // lens center X offset from the camera-board center — brd_ov5647_lens_dx(), the vendor CAD
lens_dy  = -1.7;     // lens center Y offset from the camera-board center: BELOW it, toward the ribbon edge — brd_ov5647_lens_dy(), the vendor CAD
cam_disc_d = 14.0;   // clear-disc seat (0 = bare aperture)
cam_disc_t = 1.0;    // clear-disc thickness (the disc sits 0.2 recessed below the face)

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
floor_t  = 2.0;      // the plate's floor under the boards (the plate itself is pl_thick: never thinner than a head and its floor)
lid_t    = 2.2;      // face thickness (the visible surface)
cav_extra = 1.0;     // headroom over the tallest component
floor_cove = 0.8;  // 45° cove where the cavity meets the walls, at the face and at the ledge (canary_core_lib pl_cavity_cut); 0 = the old square corner  // [0:0.2:1.2]
                   // The sharp notch there was the crack-starter in every flat-printed shell — a corner drop
                   // hinges the floor about it along one layer boundary.
lid_key    = true; // poka-yoke: a rib on the +Y (camera-end) bore wall and a slot in the plate's edge — the
                   // screw pattern fits a plate two ways and the lens/button line up one way; turned round it
                   // stands on the rib
zone_top = 8.0;      // margin above the camera (top posts live here)
zone_gap = 2.0;      // camera <-> module gap
zone_well = 12.0;    // cable well between module and button (USB plugs live here)
zone_btn = 21.0;     // button zone height
usb_exit_w = 12.0;   // oval cable exit through the back plate (into the wall-plate hole) — sized
                     // for a molded right-angle USB-C plug HEAD, not the 8.94 mm
                     // shell (port_usbc_shell_w()): molded heads run ~10-12 mm
usb_exit_h = 7.0;    // the cable exit oval's height (Y); usb_exit_w is its length
usb_exit_dx = 0.0;   // exit offset from the centerline (0 keeps it centered in the well)
usb_exit_dy = 0.0;   // exit offset from the well center: the oval sits wholly in the well, under
                     // nothing (at +6 it reached under the module and the XIAO)

/* [Print tolerances] — the catalog trio (canary_core_lib core_tol_*(),
   dialed on the fit coupon) */
tol_slide = 0.20;    // core_tol_slide()
tol_press = 0.10;    // core_tol_press()
tol_hole  = 0.30;    // core_tol_hole()

/* [Engineering] (see README "Engineering & materials") */
screw_insert = false;  // M2 brass heat-set inserts in the post ends AND the security boss (the one
                       // screw undone at every service — self-tapped plastic strips there first)
insert_d     = 3.5;     // (m2) heat-set insert OD — its bore is cut 0.3 under it; other sizes read the registry
insert_h     = 4.0;     // (m2) insert length; other sizes read the registry
lid_ribs     = true;   // perimeter rib ring under the face
lid_rib_w    = 2.5;     // rib ring width
lid_rib_h    = 1.0;     // rib depth below the face — asserted within the headroom over the stack (lid_headroom)
foot_cham    = 0.5;    // 45° chamfer on the shell's back edge

/* [Screw posts] (the plate screws — use black-oxide M2 for looks) */
post_d       = 5.0;   // screw post Ø — the plate screws thread into the post ends (auto-fattened for inserts and larger screws)
screw_size   = "m2";  // ["m2","m2.5","m3"] plate screw — the core lib's registry sets pilot, clearance,
                      // head seat, insert bore and post floor; "m2" keeps the validated numbers below
screw_head   = "pan"; // ["pan","flat"] pan = flat-floored seat (the BOM's black-oxide pan heads; what
                      // the seal mode's O-ring gland needs), flat = 90° countersink
screw_d      = 1.6;   // (m2)
screw_head_d = 4.0;   // (m2 pan)
screw_head_h = 2.0;   // (m2 pan) head height; the seat recesses it flush in the plate's back face

/* [Weather sealing] */
gasket_w      = 1.6;  // gasket groove width in the shell's ledge (the printed gasket is 0.5 narrower)
gasket_groove = 1.2;  // groove depth into the ledge
gasket_proud  = 0.3;  // how far the printed gasket stands proud of its groove, uncompressed — what the plate screws squeeze

/* [Wall plate + security screw] */
wplate_t    = 4.0;    // wall plate thickness at the THIN end
plate_wedge = 0;      // vertical wedge: camera tilts down the approach  // [0:5:15]
plate_wedge_x = 0;    // horizontal wedge: aims left/right (corner installs)  // [-15:5:15]
sec_screw_d = 2.2;    // security screw (M2 self-tap; use a Torx/security drive)
plate_screw_d = 4.2;  // wall screws (#8 / M4 PAN head — the seats are flat counterbores)
plate_head_h = 2.8;   // wall-screw pan head height: the seat is cut this deep (+0.2) so the head sits flush  // [2.0:0.1:3.2]
collar_t    = 2.4;    // the collar's side-return thickness (it wraps the body's bottom end; 6 perimeters at 0.4)
collar_tb   = 3.5;    // the collar's bottom wall — the security screw passes up through it and its head bears on it
collar_arm  = 18.0;   // how far the side returns climb the body from its bottom edge (past the pill's corner radius)
collar_clear = 0.2;   // side clearance between the body and the collar's returns (the bottom is the body's rest — no gap)

/* [Dovetail hanger] — the wall plate's lugs and the back plate's blind pockets */
// the lug/pocket pair is canary_mount_lib's drop-durable hanger (mount_dovelug):
// the library owns the section and the 8.0 drop; these knobs place the lugs
// and dial the fit per printer (tune dt_clear on the coupon's POCKET station)
lug_y     = 40.0;   // the outer lugs park at y = ±lug_y (clear of the cable exit and the well)
lug_mid_y = 6.0;    // the middle lug parks here — a third lug, so losing any one still leaves two
dt_clear  = 0.2;    // pocket clearance per face — mount_dt_clear() (core_tol_slide)
lug_extra = 3.0;    // plate thickening below the floor that hosts the pockets (the plate is at least floor_t + lug_extra)

/* [Aesthetics] */
colorway    = "graphite"; // ["graphite","canary","snow","forest","midnight"] assembled-preview spool set (canary_color_lib; single-part exports carry no color)
lid_edge    = 1.0;    // deviates: scaled to the pill — the 12 mm face radius carries a wider first stage
lid_edge2   = 0.8;    // second, steeper stage (~66°) — softens the face edge toward a roundover
// The wordmark sits where label_text would (label_dx/dy/rot/size/depth place
// and size it) and is gated by the mark library's measured type
opt_mark    = false;  // deboss the house wordmark instead of a custom label (exclusive with label_text)
                      // metrics, so a size that would print as a smudge or run off
                      // the face is refused before a print, not after
label_text  = "";     // debossed face label ("" = off)
label_size  = 4.5;    // label text height (the wordmark's size too, with opt_mark)
label_depth = 0.5;    // deboss depth into the face
label_dx    = 0.0;    // label center X offset from the FACE's center (not the module center)
label_dy    = -26.0;  // label center Y offset from the FACE's center
label_rot   = 0;      // label rotation (degrees)
label_font  = "Liberation Sans:style=Bold";  // the font label_text is set in (it must be installed)

/* [Front-face features] — offsets from the MODULE center */
lp_d   = 3.0;      // light-pipe diameter (hole = lp_d + 2*tol_press) — core_lightpipe_d()
lp_dx  = 8.0;      // light-pipe port center X, from the module center
lp_dy  = -8.0;     // light-pipe port center Y, from the module center
vent_pad_d     = 12.0;  // GORE-vent seat Ø, on the face's INNER side (it prints face-down: an outer seat cannot bridge) — core_vent_pad_d()
vent_pad_depth = 0.8;   // that seat's recess depth — core_vent_pad_depth()
vent_hole_d    = 1.0;   // fine holes — insect-resistant (the README's outdoor rule: <= 1.0 mm)
vent_ring_d    = 6.0;   // Ø of the ring the vent holes sit on — core_vent_ring_d()
vent_holes     = 10;    // more, smaller holes recover the open area at 1.0 mm
vent_dx        = 0.0;    // vent cluster center X, from the module center; 0 keeps it on the face's vertical axis
                         // That axis is the camera → grille → button rhythm of a real doorbell;
                         // off-axis, the cluster read as an accidental drill pattern.
vent_dy        = -8.0;   // vent cluster center Y, from the module center
mag_d  = 6.0;      // tamper MAGNET diameter (pocket = mag_d + 2*tol_press — press fit)
mag_h  = 3.2;      // magnet thickness, and the pocket ring's height off the face's inside
mag_dx = 8.0;      // magnet pocket center X, from the module center
mag_dy = 8.0;      // magnet pocket center Y, from the module center

/* [Board snap clips] — the WAP's print-proven numbers (the canary_snap_lib
   snap_boardclip defaults); the lib's strain gate holds them honest */
clip_w      = 6.0;   // board-clip tab width along the board edge — snap_boardclip default
clip_t      = 1.0;   // clip beam thickness — snap_boardclip default; the lib asserts its insertion strain
clip_hook   = 0.5;   // lip overhang over the board top — snap_boardclip default
clip_hook_h = 1.2;   // lip + 45° lead-in height above the board top — snap_boardclip default
clip_clear  = 0.25;  // beam face to board edge (a fit — tune on the coupon) — snap_boardclip default
clip_root_r = 0.6;   // 45° root fillet on each clip beam (snap_boardclip root_r) — a cantilever breaks at its root

/* [Quality] */
bridge_layer = 0.2;  // your slicer's layer height: the face's counterbores get bridge steps this thick (core_bridge_steps)  // [0.08:0.04:0.32]
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
scr_d   = (screw_size == "m2") ? screw_d : scr_pilot(screw_size);
scr_c   = max(scr_d + 2*tol_hole, scr_clear(screw_size));
head_d  = (screw_size == "m2" && screw_head == "pan") ? screw_head_d
        : (screw_head == "pan") ? scr_pan_d(screw_size) : scr_flat_d(screw_size);
head_h  = (screw_size == "m2" && screw_head == "pan") ? screw_head_h
        : (screw_head == "pan") ? scr_pan_h(screw_size) : scr_flat_h(screw_size);
ins_od  = (screw_size == "m2") ? insert_d : scr_insert_d(screw_size) + 0.3;
ins_h   = (screw_size == "m2") ? insert_h : scr_insert_h(screw_size);
pd = max(screw_insert ? max(post_d, ins_od + 3.0) : post_d, scr_post_min(screw_size));
// a sealed build seats an O-ring under every plate screw head: the seat is a
// hole through the seal line from outside (canary_core_lib pl_seat_cut)
e_gland  = e_seal;
clip_stack  = clip_clear + clip_t;
vm_standoff = stack_sock_h + xiao_below;
// the camera hangs from its posts with the lens barrel cam_barrel_in into the
// face's bore: the post length is the barrel's height less that entry, and the
// bore is the barrel plus the catalog's slide fit (it registers the lens)
cam_post_eff = cam_lens_h - cam_barrel_in;
cam_ap_d     = cam_barrel_d + 2*tol_slide;
cam_half     = cam_fov/2 + cam_fov_margin;            // the cone the hole must clear, per side
cam_bore_top = lid_t - ((cam_disc_t > 0 && cam_disc_d > 0) ? cam_disc_t + 0.2 : 0);   // bore floor -> disc seat
function cam_cone_d(z) = cam_ap_d + 2*max(0, z - cam_barrel_in)*tan(cam_half);         // the lens cone's Ø at face height z

// the zones along Y are set by the boards alone, so they come first: the
// post rows and the mid posts read them, and the cavity's width reads the posts
// the top margin holds the top post row over the camera carrier: board_clear
// + the post + 1.0 of wall clearance — an insert build fattens the post, and
// zone_top's 8.0 was 0.1 short of it (the +inserts hardware set found it)
zone_top_eff = max(zone_top, board_clear + pd + 1.0);
inner_y = zone_btn + zone_well + (vm_l + board_clear) + zone_gap + cam_h + zone_top_eff;
btn_cy  = -inner_y/2 + zone_btn/2;
well_cy = -inner_y/2 + zone_btn + zone_well/2;      // cable well / USB plug space
vm_cy   = -inner_y/2 + zone_btn + zone_well + board_clear + vm_l/2;
cam_cy  = vm_cy + vm_l/2 + zone_gap + cam_h/2;
vm_cx   = 0;  cam_cx = 0;
lens_x  = lens_dx;  lens_y = cam_cy + lens_dy;
cam_grid_cy = cam_cy + cam_grid_dy;                 // the hole grid's center (the posts)
function cam_post_xy() = [for (sx = [1, -1], sy = [1, -1]) [cam_cx + sx*cam_hole_x/2, cam_grid_cy + sy*cam_hole_y/2]];

// the post rows: corner posts in the top/bottom margins (the bottom pair 1.0
// from the wall, not 2.5: at 2.5 a 14 AF button nut touched them), and as
// many MID posts along each long wall as the gasket clamp rule needs — every
// span between screws <= 40 mm (DESIGN_RULES §6), evenly spaced. The v0.4
// single mid pair sat at the cable well and left a 75.6 mm upper span.
// the top row sits 2.5 from the top wall, or as high as the camera carrier's
// board_clear needs (at 2.5 the v0.4 posts stood 0.5 off the carrier's top
// edge, 0.1 inside its clearance); the bottom row keeps 1.0 off the wall
post_y_top = max(inner_y/2 - pd/2 - 2.5, cam_cy + cam_h/2 + board_clear + pd/2);
post_y_bot = -inner_y/2 + pd/2 + 1.0;
assert(inner_y/2 - post_y_top - pd/2 >= 1.0 - 1e-9,
       "the top posts run into the top wall — lengthen zone_top or shorten cam_h's zone");
n_mid  = (e_seal && seal_mid_posts) ? max(0, ceil((post_y_top - post_y_bot)/40) - 1) : 0;
mid_ys = n_mid > 0 ? [for (i = [1 : n_mid]) post_y_bot + i*(post_y_top - post_y_bot)/(n_mid + 1)] : [];

// a mid post stands beside whatever shares its band of Y: the module (with
// its clips), the camera carrier or the button nut. The cavity widens so the
// post's inner edge clears the widest of them by 0.5 — the doctrine that grew
// inner_x 26.2 -> 33.1 rather than let a post land on the module's clip
function _in_band(y, c, half) = abs(y - c) < half + pd/2 + 0.5;
mid_need = n_mid == 0 ? 0 : max([for (y = mid_ys) max(
    _in_band(y, vm_cy,  vm_l/2 + board_clear) ? vm_w/2 + clip_stack + 0.5 : 0,
    _in_band(y, cam_cy, cam_h/2 + board_clear) ? cam_w/2 + board_clear + 0.5 : 0,
    _in_band(y, btn_cy, btn_nut_ac/2)          ? btn_nut_ac/2 + 0.5 : 0)]);

// screw_insert grows the posts (pd) by 1.5, which walked the bottom pair into
// the button nut's arc — the option asserted itself dead. The cavity widens
// by what the nut arc needs instead (the posts stay 1.0 off the walls; they
// cannot move down, and up is toward the nut)
inner_x = max(cam_w + 2*board_clear, vm_w + 2*(clip_stack + board_clear) + 0.5,
              2*(mid_need + pd - 0.2),
              screw_insert ? 2*(sqrt(max(0, pow(btn_nut_ac/2 + pd/2 + 0.5, 2) - pow(zone_btn/2 - pd/2 - 1.0, 2)))
                                + pd/2 + 1.0) + 0.1 : 0);
cav_d   = max(vm_standoff + pcb_t + vm_front_h + cav_extra, btn_body_l - lid_t + 1);

out_x  = inner_x + 2*wall_eff;
out_y  = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;
rr     = min(db_r, out_x/2 - 0.1);          // pill radius, clamped to the width
cav_r  = core_cav_r(rr, wall_eff);          // the cavity's corner radius

// the plate (canary_core_lib pl_*): never thinner than a head and its floor,
// and never thinner than the floor plus the lug-pocket slab (lug_extra); the
// head recesses as far as that floor allows; the screw is the shortest
// standard length that engages hw_engage() in the post end past the relief
plate_t  = pl_thick(floor_t, lug_extra, screw_size, screw_head, e_gland);
mount_extra = plate_t - floor_t;                   // the plate below z = 0 (the pocket slab, or the head's floor)
pl_r     = pl_recess(plate_t, screw_size, screw_head, e_gland);
pl_L     = pl_len(plate_t, pl_r, screw_size, screw_head);
pl_eng   = pl_engage(plate_t, pl_r, screw_size, screw_head);
pl_pil   = pl_pilot(plate_t, pl_r, screw_size, screw_head);
post_h   = cav_d - pl_relief();                    // face underside -> the relief over the ledge plane
shell_d  = cav_d + plate_t;                        // the walls, face underside -> the back face
bore_x   = inner_x + 2*ledge_w;  bore_y = inner_y + 2*ledge_w;
bore_r   = cav_r + ledge_w;
plate_x  = bore_x - 2*tol_slide;  plate_y = bore_y - 2*tol_slide;  plate_r = max(bore_r - tol_slide, 0.4);
assert(!e_gland || screw_head == "pan", "a sealed build seats an O-ring under each plate screw head — that needs screw_head = \"pan\"");
assert(pl_pil + 1.0 <= post_h, str("the post is too short for its pilot (", pl_pil, " into ", post_h, " mm)"));
assert(!screw_insert || ins_h + 1.0 <= post_h, "the post is shorter than the insert it must hold");

// the security screw: its bore runs through the bottom wall into a boss on
// the wall's inner face. The axis is DERIVED so the bore's lowest edge stays
// 0.8 above the gasket groove's roof (the ledge band is the bore's wall, and
// the v0.4 height of 3.0 would have opened the groove to the outside); the
// wider of the two bores (insert / self-tap) sets it, so one wall plate fits
// either build
sec_bore_d   = screw_insert ? scr_nominal(screw_size) + 0.3 : sec_screw_d - 0.5;
sec_bore_max = max(ins_od - 0.3, sec_screw_d - 0.5);
sec_z        = floor_t + (e_seal ? gasket_groove : 0) + 0.8 + sec_bore_max/2;
sec_boss_top = sec_z + sec_bore_max/2 + 1.2;       // 1.2 mm of stock over the bore
sec_boss_d   = 4.0;                                // the boss's depth off the wall's inner face

// ASSEMBLED-FIT PROBE for canary_case_fitcheck.scad. It lives HERE, next to
// the geometry, for two reasons: it reads this file's own derived datum
// instead of duplicating the arithmetic it is checking, and its name is
// unique — every case in this catalog calls its halves front()/back() (here
// body()/face(), with those as aliases), so a
// single fit-check file that `use`d them all would silently resolve to
// whichever was parsed last and check the wrong case.
//
// MUST RENDER EMPTY. `lift` separates intended face-on-face contact from real
// interference: coplanar faces intersect to a zero-volume patch that CGAL
// reports as non-2-manifold, which is a dirty render, not a pass.
// `turned` seats the shell rotated 180° about Z — the poka-yoke CONTROL: with
// lid_key on, this must NOT be empty (the plate's edge lands on the key rib).
// A gate that can only pass has proved nothing; this is the case it must fail.
module doorbell_fitcheck(lift = 0.1, turned = false) {
    intersection() { translate([0, 0, base_d + lift]) rotate([0, 0, turned ? 180 : 0]) face(); body(); }
}

// the screw posts: the four corners, plus the mid posts 0.2 INTO each ±X wall
function post_xy() = concat([
    [ inner_x/2 - pd/2 - 1.0,  post_y_top],
    [-inner_x/2 + pd/2 + 1.0,  post_y_top],
    [ inner_x/2 - pd/2 - 1.0,  post_y_bot],
    [-inner_x/2 + pd/2 + 1.0,  post_y_bot],
], [for (y = mid_ys, s = [1, -1]) [s*(inner_x/2 - pd/2 + 0.2), y]]);
function _is_corner(p) = abs(p[1]) > inner_y/2 - 10;

// the clamp-spacing rule (DESIGN_RULES §6: <= 40 mm between gasket screws),
// said on every render, as DESIGN_RULES claims this file does — and, with the
// mid posts on, held: they are placed so no span can exceed it
_ys = [for (p = post_xy()) if (p[0] > 0) p[1]];
_seal_span = max([for (a = _ys) let (g = min([for (b = _ys) if (b > a) b - a, 1e9])) if (g < 1e8) g]);
if (e_seal)
    echo(str("seal mode: longest gasket span between screws ", _seal_span, " mm (rule: <= 40)",
             _seal_span > 40 ? " — mid-span squeeze rests on the plate's stiffness; enable seal_mid_posts" : ""));
assert(!(e_seal && seal_mid_posts) || _seal_span <= 40 + 1e-9,
       str("seal_mid_posts left a ", _seal_span, " mm gasket span — the mid posts are misplaced"));

assert(btn_bez_d == 0 || btn_bez_d > btn_d + 2, "btn_bez_d must exceed the button hole");
assert(head_d > scr_c, "the screw head must be larger than its clearance hole, or it falls through the plate");
assert(!e_seal || e_vent || e_weep,
       "seal mode with no pressure path — enable opt_vent (GORE seat) or opt_weep (field_ratings.md)");
// the button's panel nut must clear the bottom posts (their inner edge vs the nut's corner radius)
assert(len([for (p = post_xy()) if (sqrt(pow(p[0], 2) + pow(p[1] - btn_cy, 2)) < btn_nut_ac/2 + pd/2 + 0.5) 1]) == 0,
       str("a ", btn_nut_ac, " mm across-corners button nut hits a screw post — smaller nut, or move the posts"));
// the cable oval must sit under the WELL: below the module's bottom edge and clear of any mid post in its band
assert(well_cy + usb_exit_dy + usb_exit_h/2 <= vm_cy - vm_l/2 - 0.5,
       "cable exit reaches under the module/XIAO — lower usb_exit_dy or lengthen zone_well");
assert(len([for (p = post_xy()) if (!_is_corner(p)
            && abs(p[1] - (well_cy + usb_exit_dy)) < pd/2 + usb_exit_h/2
            && abs(usb_exit_dx) + usb_exit_w/2 > abs(p[0]) - pd/2 - 1.0) 1]) == 0,
       "cable exit runs into a mid-span post — center it (usb_exit_dx) or narrow it");
assert(btn_body_l - lid_t + 1 <= cav_d, "button too deep — raise btn_body_l budget or cavity");
assert(zone_btn >= btn_bez_d + 3, "zone_btn too short for the button bezel");
lid_headroom = cav_extra + (cav_d - (vm_standoff + pcb_t + vm_front_h + cav_extra));
assert(!lid_ribs || lid_rib_h <= lid_headroom,
       str("lid_rib_h ", lid_rib_h, " reaches past the ", lid_headroom, " mm headroom over the stack — the ribs land on a component"));
assert(!lid_ribs || lid_rib_w >= core_min_wall(), "lid_rib_w is under the structural wall floor");
assert(!e_seal || gasket_groove + 0.5 <= cav_d, "gasket_groove runs out of the ledge");
assert(!e_seal || core_gasket_fill(gasket_w, gasket_groove, gasket_proud) <= core_gasket_fill_max(),
       str("the printed TPU ring would fill ", round(100*core_gasket_fill(gasket_w, gasket_groove, gasket_proud)),
           " % of its groove - past ", round(100*core_gasket_fill_max()),
           " % the incompressible gasket props the plate off its ledge instead of sealing; narrow gasket_w or deepen gasket_groove"));
assert(cam_disc_d == 0 || cam_disc_d > cam_ap_d, "cam_disc_d must exceed cam_ap_d");
// the face prints face-down: each bed-side counterbore floor must sit on a whole layer,
// or the bridge steps over it land between layers and the slicer merges them away
function _on_layer(d) = abs(d/bridge_layer - round(d/bridge_layer)) < 1e-6;
assert(_on_layer(lid_t - cam_bore_top) && (btn_bez_d == 0 || _on_layer(btn_bez_t)),
       str("the disc seat (", lid_t - cam_bore_top, ") and bezel seat (", btn_bez_t, ") depths must be whole ",
           bridge_layer, " mm layers — set bridge_layer to your slicer's layer height"));
assert(mount_dt_depth() + 1.5 <= plate_t, "the lug pockets leave under 1.5 mm of plate over them — raise lug_extra");
// the camera: the holder clears the face, the barrel keeps focus travel under
// the disc, and nothing in the face crops the lens's field
assert(cam_holder_h + 0.5 <= cam_post_eff - 1e-9,
       str("the lens holder stands ", cam_post_eff - cam_holder_h, " mm under the face (< 0.5) — lower cam_barrel_in"));
assert(cam_barrel_in >= 0.3 && cam_bore_top - cam_barrel_in >= 0.3 - 1e-9,
       "the lens barrel needs >= 0.3 in its bore and >= 0.3 of focus travel under the disc — adjust cam_barrel_in");
// the disc must sit on a ledge round the cone actually cut (the cone starts at
// cam_ap_d, the bore), at least 1.0 a side under the disc seat's own cut; and
// the field leaving the barrel's mouth (its OD bounds it from above), spread
// at cam_half, must pass the seat's opening at the face
_cone_at_seat = cam_cone_d(cam_bore_top);
assert(cam_disc_d == 0 || _cone_at_seat + 2*1.0 <= cam_disc_d + 2*tol_slide - 1e-9,
       str("the lens cone is ", _cone_at_seat, " mm across where the disc seats — under 1.0 of ledge a side on a Ø",
           cam_disc_d, " disc; widen cam_disc_d"));
_fov_at_face = cam_barrel_d + 2*(lid_t - cam_barrel_in)*tan(cam_half);
assert(cam_disc_d == 0 || _fov_at_face <= cam_disc_d + 2*tol_slide - 1e-9,
       str("the lens field is ", _fov_at_face, " mm across at the face — past the disc seat's Ø",
           cam_disc_d + 2*tol_slide, " opening; widen cam_disc_d"));
assert(abs(lens_dy) + cam_lens_sq/2 < cam_h/2 && abs(lens_dx) + cam_lens_sq/2 < cam_w/2
       && abs(cam_grid_dy) + cam_hole_y/2 < cam_h/2 && cam_hole_x/2 < cam_w/2,
       "the lens holder or the hole grid runs off the camera carrier — check lens_dx / lens_dy / cam_grid_dy / cam_hole_x");
// the security boss (and its 45° taper, sec_boss_d tall) stays under the rib ring and the button
assert(sec_boss_top + sec_boss_d + 1.0 <= base_d - lid_rib_h,
       str("the security boss reaches ", sec_boss_top + sec_boss_d, " mm — into the face's rib ring"));
assert(-inner_y/2 + sec_boss_d <= btn_cy - btn_d/2 - 0.3,
       "the security boss reaches under the button body — shorten sec_boss_d or lengthen zone_btn");
assert(lid_edge == 0 || (lid_edge >= 0.01 && lid_edge < lid_t), "lid_edge out of range");
assert(lid_edge2 >= 0 && (lid_edge > 0 || lid_edge2 == 0) && lid_edge + lid_edge2 < lid_t,
       "lid_edge2 requires lid_edge > 0, and their sum must stay below lid_t");
assert(abs(plate_wedge_x) <= 15 && plate_wedge <= 15, "keep wedge angles <= 15 degrees");
// the lugs: three pockets on the plate's back, each clear of the cable oval,
// the screw seats and the plate's edge
lug_ys = [lug_y, lug_mid_y, -lug_y];
assert(len([for (c = lug_ys) let (r = mount_dt_pocket_y(c, clear = dt_clear))
            if (r[0] < well_cy + usb_exit_dy + usb_exit_h/2 + 0.8 && r[1] > well_cy + usb_exit_dy - usb_exit_h/2 - 0.8) 1]) == 0,
       "a lug pocket overlaps the cable exit — move lug_y / lug_mid_y");
assert(len([for (c = lug_ys) let (r = mount_dt_pocket_y(c, clear = dt_clear))
            if (r[0] < -plate_y/2 + 2.0 || r[1] > plate_y/2 - 2.0) 1]) == 0,
       "a lug pocket runs within 2 mm of the plate's edge — move lug_y");
assert(len([for (c = lug_ys, p = post_xy()) let (r = mount_dt_pocket_y(c, clear = dt_clear))
            if (abs(p[0]) - head_d/2 - 2.0 < mount_dt_window_w(dt_clear)/2 && p[1] > r[0] - head_d && p[1] < r[1] + head_d) 1]) == 0,
       "a lug pocket runs into a plate-screw seat")
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
assert(!opt_mark || mark_word_ink_w("securaCV", label_size) <= out_x - 4.0,
       str("the wordmark draws ", mark_word_ink_w("securaCV", label_size),
           " mm at label_size ", label_size, " on a ", out_x,
           " mm face (2 mm margin per side) — shrink label_size"));
// the hardware, DERIVED from the same knobs that draw the holes (canary_core_lib)
hw_thread = screw_insert ? "machine (into the inserts)" : "self-tap";
hw_echo("Vision doorbell", [
    hw_item(len(post_xy()), str(hw_screw(screw_size, screw_head, pl_L, hw_thread), " (plate to the post ends)")),
    hw_item(4, "M2 pan x 6 self-tap (OV5647 to the face posts)"),
    // the security boss takes screw_size's insert when screw_insert is on (its bore is ins_od),
    // so the security screw is that size's machine thread; self-tap builds keep the M2 pilot
    hw_item(1, str(screw_insert ? hw_size_name(screw_size) : "M2", " x 10 security screw, Torx pin/tri-wing, ",
                   screw_insert ? "machine thread (into the boss insert)" : "self-tap", " (wall plate collar into the shell's boss)")),
    screw_insert ? hw_item(len(post_xy()) + 1, str(str(hw_size_name(screw_size), " heat-set insert ", ins_od, " OD x ", ins_h), " — the +1 seats in the security boss")) : "",
    e_gland      ? hw_item(len(post_xy()), hw_oring(screw_size)) : "",
    hw_item(1, str("Ø", btn_d, " illuminated momentary button + panel nut (", btn_nut_ac, " AC)")),
    e_seal       ? hw_item(1, "TPU gasket (print part=\"gasket\")") : "",
    e_vent       ? hw_item(1, str("Ø", vent_pad_d, " adhesive ePTFE/GORE vent patch")) : "",
    e_led        ? hw_item(1, str("Ø", lp_d, " light pipe")) : "",
    cam_disc_t > 0 && cam_disc_d > 0 ? hw_item(1, str("Ø", cam_disc_d, " x ", cam_disc_t, " clear disc (neutral-cure silicone)")) : "",
    e_tamper     ? hw_item(1, str("Ø", mag_d, " x ", mag_h, " disc magnet (press + glue)")) : "",
    hw_item(4, "#8 pan wall screw (plate)"),
]);
echo(str("Canary Vision DOORBELL v0.6 — shell ", out_x, " x ", out_y, " x ", shell_d + lid_t,
         " mm (plate ", plate_x, " x ", plate_y, " x ", plate_t, " in the bore; ", len(post_xy()),
         " screws) + wall plate ", wplate_t, " mm (wedge ", plate_wedge, " deg, seal=", e_seal, ")"));

// ----------------------------------------------------------------------------
//  Helpers — the idiom once shared by copy with the other Canary enclosures
//  now comes from the canary_*_lib set; what stays local is doorbell-specific
//  (the rim ring, the wedge plate — and the T-stud, for the mesh-stability
//  reason at its definition)
// ----------------------------------------------------------------------------
// a ring of width w centered on the ledge band (the gasket's home)
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + ledge_w, inner_y + ledge_w, cav_r + ledge_w/2);
        offset(r = -w/2) rrect2d(inner_x + ledge_w, inner_y + ledge_w, cav_r + ledge_w/2);
    }
}
// The WAP cantilever clip, routed through canary_snap_lib so the strain gate
// runs on every render (the beam here rises 12 mm off the plate — 0.6 %,
// nothing near the budget). The lib places clips across a Y edge line; these
// stand on ±X board edges, so the wrapper keeps this file's original
// rotate-into-place transform — identity ops only, the released mesh stays put.
module edgeclip(px, py, ang, soff) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, floor_t, floor_t + soff + pcb_t,
                       w = clip_w, t = clip_t, hook = clip_hook,
                       hook_h = clip_hook_h, clear = clip_clear, root_r = clip_root_r);
}
// the module's edge guides stand this far over its top face: past the shell's
// headroom over the stack, so with the shell closed the module cannot lift
// out of them even if every clip is gone
guide_up  = lid_headroom + 0.8;
guide_top = floor_t + vm_standoff + pcb_t + guide_up;
assert(guide_top + 1.0 <= base_d, "the module's edge guides reach the face — lower guide_up");
// a CORNER SHOE under each lower corner of the module, outboard of the XIAO
// (which hangs under the lower half) and inside the clips' band (the mid posts
// clear that band by 0.5, mid_need): a seat under the corner, a guide up its
// side edge, a STOP under its bottom edge — mounted, the plate is vertical and
// that edge is where the module's weight goes — and a buttress behind the stop.
// It replaces a Ø2 x 12 pin: one 3 mm² layer line, no stop, nothing to brace it.
module corner_shoe(s) {
    x0 = xiao_w/2 + 0.1;  xg = vm_w/2 + clip_clear;  x1 = vm_w/2 + clip_stack;
    ye = vm_cy - vm_l/2;  stop_t = 1.6;  ys = ye - clip_clear - stop_t;
    translate([vm_cx, 0, 0]) mirror([s < 0 ? 1 : 0, 0, 0]) translate([0, 0, floor_t - 0.01]) {
        translate([x0, ye, 0]) cube([xg - x0, 3.0, vm_standoff + 0.01]);                 // seat
        translate([xg, ys, 0]) cube([x1 - xg, ye + 3.0 - ys, guide_top - floor_t + 0.01]); // side guide
        translate([x0, ys, 0]) cube([x1 - x0, stop_t, guide_top - floor_t + 0.01]);       // bottom stop
        hull() {                                                                            // buttress
            translate([x0, ys, 0]) cube([x1 - x0, 0.01, guide_top - floor_t + 0.01]);
            translate([x0, ys - 5.0, 0]) cube([x1 - x0, 0.01, 0.6]);
        }
    }
}
// a TOP GUIDE at each upper corner, on the rail's end: the rail still carries
// the module, the guide stands up its side edge (open at the top end, so the
// module drops in past the camera flex)
module top_guide(s) {
    xr = vm_w/2 - 3.0;  xg = vm_w/2 + clip_clear;  x1 = vm_w/2 + clip_stack;
    y0 = vm_cy + vm_l/2 - 4.0;
    translate([vm_cx, 0, 0]) mirror([s < 0 ? 1 : 0, 0, 0]) translate([0, y0, floor_t - 0.01]) {
        translate([xr, 0, 0]) cube([x1 - xr, 3.0, vm_standoff + 0.01]);
        translate([xg, 0, 0]) cube([x1 - xg, 3.0, guide_top - floor_t + 0.01]);
    }
}

// ----------------------------------------------------------------------------
//  The PLATE (part="body") — the chassis: the floor with its pocket slab, the
//  module rails, clips and pins, the cable exit, the T-stud pockets and the
//  seats the screws enter through. Drawn in the assembled frame: front face
//  at z = floor_t, body down to -mount_extra. Prints back-face down.
//  body() is the part name the catalog has always used for the chassis half
//  (render.sh, the builder manifest, the assembled-dims ledger); back() is
//  the piston-plate family name the pose/dims generators and probes read.
// ----------------------------------------------------------------------------
module back() body();
module body() {
    difference() {
        union() {
            translate([0, 0, floor_t]) pl_plate(plate_x, plate_y, plate_r, plate_t);
            // module rails, TOP HALF ONLY — the stacked XIAO (17.8 wide on the 20 mm
            // module) hangs beneath the LOWER half, so full-length side rails would
            // collide with it. Four clips (each edge, each half: the beams stand
            // outboard of the module's edge, so they clear the XIAO too — one can
            // break and three still hold), corner shoes under the lower corners,
            // top guides on the rails' ends.
            for (s = [1, -1]) {
                rail_l = vm_l/2 - 4;
                difference() {
                    translate([vm_cx + s*(vm_w/2 - 1.5) - 1.5, vm_cy + 2, floor_t - 0.01])
                        cube([3, rail_l, vm_standoff + 0.01]);
                    translate([vm_cx + s*(vm_w/2 - 1.5), vm_cy + 2 + rail_l/2, floor_t + vm_standoff/2])
                        cube([5, clip_w + 2, vm_standoff + 1], center = true);
                }
                edgeclip(vm_cx + s*vm_w/2, vm_cy + 2 + rail_l/2, s > 0 ? 0 : 180, vm_standoff);
                edgeclip(vm_cx + s*vm_w/2, vm_cy - vm_l/4, s > 0 ? 0 : 180, vm_standoff);
                corner_shoe(s);
                top_guide(s);
            }
        }
        // oval cable exit through the plate (aligns with the wall plate's hole)
        translate([usb_exit_dx, well_cy + usb_exit_dy, 0]) hull()
            for (s = [1, -1]) translate([s*(usb_exit_w - usb_exit_h)/2, 0, floor_t - plate_t - 1])
                cylinder(d = usb_exit_h, h = plate_t + 2);
        // the blind dovetail pockets from the back face, the drop-in window
        // under each and the channel above it (canary_mount_lib): offer the body
        // mount_dt_travel() high, windows over the lugs, and let it drop
        for (c = lug_ys) mount_dovelug_pocket(c, -mount_extra, clear = dt_clear);
        // the screws: seats through the plate, pan heads over their glands in seal mode
        for (p = post_xy())
            pl_seat_cut(p[0], p[1], floor_t, plate_t, screw_size, screw_head, pl_r, scr_c, tol_hole, e_gland);
        // the key slot in the plate's +Y edge (the rib is on the shell's bore wall)
        if (lid_key) translate([0, 0, floor_t]) lid_key_slot(0, bore_y/2 - tol_slide, 270, plate_t + 0.2, ledge_w + 0.4);
    }
}

// ----------------------------------------------------------------------------
//  The SHELL (part="face") — face, walls, posts and the security boss, one
//  piece: lens + disc seat, button bezel, optional vent/LED/label on the face,
//  the walls hanging below to the back face. Drawn with the face at
//  z = 0..lid_t; everything below z = 0 is inside or the walls. Prints face-down.
// ----------------------------------------------------------------------------
// The vent cluster and light-pipe bore are canary_core_lib's now
// (core_vent_cluster / core_lightpipe_bore) — this file used to carry its
// own copy of both, and the four copies across the weather shells had
// forked. The knobs above still ride in as arguments.
module front() face();   // the family name (see body/back above)
module face() { translate([0, 0, -base_d]) shell_asm(); }

// the shell in the ASSEMBLED frame (back face at -mount_extra, face at base_d..base_d + lid_t)
module shell_asm() {
    posts = post_xy();
    difference() {
        shell_solid();
        // the posts' blind pilots (or insert bores), up from their end faces —
        // cut LAST, through the posts the union below adds
        for (p = posts) {
            pl_post_pilot(p[0], p[1], floor_t + pl_relief(), pl_pil,
                          screw_insert ? scr_nominal(screw_size) + 0.3 : scr_d,
                          screw_insert, ins_od - 0.3, ins_h);
            // ...and the bore's footprint cleared down to the ledge plane: at
            // the pill's cavity corner a corner post overlaps the ledge cove,
            // whose 45° ramp otherwise leaves a 0.1 mm lip on the screw line
            // under the post end (the seal-off build's 9.6 mm corner)
            translate([p[0], p[1], floor_t - 0.1])
                cylinder(d = screw_insert ? ins_od - 0.3 : scr_d, h = pl_relief() + 0.2);
        }
        // security-screw bore, drilled LAST so it passes through the bottom
        // wall AND the boss, ending blind inside the boss (the seal envelope
        // stays intact). The length is DERIVED: wall_eff + sec_boss_d - 0.9
        // always leaves 1.0 mm of the boss as web. The old fixed 6.5 was
        // drilled for the sealed wall and broke through into the cavity on
        // any config with a thinner wall — a "blind" pilot that opened the
        // case it was securing.
        translate([0, -out_y/2 - 0.1, sec_z]) rotate([-90, 0, 0])
            cylinder(d = sec_bore_d, h = wall_eff + sec_boss_d - 0.9);
        // screw_insert: the security screw's heat-set insert seats from the OUTER
        // face (the wall plate's collar sits under it and the screw passes up
        // through the collar into the insert) — same bag as the posts' inserts
        if (screw_insert)
            translate([0, -out_y/2 - 0.1, sec_z]) rotate([-90, 0, 0])
                cylinder(d = ins_od - 0.3, h = ins_h + 0.1);
    }
}
module shell_solid() {
    posts = post_xy();
    gusset_h = max(2, post_h - 0.5);
    gusset_w = min(2.0, rib_t_max(wall_eff));
    union() {
        difference() {
            union() {
                // the catalog's two-stage face-down soft edge (canary_core_lib)
                translate([0, 0, base_d]) soft_edge_plate(out_x, out_y, rr, lid_t, lid_edge, lid_edge2);
                translate([0, 0, -mount_extra]) rrect(out_x, out_y, rr, shell_d + 0.01);
            }
            // the cavity, coved at the face and at the ledge (canary_core_lib)
            translate([0, 0, base_d]) pl_cavity_cut(inner_x, inner_y, cav_r, cav_d, floor_cove);
            // the plate's bore below the ledge
            pl_bore_cut(bore_x, bore_y, bore_r, floor_t, plate_t);
            // the gasket groove, cut into the ledge
            if (e_seal)
                translate([0, 0, floor_t - 0.01]) linear_extrude(gasket_groove + 0.01) rim_ring2d(gasket_w);
            translate([0, 0, base_d]) {
                // the lens hole, cut to the lens: the barrel's bore up to the
                // barrel's top, then a cone at the FOV half-angle (+ margin)
                // out through the face — the disc seat sits over it
                translate([lens_x, lens_y, -1]) cylinder(d = cam_ap_d, h = cam_barrel_in + 1.01);
                translate([lens_x, lens_y, cam_barrel_in])
                    cylinder(d1 = cam_ap_d, d2 = cam_ap_d + 2*(lid_t - cam_barrel_in + 0.01)*tan(cam_half),
                             h = lid_t - cam_barrel_in + 0.01);
                if (cam_disc_t > 0 && cam_disc_d > 0) {
                    translate([lens_x, lens_y, lid_t - (cam_disc_t + 0.2)])
                        cylinder(d = cam_disc_d + 2*tol_slide, h = cam_disc_t + 1);
                    // cosmetic 45° lead-in around the seat rim (cleaner edge, easier disc entry)
                    translate([lens_x, lens_y, lid_t - 0.4])
                        cylinder(d1 = cam_disc_d + 2*tol_slide, d2 = cam_disc_d + 2*tol_slide + 1.0, h = 0.41);
                    // the face prints face-DOWN: this seat is a counterbore on the bed, and the
                    // layer over its floor would bridge a ring round the cone — bridge steps
                    core_bridge_steps(lens_x, lens_y, cam_bore_top, cam_cone_d(cam_bore_top),
                                      cam_disc_d + 2*tol_slide, up = -1, layer = bridge_layer);
                }
                // button hole + bezel seat (+ matching lead-in rim)
                translate([0, btn_cy, -1]) cylinder(d = btn_d + 2*tol_slide, h = lid_t + 2);
                if (btn_bez_d > 0) {
                    translate([0, btn_cy, lid_t - btn_bez_t])
                        cylinder(d = btn_bez_d + 2*tol_slide, h = btn_bez_t + 1);
                    translate([0, btn_cy, lid_t - 0.4])
                        cylinder(d1 = btn_bez_d + 2*tol_slide, d2 = btn_bez_d + 2*tol_slide + 1.0, h = 0.41);
                    core_bridge_steps(0, btn_cy, lid_t - btn_bez_t, btn_d + 2*tol_slide,
                                      btn_bez_d + 2*tol_slide, up = -1, layer = bridge_layer);
                }
                if (e_led) core_lightpipe_bore(vm_cx + lp_dx, vm_cy + lp_dy, lid_t, lp_d, tol_press);
                if (e_vent) core_vent_cluster(vm_cx + vent_dx, vm_cy + vent_dy, lid_t,
                                              vent_pad_d, vent_pad_depth, vent_ring_d, vent_hole_d, vent_holes,
                                              seat_inner = true);   // the membrane goes INSIDE: nothing to bridge on the bed
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
            if (foot_cham > 0) foot_chamfer_ring(out_x, out_y, rr, foot_cham, -mount_extra);
            // weep at the cavity's lowest point (mounted button-down): through the
            // BOTTOM wall just above the plate's face, angled down, beside the
            // security screw, in front of the collar's drain slot and inboard of the
            // bottom posts' gussets. Too
            // small to matter for ingress; the pressure path is the vent membrane.
            if (e_weep)
                weep_cut(weep_x, -inner_y/2, floor_t + weep_d/2 + 0.2, "-y", wall_eff, weep_d);
        }
        // screw posts from the face's underside to the relief over the ledge,
        // gusseted to their walls (a mid-span post only to its own) — the
        // gussets root at the face and taper toward the ledge. A corner post
        // also fills the pocket between its two gussets and the cavity's
        // corner: with the cavity coved at the ledge too, that pocket would
        // otherwise close into a sealed void (the mesh gate counts it a part).
        // The webs and fills are clipped to the cavity outline + 0.5: at the
        // pill's 6.8 mm cavity corner a square fill would reach the gasket
        // groove, 1.2 mm out in the ledge band
        for (p = posts) translate([p[0], p[1], floor_t + pl_relief()]) cylinder(d = pd, h = post_h);
        intersection() {
            union() {
                for (p = posts) translate([0, 0, base_d]) mirror([0, 0, 1]) {
                    sx = sign(p[0]); sy = _is_corner(p) ? sign(p[1]) : 0;
                    corner_gusset(p[0], p[1], sx*(inner_x/2 + 0.5), p[1], gusset_h, wall_eff, pd, gusset_w);
                    if (sy != 0)
                        corner_gusset(p[0], p[1], p[0], sy*(inner_y/2 + 0.5), gusset_h, wall_eff, pd, gusset_w);
                }
                for (p = posts) if (_is_corner(p))
                    translate([min(p[0], sign(p[0])*(inner_x/2 + 0.5)), min(p[1], sign(p[1])*(inner_y/2 + 0.5)), floor_t + pl_relief()])
                        cube([inner_x/2 + 0.5 - abs(p[0]), inner_y/2 + 0.5 - abs(p[1]), post_h]);
            }
            translate([0, 0, floor_t]) rrect(inner_x + 1.0, inner_y + 1.0, cav_r + 0.5, cav_d + 1);
        }
        // internal boss backing the security screw, on the bottom wall's inner
        // face from the relief over the ledge up past the bore, its face-side
        // end tapered 45° to the wall so the face-down print carries it with
        // no support (a square block would hang sec_boss_d out from the wall).
        // With screw_insert the bore is the insert's; the boss's stock is the
        // same, the wall carries the insert
        hull() {
            translate([-4, -inner_y/2 - 0.1, floor_t + pl_relief()])
                cube([8, sec_boss_d + 0.1, sec_boss_top - floor_t - pl_relief()]);
            translate([-4, -inner_y/2 - 0.1, floor_t + pl_relief()])
                cube([8, 0.1, sec_boss_top + sec_boss_d - floor_t - pl_relief()]);
        }
        // the plate key (canary_core_lib): a rib on the +Y (camera-end) bore
        // wall, centered — the plate fits one way (lens up, button down)
        if (lid_key) lid_key_rib(0, bore_y/2, 270, floor_t, plate_t + 0.5);
        translate([0, 0, base_d]) {
            // perimeter rib ring (t³ stiffening), cleared around features and
            // posts; fused 0.2 into the walls
            if (lid_ribs) {
                ro_x = inner_x + 0.4;
                ro_y = inner_y + 0.4;
                difference() {
                    translate([0, 0, -lid_rib_h]) linear_extrude(lid_rib_h + 0.1)
                        difference() {
                            rrect2d(ro_x, ro_y, cav_r + 0.2);
                            rrect2d(ro_x - 2*lid_rib_w, ro_y - 2*lid_rib_w, 0.1);
                        }
                    for (p = post_xy())
                        translate([p[0], p[1], -lid_rib_h - 0.1]) cylinder(d = pd + 1.6, h = lid_rib_h + 0.2);
                    translate([lens_x, lens_y, -lid_rib_h - 0.1])
                        cylinder(d = max(cam_ap_d, cam_disc_d) + 3, h = lid_rib_h + 0.2);
                    for (c = cam_post_xy())
                        translate([c[0], c[1], -lid_rib_h - 0.1])
                            cylinder(d = cam_post_d + 2, h = lid_rib_h + 0.2);
                    translate([0, btn_cy, -lid_rib_h - 0.1])
                        cylinder(d = max(btn_bez_d, btn_d) + 4, h = lid_rib_h + 0.2);
                    if (e_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -lid_rib_h - 0.1])
                        cylinder(d = lp_d + 4, h = lid_rib_h + 0.2);
                    if (e_vent) translate([vm_cx + vent_dx, vm_cy + vent_dy, -lid_rib_h - 0.1])
                        cylinder(d = vent_pad_d + 3, h = lid_rib_h + 0.2);
                    if (e_tamper) translate([vm_cx + mag_dx, vm_cy + mag_dy, -lid_rib_h - 0.1])
                        cylinder(d = mag_d + 2*tol_press + 4.8, h = lid_rib_h + 0.2);
                }
            }
            // camera posts on the carrier's hole grid (cam_grid_dy above its center), the
            // length that stands the lens barrel cam_barrel_in into the face's bore
            for (c = cam_post_xy())
                translate([c[0], c[1], -cam_post_eff])
                    difference() {
                        cylinder(d = cam_post_d, h = cam_post_eff + 0.1);
                        translate([0, 0, -0.1]) cylinder(d = cam_screw_d, h = cam_post_eff - 0.8);
                    }
            if (e_tamper)
                translate([vm_cx + mag_dx, vm_cy + mag_dy, -mag_h]) difference() {
                    cylinder(d = mag_d + 2*tol_press + 2.4, h = mag_h + 0.1);
                    translate([0, 0, -0.1]) cylinder(d = mag_d + 2*tol_press, h = mag_h + 0.1);
                }
        }
    }
}

// ----------------------------------------------------------------------------
//  GASKET
// ----------------------------------------------------------------------------
module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5); }

// ----------------------------------------------------------------------------
//  WALL PLATE — flat or wedge; three dovetail LUGS the body drops onto;
//  counterbored wall screws; cable pass; and a COLLAR wrapping the body's
//  bottom end that carries the security screw. The slab spans the shell's
//  whole footprint, so mounted it covers the back face — the plate screws
//  under it are reachable only with the body off its lugs.
//  Modeled in print orientation (back on the bed).
// ----------------------------------------------------------------------------
// The collar replaces the v0.5 L-foot. A foot is a tab: standing off the plate
// it is a cantilever whose root is one layer line, and a pry at the doorbell's
// bottom (or a drop of the plate) peels it there. The collar is a U — a
// bottom wall the body rests on and side returns climbing the body's flanks —
// so a pry bends the returns IN THEIR OWN PLANE (deep beams), and the whole U
// roots along its length into the slab through a bridge under the body's
// edge. In the tilted frame (origin at the body's bottom edge, z = 0 the
// body's back face) the body's outline is rrect2d(out_x, out_y) at y = out_y/2.
module body_outline2d() translate([0, out_y/2]) rrect2d(out_x, out_y, rr);
module collar_in2d()    translate([0, collar_clear]) offset(r = collar_clear) body_outline2d();
module collar_out2d()   hull() { offset(r = collar_t) collar_in2d();
                                 translate([0, -(collar_tb - collar_t)]) offset(r = collar_t) collar_in2d(); }
module collar_clip2d()  translate([-out_x, -collar_tb - 1]) square([2*out_x, collar_arm + collar_tb + 1]);
collar_h = mount_extra + sec_z + max(sec_bore_max, sec_screw_d + 0.4)/2 + 2.4;   // 2.4 of collar over the bore
weep_x   = 7.0;   // the shell's weep exits its bottom wall here (beside the security screw)
assert(collar_arm > rr, "the collar's returns must climb past the pill's corner radius, or they wrap nothing");
assert(collar_tb >= 3.0, "the collar's bottom wall carries the security screw's head — keep it >= 3.0");
// top-surface height of the (possibly wedged) plate at a given y —
// thin at the bottom (-Y), thick at the top: the camera tilts DOWN toward
// the walk-up, the usual doorbell wedge direction
function plate_z(y) = wplate_t + (plate_wedge > 0 ? (y + out_y/2) * tan(plate_wedge) : 0);
// extra height the horizontal wedge adds at the plate edge
function plate_zx() = out_x/2 * tan(abs(plate_wedge_x));

// the wall-screw seat floor, from the wall side (see plate()): the head lands
// flush with the thin end, over the >= 1.0 mm web DESIGN_RULES §4 requires
plate_seat_z = wplate_t - plate_head_h - 0.2;
assert(plate_seat_z >= 1.0 - 1e-9,
       str("plate wall-screw seat leaves ", plate_seat_z, " mm under the head (< 1.0) — raise wplate_t or use a lower head"));

module plate() {
    hmax = plate_z(out_y/2) + 2*plate_zx() + 0.1;   // covers the HIGH side of the x-wedge too
    z_bot = -(wplate_t + plate_zx() + (collar_arm + collar_tb)*sin(abs(plate_wedge)) + 1);   // down past the wall plane
    difference() {
        union() {
            // slab with a sloped top: straight prism cut by the wedge plane
            difference() {
                rrect(out_x, out_y, rr, hmax);
                translate([0, -out_y/2, wplate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0])
                    translate([-out_x - 1, -1, 0]) cube([2*out_x + 2, out_y + rr + 2, hmax + out_y + out_x]);
            }
            // lugs, collar and (below) the security bore all live in the TILTED
            // frame so they stay aligned with the body resting on the wedge face.
            // The body rests on the collar's bottom wall with its outline on the
            // slab's, so each lug is drawn where its pocket parks it: offer the
            // windows over the lugs, drop the body mount_dt_travel(), it lands in
            // the collar
            translate([0, -out_y/2, wplate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0]) {
                for (c = lug_ys) mount_dovelug(out_y/2 + c);
                // the collar: the U up to collar_h over the body's back face...
                translate([0, 0, z_bot]) linear_extrude(collar_h - z_bot)
                    intersection() { difference() { collar_out2d(); collar_in2d(); } collar_clip2d(); }
                // ...and the bridge under the body's edge that roots it into the slab
                translate([0, 0, z_bot]) linear_extrude(-z_bot)
                    intersection() { difference() { collar_out2d(); offset(delta = -1) body_outline2d(); } collar_clip2d(); }
            }
        }
        // wall screws: through-holes + flat counterbores whose floor sits at a
        // CONSTANT height from the wall side, so standard-length screws work at
        // any wedge angle. The floor is DERIVED so a pan head lands flush with
        // the plate's thin end: the old constant 3.0 left a 1.0 seat on a 4.0
        // plate, and the heads (#8 pan 2.8 tall) stood 1.5-2 mm proud under a
        // SOLID body back — the body could not reach its lugs or security bore
        // (7.5 from the side edge: the counterbores stay clear of the lugs and,
        // at the bottom, of the collar's returns)
        for (sy = [1, -1], sx = [1, -1]) {
            translate([sx*(out_x/2 - 7.5), sy*(out_y/2 - 14), -0.1])
                cylinder(d = plate_screw_d, h = hmax + 1);
            translate([sx*(out_x/2 - 7.5), sy*(out_y/2 - 14), plate_seat_z])
                cylinder(d = plate_screw_d + 4.4, h = hmax + 1);
        }
        // cable pass (a roomier match for the back plate's oval exit)
        translate([usb_exit_dx, well_cy + usb_exit_dy, -0.1]) hull()
            for (s = [1, -1]) translate([s*(usb_exit_w - usb_exit_h)/2, 0, 0])
                cylinder(d = usb_exit_h + 4, h = hmax + 1);
        translate([0, -out_y/2, wplate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0]) {
            // security-screw bore: up through the collar's bottom wall into the shell's bore
            translate([0, -collar_tb - 0.1, mount_extra + sec_z]) rotate([-90, 0, 0])
                cylinder(d = sec_screw_d + 0.4, h = collar_tb + 0.3);
            // the weep's drain: a slot through the collar's bottom wall in front
            // of the shell's weep, open down to the slab — the collar's band over
            // it keeps the U closed
            translate([weep_x, -collar_tb - 0.1, 0]) hull() for (z = [(weep_d + 1.6)/2, mount_extra + floor_t + weep_d + 0.2])
                translate([0, 0, z]) rotate([-90, 0, 0]) cylinder(d = weep_d + 1.6, h = collar_tb + 0.3);
        }
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
    color(cw_body(colorway)) translate([out_x + 10, 0, lid_t]) rotate([180, 0, 0]) face();
    color(cw_body(colorway)) translate([-(out_x + 14), 0, 0]) plate();
    if (e_seal) color(cw_light(colorway)) translate([0, out_y + 12, 0]) gasket();
}
