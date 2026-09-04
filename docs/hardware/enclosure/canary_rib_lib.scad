// ============================================================================
//  Canary RIB library — stiffening as a contract, not a habit
//
//  NOT A PRINTABLE PART. `use <canary_rib_lib.scad>` from a case file.
//
//  WHY THIS FILE EXISTS. The catalog already knew every rule in here and
//  enforced none of them. `canary_core_lib.scad` carried `wall_rib()` with a
//  docstring that is a drop specification — "a 2 mm PETG wall longer than
//  ~60 mm oil-cans under a thumb and rings when dropped; a rib the height of
//  the cavity every 25-30 mm takes the panel's first bending mode out for a
//  few tenths of a gram" — and `grep -rn wall_rib *.scad` found ZERO adopters
//  in 44 files. The rule was written, shipped, and unread, while released
//  shells ran 70.6 to 107.6 mm of bare wall.
//
//  Meanwhile the same three idioms were hand-copied across the catalog:
//    * the corner-post gusset — SIX copies (wap, vision, sense, doorbell,
//      combo, relay_solar), and they had already drifted: the four released
//      copies clamp `gusset_h = max(2, ...)`, the two dev copies dropped the
//      clamp, so a shallower cavity draws a zero-height gusset in silence.
//    * the board support rail — ELEVEN copies, every one a typed `cube([3, ...])`
//      that never consults the floor it stands on. On a 2.0 mm floor that is
//      1.5x the plate it sits on.
//    * the root fillet — ONE file has them (`canary_s3_lcd7.scad`, which says
//      why at length: "a square internal corner there is a crack starter").
//      About twenty-five other rib, rail, boss and blade roots are square.
//
//  That is the catalog's own named failure mode — "a lesson applied to a copy
//  stays in that copy" — caught in the act three times over. So the rules live
//  here as asserts, and the geometry lives here as modules, and an adopter
//  gets both or neither.
//
//  WHAT BELONGS HERE: geometry whose job is stiffness, and the process rules
//  that decide whether a given rib helps or hurts.
//  WHAT DOES NOT: the shell primitives themselves (canary_core_lib), snap fits
//  (canary_snap_lib), the stud/keyhole interface (canary_mount_lib).
//
//  `use<>` does not run top-level statements, so the load-time checks live in
//  rib_selfcheck() — call it once from an adopter (the fit coupon does).
// ============================================================================

use <canary_core_lib.scad>   // the process floors these rules are written against

// ---------------------------------------------------------------------------
//  THE THICKNESS RULE — how wide a rib may be for the wall it stands on.
//
//  Two things decide this on an FDM machine, and only one of them is the
//  injection-molding "sink mark" everyone quotes:
//
//   1. WHOLE EXTRUSIONS. A rib is printed as perimeters. A width that is not
//      a whole number of extrusion lines leaves a sliver the slicer fills
//      with gap-fill — a thin, poorly-bonded bead down the middle of the very
//      feature you added for strength. 3.0 mm on a 0.4 nozzle is 7.5 lines;
//      2.5 mm is 6.25. 1.6 mm is 4 lines exactly, which is why the number the
//      library already used is the right shape of number.
//   2. LOCAL MASS. A rib thicker than the wall it stands on is a heat sink
//      against a thin skin. On FDM this does not pit like a molded part; it
//      reads as a gloss band and a slight pull-in down the outside of the
//      wall, and it is the one rib defect a customer can see from a meter away.
//
//  So the ceiling is 0.8 x the wall, floored to whole extrusions. That is not
//  a number invented here — it reproduces `wall_rib`'s own long-standing
//  default exactly (0.8 x 2.0 = 1.6 = four lines), which is the evidence that
//  the catalog already believed it.
// ---------------------------------------------------------------------------
function rib_t_max(wall) =
    floor(0.8 * wall / core_extrusion() + 1e-9) * core_extrusion();

// A rib width that is a whole number of extrusion lines (see rule 1 above).
function rib_lines(t)     = t / core_extrusion();
function rib_t_snap(t)    = floor(t / core_extrusion() + 1e-9) * core_extrusion();
function rib_t_ok(t, wall) = t <= rib_t_max(wall) + 1e-9;

// ---------------------------------------------------------------------------
//  THE HEIGHT RULE — how tall a free-standing blade may be for its width.
//
//  Past this a rib stops stiffening and starts buckling (and, on the way to
//  that, rings under the nozzle and prints badly). The catalog already states
//  this number in the one place it does the arithmetic —
//  `canary_s3_lcd147.scad`: "the compliant ribs would stand ... on a ... mm
//  section — too slender to load", asserted at 9.0 x the rib width. This is
//  that assert, generalized so every adopter gets it.
// ---------------------------------------------------------------------------
function rib_h_max(t) = 9.0 * t;

