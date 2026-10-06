// ============================================================================
//  SecuraCV Canary Vision — DOORBELL enclosure (parametric)  v0.8
// @env cer=2 ip="~IP54 (button ~IP65)"
//  A slim vertical unit in the Wyze/Ring video-doorbell form factor, holding
//  the stacked-XIAO Vision build: OV5647 camera (top) + Grove Vision AI V2
//  with a XIAO ESP32-C3/S3 seated in its socket (middle) + a 12 mm
//  illuminated momentary button (bottom, on the doorbell's own D1 input —
//  docs/hardware/canary_vision_doorbell_wiring.md), with a bay for a LiPo
//  ride-through cell beside the module and a landing for the XIAO's FPC
//  Wi-Fi antenna on the opposite wall.
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
//  v0.7 (2026-10-05): THE FIRST PRINT'S FINDINGS — a built v0.6, loaded with
//  the real stack, and everything it taught. Every mesh moves, and there is
//  a fifth part:
//    * THE LENS OPENING IS A LIP, NOT A COUNTERBORE. v0.6 seated the disc in
//      a pocket cut from the OUTER face, so printed face-down the pocket's
//      floor was a bridge over the lens cone and its steps showed through
//      the window as a slot and a square. The disc now goes in from the
//      INSIDE: it sits behind a cam_lip_t skin lip whose aperture is the
//      lens cone itself, and a printed RETAINER ring (part="retainer")
//      presses into a boss behind it and holds it — nothing in the hole is
//      bridged, the aperture is a clean cone with a 45° lead, and the lens
//      barrel nests into the retainer's bore as it did into the face;
//    * THE BODY IS LONGER WHERE THE CABLE LIVES. The cable well grows
//      12 -> 16 so a molded right-angle USB-C head (usb_plug_reach, asserted
//      against the button's nut) and the button's wires stop fighting; the
//      camera gap grows 2 -> 6 for the ribbon's loop; the oval exit is
//      14 x 10 (was 12 x 7) and the wall plate's pass is a SLOT that runs
//      the body's whole 8 mm drop, so the plug rides the slide;
//    * THE MODULE SEAT. The clips came up ~1.5 short of the module's top
//      face on the print: the seat (rails, shoes, guides, clip lips) rises
//      vm_seat_lift together, the upper clip pair moves down off the CSI
//      connector's ends (which span the module's full width at its top
//      edge), and a Ø5 post with an M2 pilot stands under each of the
//      module's two mounting holes (vm_hole_dx/dy — measured off the
//      print's photo, ±0.5: MEASURE), relieved where the XIAO's inner end
//      passes it, so the module is screwed down as well as clipped;
//    * TWO MILLIMETERS UNDER EVERY HEAD. The plate's recessed screw heads
//      bore on a 1.15 mm web (0.4 past the O-ring gland) and the wall
//      plate's counterbores left 1.0 under a #8 head — a driver would pull
//      either through. head_floor = 2.0 now sets the plate's recess (the
//      screws grow to M2 x 10) and the wall plate is 5.0 thick;
//    * A BATTERY BAY on the -X side: an 802030-class LiPo standing on edge
//      between the -X mid posts (now its end stops) behind a fence, and an
//      ANTENNA LANDING on the +X wall's inner face between two locating
//      ribs, the +X mid posts standing off the wall so the FPC passes
//      behind them. The module sits off the face's centerline by what the
//      bay needs (vm_cx derived); the lens, vent and button stay centered.
//  v0.8 (2026-10-06): THE SPEAKER. A doorbell that refuses a feed and a
//  microphone still owes the visitor one thing the glow cannot give: a
//  sound that says the house heard the press, loud enough for a street
//  (docs/hardware/canary_doorbell_research.md §4.3; the voice itself is
//  firmware/common/doorbell/doorbell_audio.h). A SPEAKER ZONE goes in
//  between the button and the cable well: a Ø spk_d sealed full-range driver
//  behind a GRILLE of Ø1.0 holes (the outdoor insect rule) on the face, an
//  acoustic mesh patch seated on the face's INNER side (nothing bridges on
//  the bed), the driver's rim in a BOSS ring on the inner side over a foam
//  gasket, and a CRADLE on the plate under the magnet so the plate screws
//  capture the driver — no screw touches the driver. The zone's walls carry
//  a mid-post pair each side of the cone (the gasket clamp rule still holds
//  at <= 40 mm) placed where the circle leaves room at the wall. The
//  amplifier board parks on edge beside the button. The body grows by the
//  zone (zone_spk = spk_d + 2*spk_gap); nothing else moves.
//  v0.9 (2026-10-06): the service plungers, the sunflower grille, the vent
//  gone. Three things the v0.8 review of the case asked for:
//    * SERVICE PLUNGERS (opt_svc): the XIAO's two tactile switches, R and
//      B, face the plate (its component side is its outward side in the
//      socket), 7-9 mm off the plate's inner face. Two TPU plungers go
//      through the plate over them — a flat head flush in a counterbore on
//      the back face, a stem in a bore through the plate and a short guide
//      boss on the inner face (stopped under the USB-C shell), a bead past
//      the boss's top that keeps it, a small tip that lands on the cap and
//      nothing else (the shell stands 0.7 beside the cap, four times
//      taller). Lift the body off its wall plate and press: a reset or a
//      boot-mode flash without a plate screw coming out. The tip rests
//      0.3 off the cap at the NEAREST the stack can sit (stack_sock_h is a
//      bench number to one PCB face, so the band is a PCB thick) — an
//      unpressed plunger can never hold R or B down; a sixth printed part,
//      part="plunger", TPU — ONE print, both buttons on a snap-off sprue
//      between their heads, so the pair cannot come out as two of one.
//      Which switch is R is a MEASURE knob (xiao_rst_side): the vendor
//      model carries no silkscreen; the dimples in the heads follow it
//      (one dimple = R, two = B).
//    * the GRILLE is a sunflower (grille_pattern): the holes sit on a
//      Fermat spiral at the golden angle, one per grille_pitch² of face —
//      the same open area as the rings, no ring reads as a ring, every
//      neighbor the same distance. The insect rule still holds (Ø1.0).
//    * the GORE VENT is OFF by default (opt_vent): the ring of ten holes
//      above the grille is gone from the face, and the case breathes
//      through the WEEP — the Ø2 drain through the bottom wall, open,
//      angled down, under the collar's slot — which the seal-path assert
//      has always accepted as a pressure path (field_ratings.md). What
//      that trades: the air exchange on a day/night cycle happens at the
//      drain, not through a membrane. The grille's membrane does NOT do
//      that job — the driver's back is sealed and its rim sits on a foam
//      ring, so the membrane protects the driver's own chamber, which the
//      cavity never sees (the first cut of this claimed otherwise; the
//      review caught it). A membrane-only path with no open hole is still
//      opt_vent = true, and the knob stays for it.
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
part = "all";        // ["body","face","plate","gasket","retainer","plunger","all"]   // (the speaker has no printed part of its own; "plunger" is the R + B pair on a sprue, TPU)

/* [Preset] — quick configs; choose "custom" to use the option checkboxes */
preset = "custom";   // ["custom","doorbell_weather"]

/* [Options (applied when preset = custom)] */
opt_seal   = true;   // perimeter TPU gasket in the shell's ledge (doorbells live outside)
opt_vent   = false;  // GORE vent cluster on the face — OFF since v0.9: the case breathes through
                     // the weep (an open Ø2 drain, opt_weep) instead of a membrane. A sealed
                     // outdoor unit with NO path at all pumps moist air past the seals on every
                     // day/night thermal cycle and the condensate never leaves — the assert below
                     // holds one path or the other. Turn this on for a membrane-only path (no
                     // open hole): then an adhesive GORE patch over the cluster is REQUIRED
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
e_vent   = _pre(opt_vent,   false);  // the released build breathes through its weep (field_ratings.md)
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
cam_barrel_in = 0.6; // how far the barrel stands into the retainer's bore; the rest under the disc is focus travel
cam_fov    = 62;     // diagonal field of view — the OV5647-62 datasheet  // [40:1:160]
cam_fov_margin = 4;  // degrees added to each side of the FOV cone the hole must clear
cam_screw_d = 1.6;   // pilot bored down each camera post for its screw
lens_dx  = 0.0;      // lens center X offset from the camera-board center — brd_ov5647_lens_dx(), the vendor CAD
lens_dy  = -1.7;     // lens center Y offset from the camera-board center: BELOW it, toward the ribbon edge — brd_ov5647_lens_dy(), the vendor CAD
cam_disc_d = 14.0;   // the clear disc's Ø — it seats from the INSIDE behind the face's lip
cam_disc_t = 1.0;    // the clear disc's thickness
cam_lip_t  = 1.0;    // the face's lip over the disc: skin left outside it, the lens cone cut through it  // [0.6:0.2:1.4]
cam_ret_t  = 1.0;    // the printed retainer ring behind the disc (part="retainer"): a light press in the boss
cam_boss_wall = 1.5; // the boss round the disc pocket, on the face's inner side, that the retainer presses into

