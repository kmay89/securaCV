// ============================================================================
//  SecuraCV Canary WAP — 3D-printable enclosure (parametric)  v0.9
//  @env cer=2 ip="~IP54" basis="weather preset"
//  Board: Seeed XIAO ESP32-S3 Sense + optional LiPo (placed beside the board)
//  Features: light-pipe port, buzzer/pressure vent, camera/sensor window with
//            sealed-disc seat, USB-C access, board standoffs + snap clips,
//            M2 screws through a piston plate, tamper-magnet pocket, opt-in
//            weather sealing (TPU gasket in the plate ledge), opt-in wall
//            mounting (keyholes/tabs).
//
//  Units: millimeters.  CAD: OpenSCAD (https://openscad.org).
//
//  ⚠️ VERIFY BEFORE PRINTING. Defaults are nominal for the XIAO ESP32-S3 Sense
//     (PCB 21.0 x 17.5 mm) plus a 503450-class LiPo. Measure YOUR board, battery,
//     camera-lens position and USB-C connector and adjust the parameters below.
//     Print the clip coupon first, then the shell (it carries the fiddly features).
//
//  Layout (top view):   [ board @ USB end ]  [ gap ]  [ battery ]  [ gps ]
//                         -X  ............................  +X
//
//  ASSEMBLY (the piston plate — see canary_core_lib pl_*):
//
//      face  ┌──────────────────────────────┐   the SHELL (part="lid") is
//            │ ┃post                   post┃ │   one piece: face + walls +
//            │ ┃   [board]  [battery]      ┃ │   posts, printed face-down
//            │ ┃                           ┃ │
//     ledge  │═╧═══════════════════════════╧═│   posts stop 0.2 above the ledge
//            │ ░░░░░░░░ PLATE (base) ░░░░░░░ │   the PLATE (part="base") nests
//      back  └──────────────────────────────┘   inside the walls, seats on the
//                ↑ M2 screws, heads in the back   ledge (gasket there in seal
//                                                 mode), screws into the posts
//
//  Render a part:  set `part` then F6 (or use the CLI in the README).
//
//  2026-08-23: adopted the shared contract libraries (core/mount/snap/port/
//              board/mark) — the local helper copies they replace drew the
//              same geometry, so every committed mesh is unchanged; new
//              opt_mark knob debosses the house wordmark (default off).
//  2026-09-03: assembly review. Selectable fastener (`screw_size` m2/m2.5/m3
//              + `screw_head` flat/pan, from the core lib's screw registry);
//              `opt_weep` drain at the hung-low USB wall (ON in the weather
//              preset); `usb_hood` drip awning for sideways/desk use;
//              `batt_hold` ribs that stop the cell bouncing in a drop; heat-set
//              bores cut 0.3 under the knurl instead of 0.1. FIT FIXES (v0.8):
//              standoff 3.0 -> 3.5 so the clip beam keeps the MEASURED 17.8
//              board under the strain budget; 1.5 mm of wall over the USB
//              opening in every mode; posts overlap the wall by 0.2; the tamper
//              pocket counts the Sense stack under it.
//  2026-09-24: THE PARTING LINE MOVED TO THE BACK (v0.9, the piston plate —
//              canary_core_lib pl_*, the Sense is the reference port). The
//              base/lid pair with a lip, a side seam, screws from the back
//              into lid bosses OR from the face (`screw_from`) is gone: the
//              shell is ONE piece (face + walls + posts hanging from the face,
//              printed face-down) and the back is a PLATE that slides into a
//              bore inside the walls and seats on a ledge they carry — the
//              walls carry the lateral load, the only seam is a hairline on
//              the back face. The plate is the chassis (standoffs, clips,
//              battery ribs + NEW side rails, GPS cradle, keyholes, knockouts,
//              the poka-yoke key SLOT); the key RIB is on the bore wall.
//              Screws are short (M2 x 10 flat / x 8 pan), through the plate
//              into blind pilots in the post ends; a sealed build glands an
//              O-ring under every pan head (e_gland = e_seal). The gasket
//              groove moved from the rim top into the LEDGE. Removed knobs:
//              lip_h, lip_t, skirt_h, skirt_t (no drip skirt — no side seam
//              to shed off), screw_from, head_seal. The sun shield keeps its
//              own screws into blind pilots from the FACE over the corner
//              posts, and the two pilots in a post are asserted never to meet.
//              CAMERA FROM SEEED'S MODEL: the OV2640 lens top is 12.7 above
//              the PCB (not 6.0) and 6.95 along the board toward the antenna
//              end (not 0) — canary_board_lib brd_xiao_sense_cam_*; the
//              window and disc seat center on it and stack_camera (manifest-
//              owned) puts the lens top 0.5 behind the disc (asserted, both
//              ways). The light pipe, vent and magnet moved off the lens.
//              CLAMP SPANS: the sealed build's 95.9 mm gasket span gets two
//              mid posts per long wall (every span <= 40, DESIGN_RULES §6),
//              the cavity grows to keep them off the cell, and the cell is
//              cradled by plate rails now (the walls are another part). Every
//              committed WAP mesh moves.
// ============================================================================

use <canary_core_lib.scad>    // rrect/rrect2d, soft-edge face, foot chamfer, the piston plate (pl_*)
use <canary_mount_lib.scad>   // the stud/keyhole hanging interface (this file cut the pattern)
use <canary_snap_lib.scad>    // the cantilever board clip + its strain budget
use <canary_port_lib.scad>    // bridge-safe USB opening (this file's polygon, promoted)
use <canary_board_lib.scad>   // board registry — the XIAO numbers the knobs cite (+ the Sense camera)
use <canary_mark_lib.scad>    // the house wordmark (opt_mark)
use <canary_rib_lib.scad>    // corner_gusset — the constant-width post web
use <canary_color_lib.scad>   // the colorway registry — assembled-preview spools

/* [What to render] */
part   = "all";       // ["base","lid","all","coupon","gasket","shield","tray"]   (base = the PLATE, the chassis; lid = the SHELL, face + walls + posts; coupon = clip-fit test; gasket = TPU seal ring; shield = solar radiation shield; tray = desiccant tray)

/* [Preset] — quick configs; choose "custom" to use the Peripherals checkboxes */
preset = "custom";    // ["custom","battery_full","compact_plain","battery_weather"]

/* [Peripherals you have — tick what is fitted (applied when preset = custom)] */
opt_camera  = true;   // XIAO *Sense* camera   -> face sensor window + taller cavity (off = plain XIAO ESP32-S3)
opt_buzzer  = true;   // piezo buzzer          -> face vent + GORE seat
opt_led     = true;   // external status LED   -> light-pipe port
opt_battery = true;   // LiPo                  -> battery bay (enlarges the case)
opt_gps     = false;  // L76K GPS module       -> internal module bay
opt_tamper  = true;   // reed/Hall + magnet    -> face magnet pocket
opt_touch   = false;  // cap-touch pad         -> thinned "touch window" on the face
opt_antenna = false;  // external u.FL antenna -> side bulkhead hole

/* [Weather sealing — opt-in; the default case stays simple/indoor] */
opt_seal    = false;  // perimeter TPU gasket in the plate ledge + O-rings under the plate screws + USB plug recess (splash-resistant, NOT immersion)
gasket_w      = 1.6;  // gasket groove width (the printed gasket is 0.5 narrower)
gasket_groove = 1.2;  // groove depth into the ledge
gasket_proud  = 0.3;  // uncompressed gasket stand-proud (~20 % squeeze under the plate screws;
                      // TPU is incompressible — groove fill is ~86 %, leaving room to flow)
usb_cover     = true; // (seal mode) shallow recess framing the USB port for a flanged silicone plug
usb_cov_pad   = 2.0;  // recess margin around the USB opening
usb_cov_dep   = 1.0;  // recess depth into the outer wall face
opt_weep      = false; // Ø2 weep through the USB wall (hung USB-down it is the low wall): condensate leaves, driven rain does not enter. The weather preset turns it on
opt_shield  = false;  // blind pilots in the face over its corner posts for the sun shield's four screws (ON in the Outdoor preset; the shield is part="shield")
usb_hood      = false; // drip awning over the USB opening — for a case standing sideways or on a desk
                       // (hung USB-down the port faces the ground and needs none)
hood_reach    = 4.0;   // how far the awning's drip edge stands off the wall  // [2:0.5:8]

/* [Mounting — opt-in; case hangs with the USB end facing DOWN] */
opt_mount   = false;  // wall-mount features (keyholes thicken the plate by `kh_extra`)
mount_style = "keyhole"; // ["keyhole","tabs","both"]
// external screw tabs — four ears on the ±Y walls, fully outside the seal envelope
tab_l       = 10.0;   // ear length along the wall
tab_w       = 8.0;    // ear protrusion from the wall
tab_t       = 3.0;    // ear thickness
tab_hole_d  = 3.6;    // through-hole (M3 / #6)
tab_cb_d    = 7.0;    // pan-head counterbore diameter
tab_cb_h    = 1.0;    // counterbore depth

/* [Stud/keyhole interface] — blind keyhole pockets in a thickened plate (seal-safe) */
// keyholes — BLIND pockets in a thickened plate; they never breach the cavity (seal-safe).
// Defaults are the catalog's stud/keyhole standard (canary_mount_lib) — deviations
// earn their keep on the fit coupon, not in a quiet knob edit
kh_extra    = 3.0;    // plate thickening that hosts the keyhole pockets
kh_head_d   = 7.0;    // screw-head pass hole (fits #6 / M3.5 pan head) — mount_kh_head_d()
kh_shank_d  = 4.2;    // shank slot width — mount_kh_shank_d()
kh_slot_l   = 8.0;    // slot travel (slot runs toward +X = UP when the USB faces down) — mount_kh_slot_l()
kh_head_h   = 3.5;    // total pocket depth (face web + head cavity) — mount_kh_head_h()
kh_face     = 1.0;    // face web thickness the screw head grips behind — mount_kh_face()
kh_inset    = 10.0;   // keyhole centers at x = ±(inner_l/2 − kh_inset); auto-merges to one on small cases

