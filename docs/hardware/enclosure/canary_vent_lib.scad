// ============================================================================
//  Canary — PLUMAGE VENT LIBRARY (the brand vent pattern)
//
//  One vent shape for every Canary enclosure: the FEATHER — a teardrop,
//  round base, tapered tip, set point-UP in offset rows like a bird's
//  coverts. It is the brand mark and the engineering answer at once:
//
//    - ROUND BASE, NO CORNERS — slot corners shed vortices and collect
//      dust lines; a teardrop's continuous curvature keeps the passive
//      convection path clean at the low Reynolds numbers these cases
//      breathe at. Fewer, larger openings beat many small ones for the
//      same open area (less wall friction), and the feather form makes
//      "fewer and larger" look deliberate instead of cheap.
//    - POINT-UP SHEDS WATER — on a VERTICAL face, a drip running down the
//      skin meets the tip and splits around the opening instead of
//      pooling on a flat slot top and wicking in. This is splash/drip
//      logic only (think IPX1-ish thinking, not a rating): on a
//      HORIZONTAL face the pattern protects nothing — outdoor builds
//      must hood the opening or relocate it (see the WAP's weather mode
//      for the gasket-grade answer).
//    - OFFSET ROWS ARE THE NEST — odd rows sit half a pitch over, so tips
//      nest between the bases above them: the woven read, uniform web
//      widths (no straight-line weakness across the plate), and the
//      densest packing for a given web.
//
//  Doctrine for reuse: `use <canary_vent_lib.scad>` and cut feather2d()
//  profiles wherever a case vents. Keep feathers POINT-UP in the part's
//  MOUNTED orientation. Echo open areas with feather_area() — it is the
//  exact hull area, so the number cannot drift from the geometry. The
//  convection doctrine stays per-case (low intake, high exhaust, and a
//  wall path that survives the back being flat against a wall) — this
//  file is only the shape language.
//
//  First adopter: canary_s3_lcd7.scad (7" frame + tray, v0.9). When a
//  second case adopts, keep the tip ratio at the default so the mark
//  stays recognisably ONE shape across the fleet.
// ============================================================================

// The feather: a teardrop of overall length l (along +y, POINT UP), base
// width w, tip diameter tip*w. Centred on the base circle's centre, so a
// drop-in swap for a circle of Ø w keeps the base row where it was.
module feather2d(l, w, tip = 0.28) {
    r = tip*w/2;
    hull() {
        circle(d = w);
        translate([0, l - w/2 - r]) circle(d = 2*r);
    }
}

// Exact open area of feather2d(l, w, tip): the hull of two circles is two
// circular sectors plus the two centre-to-centre tangent quads. With
// R = w/2, r = tip*w/2, centre distance d = l - R - r and
// phi = asin((R - r)/d):
//   A = (pi/2 + phi)·R² + (pi/2 - phi)·r² + (R + r)·d·cos(phi)
// (checks: r = R gives phi = 0 → pi·R² + 2·R·d, the stadium; r = 0 gives
// the circle-to-point teardrop — both verified against a polygon hull to
// 6 decimals before this shipped in an echo).
function feather_area(l, w, tip = 0.28) =
    let (R = w/2, r = tip*w/2, d = l - R - r,
         phi = asin((R - r)/d))
    (PI/2 + phi*PI/180) * R * R
  + (PI/2 - phi*PI/180) * r * r
  + (R + r) * d * cos(phi);

// Offset-row cell centres for a plumage field: nx columns, ny rows at
// (px, py) pitch, odd rows shifted +px/2 (and the whole field re-centred).
// Returns [x, y] pairs; the caller filters against its own keepouts — every
// case knows its own bosses, ports and rails, and the honest-echo rule
// ("compute the area from the SAME list the cutter uses") stays with the
// caller too.
function plumage_cells(nx, ny, px, py) =
    [for (r = [0 : ny - 1], c = [0 : nx - 1])
        [(c - (nx - 1)/2)*px + (r % 2 == 1 ? px/2 : 0) - px/4,
         (r - (ny - 1)/2)*py]];
