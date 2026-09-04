// ============================================================================
//  SecuraCV Canary WAP — 3D-printable enclosure (parametric)  v0.7
//  @env cer=2 ip="~IP54" basis="weather preset"
//  Board: Seeed XIAO ESP32-S3 Sense + optional LiPo (placed beside the board)
//  Features: light-pipe port, buzzer/pressure vent, camera/sensor window with
//            sealed-disc seat, USB-C access, board standoffs + snap clips,
//            M2 screw lid, tamper-magnet pocket, opt-in weather sealing
//            (TPU gasket + drip-edge lid), opt-in wall mounting (keyholes/tabs).
//
//  Units: millimeters.  CAD: OpenSCAD (https://openscad.org).
//
//  ⚠️ VERIFY BEFORE PRINTING. Defaults are nominal for the XIAO ESP32-S3 Sense
//     (PCB 21.0 x 17.5 mm) plus a 503450-class LiPo. Measure YOUR board, battery,
//     camera-lens position and USB-C connector and adjust the parameters below.
//     Print the clip coupon first, then the lid (it carries the fiddly features).
//
//  Layout (top view):   [ board @ USB end ]  [ gap ]  [ battery ]  [ gps ]
//                         -X  ............................  +X
//
//  Render a part:  set `part` then F6 (or use the CLI in the README).
//
//  2026-08-23: adopted the shared contract libraries (core/mount/snap/port/
//              board/mark) — the local helper copies they replace drew the
//              same geometry, so every committed mesh is unchanged; new
//              opt_mark knob debosses the house wordmark (default off).
//  2026-09-03: assembly review. Selectable fastener (`screw_size` m2/m2.5/m3
//              + `screw_head` flat/pan, from the core lib's screw registry —
//              the M2 flat row IS this file's validated seat, so the default
//              mesh is unchanged); seal-mode `head_seal` O-ring glands (the
//              corner screws sit INSIDE the gasket line, so a bare screw was
//              a drip path into the cavity); `opt_weep` drain at the hung-low
//              USB wall (ON in the weather preset); `usb_hood` drip awning
//              for sideways/desk use; `batt_hold` lid ribs that stop the cell
//              bouncing in a drop (the cell sat under ~8 mm of free air);
//              heat-set bores cut 0.3 under the knurl instead of 0.1 (an
//              insert that spins under torque is not a service thread).
//              FIT FIXES that move the released meshes (v0.8): the board
//              standoff rises 3.0 -> 3.5 so the clip beam (now 4.7 mm) keeps
//              the MEASURED 17.8 board under the 4.5 % strain budget — the
//              gate only ever saw the drawn 17.5 board (5.5 % on the real
//              one); the standoff->post tie ribs no longer root into the -X
//              clips (a rib under the beam cut its free length to 1.2 mm,
//              ~50 % strain); the cavity now guarantees 1.5 mm of wall over
//              the USB opening in EVERY mode (compact shipped 0.65 mm, three
//              layers of unsupported bridge); corner posts overlap the wall
//              by 0.2 instead of floating 0.2 off it; battery ribs reach the
//              walls; the GPS rim keeps a printable 1.2 wall and fuses to
//              the battery rib; anti-lift knockouts sit under the battery
//              bay (on a case with no bay they landed under the board's
//              clips) — and the tamper pocket counts the Sense stack under
//              it, not the bare PCB.
// ============================================================================

use <canary_core_lib.scad>    // rrect/rrect2d, soft-edge lid, foot chamfer, screw seats
use <canary_mount_lib.scad>   // the stud/keyhole hanging interface (this file cut the pattern)
use <canary_snap_lib.scad>    // the cantilever board clip + its strain budget
use <canary_port_lib.scad>    // bridge-safe USB opening (this file's polygon, promoted)
use <canary_board_lib.scad>   // board registry — the XIAO numbers the knobs cite
use <canary_mark_lib.scad>    // the house wordmark (opt_mark)
use <canary_color_lib.scad>   // the colorway registry — assembled-preview spools

/* [What to render] */
part   = "all";       // ["base","lid","all","coupon","gasket","shield","tray"]   (coupon = clip-fit test; gasket = TPU seal ring; shield = solar radiation shield; tray = desiccant tray)

/* [Preset] — quick configs; choose "custom" to use the Peripherals checkboxes */
preset = "custom";    // ["custom","battery_full","compact_plain","battery_weather"]

/* [Peripherals you have — tick what is fitted (applied when preset = custom)] */
opt_camera  = true;   // XIAO *Sense* camera   -> lid sensor window + taller cavity (off = plain XIAO ESP32-S3)
opt_buzzer  = true;   // piezo buzzer          -> lid vent + GORE seat
opt_led     = true;   // external status LED   -> light-pipe port
opt_battery = true;   // LiPo                  -> battery bay (enlarges the case)
opt_gps     = false;  // L76K GPS module       -> internal module bay
opt_tamper  = true;   // reed/Hall + magnet    -> lid magnet pocket
opt_touch   = false;  // cap-touch pad         -> thinned "touch window" on the lid
opt_antenna = false;  // external u.FL antenna -> side bulkhead hole

/* [Weather sealing — opt-in; the default case stays simple/indoor] */
opt_seal    = false;  // perimeter TPU gasket + drip-edge lid + USB plug recess (splash-resistant, NOT immersion)
gasket_w      = 1.6;  // gasket groove width (the printed gasket is 0.5 narrower)
gasket_groove = 1.2;  // groove depth into the base rim
gasket_proud  = 0.3;  // uncompressed gasket stand-proud (~20 % squeeze under the lid screws;
                      // TPU is incompressible — groove fill is ~86 %, leaving room to flow)
skirt_h       = 3.0;  // drip-edge skirt drop over the base wall (sheds water off the seam)
skirt_t       = 1.6;  // skirt wall thickness
usb_cover     = true; // (seal mode) shallow recess framing the USB port for a flanged silicone plug
usb_cov_pad   = 2.0;  // recess margin around the USB opening
usb_cov_dep   = 1.0;  // recess depth into the outer wall face
head_seal     = false; // (seal mode) O-ring under every lid screw head: the posts stand INSIDE the gasket
                       // line, so a bare screw is a drip path down the thread into the cavity —
                       // needs screw_head = "pan" (a flat head's cone would eject the ring)
opt_weep      = false; // Ø1.5 weep at the cavity's low point (the USB wall, hung USB-down): condensate
                       // leaves, driven rain does not enter. The weather preset turns it on.
usb_hood      = false; // drip awning over the USB opening — for a case standing sideways or on a desk
                       // (hung USB-down the port faces the ground and needs none)
hood_reach    = 4.0;   // how far the awning's drip edge stands off the wall  // [2:0.5:8]

