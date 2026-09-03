// ============================================================================
//  Canary — HATCHERY VENT LIBRARY (the brand vent pattern)
//
//  One vent shape for every Canary enclosure: the EGG — a true ovoid, wide
//  base, rounded crown, set upright in offset rows like a clutch in a nest.
//  It is the brand mark and the engineering answer at once:
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
//  THE PROFILE IS A TANGENT-ARC EGG, fully derived from (l, w, tip) — no
//  styling knob. Below the equator it is the base semicircle (radius w/2);
//  the flanks leave the equator TANGENT and curve inward on one arc each;
//  the crown caps it (radius tip*w/2). Every join is tangent-continuous and
//  the flank radius is solved, not chosen: it is the one radius that meets
//  both circles tangentially FROM the equator, which makes this the roundest
//  egg that still measures exactly w across. The first cut of this library
//  drew the profile as a hull of two circles — same circles, but the hull's
//  flanks are STRAIGHT tangent lines, and straight flanks are why it read
//  as a tapered slot instead of an egg. The flanks are the fix, and they are
//  geometry, not styling: convex everywhere, self-supporting everywhere
//  (nothing overhangs past 45° that the old capsule didn't), and at tip → 1
//  the flank radius runs to infinity and the profile degrades gracefully
//  toward the stadium it would have been anyway.
//
//  THE TIP RATIO IS THE BRAND CONSTANT — leave it at the default.
//  The ratio alone decides whether the profile reads as a teardrop with a
//  point (0.28 — the FEATHER this library shipped with) or an egg with a
//  rounded crown (0.72 — what it is now). Passing your own `tip` is how the
//  line ends up wearing two marks, so don't: change it HERE, once, and every
//  case that cuts with this library follows.
//
//  Doctrine for reuse: `use <canary_vent_lib.scad>` and cut egg2d()
//  profiles wherever a case vents a FIELD. Keep eggs UPRIGHT in the part's
//  MOUNTED orientation. Echo open areas with egg_area() — it is the
//  shoelace area of the SAME point list egg2d() draws, so the number
//  cannot drift from the geometry. The convection doctrine stays per-case
//  (low intake, high exhaust, and a wall path that survives the back being
//  flat against a wall) — this file is only the shape language.
//
//  WHAT IS NOT A HATCHERY VENT — do not "helpfully" convert these:
//    - The GORE-membrane clusters on the WAP, Sense and Vision lids
//      (`vent_cluster`) are a ring of round holes behind an adhesive
//      pressure-equalization membrane. They are a seal, not a grille; the
//      hole pattern is set by the membrane pad, and shaping them would
//      cost sealing area for no visible mark.
//    - Sub-2 mm slots (the C6 and 1.69" back grilles run at vent_w = 1.4).
//      An egg needs length enough to read as one; at those widths the
//      profile is smaller than the eye resolves at arm's length and smaller
//      than most nozzles place cleanly. Those cases vent, they do not wear
//      the mark. Use egg_reads() rather than re-checking this by hand —
//      this paragraph used to excuse the DIN hub here too, on the grounds
//      that its slots also ran at 1.4, and they run at 2.0. The exemption
//      was entirely a size argument and the size was wrong by 43 %.
//    - The DIN hub, on the real reason rather than that one: its floor and
//      chimney slots are structural cuts on faces nobody looks at (a rail-
//      mounted box, slots facing the panel and the ceiling).
//  The mark lives on faces big enough to show a field. Today that is the
//  7" back plate (canary_s3_lcd7.scad, the library's first and so far only
//  adopter); it is a property of the surface, not a ranking of the cases.
// ============================================================================

// The egg: a true ovoid of overall length l (along +y, UPRIGHT), base
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

// The width below which the egg stops reading as an egg — smaller than the
// eye resolves at arm's length, and smaller than most nozzles place cleanly.
// A FUNCTION and not a sentence, because the sentence went stale: the prose
// above excused the DIN hub at "vent_slot_w = 1.4" while the hub actually
// runs 2.0, and the whole exemption was a size argument. A maintainer should
// ask the library, not re-check a comment against five files.
function egg_min_w() = 2.0;
function egg_reads(w) = w >= egg_min_w();

