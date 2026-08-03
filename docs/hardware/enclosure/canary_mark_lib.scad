// ============================================================================
//  Canary — THE MARK (the bird, as printable geometry)
//
//  One bird for every Canary enclosure. Same doctrine as canary_vent_lib.scad:
//  the shape lives here once, consumers `use <canary_mark_lib.scad>` and draw
//  it, and a change here reaches every case rather than one of them.
//
//  WHY IT IS AUTHORED, NOT TRACED. The brand artwork is line work about
//  0.08 mm wide at badge size — no nozzle lays that down, so a trace would
//  produce a mark that renders beautifully and prints as a smear. These are
//  hand-authored paths with Chaikin corner-cutting, and the mark's NARROWEST
//  feature is exactly one stroke wide: tapers stop AT the stroke rather than
//  running to a point, because a point is a feature the slicer drops.
//
//  EVERYTHING CROSSES AS A FUNCTION, not a variable. OpenSCAD's `use <>`
//  imports modules and functions but NOT variables, so a consumer that needs
//  the design-unit span or the bbox center must call for it. This is the same
//  trap egg_tip() exists to avoid in the vent library: a consumer that copies
//  the number instead of calling for it is a consumer that silently keeps the
//  old mark the first time this file changes.
//
//  DESIGN UNITS. The paths are drawn 110 units tall, facing LEFT, and are
//  NOT centered on the origin — canary_mark_cx/cy() give the bbox center so a
//  consumer can place the mark by its middle. Scale = target_height / span().
// ============================================================================

function canary_mark_span() = 110;   // design-unit height the mark spans
function canary_mark_cx()   = 3.7;   // design-unit bbox center, x
function canary_mark_cy()   = -0.5;  // design-unit bbox center, y

// The paths, in design units. Proportions are the point: a plump upright body,
// a big pointed wing, a long slender tail and a splayed stance. The stance is
// not styling — two feet need ~2.4 mm of separation at badge size or they fuse
// into one bar on the plate.
function _cm_body() = [[-38,32],[-35,43],[-26,50],[-12,49],[1,42],[13,29],
    [21,12],[25,-6],[21,-24],[8,-34],[-9,-36],[-24,-30],[-34,-17],[-40,0],[-41,18]];
function _cm_wing() = [[-30,12],[-13,24],[6,18],[24,-6],[9,-17],[-11,-11],[-24,0]];
function _cm_tailu() = [[23,3],[43,-17],[67,-43]];   // tail edges, converging
function _cm_taill() = [[12,-27],[39,-33],[67,-43]];

// Chaikin corner-cutting; three passes is plenty to read as drawn curves.
function _cm_chaik(p, cl) = let(n = len(p))
    [ for (i = [0 : (cl ? n-1 : n-2)]) each
        let(a = p[i], b = p[(i+1) % n])
        [ [0.75*a[0] + 0.25*b[0], 0.75*a[1] + 0.25*b[1]],
          [0.25*a[0] + 0.75*b[0], 0.25*a[1] + 0.75*b[1]] ] ];
function _cm_smooth(p, cl, k = 3) =
    k <= 0 ? p : _cm_smooth(_cm_chaik(p, cl), cl, k-1);

module _cm_stroke(pts, t, closed = false) {
    n = len(pts);
    for (i = [0:n-2]) hull() { translate(pts[i]) circle(d = t);
                               translate(pts[i+1]) circle(d = t); }
    if (closed) hull() { translate(pts[n-1]) circle(d = t);
                         translate(pts[0]) circle(d = t); }
}
module _cm_taper(pts, t0, t1) {          // width runs t0 -> t1 along the path
    n = len(pts);
    for (i = [0:n-2]) hull() {
        translate(pts[i])   circle(d = t0 + (t1-t0)*i/(n-1));
        translate(pts[i+1]) circle(d = t0 + (t1-t0)*(i+1)/(n-1));
    }
}

// The mark as a 2D profile, every stroke `t` design-units wide. Nothing here
// may be drawn thinner than t — the tail and beak taper, so they bottom out AT
// t rather than running to a point.
module canary_mark_2d(t) {
    union() {
        _cm_stroke(_cm_smooth(_cm_body(), true), t, true);
        _cm_stroke(_cm_smooth(_cm_wing(), true), t, true);
        _cm_stroke(_cm_smooth(_cm_tailu(), false, 1), t);
        _cm_stroke(_cm_smooth(_cm_taill(), false, 1), t);
        _cm_taper([[-37,26.5],[-63,27]], 15, t);     // beak: cone -> t tip
        translate([-27,34]) circle(d = t*1.35);      // eye
        _cm_stroke([[-12,-36],[-20,-50]], t);        // legs, splayed
        _cm_stroke([[4,-35],[12,-50]], t);
        _cm_stroke([[-27,-51],[-13,-51]], t);        // feet, clear of each other
        _cm_stroke([[5,-51],[19,-51]], t);
    }
}

// The mark RAISED, centered on the origin, sitting on z = 0 and standing `ez`
// tall. `h` is the finished height of the mark in mm, `rib` the stroke width in
// mm, `crown` how far the stroke tops draw in.
//
// The strokes are a short stack of inward offsets on a quarter-circle profile,
// so each carries a DOMED crown rather than a flat slab top — that is what
// makes an embossed mark catch light like metal instead of reading as a
// sticker. The footprint stays a full rib wide; only the crown draws in.
module canary_mark_emboss(h, ez, rib = 0.7, crown = 0.15) {
    s = h / canary_mark_span();
    t = rib / s;                                  // stroke, in design units
    n = crown > 0 ? 4 : 1;
    for (i = [0:n-1]) {
        // the inset runs 0 at the base to the FULL crown on the top slice; the
        // slice's z is a separate ramp, or the last one would sit proud
        f   = n > 1 ? i/(n - 1) : 0;
        ins = (crown / s) * (1 - sqrt(1 - f*f));
        translate([0, 0, i*ez/n]) linear_extrude(ez/n + 0.01)
            scale([s, s]) translate([-canary_mark_cx(), -canary_mark_cy()])
                offset(r = -ins) canary_mark_2d(t);
    }
}
