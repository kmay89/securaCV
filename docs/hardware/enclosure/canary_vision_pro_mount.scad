// ============================================================================
//  Canary — VISION PRO MOUNT  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Bridges a Seeed reCamera Pro (or any reCamera-family unit) onto the
//  shared T-stud wall ecosystem. The BACK carries two BLIND KEYHOLE
//  POCKETS — identical geometry to every Canary case's back
//  (canary_mount_lib.scad's mount_keyhole_pocket()) — so this plate hangs
//  on any existing stud surface already in the catalog: a bare wall
//  bracket, canary_mount_adapters.scad's corner/magnet/pole plates, or
//  canary_vehicle_mount.scad's dash plate. The FRONT carries the
//  camera's own confirmed mounting interface — reCamera's "magnetic and
//  1/4" thread mount" (Seeed reCamera hardware wiki; see
//  docs/hardware/canary_vision_pro_recamera.md) — as a 1/4"-20 tripod
//  screw counterbore and/or a magnet disc pocket.
//
//  ⚠️ No confirmed reCamera Pro BODY dimensions exist in this repo yet —
//  no bench unit has been measured. This plate only carries the MOUNTING
//  interface, not a body-hugging shell. The 1/4"-20 screw geometry is a
//  universal camera-tripod standard (safe to model without guessing
//  vendor dimensions); MEASURE your actual screw/washer stack before
//  printing if it differs from the defaults below.
//
//  Aiming/tilt is deliberately NOT built into this plate — pair it with
//  whichever host surface already provides angle (a tilted bracket, the
//  vehicle dash's riser, canary_mount_adapters.scad's corner wedge).
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print- or bench-validated.
//  See docs/hardware/canary_vision_pro_recamera.md for the integration
//  story this hardware serves (webhook sensor adapter, no new firmware).
// ============================================================================

use <canary_core_lib.scad>   // shared 2D primitives — rrect2d has one home now
use <canary_mount_lib.scad>  // the stud/keyhole standard this file hangs on

/* [What to render] */
part = "plate";       // ["plate"]

/* [Device-facing interface] — reCamera's confirmed mount options */
mount = "both";        // ["tripod","magnet","both"]

/* [Tripod interface — 1/4"-20 UNC, universal camera-mount standard] */
tripod_shaft_d = 6.6;  // clearance for a 1/4"-20 screw shank (6.35 mm major dia)
tripod_head_d  = 12.0; // counterbore for the screw head — MEASURE your screw
tripod_head_h  = 3.5;  // counterbore depth

/* [Magnet interface — matches canary_mount_adapters.scad's magnet plate] */
mag_d = 20.0;   // Ø20 x 3 neodymium disc pocket
mag_t = 3.0;

/* [Plate] */
plate_w = 50.0;
plate_h = 64.0;
plate_t = 7.0;
plate_r = 4.0;     // corner rounding

/* [T-stud keyhole interface — matches canary_mount_lib exactly] */
kh_head_d  = 7.0;   // head pass hole (Ø6.6 stud head) — mount_kh_head_d()
kh_shank_d = 4.2;   // slot width (Ø4 stud stem) — mount_kh_shank_d()
kh_slot_l  = 8.0;   // slot travel — mount_kh_slot_l()
kh_head_h  = 3.5;   // total pocket depth (face web included) — mount_kh_head_h()
kh_face    = 1.0;   // face web the stud head grips behind — mount_kh_face()
kh_x       = 12.0;  // keyhole pair spacing (± from center)
kh_y       = 22.0;  // keyhole row offset from center, toward the TOP edge

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary Vision Pro mount v0.1-dev — mount=", mount,
         "  (IN DEVELOPMENT — measure your screw/nut before printing!)"));
assert(plate_t > kh_head_h, "plate_t must exceed the keyhole pocket depth");
assert(plate_t > tripod_head_h + 1.5, "plate_t must clear the tripod counterbore + a solid wall");
assert(mag_t < plate_t, "mag_t must be shallower than plate_t");
assert(kh_x * 2 + kh_head_d < plate_w, "keyhole pair too wide for plate_w");
assert(kh_y + kh_slot_l/2 + kh_head_d/2 < plate_h/2, "keyhole row too close to the top edge");

// Blind keyhole pocket, opening on the BACK face (z=0) and cutting +Z into
// the plate — canary_mount_lib's pocket on its native Y axis (slot vertical,
// so the plate slides DOWN onto a stud pair to lock — same convention as
// every wall-hung Canary case).
module keyhole_pocket(xc, yc) {
    translate([xc, 0, 0])
        mount_keyhole_pocket(yc, 0, axis = "y",
                             head_d = kh_head_d, shank_d = kh_shank_d,
                             slot_l = kh_slot_l, head_h = kh_head_h,
                             face = kh_face);
}

// Tripod screw: shank clears straight through; the head seats in a
// counterbore on the BACK (away from the camera) so the threaded tip
// protrudes from the FRONT to catch the camera's 1/4"-20 socket.
module tripod_hole(xc, yc) {
    translate([xc, yc, -0.1]) {
        cylinder(d = tripod_shaft_d, h = plate_t + 0.2);
        cylinder(d = tripod_head_d, h = tripod_head_h + 0.1);
    }
}

// Magnet disc pocket, recessed into the FRONT face (the disc glues in
// flush and mates with the camera's own magnetic mount).
module magnet_pocket(xc, yc) {
    translate([xc, yc, plate_t - mag_t]) cylinder(d = mag_d, h = mag_t + 0.1);
}

module plate() {
    tripod_x = (mount == "both") ? -12 : 0;
    magnet_x = (mount == "both") ?  12 : 0;
    difference() {
        linear_extrude(plate_t) rrect2d(plate_w, plate_h, plate_r);
        keyhole_pocket(-kh_x, kh_y);
        keyhole_pocket( kh_x, kh_y);
        if (mount == "tripod" || mount == "both") tripod_hole(tripod_x, -16);
        if (mount == "magnet" || mount == "both") magnet_pocket(magnet_x, -16);
    }
}

if (part == "plate") plate();