/* [Mounting — opt-in; case hangs with the USB end facing DOWN] */
opt_mount   = false;  // wall-mount features (keyholes thicken the case back by `kh_extra`)
mount_style = "keyhole"; // ["keyhole","tabs","both"]
// keyholes — BLIND pockets in a thickened back; they never breach the cavity (seal-safe).
// Defaults are the catalog's stud/keyhole standard (canary_mount_lib) — deviations
// earn their keep on the fit coupon, not in a quiet knob edit
kh_extra    = 3.0;    // back thickening that hosts the keyhole pockets
kh_head_d   = 7.0;    // screw-head pass hole (fits #6 / M3.5 pan head) — mount_kh_head_d()
kh_shank_d  = 4.2;    // shank slot width — mount_kh_shank_d()
kh_slot_l   = 8.0;    // slot travel (slot runs toward +X = UP when the USB faces down) — mount_kh_slot_l()
kh_head_h   = 3.5;    // total pocket depth (face web + head cavity) — mount_kh_head_h()
kh_face     = 1.0;    // face web thickness the screw head grips behind — mount_kh_face()
kh_inset    = 10.0;   // keyhole centers at x = ±(inner_l/2 − kh_inset); auto-merges to one on small cases
// external screw tabs — four ears on the ±Y walls, fully outside the seal envelope
tab_l       = 10.0;   // ear length along the wall
tab_w       = 8.0;    // ear protrusion from the wall
tab_t       = 3.0;    // ear thickness
tab_hole_d  = 3.6;    // through-hole (M3 / #6)
tab_cb_d    = 7.0;    // pan-head counterbore diameter
tab_cb_h    = 1.0;    // counterbore depth

/* [Aesthetics] */
colorway    = "graphite"; // ["graphite","canary","snow","forest","midnight"] assembled-preview spool set (canary_color_lib; single-part exports carry no color)
lid_edge    = 0.8;    // 45° chamfer around the lid's top edge (0 = sharp slab)  // [0:0.1:1.5]
lid_edge2   = 0.8;    // second (~66°) stage of the show-face edge, mm — ON is the house look (core_face_edge2()); it is what reads as a roundover instead of a bevel. 0 leaves the plain 45° facet any CAD default gives you  // [0:0.1:1.5]
// The wordmark sits where label_text would (label_dx/dy/rot/size/depth place
// and size it) and is gated by the mark library's measured type metrics, so a
// size that would print as a smudge or run off the lid is refused before a
// print, not after
opt_mark    = false;  // deboss the house wordmark instead of a custom label (exclusive with label_text)
label_text  = "";     // debossed lid label, e.g. "CANARY" ("" = off; needs the font installed)
label_size  = 5.0;    // text height
label_depth = 0.5;    // deboss depth (prints as crisp first-layer voids, lid prints face-down)
label_dx    = 0.0;    // label center offset from the LID center (not the board center)
label_dy    = -10.0;
label_rot   = 0;      // label rotation (degrees)
label_font  = "Liberation Sans:style=Bold";

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
stack_camera   = 8.0;   // headroom above the PCB with the Sense camera
stack_plain    = 4.5;   // headroom above the PCB for a plain XIAO ESP32-S3
board_stack_h  = e_camera ? stack_camera : stack_plain;

/* [Battery] (LiPo, placed beside the board) */
batt_l         = 50.0;  // 503450 ~ 50 x 34 x 5 mm
batt_w         = 34.0;
batt_h         = 6.0;   // bay height — keep >= 1 mm over the nominal cell for LiPo swelling  // [4:0.5:12]
batt_gap       = 2.5;   // gap between board zone and battery zone
batt_wire_w    = 4.0;   // lead channel notched into the bay ribs, hugs the +Y wall (0 = off)
batt_hold      = true;  // lid-side ribs over the bay that stop the cell bouncing in a drop: they
                        // rest at batt_h over the floor, so the swelling allowance built into
                        // batt_h stays (the cell used to sit under ~8 mm of free air)

/* [GPS module] (L76K, internal bay after the board/battery) */
gps_l          = 16.0;
gps_w          = 16.0;
gps_h          = 4.0;
gps_gap        = 2.3;   // rim fuses into the battery rib at 2.3 (a larger gap leaves a slot the slicer can't print)

/* [Antenna] external u.FL/SMA bulkhead hole on the far (+X) wall */
ant_d          = 6.5;   // SMA bulkhead thread is 6.35; the bore is a teardrop so its crown prints without sag
ant_z          = 4.0;   // bore center above the PCB underside (clears the corner posts' gussets)

/* [Shell] */
wall_t         = 2.0;   // side wall thickness (auto-thickened in seal mode to host the groove)
floor_t        = 2.0;   // base floor thickness
lid_t          = 2.0;   // lid top thickness
lip_h          = 4.0;   // how far the lid lip drops into the base
lip_t          = 1.2;   // lid lip wall thickness
corner_r       = 3.0;   // outside corner radius

/* [Print tolerances] — per-side clearances; tune these once for your printer
   (defaults = the catalog trio, core_tol_*() in canary_core_lib, dialed on the fit coupon) */
tol_slide      = 0.20;  // sliding fits: lid lip <-> base, drip skirt, camera-disc seat — core_tol_slide()
tol_press      = 0.10;  // press fits: tamper magnet, LED light pipe — core_tol_press()
tol_hole       = 0.30;  // clearance holes: lid screws — core_tol_hole()

/* [Engineering — durability/rigidity options (see README "Engineering & materials")] */
screw_insert = false;   // M2 brass heat-set inserts in the corner posts (service-grade threads;
                        // posts auto-fatten, M2 machine screws replace the self-tappers)
insert_d     = 3.5;     // (m2) insert knurl OD (M2 short series: 3.5 x 4.0) — the bore is cut 0.3 under
                        // it so the brass bites (a 0.1 interference spun under driver torque)
insert_h     = 4.0;     // (m2) insert length; other sizes read the registry
lid_ribs     = true;    // perimeter rib ring under the lid — stiffens the flat face (t³) against pry/flex
lid_rib_w    = 2.5;     // rib ring width
lid_rib_h    = 1.0;     // rib depth below the lid underside — keep <= the 1.0 mm component
                        // headroom built into cav_h, or raise stack_camera/stack_plain to suit
foot_cham    = 0.5;     // 45° chamfer on the bottom edge: elephant-foot + first-layer delamination guard (0 = off)
kh_lock      = true;    // (keyhole mounts) two anti-lift knockout bosses: 0.6 mm web, pierce with
                        // #4/M3 screws on install so the case can't be lifted off the wall screws

/* [Thermal / outdoor kit] — part="shield" is a Stevenson-screen style solar
   radiation shield: a second roof standing sh_gap above the lid on hollow
   standoffs, fastened by the existing corner screws (swap in M2 x 16-18).
   It shades the case and vents the gap; apertures open automatically over
   the camera / light pipe / touch window. part="tray" is a slotted clip-in
   desiccant tray for a 1 g silica pack (VHB or friction fit). */