// The construction, one place (everything else reads its fields):
//   R    base radius w/2            — the semicircle below the equator
//   r    crown radius tip*w/2       — the cap, center at height c
//   c    crown center height        — l - R - r, same as the old capsule
//   rho  flank radius, SOLVED       — the arc that leaves the equator at
//        (±R, 0) tangent (vertical) and meets the crown circle tangent:
//        internal tangency to both circles gives (rho-R)^2 + c^2 = (rho-r)^2,
//        so rho = (c^2/(R - r) + R + r)/2. Flank center sits ON the equator
//        at (∓(rho - R), 0), which is what pins the widest point of the whole
//        profile to exactly w — the flank can only curve INWARD from there.
//   a2   flank/crown tangency angle — atan2(c, rho - R), measured at both
//        the flank center and the crown center (same ray: that IS tangency).
// The two asserts are the two ways the construction can stop being an egg.
function _egg_geo(l, w, tip) =
    assert(tip > 0 && tip < 1,
           "egg2d: tip must be in (0,1) — for tip = 1 the egg is a stadium; cut pill2d/stadium2d instead")
    assert(l > w,
           "egg2d: too short to be an egg — the tangency needs c > R - r, which reduces to l > w exactly")
    let (R = w/2, r = tip*w/2, c = l - R - r,
         rho = (c*c/(R - r) + R + r)/2)
    [R, r, c, rho, atan2(c, rho - R)];

// Arc sampler, degrees, CCW a0 -> a1. `tail` drops the first point so
// chained arcs share their tangency vertex once instead of twice.
function _egg_arc(cx, cy, rad, a0, a1, tail = false) =
    let (n = max(4, ceil(abs(a1 - a0)/5)))
    [for (i = [tail ? 1 : 0 : n])
        [cx + rad*cos(a0 + (a1 - a0)*i/n),
         cy + rad*sin(a0 + (a1 - a0)*i/n)]];

// Right half of the outline, bottom (0,-R) to apex (0, c+r), CCW:
// base quarter-arc to the equator, flank arc to the crown tangency, crown
// arc over the top.
function _egg_half(l, w, tip) =
    let (g = _egg_geo(l, w, tip),
         R = g[0], r = g[1], c = g[2], rho = g[3], a2 = g[4])
    concat(_egg_arc(0, 0, R, -90, 0),
           _egg_arc(-(rho - R), 0, rho, 0, a2, tail = true),
           _egg_arc(0, c, r, a2, 90, tail = true));

// The full outline, CCW, apex and bottom vertices appearing once each.
// This ONE list is both the cutter's polygon and the area's input.
function egg_pts(l, w, tip = egg_tip()) =
    let (right = _egg_half(l, w, tip), n = len(right))
    concat(right, [for (i = [n - 2 : -1 : 1]) [-right[i][0], right[i][1]]]);

// `tip` defaults to the brand constant — see the header before overriding.
module egg2d(l, w, tip = egg_tip()) polygon(egg_pts(l, w, tip));

// Open area of egg2d(l, w, tip): the shoelace area of the SAME point list
// the cutter draws — not a closed form beside the geometry, so the echoed
// number cannot drift from the cut shape, ever, including through the
// profile change above (the old exact-hull formula was correct for the old
// hull and would have silently overstated nothing and understated the
// flanks). At the 5°-per-segment sampling the polygon sits inside the true
// arcs by well under 0.1 %, and it is the polygon that gets printed.
function _egg_sum(v, i = 0, acc = 0) =
    i >= len(v) ? acc : _egg_sum(v, i + 1, acc + v[i]);
function egg_area(l, w, tip = egg_tip()) =
    let (p = egg_pts(l, w, tip), n = len(p))
    0.5*abs(_egg_sum([for (i = [0 : n - 1])
        p[i][0]*p[(i + 1) % n][1] - p[(i + 1) % n][0]*p[i][1]]));

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
