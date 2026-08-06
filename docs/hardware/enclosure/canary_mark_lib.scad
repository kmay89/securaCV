// ============================================================================
//  Canary — HOUSE MARK LIBRARY (the bird, as printable geometry)
//
//  One definition of the Canary bird for every enclosure that wears it, the
//  same way canary_vent_lib.scad owns the egg. It was authored inside
//  canary_fit_coupon.scad (the rib-width test coupon, which is where the
//  printability of the strokes gets proven); it lives here now so a case can
//  wear the mark without depending on a test fixture — and so there is exactly
//  one bird in the line to change if the mark ever changes.
//
//  WHAT THE MARK IS
//    A MONOLINE bird carrying a notepad, with the company initials built into
//    it: the wing spirals into a **C**, and a **V** is nested in the tail. It
//    is one continuous weight of line throughout — that is the whole idea, and
//    it is also why this drawing prints. Every stroke is the same width, so
//    there is one number (`rib`) that decides whether the mark survives a
//    nozzle, and the answer is the same for every part of it.
//
//  WHY PATHS AND NOT THE ARTWORK
//    The brand artwork is a raster. Tracing a raster gives thousands of
//    points describing the same curve twice (once per side of the stroke),
//    which is unusable as printable geometry and impossible to re-weight. The
//    bird here is authored as CENTERLINE polylines and given its width by a
//    stroke parameter, so the mark is re-drawn at whatever weight a given part
//    can actually print, rather than being scaled down until it disappears.
//    Chaikin corner-cutting rounds the polylines; two passes is plenty.
//
//  THE ONE RULE
//    Nothing may be drawn thinner than the stroke `t`, and nothing may be
//    drawn CLOSER than `t` to its neighbor either — the second half is new
//    with this drawing, because this drawing has interior detail. The C's
//    mouth, the pad's opening and the gap where the wing tucks under the
//    shoulder are all designed clearances, and a rib that eats them turns the
//    mark into a blob. mark_rib_ok() states the limit and every consumer
//    asserts it; see MINIMUM SIZE below.
//
//  MINIMUM SIZE — this mark does not shrink to a badge
//    The old bird was a silhouette: it read at any size because it had no
//    interior. This one has a letterform inside a spiral inside a bird, and
//    below roughly 30 mm those close up. That is a property of the mark, not
//    a bug to route around, and it is asserted rather than discovered on a
//    printed part. A part too small to carry it should carry the wordmark
//    alone — not a quietly simplified bird, which would put two different
//    birds back in the line.
//
//  Import with `use <canary_mark_lib.scad>` (modules + functions cross; the
//  design-unit constants are exposed as functions precisely so they do).
// ============================================================================

// ── Path smoothing ─────────────────────────────────────────────────────────
function _chaik(p, cl) = let(n = len(p))
    [ for (i = [0 : (cl ? n-1 : n-2)]) each
        let(a = p[i], b = p[(i+1) % n])
        [ [0.75*a[0] + 0.25*b[0], 0.75*a[1] + 0.25*b[1]],
          [0.25*a[0] + 0.75*b[0], 0.25*a[1] + 0.75*b[1]] ] ];
function _smooth(p, cl, k = 2) = k <= 0 ? p : _smooth(_chaik(p, cl), cl, k-1);

// ── The mark in design units — traced from the brand art, y UP, facing LEFT ─
// Centerlines only. The bbox below is centered on the origin by construction,
// so mark_cx()/mark_cy() come out as zero and stay that way if the drawing
// moves — they are computed, not typed.
_m_head    = [[-41.3,28.5],[-41.6,35.8],[-37.8,42.6],[-31.4,46.1],[-23.4,47.0],
              [-14.7,45.4],[-8.0,40.6],[-4.2,33.6],[-2.6,26.2],[-2.6,18.9]];