sh_gap   = 6.0;    // shield air gap above the lid
sh_over  = 6.0;    // shield overhang beyond the case walls (shade + rain shadow)
sh_t     = 1.6;    // shield panel thickness
tray_l   = 24.0;   // desiccant tray footprint
tray_w   = 18.0;
tray_h   = 8.0;

/* [Standoffs / screw posts] */
standoff_h     = 3.5;   // PCB sits this high off the floor (clearance for bottom parts). 3.5, not
                        // 3.0: the clip beam is standoff + PCB, and at 4.7 mm the MEASURED 17.8
                        // board inserts at 4.4 % strain; at 4.2 it was 5.5 % against a 4.5 % budget
standoff_d     = 4.0;
post_d         = 5.0;   // corner screw posts (lid screws thread into these; auto-fattened for larger screws)
screw_size     = "m2";  // ["m2","m2.5","m3"] lid screw — the catalog screw registry (canary_core_lib) sets
                        // pilot, clearance, head seat, insert bore and post floor; "m2" keeps the three
                        // print-validated numbers below exactly as they are
screw_head     = "flat"; // ["flat","pan"] the head in the bag: flat = 90° countersink (this lid's validated
                        // seat), pan = flat-floored counterbore (what head_seal needs)
screw_d        = 1.6;   // (m2) self-tapping pilot — 1.6 mm so threads bite (2.0 = no grip)
screw_head_d   = 4.0;   // (m2 flat) the flat head's Ø, used by the shield's seat; the lid's 90° cone
                        // mouths at scr_c + 2*screw_head_h = 4.6, so a Ø3.8 head seats 0.4 sub-flush
screw_head_h   = 1.2;   // (m2 flat) countersink depth — the cone below is a true 90° seat for an M2 flat head

/* [USB-C port] — on the board's USB end (-X short wall) */
usb_w          = 12.0;  // opening width: clears rugged USB-C cable boots (connector body ~8.9 mm)  // [9:0.5:14]
usb_h          = 6.5;   // opening height: boot clearance (connector body ~3.2 mm) — slim if cable is bare  // [4:0.5:8]
usb_web        = 1.5;   // wall guaranteed ABOVE the opening in every mode (the compact case shipped 0.65)
usb_z          = -1.65; // centers the opening on the connector AXIS: the C shell is 3.2 mm tall on the
                        // PCB, so the axis sits at PCB-top + 1.6; a boot needs equal room below the axis

/* [Lid features] — offsets are measured FROM THE BOARD CENTER (mm). Measure your board! */
// Camera / sensor window + recessed seat for a glued clear disc (12 x 1 mm PMMA/PC)
cam_win_d      = 9.0;   // asserted against cam_fov at the disc's inner plane so a corner never vignettes
cam_fov        = 66;    // lens diagonal field of view (OV2640 on the Sense: 66°)  // [40:1:120]
cam_lens_h     = 6.0;   // lens front above the PCB top (Sense expansion board + OV2640 module) — MEASURE
cam_disc_d     = 12.0;  // clear-disc diameter (seat = disc + 2*tol_slide; 0 = no seat, bare hole)
cam_disc_t     = 1.0;   // clear-disc thickness (disc sits 0.2 recessed below the lid face)
cam_dx         = 0.0;
cam_dy         = 0.0;
// Light-pipe / status-LED port (press fit: hole = lp_d + 2*tol_press)
lp_d           = 3.0;   // light-pipe diameter (3 mm pipe -> 3.2 mm hole at default tol_press)
lp_dx          = 5.0;
lp_dy          = 5.0;
// Buzzer + pressure vent (recess seats an adhesive GORE vent; ring of holes passes sound/pressure)
vent_pad_d     = 12.0;
vent_pad_depth = 0.8;
vent_hole_d    = 1.0;   // fine holes — insect-resistant (the README's outdoor rule: <= 1.0 mm); the
                        // membrane behind them seals, so the holes only need to pass sound + pressure
vent_ring_d    = 6.0;
vent_holes     = 10;
vent_dx        = 7.0;
vent_dy        = -4.0;
// Cap-touch window — local thinning so capacitance couples through the lid (when opt_touch)
touch_d        = 12.0;
touch_wall     = 0.8;   // remaining lid thickness at the pad
touch_dx       = -3.0;
touch_dy       = -5.0;

/* [Tamper magnet] — blind pocket on the LID underside, over the board's reed/Hall switch */
mag_d          = 6.0;   // MAGNET diameter (pocket = mag_d + 2*tol_press — press fit; add a drop of glue)
mag_h          = 2.2;   // pocket depth — a 6 x 2 mm disc is standard
mag_under      = 6.0;   // tallest part on the board UNDER the pocket: the Sense expansion board's top
                        // (camera excluded — keep the pocket off the lens) — MEASURE. The cavity grows
                        // to keep 1 mm between it and the pocket ring
mag_dx         = -6.0;
mag_dy         = 5.0;

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
clip_dx        = 5.25;  // clip centers at board_cx ± clip_dx (5.25 = board_l/4, the validated spot). The
                        // XIAO's castellated pads run to ±8.5 along each long edge, so a 6 mm tab here
                        // sits over three pads: solder wires from the UNDERSIDE and keep the top pad
                        // flat, or a fillet stops the lip latching  // [3:0.25:8]
clip_over      = 0.15;  // extra lip travel per side for the MEASURED board (brd_xiao_w_measured() 17.8
                        // vs the drawn 17.5) — fed to the strain gate, not to the drawing

/* [Quality] */
// curve quality: $fa/$fs give smooth big arcs (pill corners, hood) without
// exploding tiny holes into thousands of facets like a large $fn would
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
// 1.2 mm cheek each side of the groove — the structural floor, not the web.
// This file shipped `gasket_w + 1.6` (0.8 mm cheeks) after its three siblings
// were corrected to 2*core_min_wall(): the fork the audit predicted, caught in
// the released case. The cheeks ARE the seal path's walls.
wall_eff     = e_seal ? max(wall_t, gasket_w + 2*core_min_wall()) : wall_t;
// the fastener, resolved once: the M2 rows of the registry equal this file's
// validated knobs, so a default render reads the knobs and larger sizes read
// the registry (a pilot sized for M2 under an M3 self-tapper splits the post)
scr_d   = (screw_size == "m2") ? screw_d : scr_pilot(screw_size);
scr_c   = max(scr_d + 2*tol_hole, scr_clear(screw_size));            // lid clearance hole
head_d  = (screw_size == "m2" && screw_head == "flat") ? screw_head_d
        : (screw_head == "flat") ? scr_flat_d(screw_size) : scr_pan_d(screw_size);
