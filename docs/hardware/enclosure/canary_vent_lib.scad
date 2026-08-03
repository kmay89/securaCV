// ============================================================================
//  Canary — HATCHERY VENT LIBRARY (the brand vent pattern)
//
//  One vent shape for every Canary enclosure: the EGG — a rounded oval,
//  wide base, rounded crown, set upright in offset rows like a clutch in a
//  nest. It is the brand mark and the engineering answer at once:
//
//    - ROUND BASE, NO CORNERS — slot corners shed vortices and collect
//      dust lines; a continuous-curvature opening keeps the passive
//      convection path clean at the low Reynolds numbers these cases
//      breathe at. Fewer, larger openings beat many small ones for the
//      same open area (less wall friction), and the egg form makes
//      "fewer and larger" look deliberate instead of cheap.
//    - UPRIGHT SHEDS WATER — on a VERTICAL face, a drip running down the
//      skin meets the crown and parts around the opening instead of
//      pooling on a flat slot top and wicking in. The egg's crown is
//      blunter than the old feather's point, so it parts a drip less
//      aggressively — this was always splash/drip logic only (think
//      IPX1-ish thinking, not a rating), and the egg does not weaken it
//      enough to matter at that standard. On a HORIZONTAL face the
//      pattern protects nothing either way — outdoor builds must hood the
//      opening or relocate it (see the WAP's weather mode for the
//      gasket-grade answer).
//    - OFFSET ROWS ARE THE NEST — odd rows sit half a pitch over, so the
//      crowns nest between the bases above them: the clutch read, uniform
//      web widths (no straight-line weakness across the plate), and the
//      densest packing for a given web.
//
//  THE TIP RATIO IS THE BRAND CONSTANT — leave it at the default.
//  Geometrically the profile is one primitive at any ratio: a hull of two
//  circles, wide end down. The ratio alone decides whether it reads as a
//  teardrop with a point (0.28 — the FEATHER this library shipped with) or
//  an egg with a rounded crown (0.72 — what it is now). Passing your own
//  `tip` is how the line ends up wearing two marks, so don't: change it
//  HERE, once, and every case that cuts with this library follows.
//
//  Doctrine for reuse: `use <canary_vent_lib.scad>` and cut egg2d()
//  profiles wherever a case vents a FIELD. Keep eggs UPRIGHT in the part's
//  MOUNTED orientation. Echo open areas with egg_area() — it is the exact
//  hull area, so the number cannot drift from the geometry. The convection
//  doctrine stays per-case (low intake, high exhaust, and a wall path that
//  survives the back being flat against a wall) — this file is only the
//  shape language.
//
//  WHAT IS NOT A HATCHERY VENT — do not "helpfully" convert these:
//    - The GORE-membrane clusters on the WAP, Sense and Vision lids
//      (`vent_cluster`) are a ring of round holes behind an adhesive
//      pressure-equalisation membrane. They are a seal, not a grille; the
//      hole pattern is set by the membrane pad, and shaping them would
//      cost sealing area for no visible mark.
//    - Sub-2 mm slots (the C6 and 1.69" back grilles, the DIN hub's floor
//      and chimney slots run at vent_slot_w = 1.4). An egg needs length
//      enough to read as one; at those widths the profile is smaller than
//      the eye resolves at arm's length and smaller than most nozzles
//      place cleanly. Those cases vent, they do not wear the mark.
//  The mark lives on faces big enough to show a field. Today that is the
//  7" back plate (canary_s3_lcd7.scad, the library's first and so far only
//  adopter); it is a property of the surface, not a ranking of the cases.
// ============================================================================

// The egg: a rounded oval of overall length l (along +y, UPRIGHT), base
// width w, crown diameter tip*w. Centered on the base circle's center, so a
// drop-in swap for a circle of Ø w keeps the base row where it was.
//
// THE brand constant, and the ONE place the number lives. A function, not a
// variable, for two reasons: it does not surface in an adopter's Customizer
// as an editable knob, and an adopter that wants a local name for it can
// DERIVE that name (vent_tip = egg_tip()) instead of typing a copy of the
// number. A typed copy is how "change it here and every case follows"
// quietly becomes false — the 7" had exactly that copy and would have been
// left behind by the first change to this line.
function egg_tip() = 0.72;

// `tip` defaults to the brand constant — see the header before overriding.
module egg2d(l, w, tip = egg_tip()) {
    r = tip*w/2;
    hull() {
        circle(d = w);
        translate([0, l - w/2 - r]) circle(d = 2*r);
    }
}

// Exact open area of egg2d(l, w, tip): the hull of two circles is two
// circular sectors plus the two center-to-center tangent quads. With
// R = w/2, r = tip*w/2, center distance d = l - R - r and
// phi = asin((R - r)/d):
//   A = (pi/2 + phi)·R² + (pi/2 - phi)·r² + (R + r)·d·cos(phi)
// (checks: r = R gives phi = 0 → pi·R² + 2·R·d, the stadium; r = 0 gives
// the circle-to-point teardrop — both verified against a polygon hull to
// 6 decimals before this shipped in an echo).
function egg_area(l, w, tip = egg_tip()) =
    let (R = w/2, r = tip*w/2, d = l - R - r,
         phi = asin((R - r)/d))
    (PI/2 + phi*PI/180) * R * R
  + (PI/2 - phi*PI/180) * r * r
  + (R + r) * d * cos(phi);

// Offset-row cell centers for a clutch field: nx columns, ny rows at
// (px, py) pitch, odd rows shifted +px/2 (and the whole field re-centered).
// Returns [x, y] pairs; the caller filters against its own keepouts — every
// case knows its own bosses, ports and rails, and the honest-echo rule
// ("compute the area from the SAME list the cutter uses") stays with the
// caller too.
function clutch_cells(nx, ny, px, py) =
    [for (r = [0 : ny - 1], c = [0 : nx - 1])
        [(c - (nx - 1)/2)*px + (r % 2 == 1 ? px/2 : 0) - px/4,
         (r - (ny - 1)/2)*py]];
