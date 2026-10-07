// ============================================================================
//  SecuraCV Canary Watch — monitoring-station puck  ⚠️ IN DEVELOPMENT (v0.3-dev)
//  Hardware: Seeed Round Display for XIAO (1.28" 240x240 GC9A01 touch,
//  Ø43.0 mm disc — PCB and cover glass alike) with a XIAO ESP32-S3 pinned
//  into its BACK SOCKET: the XIAO rides the display's own two 7-pin headers,
//  component/USB side facing the drum floor, USB-C pointing radially out
//  through the drum's side slot. See docs/hardware/display_research.md for
//  the selection rationale.
//
//  v0.3 — THE FIRST PRINT'S FINDINGS (2026-10, calipers on the bench unit):
//    · the GLASS is the whole disc. v0.2 assumed a Ø37.7 glass on a Ø43 PCB
//      and sized the bezel's skirt to drop over the PCB rim (ID 42.5) — the
//      43.01 glass could not enter it, so the "ring face" never seated. The
//      bezel now carries a GLASS POCKET (the skirt's own bore, disc_d +
//      2*glass_clear) and a 1.5 mm LIP over the glass edge (bez_ap_d 40.0,
//      still 3.8 a side clear of the GC9A01's Ø32.4 pixels); the drum gains
//      a COUNTERBORE at its rim so the skirt rides around the glass, and the
//      drum grows to Ø50.5 to keep its 2.3 wall behind the snap windows.
//    · the STACK is 18.44, not 14.4. Glass front → XIAO USB-shell face reads
//      18.44 on the bench unit (the vendor GLB draws a 5.0 socket; the real
//      one is the 8.5 mm female-header standard plus seating). v0.2's drum
//      left the glass 3.6 mm proud of the rim. The manifest now owns the
//      caliper number as stack_t, and the socket zone (disp_back) is the
//      remainder — derived, never retyped.
//    · the board FLOATED. Nothing located the disc axially: the XIAO's shell
//      hovered 1.2 over the floor and the bezel skirt was a radial fit only,
//      so a drop drove the whole stack into the bezel. Three SEAT PADS now
//      rise from the floor to the PCB's back face in the sectors the vendor
//      GLB leaves clear of the display's back parts, the bezel lip holds the
//      glass front against them (glass_float), and an END-STOP boss under
//      the XIAO's USB shell limits its travel to stop_gap so its pins cannot
//      leave the socket in a drop.
//    · the USB cut-out was a chamfered rectangle, 8.6 tall around a 3.3 shell
//      at a guessed height. It is now the catalog's STADIUM, the spec-max
//      overmold + 0.4 a side, centered on the shell the measured stack puts
//      it at (usb_cz) — and the bridge rule holds by construction (the flat
//      between the round ends is w − h, under the 7.0 ceiling).
//    · the STAND was a block. A 63 x 52 x 55 hull with a divot bored into
//      it: half a spool for a desk puck. It is now an OPEN CRADLE: a 220°
//      saddle around the drum's barrel on a reclined seat plate, a push-out
//      window through the plate (a teardrop, so it prints), a sculpted spine
//      to the base, the chin pocket for a 90° USB-C lead and the under-base
//      channel kept. No scallops — the open top lifts the puck straight out.
//  All parts print flat, no supports.
//
//  2026-08-23: adopted the shared contract libraries (core/mount/board).
//    The keyhole is the catalog standard 8.0 / 3.5, cut through
//    mount_keyhole_pocket on its native axis; back_t + kh_extra = 5.0 keeps
//    the pocket blind at exactly head_h + 1.5.
//
//  ⚠️ DEV STATUS: one print of v0.2 measured, v0.3 render/mesh-verified only.
//     MEASURE your display stack (stack_t) and the USB position before
//     printing; the MEASURE tags mark the two numbers only a bench can give.
// ============================================================================

use <canary_core_lib.scad>   // rrect, soft_edge_plate + the catalog tolerance trio the knobs cite
use <canary_mount_lib.scad>  // the stud/keyhole hanging standard — the drum's blind pocket
use <canary_snap_lib.scad>   // the finger strain budget, and the window derived from its ridge
use <canary_port_lib.scad>   // the USB-C stadium the drum cuts, the bridge profile the base channel cuts
use <canary_board_lib.scad>  // board registry — disc_d, the XIAO's outline and socket offset, the GC9A01 active area