/* [Aesthetics] */
colorway    = "graphite"; // ["graphite","canary","snow","forest","midnight"] assembled-preview spool set (canary_color_lib; single-part exports carry no color)
lid_edge    = 0.8;    // 45° chamfer around the face's top edge (0 = sharp slab)  // [0:0.1:1.5]
lid_edge2   = 0.8;    // second (~66°) stage of the show-face edge, mm — ON is the house look (core_face_edge2()); it is what reads as a roundover instead of a bevel. 0 leaves the plain 45° facet any CAD default gives you  // [0:0.1:1.5]
// The wordmark sits where label_text would (label_dx/dy/rot/size/depth place
// and size it) and is gated by the mark library's measured type metrics, so a
// size that would print as a smudge or run off the face is refused before a
// print, not after
opt_mark    = false;  // deboss the house wordmark instead of a custom label (exclusive with label_text)
label_text  = "";     // debossed face label, e.g. "CANARY" ("" = off; needs the font installed)
label_size  = 5.0;    // text height
label_depth = 0.5;    // deboss depth (prints as crisp first-layer voids, the shell prints face-down)
label_dx    = 0.0;    // label center offset from the FACE center (not the board center)
label_dy    = -10.0;  // label center Y offset from the FACE center
label_rot   = 0;      // label rotation (degrees)
label_font  = "Liberation Sans:style=Bold";  // the font label_text is set in (it must be installed)

// effective flags (a preset overrides the checkboxes above)
function _pre(c, f, p, w) = (preset == "battery_full")    ? f
                          : (preset == "compact_plain")   ? p
                          : (preset == "battery_weather") ? w : c;
e_camera  = _pre(opt_camera,  true,  false, true);
e_buzzer  = _pre(opt_buzzer,  true,  true,  true);
e_led     = _pre(opt_led,     true,  true,  true);
e_battery = _pre(opt_battery, true,  false, true);
e_gps     = _pre(opt_gps,     true,  false, true);
e_tamper  = _pre(opt_tamper,  true,  false, true);
e_touch   = _pre(opt_touch,   false, false, false);
e_antenna = _pre(opt_antenna, false, false, false);
e_seal    = _pre(opt_seal,    false, false, true);
e_mount   = _pre(opt_mount,   false, false, true);
e_weep    = _pre(opt_weep,    false, false, true);

/* [Board] — Seeed XIAO ESP32-S3 official: PCB 21.0 x 17.5 mm, 2.54 mm pitch */
board_l        = 21.0;  // PCB length (USB end to far end, along X) — brd_l("xiao") nominal spec, canary_board_lib
board_w        = 17.5;  // PCB width (along Y) — brd_w("xiao") nominal spec; a real board mics
                        // brd_xiao_w_measured() = 17.8 (canary_dock lesson) — here the board sits
                        // in CLIPS, so clip_clear absorbs the difference and the spec default stays;
                        // the clip strain gate is told about the extra 0.15 per side (see clip_over)
board_h        = 1.2;   // PCB thickness
board_clear    = 0.6;   // per-side clearance around the PCB
stack_camera   = 11.4;   // cavity depth over the PCB top with the Sense camera — LESS than the lens height brd_xiao_sense_cam_h() on purpose: the lens barrel stands in the face's window with its top 0.2-0.5 behind the disc (asserted both ways; the manifest owns this number)
stack_plain    = 4.5;   // headroom above the PCB for a plain XIAO ESP32-S3
board_stack_h  = e_camera ? stack_camera : stack_plain;

/* [Battery] (LiPo, placed beside the board) */
batt_l         = 50.0;  // 503450 ~ 50 x 34 x 5 mm
batt_w         = 34.0;  // bay width across the case (Y) — the cell's 34; the plate's side rails sit 0.5 off it
batt_h         = 6.0;   // bay height — keep >= 1 mm over the nominal cell for LiPo swelling  // [4:0.5:12]
batt_gap       = 2.5;   // gap between board zone and battery zone
batt_wire_w    = 4.0;   // lead channel notched into the bay's end ribs at the +Y rail (0 = off)
batt_hold      = true;  // face-side ribs over the bay that stop the cell bouncing in a drop: they
                        // rest at batt_h over the plate, so the swelling allowance built into
                        // batt_h stays (the cell used to sit under ~8 mm of free air)

/* [GPS module] (L76K, internal bay after the board/battery) */
gps_l          = 16.0;  // GPS module length (X): the bay's cradle rim is drawn 0.4 a side around it
gps_w          = 16.0;  // GPS module width (Y); with the GPS fitted the cavity is at least this + 1
gps_h          = 4.0;   // GPS module height — the cavity keeps 1 mm over it
gps_gap        = 2.3;   // rim fuses into the battery rib at 2.3 (a larger gap leaves a slot the slicer can't print)

/* [Antenna] external u.FL/SMA bulkhead hole on the far (+X) wall */
ant_d          = 6.5;   // SMA bulkhead thread is 6.35; the bore is a teardrop so its crown prints without sag
ant_z          = 4.0;   // bore center above the PCB underside (clears the corner posts' gussets)

/* [Shell] */
wall_t         = 2.0;   // side wall thickness (auto-thickened to keep a structural skin outside the plate's bore, and in seal mode to host the gasket ledge)
floor_t        = 2.0;   // plate floor thickness (the plate's front face sits this far above the back face)
lid_t          = 2.0;   // face thickness
corner_r       = 3.0;   // outside corner radius
floor_cove = 0.8;  // 45° cove where the ledge meets the walls, inside (canary_core_lib pl_cavity_cut, at the face too); 0 = the old square corner  // [0:0.2:1.2]
                   // The sharp notch there was the crack-starter in every flat-printed shell — a corner drop
                   // hinges the floor about it along one layer boundary.
lid_key    = true; // poka-yoke: a rib on the +Y bore wall and a slot in the plate's edge — the posts fit
                   // a plate two ways and every feature lines up one way; turned round it stands proud

/* [Print tolerances] — per-side clearances; tune these once for your printer
   (defaults = the catalog trio, core_tol_*() in canary_core_lib, dialed on the fit coupon) */
tol_slide      = 0.20;  // sliding fits: plate <-> bore, camera-disc seat — core_tol_slide()
tol_press      = 0.10;  // press fits: tamper magnet, LED light pipe — core_tol_press()
tol_hole       = 0.30;  // clearance holes: plate screws — core_tol_hole()

/* [Engineering — durability/rigidity options (see README "Engineering & materials")] */
screw_insert = false;   // M2 brass heat-set inserts in the post ends (service-grade threads;
                        // posts auto-fatten, M2 machine screws replace the self-tappers)
insert_d     = 3.5;     // (m2) insert knurl OD (M2 short series: 3.5 x 4.0) — the bore is cut 0.3 under
                        // it so the brass bites (a 0.1 interference spun under driver torque)
insert_h     = 4.0;     // (m2) insert length; other sizes read the registry
lid_ribs     = true;    // perimeter rib ring under the face — stiffens the flat face (t³) against pry/flex
lid_rib_w    = 2.5;     // rib ring width
lid_rib_h    = 1.0;     // rib depth below the face underside — keep <= the 1.0 mm component
                        // headroom built into cav_h, or raise stack_camera/stack_plain to suit
foot_cham    = 0.5;     // 45° chamfer on the back edge: elephant-foot + first-layer delamination guard (0 = off)
kh_lock      = true;    // (keyhole mounts) two anti-lift knockout bosses: 0.6 mm web, pierce with
                        // #4/M3 screws on install so the case can't be lifted off the wall screws

/* [Thermal / outdoor kit] — part="shield" is a Stevenson-screen style solar
   radiation shield: a second roof standing sh_gap above the face on hollow
   standoffs, fastened by four short flat-head screws of its own into blind
   pilots over the face's corner posts (opt_shield; the plate screws stay in
   the back, under their O-rings, and the two pilots in a post never meet).
   It shades the case and vents the gap; apertures open automatically over
   the camera / light pipe / touch window. part="tray" is a slotted clip-in
   desiccant tray for a 1 g silica pack (VHB or friction fit). */
sh_gap   = 6.0;    // shield air gap above the face
sh_over  = 6.0;    // shield overhang beyond the case walls (shade + rain shadow)
sh_t     = 2.4;    // shield panel thickness (2.4, not 1.6: its flat-head cones are 1.2 deep and keep a 1.0 floor)
tray_l   = 24.0;   // desiccant tray footprint length
tray_w   = 18.0;   // desiccant tray footprint width
tray_h   = 8.0;    // desiccant tray height (1.2 floor, slotted)

/* [Standoffs / screw posts] */
standoff_h     = 3.5;   // PCB sits this high off the plate (clearance for bottom parts). 3.5, not
                        // 3.0: the clip beam is standoff + PCB, and at 4.7 mm the MEASURED 17.8
                        // board inserts at 4.4 % strain; at 4.2 it was 5.5 % against a 4.5 % budget
standoff_d     = 4.0;   // PCB standoff Ø — one under each board corner
post_d         = 5.0;   // screw posts hanging from the face (the plate screws thread into their ends; auto-fattened for larger screws)
screw_size     = "m2";  // ["m2","m2.5","m3"] plate screw — the catalog screw registry (canary_core_lib) sets
                        // pilot, clearance, head seat, insert bore and post floor; "m2" keeps the three
                        // print-validated numbers below exactly as they are
screw_head     = "flat"; // ["flat","pan"] the head in the bag: flat = 90° countersink in the plate's back,
                        // pan = flat-floored counterbore (what a sealed build's O-ring gland needs — the weather preset forces it)