//  A BOSS TOWER is a tube, not a blade, so it carries further before it goes
//  slender — but a 20 mm tall tube on a 1.2 mm wall (the DIN hub's Pi cover
//  screws) is 16.7x and snaps at the root under a side nudge during assembly.
function boss_h_max(wall) = 12.0 * wall;

// ---------------------------------------------------------------------------
//  THE SPAN RULE — how far a panel may run before it needs a rib.
//
//  `wall_rib`'s docstring fixes the anchor: ~60 mm at the catalog's 2.0 mm
//  wall. Scaling it to other thicknesses is plate bending, not taste. For a
//  strip of thickness t under a given pressure, deflection goes as
//  L^4 / t^3, so an equal-deflection span goes as t^(3/4):
//
//      L_max(t) = 60 * (t / 2.0)^0.75
//
//  which gives 85 mm at 3.2 mm and 101 mm at 4.0 mm. Those are the numbers
//  that decide the real cases: the relay's 81.7 mm run of 3.2 mm wall passes
//  (marginally, and it is a pole-mounted part, so marginal is worth knowing),
//  and the doorbell's 107.6 mm of 4.0 mm wall does not.
//
//  This is an engineering approximation stated as one, not a measured limit.
//  It is here so a future long case trips a gate instead of shipping a panel
//  that rings.
// ---------------------------------------------------------------------------
function rib_span_max(t) = 60.0 * pow(t / core_wall(), 0.75);
function rib_span_ok(len, t) = len <= rib_span_max(t) + 1e-6;

//  Rib spacing along a wall that needs them — the middle of `wall_rib`'s own
//  "every 25-30 mm" band.
function rib_pitch() = 28.0;

//  How many ribs a bare span wants (0 when the span is inside the limit).
function rib_count(len, t) =
    rib_span_ok(len, t) ? 0 : max(1, ceil(len / rib_pitch()) - 1);

// ---------------------------------------------------------------------------
//  THE ROOT RULE — every rib, rail, blade and boss meets its parent in a
//  fillet, never a square internal corner.
//
//  A square re-entrant corner is where an FDM part splits: the crack runs one
//  layer boundary and there is nothing to turn it. `canary_s3_lcd7.scad` is
//  the only file in the catalog that already does this, and it says why —
//  "a corner drop flexes the walls against the plate; a square internal
//  corner there is a crack starter (same doctrine as the boss root fillets)".
//
//  It is a 45 degree wedge and not a true radius, for the same reason
//  `soft_edge_plate` is a taper by construction: 45 degrees prints as a
//  self-supporting ramp in the part's own orientation, and a true radius on
//  an internal corner has an overhang at the top of the arc.
// ---------------------------------------------------------------------------
function rib_root() = core_min_wall();     // 1.2 — the default fillet leg

// A 45 degree fillet along a straight root of length `len`, lying in the
// corner between the plane z = 0 and the plane y = 0, material toward +y/+z.
// Centered on x, running along X.
//  The wedge is drawn 0.01 PROUD into both parents. A fillet that merely
//  touches its wall on a shared face is not joined to it — CGAL returns a
//  non-2-manifold union and the mesh gate fails. The catalog already states
//  this rule where it first bit: `port_hood`'s embed assert, "the root must
//  embed into the wall — a shared face is not a join".
module rib_root_fillet(len, r = rib_root()) {
    assert(r > 0, "rib_root_fillet: a zero fillet is the square corner this exists to remove");
    e = 0.01;
    translate([-len/2, 0, 0]) rotate([90, 0, 90])
        linear_extrude(len) polygon([[-e, -e], [r, -e], [-e, r]]);
}

// The same fillet run around the OUTSIDE of a rectangular upstand — the cove
// where a rib loop or a wall meets the plate it stands on. Four straight runs
// and no corner blend: at these leg lengths the corner is a 1.2 mm notch in a
// 3 mm radius, which is not what starts a crack, and mitring it in OpenSCAD
// costs a hull per corner for nothing you can feel.
module rib_root_fillet_rect(l, w, r = rib_root()) {
    // rib_root_fillet runs along +X with its material toward +y, so the +Y
    // face needs no rotation and each other face is that one turned to put
    // the material OUTBOARD of the upstand it is filleting.
    for (s = [-1, 1]) {
        translate([0, s*w/2, 0]) rotate([0, 0, s > 0 ?   0 : 180]) rib_root_fillet(l, r);
        translate([s*l/2, 0, 0]) rotate([0, 0, s > 0 ? 270 :  90]) rib_root_fillet(w, r);
    }
}