/* [What to render] */
part = "all";        // ["drum","bezel","stand","all"]

/* [Display stack] — Round Display for XIAO + XIAO in its back socket.
   The whole pinned stack was measured on the bench (mm). */
disc_d    = 43.0;    // display disc diameter, PCB and cover glass — brd_l("round_disp"), measured (canary_board_lib)
disc_t    = 5.0;     // disc pack: PCB back face → glass front (measured; the vendor GLB agrees)
stack_t   = 18.44;   // MEASURED: glass front → XIAO USB-shell face, the whole pinned stack (calipers, 2026-10)
xiao_t    = 4.4;     // XIAO seated proud of the socket: PCB + USB shell (measured)
xiao_gap  = 1.2;     // clearance under the XIAO's USB shell (boot-button room); the end-stop boss stands in it
stop_gap  = 0.4;     // end-stop boss → USB-shell face: how far the XIAO may travel in a drop before the shell lands
bore_clear = 0.7;    // bore radial clearance over disc_d (the back parts sweep Ø43.2; v0.2's print passed the disc at this)
glass_clear = 0.5;   // bezel glass pocket, radial clearance over the Ø43.0 glass (the skirt's fingers flex inward snap_proud)
glass_float = 0.1;   // axial float between the bezel lip and the glass front, the disc seated on its pads
usb_ang   = 270;     // XIAO USB direction, degrees (0 = +X; 270 = -Y = chin/front in the stand)
usb_proud = 1.2;     // MEASURE: the XIAO's USB-C receptacle stands this far past the board edge
stop_len  = 6.0;     // end-stop boss reach, inward from the receptacle face: under the shell, short of the XIAO's buttons
stop_w    = 10.0;    // end-stop boss width: the 8.94 shell plus a millimeter of XIAO misalignment

/* [Battery] — optional LiPo laid on the drum floor (display has JST + charger) */
opt_batt = false;
batt_l = 30.0;       // cell length — 302030-class cell, MEASURE
batt_w = 20.0;       // cell width — 302030-class cell, MEASURE
batt_h = 3.4;        // cell thickness — 302030-class cell, MEASURE

/* [Puck] */
wall_t   = 2.3;      // deviates: the bezel's snap fingers seat through this drum wall — tuned as a set with snap_depth/finger_t
back_t   = 2.5;      // drum back plate
bez_t    = 2.2;      // bezel face plate thickness
bez_ap_d = 40.0;     // face aperture: the lip covers 1.5 mm of the Ø43.0 glass edge, 3.8 a side clear of the Ø32.4 active area
tilt     = 25;       // stand recline from vertical  // [15:5:35]

/* [Seat pads] — three ribs from the floor to the PCB's back face, in the
   sectors the vendor GLB leaves clear of the display's back parts */
seat_w    = 4.0;     // pad chord width
seat_bite = 2.3;     // pad reach under the PCB rim, inward from the disc edge (the back parts start 0.2 further in)

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide() (canary_core_lib)
tol_press = 0.10;    // catalog default — core_tol_press() (canary_core_lib)

/* [Snap bezel] */
snap_n       = 4;     // nubs / wall slots
pry_notch    = true;  // a fingernail notch in the drum rim at pry_ang (mid-wall between two snap windows,
                      // away from the USB and keyhole): the bezel snaps flush Ø-for-Ø with nothing to lift it by
pry_ang      = 0;     // degrees, 0 = +X  // [0:15:345]
nub_w        = 4.2;   // the nub ridge's drawn width (arc chord) — exactly what the old end plates drew
snap_play    = 0.15;  // window clearance per side — the catalog default (canary_snap_lib)
snap_h       = 1.8;   // slot height
snap_depth   = 3.3;   // slot center below the drum rim: low on the finger, where its lever is longest
snap_proud   = 0.25;  // nub stand-proud of the skirt: 0.1 of working interference over the counterbore
                      // (0.4 needed 0.25 of finger travel — 9 % strain on the 2.35 mm skirt; 0.3 on a
                      // 1.0 finger at the real 1.9 lever was 6.2 %)
