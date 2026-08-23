// ============================================================================
//  Canary CORE library — the catalog's shared 2D/3D helpers and process floors
//
//  NOT A PRINTABLE PART. `use <canary_core_lib.scad>` from a case file.
//
//  WHY THIS FILE EXISTS. `rrect2d` was defined 26 times across this folder,
//  the two-stage soft edge 5 times, the teardrop bore twice, the counterbore
//  idiom in every case that screws shut — all copies, all drifting on their
//  own schedule. A copy is a fork: the pan-head counterbore lesson (a shallow
//  cone leaves the head standing on the show face) reached the Vision and the
//  doorbell and missed the Sense, the combo and the relay, because a lesson
//  applied to a copy stays in that copy. This library is the one home. The
//  modules are the SAME geometry the cases already draw — adopters must
//  render mesh-identical parts (the overhaul's hash gate proves it) — so
//  adopting the library is a dedup, never a redesign.
//
//  WHAT BELONGS HERE: geometry every case draws the same way, and the
//  process floors (minimum walls/webs) that make a drawing printable.
//  WHAT DOES NOT: anything with an interface contract of its own — the
//  stud/keyhole system (canary_mount_lib), snap fits (canary_snap_lib),
//  port cuts (canary_port_lib), board dimensions (canary_board_lib), the
//  bird (canary_mark_lib), the egg vents (canary_vent_lib), the wall dock
//  (canary_cradle_lib), measured display panels (canary_panel_lib).
//
//  `use<>` does not run top-level statements, so the load-time checks live
//  in core_selfcheck() — call it once from an adopter (the fit coupon does).
// ============================================================================

// ---------------------------------------------------------------------------
//  Process floors — FDM constants the whole catalog designs against.
//  One 0.4 mm nozzle extrusion is the atom; two make the thinnest wall that
//  is a wall and not a ribbon; five make the 2.0 mm catalog default that the
//  README's engineering section reasons from. Functions, not constants:
//  `use<>` carries functions across, so one number has one home.
// ---------------------------------------------------------------------------
function core_extrusion()  = 0.4;   // one nozzle line — the resolution floor
function core_min_web()    = 0.8;   // thinnest self-supporting web (2 lines)
function core_min_wall()   = 1.2;   // thinnest STRUCTURAL wall (3 lines)
function core_wall()       = 2.0;   // catalog default shell wall (5 lines)

// The catalog's print-tolerance trio, tuned once on canary_fit_coupon.scad.
// These are the DEFAULTS a new file starts from; each case exposes its own
// Customizer knobs so a printer can be dialed in, and the coupon is where
// that dialing happens. (0.25/0.12-class deviations belong in a file header
// with a reason, not in a quiet knob default.)
function core_tol_slide()  = 0.20;  // sliding fits: lid lip <-> base, skirts
function core_tol_press()  = 0.10;  // press fits: magnets, light pipes
function core_tol_hole()   = 0.30;  // clearance holes: lid screws

// ---------------------------------------------------------------------------
//  2D primitives
// ---------------------------------------------------------------------------
module rrect2d(l, w, r) {                        // rounded rectangle (2D)
    offset(r = r) offset(r = -r) square([l, w], center = true);
}

module rrect(l, w, r, h) {                       // rounded rectangular prism
    linear_extrude(height = h) rrect2d(l, w, r);
}

module pill2d(l, d) {                            // stadium: full-round ends
    hull() {
        translate([-(l - d)/2, 0]) circle(d = d);
        translate([ (l - d)/2, 0]) circle(d = d);
    }
}

// ---------------------------------------------------------------------------
//  Teardrop bore, horizontal axis — a round bore printed on its side sags at
//  the crown; the teardrop roof keeps every overhang at 45° or better. The
//  cap height is proportionally clamped so the polygon stays valid at ANY
//  bore size (the Vision hinge's version, promoted verbatim).
//  Axis along +X, length l, centered at (x0 = start, y, z).
// ---------------------------------------------------------------------------
module tearbore_x(x0, y, z, l, d) {
    r = d/2; cy = min(r + 0.75, r*1.38);
    translate([x0, y, z]) rotate([90, 0, 0]) rotate([0, 90, 0])
        linear_extrude(l) union() {
            circle(d = d);
            polygon([[-r*0.7071, r*0.7071], [-(r*1.4142 - cy), cy],
                     [ r*1.4142 - cy, cy], [ r*0.7071, r*0.7071]]);
        }
}