// ---------------------------------------------------------------------------
//  Wall rib — MOVED HERE from canary_core_lib.scad, where it sat for its
//  whole life with no adopters. Same geometry, same defaults, same wedge: a
//  vertical stiffener fused to the inside of a shell wall, triangular in
//  section toward the cavity (full reach at the floor, zero at the top) so it
//  prints without support and does not become a component keep-out at board
//  height.
//
//  ADD inside the cavity, in the wall's frame: the rib stands on the floor
//  (z = 0) against a wall whose inner face is the plane y = 0, cavity toward -y.
//    h — rib height (the cavity height, or less to stay under a board or a lip)
//    w — rib width along the wall;  reach — how far it projects at the root
//
//  It takes no root fillet, and that is deliberate: the section is already a
//  wedge with its full reach AT the floor and nothing at the top, so the
//  floor junction is a ramp rather than a corner. The fillet modules above
//  are for the square roots the audit actually found — rails, dividers,
//  blades and boss towers.
// ---------------------------------------------------------------------------
module wall_rib(h, w = 1.6, reach = 2.0) {
    assert(w >= core_min_wall(), "wall_rib: rib width below the structural floor");
    assert(h <= rib_h_max(w) + 1e-9,
           str("wall_rib: a ", h, " mm rib on a ", w,
               " mm section is past the ", rib_h_max(w),
               " mm slenderness ceiling — widen it or shorten it"));
    translate([-w/2, 0, 0]) rotate([90, 0, 90])
        linear_extrude(w) polygon([[0, 0], [-reach, 0], [0, h]]);
}

// A row of wall ribs across a bare span, placed only when the span needs them.
// Runs along X, centered at the origin, against a wall whose inner face is
// y = 0. `skip` is a list of x positions to leave clear (a port, a clip, a
// bay); any station within `clear` of one is dropped rather than drawn into it.
module wall_rib_row(span, wall, h, w = 0, reach = 2.0, fil = 0,
                    skip = [], clear = 6.0) {
    t = (w > 0) ? w : min(1.6, rib_t_max(wall));
    n = rib_count(span, wall);
    if (n > 0) for (i = [1 : n]) {
        x = -span/2 + i * span / (n + 1);
        keep = [for (s = skip) if (abs(s - x) < clear) 1];
        if (len(keep) == 0) translate([x, 0, 0]) wall_rib(h, t, reach, fil);
    }
}

// ---------------------------------------------------------------------------
//  Corner gusset — the web that ties a screw post to the two walls beside it.
//
//  The six hand-copies drew this as `hull(cylinder(d = post), cylinder(d = 2)
//  at the wall)`, and a hull between two circles of different radii flares:
//  the web does NOT arrive at the wall as wide as the small circle, it arrives
//  wider. Measured on the combo, a nominally 2.0 mm tip lands 2.08 mm wide on
//  a 2.0 mm wall — 1.04x, on the outside skin of a wall-mounted camera. The
//  tip diameter was never the number that mattered; the LANDING width is.
//
//  So this draws a constant-width web instead: no flare, nothing to
//  reverse-engineer, and the width that is asserted is the width that lands.
//  The top is chamfered at 45 degrees so the web does not leave a shelf inside
//  the cavity and prints clean off the post.
//
//    px, py    — post center
//    ax, ay    — the point on the wall the web runs to
//    h         — web height (clamped, see below)
//    wall      — the wall it lands on (sets the width ceiling)
//    post_d    — the post it springs from
// ---------------------------------------------------------------------------
module corner_gusset(px, py, ax, ay, h, wall, post_d, t = 0, slope = true) {
    // The four released copies clamp this; the two dev copies dropped the
    // clamp and would draw a zero- or negative-height web without complaint.
    hh  = max(2.0, h);
    ww  = (t > 0) ? t : rib_t_max(wall);
    assert(rib_t_ok(ww, wall),
           str("corner_gusset: a ", ww, " mm web landing on a ", wall,
               " mm wall is over the ", rib_t_max(wall), " mm ceiling"));
    len = norm([ax - px, ay - py]);
    ang = atan2(ay - py, ax - px);
    assert(len > post_d/2,
           "corner_gusset: the wall is inside the post — nothing to web");
    // The web is drawn as one profile swept across its width: tall at the
    // post, and (when sloped) falling at 45 degrees toward the wall so it
    // leaves no horizontal shelf inside the cavity and prints off the post
    // without an overhang.
    h_end = slope ? max(2.0, hh - len) : hh;
    translate([px, py, 0]) rotate([0, 0, ang])
        translate([0, ww/2, 0]) rotate([90, 0, 0]) linear_extrude(ww)
            polygon([[0, 0], [len, 0], [len, h_end], [0, hh]]);
}

