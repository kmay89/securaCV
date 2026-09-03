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
//  Screw registry — ONE table for the fasteners the catalog closes with.
//  Every case used to carry its own screw_d / screw_head_d / screw_head_h
//  trio, all typed for M2 and all silently wrong the day someone wanted an
//  M2.5 or M3 (a Ø1.6 pilot under an M3 self-tapper splits a Ø5 post). A
//  case now exposes `screw_size` and READS the numbers here; the M2 row is
//  the catalog's print-validated set, so a case whose knobs still say M2
//  renders the same mesh it always did.
//
//  Columns: [id, nominal, self-tap PILOT (PETG), CLEARANCE hole,
//            pan head Ø, pan seat depth, flat head Ø, flat (90°) seat depth,
//            heat-set insert BORE, insert length, O-ring [ID, section]]
//    pilot     — 0.8 x nominal, the bite that neither strips nor splits
//                (the catalog's 1.6 for M2 is exactly this rule)
//    clearance — ISO 273 fine series
//    pan       — ISO 7045 dk, seat = head k + 0.4 sink (flush, never proud)
//    flat      — ISO 7046 dk, seat = head k (a 90° cone seats a flat head
//                on its cone, not on the floor — cs_cone90_cut draws it)
//    insert    — the BORE (not the knurl OD): CNC Kitchen / Ruthex short
//                series, ~0.3 mm under the knurl so the brass bites
//    o-ring    — a standard ring whose ID rides the shank: the head-seal
//                gland (cb_oring_cut) sizes itself from this
// ---------------------------------------------------------------------------
SCR_REGISTRY = [
    ["m2",   2.0, 1.6, 2.2, 4.0, 2.0, 4.0, 1.2, 3.2, 4.0, [2.0, 1.0]],
    ["m2.5", 2.5, 2.0, 2.8, 5.0, 2.4, 4.7, 1.5, 3.6, 5.7, [2.5, 1.0]],
    ["m3",   3.0, 2.5, 3.4, 5.6, 2.4, 6.0, 1.65, 4.0, 5.7, [3.0, 1.0]],
];
function _scr_find(id, i = 0) =
    i >= len(SCR_REGISTRY)
        ? assert(false, str("screw registry: unknown size \"", id,
                            "\" — the catalog knows m2, m2.5 and m3")) undef
        : SCR_REGISTRY[i][0] == id ? SCR_REGISTRY[i] : _scr_find(id, i + 1);
function scr_nominal(id)  = _scr_find(id)[1];
function scr_pilot(id)    = _scr_find(id)[2];
function scr_clear(id)    = _scr_find(id)[3];
function scr_pan_d(id)    = _scr_find(id)[4];
function scr_pan_h(id)    = _scr_find(id)[5];
function scr_flat_d(id)   = _scr_find(id)[6];
function scr_flat_h(id)   = _scr_find(id)[7];
function scr_insert_d(id) = _scr_find(id)[8];
function scr_insert_h(id) = _scr_find(id)[9];
function scr_oring_id(id) = _scr_find(id)[10][0];
function scr_oring_cs(id) = _scr_find(id)[10][1];
// the thinnest post that carries a self-tapped pilot: 1.5 mm of wall each
// side of the pilot (the catalog's Ø5 post around the Ø1.6 M2 pilot has 1.7)
function scr_post_min(id) = scr_pilot(id) + 3.0;

// ---------------------------------------------------------------------------
//  O-ring head seal — the gland that makes a lid screw stop being a leak.
//  Every sealed Canary puts its corner screws INSIDE the gasket line (the
//  posts stand in the cavity corners), so water on the show face runs down
//  the screw thread onto the post top and into the cavity. A gasket
//  cannot fix that; a ring under the head can. This cuts a flat-floored
//  gland around the through-hole: a standard O-ring (ID = shank, section
//  cs) sits in it and the head squeezes it ~25 % — the standard face-seal
//  squeeze — against the floor. The gland is sized so the ring's OD is
//  captured (radial constraint) and the head still clears the gland wall.
//  Use PAN heads with it (a flat head's cone would push the ring out).
//  z0 = show face; cuts DOWN into a plate of thickness t.
//    cs    — ring section;  id_r — ring inner diameter
//    head  — the screw head's Ø (asserted to cover ≥ 85 % of the section)
// ---------------------------------------------------------------------------
function oring_gland_d(id_r, cs)  = id_r + 2*cs + 0.3;     // ring OD + radial room
function oring_gland_h(cs)        = 0.75*cs;               // 25 % squeeze
module cb_oring_cut(x, y, t, d_screw, id_r, cs, head) {
    gd = oring_gland_d(id_r, cs); gh = oring_gland_h(cs);
    assert(head >= id_r + 2*cs*0.85,
           str("cb_oring_cut: a ", head, " mm head covers under 85 % of a ",
               id_r, "x", cs, " O-ring's section — use a larger head or a smaller ring"));
    assert(gh + 0.8 <= t, "cb_oring_cut: the plate is too thin to keep a floor under the gland");
    translate([x, y, -0.1])      cylinder(d = d_screw, h = t + 0.2);
    translate([x, y, t - gh])    cylinder(d = gd,      h = gh + 0.1);
}