_m_beak    = [[-41.3,28.5],[-50.6,23.7],[-41.9,18.2]];
// The strap the pad hangs from — it is the breast line, which is why the pad
// reads as carried rather than as a box parked beside a bird.
_m_strap   = [[-41.9,18.2],[-41.0,12.2],[-39.7,6.4]];
// EIGHT points, not four, and the extra four are what make it a notepad. A
// four-corner rectangle put through the same smoothing as the body curves
// comes out an oval — Chaikin does not know which corners are meant to be
// crisp. Pairs of points 15% in from each corner hold the edges straight and
// leave the rounding to the corners, which is what a pad looks like.
_m_pad     = [[-48.1,2.2],[-33.8,5.2],[-30.0,2.2],[-26.9,-14.3],
              [-29.0,-18.5],[-42.3,-21.5],[-46.0,-18.5],[-50.3,-2.0]];
// The clip. It sits close enough to the pad's top edge that the two strokes
// MERGE at any printable rib, and that is deliberate: in the art they are
// separated by a hairline, and a hairline in a deboss is a defect waiting for
// a nozzle. Merged, it reads as a clipboard's clip, which is what it is.
_m_padclip = [[-39.0,7.4],[-29.8,9.3]];
_m_belly   = [[-42.9,-16.0],[-40.3,-23.0],[-34.9,-29.1],[-26.9,-33.9],
              [-16.6,-36.5],[-5.4,-37.4],[4.5,-37.1],[10.6,-36.2]];
_m_back    = [[-1.6,17.6],[6.1,13.1],[14.4,7.7],[23.7,1.9],[33.6,-3.2],
              [43.2,-6.7],[51.2,-8.0]];
_m_tailend = [[51.2,-8.0],[41.0,-14.7],[31.0,-20.2],[23.7,-25.3],[18.9,-28.8]];
// The wing: one spiral from under the shoulder, round the body, out to the
// tail. Its first point is held clear of _m_back's first point — see
// _mark_gaps() for why that clearance is a dimension and not a coincidence.
_m_wing    = [[-7.0,12.5],[-14.7,13.1],[-22.7,7.5],[-27.0,-1.3],[-28.0,-10.9],
              [-24.3,-20.5],[-16.3,-26.9],[-5.1,-31.7],[6.1,-34.4],[17.3,-35.7]];
_m_cee     = [[11.8,1.0],[4.2,5.1],[-4.2,4.8],[-10.2,-0.6],[-12.5,-8.6],
              [-10.6,-16.0],[-4.8,-20.8],[2.9,-22.1],[10.6,-19.8]];
_m_vee     = [[11.7,-10.9],[18.1,-28.5],[29.0,-6.9]];
_m_legs    = [[[-27.8,-35.5],[-28.2,-43.5]], [[-6.1,-37.4],[-5.4,-43.5]]];
_m_feet    = [[[-40.3,-46.7],[-33.3,-43.8],[-28.2,-43.8],[-23.0,-46.7]],
              [[-16.6,-47.0],[-10.2,-44.2],[-5.4,-44.2],[0.0,-46.7]]];
_m_eye     = [-29.1, 29.4];
function _m_eye_d() = 6.4;

// Design-unit metrics, as functions so `use <>` carries them.
function mark_span() = 110;   // design-unit height the mark is scaled against

// The drawn extent, in design units, INCLUDING the stroke's round caps. Every
// consumer that has to keep something away from the mark asks this rather
// than carrying its own ratio: the 7" case's grille keepout and the hallway
// stick's plate-width check were both hand-typed fractions of `h`, and both
// were stale the moment the drawing moved. `t` is the stroke in design units.
function mark_x0(t) = -51.2 - t/2;   // the pad's left edge
function mark_x1(t) =  51.2 + t/2;   // the tail tip
function mark_y0(t) = -47.0 - t/2;   // the feet
function mark_y1(t) =  47.0 + t/2;   // the crown
function mark_bbox(t = 0) = [mark_x0(t), mark_y0(t), mark_x1(t), mark_y1(t)];
function mark_cx(t = 0) = (mark_x0(t) + mark_x1(t)) / 2;   // bbox center
function mark_cy(t = 0) = (mark_y0(t) + mark_y1(t)) / 2;
// The same box in mm, for a mark drawn at height `h` with stroke `rib`.
function mark_scale(h) = h / mark_span();
function mark_rib_du(h, rib) = rib / mark_scale(h);       // rib in design units
function mark_w_mm(h, rib) = let (s = mark_scale(h))
    (mark_x1(mark_rib_du(h, rib)) - mark_x0(mark_rib_du(h, rib))) * s;