/* [Button] — 12 mm panel-mount illuminated momentary (short body, IP65) */
btn_d      = 12.0;   // button thread/body diameter (hole = btn_d + 2*tol_slide)
btn_bez_d  = 16.5;   // bezel seat diameter on the face (0 = no seat)
btn_bez_t  = 1.0;    // bezel seat depth
btn_body_l = 18.0;   // body + terminals depth behind the panel — checked against the cavity
btn_nut_ac = 16.2;   // panel nut across corners (14 AF M12 nut = 16.2; 16 AF = 18.5) — asserted
                     // against the bottom screw posts
btn_wire_room = 5.0; // air behind the button's terminals for the solder joints and the wire bends toward the
                     // well (the v0.6 cavity left 3.2 — the first print's wires had nowhere to turn)

/* [Speaker] — a sealed full-range driver behind the face, between the button and the cable well (v0.8).
   Sized for the 36 mm / 4 Ω / 3 W class the BOM names (SPK1): with the 2.5 W class-D amplifier the
   target is a chime that carries over a busy street (unmetered — the wiring page owns that number). The driver's rim seats in a boss on the face's inner side over a foam
   gasket; a cradle on the plate presses its magnet when the plate screws down. */
opt_spk    = true;   // the speaker zone (grille, boss, cradle, its mid posts); off = the v0.7 body
spk_d      = 36.0;   // the driver's outside Ø (its frame or rim)
spk_h      = 6.0;    // the driver's depth, rim to magnet back
spk_rim_t  = 1.2;    // the frame's rim thickness the boss holds (a mylar driver's plastic frame)
spk_mag_d  = 20.0;   // the magnet's Ø — the cradle bears here, through a foam pad
spk_cone_d = 30.0;   // the grille's Ø over the cone (the holes stay inside the surround)
spk_gap    = 2.0;    // air round the driver to its zone's ends
spk_gasket_t = 1.0;  // the foam ring between the driver's rim and the face (compressed from ~1.5)
spk_pad_t  = 1.0;    // the foam pad between the magnet and the cradle
spk_boss_wall = 1.5; // the boss ring's wall on the face's inner side
spk_mesh_t = 0.3;    // the acoustic mesh patch's seat depth on the inner face (the patch is 0.2–0.3)
grille_hole_d = 1.0; // grille holes — the outdoor insect rule (<= 1.0)
grille_pitch  = 2.6; // hole spacing: one hole per grille_pitch² of face (sunflower), or ring-to-ring (rings)
grille_pattern = "sunflower";   // ["sunflower","rings"]  sunflower = a Fermat spiral at the golden angle (v0.9); rings = the v0.8 grille

/* [Service buttons] — two TPU plungers through the back plate over the XIAO's R and B switches (v0.9):
   lift the body off its wall plate and press, no plate screw comes out. The switch positions are the
   board registry's (brd_xiao_btn_*, the vendor GLB); which one is R is NOT in the model. */
opt_svc     = true;   // the plungers, their counterbores, bores and guide bosses; off = a plain plate there
svc_head_d  = 8.0;    // the plunger's head Ø — flat, flush in a counterbore on the back face
svc_web_t   = 0.8;    // the head's thickness: the web that flexes when you press (TPU 90-95A)
svc_stem_d  = 3.2;    // the stem, a sliding fit in its bore (+0.3)
svc_tip_d   = 1.8;    // the tip that lands on the switch cap (1.6 across; the USB-C shell stands 0.7 beside it)
svc_tip_h   = 0.8;    // the tip's height off the stem's end
svc_rest    = 0.3;    // the tip's rest gap over the cap at the NEAREST the stack can sit — never pressed by the case
svc_dy      = 0.4;    // the tip sits this far up the cap from its center (the guide boss then clears the plug's head)
svc_bead    = 0.4;    // the retaining bead's stand past the stem, just past the guide boss's top
svc_boss_wall = 1.0;  // the guide boss's wall on the plate's inner face
svc_pip_d   = 1.6;    // the dimples in the head that say which is which: one = R, two = B
svc_pip_depth = 0.4;
xiao_rst_side = -1;   // MEASURE: the side (±X, this file's frame) whose switch is R — the vendor model has no silkscreen;
                      // confirm against the board before trusting the dimples  // [-1, 1]
xiao_btn_dx = 5.93;   // the switches ± across the XIAO from its centerline — brd_xiao_btn_dx(), the vendor GLB
xiao_btn_dy = 1.61;   // ... inboard of the USB end along its length — brd_xiao_btn_dy()
xiao_btn_h  = 0.75;   // the cap's top over the PCB face — brd_xiao_btn_h()
xiao_btn_travel = 0.25;   // a 2.6 x 1.6 SMD tactile switch's travel to actuation
xiao_usb_overhang = 1.5;  // the USB-C shell past the XIAO's end — brd_xiao_usb_overhang()

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
zone_gap = 6.0;      // camera <-> module gap: the ribbon's loop between the carrier's edge and the CSI connector
zone_well = 16.0;    // cable well between module and button: the USB plug's head (usb_plug_reach), the button's
                     // wires and the glow driver's sleeve live here — asserted against the button nut
zone_btn = 23.0;     // button zone height: the body, its nut, and air round its rear terminals before the bottom wall's security boss
                     // (the speaker zone, zone_spk, is derived: spk_d + 2*spk_gap, between the button and the well)
usb_plug_reach = 13.0; // how far a molded right-angle USB-C HEAD stands off the XIAO's port face along the
                       // module's axis (molded heads run 9-13; a straight plug does not fit a doorbell)
usb_port_proud = 1.3;  // the XIAO's USB-C shell past the module's bottom edge (the port face the plug reach counts from)
usb_exit_w = 14.0;   // oval cable exit through the back plate (into the wall-plate slot) — sized for the
                     // plug head's cable leg with room to find it blind, not the 8.94 mm shell
                     // (port_usbc_shell_w()): v0.6's 12 x 7 was a fit, not a clearance
usb_exit_h = 10.0;   // the cable exit oval's height (Y); usb_exit_w is its length
usb_exit_dx = 0.0;   // exit offset from the MODULE's centerline (the plug hangs off the XIAO's port, not the face's axis)
usb_exit_dy = -1.0;  // exit offset from the well center: the oval sits wholly in the well, under nothing, a hair
                     // low so a head whose cable leg sits far from the port face still lands in it

/* [Print tolerances] — the catalog trio (canary_core_lib core_tol_*(),
   dialed on the fit coupon) */
tol_slide = 0.20;    // core_tol_slide()
tol_press = 0.10;    // core_tol_press()
tol_hole  = 0.30;    // core_tol_hole()

/* [Module seat] — what the first print taught about holding the module */
vm_seat_lift  = 1.5;    // the whole seat (rails, shoes, guides, clip lips, posts) rises this much over the
                     // measured stack: the built v0.6 held the module ~1.5 above its rails, so the lips
                     // never reached its top face. Re-measure on the next print and dial it to 0 if it seats.
vm_screws  = true;   // Ø vm_post_d posts with M2 pilots under the module's two mounting holes
vm_hole_dx = 7.5;    // the module's mounting holes, X from its centerline (one each side) — MEASURE: read off
                     // the print's photo (±0.5), the vendor CAD carries no holes
vm_hole_dy = 2.5;    // ...and Y from its center, toward the CSI (camera) end — MEASURE (±0.5)
vm_post_d  = 5.0;    // the board post's Ø (the M2 pilot leaves 1.7 of wall)
xiao_gap   = 0.3;    // air between a board post's relief flat and the XIAO's inner end, where it passes the post
clip_flex  = 1.0;    // free air behind each -X clip beam for its flex (the battery fence stands past it)

/* [Battery bay] — an 802030-class LiPo (8 x 20 x 30, 400-450 mAh) standing on edge beside the module,
   between the -X mid posts, behind a fence. Ride-through for a USB supply that browns out; the XIAO's own
   charger (C3: ~370 mA, S3: 100 mA) charges it. NO cold gate on either charger: outdoors below 0 °C leave
   the cell out (docs/hardware/cold_weather_envelope.md). */
opt_batt   = true;   // the bay (fence, end stops, the width it needs); off = the v0.6 cavity width on that side
batt_t     = 8.0;    // cell thickness (X, standing) — the 802030's 8.0 includes its PCM wrap
batt_w     = 20.0;   // cell width (Z, the cavity's depth) — asserted under the face
batt_l     = 30.0;   // cell length (Y, along the module)
batt_pcm   = 1.0;    // the protection board's length past the cell, under the wrap (0 on an unprotected cell — don't)
batt_clear = 0.3;    // air round the cell in the bay, per side
bay_wall_gap = 0.8;  // the cell stands this far off the -X wall (its foam strip and the wall's cove)
fence_t    = 0.8;    // the bay's fence, module side (two perimeters)
fence_h    = 6.0;    // ...and its height off the plate: the cell drops in from the front, the face holds it

/* [Antenna landing] — the XIAO's FPC Wi-Fi antenna (C3 kit: 40 x 20, Seeed 318020748; S3 A-02: 37.4 x 17.5)
   stuck to the +X wall's inner face between two locating ribs, its plane across the cavity so it radiates
   through the body toward the door and the house alike. The +X mid posts stand off the wall so the FPC
   passes behind them; the pigtail runs under the module to the XIAO's u.FL. */
