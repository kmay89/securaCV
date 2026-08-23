// ============================================================================
//  SecuraCV Canary Watch — monitoring-station puck  ⚠️ IN DEVELOPMENT (v0.2-dev)
//  Hardware: Seeed Round Display for XIAO (1.28" 240x240 GC9A01 touch,
//  Ø43.0 mm PCB disc — the "39 mm" in marketing copy is the glass, not the
//  board) with a XIAO ESP32-S3 pinned into its BACK SOCKET: the XIAO rides
//  the display's own two 7-pin headers, component/USB side facing the drum
//  floor, USB-C pointing radially out through the drum's side slot.
//  See docs/hardware/display_research.md for the selection rationale.
//
//  v0.2 REDESIGN — measured against the vendor CAD (the committed board GLB,
//  canary-local/boards/seeed_round_display_xiao.glb) and cross-checked against
//  Seeed's own printable case for this display (Ø48 body, Ø44 bore, snap ring):
//    · disc_d 39 → 43.0. The v0.1 drum was designed around the marketing
//      "39 mm disc"; the real PCB is Ø43.0 with back parts sweeping Ø43.9 —
//      it could never seat. Bore and bezel now fit the measured board.
//    · screwed bezel → SNAP bezel. The display's back parts fill the bore
//      wall-to-wall (Ø43.9 envelope), so internal screw posts cannot exist
//      at any sane drum diameter. The bezel is now a snap ring: a skirt
//      drops into the bore over the disc edge and four nubs click into
//      wall slots — same architecture as Seeed's reference case.
//    · the USB slot moved from the rim down to the XIAO's actual level
//      (the XIAO hangs BELOW the display socket, USB shell toward the
//      floor — the old rim slot pointed at the display glass).
//    · the STAND is a true cradle: a full-depth cylindrical divot bored
//      normal to the 25°-reclined face — the drum sinks pocket_dep into it,
//      with thumb scallops for removal, a chin slot passing the USB cable,
//      and an open under-base channel routing the cable out the back.
//  All parts print flat, no supports.
//
//  2026-08-23: adopted the shared contract libraries (core/mount/board).
//    One deliberate fix rides along, so the DRUM is NOT mesh-neutral: the
//    keyhole had drifted (kh_slot_l 7.0, kh_head_h 3.0) — a slide the
//    standard stud's head never finishes and a pocket it bottoms out in
//    0.4 early. Now the catalog standard 8.0 / 3.5, cut through
//    mount_keyhole_pocket on its native axis; back_t + kh_extra = 5.0
//    keeps the pocket blind at exactly head_h + 1.5. Bezel and stand
//    geometry unchanged.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. Measure
//     your display disc (disc_d) and XIAO USB position before printing.
// ============================================================================

use <canary_core_lib.scad>   // rrect + the catalog tolerance trio the knobs cite
use <canary_mount_lib.scad>  // the stud/keyhole hanging standard — the drum's blind pocket
use <canary_board_lib.scad>  // board registry — the measured round_disp record disc_d cites

/* [What to render] */
part = "all";        // ["drum","bezel","stand","all"]

/* [Display stack] — Round Display for XIAO + XIAO in its back socket.
   Defaults measured from the committed vendor GLB (mm). */
disc_d    = 43.0;    // display PCB disc diameter — brd_l("round_disp"), measured (canary_board_lib)
disc_t    = 5.0;     // disc pack: PCB back face → front trim ring (measured)
disp_back = 5.0;     // back-side stack: sockets / SD / RTC below the PCB (measured)
xiao_t    = 4.4;     // XIAO seated proud of the socket: PCB + USB shell (measured)
xiao_gap  = 1.2;     // clearance under the XIAO's USB shell (boot-button room)
bore_clear = 0.7;    // bore radial clearance over disc_d (back parts sweep Ø43.9)
usb_ang   = 270;     // XIAO USB direction, degrees (0 = +X; 270 = -Y = chin/front in the stand)
usb_w     = 11.0;    // side-slot width (USB-C plug shells run ~10.5)
usb_h     = 7.0;