screw_d        = 1.6;   // (m2) self-tapping pilot — 1.6 mm so threads bite (2.0 = no grip)
screw_head_d   = 4.0;   // (m2 flat) the flat head's Ø — the registry's M2 row, which the plate seat draws
screw_head_h   = 1.2;   // (m2 flat) countersink depth — a true 90° seat for an M2 flat head
// the Outdoor build seals every screw seat with an O-ring under a PAN head
// (a flat head's cone ejects the ring), and its face carries the blind pilots
// the sun shield's own screws thread into (opt_shield)
e_head    = _pre(screw_head,  screw_head, screw_head, "pan");
e_shield  = _pre(opt_shield,  false, false, true);
// a sealed build seats an O-ring under every plate screw head: the seat is a
// hole through the seal line from outside (canary_core_lib pl_seat_cut)
e_gland   = e_seal;

/* [USB-C port] — on the board's USB end (-X short wall) */
usb_w          = 12.0;  // opening width: clears rugged USB-C cable boots (connector body ~8.9 mm)  // [9:0.5:14]
usb_h          = 6.5;   // opening height: boot clearance (connector body ~3.2 mm) — slim if cable is bare  // [4:0.5:8]
usb_web        = 1.5;   // wall guaranteed between the opening and the face underside in every mode
usb_z          = -1.65; // centers the opening on the connector AXIS: the C shell is 3.2 mm tall on the
                        // PCB, so the axis sits at PCB-top + 1.6; a boot needs equal room below the axis

/* [Face features] — offsets are measured FROM THE BOARD CENTER (mm). Measure your board! */
// Camera / sensor window + recessed seat for a glued clear disc (12 x 1 mm PMMA/PC)
cam_win_d      = 10.0;  // asserted against cam_fov at the disc's inner plane so a corner never vignettes, and >= the Ø8 lens barrel + 1.0 (the barrel stands in it); 10 leaves 1.0 a side for the board's clip float and the 0.64 the vendor model may put the module off the width centerline (MEASURE) — the disc's bond ring stays 1.2
cam_fov        = 66;    // lens diagonal field of view (OV2640 on the Sense: 66°)  // [40:1:120]
cam_lens_h     = 12.7;   // lens top above the PCB top — brd_xiao_sense_cam_h() (Seeed's XIAO ESP32-S3 Sense model), canary_board_lib
cam_disc_d     = 12.0;  // clear-disc diameter (seat = disc + 2*tol_slide; 0 = no seat, bare hole)
cam_disc_t     = 1.0;   // clear-disc thickness (disc sits 0.2 recessed below the face)
cam_dx         = 6.95;   // camera window center X, from the board center — brd_xiao_sense_cam_dx() (toward the antenna end, away from the USB), canary_board_lib
cam_dy         = 0.0;   // camera window center Y, from the board center — brd_xiao_sense_cam_dy() (on the width centerline), canary_board_lib
// Light-pipe / status-LED port (press fit: hole = lp_d + 2*tol_press)
lp_d           = 3.0;   // light-pipe diameter (3 mm pipe -> 3.2 mm hole at default tol_press)
lp_dx          = 1.0;   // light-pipe port center X, from the board center (off the lens now at +6.95: the Ø3.2 bore keeps a web to the Ø12.4 disc seat — asserted)
lp_dy          = 9.5;   // light-pipe port center Y, from the board center (9.5, past the board's edge: at 7.5 the bore broke into the moved disc seat)
// Buzzer + pressure vent (recess seats an adhesive GORE vent; ring of holes passes sound/pressure)
vent_pad_d     = 12.0;  // GORE-vent recess Ø (the buzzer + pressure vent: the recess seats an adhesive vent)
vent_pad_depth = 0.8;   // GORE-vent recess depth into the face's outer surface — core_vent_pad_depth()
vent_hole_d    = 1.0;   // fine holes — insect-resistant (the README's outdoor rule: <= 1.0 mm); the
                        // membrane behind them seals, so the holes only need to pass sound + pressure
vent_ring_d    = 6.0;   // Ø of the ring the vent holes sit on — core_vent_ring_d()
vent_holes     = 10;    // hole count around that ring — core_vent_holes()
vent_dx        = -5.0;  // buzzer vent center X, from the board center (the USB half of the board: the lens owns the other)
vent_dy        = -6.5;  // buzzer vent center Y, from the board center (the Ø12 pad keeps a web to the disc seat and the magnet ring — asserted)
// Cap-touch window — local thinning so capacitance couples through the face (when opt_touch)
touch_d        = 12.0;  // cap-touch window Ø — local face thinning so capacitance couples through (opt_touch)
touch_wall     = 0.8;   // remaining face thickness at the pad
touch_dx       = -3.0;  // cap-touch window center X, from the board center (shares the USB half with the vent: with opt_buzzer on, move one — asserted)
touch_dy       = -5.0;  // cap-touch window center Y, from the board center

/* [Tamper magnet] — blind pocket on the FACE underside, over the board's reed/Hall switch */
mag_d          = 6.0;   // MAGNET diameter (pocket = mag_d + 2*tol_press — press fit; add a drop of glue)
mag_h          = 2.2;   // pocket depth — a 6 x 2 mm disc is standard
mag_under      = 6.0;   // tallest part on the board UNDER the pocket: the Sense expansion board's top
                        // (camera excluded — keep the pocket off the lens) — MEASURE. The cavity grows
                        // to keep 1 mm between it and the pocket ring
mag_dx         = -6.0;  // magnet pocket center X, from the board center
mag_dy         = 6.0;   // magnet pocket center Y, from the board center (6, not 5: the ring keeps its web to the moved vent pad)

/* [Board snap clips] — press-fit retention so the PCB clicks in with NO screws */
board_clips    = true;  // cantilever tabs hook over the board's two long edges
clip_w         = 6.0;   // tab width (along the board edge)
clip_t         = 1.0;   // beam thickness — thinner = easier flex (tune to your material)
                        // (1.0/0.5 keeps insertion strain ~4 % — vertical-print PETG cracks near
                        // 1.5/0.8; canary_snap_lib now runs that arithmetic as an assert, so the
                        // cracking numbers refuse to render instead of relying on this comment)
clip_hook      = 0.5;   // how far the lip overhangs the board top
clip_hook_h    = 1.2;   // lip + 45° lead-in height above the board top
clip_clear     = 0.25;  // gap between tab inner face and the board edge (a fit — tune on the coupon)
clip_dx        = 5.25;  // clip centers at board_cx ± clip_dx (5.25 = board_l/4, the validated spot)  // [3:0.25:8]
                        // The XIAO's castellated pads run to ±8.5 along each long edge, so a 6 mm tab here
                        // sits over three pads: solder wires from the UNDERSIDE and keep the top pad
                        // flat, or a fillet stops the lip latching
clip_over      = 0.15;  // extra lip travel per side for the MEASURED board (brd_xiao_w_measured() 17.8
                        // vs the drawn 17.5) — fed to the strain gate, not to the drawing

/* [Quality] */
// curve quality: $fa/$fs give smooth big arcs (pill corners, hood) without
// exploding tiny holes into thousands of facets like a large $fn would
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
// the piston plate (canary_core_lib pl_*): the plate seats on a ledge inside
// the walls and the parting line is the back face itself. The ledge band is
// the gasket with a 1.2 cheek each side (the cheeks ARE the seal path's
// walls — this file once shipped 0.8 cheeks), or one contact band
ledge_w  = pl_ledge(e_seal, gasket_w);
wall_eff = max(wall_t, ledge_w + core_min_wall());   // the skin outside the plate's bore stays a structural wall
// the fastener, resolved once: the M2 rows of the registry equal this file's
// validated knobs, so a default render reads the knobs and larger sizes read
// the registry (a pilot sized for M2 under an M3 self-tapper splits the post)
scr_d   = (screw_size == "m2") ? screw_d : scr_pilot(screw_size);
scr_c   = max(scr_d + 2*tol_hole, scr_clear(screw_size));            // plate clearance hole
head_d  = (screw_size == "m2" && e_head == "flat") ? screw_head_d
        : (e_head == "flat") ? scr_flat_d(screw_size) : scr_pan_d(screw_size);
head_h  = (screw_size == "m2" && e_head == "flat") ? screw_head_h
        : (e_head == "flat") ? scr_flat_h(screw_size) : scr_pan_h(screw_size);
ins_od  = (screw_size == "m2") ? insert_d : scr_insert_d(screw_size) + 0.3;   // knurl OD
ins_h   = (screw_size == "m2") ? insert_h : scr_insert_h(screw_size);
ins_bore = ins_od - 0.3;                                              // 0.3 interference: the brass bites
pd      = max(screw_insert ? max(post_d, ins_od + 2.4) : post_d,     // >=1.2 mm wall around an insert
              scr_post_min(screw_size));                              // >=1.5 mm wall around the pilot

board_zone_l = board_l + 2*board_clear;
batt_zone_l  = e_battery ? (batt_gap + batt_l) : 0;
gps_zone_l   = e_gps     ? (gps_gap  + gps_l)  : 0;
extra_l      = batt_zone_l + gps_zone_l;                 // internal bays appended after the board
post_corner  = pd + 1.5;                 // positioning margin so a screw post sits in the corner, clear of the board
rail_t       = core_min_wall();          // the battery bay's side rails on the plate (the cell lies 0.5 off them)