head_h  = (screw_size == "m2" && screw_head == "flat") ? screw_head_h
        : (screw_head == "flat") ? scr_flat_h(screw_size) : scr_pan_h(screw_size);
ins_od  = (screw_size == "m2") ? insert_d : scr_insert_d(screw_size) + 0.3;   // knurl OD
ins_h   = (screw_size == "m2") ? insert_h : scr_insert_h(screw_size);
ins_bore = ins_od - 0.3;                                              // 0.3 interference: the brass bites
pd      = max(screw_insert ? max(post_d, ins_od + 2.4) : post_d,     // >=1.2 mm wall around an insert
              scr_post_min(screw_size));                              // >=1.5 mm wall around the pilot
// a pan head's flat floor must leave a web under it: if the seat would cut
// through the lid, a pad on the underside carries the missing thickness and
// the posts shorten by the same amount (the Vision/Sense lesson: a 2.0 seat
// in a 2.0 plate is a through-hole the head falls through)
head_pad = (screw_head == "pan") ? max(0, head_h + 1.0 - lid_t) : 0;

board_zone_l = board_l + 2*board_clear;
batt_zone_l  = e_battery ? (batt_gap + batt_l) : 0;
gps_zone_l   = e_gps     ? (gps_gap  + gps_l)  : 0;
extra_l      = batt_zone_l + gps_zone_l;                 // internal bays appended after the board
post_corner  = pd + 1.5;                 // positioning margin so a screw post sits in the corner, clear of the board

// cavity: board + bays along X, +X dead zone keeps true corners for the screw posts.
// The board is ALWAYS biased to the -X (USB) wall so the connector reaches the opening
// (v0.7 fix — centering it left the USB ~6.5 mm behind the wall on the compact case).
clip_stack = board_clips ? (clip_clear + clip_t) : 0;
inner_l = board_zone_l + extra_l + post_corner + 1.0;
inner_w = max(board_w + 2*board_clear,
              e_battery ? batt_w : 0,
              e_gps ? gps_w : 0,
              board_w + 2*post_corner,
              board_w + 2*(clip_stack + 0.6 + pd + 0.2)   // keep the clips clear of the corner posts
             ) + 1.0;
cav_h_min = max(standoff_h + board_h + board_stack_h + 1.0,          // internal height above floor
                e_tamper ? standoff_h + board_h + mag_under + 1.0 + mag_h : 0,   // pocket ring clears the stack under it
                e_battery ? batt_h + 1.0 : 0, e_gps ? gps_h + 1.0 : 0);
// the wall above the USB opening: usb_web of solid in every mode; seal mode adds the gasket groove
// and, with the plug recess, its top frame (a flanged plug needs the frame on all four sides)
usb_over = usb_web + (e_seal ? gasket_groove + (usb_cover ? usb_cov_pad : 0) : 0);
cav_h    = max(cav_h_min, standoff_h + board_h + usb_h + usb_z + usb_over);

out_l  = inner_l + 2*wall_eff;
out_w  = inner_w + 2*wall_eff;
base_h = floor_t + cav_h;
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
module wap_fitcheck(lift = 0.1) {
    intersection() { translate([0, 0, base_h + lift]) lid(); base(); }
}

pcb_z    = floor_t + standoff_h;                // absolute z of PCB underside
board_cx = -inner_l/2 + board_clear + board_l/2 + 0.5;   // USB-biased (0.5 = positioning margin, not a fit)
board_cy = 0;
zone0    = -inner_l/2 + board_zone_l + 0.5;     // x where the appended bays begin
batt_cx  = zone0 + batt_gap + batt_l/2;         // battery bay center
gps_cx   = zone0 + batt_zone_l + gps_gap + gps_l/2;   // GPS bay center (after battery, if any)

// mounting back-thickening (keyhole pockets live below the floor, never in the cavity)
mount_extra = (e_mount && (mount_style == "keyhole" || mount_style == "both")) ? kh_extra : 0;
// keyhole positions: two near the ends, or one centered when the case is too short
kh_x   = inner_l/2 - kh_inset;
kh_xs  = (kh_x >= kh_slot_l/2 + kh_head_d/2 + 2) ? [-kh_x, kh_x] : [0];

// drip-edge lid plate (seal mode: plate grows so the skirt overlaps the base wall)
skirt_gap = tol_slide + 0.2;                    // skirt-to-wall running clearance
plate_l   = e_seal ? out_l + 2*(skirt_gap + skirt_t) : out_l;
plate_w   = e_seal ? out_w + 2*(skirt_gap + skirt_t) : out_w;
plate_r   = e_seal ? corner_r + skirt_gap + skirt_t : corner_r;

// sanity checks + a measurable size echo for scripted verification
assert(wall_t > 0 && floor_t > 0 && lid_t > 0, "shell thicknesses must be positive");
assert(standoff_h > 0, "standoff_h must be positive");
assert(lip_h < cav_h, "lip_h must be less than the cavity height or the lid bottoms out");
assert(screw_head_d > screw_d, "screw_head_d must be larger than screw_d");
assert(head_d > scr_c, "the screw head must be larger than its clearance hole, or it falls through the lid");
assert(!head_seal || screw_head == "pan",
       "head_seal seats an O-ring under a PAN head — set screw_head = \"pan\" (a flat head's cone ejects the ring)");
assert(!head_seal || e_seal, "head_seal only means something in seal mode (opt_seal / the weather preset)");
assert(screw_head == "flat" || head_h + 1.0 - lid_t <= 1.5,
       "pan-head seat needs more than 1.5 mm of underside pad — thicken lid_t instead");
assert(!(usb_hood && e_seal && usb_cover),
       "usb_hood and the silicone-plug recess (usb_cover) both own the wall around the port — pick one");
assert(!batt_hold || !e_battery || cav_h - batt_h >= 1.5,
       "batt_hold ribs would be shorter than 1.5 mm — the bay is already nearly full-height");
assert(!e_seal || gasket_groove <= lip_h - 0.5, "gasket_groove must stay below the lid lip depth");
assert(!e_seal || core_gasket_fill(gasket_w, gasket_groove, gasket_proud) <= core_gasket_fill_max(),
       str("the printed TPU ring would fill ", round(100*core_gasket_fill(gasket_w, gasket_groove, gasket_proud)),
           " % of its groove - past ", round(100*core_gasket_fill_max()),
           " % the incompressible gasket props the lid open instead of sealing; narrow gasket_w or deepen gasket_groove"));
assert(!e_seal || skirt_h < base_h, "skirt_h must be shorter than the base");
assert(cam_disc_t == 0 || cam_disc_t + 0.2 < lid_t, "cam_disc_t too thick for lid_t");
assert(cam_disc_t == 0 || cam_disc_d == 0 || cam_disc_d > cam_win_d,
       "cam_disc_d must be larger than cam_win_d or the disc falls through");