opt_ant    = true;   // the landing (ribs, the post stand-off); off = the mid posts fuse to the wall as before
ant_l      = 40.0;   // antenna FPC length (Y)
ant_w      = 20.0;   // antenna FPC width (Z, across the cavity's depth) — the cavity deepens to center it
ant_t      = 0.3;    // FPC + its adhesive
ant_post_gap = 1.0;  // a +X mid post's stand-off from the wall: ant_t plus air for the FPC's edge
ant_rib_w  = 0.8;    // the two locating ribs along the landing's ends, proud of the wall by ant_rib_d
ant_rib_d  = 0.4;

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
head_floor   = 2.0;   // solid plastic under every recessed head — the plate screws' web past their gland and the
                      // wall plate's counterbore floors (the first print's 1.15 and 1.0 would pull through)

/* [Weather sealing] */
gasket_w      = 1.6;  // gasket groove width in the shell's ledge (the printed gasket is 0.5 narrower)
gasket_groove = 1.2;  // groove depth into the ledge
gasket_proud  = 0.3;  // how far the printed gasket stands proud of its groove, uncompressed — what the plate screws squeeze

/* [Wall plate + security screw] */
wplate_t    = 5.0;    // wall plate thickness at the THIN end: a #8 pan head recessed flush over head_floor
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
lug_y     = 41.5;   // the outer lugs park at y = ±lug_y (clear of the cable exit, its slide slot and the well)
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

/* [Front-face features] — X from the FACE's center (the module sits off it, vm_cx), Y from the MODULE center */
lp_d   = 3.0;      // light-pipe diameter (hole = lp_d + 2*tol_press) — core_lightpipe_d()
lp_dx  = 8.0;      // light-pipe port center X, from the face's center
lp_dy  = -8.0;     // light-pipe port center Y, from the module center
vent_pad_d     = 12.0;  // GORE-vent seat Ø, on the face's INNER side (it prints face-down: an outer seat cannot bridge) — core_vent_pad_d()
vent_pad_depth = 0.8;   // that seat's recess depth — core_vent_pad_depth()
vent_hole_d    = 1.0;   // fine holes — insect-resistant (the README's outdoor rule: <= 1.0 mm)
vent_ring_d    = 6.0;   // Ø of the ring the vent holes sit on — core_vent_ring_d()
vent_holes     = 10;    // more, smaller holes recover the open area at 1.0 mm
vent_dx        = 0.0;    // vent cluster center X, from the FACE's center; 0 keeps it on the face's vertical axis
                         // That axis is the camera → grille → button rhythm of a real doorbell;
                         // off-axis, the cluster read as an accidental drill pattern.
vent_dy        = -8.0;   // vent cluster center Y, from the module center
mag_d  = 6.0;      // tamper MAGNET diameter (pocket = mag_d + 2*tol_press — press fit)
mag_h  = 3.2;      // magnet thickness, and the pocket ring's height off the face's inside
mag_dx = 8.0;      // magnet pocket center X, from the face's center
mag_dy = 8.0;      // magnet pocket center Y, from the module center

/* [Board snap clips] — the WAP's print-proven numbers (the canary_snap_lib
   snap_boardclip defaults); the lib's strain gate holds them honest */
clip_w      = 6.0;   // board-clip tab width along the board edge — snap_boardclip default
clip_t      = 1.0;   // clip beam thickness — snap_boardclip default; the lib asserts its insertion strain
clip_hook   = 0.5;   // lip overhang over the board top — snap_boardclip default
clip_hook_h = 1.2;   // lip + 45° lead-in height above the board top — snap_boardclip default
clip_clear  = 0.25;  // beam face to board edge (a fit — tune on the coupon) — snap_boardclip default
clip_root_r = 0.6;   // 45° root fillet on each clip beam (snap_boardclip root_r) — a cantilever breaks at its root
clip_dy_hi  = 8.5;   // the upper clip pair's center, from the module's center (+ toward the camera): below the CSI
                     // connector's ends (v0.6's +10 put a lip on them) and above the board posts
clip_dy_lo  = -10.0; // the lower clip pair's center, from the module's center (over the XIAO's half)
csi_len     = 7.6;   // the module's CSI (camera ribbon) connector reaches this far in from its top edge, the
                     // full width — read off the print's photo (the vendor CAD puts its body at 1-7)

/* [Quality] */
bridge_layer = 0.2;  // your slicer's layer height: the button's bezel counterbore gets bridge steps this thick (core_bridge_steps)  // [0.08:0.04:0.32]
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
// the module's seat: the measured stack plus the v0.7 lift (the built v0.6
// held the module ~1.5 above its rails — whatever bottomed first, the seat
// now carries it there and the clips' lips meet its top face)
vm_standoff = stack_sock_h + xiao_below + vm_seat_lift;
z_vm_top    = floor_t + vm_standoff + pcb_t;           // the module's top face (assembled frame)
xiao_end_y  = -vm_l/2 + xiao_l;                        // the XIAO's inner end, from the module's center (+Y)

// THE LENS STACK, from the inside out, in the face's own frame (z = 0 the
// face's inner side, z = lid_t the skin): the printed RETAINER ring presses
// into a boss on the inner side, the clear DISC sits on it, and the face's
// LIP holds the disc from outside with the lens cone cut through it. The
// barrel nests cam_barrel_in into the retainer's bore (it registers the lens
// and blocks cavity light), the rest under the disc is focus travel.
cam_ap_d     = cam_barrel_d + 2*tol_slide;             // the retainer's bore
z_disc0      = lid_t - cam_lip_t - cam_disc_t;         // the disc's inner face
z_ret0       = z_disc0 - cam_ret_t;                    // the retainer's inner face
z_barrel_top = z_ret0 + cam_barrel_in;                 // the barrel's mouth
cam_focus    = z_disc0 - z_barrel_top;                 // air between the barrel's mouth and the disc
cam_post_eff = cam_lens_h - z_barrel_top;              // face underside -> the carrier's PCB face
cam_pock_d   = cam_disc_d + 2*tol_slide;               // the pocket: the disc slides, the retainer presses
cam_ret_od   = cam_pock_d - 2*tol_press;               // the retainer's OD, a light press in that pocket
cam_boss_od  = cam_pock_d + 2*cam_boss_wall;
cam_boss_h   = -z_ret0 + 0.1;                          // the boss stands this far into the cavity (the retainer 0.1 inside it)
cam_half     = cam_fov/2 + cam_fov_margin;             // the cone the opening must clear, per side
function cam_cone_d(z) = cam_barrel_d + 2*max(0, z - z_barrel_top)*tan(cam_half);   // the field's Ø at face height z
cam_lip_in   = cam_cone_d(lid_t - cam_lip_t) + 2*0.2;  // the lip's aperture at its back (the disc's ledge starts here)
cam_lip_out  = cam_cone_d(lid_t) + 2*0.2;              // ...and at the skin, before its 45° lead
// the camera's own screws: the longest standard length whose pilot stays 0.8
// short of the post's end (v0.6 named 6 mm for a post that could not take it)
cam_pil      = cam_post_eff - 0.8;
cam_scr_len  = max([for (l = hw_std_lens()) if (l - pcb_t <= cam_pil - 0.2) l]);

// the zones along Y are set by the boards alone, so they come first: the
// post rows and the mid posts read them, and the cavity's width reads the posts
// the top margin holds the top post row over the camera carrier: board_clear
// + the post + 1.0 of wall clearance — an insert build fattens the post, and
// zone_top's 8.0 was 0.1 short of it (the +inserts hardware set found it)
zone_top_eff = max(zone_top, board_clear + pd + 1.0);
zone_spk = opt_spk ? spk_d + 2*spk_gap : 0;        // the speaker zone, between the button and the well
inner_y = zone_btn + zone_spk + zone_well + (vm_l + board_clear) + zone_gap + cam_h + zone_top_eff;
btn_cy  = -inner_y/2 + zone_btn/2;
spk_cy  = -inner_y/2 + zone_btn + zone_spk/2;       // the driver's axis (on the face's centerline)
well_cy = -inner_y/2 + zone_btn + zone_spk + zone_well/2;      // cable well / USB plug space
vm_cy   = -inner_y/2 + zone_btn + zone_spk + zone_well + board_clear + vm_l/2;
cam_cy  = vm_cy + vm_l/2 + zone_gap + cam_h/2;
cam_cx  = 0;                                        // the lens stays on the face's centerline (vm_cx is derived below)
lens_x  = cam_cx + lens_dx;  lens_y = cam_cy + lens_dy;
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
// with the battery bay the mid posts are its END STOPS: one pair, centered on
// the module, their inner faces the cell's length plus its clearance apart
// (the clamp-span rule below still holds them — it is asserted, not assumed)
batt_cy      = vm_cy;
batt_pitch_y = batt_l + batt_pcm + 2*batt_clear + pd;    // post center to post center: the cell and its clearance
// the speaker zone carries a pair of mid posts each side of the cone, where
// the circle leaves room at the wall (spk_post_dy from the axis, asserted
// clear of the driver); with the bay off the zone still needs them
spk_post_dy = 16.0;
spk_mid_ys  = opt_spk ? [spk_cy - spk_post_dy, spk_cy + spk_post_dy] : [];
bay_ys = opt_batt ? [batt_cy - batt_pitch_y/2, batt_cy + batt_pitch_y/2] : [];   // the bay's end-stop posts
mid_ys = opt_batt ? concat(spk_mid_ys, bay_ys)
       : opt_spk  ? concat(spk_mid_ys, n_mid > 1 ? [for (i = [1 : n_mid - 1]) spk_cy + spk_post_dy + i*(post_y_top - spk_cy - spk_post_dy)/n_mid] : [])
       : n_mid > 0 ? [for (i = [1 : n_mid]) post_y_bot + i*(post_y_top - post_y_bot)/(n_mid + 1)] : [];