skirt_dep    = 4.0;   // bezel skirt reach into the counterbore (the counterbore is cut 0.3 deeper so the tip floats)
finger_t     = 0.8;   // finger thickness — the skirt is relieved to this behind the nubs so the
                      // fingers flex (a 2.35 wall did not); the snap lib's cycle budget gates it
finger_root  = 0.7;   // full-thickness band under the face plate the fingers hang from — their lever runs root → nub;
                      // its underside is the LIP that lands on the glass edge

/* [Stud/keyhole interface] — a blind keyhole pocket in the drum back */
kh_head_d  = 7.0;    // blind keyhole pocket in the drum back (wall mount) — catalog standard, canary_mount_lib
kh_shank_d = 4.2;    // catalog standard — mount_kh_shank_d()
kh_slot_l  = 8.0;    // catalog standard — mount_kh_slot_l()
kh_head_h  = 3.5;    // catalog standard — mount_kh_head_h()
kh_face    = 1.0;    // catalog standard — mount_kh_face()
kh_extra   = 2.5;    // drum back thickening hosting the pocket; back_t + kh_extra covers head_h + the 1.5 blind web

/* [Stand cradle] — an open saddle on a reclined seat plate */
pocket_dep  = 13.0;  // how deep the drum sits in the saddle (the USB slot inside the saddle's reach)
pocket_clear = 0.4;  // radial clearance around the drum barrel
cup_t       = 2.4;   // saddle wall, radial
cup_open    = 140;   // the saddle's open top, degrees of arc centered straight up the face  // [100:10:180]
seat_t      = 2.4;   // seat plate behind the drum's back cap
window_r    = 12.0;  // push-out window through the seat plate (a teardrop, point up the face, so it prints)
spine_t     = 6.0;   // the back spine's thickness
spine_w_top = 14.0;  // spine width where it meets the seat plate
spine_w_base = 24.0; // spine width where it meets the base
spine_rake  = 30;    // spine lean from vertical: its underside is this overhang, printed base-down  // [20:5:45]
spine_rise  = 6.0;   // where the spine meets the seat plate, up the face from the drum axis
base_t      = 5.0;   // base plate; the cable channel bridges inside it
foot_margin = 8.0;   // base plate in front of the saddle's lowest point
chin_w      = 14.0;  // chin pocket width: a 90° USB-C lead's elbow, 12.35 spec-max + play

/* [Aesthetics] */
lid_edge  = 1.0;     // deviates: scaled to the round bezel — the drum face carries a wider single stage
label_text = "";
label_size = 4.0;  label_depth = 0.5;  label_font = "Liberation Sans:style=Bold";

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived — the axial stack, from the drum back (z = 0)
// ----------------------------------------------------------------------------
disp_back = stack_t - disc_t - xiao_t;       // the socket zone: what the measured stack leaves (9.04; the GLB draws 5.0)
bore_d  = disc_d + 2*bore_clear;             // Ø44.4 — passes the Ø43.2 envelope; locates the disc radially
floor_z = back_t + kh_extra;                 // 5.0 — inner floor
gap_eff = opt_batt ? batt_h + 1.0 : xiao_gap;
z_xiao0 = floor_z + gap_eff;                 // XIAO USB-shell face (toward floor)
z_sock  = z_xiao0 + xiao_t;                  // display socket underside
z_pcb   = z_sock + disp_back;                // display PCB back face — the seat pads' top
flush   = finger_root + glass_float;         // glass front below the drum rim: under the lip, with its float
drum_h  = z_pcb + disc_t + flush;            // rim
usb_cz  = z_xiao0 + (xiao_t - 1.2 - 1.6);    // USB shell center (PCB 1.2, shell/2 1.6)
usb_slot_w = port_usbc_overmold_w() + 2*0.4; // the stadium: spec-max overmold + 0.4 a side
usb_slot_h = port_usbc_overmold_h() + 2*0.4;