assert(mount_extra == 0 || kh_head_h + 1.5 <= floor_t + kh_extra, "keyhole pocket too deep — raise kh_extra");
assert(mount_extra == 0 || kh_head_h > kh_face, "kh_head_h must exceed kh_face (it includes the face web)");
assert(mount_extra == 0 || kh_head_d > kh_shank_d, "kh_head_d must be larger than kh_shank_d");
assert(mount_extra == 0 || kh_slot_l > 0, "kh_slot_l must be positive");
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
assert(!opt_mark || mark_word_ink_w("securaCV", label_size) <= plate_l - 4.0,
       str("the wordmark draws ", mark_word_ink_w("securaCV", label_size),
           " mm at label_size ", label_size, " on a ", plate_l,
           " mm lid (2 mm margin per side) — shrink label_size"));
assert(batt_wire_w >= 0, "batt_wire_w must be non-negative");
assert(!e_antenna || !e_seal || pcb_z + ant_z + ant_d/2 + 1.5 <= base_h - gasket_groove,
       "antenna bore runs into the gasket groove's cheek — lower ant_z");
assert(!e_antenna || pcb_z + ant_z - ant_d/2 >= floor_t + 0.8, "antenna bore breaks into the floor — raise ant_z");
// the camera window at the disc's inner plane must pass the lens's field of view:
// lens front -> disc underside is the throw; half-angle tan(fov/2); +2 for the aperture itself
cam_throw = base_h + lid_t - (cam_disc_t > 0 ? cam_disc_t + 0.2 : 0) - (pcb_z + board_h + cam_lens_h);
cam_need  = 2*cam_throw*tan(cam_fov/2) + 2.0;
assert(!e_camera || cam_throw >= 0.5,
       "the lens front reaches the lid — raise stack_camera or shorten cam_lens_h");
assert(!e_camera || cam_win_d >= cam_need,
       str("camera window ", cam_win_d, " mm vignettes a ", cam_fov, "° lens ", cam_throw,
           " mm behind it — needs ", round(cam_need*10)/10, " mm (raise cam_win_d or cam_fov is optimistic)"));
assert(!e_battery || cav_h >= batt_h + 1.0, "battery bay taller than the cavity");
assert(!e_gps || cav_h >= gps_h + 1.0, "GPS bay taller than the cavity");
assert(base_h - (e_seal ? gasket_groove : 0) - (pcb_z + board_h + usb_h + usb_z) >= usb_web - 1e-6,
       "less than usb_web of wall above the USB opening");
echo(str("Canary WAP enclosure v0.8 — outer ", out_l, " x ", out_w, " x ",
         base_h + lid_t + mount_extra, " mm  (preset=", preset, ", seal=", e_seal, ", mount=", e_mount, ")"));
echo(str("lid screws: ", screw_size, " ", screw_head, " head, max length ",
         floor(lid_t + head_pad + cav_h - head_pad - 2.5), " mm (the pilot is blind 2 mm above the floor)",
         e_seal ? (head_seal ? " — O-ring glands under the heads" : " — NOTE: heads sit inside the gasket line; head_seal=true rings them") : ""));
if (e_seal && wall_eff > wall_t)
    echo(str("seal mode: walls auto-thickened ", wall_t, " -> ", wall_eff, " mm to host the gasket groove"));
if (mount_extra > 0 && kh_lock && !e_battery)
    echo("kh_lock: no battery bay, so no free floor for the anti-lift knockouts — skipped (use mount_style=\"tabs\" for a screwed install)");
assert(!(mount_extra > 0 && kh_lock && e_battery)
       || len([for (x = kh_xs) if (abs(batt_cx + 10 - x) < kh_slot_l/2 + kh_head_d/2 + 4) 1]) == 0,
       "anti-lift knockout lands on a keyhole pocket — move kh_inset");

// ----------------------------------------------------------------------------
//  Helpers — rrect2d/rrect come from canary_core_lib; only file-specific
//  geometry stays local
// ----------------------------------------------------------------------------
// ring traced on the wall midline — shared by the gasket groove and the gasket part
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_l + wall_eff, inner_w + wall_eff, max(0.1, corner_r - wall_eff/2));
        offset(r = -w/2) rrect2d(inner_l + wall_eff, inner_w + wall_eff, max(0.1, corner_r - wall_eff/2));
    }
}

// corner screw-post centers: 0.2 INTO each wall, so the post fuses to the walls
// above the gussets (at -0.2 it stood 0.2 off them — a slot the slicer either
// filled or left as a crack line through the corner)
function post_xy() = [
    [ inner_l/2 - pd/2 + 0.2,  inner_w/2 - pd/2 + 0.2],
    [-inner_l/2 + pd/2 - 0.2,  inner_w/2 - pd/2 + 0.2],
    [ inner_l/2 - pd/2 + 0.2, -inner_w/2 + pd/2 - 0.2],
    [-inner_l/2 + pd/2 - 0.2, -inner_w/2 + pd/2 - 0.2],
];
// does a floor rib from a to b (width w) run through a board clip's footprint?
// (the clip beam roots on the floor: a rib under it shortens its free length
// from 4.7 mm to about 1 mm and the insertion strain goes past 50 %)
function _clip_boxes() = board_clips
    ? [for (sy = [1, -1], cx = [board_cx - clip_dx, board_cx + clip_dx])
        [cx - clip_w/2, cx + clip_w/2,
         sy > 0 ? board_cy + board_w/2 + clip_clear - 0.6 : board_cy - board_w/2 - clip_clear - clip_t - 0.6,
         sy > 0 ? board_cy + board_w/2 + clip_clear + clip_t + 0.6 : board_cy - board_w/2 - clip_clear + 0.6]]
    : [];
function _rib_hits_clip(a, b, w) =
    len([for (bx = _clip_boxes(), i = [0 : 20])
            let (q = a + (b - a)*i/20)
            if (q[0] > bx[0] - w/2 && q[0] < bx[1] + w/2 && q[1] > bx[2] - w/2 && q[1] < bx[3] + w/2) 1]) > 0;

function _d2(a, b) = pow(a[0]-b[0], 2) + pow(a[1]-b[1], 2);

// solid web between two floor points at standoff height (a connecting rib)
module floorrib(a, b, w) {
    hull() {
        translate([a[0], a[1], floor_t]) cylinder(d = w, h = standoff_h);
        translate([b[0], b[1], floor_t]) cylinder(d = w, h = standoff_h);
    }
}