// THE CAVITY'S WIDTH is the sum of what stands across it, read outward from
// the module's two edges — the v0.5 doctrine (a mid post clears the module's
// clip by 0.5, never lands on it) with the bay and the landing added:
//   +X: clip | 0.5 | mid post | the antenna's air behind it (or 0.2 into the wall)
//   -X: clip | its flex | fence | the cell and its clearance | the wall gap
//       (or the +X stack mirrored)
hx_p = vm_w/2 + clip_stack + 0.5 + pd + (opt_ant ? ant_post_gap : -0.2);
hx_m = vm_w/2 + clip_stack + (opt_batt ? clip_flex + fence_t + batt_clear + batt_t + bay_wall_gap
                                       : 0.5 + pd - 0.2);
// screw_insert grows the posts (pd) by 1.5, which walked the bottom pair into
// the button nut's arc — the option asserted itself dead. The cavity widens
// by what the nut arc needs instead (the posts stay 1.0 off the walls; they
// cannot move down, and up is toward the nut); the camera carrier is centered
// on the face, so its clearance reads against the half-width either side
inner_x = max(hx_p + hx_m, cam_w + 2*board_clear,
              screw_insert ? 2*(sqrt(max(0, pow(btn_nut_ac/2 + pd/2 + 0.5, 2) - pow(zone_btn/2 - pd/2 - 1.0, 2)))
                                + pd/2 + 1.0) + 0.1 : 0);
vm_cx   = -inner_x/2 + hx_m + (inner_x - hx_p - hx_m)/2;   // the module's centerline (any slack splits evenly)
mid_px_p =  inner_x/2 - pd/2 - (opt_ant ? ant_post_gap : -0.2);   // the +X mid posts' axis
mid_px_m = -(inner_x/2 - pd/2 + 0.2);                             // the -X mid posts' axis (0.2 into the wall)
clip_y_hi = vm_cy + clip_dy_hi;  clip_y_lo = vm_cy + clip_dy_lo;   // the clip pairs
fence_x   = -inner_x/2 + bay_wall_gap + batt_t + batt_clear;      // the bay fence's -X face (the cell side)
// the cavity's depth: the stack and its headroom, the button's body with its
// wire room, the cell standing on edge, the antenna centered between the coves
cav_d   = max(vm_standoff + pcb_t + vm_front_h + cav_extra,
              btn_body_l - lid_t + btn_wire_room,
              opt_batt ? batt_w + 2*batt_clear + 0.5 : 0,
              opt_ant  ? ant_w + 2*floor_cove + 0.4 : 0,
              opt_spk  ? spk_gasket_t + spk_h + spk_pad_t + 3.0 : 0);
// the speaker stack from the face inward: gasket, driver, pad, then the cradle
// down to the plate's face
spk_boss_h  = spk_gasket_t + spk_rim_t + 0.5;         // the boss ring stands this far into the cavity
spk_boss_id = spk_d + 2*tol_slide;
spk_boss_od = spk_boss_id + 2*spk_boss_wall;
spk_mesh_d  = spk_cone_d + 4.0;                       // the mesh patch, inside the boss
cradle_h    = cav_d - (spk_gasket_t + spk_h + spk_pad_t);   // plate face -> the pad under the magnet
cradle_od   = spk_mag_d + 2.0;
cradle_id   = max(4.0, spk_mag_d - 6.0);

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
// the head's recess: as deep as leaves head_floor of SOLID plate past the
// gland (pl_recess's own floor is the gland web, 0.4 — the first print's
// heads bore on 1.15 mm of plastic); a shallower recess means a longer screw,
// which pl_len picks from the standard lengths
pl_r     = max(0, plate_t - pl_hh(screw_size, screw_head) - pl_gland_raw(screw_size, e_gland) - head_floor);
assert(pl_r <= pl_recess(plate_t, screw_size, screw_head, e_gland) + 1e-9, "head_floor is thinner than the plate's own gland web");
assert(plate_t - pl_r - pl_hh(screw_size, screw_head) - pl_gland_raw(screw_size, e_gland) >= head_floor - 1e-9,
       str("the plate leaves under ", head_floor, " mm of solid web under a recessed screw head — raise lug_extra"));
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

// the screw posts: the four corners, plus the mid posts — the -X pair 0.2 INTO
// its wall (the bay's end stops), the +X pair ant_post_gap off its wall with
// the antenna behind them (or 0.2 into it without the landing)
function post_xy() = concat([
    [ inner_x/2 - pd/2 - 1.0,  post_y_top],
    [-inner_x/2 + pd/2 + 1.0,  post_y_top],
    [ inner_x/2 - pd/2 - 1.0,  post_y_bot],
    [-inner_x/2 + pd/2 + 1.0,  post_y_bot],
], [for (y = mid_ys, s = [1, -1]) [s > 0 ? mid_px_p : mid_px_m, y]]);
function _is_corner(p) = abs(p[1]) > inner_y/2 - 10;
function _is_free(p)   = opt_ant && !_is_corner(p) && p[0] > 0;   // a +X mid post: no wall gusset, the antenna passes behind it

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
// the speaker is NOT a path: its membrane fronts a sealed-back driver on a foam
// ring, a chamber the cavity never sees (the v0.9 review caught the first cut
// of this counting it)
assert(!e_seal || e_vent || e_weep,
       "seal mode with no pressure path — enable opt_vent (GORE seat) or opt_weep (field_ratings.md)");
// the button's panel nut must clear the bottom posts (their inner edge vs the nut's corner radius)
assert(len([for (p = post_xy()) if (sqrt(pow(p[0], 2) + pow(p[1] - btn_cy, 2)) < btn_nut_ac/2 + pd/2 + 0.5) 1]) == 0,
       str("a ", btn_nut_ac, " mm across-corners button nut hits a screw post — smaller nut, or move the posts"));
// the cable oval must sit under the WELL: below the module's bottom edge and clear of any mid post in its band
exit_cx = vm_cx + usb_exit_dx;  exit_cy = well_cy + usb_exit_dy;
assert(exit_cy + usb_exit_h/2 <= vm_cy - vm_l/2 - 0.5,
       "cable exit reaches under the module/XIAO — lower usb_exit_dy or lengthen zone_well");
assert(len([for (p = post_xy()) if (!_is_corner(p)
            && abs(p[1] - exit_cy) < pd/2 + usb_exit_h/2
            && abs(exit_cx) + usb_exit_w/2 > abs(p[0]) - pd/2 - 1.0) 1]) == 0,
       "cable exit runs into a mid-span post — center it (usb_exit_dx) or narrow it");
// the USB plug: a right-angle head reaching usb_plug_reach off the XIAO's port
// face must stop 1.5 short of the button's nut, and the well must hold it
plug_end_y = vm_cy - vm_l/2 - usb_port_proud - usb_plug_reach;
assert(plug_end_y - (btn_cy + btn_nut_ac/2) >= 1.5 - 1e-9,
       str("a ", usb_plug_reach, " mm USB-C plug head reaches ", (btn_cy + btn_nut_ac/2) - plug_end_y + 1.5,
           " mm into the button nut's room — lengthen zone_well"));
assert(btn_body_l - lid_t + btn_wire_room <= cav_d + 1e-9, "button too deep — raise btn_body_l budget or cavity");
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
assert(cam_disc_d > cam_ap_d && cam_disc_t > 0, "the lens stack needs a clear disc wider than the lens bore (cam_disc_d, cam_disc_t)");
// the face prints face-down: the bezel's bed-side counterbore floor must sit on a whole layer,
// or the bridge steps over it land between layers and the slicer merges them away
// (the lens opening has no counterbore on the bed any more: the disc seats from inside)
function _on_layer(d) = abs(d/bridge_layer - round(d/bridge_layer)) < 1e-6;
assert(btn_bez_d == 0 || _on_layer(btn_bez_t),
       str("the bezel seat depth (", btn_bez_t, ") must be a whole number of ", bridge_layer,
           " mm layers — set bridge_layer to your slicer's layer height"));
assert(mount_dt_depth() + 1.5 <= plate_t, "the lug pockets leave under 1.5 mm of plate over them — raise lug_extra");
// the camera: the holder clears the boss the retainer presses into, the barrel
// nests in the retainer and keeps focus travel under the disc, the lip leaves
// the disc a ledge, the skin stays over the lip, and nothing crops the field
assert(cam_lip_t >= 0.6 && cam_lip_t + cam_disc_t <= lid_t + 1e-9,
       "cam_lip_t: the lip needs >= 0.6 of skin and the disc must sit within the face (cam_lip_t + cam_disc_t <= lid_t)");