function mark_h_mm(h, rib) = let (s = mark_scale(h))
    (mark_y1(mark_rib_du(h, rib)) - mark_y0(mark_rib_du(h, rib))) * s;

// ── THE PRINTABILITY LIMIT ─────────────────────────────────────────────────
// The tightest DESIGNED clearances in the drawing, centerline to centerline,
// in design units. The rib eats one full width out of each of these (half from
// each stroke), so the mark stops reading when the rib approaches the
// smallest of them. Listed rather than summarized, because when the assert
// fires the useful question is "which feature closed?".
//
//   7.4  the wing's top end vs the back's start — the notch at the shoulder
//   8.8  the C's mouth vs the wing spiral beside it
//  10.4  the beak's two edges at their open end
//  16.4  the pad's short side, inner clear
//  20.8  the C's own mouth, end to end
function _mark_tightest() = 7.4;
// Leave a third of the tightest gap open at the limit: two strokes that stop
// exactly one rib apart print as one stroke with a seam, not as two.
function mark_max_rib_du() = _mark_tightest() * 2/3;
function mark_rib_ok(h, rib) = mark_rib_du(h, rib) <= mark_max_rib_du();
// The smallest mark height that can carry `rib`, in mm — what to tell someone
// whose part is too small, instead of just refusing.
function mark_min_h(rib) = rib * mark_span() / mark_max_rib_du();

module _gstroke(pts, t, closed = false) {
    n = len(pts);
    for (i = [0:n-2]) hull() { translate(pts[i]) circle(d = t); translate(pts[i+1]) circle(d = t); }
    if (closed) hull() { translate(pts[n-1]) circle(d = t); translate(pts[0]) circle(d = t); }
}
module _gtaper(pts, t0, t1) {           // width runs t0 -> t1 along the path
    n = len(pts);
    for (i = [0:n-2]) hull() {
        translate(pts[i])   circle(d = t0 + (t1-t0)*i/(n-1));
        translate(pts[i+1]) circle(d = t0 + (t1-t0)*(i+1)/(n-1));
    }
}

// The bird, in design units, stroked at width `t`. Center it with
// translate([-mark_cx(), -mark_cy()]) and scale by h/mark_span().
//
// Monoline: every stroke is `t`, including the letters. There is no fill mode
// and no outline mode, because there is no silhouette to fill — the drawing IS
// the line.
//
// The heavier brand treatment (the mark with a contour ringing it) is NOT
// offered here, and the reason is worth recording so nobody spends an evening
// on it again: an OpenSCAD offset() of an open line drawing fattens every
// stroke individually, so difference(offset(a), offset(b)) draws a concentric
// ring around each stroke instead of one contour around the mark. Getting the
// real thing means filling the enclosed regions first, which this drawing does
// not have — its regions are open. Rendered, the attempt looks like a maze.
module mark_bird_2d(t) {
    union() {
        _gstroke(_smooth(_m_head, false), t);
        _gstroke(_m_beak, t);                     // two straight edges, a point
        _gstroke(_m_strap, t);
        _gstroke(_smooth(_m_pad, true), t, true);
        _gstroke(_m_padclip, t);
        _gstroke(_smooth(_m_belly, false), t);
        _gstroke(_smooth(_m_back, false), t);
        _gstroke(_smooth(_m_tailend, false), t);
        _gstroke(_smooth(_m_wing, false), t);
        _gstroke(_smooth(_m_cee, false), t);
        _gstroke(_m_vee, t);                      // a V is two straight edges
        for (l = _m_legs) _gstroke(l, t);
        for (f = _m_feet) _gstroke(_smooth(f, false, 1), t);
        translate(_m_eye) circle(d = max(_m_eye_d(), t));
    }
}