skirt_od = disc_d + 2*(glass_clear + finger_t);  // the skirt around the glass pocket
cb_d     = skirt_od + 2*tol_press + 0.1;         // rim counterbore the skirt slides into
cb_dep   = skirt_dep + 0.3;                      // ...cut 0.3 deeper than the skirt reaches
drum_d   = cb_d + 2*wall_t;                      // Ø50.5
puck_len = drum_h + bez_t;                       // full seated length, back cap → face

// the XIAO in the socket: centered brd_round_disp_socket_x() off the disc
// center toward usb_ang, its USB edge and receptacle face from there
xiao_edge_r = brd_round_disp_socket_x() + brd_l("xiao")/2;
usb_face_r  = xiao_edge_r + usb_proud;
seat_r_in   = disc_d/2 - seat_bite;
// the three seat sectors, from the USB azimuth: 180 / −55 / +60. In the
// vendor GLB (raw: socket rows toward +X, which the pose puts at usb_ang)
// the back parts that reach past r 19 sit at usb_ang+90 (the display's
// flex), usb_ang±25 (the socket rows' outer ends), usb_ang−127 and −144
// (two edge blocks), usb_ang+144 (a small part) and usb_ang−90 (a block
// at the far edge) — these three fall between them with ≥ 10° to spare
function seat_angs() = [for (o = [180, -55, 60]) usb_ang + o];

assert(bez_ap_d <= disc_d - 2*1.0, "the bezel lip must cover at least 1.0 mm of glass edge — shrink bez_ap_d");
assert(bez_ap_d >= brd_round_disp_active_d() + 2*1.0, "the aperture would cover pixels (GC9A01 active area, canary_board_lib) — grow bez_ap_d");
assert(bez_ap_d < skirt_od - 2*finger_t - 2, "skirt wall too thin — shrink bez_ap_d");
assert(disp_back >= 5.0, "the measured stack leaves less socket than the vendor CAD draws (5.0) — re-measure stack_t");
assert(kh_head_h + 1.5 <= back_t + kh_extra, "keyhole pocket must stay blind (head_h + 1.5 web, canary_mount_lib) — raise kh_extra");
assert(usb_cz - usb_slot_h/2 > 1.0, "USB slot digs into the drum back — raise xiao_gap");
assert(usb_cz + usb_slot_h/2 < z_pcb, "USB slot reaches the display PCB — check stack");
assert(usb_slot_w - usb_slot_h <= port_flat_span_max() + 1e-9, "the USB stadium's flat top is over the bridge ceiling (canary_port_lib)");
assert(usb_face_r < bore_d/2 - 0.5, "the XIAO's receptacle lands in the drum wall — check brd_round_disp_socket_x / usb_proud");
assert(port_usbc_insertion() > drum_d/2 - usb_face_r, "a plug's shell cannot reach the receptacle through the drum wall");
assert(seat_bite > 0 && seat_bite <= 2.5, "a seat pad reaches under the display's back parts (they start 2.5 in from the disc edge in the free sectors) — shrink seat_bite");
assert(snap_depth + snap_h/2 < cb_dep, "a snap window reaches below the counterbore into the thick wall — shrink snap_depth");
assert(!opt_batt || sqrt(pow(batt_l/2,2) + pow(batt_w/2 + 2,2)) < bore_d/2, "battery too large for the bore");
echo(str("Canary Watch station v0.3-dev — drum Ø", drum_d, " x ", puck_len,
         " mm (bore Ø", bore_d, ", counterbore Ø", cb_d, " x ", cb_dep, ", seat z ", z_pcb,
         ", USB slot z ", usb_cz, " ", usb_slot_w, " x ", usb_slot_h, "), stand tilt ", tilt, " deg"));