/* [Battery] — optional LiPo laid on the drum floor (display has JST + charger) */
opt_batt = false;
batt_l = 30.0;  batt_w = 20.0;  batt_h = 3.4;   // 302030-class cell — MEASURE

/* [Puck] */
wall_t   = 2.3;      // drum wall
back_t   = 2.5;      // drum back plate
bez_t    = 2.2;      // bezel face plate thickness
bez_ap_d = 39.4;     // face aperture — shows the full Ø37.7 glass, covers the PCB rim
flush    = 0.4;      // disc front ring sits this far below the drum rim
tilt     = 25;       // stand recline from vertical  // [15:5:35]

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide() (canary_core_lib)
tol_press = 0.10;    // catalog default — core_tol_press() (canary_core_lib)

/* [Snap bezel] */
snap_n       = 4;     // nubs / wall slots
snap_w       = 6.5;   // slot width (arc chord)
snap_h       = 1.8;   // slot height
snap_depth   = 2.6;   // slot center below the drum rim
snap_proud   = 0.4;   // nub stand-proud of the skirt (with the relief slits this is
                      // the working snap interference — 0.6 needed crush to insert)
skirt_dep    = 4.0;   // bezel skirt reach into the bore

/* [Mounting] */
kh_head_d  = 7.0;    // blind keyhole pocket in the drum back (wall mount) — catalog standard, canary_mount_lib
kh_shank_d = 4.2;    // catalog standard — mount_kh_shank_d()
kh_slot_l  = 8.0;    // catalog standard — mount_kh_slot_l() (a drifted 7.0 lived here: the head never finished its slide)
kh_head_h  = 3.5;    // catalog standard — mount_kh_head_h() (a drifted 3.0 lived here: the stud bottomed out 0.4 early)
kh_face    = 1.0;    // catalog standard — mount_kh_face()
kh_extra   = 2.5;    // drum back thickening hosting the pocket; back_t + kh_extra covers head_h + the 1.5 blind web

/* [Stand cradle] */
pocket_dep  = 11.0;  // how deep the drum sinks into the divot (buries the USB slot)
pocket_clear = 0.4;  // radial clearance around the drum barrel
rim_margin  = 5.0;   // face margin around the pocket circle
chin_w      = 14.0;  // USB/cable chin-slot width
scallop_d   = 18.0;  // thumb scallops for lifting the drum out
foot_h      = 1.5;   // foot pads (base cable channel runs beneath)

/* [Aesthetics] */
lid_edge  = 1.0;     // bezel face chamfer
label_text = "";
label_size = 4.0;  label_depth = 0.5;  label_font = "Liberation Sans:style=Bold";

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived — the axial stack, from the drum back (z = 0)
// ----------------------------------------------------------------------------
bore_d  = disc_d + 2*bore_clear;             // Ø44.4 — passes the Ø43.9 envelope
drum_d  = bore_d + 2*wall_t;                 // Ø49.0
floor_z = back_t + kh_extra;                 // 5.0 — inner floor
gap_eff = opt_batt ? batt_h + 1.0 : xiao_gap;
z_xiao0 = floor_z + gap_eff;                 // XIAO USB-shell face (toward floor)
z_sock  = z_xiao0 + xiao_t;                  // display socket underside
z_pcb   = z_sock + disp_back;                // display PCB back face
drum_h  = z_pcb + disc_t + flush;            // rim — disc front ring 0.4 below it
usb_cz  = z_xiao0 + (xiao_t - 1.2 - 1.6);    // USB shell center (PCB 1.2, shell/2 1.6)

skirt_od = bore_d - 2*tol_press - 0.1;       // bezel skirt slides into the bore
puck_len = drum_h + bez_t;                   // full seated length, back cap → face

assert(bez_ap_d < disc_d - 3, "aperture must land on the display's trim ring");
assert(bez_ap_d < skirt_od - 3, "skirt wall too thin — shrink bez_ap_d");
assert(kh_head_h + 1.5 <= back_t + kh_extra, "keyhole pocket must stay blind (head_h + 1.5 web, canary_mount_lib) — raise kh_extra");
assert(usb_cz - usb_h/2 > 1.0, "USB slot digs into the drum back — raise xiao_gap");
assert(usb_cz + usb_h/2 < z_pcb, "USB slot reaches the display PCB — check stack");
assert(!opt_batt || sqrt(pow(batt_l/2,2) + pow(batt_w/2 + 2,2)) < bore_d/2, "battery too large for the bore");
echo(str("Canary Watch station v0.2-dev — drum Ø", drum_d, " x ", puck_len,
         " mm (bore Ø", bore_d, ", USB slot z ", usb_cz, "), stand tilt ", tilt, " deg"));