// ---------------------------------------------------------------------------
//  Weep hole — a sealed box that breathes still condenses, and the water has
//  to leave. Ø2.0 is the catalog's weep: small enough that driven rain does
//  not enter against the air cushion behind it, large enough that a drop
//  actually leaves through 3-5 mm of wall (the doorbell's first Ø1.5 was a
//  capillary that held its water — and was blind besides). Cut it at the
//  cavity's LOWEST point in the MOUNTED orientation, angled 30° downward
//  and outward so a drip has a path and a splash does not. Subtract from
//  the shell. Axis: the hole leaves through a wall whose outward normal is
//  `dir` ("+x","-x","+y","-y") or through the floor ("-z"); (x, y, z) is
//  the point on the cavity face where it starts.
// ---------------------------------------------------------------------------
function weep_d() = 2.0;
module weep_cut(x, y, z, dir, wall, d = weep_d(), tilt = 30) {
    L = wall + 2 + (wall + 2)*tan(tilt);
    rot = dir == "+x" ? [0, 90 + tilt, 0] : dir == "-x" ? [0, -90 - tilt, 0]
        : dir == "+y" ? [-90 - tilt, 0, 0] : dir == "-y" ? [90 + tilt, 0, 0]
        : dir == "-z" ? [180, 0, 0]
        : assert(false, "weep_cut: dir is one of +x -x +y -y -z") [0, 0, 0];
    translate([x, y, z]) rotate(rot) translate([0, 0, -1]) cylinder(d = d, h = L);
}

// ---------------------------------------------------------------------------
//  Port hood — an eyebrow over a wall opening. A connector in a wall that
//  faces sideways or up takes rain straight in; a solid awning standing
//  proud of the wall turns a stream into a drip that falls clear of the
//  plug. The section is a wedge whose UNDERSIDE rises at 45° from the wall
//  (self-supporting when the wall prints upright — a flat-bottomed eyebrow
//  is a bridge to nowhere) and whose top slopes back down toward the tip
//  so water leaves at the drip edge, not at the wall. Optional cheeks (side
//  plates, 45°-chamfered underneath for the same reason) shed sideways
//  splash. ADD to the shell, in the wall's own frame: the opening is
//  centered at the origin, +y is up, +z is OUT of the wall.
//    w, h  — the opening;  reach — how far the drip edge stands off the wall
//    drop  — how far below the opening's top the cheeks come down (0 = none)
// ---------------------------------------------------------------------------
module port_hood(w, h, reach, drop = 0, embed = 0.5) {
    assert(reach >= 2.0, "port_hood: a hood shorter than 2 mm sheds nothing");
    assert(embed > 0, "port_hood: the root must embed into the wall — a shared face is not a join");
    W   = w + 2*1.6 + 1.0;          // 0.5 side clearance to the opening + a cheek each side
    top = h/2 + 0.5;                // the awning's root clears the opening's top edge
    s   = reach/2;                  // top-surface fall from the wall to the drip edge (~27°)
    // awning: root block buried `embed` into the wall, then wall/low -> tip
    // (45° underside) -> wall/high (top sloping outward)
    translate([-W/2, 0, 0]) rotate([90, 0, 90]) linear_extrude(W)
        polygon([[top, -embed], [top, 0], [top + reach, reach],
                 [top + reach + s, 0], [top + reach + s, -embed]]);
    // cheeks: a plate each side, 45° under-chamfer so the first layer has a root
    if (drop > 0) for (sx = [-1, 1]) {
        d = max(drop, reach + 0.5);
        translate([sx*(W/2 - 0.8) - 0.8, 0, 0]) rotate([90, 0, 90]) linear_extrude(1.6)
            polygon([[top - d, -embed], [top - d, 0], [top - d + reach, reach],
                     [top + reach, reach], [top + reach + s, 0], [top + reach + s, -embed]]);
    }
}

// ---------------------------------------------------------------------------
//  Wall rib — a vertical stiffener fused to the inside of a shell wall.
//  A 2 mm PETG wall longer than ~60 mm oil-cans under a thumb and rings
//  when dropped; a rib the height of the cavity every 25-30 mm takes the
//  panel's first bending mode out for a few tenths of a gram. Triangular
//  in section toward the cavity (a 45° gusset from the floor) so it prints
//  without support and does not become a component keep-out at board
//  height. ADD inside the cavity, in the wall's frame: the rib stands on
//  the floor (z = 0) against a wall whose inner face is the plane y = 0
//  with the cavity toward -y.
//    h — rib height (the cavity height, or less to stay under a board)
//    w — rib width along the wall;  reach — how far it projects at the root
// ---------------------------------------------------------------------------
module wall_rib(h, w = 1.6, reach = 2.0) {
    assert(w >= core_min_wall(), "wall_rib: rib width below the structural floor");
    translate([-w/2, 0, 0]) rotate([90, 0, 90])
        linear_extrude(w) polygon([[0, 0], [-reach, 0], [0, h]]);   // wedge: full reach at the floor, zero at the top
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
    // the screw registry: the M2 row IS the catalog's print-validated trio
    assert(scr_pilot("m2") == 1.6 && scr_flat_d("m2") == 4.0 && scr_flat_h("m2") == 1.2,
           "core: the M2 row must stay the WAP's validated 1.6 / 4.0 / 1.2 — every released mesh reads it");
    assert(scr_pan_d("m2") == 4.0 && scr_pan_h("m2") == 2.0,
           "core: the M2 pan seat must stay the Vision's validated 4.0 / 2.0");
    for (r = SCR_REGISTRY) {
        assert(len(r) == 11, str("core: screw record \"", r[0], "\" malformed"));
        assert(r[2] < r[1] && r[1] < r[3], str("core: \"", r[0], "\": pilot < nominal < clearance, or the screw either strips or jams"));
        assert(r[8] > r[1] && r[4] > r[3] && r[6] > r[3],
               str("core: \"", r[0], "\": the insert bore and both heads must pass the screw"));
        assert(abs(r[2] - 0.8*r[1]) < 0.11, str("core: \"", r[0], "\": self-tap pilot is 0.8 x nominal"));
    }
    assert(oring_gland_h(1.0) == 0.75, "core: the O-ring gland squeezes 25 %");
    echo("canary_core_lib: self-check OK");
}