assert(cam_holder_h + 0.5 <= cam_post_eff - cam_boss_h - 1e-9,
       str("the lens holder stands ", cam_post_eff - cam_boss_h - cam_holder_h, " mm under the retainer boss (< 0.5) — lower cam_barrel_in"));
assert(cam_barrel_in >= 0.3 && cam_barrel_in <= cam_ret_t - 0.2 && cam_focus >= 0.3 - 1e-9,
       "the lens barrel needs >= 0.3 in the retainer's bore (and the retainer 0.2 past it) and >= 0.3 of focus travel under the disc");
assert(cam_lip_in + 2*1.0 <= cam_pock_d - 1e-9,
       str("the lens cone is ", cam_lip_in, " mm across at the lip's back — under 1.0 of ledge a side under a Ø",
           cam_disc_d, " disc; widen cam_disc_d or thin cam_lip_t"));
assert(cam_lip_out + 0.8 <= cam_disc_d - 1e-9,
       str("the lens opening is ", cam_lip_out, " mm across at the skin (+0.8 lead) — the lip would open past the disc; widen cam_disc_d"));
assert(cam_ret_od - cam_ap_d >= 2*2.0, "the retainer ring is under 2.0 wide between its bore and its OD");
assert(cam_boss_h <= boss_h_max(cam_boss_wall) + 1e-9, "the retainer boss is too tall for its wall (canary_rib_lib boss_h_max)");
assert(cam_scr_len >= 4, "no standard screw length fits the camera posts' pilot");
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
// with the speaker zone the cable exit sits near the body's middle, so the
// middle lug parks BELOW it (16 off the exit's axis: its pocket clears the
// oval, the wall plate's slot and the lower outer lug's pocket); without
// the zone it parks where lug_mid_y says, above the exit as in v0.6
lug_mid_eff = opt_spk ? exit_cy - 16.0 : lug_mid_y;
lug_ys = [lug_y, lug_mid_eff, -lug_y];
assert(len([for (c = lug_ys) let (r = mount_dt_pocket_y(c, clear = dt_clear))
            if (r[0] < well_cy + usb_exit_dy + usb_exit_h/2 + 0.8 && r[1] > well_cy + usb_exit_dy - usb_exit_h/2 - 0.8) 1]) == 0,
       "a lug pocket overlaps the cable exit — move lug_y / lug_mid_y");
assert(len([for (c = lug_ys) let (r = mount_dt_pocket_y(c, clear = dt_clear))
            if (r[0] < -plate_y/2 + 2.0 || r[1] > plate_y/2 - 2.0) 1]) == 0,
       "a lug pocket runs within 2 mm of the plate's edge — move lug_y");
assert(len([for (c = lug_ys, p = post_xy()) let (r = mount_dt_pocket_y(c, clear = dt_clear))
            if (abs(p[0]) - head_d/2 - 2.0 < mount_dt_window_w(dt_clear)/2 && p[1] > r[0] - head_d && p[1] < r[1] + head_d) 1]) == 0,
       "a lug pocket runs into a plate-screw seat")
assert(abs(exit_cx) + usb_exit_w/2 <= inner_x/2 - 1, "cable exit too wide/offset for the cavity");
// the LUGS (on the wall plate) must also clear its cable SLOT, which runs the
// oval's pass mount_dt_travel() up the slide (the plug rides the drop), by 1.0
assert(len([for (c = lug_ys)
            if (c - mount_dt_len()/2 < exit_cy + usb_exit_h/2 + mount_dt_travel() + 2.0 + 1.0
                && c + mount_dt_len()/2 > exit_cy - usb_exit_h/2 - 2.0 - 1.0) 1]) == 0,
       "a lug lands on the wall plate's cable slot — move lug_y / lug_mid_y");
// THE MODULE SEAT: the board posts stand on the module's holes, clear of the
// XIAO's socket below the module (its plastic ends 1.6 inside the XIAO's
// outline) and relieved past the XIAO's own end; the lips land on the top face
vm_post_xy = vm_screws ? [for (sx = [1, -1]) [vm_cx + sx*vm_hole_dx, vm_cy + vm_hole_dy]] : [];
sock_end_y = vm_cy + xiao_end_y - 1.6;                          // the socket header's plastic, the XIAO's end less its 1.6 overhang
// the XIAO's PCB band below the socket: stack_sock_h is a bench number to one
// of its faces, so the band is drawn a PCB thick either side of it, plus 0.5
xiao_lo_z  = z_vm_top - pcb_t - stack_sock_h - brd_t("xiao") - 0.5;
xiao_hi_z  = z_vm_top - pcb_t - stack_sock_h + brd_t("xiao") + 0.5;
post_flat_y = vm_cy + xiao_end_y + xiao_gap;                    // a post is relieved to this flat in that band

// SERVICE PLUNGERS (v0.9) — everything in the plate's frame (z = floor_t its
// inner face, floor_t - plate_t its back face). The XIAO's component face is
// its outward face in the socket: stack_sock_h is a bench number to ONE of
// its PCB faces, so the face sits somewhere in a PCB-thick band, and the
// plunger is drawn against BOTH ends of it — the tip rests svc_rest off the
// cap at the near end (it can never hold a switch), and the stroke is
// stated for the far end.
xiao_face_hi_z = z_vm_top - pcb_t - stack_sock_h;               // the component face, the measurement to that face
xiao_face_lo_z = xiao_face_hi_z - brd_t("xiao");                // ...or to the socket face: the component face a PCB nearer the plate
svc_cap_lo_z   = xiao_face_lo_z - xiao_btn_h;                   // the cap's top at the nearest the stack can sit
svc_cap_hi_z   = xiao_face_hi_z - xiao_btn_h;
xiao_usb_end_y = vm_cy - vm_l/2 - usb_port_proud + xiao_usb_overhang;   // the XIAO's board end (the USB-C shell stands past it)
svc_cy         = xiao_usb_end_y + xiao_btn_dy + svc_dy;         // the plunger axis along Y
function svc_xy() = opt_svc ? [for (sx = [1, -1]) [vm_cx + sx*xiao_btn_dx, svc_cy]] : [];
svc_bore_d     = svc_stem_d + 0.3;                              // the stem slides
svc_cb_d       = svc_head_d + 0.4;                              // the head's counterbore on the back face
svc_cb_depth   = svc_web_t + 0.3;                               // the head sits 0.3 under the back face: nothing proud against the wall plate's slab
svc_boss_od    = svc_bore_d + 2*svc_boss_wall;
svc_boss_top   = xiao_face_lo_z - port_usbc_shell_h() - 0.5;    // the guide boss stops under the USB-C shell's lowest edge, stack at the near end of its band
svc_boss_h     = svc_boss_top - floor_t;
svc_tip_z      = svc_cap_lo_z - svc_rest;                       // the tip's end at rest
svc_head_z     = floor_t - plate_t + (svc_cb_depth - svc_web_t);   // the head's OUTER face: 0.3 under the back face; its inner face is the counterbore's floor
svc_stem_len   = svc_tip_z - svc_tip_h - (svc_head_z + svc_web_t);   // the stem, head to tip
svc_bead_z     = svc_boss_top + 0.2 - svc_head_z;               // the bead's start, in the plunger's own frame (head's floor = 0)
svc_stroke_lo  = svc_rest + xiao_btn_travel;                    // the press that clicks, stack at the near end
svc_stroke_hi  = (svc_cap_hi_z - svc_tip_z) + xiao_btn_travel;  // ...and at the far end
// the tip lands on the cap and nothing else: the USB-C shell beside it, the cap's own extent
assert(!opt_svc || xiao_btn_dx - svc_tip_d/2 >= port_usbc_shell_w()/2 + 0.5 - 1e-9,
       "the service plunger's tip reaches the USB-C shell beside the switch — a smaller svc_tip_d");
assert(!opt_svc || svc_dy + svc_tip_d/2 <= brd_xiao_btn_cap()[0]/2 + 0.1 + 1e-9 && svc_tip_d <= brd_xiao_btn_cap()[1] + 0.2 + 1e-9,
       "the service plunger's tip runs off the switch cap — svc_dy / svc_tip_d");
// THE ONE THAT MATTERS: an unpressed plunger must never hold R or B down — it
// rests svc_rest off the cap even with the stack at the nearest the band allows
assert(!opt_svc || svc_rest >= 0.3 - 1e-9, "svc_rest under 0.3 — a plunger that can hold RESET down is a dead board with no symptom");
assert(!opt_svc || svc_stem_len >= 3.0, "the service plunger's stem is too short to reach — the stack sits too near the plate");
// the guide boss: under the USB-C shell, inside canary_rib_lib's boss rule, clear
// of the plug's head below the port face and of the corner shoes beside the XIAO
assert(!opt_svc || svc_boss_h >= 1.5 && svc_boss_h <= boss_h_max(svc_boss_wall) + 1e-9,
       str("the service plunger's guide boss stands ", svc_boss_h, " mm on a ", svc_boss_wall, " mm wall"));
assert(!opt_svc || svc_cy - svc_boss_od/2 >= vm_cy - vm_l/2 - usb_port_proud + 0.3 - 1e-9,
       "the service plunger's guide boss reaches past the port face into the USB plug's head — raise svc_dy");
