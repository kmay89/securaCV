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
//  WHY PATHS AND NOT THE ARTWORK
//    The brand artwork is line work about 0.08 mm wide at badge size. No
//    nozzle lays that down. The bird here is authored as polylines and given
//    its width by a stroke parameter, so the mark is re-drawn at whatever
//    weight a given part can actually print, rather than being scaled down
//    until it disappears. Chaikin corner-cutting rounds the polylines; three
//    passes is plenty.
//
//  THE ONE RULE
//    Nothing may be drawn thinner than the stroke `t`. The tail and beak
//    taper, so they bottom out AT t rather than running to a point — a point
//    is a feature the slicer silently drops, and a dropped feature makes the
//    mark read as a different bird.
//
//  Proportions are the design: a plump upright body, a big pointed wing, a
//  long slender tail, and a splayed stance. The stance is not styling — two
//  feet need ~2.4 mm of separation at badge size or they fuse into one bar on
//  the plate.
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
function _smooth(p, cl, k = 3) = k <= 0 ? p : _smooth(_chaik(p, cl), cl, k-1);

// ── The mark in design units — 110 tall, drawn facing LEFT ─────────────────
_g_body = [[-38,32],[-35,43],[-26,50],[-12,49],[1,42],[13,29],[21,12],[25,-6],
           [21,-24],[8,-34],[-9,-36],[-24,-30],[-34,-17],[-40,0],[-41,18]];
_g_wing = [[-30,12],[-13,24],[6,18],[24,-6],[9,-17],[-11,-11],[-24,0]];
_g_tailu = [[23,3],[43,-17],[67,-43]];     // tail edges, converging to a point
_g_taill = [[12,-27],[39,-33],[67,-43]];

// Design-unit metrics, as functions so `use <>` carries them.
function mark_span() = 110;   // design-unit height the mark spans
function mark_cx()   = 3.7;   // design-unit bbox center
function mark_cy()   = -0.5;

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
module mark_bird_2d(t) {
    union() {
        _gstroke(_smooth(_g_body, true), t, true);
        _gstroke(_smooth(_g_wing, true), t, true);
        _gstroke(_smooth(_g_tailu, false, 1), t);
        _gstroke(_smooth(_g_taill, false, 1), t);
        _gtaper([[-37,26.5],[-63,27]], 15, t);       // beak: cone -> t tip
        translate([-27,34]) circle(d = t*1.35);      // eye
        _gstroke([[-12,-36],[-20,-50]], t);          // legs, splayed
        _gstroke([[4,-35],[12,-50]], t);
        _gstroke([[-27,-51],[-13,-51]], t);          // feet, clear of each other
        _gstroke([[5,-51],[19,-51]], t);
    }
}

// The bird centered on the origin at a real height `h` (mm), stroked at a real
// width `rib` (mm). This is the form a case actually wants — it does the
// design-unit bookkeeping so callers do not repeat it and get it subtly wrong.
module mark_bird(h, rib) {
    s = h / mark_span();
    scale([s, s]) translate([-mark_cx(), -mark_cy()]) mark_bird_2d(rib / s);
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
    s = h / mark_span();
    t = rib / s;                                  // stroke, in design units
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
module mark_lockup(h, rib, word = "securaCV", font = "DejaVu Sans:style=Bold") {
    // The bird's drawn extent is a little under `h` (the design span includes
    // headroom), so the gap is measured generously — at badge size the feet
    // and the ascenders are the two things that touch first, and a mark whose
    // word collides with its bird reads as a printing fault, not a lockup.
    word_h = h * 0.30;              // cap height, as a fraction of the bird
    gap    = h * 0.22;
    // Centering: the pair spans (h + gap + word_h); shift so its middle is 0.
    translate([0, (gap + word_h) / 2]) mark_bird(h, rib);
    translate([0, -(h + gap) / 2 + word_h / 2])
        text(word, size = word_h, font = font,
             halign = "center", valign = "center");
}

// Height of the whole lockup, for callers laying out around it.
function mark_lockup_h(h) = h + h*0.16 + h*0.30;