// cavity: board + bays along X, +X dead zone keeps true corners for the screw posts.
// The board is ALWAYS biased to the -X (USB) wall so the connector reaches the opening
// (v0.7 fix — centering it left the USB ~6.5 mm behind the wall on the compact case).
clip_stack = board_clips ? (clip_clear + clip_t) : 0;
inner_l = board_zone_l + extra_l + post_corner + 1.0;
// the corner posts sit 0.2 INTO each wall (fused above the gussets); a sealed
// build's gasket is squeezed only between screws, so the span between them
// along the long walls stays <= 40 (DESIGN_RULES §6): n_mid evenly spaced
// mid posts per long wall, and the cavity grows so they clear the cell
post_cx = inner_l/2 - pd/2 + 0.2;
span_l  = 2*post_cx;                                     // corner to corner along X
n_mid   = e_seal ? max(0, ceil(span_l/40 - 1e-9) - 1) : 0;
inner_w = max(board_w + 2*board_clear,
              // the cell 0.5 off the plate's rails (the +1.0 below), the rails clear of the ledge cove
              e_battery ? batt_w + 2*(rail_t + floor_cove + 0.4) : 0,
              // a mid post's inner face flush with the rails: the cell lies 0.5 off it too
              (e_battery && n_mid > 0) ? batt_w + 2*(pd - 0.2) : 0,
              e_gps ? gps_w : 0,
              board_w + 2*post_corner,
              board_w + 2*(clip_stack + 0.6 + pd + 0.2)   // keep the clips clear of the corner posts
             ) + 1.0;
post_cy = inner_w/2 - pd/2 + 0.2;
cav_h_min = max(standoff_h + board_h + board_stack_h + 1.0,          // internal height above the plate
                e_tamper ? standoff_h + board_h + mag_under + 1.0 + mag_h : 0,   // pocket ring clears the stack under it
                e_battery ? batt_h + 1.0 : 0, e_gps ? gps_h + 1.0 : 0);
// the wall between the USB opening and the face: usb_web of solid in every mode
cav_h    = max(cav_h_min, standoff_h + board_h + usb_h + usb_z + usb_web);

out_l  = inner_l + 2*wall_eff;
out_w  = inner_w + 2*wall_eff;
base_h = floor_t + cav_h;                // the face's underside (this file's name for the catalog's base_d)
base_d = base_h;                         // the catalog's name for the same datum (the shared probes read it)
// ASSEMBLED-FIT PROBE for canary_case_fitcheck.scad. It lives HERE, next to
// the geometry, for two reasons: it reads this file's own derived datum
// instead of duplicating the arithmetic it is checking, and its name is
// unique — every case in this catalog calls its halves front()/back() (this
// file's base()/lid(), aliased), so a single fit-check file that `use`d them
// all would silently resolve to whichever was parsed last and check the
// wrong case.
//
// MUST RENDER EMPTY. `lift` separates intended face-on-face contact from real
// interference: coplanar faces intersect to a zero-volume patch that CGAL
// reports as non-2-manifold, which is a dirty render, not a pass.
// `turned` seats the shell rotated 180° about Z — the poka-yoke CONTROL: with
// lid_key on, this must NOT be empty (the plate's edge lands on the key rib).
// A gate that can only pass has proved nothing; this is the case it must fail.
module wap_fitcheck(lift = 0.1, turned = false) {
    intersection() { translate([0, 0, base_h + lift]) rotate([0, 0, turned ? 180 : 0]) lid(); base(); }
}

pcb_z    = floor_t + standoff_h;                // absolute z of PCB underside
board_cx = -inner_l/2 + board_clear + board_l/2 + 0.5;   // USB-biased (0.5 = positioning margin, not a fit)
board_cy = 0;
zone0    = -inner_l/2 + board_zone_l + 0.5;     // x where the appended bays begin
batt_cx  = zone0 + batt_gap + batt_l/2;         // battery bay center
batt_cy  = 0;
gps_cx   = zone0 + batt_zone_l + gps_gap + gps_l/2;   // GPS bay center (after battery, if any)

// mounting back-thickening (keyhole pockets live in the plate, never in the cavity)
mount_extra0 = (e_mount && (mount_style == "keyhole" || mount_style == "both")) ? kh_extra : 0;
// the plate (canary_core_lib pl_*): never thinner than a head and its floor;
// the head recesses as far as that floor allows; the screw is the shortest
// standard length that engages hw_engage() in the post end past the relief
plate_t  = pl_thick(floor_t, mount_extra0, screw_size, e_head, e_gland);
mount_extra = plate_t - floor_t;                   // the plate below z = 0 (the keyhole slab, or the head's floor)
pl_r     = max(0, pl_recess(plate_t, screw_size, e_head, e_gland));   // the flat case lands on 0 exactly; keep float dust out of the seat
pl_L     = pl_len(plate_t, pl_r, screw_size, e_head);
pl_eng   = pl_engage(plate_t, pl_r, screw_size, e_head);
pl_pil   = pl_pilot(plate_t, pl_r, screw_size, e_head);
post_h   = cav_h - pl_relief();                    // face underside -> the relief over the ledge plane
shell_d  = cav_h + plate_t;                        // the walls, face underside -> the back face
bore_x   = inner_l + 2*ledge_w;  bore_y = inner_w + 2*ledge_w;
bore_r   = core_cav_r(corner_r, wall_eff) + ledge_w;
plate_x  = bore_x - 2*tol_slide;  plate_y = bore_y - 2*tol_slide;  plate_r = max(bore_r - tol_slide, 0.4);
// the sun shield's screws: flat heads in the shield's top (registry seats),
// through its tubes, into blind pilots sh_pilot deep from the face — so the
// post end's own pilot (pl_pil, from below) and the shield's (from above)
// stay a full millimeter apart
sh_L      = hw_len(sh_t + sh_gap, 0, 3.0);         // 3 mm in the face: 1.5 x d for a shield that sees wind, not load
sh_pilot  = sh_L - sh_t - sh_gap;
assert(!e_gland || e_head == "pan", "a sealed build seats an O-ring under each plate screw head — that needs a pan head (the weather preset forces it; set screw_head = \"pan\")");
assert(pl_pil + 1.0 <= post_h, str("the post is too short for its pilot (", pl_pil, " into ", post_h, " mm) — raise stack_camera/stack_plain"));
assert(!screw_insert || ins_h + 1.0 <= post_h, "the post is shorter than the insert it must hold");
assert(!e_shield || post_h >= (sh_pilot - lid_t) + pl_pil + 1.0,
       str("the shield's pilot (", sh_pilot - lid_t, " into the post from the face) meets the plate screw's (", pl_pil,
           " from the end) in a ", post_h, " mm post — deepen the case or shorten sh_L"));
assert(!e_shield || sh_t - scr_flat_h(screw_size) >= 1.0 - 1e-9, "the shield plate is too thin to keep a floor under its screw heads (sh_t)");

// keyhole positions: two near the ends, or one centered when the case is too short
kh_x   = inner_l/2 - kh_inset;
kh_xs  = (kh_x >= kh_slot_l/2 + kh_head_d/2 + 2) ? [-kh_x, kh_x] : [0];

// sanity checks + a measurable size echo for scripted verification
assert(wall_t > 0 && floor_t > 0 && lid_t > 0, "shell thicknesses must be positive");
assert(standoff_h > 0, "standoff_h must be positive");
assert(screw_head_d > screw_d, "screw_head_d must be larger than screw_d");
assert(head_d > scr_c, "the screw head must be larger than its clearance hole, or it falls through the plate");
// the face rib ring may reach down only as far as the headroom over the tallest
// component (the +1.0 in cav_h_min, plus whatever the USB rule added)
lid_headroom = 1.0 + (cav_h - cav_h_min);
assert(!lid_ribs || lid_rib_h <= lid_headroom,
       str("lid_rib_h ", lid_rib_h, " reaches past the ", lid_headroom, " mm headroom over the stack — the ribs land on a component"));
assert(!lid_ribs || lid_rib_w >= core_min_wall(), "lid_rib_w is under the structural wall floor");
// the ledge cove eats 0.8 of cavity width at plate level: the GPS pocket must clear it
assert(!(e_gps && floor_cove > 0) || inner_w/2 - (gps_w + 0.8)/2 >= floor_cove + 0.3,
       "the GPS pocket runs into the ledge cove — set floor_cove = 0 or widen the cavity");
key_x = inner_l/2 - post_corner - 2.5;   // plate key: on the +Y bore wall, inboard of the +X corner post
// a sealed box with no pressure path pumps air past its gasket on every
// thermal cycle (field_ratings.md): the buzzer cluster's membrane seat or a
// weep must be in the build — nothing asserted this before
assert(!e_seal || e_buzzer || e_weep,
       "seal mode with no pressure path — enable opt_buzzer (the vent cluster + GORE seat) or opt_weep");
assert(!(usb_hood && e_seal && usb_cover),
       "usb_hood and the silicone-plug recess (usb_cover) both own the wall around the port — pick one");
assert(!batt_hold || !e_battery || cav_h - batt_h >= 1.5,
       "batt_hold ribs would be shorter than 1.5 mm — the bay is already nearly full-height");
assert(!e_seal || gasket_groove + 0.5 <= cav_h, "gasket_groove runs out of the ledge");
assert(!e_seal || core_gasket_fill(gasket_w, gasket_groove, gasket_proud) <= core_gasket_fill_max(),
       str("the printed TPU ring would fill ", round(100*core_gasket_fill(gasket_w, gasket_groove, gasket_proud)),
           " % of its groove - past ", round(100*core_gasket_fill_max()),
           " % the incompressible gasket props the plate open instead of sealing; narrow gasket_w or deepen gasket_groove"));
assert(cam_disc_t == 0 || cam_disc_t + 0.2 < lid_t, "cam_disc_t too thick for lid_t");
assert(cam_disc_t == 0 || cam_disc_d == 0 || cam_disc_d > cam_win_d,
       "cam_disc_d must be larger than cam_win_d or the disc falls through");