// transverse rib across the cavity floor (bounds a battery bay; walls cradle the sides),
// notched at the +Y wall so the battery leads route flat instead of climbing it.
// Drawn 0.3 INTO each wall: it used to stop 0.3 short, leaving a slot and an
// orphan stub beside the notch
module divrib(x) {
    difference() {
        translate([x - 0.6, -(inner_w + 0.6)/2, floor_t]) cube([1.2, inner_w + 0.6, 2.0]);
        if (batt_wire_w > 0)
            translate([x - 1.0, inner_w/2 - batt_wire_w, floor_t - 0.1])
                cube([2.0, batt_wire_w + 1.0, 2.4]);
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

// blind keyhole pocket cut into the thickened back (xc = feature center along X);
// head circle at the -X end, slot toward +X = UP when the case hangs USB-down.
// This file's pocket is canary_mount_lib's pattern piece — the library draws
// it natively along X, so the interface has one home and this mesh stays put
module keyhole_pocket(xc) {
    mount_keyhole_pocket(xc, -mount_extra, "x",
                         kh_head_d, kh_shank_d, kh_slot_l, kh_head_h, kh_face);
}

// peripheral wedge that 45°-chamfers the bottom edge (subtract from the shell);
// canary_core_lib owns the drawing — bounded to the footprint so external
// features (tabs) lose only a root nick
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
//  BASE
// ----------------------------------------------------------------------------
module base() {
    bx = board_l/2 - standoff_d/2;
    by = board_w/2 - standoff_d/2;
    corners = [ [board_cx+bx, board_cy+by], [board_cx+bx, board_cy-by],
                [board_cx-bx, board_cy-by], [board_cx-bx, board_cy+by] ];   // standoff/board-rest corners
    posts    = post_xy();
    gusset_h = max(2, cav_h - lip_h - 1.0);   // keep wall gussets below where the lid lip nests

    union() {
        // hollow shell with the USB-C wall opening (+ optional mounting/seal features)
        difference() {
            union() {
                rrect(out_l, out_w, corner_r, base_h);
                if (mount_extra > 0)                       // thickened back hosts the keyhole pockets
                    translate([0, 0, -mount_extra]) rrect(out_l, out_w, corner_r, mount_extra);
                if (e_mount && (mount_style == "tabs" || mount_style == "both"))
                    mount_tabs();
            }
            translate([0, 0, floor_t])
                rrect(inner_l, inner_w, max(0.1, corner_r - wall_eff), cav_h + 1);
            // USB opening: 45°-chamfered top corners halve the unsupported bridge in the
            // upright-printed wall and keep any droop out of the plug envelope — this
            // file's print-validated profile, now served by canary_port_lib to the
            // siblings that were still cutting flat-topped rectangles
            translate([-out_l/2 - wall_eff*1.5, board_cy, pcb_z + board_h + usb_h/2 + usb_z])
                rotate([90, 0, 90]) linear_extrude(wall_eff*3)
                    port_bridge_profile2d(usb_w, usb_h);
            // external antenna bulkhead hole on the far (+X) wall — a teardrop bore
            // (canary_core_lib), so the crown of a horizontal hole prints without sag
            if (e_antenna)
                tearbore_x(out_l/2 - 2*wall_eff, board_cy, pcb_z + ant_z, 4*wall_eff, ant_d);
            // perimeter gasket groove in the rim top (seal mode)
            if (e_seal)
                translate([0, 0, base_h - gasket_groove])
                    linear_extrude(gasket_groove + 1) rim_ring2d(gasket_w);
            // shallow recess framing the USB opening so a flanged silicone plug seats flush;
            // clamped below the gasket groove so the seal path keeps its outer cheek
            if (e_seal && usb_cover) {
                ur_bot = pcb_z + board_h + usb_z - usb_cov_pad;
                ur_top = min(pcb_z + board_h + usb_h + usb_z + usb_cov_pad,
                             base_h - gasket_groove - 0.5);
                translate([-out_l/2 - 1, board_cy - (usb_w/2 + usb_cov_pad), ur_bot])
                    cube([1 + usb_cov_dep, usb_w + 2*usb_cov_pad, ur_top - ur_bot]);
            }
            // blind keyhole pockets (never reach the cavity floor — seal-safe)
            if (mount_extra > 0)
                for (xc = kh_xs) keyhole_pocket(xc);
            // anti-lift knockouts: blind bores leaving a 0.6 mm web at the back face —
            // after hanging, pierce with M3 countersunk screws into the wall so the case
            // cannot be lifted off the keyholes. The web stays sealed until deliberately
            // used. The 90° head seat keeps the head flush under the battery bay.
            // Placed under the battery bay, where the floor is free; a case with no bay
            // has no floor a screw head can sit under (the board's clips own it), so
            // there they are skipped and the echo says so
            if (mount_extra > 0 && kh_lock && e_battery)
                for (sx = [1, -1]) translate([batt_cx + sx*10, -inner_w/2 + 5, 0]) {
                    translate([0, 0, -mount_extra + 0.6]) cylinder(d = 3.2, h = mount_extra + floor_t);
                    translate([0, 0, floor_t - 1.7]) cylinder(d1 = 3.2, d2 = 6.6, h = 1.71);  // 90° head seat, flush inside
                }
            // 45° bottom-edge chamfer: kills elephant-foot and the sharp first-layer
            // edge where impact delamination starts
            if (foot_cham > 0) foot_chamfer_cut();
            // weep: the case hangs USB-down, so the -X wall/floor corner is the
            // low point; the hole leaves the wall angled downward, beside the
            // USB opening (out of its plug recess) — canary_core_lib weep_cut
            if (e_weep)
                weep_cut(-inner_l/2, board_cy + usb_w/2 + usb_cov_pad + 2.5, floor_t + 0.75,
                         "-x", wall_eff);
        }
        // drip awning over the USB opening (sideways/desk use) — a solid 45°
        // wedge that prints with the wall; canary_core_lib port_hood
        if (usb_hood)
            translate([-out_l/2, board_cy, pcb_z + board_h + usb_h/2 + usb_z])
                rotate([0, -90, 0]) rotate([0, 0, 90]) port_hood(usb_w, usb_h, hood_reach, usb_h/2);

        // corner screw posts — fused to BOTH adjacent walls by gussets (no free-standing towers),
        // with self-tapping pilots
        difference() {
            union() {
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_h - head_pad);
                for (p = posts) {
                    sx = sign(p[0]); sy = sign(p[1]);
                    hull() {  // web to the X wall
                        translate([p[0], p[1], floor_t]) cylinder(d = pd, h = gusset_h);
                        translate([sx*(inner_l/2 - 0.3), p[1], floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                    hull() {  // web to the Y wall
                        translate([p[0], p[1], floor_t]) cylinder(d = pd, h = gusset_h);
                        translate([p[0], sy*(inner_w/2 - 0.3), floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                }
            }
            // self-tap pilot — or, with inserts, a clearance bore the whole way (a machine
            // screw longer than the insert must not bite below it and push the brass out)
            for (p = posts) translate([p[0], p[1], floor_t + 2.0])
                cylinder(d = screw_insert ? scr_nominal(screw_size) + 0.3 : scr_d, h = cav_h);
            // heat-set insert bore at the post top (0.3 interference; melt in flush)
            if (screw_insert)
                for (p = posts) translate([p[0], p[1], floor_t + cav_h - head_pad - ins_h - 0.5])
                    cylinder(d = ins_bore, h = ins_h + 1);
        }

        // board support: standoffs + a perimeter frame + ribs that tie it into the screw posts
        for (c = corners) translate([c[0], c[1], floor_t]) cylinder(d = standoff_d, h = standoff_h);
        for (i = [0:3]) floorrib(corners[i], corners[(i+1) % 4], 2.6);          // perimeter cradle frame
        for (c = corners) {
            // connect each standoff to the screw post in its quadrant, when reasonably
            // close — unless the rib would run under a board clip (its beam roots on
            // the floor, and a rib there is a beam 1 mm long, not 4.7)
            np = [ sign(c[0]) * (inner_l/2 - pd/2 + 0.2),
                   sign(c[1]) * (inner_w/2 - pd/2 + 0.2) ];
            if (_d2(c, np) <= 196 && !_rib_hits_clip(c, np, 2.6)) floorrib(c, np, 2.6);   // tie to post (<=14 mm)
            // short anchor ribs to nearby walls (helps mid-board standoffs in the battery variant)
            if (inner_l/2 - abs(c[0]) < 6) floorrib(c, [sign(c[0])*(inner_l/2-0.3), c[1]], 2.6);
            if (inner_w/2 - abs(c[1]) < 6) floorrib(c, [c[0], sign(c[1])*(inner_w/2-0.3)], 2.6);
        }

        // press-fit snap clips over the board's two long edges (no screws to hold the PCB)
        if (board_clips)
            for (sy = [1, -1])
                for (cx = [board_cx - clip_dx, board_cx + clip_dx])
                    boardclip(cx, sy);

        // battery: the snug side walls cradle the cell; two transverse ribs bound its length
        // (a full rim would self-delete here and the cell would foul the corner posts)
        if (e_battery) {
            divrib(batt_cx - batt_l/2 - 0.6);
            divrib(batt_cx + batt_l/2 + 0.6);
        }

        // GPS module cradle rim (narrow module, so a proper rim is fine).
        // A lead notch on the board side lets the wires exit at floor level
        // instead of cresting the rim and holding the module proud.
        // rim wall is a full 1.2 (it was clamped to the zone and came out 0.6 —
        // under the 0.8 web floor); with a battery the rim's -X wall overlaps the
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
}

// ----------------------------------------------------------------------------
//  LID
// ----------------------------------------------------------------------------
module vent_cluster(x, y) {
    translate([x, y, 0]) {
        // recessed seat for the adhesive GORE vent on the OUTER face
        translate([0, 0, lid_t - vent_pad_depth])
            cylinder(d = vent_pad_d, h = vent_pad_depth + 1);
        // ring of through-holes for sound + pressure equalization
        for (i = [0 : vent_holes - 1])
            rotate([0, 0, i * 360 / vent_holes])
                translate([vent_ring_d/2, 0, -1])
                    cylinder(d = vent_hole_d, h = lid_t + 2);
    }
}

// lid plate with a 45° chamfered top edge (prints face-down: the chamfer is a
// clean 45° outward slope off the bed — no supports). This lid's two-stage
// chamfer stack is the catalog's standard face-down edge treatment now, so
// canary_core_lib draws it
module lid_plate() {
    soft_edge_plate(plate_l, plate_w, plate_r, lid_t, lid_edge, lid_edge2);
}

module lid() {
    cam = [board_cx + cam_dx,   board_cy + cam_dy];
    lp  = [board_cx + lp_dx,    board_cy + lp_dy];
    vnt = [board_cx + vent_dx,  board_cy + vent_dy];
    mag = [board_cx + mag_dx,   board_cy + mag_dy];
    tch = [board_cx + touch_dx, board_cy + touch_dy];

    union() {
        difference() {
            union() {
                lid_plate();
                // pan-head pads: the web the counterbore needs, on the underside
                // (z < 0 is inside the case; the plate's cuts below are measured
                // from the pad's underside, so the seat floor lands 1.0 above it)
                if (head_pad > 0) for (p = post_xy())
                // pan-head pads: the floor under each head the front cannot
                // spare — CROPPED to the cavity (canary_core_lib). Drawn as a
                // bare cylinder the pad overhung its post and landed on the
                // shell wall rim, holding the front proud so the lip never
                // entered the cavity and the screws clamped nothing.
                    cb_head_pad(p[0], p[1], head_pad,
                                cb_pad_d(head_d, tol_hole),
                                inner_l, inner_w, core_cav_r(corner_r, wall_eff),
                                head_d + 2*tol_hole);
            }

            if (e_camera) {
                translate([cam[0], cam[1], -1]) cylinder(d = cam_win_d, h = lid_t + 2);  // camera window
                // recessed seat on the outer face for a glued clear disc (sits 0.2 below the surface)
                if (cam_disc_t > 0 && cam_disc_d > 0)
                    translate([cam[0], cam[1], lid_t - (cam_disc_t + 0.2)])
                        cylinder(d = cam_disc_d + 2*tol_slide, h = cam_disc_t + 1);
            }
            if (e_led)    translate([lp[0],  lp[1],  -1]) cylinder(d = lp_d + 2*tol_press, h = lid_t + 2);  // light pipe
            if (e_buzzer) vent_cluster(vnt[0], vnt[1]);                                                     // buzzer vent
            if (e_touch)  translate([tch[0], tch[1], -1]) cylinder(d = touch_d, h = lid_t - touch_wall + 1); // touch window (blind thinning)

            // lid screws over the posts — canary_core_lib's seats, chosen by the
            // head in the bag (screw_head), not taste: a 90° cone for FLAT heads,
            // a flat floor for PAN heads, and in seal mode with head_seal the
            // pan head squeezes an O-ring in a gland so the screw stops being
            // the one hole through the seal line. A pan seat that would breach
            // the plate cuts into the head_pad boss on the underside instead.
            for (p = post_xy()) translate([0, 0, -head_pad]) {
                if (screw_head == "flat")
                    cs_cone90_cut(p[0], p[1], lid_t, scr_c, head_h);
                else if (head_seal)
                    cb_oring_cut(p[0], p[1], lid_t + head_pad, scr_c,
                                 scr_oring_id(screw_size), scr_oring_cs(screw_size), head_d);
                else
                    cb_flat_cut(p[0], p[1], lid_t + head_pad, scr_c, head_d + 2*tol_hole, head_h);
            }

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

        // perimeter rib ring under the lid: raises the flat face's bending stiffness
        // (stiffness ~ t³) against pry/oil-canning for ~1 g of material. Routed just
        // inside the lip and cleared around every lid feature and the screw posts.
        if (lid_ribs) {
            ro_l = inner_l - 2*tol_slide - 2*lip_t - 0.8;
            ro_w = inner_w - 2*tol_slide - 2*lip_t - 0.8;
            difference() {
                translate([0, 0, -lid_rib_h]) linear_extrude(lid_rib_h + 0.1)
                    difference() {
                        rrect2d(ro_l, ro_w, max(0.1, corner_r - wall_eff - lip_t));
                        rrect2d(ro_l - 2*lid_rib_w, ro_w - 2*lid_rib_w, 0.1);
                    }
                for (p = post_xy())
                    translate([p[0], p[1], -lid_rib_h - 0.1]) cylinder(d = pd + 1.6, h = lid_rib_h + 0.2);
                // keep-outs so the ring never blocks a lid feature, wherever it is placed
                kos = [ [cam[0], cam[1], e_camera ? max(cam_win_d, cam_disc_d) + 3 : 0],
                        [lp[0],  lp[1],  e_led    ? lp_d + 4                     : 0],
                        [vnt[0], vnt[1], e_buzzer ? vent_pad_d + 3               : 0],
                        [tch[0], tch[1], e_touch  ? touch_d + 3                  : 0],
                        [mag[0], mag[1], e_tamper ? mag_d + 2*tol_press + 4.8    : 0] ];
                for (k = kos) if (k[2] > 0)
                    translate([k[0], k[1], -lid_rib_h - 0.1]) cylinder(d = k[2], h = lid_rib_h + 0.2);
                // keep the USB cable path over the lip notch clear
                translate([-inner_l/2, board_cy, 0]) cube([14, usb_w + 4, 3*lid_rib_h], center = true);
            }
        }

        // battery hold-down: two ribs on the lid underside over the bay, resting at
        // batt_h above the floor (the swelling allowance stays); shortened at the
        // +Y wall so the lead channel under them stays clear
        if (batt_hold && e_battery) {
            rib_h = cav_h - batt_h;
            for (dx = [-batt_l/4, batt_l/4])
                translate([batt_cx + dx - 0.8, -(batt_w - 2)/2 - 1.0, -rib_h])
                    cube([1.6, batt_w - 2 - batt_wire_w, rib_h + 0.1]);
        }

        // lid lip that nests into the base (with sliding clearance), cleared around the posts
        difference() {
            translate([0, 0, -lip_h])
                difference() {
                    rrect(inner_l - 2*tol_slide, inner_w - 2*tol_slide,
                          max(0.1, corner_r - wall_eff - tol_slide), lip_h);
                    rrect(inner_l - 2*tol_slide - 2*lip_t, inner_w - 2*tol_slide - 2*lip_t,
                          0.1, lip_h + 1);
                }
            for (p = post_xy())
                translate([p[0], p[1], -lip_h - 0.1])
                    cylinder(d = pd + 1.2, h = lip_h + 0.2);
            // notch the lip at the USB end so the cable plug/overmold clears it
            translate([-inner_l/2, board_cy, -lip_h/2])
                cube([lip_t * 4, usb_w + 4, lip_h + 0.2], center = true);
        }

        // drip-edge skirt (seal mode): overlaps the base wall so water can't sit on the seam
        if (e_seal)
            difference() {
                translate([0, 0, -skirt_h])
                    linear_extrude(skirt_h)
                        difference() {
                            rrect2d(plate_l, plate_w, plate_r);
                            rrect2d(out_l + 2*skirt_gap, out_w + 2*skirt_gap, corner_r + skirt_gap);
                        }
                // notch the skirt at the USB end (mirrors the lip notch; faces DOWN when wall-mounted)
                translate([-(out_l/2 + skirt_gap + skirt_t/2), board_cy, -skirt_h/2])
                    cube([skirt_t*3, usb_w + 6, skirt_h + 0.4], center = true);
            }

        // tamper magnet pocket (blind, opens downward; press fit — add a drop of glue)
        // the ring is embedded 0.1 into the plate so the export is one watertight shell
        if (e_tamper)
            translate([mag[0], mag[1], -mag_h])
                difference() {
                    cylinder(d = mag_d + 2*tol_press + 2.4, h = mag_h + 0.1);
                    translate([0, 0, -0.1]) cylinder(d = mag_d + 2*tol_press, h = mag_h + 0.1);
                }
    }
}

// ----------------------------------------------------------------------------
//  GASKET — TPU seal ring matching the base groove (seal mode).
//  Print in TPU 90–95A, 2 perimeters, 100 % infill. Squeezes ~20 % under the
//  screws; the ring is 0.5 narrower than the groove (~86 % fill) so the
//  incompressible TPU has somewhere to flow instead of propping the lid open.
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
        // the tie rib the case draws beside its -X clips, at the case's spacing: a
        // coupon that models the case must carry what the case carries
        translate([-cl/2 + 1.3, board_w/2 - standoff_d/2, floor_t]) cylinder(d = 2.6, h = standoff_h);
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
//  SOLAR RADIATION SHIELD — second roof on hollow standoffs over the lid.
//  Prints panel-on-bed, tubes up; installs FLIPPED (about X), so aperture
//  positions are mirrored in y here to land over the real lid features.
//  Fasten with the corner screws lengthened to M2 x 16-18.
// ----------------------------------------------------------------------------
module shield() {
    difference() {
        union() {
            rrect(out_l + 2*sh_over, out_w + 2*sh_over, corner_r + sh_over, sh_t);
            for (p = post_xy()) translate([p[0], p[1], 0]) cylinder(d = 7, h = sh_t + sh_gap);
        }
        for (p = post_xy()) {
            translate([p[0], p[1], -0.1]) cylinder(d = scr_d + 0.8, h = sh_t + sh_gap + 0.2);
            // head seat on the bed face = the installed top (first-layer void)
            translate([p[0], p[1], -0.01])
                cylinder(d1 = head_d + 0.6, d2 = scr_d + 0.8, h = head_h);
        }
        // apertures over lid features (y mirrored for the installation flip)
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

if      (part == "coupon") coupon();
else if (part == "shield") shield();
else if (part == "tray")   tray();
else if (part == "gasket") {
    assert(e_seal, "the gasket needs opt_seal=true (or preset=battery_weather) so its ring matches the groove");
    gasket();
}
else if (part == "base")   base();
else if (part == "lid")    translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();   // printable orientation
else {
    // the assembled preview wears the chosen colorway (registry: canary_color_lib);
    // color() is preview-only — a single-part STL export is byte-identical
    color(cw_body(colorway)) base();
    color(cw_body(colorway))
        translate([0, out_w/2 + plate_w/2 + 8, 0]) translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();
    if (e_seal) color(cw_light(colorway)) translate([0, -(out_w + 8), 0]) gasket();
}