// ---------------------------------------------------------------------------
//  Board rail — the pedestal a PCB edge rests on.
//
//  Eleven files typed `cube([3, ...])` for this and none of them looked at the
//  floor underneath. On the catalog's 2.0 mm floor a 3.0 mm rail is 1.5x the
//  plate it stands on, and the rail is 21-44 mm long, so the thick/thin
//  junction is a LINE, not a spot — which is exactly the geometry that reads
//  through a show face. On `canary_sense_gang.scad` the far side of that line
//  is a plate whose entire product claim is that it looks factory-made.
//
//  The rail's INBOARD face is the datum every call site places from, so
//  thinning the rail does not move a board.
//
//    x, y   — the rail's center;  l — length along Y;  h — height off the floor
//    wall   — the floor it stands on (sets the width ceiling)
//    t      — override width (0 = derive it)
// ---------------------------------------------------------------------------
module board_rail(x, y, l, h, wall, t = 0, fil = 0, dir = 1) {
    ww = (t > 0) ? t : min(3.0, rib_t_max(wall));
    assert(ww >= core_min_wall(),
           str("board_rail: a ", wall, " mm floor cannot carry a rail at the ",
               core_min_wall(), " mm structural floor — thicken the plate"));
    assert(rib_t_ok(ww, wall),
           str("board_rail: a ", ww, " mm rail on a ", wall, " mm floor is over the ",
               rib_t_max(wall), " mm ceiling — it will read through the face"));
    // placed from the INBOARD face so the board datum does not move
    translate([x + dir * ww/2, y, 0]) {
        translate([-ww/2, -l/2, 0]) cube([ww, l, h]);
        // a cove down each long side, where the rail meets the plate
        if (fil > 0) for (s = [-1, 1])
            translate([s*ww/2, 0, 0]) rotate([0, 0, s > 0 ? 270 : 90])
                rib_root_fillet(l, fil);
    }
}

// ---------------------------------------------------------------------------
//  Plate ribs — a section for a flat panel.
//
//  A 158 x 88 x 2.4 mm chassis plate, a 113.7 x 73.6 x 2.4 mm display back, a
//  110 x 70 x 3.0 mm sign: three sheets in this catalog with no third
//  dimension anywhere on them. They warp off the bed, and afterwards they do
//  not stay flat — which matters most on the display back, because four snap
//  dock pads on it are assumed coplanar by `canary_cradle_lib`'s span checks,
//  and a fit gate that catches a bad SPAN cannot see a bad PLANE.
//
//  A peripheral upstand plus a cross rib or two turns the sheet into a shallow
//  box. It also becomes the standoff that holds the plate off whatever it sits
//  against, so it usually pays for itself twice.
//
//    l, w   — the plate;  t — plate thickness;  h — upstand height
//    inset  — how far inboard of the outline the loop runs
//    n      — transverse ribs across the short axis (0 = just the loop)
// ---------------------------------------------------------------------------
module plate_ribs(l, w, t, h, r = 3.0, inset = 3.0, n = 2, rw = 0, fil = 0) {
    ww = (rw > 0) ? rw : min(1.6, rib_t_max(t));
    assert(ww >= core_min_wall(),
           str("plate_ribs: a ", t, " mm plate is too thin to carry a rib at the ",
               core_min_wall(), " mm floor"));
    assert(h <= rib_h_max(ww) + 1e-9,
           str("plate_ribs: a ", h, " mm upstand on a ", ww,
               " mm section is past the ", rib_h_max(ww), " mm slenderness ceiling"));
    li = l - 2*inset;  wi = w - 2*inset;
    // the peripheral loop
    linear_extrude(h) difference() {
        rrect2d(li, wi, max(0.1, r - inset));
        rrect2d(li - 2*ww, wi - 2*ww, max(0.1, r - inset - ww));
    }
    if (fil > 0) rib_root_fillet_rect(li, wi, fil);
    // transverse ribs
    if (n > 0) for (i = [1 : n]) {
        x = -li/2 + i * li / (n + 1);
        translate([x - ww/2, -wi/2, 0]) cube([ww, wi, h]);
    }
}