assert(mount_extra0 == 0 || kh_head_h + 1.5 <= floor_t + kh_extra, "keyhole pocket too deep — raise kh_extra");
assert(mount_extra0 == 0 || kh_head_h > kh_face, "kh_head_h must exceed kh_face (it includes the face web)");
assert(mount_extra0 == 0 || kh_head_d > kh_shank_d, "kh_head_d must be larger than kh_shank_d");
assert(mount_extra0 == 0 || kh_slot_l > 0, "kh_slot_l must be positive");
assert(!e_mount || mount_style == "keyhole" || (tab_cb_h >= 0 && tab_cb_h < tab_t),
       "tab_cb_h must be between 0 and tab_t");
assert(!e_mount || mount_style == "keyhole" || tab_cb_h == 0 || tab_cb_d > tab_hole_d,
       "tab_cb_d must be larger than tab_hole_d when counterbored");
assert(lid_edge == 0 || (lid_edge >= 0.01 && lid_edge < lid_t),
       "lid_edge must be 0, or between 0.01 and lid_t");
assert(lid_edge2 >= 0 && (lid_edge > 0 || lid_edge2 == 0) && lid_edge + lid_edge2 < lid_t,
       "lid_edge2 requires lid_edge > 0, and their sum must stay below lid_t");
assert((label_text == "" && !opt_mark) || (label_depth > 0 && label_depth < lid_t),
       "label_depth must be between 0 and lid_t");
assert(!(opt_mark && label_text != ""),
       "opt_mark and label_text share the label spot — set one, not both");
// the wordmark's two gates, from the mark library's measured type metrics:
// below mark_word_min_h() a 0.4 mm bead no longer reaches the letterforms and
// the deboss prints as a smudge with the rhythm of type — the render looks
// perfect either way, which is why this is an assert and not an eyeball
assert(!opt_mark || label_size >= mark_word_min_h(),
       str("opt_mark at label_size ", label_size, " mm is under the ",
           mark_word_min_h(), " mm cap height where a 0.4 mm bead still ",
           "reaches the letterforms — raise label_size"));
assert(!opt_mark || mark_word_ink_w("securaCV", label_size) <= out_l - 4.0,
       str("the wordmark draws ", mark_word_ink_w("securaCV", label_size),
           " mm at label_size ", label_size, " on a ", out_l,
           " mm face (2 mm margin per side) — shrink label_size"));
assert(batt_wire_w >= 0, "batt_wire_w must be non-negative");
assert(!e_antenna || pcb_z + ant_z + ant_d/2 + 1.5 <= base_h, "antenna bore runs into the face — lower ant_z");
assert(!e_antenna || pcb_z + ant_z - ant_d/2 >= floor_t + (e_seal ? gasket_groove : 0) + core_min_web(),
       "antenna bore breaks into the ledge band — raise ant_z");
// the camera, from the registry: the lens barrel STANDS IN the window, its
// top 0.2-0.5 behind the disc's inner face (the disc is the weather surface;
// closer than 0.2 and a print's height error puts the lens on it, further
// than 0.5 and the barrel's corners are back inside the cavity where the
// window web can vignette). stack_camera is the number that sets it and the
// manifest owns it — a wrong value fails here, both ways
cam_top    = pcb_z + board_h + cam_lens_h;                                // the lens top, absolute
cam_seat_z = base_h + lid_t - (cam_disc_t > 0 ? cam_disc_t + 0.2 : 0);   // the disc's inner face (or the outer face, bare hole)
cam_throw  = cam_seat_z - cam_top;
cam_need   = 2*cam_throw*tan(cam_fov/2) + 2.0;
assert(!e_camera || cam_throw >= 0.2 - 1e-6,
       str("the lens top is ", cam_throw, " mm from the disc — under 0.2: lower stack_camera or shorten cam_lens_h"));
assert(!e_camera || cam_throw <= 0.5 + 1e-6,
       str("the lens top is ", cam_throw, " mm behind the disc — over 0.5: raise stack_camera (the barrel is meant to stand in the window)"));
assert(!e_camera || cam_top <= base_h || cam_win_d >= brd_xiao_sense_cam_fp() + 1.0,
       str("the Ø", brd_xiao_sense_cam_fp(), " lens barrel stands ", cam_top - base_h, " mm into a Ø", cam_win_d, " window — needs Ø",
           brd_xiao_sense_cam_fp() + 1.0, " (0.5 a side) — raise cam_win_d"));
assert(!e_camera || cam_win_d >= cam_need,
       str("camera window ", cam_win_d, " mm vignettes a ", cam_fov, "° lens ", cam_throw,
           " mm behind it — needs ", round(cam_need*10)/10, " mm (raise cam_win_d or cam_fov is optimistic)"));
// the face features share one face: every pair keeps a core_min_web() web
// (at lp (5, 5) the Ø3.2 bore once cut 0.7 mm into the Ø12.4 disc seat, so a
// glued disc landed on the flush pipe's rim and could not seat)
cam_r  = e_camera ? max(cam_win_d/2, cam_disc_d > 0 ? cam_disc_d/2 + tol_slide : 0) : 0;
lp_r   = e_led    ? lp_d/2 + tol_press : 0;
vent_r = e_buzzer ? vent_pad_d/2 : 0;
mag_r  = e_tamper ? mag_d/2 + tol_press + 1.2 : 0;
tch_r  = e_touch  ? touch_d/2 : 0;
function _web_ok(ax, ay, ar, bx, by, br) = ar == 0 || br == 0 || norm([ax - bx, ay - by]) >= ar + br + core_min_web();
assert(_web_ok(cam_dx, cam_dy, cam_r, lp_dx, lp_dy, lp_r),       "the light-pipe bore breaks into the camera disc seat — move lp_dx/lp_dy");
assert(_web_ok(cam_dx, cam_dy, cam_r, vent_dx, vent_dy, vent_r), "the vent pad breaks into the camera disc seat — move vent_dx/vent_dy");
assert(_web_ok(cam_dx, cam_dy, cam_r, mag_dx, mag_dy, mag_r),    "the magnet pocket ring runs into the camera window — move mag_dx/mag_dy");
assert(_web_ok(cam_dx, cam_dy, cam_r, touch_dx, touch_dy, tch_r), "the touch window runs into the camera disc seat — move touch_dx/touch_dy");
assert(_web_ok(vent_dx, vent_dy, vent_r, lp_dx, lp_dy, lp_r),     "the light-pipe bore breaks into the vent pad — move lp_dx/lp_dy");
assert(_web_ok(vent_dx, vent_dy, vent_r, mag_dx, mag_dy, mag_r),  "the magnet pocket ring runs into the vent pad — move mag_dx/mag_dy");
assert(_web_ok(vent_dx, vent_dy, vent_r, touch_dx, touch_dy, tch_r), "the touch window runs into the vent pad — move touch_dx/touch_dy (or drop opt_buzzer)");
assert(_web_ok(mag_dx, mag_dy, mag_r, lp_dx, lp_dy, lp_r),       "the light-pipe bore breaks into the magnet pocket ring — move lp_dx/lp_dy");
assert(_web_ok(mag_dx, mag_dy, mag_r, touch_dx, touch_dy, tch_r), "the touch window runs into the magnet pocket ring — move touch_dx/touch_dy");
assert(!e_battery || cav_h >= batt_h + 1.0, "battery bay taller than the cavity");
assert(!e_gps || cav_h >= gps_h + 1.0, "GPS bay taller than the cavity");
usb_bot = pcb_z + board_h + usb_z;               // the opening's bottom edge
usb_top = usb_bot + usb_h;
assert(base_h - usb_top >= usb_web - 1e-6, "less than usb_web of wall between the USB opening and the face");
assert(usb_bot - floor_t >= (e_seal ? gasket_groove : 0) + core_min_web() - 1e-6,
       "the USB opening breaks into the ledge band — raise usb_z or standoff_h");
// the mid posts (seal mode) hang beside the bays: each one clears the board's
// clips, the cell and the GPS cradle — in X, or by the cavity's width
function corner_xy() = [
    [ post_cx,  post_cy], [-post_cx,  post_cy],
    [ post_cx, -post_cy], [-post_cx, -post_cy],
];
function mid_xs() = n_mid > 0 ? [for (i = [1 : n_mid]) -post_cx + i*span_l/(n_mid + 1)] : [];
function mid_xy() = [for (x = mid_xs(), s = [1, -1]) [x, s*post_cy]];
function post_xy() = concat(corner_xy(), mid_xy());
function _is_corner(p) = abs(abs(p[0]) - post_cx) < 1e-6;
mid_face = inner_w/2 - (pd - 0.2);                // a mid post's inner face, from the cavity center
function _mid_clear(x, half_w, x0, x1) = (x + pd/2 + 0.4 <= x0) || (x - pd/2 - 0.4 >= x1) || (mid_face >= half_w - 1e-6);
for (x = mid_xs()) {
    assert(_mid_clear(x, board_w/2 + clip_stack + 0.4, board_cx - board_l/2 - 0.5, board_cx + board_l/2 + 0.5),
           "a mid post lands on the board's clips — widen the cavity (inner_w)");
    assert(!e_battery || _mid_clear(x, batt_w/2 + 0.5, batt_cx - batt_l/2 - 1.2, batt_cx + batt_l/2 + 1.2),
           "a mid post lands on the cell — widen the cavity (inner_w)");
    assert(!e_gps || _mid_clear(x, (gps_w + 2.4)/2 + 0.4, gps_cx - (gps_l + 2.4)/2, gps_cx + (gps_l + 2.4)/2),
           "a mid post lands on the GPS cradle — widen the cavity (inner_w)");
    assert(abs(key_x - x) >= pd/2 + core_key_w()/2 + 0.5, "a mid post lands on the plate key — move key_x");
}
// the hardware, DERIVED from the same knobs that draw the holes (canary_core_lib)
hw_thread = screw_insert ? "machine (into the inserts)" : "self-tap";
hw_echo("WAP", [
    hw_item(len(post_xy()), str(hw_screw(screw_size, e_head, pl_L, hw_thread), " (plate to the post ends)")),
    screw_insert ? hw_item(len(post_xy()), str(hw_size_name(screw_size), " heat-set insert ", ins_od, " OD x ", ins_h)) : "",
    e_gland      ? hw_item(len(post_xy()), hw_oring(screw_size)) : "",
    e_seal       ? hw_item(1, "TPU gasket (print part=\"gasket\")") : "",
    e_buzzer && e_seal ? hw_item(1, str("Ø", vent_pad_d, " adhesive ePTFE/GORE vent patch")) : "",
    e_led        ? hw_item(1, str("Ø", lp_d, " light pipe")) : "",
    e_camera && cam_disc_t > 0 ? hw_item(1, str("Ø", cam_disc_d, " x ", cam_disc_t, " clear disc (bond)")) : "",
    e_tamper     ? hw_item(1, str("Ø", mag_d, " x ", mag_h, " disc magnet (press + glue)")) : "",
    e_mount && (mount_style == "keyhole" || mount_style == "both") ? hw_item(2, "#6 pan wall screw (keyholes)") : "",
    // the anti-lift knockouts exist only with keyholes + a battery bay (the floor they sit under)
    mount_extra0 > 0 && kh_lock && e_battery ? hw_item(2, "M3 flat-head wall screw x 12 (pierce the anti-lift knockouts after hanging; 90° seat)") : "",
    // the sun shield's own screws: through its top and tubes into the face's blind pilots over the corner posts
    part == "shield" ? hw_item(len(corner_xy()), str(hw_screw(screw_size, "flat", sh_L, "self-tap"),
                           " (shield to the face's pilots)")) : "",
]);
echo(str("Canary WAP enclosure v0.9 — outer ", out_l, " x ", out_w, " x ",
         base_h + lid_t + mount_extra, " mm  (preset=", preset, ", seal=", e_seal, ", mount=", e_mount, ")"));