// ----------------------------------------------------------------------------
//  DRUM — straight cup, prints open-face-up; keyhole pocket in the back;
//  a rim counterbore for the bezel skirt with the snap windows in it; seat
//  pads to the PCB; the USB stadium at the XIAO's measured level
// ----------------------------------------------------------------------------
// the four snap windows, mid-wall between the USB slot (270°) and keyhole (90°)
function snap_angs() = [45, 135, 225, 315];
// the window derives from the ridge that parks in it (canary_snap_lib's rule)
snap_w = snap_window(nub_w, snap_play);
// the bezel's fingers are a snap worked at every service: the lib's CYCLE
// budget, on the finger's real numbers — thickness finger_t, travel = the
// nub's interference over the counterbore, and the lever from the finger's
// ROOT to the NUB (the root sits at bezel z = −finger_root, the nub at
// −snap_depth)
snap_defl  = skirt_od/2 + snap_proud - cb_d/2;
snap_lever = snap_depth - finger_root;
assert(snap_defl > 0, "the nubs do not reach the counterbore — raise snap_proud");
assert(snap_defl < glass_clear, "a finger's snap travel would press the glass — grow glass_clear or shrink snap_proud");
assert(snap_depth + (snap_h - 0.4)/2 <= skirt_dep + 1e-9, "the nub hangs off the finger's tip — shrink snap_depth or snap_h");
assert(snap_strain(finger_t, snap_defl, snap_lever) <= snap_budget_cycle(),
       str("bezel fingers strain ", round(snap_strain(finger_t, snap_defl, snap_lever)*1000)/10,
           " % — over the ", round(snap_budget_cycle()*1000)/10, " % cycle budget: thin finger_t, shrink snap_proud, or drop the nub lower on the finger (snap_depth)"));
assert(!pry_notch || min([for (a = snap_angs()) abs(((pry_ang - a + 540) % 360) - 180)]) >= 20,
       "pry_ang lands on a snap window — keep it 20 degrees off snap_angs()");
assert(!pry_notch || abs(((pry_ang - usb_ang + 540) % 360) - 180) >= 25, "pry_ang lands on the USB slot");
assert(drum_h - cb_dep > z_pcb, "the counterbore step lands below the PCB's back face — check the stack");
assert(min([for (s = seat_angs()) for (a = snap_angs()) abs(((s - a + 540) % 360) - 180)]) >= 0
       && min([for (s = seat_angs()) abs(((s - usb_ang + 540) % 360) - 180)]) >= 30,
       "a seat pad lands on the USB slot");
module drum() {
    union() {
        difference() {
            cylinder(d = drum_d, h = drum_h);
            // bore — smooth wall to wall below the counterbore: the display's
            // back parts sweep Ø43.2; this is what locates the disc radially
            translate([0, 0, floor_z]) cylinder(d = bore_d, h = drum_h);
            // rim counterbore: the bezel skirt rides here, around the glass,
            // and the snap windows cut its wall. The step down to the bore
            // is a 45° funnel so the disc finds the bore — facing up, no
            // overhang in the open-face-up print
            translate([0, 0, drum_h - cb_dep]) cylinder(d = cb_d, h = cb_dep + 1);
            translate([0, 0, drum_h - cb_dep - (cb_d - bore_d)/2]) cylinder(d1 = bore_d, d2 = cb_d, h = (cb_d - bore_d)/2 + 0.01);
            // XIAO USB-C stadium through the wall at the measured shell
            // height — the catalog profile: round ends, a flat under the 7.0
            // bridge ceiling, so the open-face-up print needs no chamfer
            rotate([0, 0, usb_ang]) translate([bore_d/2 - 1, 0, usb_cz])
                rotate([90, 0, 90]) linear_extrude(drum_d)
                    port_usbc_stadium2d(usb_slot_w, usb_slot_h);
            // snap windows for the bezel nubs, through the counterbore wall
            for (a = snap_angs()) rotate([0, 0, a])
                translate([drum_d/2 - wall_t/2, 0, drum_h - snap_depth])
                    cube([wall_t*3, snap_w, snap_h], center = true);
            // blind keyhole pocket (single, center 6 up from the drum axis, slot
            // toward drum top so the puck slides DOWN to seat) for stand-less
            // wall mount — the catalog pocket, on its native axis
            mount_keyhole_pocket(6, 0, axis = "y",
                head_d = kh_head_d, shank_d = kh_shank_d,
                slot_l = kh_slot_l, head_h = kh_head_h, face = kh_face);
            // bottom-edge chamfer
            difference() {
                translate([0, 0, -0.01]) cylinder(d = drum_d + 0.1, h = 0.5);
                translate([0, 0, -0.02]) cylinder(d1 = drum_d - 1.0, d2 = drum_d + 0.12, h = 0.53);
            }
            // rim lead-in — the disc and the bezel skirt both enter here
            translate([0, 0, drum_h - 0.6]) cylinder(d1 = cb_d, d2 = cb_d + 1.2, h = 0.61);
            // pry notch: 8 wide, 0.6 into the rim's outer edge, 0.8 down
            if (pry_notch) rotate([0, 0, pry_ang])
                translate([drum_d/2, 0, drum_h]) cube([2*0.6, 8, 2*0.8], center = true);
        }
        // SEAT PADS — the disc's axial datum. The PCB's back face rests on
        // their tops at z_pcb; the bezel lip holds the glass front against
        // them, so the stack is captured both ways. They grow out of the bore
        // wall (1 mm buried) and reach seat_bite under the PCB rim
        for (s = seat_angs()) rotate([0, 0, s])
            translate([seat_r_in, -seat_w/2, floor_z - 0.01])
                cube([bore_d/2 - seat_r_in + 1.0, seat_w, z_pcb - floor_z + 0.01]);
        // END-STOP boss under the XIAO's USB shell: the shell (the stiffest
        // thing on the board) lands on it after stop_gap of travel, so a drop
        // cannot pull the XIAO's pins out of the socket. Short of the wall,
        // short of the buttons, inside the plug's path only above z_xiao0
        rotate([0, 0, usb_ang])
            translate([usb_face_r - stop_len, -stop_w/2, floor_z - 0.01])
                cube([stop_len, stop_w, gap_eff - stop_gap + 0.01]);
        // LiPo fence rails on the drum floor (cell strapped with foam tape)
        if (opt_batt) for (s2 = [1, -1])
            translate([-batt_l/2, s2*(batt_w/2 + tol_slide + 1.0) - 1.0, floor_z - 0.01])
                cube([batt_l, 2.0, 2.0]);
    }
}