assert(!opt_svc || xiao_btn_dx + svc_boss_od/2 <= xiao_w/2 + 0.1 - 1e-9,
       "the service plunger's guide boss runs into the module's corner shoe — a thinner svc_boss_wall");
// the plate's other features: the cable oval below, the lug pockets, the screw seats
assert(!opt_svc || svc_cy - svc_cb_d/2 >= exit_cy + usb_exit_h/2 + 1.0 - 1e-9,
       "a service plunger's counterbore meets the cable exit's oval");
assert(!opt_svc || len([for (q = svc_xy(), c = lug_ys) let (r = mount_dt_pocket_y(c, clear = dt_clear))
                        if (q[1] + svc_cb_d/2 > r[0] - 1.0 && q[1] - svc_cb_d/2 < r[1] + 1.0
                            && abs(q[0]) - svc_cb_d/2 < mount_dt_window_w(dt_clear)/2 + 1.0) 1]) == 0,
       "a service plunger's counterbore meets a dovetail lug pocket");
assert(!opt_svc || len([for (q = svc_xy(), p = post_xy()) if (norm([q[0] - p[0], q[1] - p[1]]) < svc_boss_od/2 + pd/2 + 1.0) 1]) == 0,
       "a service plunger's guide boss meets a screw post's seat");
if (opt_svc)
    echo(str("service plungers: tip rests ", svc_rest, " off the cap at the near end of the stack band; the press that clicks is ",
             svc_stroke_lo, " to ", svc_stroke_hi, " mm (stack_sock_h to one PCB face or the other); stem ", svc_stem_len,
             " mm; guide boss ", svc_boss_h, " mm, stopped 0.5 under the USB-C shell at the near end of the band"));
assert(!vm_screws || vm_hole_dy - vm_post_d/2 >= xiao_end_y - 1.6 + 0.3 - 1e-9,
       str("a board post (Ø", vm_post_d, " on vm_hole_dy ", vm_hole_dy, ") lands on the XIAO socket's end — measure vm_hole_dy"));
assert(!vm_screws || abs(vm_hole_dx) + vm_post_d/2 <= vm_w/2 + clip_clear - 1e-9,
       "a board post stands past the module's edge into the clips — check vm_hole_dx");
assert(!vm_screws || vm_hole_dy + vm_post_d/2 <= vm_l/2 - 1e-9, "a board post runs off the module's end");
// the board screw: the longest standard length whose pilot ends 0.3 above the
// relief band — the relieved side of the post never carries thread, so the
// flat may sit as close to the pilot as the hole's position makes it
vm_scr_len = vm_screws ? max(concat([0], [for (l = hw_std_lens()) if (l - pcb_t + 1.0 <= z_vm_top - pcb_t - xiao_hi_z - 0.3) l])) : 0;
assert(!vm_screws || vm_scr_len >= 4, "no standard screw length fits the board posts above the XIAO relief — raise stack_sock_h's seat or drop vm_screws");
// the upper clips sit below the CSI connector's ends (it spans the module's
// full width at its top edge) and above the board posts
csi_y0 = vm_cy + vm_l/2 - csi_len;
assert(clip_y_hi + clip_w/2 <= csi_y0 - 0.3 && clip_y_hi - clip_w/2 >= (vm_screws ? vm_cy + vm_hole_dy + vm_post_d/2 + 0.3 : -1e9),
       str("the upper clip pair (", clip_y_hi - vm_cy, " from the module's center) must clear the CSI connector (from ",
           csi_y0 - vm_cy, ") and the board posts — move clip_dy_hi"));
assert(clip_y_lo + clip_w/2 <= (vm_screws ? vm_cy + vm_hole_dy - vm_post_d/2 - 0.3 : 1e9)
       && clip_y_lo - clip_w/2 >= vm_cy - vm_l/2 + 3.0 + 0.3,
       "the lower clip pair must sit between the corner shoes' seats and the board posts — move clip_dy_lo");
// THE BATTERY BAY: the cell stands between the -X mid posts (its end stops),
// the fence stands clip_flex behind the -X clip beams, and the face holds it
assert(!opt_batt || len(bay_ys) == 2, "the battery bay needs its two end-stop posts");
assert(!opt_batt || (bay_ys[1] - pd/2) - (bay_ys[0] + pd/2) >= batt_l + batt_pcm + 2*batt_clear - 1e-9,
       "the battery bay's end stops are too close for the cell");
assert(!opt_batt || bay_ys[0] - pd/2 >= post_y_bot + pd/2 + 2.0 && bay_ys[1] + pd/2 <= post_y_top - pd/2 - 2.0,
       "the battery bay's end stops run into the corner posts — a shorter cell, or lengthen the body");
// the -X corner shoe's buttress and the fence must not meet: the shoe hugs
// the module's lower corner, the fence stands further out
assert(!opt_batt || fence_x + fence_t <= vm_cx - vm_w/2 - clip_stack - clip_flex + 1e-9, "the bay fence stands inside the clips' flex room");
// THE SPEAKER: the driver clears its zone's posts and the walls, the grille
// stays inside the cone, the mesh patch inside the boss, the boss inside the
// cavity, and the cradle is a boss a tube can be
assert(!opt_spk || spk_d + 2*0.5 <= inner_x + 1e-9, "the speaker driver is wider than the cavity (0.5 a side) — a smaller spk_d, or widen it");
assert(!opt_spk || spk_boss_od <= inner_x + 2*0.5 + 1e-9, "the speaker boss ring runs past the walls");
assert(!opt_spk || len([for (p = post_xy()) if (norm([p[0], p[1] - spk_cy]) - pd/2 < spk_d/2 + 0.5) 1]) == 0,
       str("a screw post lands on the Ø", spk_d, " speaker driver — move spk_post_dy or shrink spk_d"));
assert(!opt_spk || spk_cone_d + 2.0 <= spk_d, "the grille runs out past the driver's surround (spk_cone_d vs spk_d)");
assert(!opt_spk || spk_mesh_d + 1.0 <= spk_boss_id, "the mesh patch does not fit inside the boss ring");
assert(!opt_spk || grille_hole_d <= 1.0 + 1e-9, "grille holes over 1.0 mm let insects in (the outdoor rule)");
assert(!opt_spk || cradle_h >= 3.0 && cradle_h <= boss_h_max((cradle_od - cradle_id)/2) + 1e-9,
       str("the speaker cradle stands ", cradle_h, " mm on a ", (cradle_od - cradle_id)/2, " mm wall — outside canary_rib_lib's boss rule"));
assert(!opt_spk || spk_mag_d + 2.0 < spk_d, "spk_mag_d must sit inside the driver");
// the driver's cone clears the button's nut and the cable well's plug: zones
assert(!opt_spk || spk_cy - spk_d/2 >= btn_cy + btn_nut_ac/2 + 1.0 - 1e-9, "the speaker reaches into the button zone");
assert(!opt_spk || spk_cy + spk_d/2 <= well_cy - zone_well/2 + 0.5 + 1e-9, "the speaker reaches into the cable well");
// THE ANTENNA: the landing's ribs, the posts' stand-off, the room behind them
assert(!opt_ant || ant_post_gap >= ant_t + 0.5, "ant_post_gap must leave 0.5 of air over the FPC behind the mid posts");
assert(!opt_ant || ant_l + 2*ant_rib_w + 0.6 <= post_y_top - post_y_bot - pd, "the antenna landing runs into the corner posts");
assert(!opt_ant || ant_w + 2*floor_cove <= cav_d + 1e-9, "the antenna does not fit the cavity's depth between the coves");
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
    hw_item(4, str("M2 pan x ", cam_scr_len, " self-tap (OV5647 to the face posts)")),
    vm_screws ? hw_item(len(vm_post_xy), str("M2 pan x ", vm_scr_len, " self-tap (module to its board posts)")) : "",
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
    hw_item(1, str("Ø", cam_disc_d, " x ", cam_disc_t, " clear disc, seated from inside behind the lip (a bead of neutral-cure silicone on the ledge seals it)")),
    hw_item(1, "lens retainer ring (print part=\"retainer\"; presses into the boss behind the disc)"),
    e_tamper     ? hw_item(1, str("Ø", mag_d, " x ", mag_h, " disc magnet (press + glue)")) : "",
    opt_batt     ? hw_item(1, str("LiPo 802030-class, ", batt_t, " x ", batt_w, " x ", batt_l, " (+", batt_pcm, " PCM), protected, JST-PH 2.0 to the XIAO's BAT pads — NOT below 0 °C")) : "",
    opt_batt     ? hw_item(1, "foam tape strip 0.5 x 6 x 30 (under the cell, on the plate)") : "",
    opt_spk      ? hw_item(1, str("Ø", spk_d, " x ", spk_h, " sealed full-range driver, 4 Ω, 3 W class (behind the face's grille)")) : "",
    opt_spk      ? hw_item(1, "class-D amplifier 2.5 W (PAM8302A class) + its RC input filter (parks on edge beside the button)") : "",
    opt_spk      ? hw_item(1, str("foam gasket ring Ø", spk_d, " / Ø", spk_cone_d, " x 1.5 (driver rim to the face)")) : "",
    opt_spk      ? hw_item(1, str("foam pad Ø", spk_mag_d, " x 1.5 (magnet to the cradle)")) : "",
    opt_spk      ? hw_item(1, str("acoustic mesh patch Ø", spk_mesh_d, " (hydrophobic, on the face's inner side over the grille — it guards the driver's chamber, not the cavity)")) : "",
    opt_svc      ? hw_item(2, "TPU service plunger (ONE print of part=\"plunger\": the R + B pair on a snap-off sprue; one dimple = R, two = B — from the back face over the XIAO's switches)") : "",
    opt_ant      ? hw_item(1, str("FPC Wi-Fi antenna ", ant_l, " x ", ant_w, ", u.FL pigtail (the XIAO's own kit antenna), on the +X wall between its ribs")) : "",
    hw_item(4, "#8 pan wall screw (plate)"),
]);
echo(str("Canary Vision DOORBELL v0.9 — shell ", out_x, " x ", out_y, " x ", shell_d + lid_t,
         " mm (plate ", plate_x, " x ", plate_y, " x ", plate_t, " in the bore; ", len(post_xy()),
         " screws M2 x ", pl_L, "; module at x ", vm_cx, ") + wall plate ", wplate_t, " mm (wedge ", plate_wedge,
         " deg, seal=", e_seal, ", battery=", opt_batt, ", antenna=", opt_ant, ", speaker=", opt_spk, ", vent=", e_vent, ", service=", opt_svc, ")"));