echo(str("plate screws: ", screw_size, " ", e_head, " x ", pl_L, ", head ", pl_r, " mm into a ", plate_t,
         " mm plate, ", pl_eng, " mm into ", post_h, " mm posts (pilot ", pl_pil, ")",
         e_gland ? " — O-ring glands under the heads" : "",
         e_shield ? str(" — the shield's pilots ", sh_pilot - lid_t, " mm into the post tops") : ""));
if (wall_eff > wall_t)
    echo(str(e_seal ? "seal mode: " : "", "walls auto-thickened ", wall_t, " -> ", wall_eff,
             " mm — a ", core_min_wall(), " mm skin outside the plate's ", ledge_w, " mm ledge band"));
// the clamp-spacing rule (DESIGN_RULES §6: <= 40 mm between the screws that
// squeeze the gasket). The long walls get n_mid posts by construction; the
// short walls are what is left, and a custom build can still overrun them —
// say so on every render rather than let a sealed build imply what it does
// not deliver
_seal_span = max(span_l/(n_mid + 1), 2*post_cy);
if (e_seal && n_mid > 0)
    echo(str("seal mode: ", n_mid, " mid post(s) per long wall at x = ", mid_xs(), " — clamp spans ", span_l/(n_mid + 1), " (long) / ", 2*post_cy, " (short) mm"));
if (e_seal && _seal_span > 40 + 1e-6)
    echo(str("seal mode: ", _seal_span, " mm between gasket screws (rule: <= 40) — mid-span squeeze rests on the plate's stiffness; treat this build as splash-resistant and put it under the shield or an eave"));
if (mount_extra0 > 0 && kh_lock && !e_battery)
    echo("kh_lock: no battery bay, so no free floor for the anti-lift knockouts — skipped (use mount_style=\"tabs\" for a screwed install)");
assert(!(mount_extra0 > 0 && kh_lock && e_battery)
       || len([for (x = kh_xs) if (abs(batt_cx + 10 - x) < kh_slot_l/2 + kh_head_d/2 + 4) 1]) == 0,
       "anti-lift knockout lands on a keyhole pocket — move kh_inset");

// ----------------------------------------------------------------------------
//  Helpers — rrect2d/rrect come from canary_core_lib; only file-specific
//  geometry stays local
// ----------------------------------------------------------------------------
// a ring of width w centered on the ledge band (the gasket's home) — shared
// by the ledge groove and the gasket part
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_l + ledge_w, inner_w + ledge_w, core_cav_r(corner_r, wall_eff) + ledge_w/2);
        offset(r = -w/2) rrect2d(inner_l + ledge_w, inner_w + ledge_w, core_cav_r(corner_r, wall_eff) + ledge_w/2);
    }
}

// solid web between two plate points at standoff height (a connecting rib)
module floorrib(a, b, w) {
    hull() {
        translate([a[0], a[1], floor_t]) cylinder(d = w, h = standoff_h);
        translate([b[0], b[1], floor_t]) cylinder(d = w, h = standoff_h);
    }
}

// transverse rib across the plate (bounds a battery bay's length), stopped
// short of the ledge cove (the walls are the other part now), notched at the
// +Y rail so the battery leads route flat across it instead of climbing it
module divrib(x) {
    hy = inner_w/2 - floor_cove - 0.2;
    difference() {
        translate([x - 0.6, -hy, floor_t]) cube([1.2, 2*hy, 2.0]);
        if (batt_wire_w > 0)
            translate([x - 0.61, batt_cy + batt_w/2 + 0.5 - batt_wire_w, floor_t - 0.1])
                cube([1.22, batt_wire_w + 0.01, 2.4]);
    }
}
// the bay's side rails: the cell lies 0.5 off them (the walls used to cradle
// it, and the walls are the shell now — a plate that floats tol_slide in its
// bore cannot locate a cell against them). Notched round the mid posts, whose
// inner faces sit flush with the rails
module batt_rails() {
    x0 = batt_cx - batt_l/2 - 0.6;
    for (s = [1, -1]) {
        y0 = batt_cy + s*(batt_w/2 + 0.5) - (s > 0 ? 0 : rail_t);
        difference() {
            translate([x0, y0, floor_t]) cube([batt_l + 1.2, rail_t, 2.0]);
            for (p = mid_xy()) translate([p[0], p[1], floor_t - 0.1]) cylinder(d = pd + 0.8, h = 2.2);
        }
    }
}

// Cantilever snap clip on a board long edge — this file's clip is the one
// canary_snap_lib promoted, so the drawing (and the strain arithmetic that
// used to live only in the clip_t comment) now comes from the library: the
// beam formula runs as an assert on every render instead of protecting one
// file's comment readers. `cx` = position along the edge; `sy` = +1/-1
// selects the +y/-y edge.
module boardclip(cx, sy) {
    snap_boardclip(cx, board_cy + sy * board_w/2, sy,
                   floor_t, floor_t + standoff_h + board_h,
                   clip_w, clip_t, clip_hook, clip_hook_h, clip_clear,
                   over = clip_over);
}

// blind keyhole pocket cut into the thickened plate (xc = feature center along X);
// head circle at the -X end, slot toward +X = UP when the case hangs USB-down.
// This file's pocket is canary_mount_lib's pattern piece — the library draws
// it natively along X, so the interface has one home and this mesh stays put
module keyhole_pocket(xc) {
    mount_keyhole_pocket(xc, -mount_extra, "x",
                         kh_head_d, kh_shank_d, kh_slot_l, kh_head_h, kh_face);
}

// peripheral wedge that 45°-chamfers the shell's back edge (subtract from the
// shell); canary_core_lib owns the drawing — bounded to the footprint so
// external features (tabs) lose only a root nick
module foot_chamfer_cut() {
    foot_chamfer_ring(out_l, out_w, corner_r, foot_cham, -mount_extra);
}

// four external screw ears on the ±Y walls (counterbored for an M3/#6 pan head)
module mount_tabs() {
    wy = out_w/2;
    for (sx = [1, -1], sy = [1, -1]) {
        tx = sx * (inner_l/2 - tab_l/2 - 2);
        hy = sy * (wy + tab_w/2);          // hole center
        translate([0, 0, -mount_extra]) difference() {
            linear_extrude(tab_t) hull() {
                translate([tx, sy * (wy - 1)]) square([tab_l, 2], center = true);  // root buried in the wall
                translate([tx, hy]) circle(d = tab_l * 0.8);
            }
            translate([tx, hy, -0.1])              cylinder(d = tab_hole_d, h = tab_t + 0.2);
            translate([tx, hy, tab_t - tab_cb_h])  cylinder(d = tab_cb_d,   h = tab_cb_h + 0.2);
        }
    }
}