// ----------------------------------------------------------------------------
//  BEZEL — snap ring, prints face-down: face plate on the rim, the skirt
//  drops into the counterbore AROUND the glass (its bore is the glass
//  pocket), the root band's underside is the lip on the glass edge, and
//  the nubs click into the wall slots
// ----------------------------------------------------------------------------
module bezel() {
    difference() {
        union() {
            // face plate with edge chamfer
            cylinder(d = drum_d, h = bez_t - lid_edge);
            translate([0, 0, bez_t - lid_edge - 0.01])
                cylinder(d1 = drum_d, d2 = drum_d - 2*lid_edge, h = lid_edge);
            // skirt: the glass pocket's wall, slit into four fingers midway
            // between the nubs (a closed ring has no compliance path, so the
            // nubs would shave instead of snapping). The relief leaves the
            // fingers finger_t thick from the tip up to finger_root under the
            // face plate; the full band above it, from the aperture out to
            // the skirt, is the LIP that lands on the glass edge
            translate([0, 0, -skirt_dep]) difference() {
                cylinder(d = skirt_od, h = skirt_dep + 0.01);
                translate([0, 0, -0.1]) cylinder(d = bez_ap_d, h = skirt_dep + 0.2);
                translate([0, 0, -0.1]) cylinder(d = skirt_od - 2*finger_t, h = skirt_dep - finger_root + 0.1);
                for (a = snap_angs()) rotate([0, 0, a + 45])
                    translate([skirt_od/2 - 3, -0.6, -0.1]) cube([6, 1.2, skirt_dep - finger_root + 0.1]);
            }
            // snap nubs, chamfered both ways (assembly AND service removal).
            // The face underside (bezel z=0) rests on the drum rim (drum z=drum_h),
            // so drum_z = drum_h + bezel_z; the drum's slot center is at
            // drum_h − snap_depth, hence the nub sits at bezel z = −snap_depth.
            for (a = snap_angs()) rotate([0, 0, a]) {
                nz = -snap_depth;             // slot center, bezel frame
                translate([skirt_od/2 - 0.5, 0, nz]) hull() {
                    translate([0, -nub_w/2 + 0.05, 0]) cube([0.5, 0.1, snap_h - 0.4], center = true);
                    // tip cube is 0.5 wide: its OUTER face lands at skirt_od/2 + snap_proud,
                    // so the strain assert's snap_defl is the travel the finger really makes
                    translate([snap_proud + 0.25, 0, 0]) cube([0.5, 0.1, 0.6], center = true);
                    translate([0, nub_w/2 - 0.05, 0]) cube([0.5, 0.1, snap_h - 0.4], center = true);
                }
            }
        }
        // face aperture + lead-in — the glass shows through, its edge under the lip
        translate([0, 0, -skirt_dep - 1]) cylinder(d = bez_ap_d, h = skirt_dep + bez_t + 2);
        translate([0, 0, bez_t - 0.5]) cylinder(d1 = bez_ap_d, d2 = bez_ap_d + 1.2, h = 0.51);
        if (label_text != "")
            translate([0, -drum_d/2 + 4, bez_t - label_depth]) linear_extrude(label_depth + 1)
                text(label_text, size = label_size, font = label_font, halign = "center", valign = "center");
    }
}
// print orientation: face-down on the bed
module bezel_print() { translate([0, 0, bez_t]) rotate([180, 0, 0]) bezel(); }