// ----------------------------------------------------------------------------
//  DRUM — straight cup, prints open-face-up; keyhole pocket in the back;
//  snap slots near the rim; USB slot at the XIAO's measured level
// ----------------------------------------------------------------------------
// the four snap windows, mid-wall between the USB slot (270°) and keyhole (90°)
function snap_angs() = [45, 135, 225, 315];
module drum() {
    difference() {
        cylinder(d = drum_d, h = drum_h);
        // bore — smooth wall to wall, nothing protrudes (the display's back
        // parts sweep Ø43.9; internal posts are impossible with this board)
        translate([0, 0, floor_z]) cylinder(d = bore_d, h = drum_h);
        // XIAO USB-C slot through the wall at the measured shell height
        rotate([0, 0, usb_ang]) translate([drum_d/2 - wall_t/2, 0, usb_cz])
            cube([wall_t*3, usb_w, usb_h], center = true);
        // snap windows for the bezel nubs
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
        translate([0, 0, drum_h - 0.6]) cylinder(d1 = bore_d, d2 = bore_d + 1.2, h = 0.61);
    }
    // LiPo fence rails on the drum floor (cell strapped with foam tape)
    if (opt_batt) for (s2 = [1, -1])
        translate([-batt_l/2, s2*(batt_w/2 + tol_slide + 1.0) - 1.0, floor_z - 0.01])
            cube([batt_l, 2.0, 2.0]);
}

