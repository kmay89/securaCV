// ============================================================================
//  Canary — VISION PRO MOUNT  ⚠️ IN DEVELOPMENT (v0.2-dev)
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
//
//  2026-10-23: v0.2-dev — the keyhole pair now runs IN LINE along its slots,
//  down the plate's centerline, as on every case back in the catalog. v0.1
//  cut the pockets side by side at (±12, 22) with vertical slots: no stud
//  surface pairs its studs that way, so on a vertical pair (the corner
//  wedge, the pole and magnet plates, the vent clip, the outlet cradle)
//  parking one pocket buried the other stud whole in the plate (70.35 mm³),
//  and turned 90° to fit, the slots ran level with nothing to latch them.
//  Set kh_gap to the stud surface's stud_gap (the adapters' 30 is the
//  default). The tripod and magnet move beside the keyhole column, below
//  its lower pocket, in every mount mode. The magnet pocket also gains the
//  press tolerance the adapters' magnet plate cuts: it was drawn at exactly
//  the disc's Ø20.0 (19.99 across the 120-gon's flats).
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
mag_t = 3.0;    // magnet pocket depth into the front face, so the disc glues in flush (asserted shallower than plate_t)

/* [Plate] */
plate_w = 50.0;    // plate width (X) — the keyhole pair, 2 x kh_x + kh_head_d across, must fit inside it (asserted)
plate_h = 64.0;    // plate height (Y) — the keyhole row must stay clear of the top edge (asserted); the tripod hole and the magnet pocket sit 16 below the center
plate_t = 7.0;     // plate thickness — more than the keyhole pocket depth, and than the tripod counterbore + 1.5 (both asserted); the magnet pocket is cut into its front face
plate_r = 4.0;     // corner rounding

/* [Stud/keyhole interface] — T-stud keyhole pockets, matching canary_mount_lib exactly */
kh_head_d  = 7.0;   // head pass hole (Ø6.6 stud head) — mount_kh_head_d()
kh_shank_d = 4.2;   // slot width (Ø4 stud stem) — mount_kh_shank_d()
kh_slot_l  = 8.0;   // slot travel — mount_kh_slot_l()
kh_head_h  = 3.5;   // total pocket depth (face web included) — mount_kh_head_h()
kh_face    = 1.0;   // face web the stud head grips behind — mount_kh_face()
kh_gap     = 30.0;  // keyhole pair spacing, IN LINE along the slots (vertical, as every case back) — set it to the stud surface's stud_gap (the asserts say when plate_h must grow)
kh_y       = 7.5;   // keyhole pair center, from the plate center toward the TOP edge

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary Vision Pro mount v0.2-dev — mount=", mount,
         "  (IN DEVELOPMENT — measure your screw/nut before printing!)"));
assert(plate_t > kh_head_h, "plate_t must exceed the keyhole pocket depth");
assert(plate_t > tripod_head_h + 1.5, "plate_t must clear the tripod counterbore + a solid wall");
assert(mag_t < plate_t, "mag_t must be shallower than plate_t");
// the upper pocket's head cavity (kh_head_d + 0.6 wide, mount_keyhole_pocket)
// keeps a structural wall to the top edge
assert(kh_y + kh_gap/2 + kh_slot_l/2 + (kh_head_d + 0.6)/2 + core_min_wall() <= plate_h/2,
       "upper keyhole too close to the top edge — lower kh_y or kh_gap, or raise plate_h");

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
// v0.2: + the catalog's press tolerance a side (the adapters' magnet plate cuts
// the same Ø20.2); at exactly mag_d it drew a hole the disc could not enter
module magnet_pocket(xc, yc) {
    translate([xc, yc, plate_t - mag_t]) cylinder(d = mag_d + 2*core_tol_press(), h = mag_t + 0.1);
}

// ---- layout (v0.2) ------------------------------------------------------------
// The keyholes sit on the centerline, in line along their slots. The device
// features sit BESIDE that column and below its lower pocket — tripod left,
// magnet right, in every mount mode: centered (x = 0), the tripod counterbore
// would cut into the lower pocket (both open on the back, 3.5 deep) and the
// magnet pocket's floor would stand 0.5 over its head cavity.
mag_r  = mag_d/2 + core_tol_press();        // the magnet pocket's radius
dev_x  = 12.0;                              // tripod at -dev_x, magnet at +dev_x
dev_y  = -plate_h/2 + mag_r + 1.4;          // both: 1.4 of wall under the magnet pocket (-20.5 at plate_h 64)
kh_lo  = kh_y - kh_gap/2 - kh_slot_l/2;     // the lower pocket's head-circle center (its low end)
kh_cav = (kh_head_d + 0.6)/2;               // its head cavity's radius (mount_keyhole_pocket)
// XY gap from a circle (x, y, r) to the lower pocket's head cavity (a stadium)
function kh_clear_to(x, y, r) = norm([x, y - min(max(y, kh_lo), kh_lo + kh_slot_l)]) - kh_cav - r;
// the magnet pocket is cut from the front, the keyhole from the back, so a web
// suffices between them; the tripod counterbore shares the keyhole's depth band
assert(kh_clear_to(dev_x, dev_y, mag_r) >= core_min_web(),
       "the magnet pocket crowds the lower keyhole — raise kh_y or plate_h, or lower kh_gap");
assert(kh_clear_to(-dev_x, dev_y, tripod_head_d/2) >= core_min_wall(),
       "the tripod counterbore cuts into the lower keyhole — raise kh_y or plate_h, or lower kh_gap");
assert(plate_h/2 + dev_y - mag_r >= core_min_wall() && plate_w/2 - dev_x - mag_r >= core_min_wall(),
       "the magnet pocket crowds the plate edge");

module plate() {
    difference() {
        linear_extrude(plate_t) rrect2d(plate_w, plate_h, plate_r);
        keyhole_pocket(0, kh_y + kh_gap/2);
        keyhole_pocket(0, kh_y - kh_gap/2);
        if (mount == "tripod" || mount == "both") tripod_hole(-dev_x, dev_y);
        if (mount == "magnet" || mount == "both") magnet_pocket( dev_x, dev_y);
    }
}

if (part == "plate") plate();