// ----------------------------------------------------------------------------
//  STAND — the open cradle. A 220° saddle around the drum's barrel stands
//  on a seat plate reclined `tilt` from vertical: the drum's back cap sits
//  on the plate, its barrel in the saddle, pocket_dep deep, and the open
//  top lifts it straight out. A teardrop window through the plate lets a
//  finger push the puck out from behind. The chin pocket takes a 90°
//  (up/down-angle) USB-C lead, elbow pointing back, into the open channel
//  under the base and out the rear; a straight plug cannot mate in the
//  cradle — its body would meet the desk. The saddle's bottom is fused into
//  the base through a saddle foot (what fills the barrel's lower 90° of
//  overhang), and a sculpted spine takes the lean from the seat plate to the
//  base's rear. Prints base-down, no supports: the seat plate is a `tilt`
//  overhang, the spine a spine_rake one, the saddle's open top never bridges.
// ----------------------------------------------------------------------------
pkt_d  = drum_d + 2*pocket_clear;                       // saddle bore
cup_od = pkt_d + 2*cup_t;                               // saddle outside
slope  = 90 - tilt;                                     // face angle from horizontal
// unit vectors (stand frame): a = pocket/drum axis (out of the face),
// u = up-the-face direction
function vec_a() = [0, -sin(slope), cos(slope)];        // (0, −0.906, 0.423) @ 25°
function vec_u() = [0,  cos(slope), sin(slope)];        // (0,  0.423, 0.906) @ 25°
function at(p, s_u, s_a) = [for (i = [0:2]) p[i] + vec_u()[i]*s_u + vec_a()[i]*s_a];

bury   = 1.0;                                           // the saddle's lowest point sinks this far into the base
// the drum's back-cap seat point: the saddle's lowest, rearmost outer point
// sits on the base plane (bury deep), with the stand's y origin there
P0     = at([0, 0, base_t - bury], cup_od/2, seat_t);
C_rim  = at(P0, 0, pocket_dep);                         // the drum rim's center
front_low = at(P0, -cup_od/2, pocket_dep);              // the saddle's front-bottom outer point
sd_f   = -front_low[1] + foot_margin;                   // front reach of the base
// the spine: a bar across the seat plate's back, spine_rise up the face from
// the drum axis, hulled to a bar on the base's rear. Its rake sets where the
// base ends
sp_top = at(P0, spine_rise, -seat_t - spine_t/2 + 0.3);
sp_bot = [0, sp_top[1] + (sp_top[2] - spine_t/2) * tan(spine_rake), spine_t/2];
sd_b   = sp_bot[1] + spine_t/2 + 2;                     // rear reach of the base
sw     = cup_od + 4;                                    // stand width
stand_h = at(P0, cup_od/2, 0)[2];                       // the seat plate's top

assert(cup_open < 180, "the saddle must wrap more than half the barrel to hold it");
assert(base_t - 1.0 >= 4.0, "the base channel needs 4 mm under a 1 mm web — raise base_t");
assert(front_low[2] > base_t, "the saddle's front rim sinks into the base — the recline or pocket_dep moved it");
echo(str("Stand cradle — saddle Ø", pkt_d, " x ", pocket_dep,
         " deep, open ", cup_open, " deg; base ", sw, " x ", sd_f + sd_b, " x ", base_t,
         ", ", stand_h, " tall; DRUM SEAT pos [", P0[0], ", ", P0[1], ", ", P0[2],
         "] rot [", slope, ", 0, 0] (drum +z along the pocket axis)"));