// ---------------------------------------------------------------------------
//  Boss tower — a screw tube that stands off a plate.
//
//  Four Ø5.2 x 20 mm tubes with a 1.2 mm wall stand unbraced on the DIN hub's
//  Pi cover, each one the whole load path for an M2.5 screw. 20 / 1.2 = 16.7,
//  past `boss_h_max` — a side nudge during assembly snaps one at the root, and
//  they ring under the nozzle on the way to being printed.
//
//  Two answers, both here: taper the tube so the root section is bigger than
//  the head section, and web it to something. A tapered tube costs nothing and
//  quadruples the root area.
//
//    d_out, d_in — the tube;  h — height off the plate;  taper — extra root Ø
//    webs        — number of 45 degree buttresses (0 = bare)
// ---------------------------------------------------------------------------
module boss_tower(d_out, d_in, h, taper = 0, webs = 0, web_t = 0, wall_t = 0,
                  fil = 0, web_reach = 0) {
    tw = (d_out - d_in) / 2;
    // - 1e-9: a wall derived as (d_in + 2*1.2 - d_in)/2 lands a float hair
    // under 1.2 and must not trip a gate about REAL thin walls
    assert(tw >= core_min_wall() - 1e-9,
           str("boss_tower: a ", tw, " mm tube wall is under the ",
               core_min_wall(), " mm structural floor"));
    assert(h <= boss_h_max(tw) + 1e-9 || taper > 0 || webs > 0,
           str("boss_tower: ", h, " mm on a ", tw, " mm wall is past the ",
               boss_h_max(tw), " mm slenderness ceiling — taper it or web it"));
    reach = (web_reach > 0) ? web_reach : d_out * 0.8;
    wt    = (web_t > 0) ? web_t
          : (wall_t > 0) ? rib_t_max(wall_t) : core_min_wall();
    difference() {
        union() {
            cylinder(d1 = d_out + taper, d2 = d_out, h = h);
            // Each buttress is a right triangle in a plane through the axis:
            // full height at the tube, falling to nothing at `reach`. (The
            // extrude maps local x -> radial and local y -> height, so the
            // polygon is [radial, height] in that order and not the reverse.)
            if (webs > 0) for (i = [0 : webs - 1])
                rotate([0, 0, i * 360 / webs])
                    translate([0, -wt/2, 0]) rotate([90, 0, 90])
                        linear_extrude(wt) polygon([[0, 0], [reach, 0], [0, h]]);
            if (fil > 0) cylinder(d1 = d_out + taper + 2*fil, d2 = d_out + taper, h = fil);
        }
        translate([0, 0, -0.1]) cylinder(d = d_in, h = h + 0.2);
    }
}

// ---------------------------------------------------------------------------
//  Self-check — the arithmetic above, verified against the numbers the
//  catalog already shipped. Call once from an adopter (the fit coupon does).
// ---------------------------------------------------------------------------
module rib_selfcheck() {
    // The thickness ceiling must reproduce wall_rib's own historical default:
    // 0.8 x the 2.0 mm catalog wall is 1.6, and 1.6 is four whole extrusions.
    assert(abs(rib_t_max(core_wall()) - 1.6) < 1e-9,
           "rib: the ceiling on the catalog wall must stay wall_rib's own 1.6");
    assert(abs(rib_lines(rib_t_max(core_wall())) - 4) < 1e-9,
           "rib: the ceiling must be a whole number of extrusion lines");
    // and it must catch the typed 3.0 rail on every floor the catalog uses
    for (floor_t = [2.0, 2.2, 2.4, 3.2]) {
        assert(!rib_t_ok(3.0, floor_t),
               str("rib: a 3.0 mm rail on a ", floor_t,
                   " mm floor must not pass the ceiling"));
    }
    // the span rule's anchor is wall_rib's own ~60 mm at the catalog wall
    assert(abs(rib_span_max(core_wall()) - 60.0) < 1e-9,
           "rib: the span anchor is 60 mm at the catalog's 2.0 mm wall");
    assert(rib_span_max(4.0) > rib_span_max(2.0),
           "rib: a thicker panel must be allowed to run further");
    assert(!rib_span_ok(107.6, 4.0),
           "rib: the doorbell's 107.6 mm run of 4.0 mm wall must not pass");
    assert(rib_span_ok(54.2, 2.0),
           "rib: the Sense's 54.2 mm wall is inside the limit and must pass");
    // slenderness: the DIN hub's 20 mm tube on a 1.2 mm wall must fail
    assert(20.0 > boss_h_max(1.2),
           "rib: a 20 mm tower on a 1.2 mm wall must trip the slenderness gate");
    assert(rib_root() > 0, "rib: the default root fillet cannot be zero");
    echo("canary_rib_lib: self-check OK");
}