// ----------------------------------------------------------------------------
//  BEZEL — snap ring, prints face-down: face plate on the rim, skirt drops
//  into the bore over the disc edge, nubs click into the wall slots
// ----------------------------------------------------------------------------
module bezel() {
    difference() {
        union() {
            // face plate with edge chamfer
            cylinder(d = drum_d, h = bez_t - lid_edge);
            translate([0, 0, bez_t - lid_edge - 0.01])
                cylinder(d1 = drum_d, d2 = drum_d - 2*lid_edge, h = lid_edge);
            // retaining skirt — captures the disc with 0.2 mm nominal float over
            // the PCB rim (it does NOT preload the trim ring). Slit into four
            // fingers midway between the nubs: a closed ring has no compliance
            // path, so the nubs would shave instead of snapping.
            translate([0, 0, -skirt_dep]) difference() {
                cylinder(d = skirt_od, h = skirt_dep + 0.01);
                translate([0, 0, -0.1]) cylinder(d = bez_ap_d, h = skirt_dep + 0.2);
                for (a = snap_angs()) rotate([0, 0, a + 45])
                    translate([skirt_od/2 - 3, -0.6, -0.1]) cube([6, 1.2, skirt_dep - 0.9]);
            }
            // snap nubs, chamfered both ways (assembly AND service removal).
            // The face underside (bezel z=0) rests on the drum rim (drum z=drum_h),
            // so drum_z = drum_h + bezel_z; the drum's slot center is at
            // drum_h − snap_depth, hence the nub sits at bezel z = −snap_depth.
            for (a = snap_angs()) rotate([0, 0, a]) {
                nz = -snap_depth;             // slot center, bezel frame
                translate([skirt_od/2 - 0.5, 0, nz]) hull() {
                    translate([0, -snap_w/2 + 1.2, 0]) cube([0.5, 0.1, snap_h - 0.4], center = true);
                    translate([snap_proud + 0.5, 0, 0]) cube([0.5, 0.1, 0.6], center = true);
                    translate([0, snap_w/2 - 1.2, 0]) cube([0.5, 0.1, snap_h - 0.4], center = true);
                }
            }
        }
        // face aperture + lead-in — shows the full glass, covers the PCB rim
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
//  STAND — the cradle. A cylindrical divot bored NORMAL to the 25°-reclined
//  face: the drum sinks pocket_dep into it and cannot rock or roll. Thumb
//  scallops free it; the chin slot passes the USB cable straight down into
//  an open channel under the base and out the back. Prints upright.
// ----------------------------------------------------------------------------
pkt_d  = drum_d + 2*pocket_clear;                       // divot bore
slope  = 90 - tilt;                                     // face angle from horizontal
// unit vectors (stand frame): a = pocket/drum axis (out of the face),
// u = up-the-face direction
function vec_a() = [0, -sin(slope), cos(slope)];        // (0, −0.906, 0.423) @ 25°
function vec_u() = [0,  cos(slope), sin(slope)];        // (0,  0.423, 0.906) @ 25°

chin   = [0, -20, 4];                                   // lowest face point
s_ctr  = rim_margin + 1 + pkt_d/2;                      // chin → pocket center, along the face
// pocket center on the face plane, and the drum's back-cap seat point
C_rim  = [for (i = [0:2]) chin[i] + vec_u()[i] * s_ctr];
P0     = [for (i = [0:2]) C_rim[i] - vec_a()[i] * pocket_dep];
s_top  = s_ctr + pkt_d/2 + rim_margin;                  // chin → face top edge
sw     = drum_d + 14;                                   // stand width
sh     = chin[2] + vec_u()[2] * s_top + 2;              // height to cover the face
sd_f   = -chin[1] + 2;                                  // front reach of the base
sd_b   = 30;                                            // rear reach

echo(str("Stand cradle — pocket Ø", pkt_d, " x ", pocket_dep,
         " deep; DRUM SEAT pos [", P0[0], ", ", P0[1], ", ", P0[2],
         "] rot [", slope, ", 0, 0] (drum +z along the pocket axis)"));

module stand_body() {
    hull() {
        translate([0, (sd_b - sd_f)/2, 0]) rrect(sw, sd_f + sd_b, 8, 5);   // base slab
        translate([0, sd_b - 11, 0]) rrect(sw, 20, 8, sh);                 // rear column
    }
}
module stand() {
    difference() {
        stand_body();
        // the reclined face: shear everything in front of the plane through
        // `chin` with normal a — the pocket rim lands as a clean circle on it
        translate(chin) rotate([slope, 0, 0])
            translate([-sw, -sh - 20, 0]) cube([2*sw, 2*(sh + 20), sh + 40]);
        // the divot — bored along a from the seat plane; generous overrun
        translate(P0) rotate([slope, 0, 0]) {
            cylinder(d = pkt_d, h = pocket_dep + 30);
            // rim chamfer (lead-in for the drum)
            translate([0, 0, pocket_dep - 0.01]) cylinder(d1 = pkt_d, d2 = pkt_d + 2.4, h = 1.2 + 0.02);
            // chin slot: cable drops from the drum's USB straight down the
            // face and under the base (floor flush with the pocket floor)
            translate([-chin_w/2, -pkt_d/2 - 60, 0]) cube([chin_w, pkt_d/2 + 60 - (pkt_d/2 - 6), 60]);
            // thumb scallops at 3 and 9 o'clock, biting the rim by ~7 mm
            for (sx = [1, -1]) translate([sx * (pkt_d/2 + scallop_d/2 - 7), 0, pocket_dep - 6])
                cylinder(d = scallop_d, h = 40);
        }
        // open cable channel under the base, chin to the rear edge
        translate([-6, -sd_f - 1, -0.1]) cube([12, sd_f + sd_b + 2, 4 + 0.1]);
    }
    // foot pads lift the base over the cable channel
    for (fx = [1, -1], fy = [1, -1])
        translate([fx*(sw/2 - 9), fy == 1 ? sd_b - 9 : -(sd_f - 9), -foot_h])
            cylinder(d = 12, h = foot_h + 0.01);
}

// ----------------------------------------------------------------------------
if      (part == "drum")  drum();
else if (part == "bezel") bezel_print();
else if (part == "stand") stand();
else {
    drum();
    translate([drum_d + 12, 0, 0]) bezel_print();
    translate([-(drum_d + 34), 0, 0]) stand();
}