module in_pocket() { translate(P0) rotate([slope, 0, 0]) children(); }
module xbar(c, w, d) { translate(c) rotate([0, 90, 0]) cylinder(d = d, h = w, center = true); }

module stand_body() {
    // the saddle and the seat plate behind it
    in_pocket() {
        translate([0, 0, -seat_t]) cylinder(d = cup_od, h = seat_t + pocket_dep);
    }
    // the saddle foot: the barrel's lower 90° of outer surface (what would
    // overhang past 45°) hulled down onto the base
    hull() {
        in_pocket() intersection() {
            translate([0, 0, -seat_t]) cylinder(d = cup_od, h = seat_t + pocket_dep);
            translate([-cup_od, -cup_od, -seat_t - 1]) cube([2*cup_od, cup_od - cup_od/2*cos(45), seat_t + pocket_dep + 2]);
        }
        translate([-cup_od/2*sin(45), front_low[1] - 1, base_t - bury])
            cube([cup_od*sin(45), -front_low[1] + 1 + 2, 0.5]);
    }
    // the spine, seat plate to base
    hull() {
        xbar(sp_top, spine_w_top, spine_t);
        xbar(sp_bot, spine_w_base, spine_t);
    }
    // the base plate, soft-edged
    translate([0, (sd_b - sd_f)/2, 0]) soft_edge_plate(sw, sd_f + sd_b, 8, base_t, 0.8);
}
module stand() {
    difference() {
        stand_body();
        in_pocket() {
            // the pocket — the drum's barrel, from the seat plane out
            cylinder(d = pkt_d, h = pocket_dep + 60);
            // rim lead-in for the drum
            translate([0, 0, pocket_dep - 0.01]) cylinder(d1 = pkt_d, d2 = pkt_d + 2.4, h = 1.2 + 0.02);
            // the open top: the saddle's upper cup_open degrees, ahead of the seat plane only
            linear_extrude(pocket_dep + 60)
                polygon([[0, 0], for (t = [90 - cup_open/2 : 2 : 90 + cup_open/2]) 2*cup_od*[cos(t), sin(t)]]);
            // the push-out window: a teardrop, point up the face, through the seat plate
            translate([0, 0, -seat_t - spine_t - 1]) linear_extrude(seat_t + spine_t + 2)
                hull() { circle(r = window_r); translate([0, window_r*sqrt(2) - 0.01]) square(0.02, center = true); }
            // chin pocket: the 90° lead's elbow drops from the drum's USB
            // straight down the face through the saddle wall and the foot,
            // from the seat plane out past the rim lead-in (so the puck
            // drops in with the lead attached), elbow turning back
            translate([-chin_w/2, -pkt_d/2 - 8, 0]) cube([chin_w, 8 + 6, pocket_dep + 1.3]);
        }
        // ...and a vertical slot through the base under the whole chin line
        // (the saddle's lowest outer line runs from the seat plane, buried at
        // y = 0, to front_low), into the channel below
        translate([-chin_w/2, front_low[1] - 5, -1]) cube([chin_w, -front_low[1] + 5 + 3, base_t + 2]);
        // open cable channel under the base, chin to the rear edge. Its
        // ceiling is a bridge (the stand prints base-down), so it takes the
        // catalog's chamfered-top profile: 12 wide at the floor, 7.0 of flat
        translate([0, sd_b + 1, (base_t - 1 - 0.1)/2]) rotate([90, 0, 0])
            linear_extrude(sd_f + sd_b + 2) port_bridge_profile2d(12, base_t - 1 + 0.1);
    }
}

// ----------------------------------------------------------------------------
if      (part == "drum")  drum();
else if (part == "bezel") bezel_print();
else if (part == "stand") stand();
else {
    drum();
    translate([drum_d + 12, 0, 0]) bezel_print();
    translate([-(drum_d + 36), 0, 0]) stand();
}