// ----------------------------------------------------------------------------
//  The PLATE (part="base") — the chassis: the floor with its slab, the board
//  standoffs, cradle frame and clips, the battery bay's ribs and rails, the
//  GPS cradle, the keyholes, and the seats the screws enter through. Drawn in
//  the assembled frame: front face at z = floor_t, body down to -mount_extra.
//  Prints back-face down. The catalog's name for this half is back() (the
//  shared probes call it) — an alias, below.
// ----------------------------------------------------------------------------
module back() base();
module base() {
    bx = board_l/2 - standoff_d/2;
    by = board_w/2 - standoff_d/2;
    corners = [ [board_cx+bx, board_cy+by], [board_cx+bx, board_cy-by],
                [board_cx-bx, board_cy-by], [board_cx-bx, board_cy+by] ];   // standoff/board-rest corners
    difference() {
        union() {
            translate([0, 0, floor_t]) pl_plate(plate_x, plate_y, plate_r, plate_t);
            // board support: standoffs + a perimeter cradle frame. (The ribs that
            // tied each standoff into its screw post or the nearest wall are gone:
            // posts and walls are the shell now, and a rib to them tied nothing.)
            for (c = corners) translate([c[0], c[1], floor_t]) cylinder(d = standoff_d, h = standoff_h);
            for (i = [0:3]) floorrib(corners[i], corners[(i+1) % 4], 2.6);
            // press-fit snap clips over the board's two long edges (no screws to hold the PCB)
            if (board_clips)
                for (sy = [1, -1])
                    for (cx = [board_cx - clip_dx, board_cx + clip_dx])
                        boardclip(cx, sy);
            // battery: two transverse ribs bound its length, two rails its width
            if (e_battery) {
                divrib(batt_cx - batt_l/2 - 0.6);
                divrib(batt_cx + batt_l/2 + 0.6);
                batt_rails();
            }
            // GPS module cradle rim (narrow module, so a proper rim is fine).
            // A lead notch on the board side lets the wires exit at plate level
            // instead of cresting the rim and holding the module proud.
            // rim wall is a full 1.2; with a battery the rim's -X wall overlaps the
            // bay's rib so the two print as one
            if (e_gps)
                translate([gps_cx, 0, floor_t])
                    difference() {
                        rrect(gps_l + 2.4, min(gps_w + 2.4, inner_w - 0.5), 1.0, 1.4);
                        rrect(gps_l + 0.8, gps_w + 0.8, 0.5, 3);
                        if (batt_wire_w > 0)
                            translate([-(gps_l + 2.4)/2, 0, 0.7])
                                cube([4, batt_wire_w, 1.7], center = true);   // lead notch, -X (board) side
                    }
        }
        // the screws: seats through the plate, pan heads over their glands in seal mode
        for (p = post_xy())
            pl_seat_cut(p[0], p[1], floor_t, plate_t, screw_size, e_head, pl_r, scr_c, tol_hole, e_gland);
        // the key slot in the plate's edge (the rib is on the shell's bore wall)
        if (lid_key) translate([0, 0, floor_t]) lid_key_slot(key_x, bore_y/2 - tol_slide, 270, plate_t + 0.2, ledge_w + 0.4);
        // blind keyhole pockets (never reach the plate's front — seal-safe)
        if (mount_extra0 > 0)
            for (xc = kh_xs) keyhole_pocket(xc);
        // anti-lift knockouts: blind bores leaving a 0.6 mm web at the back face —
        // after hanging, pierce with M3 countersunk screws into the wall so the case
        // cannot be lifted off the keyholes. The web stays sealed until deliberately
        // used. The 90° head seat keeps the head flush under the battery bay.
        // Placed under the cell, inboard of the bay's rails, where the plate is
        // free; a case with no bay has no floor a screw head can sit under (the
        // board's clips own it), so there they are skipped and the echo says so
        if (mount_extra0 > 0 && kh_lock && e_battery)
            for (sx = [1, -1]) translate([batt_cx + sx*10, batt_cy - (batt_w/2 - 4), 0]) {
                translate([0, 0, -mount_extra + 0.6]) cylinder(d = 3.2, h = mount_extra + floor_t);
                translate([0, 0, floor_t - 1.7]) cylinder(d1 = 3.2, d2 = 6.6, h = 1.71);  // 90° head seat, flush inside
            }
    }
}

// ----------------------------------------------------------------------------
//  The SHELL (part="lid") — face, walls and posts, one piece, with the
//  walls hanging below the face to the back face. Drawn with the face at
//  z = 0..lid_t; everything below z = 0 is inside or the walls. Prints
//  face-down. The catalog's name for this half is front() (the shared probes
//  call it) — an alias, below; the dims/poses generators place lid() at
//  z = base_h, as they always did.
//  The vent cluster and light-pipe bore are canary_core_lib's now
//  (core_vent_cluster / core_lightpipe_bore) — this file used to carry its
//  own copy of both, and the four copies across the weather shells had
//  forked. The knobs above still ride in as arguments.
// ----------------------------------------------------------------------------
module front() lid();
module lid() { translate([0, 0, -base_h]) shell_asm(); }

// the shell in the ASSEMBLED frame (back face at -mount_extra, face at base_h..base_h + lid_t)
module shell_asm() {
    difference() {
        shell_solid();
        // the posts' blind pilots (or insert bores), up from their end faces —
        // cut LAST, through the posts the union below adds (a cut inside that
        // union's own difference would cut nothing: the Sense had that bug)
        for (p = post_xy())
            pl_post_pilot(p[0], p[1], floor_t + pl_relief(), pl_pil,
                          screw_insert ? scr_nominal(screw_size) + 0.3 : scr_d,
                          screw_insert, ins_bore, ins_h);
        // the sun shield's blind pilots, from the face into the corner post tops
        if (e_shield) for (p = corner_xy())
            translate([p[0], p[1], base_h + lid_t - sh_pilot]) cylinder(d = scr_d, h = sh_pilot + 0.1);
    }
}
module shell_solid() {
    posts = post_xy();
    cam = [board_cx + cam_dx,   board_cy + cam_dy];
    lp  = [board_cx + lp_dx,    board_cy + lp_dy];
    vnt = [board_cx + vent_dx,  board_cy + vent_dy];
    mag = [board_cx + mag_dx,   board_cy + mag_dy];
    tch = [board_cx + touch_dx, board_cy + touch_dy];
    gusset_h = max(2, post_h - 0.5);
    gusset_w = min(2.0, rib_t_max(wall_eff));   // the landing width, capped at the old 2.0 target
    usb_zc = usb_bot + usb_h/2;
    union() {
        difference() {
            union() {
                translate([0, 0, base_h]) soft_edge_plate(out_l, out_w, corner_r, lid_t, lid_edge, lid_edge2);
                translate([0, 0, -mount_extra]) rrect(out_l, out_w, corner_r, shell_d + 0.01);
                if (e_mount && (mount_style == "tabs" || mount_style == "both"))
                    mount_tabs();
            }
            // the cavity, coved at the face and at the ledge (canary_core_lib)
            translate([0, 0, base_h]) pl_cavity_cut(inner_l, inner_w, core_cav_r(corner_r, wall_eff), cav_h, floor_cove);
            // the plate's bore below the ledge
            pl_bore_cut(bore_x, bore_y, bore_r, floor_t, plate_t);
            // the gasket groove, cut into the ledge (seal mode)
            if (e_seal)
                translate([0, 0, floor_t - 0.01]) linear_extrude(gasket_groove + 0.01) rim_ring2d(gasket_w);
            // face features, cut from the face
            translate([0, 0, base_h]) {
                if (e_camera) {
                    translate([cam[0], cam[1], -1]) cylinder(d = cam_win_d, h = lid_t + 2);  // camera window
                    // recessed seat on the outer face for a glued clear disc (sits 0.2 below the surface)
                    if (cam_disc_t > 0 && cam_disc_d > 0)
                        translate([cam[0], cam[1], lid_t - (cam_disc_t + 0.2)])
                            cylinder(d = cam_disc_d + 2*tol_slide, h = cam_disc_t + 1);
                }
                if (e_led)    core_lightpipe_bore(lp[0], lp[1], lid_t, lp_d, tol_press);                        // light pipe
                if (e_buzzer) core_vent_cluster(vnt[0], vnt[1], lid_t, vent_pad_d, vent_pad_depth,
                                                vent_ring_d, vent_hole_d, vent_holes);                          // buzzer vent
                if (e_touch)  translate([tch[0], tch[1], -1]) cylinder(d = touch_d, h = lid_t - touch_wall + 1); // touch window (blind thinning)
                // debossed label on the outer face (prints face-down -> crisp first-layer voids)
                if (label_text != "")
                    translate([label_dx, label_dy, lid_t - label_depth])
                        linear_extrude(label_depth + 1)
                            rotate(label_rot)
                                text(label_text, size = label_size, font = label_font,
                                     halign = "center", valign = "center");
                // the house wordmark (opt_mark), debossed exactly where the label would
                // sit and by the same first-layer machinery — canary_mark_lib owns the
                // word and its face, this file only places it (label_dx/dy/rot/size/depth)
                if (opt_mark)
                    translate([label_dx, label_dy, lid_t - label_depth])
                        linear_extrude(label_depth + 1)
                            rotate(label_rot)
                                mark_wordmark(label_size);
            }
            // USB opening: 45°-chamfered corners halve the unsupported bridge and keep
            // any droop out of the plug envelope — this file's print-validated profile
            // (canary_port_lib), MIRRORED so the chamfers face the BACK: in the
            // face-down print that side is the opening's roof
            translate([-out_l/2 - wall_eff*1.5, board_cy, usb_zc])
                rotate([90, 0, 90]) linear_extrude(wall_eff*3)
                    mirror([0, 1, 0]) port_bridge_profile2d(usb_w, usb_h);
            // external antenna bulkhead hole on the far (+X) wall — a teardrop bore
            // (canary_core_lib), so the crown of a horizontal hole prints without sag
            if (e_antenna)
                tearbore_x(out_l/2 - 2*wall_eff, board_cy, pcb_z + ant_z, 4*wall_eff, ant_d);
            // shallow recess framing the USB opening so a flanged silicone plug seats
            // flush; its floor never dips below the ledge (the skin below is the bore)
            if (e_seal && usb_cover) {
                ur_bot = max(usb_bot - usb_cov_pad, floor_t + 0.3);
                ur_top = min(usb_top + usb_cov_pad, base_h - 0.5);
                translate([-out_l/2 - 1, board_cy - (usb_w/2 + usb_cov_pad), ur_bot])
                    cube([1 + usb_cov_dep, usb_w + 2*usb_cov_pad, ur_top - ur_bot]);
            }
            // 45° back-edge chamfer: kills elephant-foot and the sharp first-layer
            // edge where impact delamination starts
            if (foot_cham > 0) foot_chamfer_cut();
            // weep: the case hangs USB-down, so the -X wall is the low wall; the
            // hole leaves it angled downward, beside the USB opening (out of its
            // plug recess) — canary_core_lib weep_cut. It starts high enough that
            // its 30° fall stays ABOVE the gasket groove all the way through the
            // ledge band (a bore through the groove's roof is a channel over the
            // gasket) and exits the skin above the plate's seam
            if (e_weep)
                weep_cut(-inner_l/2, board_cy + usb_w/2 + usb_cov_pad + 2.5,
                         floor_t + (e_seal ? gasket_groove : 0) + weep_d()/2 + 0.4 + (ledge_w - core_min_wall())*tan(30),
                         "-x", wall_eff);
        }
        // drip awning over the USB opening (sideways/desk use) — a solid 45°
        // wedge that prints with the wall; canary_core_lib port_hood
        if (usb_hood)
            translate([-out_l/2, board_cy, usb_zc])
                rotate([0, -90, 0]) rotate([0, 0, 90]) port_hood(usb_w, usb_h, hood_reach, usb_h/2);