// The bird centered on the origin at a real height `h` (mm), stroked at a real
// width `rib` (mm). This is the form a case actually wants — it does the
// design-unit bookkeeping so callers do not repeat it and get it subtly wrong.
module mark_bird(h, rib) {
    s = mark_scale(h);
    t = rib / s;
    assert(mark_rib_ok(h, rib),
           str("mark_bird: rib ", rib, " mm at h ", h, " mm is ", t,
               " design units, over the ", mark_max_rib_du(), " this drawing ",
               "can carry — the shoulder notch and the C's mouth close up. ",
               "Draw the mark at ", mark_min_h(rib), " mm or taller, or thin ",
               "the rib to ", mark_max_rib_du()*s, " mm. See MINIMUM SIZE in ",
               "canary_mark_lib.scad: this mark has interior detail and does ",
               "not shrink to a badge."));
    scale([s, s]) translate([-mark_cx(), -mark_cy()]) mark_bird_2d(t);
}

// The bird RAISED, centered on the origin, sitting on z = 0 and standing `ez`
// tall. `h` is the finished height of the mark in mm, `rib` the stroke width
// in mm, `crown` how far the stroke tops draw in.
//
// The strokes are a short stack of inward offsets on a quarter-circle profile,
// so each carries a DOMED crown rather than a flat slab top — that is what
// makes an embossed mark catch light like metal instead of reading as a
// sticker. The footprint stays a full rib wide; only the crown draws in.
//
// This is the emboss every part shares. It lives here rather than in the part
// that first needed it for the usual reason: the coupon proves the crown
// prints, and a case that re-derived the offset stack would be proving
// something slightly different.
module mark_emboss(h, ez, rib = 0.7, crown = 0.15) {
    s = mark_scale(h);
    t = rib / s;                                  // stroke, in design units
    // The same limit mark_bird() enforces, and it has to be repeated here
    // rather than inherited: mark_emboss builds its crown out of mark_bird_2d
    // directly, so it walked straight past the assert and rendered an
    // unprintable mark without a word. A check that only guards one of two
    // doors is not a check.
    assert(mark_rib_ok(h, rib),
           str("mark_emboss: rib ", rib, " mm at h ", h, " mm is ", t,
               " design units, over the ", mark_max_rib_du(), " this drawing ",
               "can carry. Emboss it at ", mark_min_h(rib), " mm or taller, ",
               "or thin the rib to ", mark_max_rib_du()*s,
               " mm. See MINIMUM SIZE in canary_mark_lib.scad."));
    n = crown > 0 ? 4 : 1;
    for (i = [0:n-1]) {
        // the inset runs 0 at the base to the FULL crown on the top slice; the
        // slice's z is a separate ramp, or the last one would sit proud
        f   = n > 1 ? i/(n - 1) : 0;
        ins = (crown / s) * (1 - sqrt(1 - f*f));
        translate([0, 0, i*ez/n]) linear_extrude(ez/n + 0.01)
            scale([s, s]) translate([-mark_cx(), -mark_cy()])
                offset(r = -ins) mark_bird_2d(t);
    }
}

// ── The lockup: bird over wordmark ─────────────────────────────────────────
// The stacked lockup the brand uses — the bird sitting above the company
// word. `h` is the BIRD's height; the word is sized off it so the proportion
// holds at any scale, and the whole group is centered on the origin.
//
// The word is real text, not paths: at badge size a nozzle draws letterforms
// far better than it draws a traced outline of them, and a font substitution
// degrades to "a slightly different sans" rather than to nothing.
// The two proportions, as functions, because BOTH the module below and
// mark_lockup_h() need them and a lockup whose stated height disagrees with
// its drawn height is a caller laying out into the middle of it. They did
// disagree: the module used a 0.22 gap while the height function said 0.16,
// so mark_lockup_h() under-reported by 0.06*h — 1.92 mm at the 7" case's
// h = 32. Typed twice is how that happens; derived once is how it stops.
function mark_word_ratio() = 0.30;   // cap height, as a fraction of the bird
function mark_gap_ratio()  = 0.22;   // bird-to-word gap, same basis