// ----------------------------------------------------------------------------
//  Helpers — the idiom once shared by copy with the other Canary enclosures
//  now comes from the canary_*_lib set; what stays local is doorbell-specific
//  (the rim ring, the wedge plate, the lens retainer, the bay fence and the
//  board post)
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
// a BOARD POST under one of the module's mounting holes: Ø vm_post_d from the
// plate to the seat, an M2 pilot down from its top for the screw through the
// module, a 45° root fillet. Where the XIAO's inner end passes it (the band
// under the socket) the post is relieved to a flat xiao_gap off the XIAO —
// the pilot ends above that band (vm_scr_len is chosen so), so the screw
// never sees the thinned side.
module board_post(p) {
    x = p[0];  y = p[1];  h = vm_standoff;
    difference() {
        translate([x, y, floor_t - 0.01]) union() {
            cylinder(d = vm_post_d, h = h + 0.01);
            cylinder(d1 = vm_post_d + 1.2, d2 = vm_post_d, h = 0.6 + 0.01);           // root fillet
        }
        translate([x, y, floor_t + h - (vm_scr_len - pcb_t + 1.0)]) cylinder(d = scr_d, h = vm_scr_len + 1);   // the pilot, 1.0 past the tip
        if (post_flat_y > y - vm_post_d/2)                                                   // the XIAO relief
            translate([x - vm_post_d, post_flat_y - vm_post_d, xiao_lo_z]) cube([2*vm_post_d, vm_post_d, xiao_hi_z - xiao_lo_z]);
    }
}
// the BATTERY BAY's fence: a wall fence_h tall along the cell's module side,
// running post to post. The -X mid posts (on the shell, fused to its wall)
// are the cell's end stops — 4 mm of each post's face across the cell's
// thickness — so the bay is a tray: wall, posts, fence; the foam strip under
// the cell and the face over it. Root-chamfered on its module side only: a
// 0.8 blade meets the plate on a 1.6 foot, and the cell side stays a plain
// face so the foot never eats the cell's clearance.
module batt_bay() {
    y0 = bay_ys[0] + pd/2 + 0.2;  y1 = bay_ys[1] - pd/2 - 0.2;   // 0.2 short of each post's face
    translate([fence_x, y0, floor_t - 0.01]) hull() {
        cube([fence_t, y1 - y0, fence_h + 0.01]);
        cube([fence_t + 0.8, y1 - y0, 0.01]);
    }
}
// the SPEAKER CRADLE on the plate: a tube under the driver's magnet, footed,
// that presses the magnet (through its foam pad) into the boss on the face
// when the plate screws down — the driver is captured, never screwed
module spk_cradle() {
    translate([0, spk_cy, floor_t - 0.01]) difference() {
        union() {
            cylinder(d = cradle_od, h = cradle_h + 0.01);
            cylinder(d1 = cradle_od + 2.0, d2 = cradle_od, h = 1.0 + 0.01);   // root flare
        }
        translate([0, 0, 1.0]) cylinder(d = cradle_id, h = cradle_h + 1);
    }
}
// the GRILLE: Ø grille_hole_d holes over the cone, inside spk_cone_d, and
// the mesh patch's seat on the INNER face (face frame, subtract).
//   sunflower (v0.9): a Fermat spiral — hole i at r = s*sqrt(i), turned i
//   golden angles (137.508°) — the pattern a sunflower's seeds take, because
//   it is the one that packs a disc evenly with no ring and no row: every
//   hole's neighbors sit at the same distance whatever its radius. The scale
//   s = pitch/sqrt(pi) gives one hole per pitch² of face, the rings' open
//   area exactly; the count is what fits inside the cone.
//   rings (v0.8): rings at the pitch, each ring's count from its circumference.
grille_r_max = spk_cone_d/2 - grille_hole_d/2 - 0.2;
grille_s     = grille_pitch / sqrt(PI);
grille_n     = floor(pow(grille_r_max / grille_s, 2));
module _grille_hole(x, y) { translate([x, y, -1]) cylinder(d = grille_hole_d, h = lid_t + 2, $fn = 12); }
module spk_grille_cut() {
    translate([0, spk_cy, 0]) {
        if (grille_pattern == "sunflower")
            for (i = [1 : grille_n]) let (r = grille_s*sqrt(i), a = i*137.50776)
                _grille_hole(r*cos(a), r*sin(a));
        else
            for (r = [grille_pitch : grille_pitch : grille_r_max])
                let (n = max(6, floor(2*3.14159*r / grille_pitch)))
                for (i = [0 : n - 1]) rotate([0, 0, i*360/n + (r/grille_pitch)*17])
                    _grille_hole(r, 0);
        _grille_hole(0, 0);                                                                  // the center
        translate([0, 0, -1]) cylinder(d = spk_mesh_d, h = spk_mesh_t + 1);                  // the mesh seat, inner side
    }
}
// a SERVICE PLUNGER's guide boss on the plate's inner face (plate frame), and
// the cut it needs: the stem's bore through plate and boss, the head's
// counterbore on the back face
module svc_boss(q) {
    translate([q[0], q[1], floor_t - 0.01]) union() {
        cylinder(d = svc_boss_od, h = svc_boss_h + 0.01);
        cylinder(d1 = svc_boss_od + 1.2, d2 = svc_boss_od, h = 0.6 + 0.01);    // root fillet
    }
}
module svc_cut(q) {
    translate([q[0], q[1], 0]) {
        translate([0, 0, floor_t - plate_t - 1]) cylinder(d = svc_bore_d, h = plate_t + svc_boss_h + 2);
        translate([0, 0, floor_t - plate_t - 0.1]) cylinder(d = svc_cb_d, h = svc_cb_depth + 0.1);
    }
}
// the PLUNGER itself (part="plunger", TPU) — drawn head-down as it prints:
// the flat head on the bed (its dimples are holes in the first layers), the
// stem up, the bead a 45° lead that squeezes through the bore and a 0.4 step
// that stays behind the boss, the tip on the end. The part is the PAIR: R
// and B side by side with a sprue between their heads (one bar, the web's
// thickness, snapped off after the print), so one export is both buttons
// and never two of one — the v0.9 review caught the single export
svc_sprue_gap = 1.0;   // the heads' edges apart; the sprue bridges it
module plunger_pair() {
    dx = svc_head_d + svc_sprue_gap;
    union() {
        plunger(xiao_rst_side);
        translate([dx, 0, 0]) plunger(-xiao_rst_side);
        translate([svc_head_d/2 - 0.2, -0.8, 0]) cube([svc_sprue_gap + 0.4, 1.6, svc_web_t]);   // the sprue
    }
}
module plunger(side = xiao_rst_side) {
    pips = (side == xiao_rst_side) ? [[0, 0]] : [[-1.5, 0], [1.5, 0]];
    difference() {
        union() {
            cylinder(d = svc_head_d, h = svc_web_t);
            translate([0, 0, svc_web_t - 0.01]) cylinder(d = svc_stem_d, h = svc_stem_len + 0.02);
            translate([0, 0, svc_bead_z]) cylinder(d1 = svc_stem_d, d2 = svc_stem_d + 2*svc_bead, h = svc_bead);
            translate([0, 0, svc_bead_z + svc_bead - 0.01]) cylinder(d = svc_stem_d + 2*svc_bead, h = 0.4);
            translate([0, 0, svc_web_t + svc_stem_len - 0.01]) cylinder(d = svc_tip_d, h = svc_tip_h + 0.01);
        }
        for (q = pips) translate([q[0], q[1], -0.1]) cylinder(d = svc_pip_d, h = svc_pip_depth + 0.1, $fn = 16);
    }
}
// the LENS POCKET (face frame: z = 0 the inner side) — cut from the boss's end
// up to the lip's back; the disc slides in, the retainer presses in after it
module cam_pocket_cut() {
    translate([lens_x, lens_y, -cam_boss_h - 0.1]) cylinder(d = cam_pock_d, h = cam_boss_h + 0.1 + (lid_t - cam_lip_t));
}
// the RETAINER (part="retainer"): the ring that holds the disc against the
// lip. Printed as drawn, bore down: the barrel's seat is a plain bore
// cam_barrel_in deep, then the bore opens at the field's half-angle so the
// ring's own edge never crops the lens
module retainer() {
    difference() {
        cylinder(d = cam_ret_od, h = cam_ret_t);
        translate([0, 0, -0.1]) cylinder(d = cam_ap_d, h = cam_barrel_in + 0.1);
        translate([0, 0, cam_barrel_in - 0.01])
            cylinder(d1 = cam_ap_d, d2 = cam_cone_d(z_disc0) + 2*0.2, h = cam_ret_t - cam_barrel_in + 0.02);
    }
}