        // screw posts from the face's underside to the relief over the ledge,
        // gusseted to their walls (a mid post only to its own) — the gussets
        // root at the face and taper toward the ledge. A corner post also
        // fills the pocket between its two gussets and the cavity's corner:
        // with the cavity coved at the ledge too, that pocket would otherwise
        // close into a sealed void (the mesh gate counts it a part)
        for (p = posts) translate([p[0], p[1], floor_t + pl_relief()]) cylinder(d = pd, h = post_h);
        for (p = posts) translate([0, 0, base_h]) mirror([0, 0, 1]) {
            sx = sign(p[0]); sy = sign(p[1]);
            if (_is_corner(p)) corner_gusset(p[0], p[1], sx*(inner_l/2 + 0.5), p[1], gusset_h, wall_eff, pd, gusset_w);
            corner_gusset(p[0], p[1], p[0], sy*(inner_w/2 + 0.5), gusset_h, wall_eff, pd, gusset_w);
        }
        for (p = posts) if (_is_corner(p))
            translate([min(p[0], sign(p[0])*(inner_l/2 + 0.5)), min(p[1], sign(p[1])*(inner_w/2 + 0.5)), floor_t + pl_relief()])
                cube([inner_l/2 + 0.5 - abs(p[0]), inner_w/2 + 0.5 - abs(p[1]), post_h]);
        // the plate key (canary_core_lib): a rib on the +Y bore wall, inboard of
        // the +X corner post; the plate's edge carries the slot
        if (lid_key) lid_key_rib(key_x, bore_y/2, 270, floor_t, plate_t + 0.5);

        translate([0, 0, base_h]) {
            // perimeter rib ring under the face: raises the flat face's bending
            // stiffness (stiffness ~ t³) against pry/oil-canning for ~1 g of
            // material. Fused 0.2 into the walls and cleared around every face
            // feature and the screw posts.
            if (lid_ribs) {
                ro_l = inner_l + 0.4;
                ro_w = inner_w + 0.4;
                difference() {
                    translate([0, 0, -lid_rib_h]) linear_extrude(lid_rib_h + 0.1)
                        difference() {
                            rrect2d(ro_l, ro_w, core_cav_r(corner_r, wall_eff) + 0.2);
                            rrect2d(ro_l - 2*lid_rib_w, ro_w - 2*lid_rib_w, 0.1);
                        }
                    for (p = posts)
                        translate([p[0], p[1], -lid_rib_h - 0.1]) cylinder(d = pd + 1.6, h = lid_rib_h + 0.2);
                    // keep-outs so the ring never blocks a face feature, wherever it is placed
                    kos = [ [cam[0], cam[1], e_camera ? max(cam_win_d, cam_disc_d) + 3 : 0],
                            [lp[0],  lp[1],  e_led    ? lp_d + 4                     : 0],
                            [vnt[0], vnt[1], e_buzzer ? vent_pad_d + 3               : 0],
                            [tch[0], tch[1], e_touch  ? touch_d + 3                  : 0],
                            [mag[0], mag[1], e_tamper ? mag_d + 2*tol_press + 4.8    : 0] ];
                    for (k = kos) if (k[2] > 0)
                        translate([k[0], k[1], -lid_rib_h - 0.1]) cylinder(d = k[2], h = lid_rib_h + 0.2);
                    // keep the USB cable path clear
                    translate([-inner_l/2, board_cy, 0]) cube([14, usb_w + 4, 3*lid_rib_h], center = true);
                }
            }
            // battery hold-down: two ribs on the face underside over the bay, resting at
            // batt_h above the plate (the swelling allowance stays); shortened at the
            // +Y rail so the lead channel under them stays clear
            if (batt_hold && e_battery) {
                rib_h = cav_h - batt_h;
                for (dx = [-batt_l/4, batt_l/4])
                    translate([batt_cx + dx - 0.8, -(batt_w - 2)/2 - 1.0, -rib_h])
                        cube([1.6, batt_w - 2 - batt_wire_w, rib_h + 0.1]);
            }
            // tamper magnet pocket (blind, opens downward; press fit — add a drop of glue)
            // the ring is embedded 0.1 into the face so the export is one watertight shell
            if (e_tamper)
                translate([mag[0], mag[1], -mag_h])
                    difference() {
                        cylinder(d = mag_d + 2*tol_press + 2.4, h = mag_h + 0.1);
                        translate([0, 0, -0.1]) cylinder(d = mag_d + 2*tol_press, h = mag_h + 0.1);
                    }
        }
    }
}

// ----------------------------------------------------------------------------
//  GASKET — TPU seal ring matching the ledge groove (seal mode).
//  Print in TPU 90–95A, 2 perimeters, 100 % infill. Squeezes ~20 % under the
//  screws; the ring is 0.5 narrower than the groove (~86 % fill) so the
//  incompressible TPU has somewhere to flow instead of propping the plate open.
// ----------------------------------------------------------------------------
module gasket() {
    linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5);
}

// ----------------------------------------------------------------------------
//  CLIP TEST COUPON — print this alone to tune clip_t / clip_hook / clip_clear.
//  A short channel the width of the board, with a snap clip on each long edge:
//  press a 1.2 mm scrap (or the real board edge) in and feel the click.
// ----------------------------------------------------------------------------
module coupon() {
    cl = 26;                                       // coupon length
    ww = board_w + 2*(clip_clear + clip_t) + 6;    // floor width (clips + margin)
    union() {
        linear_extrude(floor_t) offset(1) offset(-1) square([cl, ww], center = true);  // floor
        for (sy = [1, -1])                          // board-rest rails at the edge line
            translate([-cl/2, sy*(board_w/2 - standoff_d/2) - standoff_d/2, floor_t])
                cube([cl, standoff_d, standoff_h]);
        boardclip(0,  1);                           // a clip on each long edge (uses board_w/board_cy)
        boardclip(0, -1);
        // (the tie-rib stub the coupon used to carry beside its -X clip went with
        // the case's tie ribs: the plate draws none, so the coupon draws none)
    }
}

// ----------------------------------------------------------------------------
//  SOLAR RADIATION SHIELD — second roof on hollow standoffs over the face.
//  Prints panel-on-bed, tubes up; installs FLIPPED (about X), so aperture
//  positions are mirrored in y here to land over the real face features.
//  Fasten with its own four flat-head screws into the face's blind pilots
//  over the CORNER posts (sh_L) — the mid posts carry plate screws only.
// ----------------------------------------------------------------------------
module shield() {
    difference() {
        union() {
            rrect(out_l + 2*sh_over, out_w + 2*sh_over, corner_r + sh_over, sh_t);
            for (p = corner_xy()) translate([p[0], p[1], 0]) cylinder(d = 7, h = sh_t + sh_gap);
        }
        for (p = corner_xy()) {
            translate([p[0], p[1], -0.1]) cylinder(d = scr_d + 0.8, h = sh_t + sh_gap + 0.2);
            // head seat on the bed face = the installed top (first-layer void)
            translate([p[0], p[1], -0.01])
                cylinder(d1 = scr_flat_d(screw_size) + 0.6, d2 = scr_d + 0.8, h = scr_flat_h(screw_size));
        }
        // apertures over face features (y mirrored for the installation flip)
        if (e_camera)
            translate([board_cx + cam_dx, -(board_cy + cam_dy), -1])
                cylinder(d = max(cam_win_d, cam_disc_d) + 4, h = sh_t + 2);
        if (e_led)
            translate([board_cx + lp_dx, -(board_cy + lp_dy), -1])
                cylinder(d = lp_d + 5, h = sh_t + 2);
        if (e_touch)
            translate([board_cx + touch_dx, -(board_cy + touch_dy), -1])
                cylinder(d = touch_d + 4, h = sh_t + 2);
    }
}

// ----------------------------------------------------------------------------
//  DESICCANT TRAY — slotted open box for a 1 g silica pack; friction-fit or
//  VHB it into any Canary cavity (battery bay, cable well). Universal part.
// ----------------------------------------------------------------------------
module tray() {
    difference() {
        rrect(tray_l, tray_w, 2, tray_h);
        translate([0, 0, 1.2]) rrect(tray_l - 2.4, tray_w - 2.4, 1.4, tray_h);
        for (i = [-1, 0, 1])                       // floor slots — moisture path
            translate([i*6, 0, -0.1]) linear_extrude(1.5) square([2.5, tray_w - 6], center = true);
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
if      (part == "coupon") coupon();
else if (part == "shield") shield();
else if (part == "tray")   tray();
else if (part == "gasket") {
    assert(e_seal, "the gasket needs opt_seal=true (or preset=battery_weather) so its ring matches the groove");
    gasket();
}
else if (part == "base")   base();
else if (part == "lid")    translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();   // printable orientation: face-down
else {
    // the assembled preview wears the chosen colorway (registry: canary_color_lib);
    // color() is preview-only — a single-part STL export is byte-identical
    color(cw_body(colorway)) base();
    color(cw_body(colorway))
        translate([0, out_w + 8, 0]) translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();
    if (e_seal) color(cw_light(colorway)) translate([0, -(out_w + 8), 0]) gasket();
}