module mark_lockup(h, rib, word = "securaCV", font = "DejaVu Sans:style=Bold") {
    // The bird's drawn extent is a little under `h` (the design span includes
    // headroom), so the gap is measured generously — at badge size the feet
    // and the ascenders are the two things that touch first, and a mark whose
    // word collides with its bird reads as a printing fault, not a lockup.
    word_h = h * mark_word_ratio();
    gap    = h * mark_gap_ratio();
    // Centering: the pair spans (h + gap + word_h); shift so its middle is 0.
    translate([0, (gap + word_h) / 2]) mark_bird(h, rib);
    translate([0, -(h + gap) / 2 + word_h / 2])
        text(word, size = word_h, font = font,
             halign = "center", valign = "center");
}

// Height of the whole lockup, for callers laying out around it. Same three
// terms the module stacks, in the same order — change a ratio above and this
// follows rather than drifting.
function mark_lockup_h(h) = h*(1 + mark_gap_ratio() + mark_word_ratio());

// ── The wordmark alone, for parts too small to carry the bird ──────────────
// This exists because the redraw took a real product with it: the hallway
// stick's back plate is 20.3 mm across, and this mark needs 20.1 mm of HEIGHT
// at the stick's 0.9 mm rib before its interior stops closing up — so the
// bird would be wider than the plate before it was legible on it.
//
// The alternative was a second, simplified bird for small parts, and that is
// precisely the thing this library exists to prevent: two birds in the line,
// drifting. A part either carries the mark or it carries the name. `word_h`
// is a CAP HEIGHT in mm, not a bird height, because there is no bird to
// measure against — callers that had a mark_lockup(h) should pass
// h*mark_word_ratio() to keep the wordmark the size it already was.
module mark_wordmark(word_h, word = "securaCV",
                     font = "DejaVu Sans:style=Bold") {
    text(word, size = word_h, font = font, halign = "center", valign = "center");
}
function mark_wordmark_h(word_h) = word_h;

// ── MEASURED TYPE METRICS — how small the name may go, how wide it lands ────
// The bird has mark_rib_ok()/mark_min_h() to refuse a size it cannot print at.
// The WORDMARK had nothing: every consumer estimated its width with a local
// constant and nobody gated its height at all. Both gaps are closed here, and
// both numbers below are MEASURED — each candidate was rendered by OpenSCAD at
// real millimeter size and its geometry read back, because this library's own
// header is right that OpenSCAD has no text-metrics primitive at RENDER time.
// It does not follow that the numbers have to be guessed; it follows that the
// measuring happens once, offline, and the result is written down here.

// How small the name may go.
//
// The test is morphological and it is the physical question directly: open the
// letterforms with a disk of radius line_w/2 — the shape a bead of that width
// can actually lay — and compare the surviving ink against the drawn ink.
// Anything the disk cannot enter is ink the nozzle cannot put down. Measured
// on "SECURACV.COM/QR", DejaVu Sans Bold, against a 0.4 mm bead:
//
//      cap 1.5 mm -> 26.8%      cap 2.5 mm -> 96.3%
//      cap 2.0 mm -> 81.5%      cap 3.0 mm -> 98.9%      cap 3.6 mm -> 99.3%
//
// The knee is brutal and it sits between 2.0 and 2.5. Below it the letters are
// not "small", they are ABSENT — at 1.5 mm three quarters of the drawing is
// finer than the nozzle, which prints as a smudge with the rhythm of type.
// (The few percent still missing at the top end is corner rounding, which a
// nozzle does to every corner anyway and which no cap height removes.)
//
// The floor is set at 95% reachable ink, which lands at six line widths —
// 2.4 mm at a 0.4 mm nozzle. Same floor the fit coupon states for the bird's
// ribs ("below 0.4 it is thinner than one extrusion"), asked of type.
function mark_word_min_h(line_w = 0.4) = 6.0 * line_w;