// ----------------------------------------------------------------------------
//  The PLATE (part="body") — the chassis: the floor with its pocket slab, the
//  module rails, clips and pins, the cable exit, the lug pockets and the
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
                    // the rail steps aside for the upper clip's root
                    translate([vm_cx + s*(vm_w/2 - 1.5), clip_y_hi, floor_t + vm_standoff/2])
                        cube([5, clip_w + 2, vm_standoff + 1], center = true);
                }
                edgeclip(vm_cx + s*vm_w/2, clip_y_hi, s > 0 ? 0 : 180, vm_standoff);
                edgeclip(vm_cx + s*vm_w/2, clip_y_lo, s > 0 ? 0 : 180, vm_standoff);
                corner_shoe(s);
                top_guide(s);
            }
            // the board posts under the module's mounting holes (unioned after
            // the rails' clip notch, so the notch never nicks a post)
            for (p = vm_post_xy) board_post(p);
            if (opt_batt) batt_bay();
            if (opt_spk) spk_cradle();
            for (q = svc_xy()) svc_boss(q);
        }
        // the service plungers' bores and counterbores
        for (q = svc_xy()) svc_cut(q);
        // oval cable exit through the plate (aligns with the wall plate's slot)
        translate([exit_cx, exit_cy, 0]) hull()
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
                }
                // the retainer boss on the face's inner side, round the disc pocket
                translate([0, 0, base_d]) translate([lens_x, lens_y, -cam_boss_h]) cylinder(d = cam_boss_od, h = cam_boss_h + 0.1);
                // the speaker boss on the face's inner side: the driver's rim drops
                // into it over its foam gasket (clipped to the cavity's outline:
                // at this width the ring meets the walls)
                if (opt_spk) intersection() {
                    translate([0, 0, base_d]) translate([0, spk_cy, -spk_boss_h]) difference() {
                        cylinder(d = spk_boss_od, h = spk_boss_h + 0.1);
                        translate([0, 0, -0.1]) cylinder(d = spk_boss_id, h = spk_boss_h + 0.3);
                    }
                    translate([0, 0, floor_t]) rrect(inner_x + 1.0, inner_y + 1.0, cav_r + 0.5, cav_d + 1);
                }
                // the antenna landing's two locating ribs on the +X wall, proud
                // of it by ant_rib_d, bracketing the FPC's ends; they stand
                // between the coves, so neither the ledge nor the face is touched
                if (opt_ant) for (sy = [1, -1])
                    translate([inner_x/2 - ant_rib_d, vm_cy + sy*(ant_l/2 + 0.3) - (sy > 0 ? 0 : ant_rib_w), floor_t + floor_cove])
                        cube([ant_rib_d + 0.5, ant_rib_w, cav_d - 2*floor_cove]);
            }
            translate([0, 0, base_d]) {
                // THE LENS OPENING: the lip's aperture is the field itself (+0.2
                // a side) through the cam_lip_t skin, a 45° lead at the skin, and
                // the disc/retainer pocket behind the lip, through the boss. No
                // counterbore faces the bed: printed face-down the lip prints
                // first and the pocket is a wider hole in the layers after it
                translate([lens_x, lens_y, lid_t - cam_lip_t - 0.01])
                    cylinder(d1 = cam_lip_in, d2 = cam_lip_out, h = cam_lip_t + 0.02);
                translate([lens_x, lens_y, lid_t - 0.4])
                    cylinder(d1 = cam_lip_out, d2 = cam_lip_out + 0.8 + 0.02, h = 0.41);
                cam_pocket_cut();
                if (opt_spk) spk_grille_cut();
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
                if (e_led) core_lightpipe_bore(lp_dx, vm_cy + lp_dy, lid_t, lp_d, tol_press);
                if (e_vent) core_vent_cluster(vent_dx, vm_cy + vent_dy, lid_t,
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
                // a +X mid post with the antenna behind it stands free: a Ø pd
                // column from the face, no web to the wall (the FPC passes there)
                for (p = posts) if (!_is_free(p)) translate([0, 0, base_d]) mirror([0, 0, 1]) {
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
                        cylinder(d = cam_boss_od + 1.6, h = lid_rib_h + 0.2);
                    for (c = cam_post_xy())
                        translate([c[0], c[1], -lid_rib_h - 0.1])
                            cylinder(d = cam_post_d + 2, h = lid_rib_h + 0.2);
                    translate([0, btn_cy, -lid_rib_h - 0.1])
                        cylinder(d = max(btn_bez_d, btn_d) + 4, h = lid_rib_h + 0.2);
                    if (e_led) translate([lp_dx, vm_cy + lp_dy, -lid_rib_h - 0.1])
                        cylinder(d = lp_d + 4, h = lid_rib_h + 0.2);
                    if (e_vent) translate([vent_dx, vm_cy + vent_dy, -lid_rib_h - 0.1])
                        cylinder(d = vent_pad_d + 3, h = lid_rib_h + 0.2);
                    if (e_tamper) translate([mag_dx, vm_cy + mag_dy, -lid_rib_h - 0.1])
                        cylinder(d = mag_d + 2*tol_press + 4.8, h = lid_rib_h + 0.2);
                    // the ring steps off the -X wall over the battery (the cell stands
                    // batt_clear under the face there) and off the +X wall over the antenna
                    if (opt_batt) translate([-inner_x/2 - 1, bay_ys[0] + pd/2, -lid_rib_h - 0.1])
                        cube([1 + bay_wall_gap + batt_t + 2*batt_clear, bay_ys[1] - bay_ys[0] - pd, lid_rib_h + 0.2]);
                    if (opt_spk) translate([0, spk_cy, -lid_rib_h - 0.1]) cylinder(d = spk_boss_od + 1.6, h = lid_rib_h + 0.2);
                    if (opt_ant) translate([inner_x/2 - ant_post_gap - 0.5, vm_cy - ant_l/2 - ant_rib_w - 0.3, -lid_rib_h - 0.1])
                        cube([ant_post_gap + 1.5, ant_l + 2*ant_rib_w + 0.6, lid_rib_h + 0.2]);
                }
            }
            // camera posts on the carrier's hole grid (cam_grid_dy above its center), the
            // length that stands the lens barrel cam_barrel_in into the retainer's bore
            for (c = cam_post_xy())
                translate([c[0], c[1], -cam_post_eff])
                    difference() {
                        cylinder(d = cam_post_d, h = cam_post_eff + 0.1);
                        translate([0, 0, -0.1]) cylinder(d = cam_screw_d, h = cam_pil + 0.1);
                    }
            if (e_tamper)
                translate([mag_dx, vm_cy + mag_dy, -mag_h]) difference() {
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
assert(plate_seat_z >= head_floor - 1e-9,
       str("plate wall-screw seat leaves ", plate_seat_z, " mm under the head (< head_floor ", head_floor, ") — raise wplate_t or use a lower head"));

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
        // cable pass: a roomier match for the back plate's oval exit, run
        // mount_dt_travel() up the slide as a SLOT — the plug's cable leg is in
        // the oval when the body is offered high, and rides the whole drop
        translate([exit_cx, exit_cy, -0.1]) hull()
            for (s = [1, -1], dy = [0, mount_dt_travel()]) translate([s*(usb_exit_w - usb_exit_h)/2, dy, 0])
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
else if (part == "retainer") retainer();
else if (part == "plunger") { assert(opt_svc, "the plunger needs opt_svc=true"); plunger_pair(); }
else if (part == "none") ;        // a probe that includes this file and draws its own checks
else {
    // assembled preview wears the chosen colorway (canary_color_lib);
    // color() is preview-only — single-part exports are byte-identical
    color(cw_body(colorway)) body();
    color(cw_body(colorway)) translate([out_x + 10, 0, lid_t]) rotate([180, 0, 0]) face();
    color(cw_body(colorway)) translate([-(out_x + 14), 0, 0]) plate();
    if (e_seal) color(cw_light(colorway)) translate([0, out_y + 12, 0]) gasket();
    color(cw_body(colorway)) translate([out_x + 10, out_y/2 + 16, 0]) retainer();
    if (opt_svc) color(cw_light(colorway)) translate([out_x + 10, out_y/2 + 30, 0]) plunger_pair();
}