// ---------------------------------------------------------------------------
//  Screw seats — subtract from the show face at the screw center.
//  Two kinds, chosen by the HEAD IN THE BAG, not by taste:
//
//  cb_flat_cut — flat-bottom counterbore for PAN heads. The lesson (Vision,
//  print-validated; the doorbell carries it too): a shallow cone under a pan
//  head bears only on the cone lip and leaves the head standing on the show
//  face. A pan head wants a flat floor.
//
//  cs_cone90_cut — true 90° cone for FLAT (countersunk) heads: the WAP lid's
//  M2 flat-head seat. A flat head on a flat floor is just as wrong the other
//  way around.
//
//  Both cut through-hole + seat together; z0 = show-face z, cutting DOWN
//  into a plate of thickness t.
// ---------------------------------------------------------------------------
module cb_flat_cut(x, y, t, d_screw, d_head, h_head) {
    translate([x, y, -0.1])        cylinder(d = d_screw, h = t + 0.2);
    translate([x, y, t - h_head])  cylinder(d = d_head,  h = h_head + 0.1);
}

module cs_cone90_cut(x, y, t, d_screw, h_head) {
    translate([x, y, -0.1]) cylinder(d = d_screw, h = t + 0.2);
    translate([x, y, t - h_head])
        cylinder(d1 = d_screw, d2 = d_screw + 2*h_head, h = h_head + 0.1);
}

// ---------------------------------------------------------------------------
//  Two-stage soft edge — the WAP lid's chamfer stack, the catalog's standard
//  face-down edge treatment: one 45° stage plus an optional steeper cap
//  (~66°) that softens the chamfer toward a roundover. Both stages print
//  face-down without support. This is a TAPER by construction — the
//  footprint grows continuously — so it can never regress into the square
//  rabbet the C3/C6 "edge chamfer" turned out to be (an untapered cut that
//  drew an overhang on the show face; check_foot_relief.py hunts those).
//  Draws the full plate: footprint l x w, corner radius r, thickness t.
// ---------------------------------------------------------------------------
module soft_edge_plate(l, w, r, t, e1, e2 = 0) {
    assert(e1 == 0 || (e1 >= 0.01 && e1 < t),
           "soft_edge_plate: e1 must be 0, or between 0.01 and t");
    assert(e2 >= 0 && (e1 > 0 || e2 == 0) && e1 + e2 < t,
           "soft_edge_plate: e2 requires e1 > 0, and e1+e2 must stay below t");
    if (e1 > 0) union() {
        rrect(l, w, r, t - e1 - e2);
        hull() {
            translate([0, 0, t - e1 - e2]) rrect(l, w, r, 0.01);
            translate([0, 0, t - e2 - 0.01])
                rrect(l - 2*e1, w - 2*e1, max(0.1, r - e1), 0.01);
        }
        if (e2 > 0) hull() {
            translate([0, 0, t - e2])
                rrect(l - 2*e1, w - 2*e1, max(0.1, r - e1), 0.01);
            translate([0, 0, t - 0.01])
                rrect(l - 2*e1 - 0.9*e2, w - 2*e1 - 0.9*e2,
                      max(0.1, r - e1 - 0.45*e2), 0.01);
        }
    } else rrect(l, w, r, t);
}

// ---------------------------------------------------------------------------
//  Foot chamfer — peripheral wedge that 45°-chamfers a shell's bottom edge
//  (subtract from the shell): kills elephant-foot and blunts the sharp
//  first-layer corner where impact delamination starts. Bounded to the
//  footprint so external features (tabs, ears) lose only a root nick.
//  Footprint l x w, corner radius r, chamfer size cham, bottom face at z0.
// ---------------------------------------------------------------------------
module foot_chamfer_ring(l, w, r, cham, z0 = 0) {
    difference() {
        translate([0, 0, z0 - 0.01]) rrect(l + 0.04, w + 0.04, r, cham + 0.01);
        hull() {
            translate([0, 0, z0])
                rrect(l - 2*cham, w - 2*cham, max(0.1, r - cham), 0.01);
            translate([0, 0, z0 + cham]) rrect(l, w, r, 0.01);
        }
    }
}

// ---------------------------------------------------------------------------
//  Self-check — geometric identities the modules above promise. Call once
//  from an adopter (the fit coupon does); `use<>` does not run top-level
//  statements, so a bare `use` cannot run these for you.
// ---------------------------------------------------------------------------
module core_selfcheck() {
    // tolerance compares: 3 * 0.4 is not bit-equal to 1.2 in floats
    assert(abs(core_min_web()  - 2*core_extrusion()) < 1e-9, "core: web floor is two extrusions");
    assert(abs(core_min_wall() - 3*core_extrusion()) < 1e-9, "core: wall floor is three extrusions");
    assert(abs(core_wall()     - 5*core_extrusion()) < 1e-9, "core: default wall is five extrusions");
    assert(core_tol_slide() > core_tol_press(),
           "core: a sliding fit must be looser than a press fit");
    assert(core_tol_hole() > core_tol_slide(),
           "core: a clearance hole must be looser than a sliding fit");
    echo("canary_core_lib: self-check OK");
}