// How wide the name lands.
//
// ⚠️ AN AVERAGE ADVANCE BOUNDS NOTHING. The single constant this line carried
// (0.875, in two files) is calibrated for mixed case, and a first cut at
// fixing it split the difference into two — one for caps, one for lowercase.
// Both are averages, and an average is exactly the wrong tool for a guard:
// "WWWWW" at 3.6 mm draws 26.8 mm and a caps average calls it 18.7, so the
// assert whose whole job is to keep type on the part waves it through and the
// word hangs 2.8 mm off each edge. Averages describe a typical word; a gate
// has to hold for the worst one. (Caught in review on this very change —
// thank you. The lowercase half had the same hole: "m" is 1.41, not 0.88.)
//
// So the table below is not an estimate of the average, it is the MEASURED
// ADVANCE OF EVERY PRINTABLE GLYPH — ASCII 32..126, DejaVu Sans Bold, as a
// fraction of cap height. Each was obtained the only way OpenSCAD allows:
// render "cc", render "c", and take the difference, which is the pen movement
// between the two copies. (Space carries no ink and went through a carrier
// pair instead.) Nothing here is inferred from anything else.
//
// Summing advances OVER-reports on purpose: the true ink extent of a string
// is the sum of its advances less the outer sidebearings, and kerned pairs
// only ever pull tighter. Checked against eight measured strings — "Canary",
// "CANARY", "securaCV", "SECURACV", "SECURACV.COM/QR", and the pathological
// "WWWWW"/"MMMMMM"/"wwwwww" — it bounds every one, by 0.3% to 2.8%, never
// under. Erring long is the point: the guard exists to refuse a name that
// nearly fits, not to certify one that nearly does not.
MARK_ADV = [
    0.4722, 0.6186, 0.7067, 1.1365, 0.9437, 1.3590, 1.1828, 0.4153,   //  !"#$%&'
    0.6199, 0.6199, 0.7093, 1.1365, 0.5152, 0.5629, 0.5152, 0.4954,   // ()*+,-./
    0.9437, 0.9437, 0.9437, 0.9437, 0.9437, 0.9437, 0.9437, 0.9437,   // 01234567
    0.9437, 0.9437, 0.5424, 0.5424, 1.1365, 1.1365, 1.1365, 0.7868,   // 89:;<=>?
    1.3563, 1.0497, 1.0338, 0.9954, 1.1259, 0.9265, 0.9265, 1.1133,   // @ABCDEFG
    1.1351, 0.5047, 0.5047, 1.0510, 0.8643, 1.3497, 1.1351, 1.1530,   // HIJKLMNO
    0.9941, 1.1530, 1.0444, 0.9159, 0.9563, 1.1014, 1.0497, 1.4961,   // PQRSTUVW
    1.0457, 0.9821, 0.9835, 0.6199, 0.4954, 0.6199, 1.1365, 0.6782,   // XYZ[\]^_
    0.6782, 0.9153, 0.9709, 0.8040, 0.9709, 0.9199, 0.5208, 0.9709,   // `abcdefg
    0.9656, 0.4649, 0.4649, 0.9020, 0.4649, 1.4133, 0.9656, 0.9318,   // hijklmno
    0.9709, 0.9709, 0.6689, 0.8073, 0.6484, 0.9656, 0.8841, 1.2530,   // pqrstuvw
    0.8749, 0.8841, 0.7894, 0.9656, 0.4954, 0.9656, 1.1365            // xyz{|}~
];
// The widest glyph in the table ("W"). Anything the table cannot identify —
// a character outside printable ASCII, which this font may not even carry —
// is charged this rather than skipped, so an unknown glyph can only ever make
// the guard stricter. A word that silently costs nothing is how type walks
// off a part.
function mark_adv_max() = 1.4961;
function _mark_adv_of(c) = let (i = ord(c) - 32)
    (i >= 0 && i < len(MARK_ADV)) ? MARK_ADV[i] : mark_adv_max();
function _mark_adv_sum(word, i = 0) =
    i >= len(word) ? 0 : _mark_adv_of(word[i]) + _mark_adv_sum(word, i + 1);
// The bounded ink width of `word` set at cap height `h`. Use this rather than
// len(word)*k*h — that form cannot tell "Canary" from "CANARY" (17% apart)
// and cannot tell either from "WWWWW" (43% apart).
function mark_word_ink_w(word, h) = _mark_adv_sum(word) * h;
