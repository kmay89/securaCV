// ============================================================================
//  Canary — CRADLE LIBRARY (the click-on wall dock)
//
//  A wall plate takes the screws; the case CLICKS onto it and pulls off
//  again. Thermostat/HVAC construction, and it buys three things at once:
//
//    - THE MOUNT LEAVES THE CASE. The screw pattern stops being a
//      compromise the case's back has to host. Before this, a case that
//      wanted to hang both ways wore either a four-armed keyhole star or —
//      on the 4.3" dash — two different fastener systems crossed into a
//      diamond (a 75 mm M4 pair on one centerline, two keyholes on the
//      other). Both were the back plate paying for the wall's geometry.
//    - IT COMES OFF. Service, re-flash, or moving a unit to another room
//      no longer means a screwdriver and four holes. The plate stays
//      leveled on the wall; the case is the part you take.
//    - IT HIDES. The plate is a skeletal ring smaller than the case's
//      footprint and it sits between four low pads, so a docked case shows
//      a shadow line and nothing else. No visible hardware from any angle
//      you look at a wall-hung display from.
//
//  WHAT ENGAGES WHAT — two rigid, two sprung, and that split is the whole
//  durability argument:
//
//    TOP PAIR — RIGID T-STUDS. The case's weight hangs here, on geometry
//      that never flexes, so the load path has no fatigue site in it. And
//      these are not a new invention: the Ø4 shaft / Ø6.6 head T-stud is
//      the catalog's mounting standard — canary_mount_lib is its one home,
//      and the cr_stud_* numbers below CITE it rather than restating it —
//      already worn by canary_mount_adapters.scad's corner wedge, magnet
//      plate and pole plate, mating with the blind keyhole pockets cases
//      already print. The cradle moves that interface onto the wall plate
//      rather than inventing a third system.
//    BOTTOM PAIR — SPRUNG CLIPS. These carry no weight. Their only job is
//      to stop the case swinging out at the bottom, so they can be tuned
//      soft enough to release under a firm pull.
//
//  WHY THE CLIPS ARE CUT INTO THE PLATE AND NOT STOOD UP OFF IT — this is
//  the part that decides whether the clip survives. A snap arm standing
//  perpendicular to a flat-printed plate is a vertical column: flexing it
//  pulls straight across the layer bonds, which is FDM's weakest direction,
//  and it breaks at the root on about the third dock. So the arm here is a
//  TONGUE CUT OUT OF THE PLATE ITSELF by a U-slot: it lies in the plate's
//  plane and bends sideways WITHIN that plane. Bending stress then runs
//  along the extrusion lines, and no layer bond is ever loaded in tension.
//  The barb it carries is the only thing that rises in +Z.
//
//  ...and why the springs live on the PLATE, not the case: the plate is
//  ~10 g and takes twenty minutes. If a clip is ever going to be the part
//  that fails, it should be on the part you can reprint, not on the 158 g
//  case whose back also carries the vents, the QR and the board.
//
//  THE ARM IS SIZED BY STRAIN, NOT BY EYE. For a cantilever of length L
//  deflected y, with the bending dimension t, peak strain is
//        e = 1.5 * t * y / L^2
//  and PETG wants to stay under ~2 % for something that gets cycled. The
//  first cut of this arm was 16 mm long and came out at 2.9 % — over
//  budget, and the kind of number that reads fine on screen and splits on
//  the fifth dock. It is 20 mm now, tapered root-to-tip (the root carries
//  the whole moment and the tip carries none, so a taper evens the stress
//  instead of piling it at one section), and lands at 1.7 %. That number is
//  not a comment: cradle_selfcheck() recomputes it, and every adopter
//  asserts on it, so changing cr_arm_l or cr_arm_w re-derives the verdict
//  rather than letting you find out on the wall.
//
//  (Why an explicit selfcheck and not a plain top-level assert: OpenSCAD's
//  `use <>` imports definitions WITHOUT executing top-level statements, so
//  an assert sitting at this file's top level is dead the moment anybody
//  consumes the library the intended way. It silently passed for exactly
//  that reason once already.)
//
//  ORIENTATION. Plate coordinates: the plate lies in XY with +Z pointing
//  AWAY from the wall, toward the case. +Y is UP in the mounted pose. It
//  prints FACE DOWN (studs and barbs pointing up, +Z up) with no support:
//  every overhang here is a bridge under 1.4 mm or a 45° ramp.
//
//  DOCK / UNDOCK. Hang: the studs enter the case's keyhole mouths, then the
//  case drops cr_drop and the studs are captured. Push the bottom in until
//  the clips click. Undock: pull the bottom of the case away from the wall
//  — the clips' release ramp cams them open — then lift the case off the
//  studs. No tool, no release tab, no button on a visible face.
//
//  Doctrine for reuse: `use <canary_cradle_lib.scad>`, pick a span with
//  cradle_span_ok(), then
//    - UNION cradle_pads(dx, dy) into the case's back plate,
//    - SUBTRACT cradle_pad_cuts(dx, dy) from it,
//    - keep vents and graphics out of cradle_keepouts(dx, dy),
//    - and export cradle_plate(dx, dy, w, h) as the wall part.
//  The FEATURE geometry is fixed by this file so one printed plate size
//  fits any case that adopts the interface at the same span; only the span
//  and the plate outline are the adopter's business.
// ============================================================================

use <canary_mount_lib.scad>  // the catalog stud standard — the cr_stud_*
                             // numbers below are CITATIONS of it, not copies
use <canary_core_lib.scad>   // rrect2d — the catalog's shared 2D helpers

// ── THE INTERFACE CONSTANTS — these are the contract ────────────────────────
// A case printed against one set of these numbers and a plate printed
// against another do not dock. They are functions, not variables, for the
// same reason egg_tip() is in canary_vent_lib.scad: an adopter that wants a
// local name derives it instead of typing a copy that silently goes stale.
// The stud numbers go one notch further: they are read from canary_mount_lib
// rather than restated, because the dock stud IS the catalog stud — change
// it THERE and the wall plate, the keyhole adapters and every pocket move
// together, which is the only way "the same stud" stays true.
// cradle_selfcheck() asserts the citation, so a re-typed literal here cannot
// quietly fork the two. Everything cradle-specific still changes here.
function cr_plate_t()  = 3.0;   // wall plate thickness (also the clip arm's Z)
function cr_pad_h()    = 6.0;   // case-side pad height = the shadow gap the
                                // case floats off the wall. Must clear the
                                // plate (cr_plate_t) plus the screw heads
                                // sitting in it — asserted below.
function cr_stud_d()   = mount_stud_d();    // T-stud shaft Ø — the catalog
                                            // standard, canary_mount_lib
function cr_stud_head()= mount_stud_head(); // T-stud head Ø — ditto
function cr_stud_h()   = mount_stud_h();    // stud total height above the
                                            // plate face — ditto
function cr_drop()     = 6.0;   // how far the case drops to capture the studs
function cr_lip_t()    = mount_stud_stem();
                                // the keyhole lip the trapped stud head pulls
                                // against — exactly the stud's STEM height
                                // (canary_mount_lib), so the head channel
                                // starts where the head starts and a seated
                                // head carries no axial play; the slide room
                                // lives in cr_fit, not here. Thin sounds
                                // alarming until you do the sum: two studs
                                // share ~160 g, so each lip sees well under a
                                // newton in shear. It is sized against
                                // PULL-OFF, not weight.
function cr_eng()      = 1.0;   // clip barb engagement (undercut depth)
function cr_fit()      = 0.3;   // per-face clearance on every docking face

// ── ARM GEOMETRY (see the strain note in the header) ────────────────────────
function cr_arm_l()      = 20.0;  // tongue length, root → barb center
function cr_arm_w()      = 2.8;   // tongue width at the TIP (the bending dim)
function cr_arm_w_root() = 4.2;   // ...and at the root, so stress evens out
function cr_arm_slot()   = 1.6;   // U-slot around the tongue = its swing room.
                                  // Must exceed cr_eng + cr_fit or the arm
                                  // bottoms out on the plate before the barb
                                  // clears (asserted).
function cr_barb_w()     = 7.0;   // barb footprint along Y
function cr_barb_throat()= 1.2;   // how far the hook travels through the
                                  // pocket's NARROW throat before it reaches
                                  // the relief. This is the deflected stroke.
function cr_ramp_in()    = 30;    // insertion ramp — shallow, so docking is easy
function cr_ramp_out()   = 52;    // release ramp — steeper, so it holds until
                                  // you mean it, but still cams open on a pull
                                  // (90 would be a permanent latch)
// Both ramps are measured FROM THE PLATE PLANE, so a ramp's rise over a run
// is run*tan(angle). Writing that as run/tan(angle) — which is what the
// first draft did — turns the 30° insertion chamfer into a 5.2 mm plunge on
// a 3 mm arm, folds the barb's outline back under its own base, and the
// whole plate stops being a closed mesh. It rendered as "not a valid
// 2-manifold" rather than as anything you could see, which is the useful
// kind of failure.
function cr_barb_rise()  = cr_eng() * tan(cr_ramp_out());   // catch face rise
function cr_barb_drop()  = cr_arm_w() * tan(cr_ramp_in());  // insertion chamfer
function cr_barb_land()  = 0.4;                             // flat at the crest
function cr_barb_h()     = cr_barb_throat() + cr_barb_rise()
                         + cr_barb_land() + cr_barb_drop(); // barb apex
// THE HOOK IS AT THE FREE END, not at the base — and the first cut had it
// backwards, which the dock gate caught as a 0.7 mm sliver at the barb's
// outboard base. The barb enters its pocket APEX FIRST (the pad comes down
// over it), so whatever is widest sits DEEPEST when seated. Flared at the
// base, the widest part ended up right at the pocket mouth, where the pocket
// has to be narrow to squeeze the arm — so the two fought on the way in and
// nothing captured on the way out. Flared at the crest, the hook squeezes
// through the throat, springs into the relief beyond it, and its underside
// lands on the shoulder. That is a snap fit; the other was a wedge.

// Peak bending strain in the tongue at full deflection, as a fraction.
// Tapered arm: the effective bending dimension is the mean of root and tip.
function cr_strain() =
    let (t = (cr_arm_w() + cr_arm_w_root())/2, y = cr_eng() + cr_fit())
    1.5 * t * y / (cr_arm_l()*cr_arm_l());

// The interface's own sanity. A FUNCTION, because `use <>` does not run a
// library's top-level statements — see the header. Every adopter does
//     assert(cradle_selfcheck(), "...");
// so these fire wherever the interface is actually consumed. Returns true or
// never returns at all.
function cradle_selfcheck() =
    // First, the lib this lib leans on — with canary_mount_lib missing every
    // cr_stud_* below is undef, and the FIRST failing assert would otherwise
    // be a geometric one whose message points at the wrong culprit.
    assert(is_num(mount_stud_d()),
           "canary_mount_lib.scad is MISSING — the dock stud numbers are cited from it. Keep it beside canary_cradle_lib.scad.")
    assert(cr_strain() < 0.02,
           str("cradle: clip strain ", cr_strain()*100, " % exceeds the 2 % ",
               "repeated-flex budget for PETG — lengthen cr_arm_l or thin cr_arm_w"))
    assert(cr_arm_slot() > cr_eng() + cr_fit() + 0.2,
           "cradle: the U-slot is narrower than the clip's own deflection — the arm hits the plate before the barb clears")
    assert(cr_pad_h() > cr_plate_t() + 1.4,
           "cradle: pads too short — the plate (and the screw heads in it) would hold the case off its own pads")
    assert(cr_stud_head() > cr_stud_d() + 2.0,
           "cradle: T-stud head barely wider than its shaft — nothing to capture")
    assert(cr_barb_drop() < cr_barb_h(),
           "cradle: the insertion chamfer eats past the barb's own base — check the ramp angle convention")
    assert(cr_pad_h() > cr_barb_h() + 0.6,
           "cradle: the barb stands taller than the pad that has to swallow it")
    // The citations above, asserted — a re-typed literal would fork the dock
    // off the catalog stud exactly the way the local stud copies once forked
    // the keyholes (see canary_mount_lib's header). Tolerance compares: the
    // lib's stem+cone+cap sum is not bit-equal to 3.4 in floats.
    assert(abs(cr_stud_d()    - mount_stud_d())    < 1e-9,
           "cradle: dock stud shaft has forked off the catalog standard — cite canary_mount_lib, do not restate it")
    assert(abs(cr_stud_head() - mount_stud_head()) < 1e-9,
           "cradle: dock stud head has forked off the catalog standard — cite canary_mount_lib, do not restate it")
    assert(abs(cr_stud_h()    - mount_stud_h())    < 1e-6,
           "cradle: dock stud height has forked off the catalog standard — cite canary_mount_lib, do not restate it")
    assert(abs(cr_lip_t()     - mount_stud_stem()) < 1e-9,
           "cradle: the keyhole lip must fill exactly the stud's stem, or a seated head gains axial play")
    // ...and _cr_tstud's own layer stack: the '-2.0' in its stem is the lib's
    // cone + cap, so the local drawing and mount_tstud() stay the same stud
    // in every layer even though they tessellate differently (see _cr_tstud)
    assert(abs((cr_stud_h() - 2.0) - mount_stud_stem()) < 1e-6,
           "cradle: _cr_tstud's stem no longer equals the catalog stud's stem — its -2.0 must be mount cone + cap")
    true;

// Minimum sane span. The clip arms run INBOARD ALONG X from the bottom
// corners, not up the sides — which is the difference between a plate that
// hides behind a display and one that peeks out past it. Displays are wide
// and short: routing a 20 mm arm up the height forced the plate to 72.6 mm
// against the 4.3"'s 73.2 mm case and it would have shown at the top and
// bottom edges. Along the width there is room to spare, and the arm still
// bends in the plate's plane, which is the only thing the layer-direction
// argument actually cares about.
function cradle_span_ok(dx, dy) =
    dx*2 > 2*(cr_arm_l() + cr_barb_w()) + 10   // the two arms must not meet
    && dy*2 > cr_drop() + cr_barb_w() + 14;

// Where the four features sit, given a span. Top pair (+y) are the rigid
// studs; bottom pair (-y) are the sprung clips. ONE list, so the plate, the
// pads and the keepouts can never drift apart.
function cradle_studs(dx, dy) = [for (sx = [1,-1]) [sx*dx,  dy]];
function cradle_clips(dx, dy) = [for (sx = [1,-1]) [sx*dx, -dy]];
// …and where the CASE's clip features go, which is NOT the same place.
//
// The case is offered to the studs and then DROPS cr_drop to capture them,
// so in the case's own coordinates every plate feature ends up cr_drop
// higher than where it started. The keyhole already knew that — its channel
// runs cr_drop past its mouth — but the clip pocket was drawn at the barb's
// un-dropped position, which made "studs captured" and "clips engaged" two
// states the case could not be in at once: seat the studs and each barb is
// 6 mm clear of the pocket it is supposed to be latched into.
//
// So the case's clip features live cr_drop above the plate's barbs, and the
// case then engages them the way a thermostat does: hook the top, drop it,
// swing the bottom in. The clips only ever move in Z, never along the drop,
// which is also why the arms may run horizontally — a clip that deflected
// along the drop axis would be cammed by its own pocket on the way down.
function cradle_case_clips(dx, dy) = [for (sx = [1,-1]) [sx*dx, -dy + cr_drop()]];

// Rounded footprint each case-side pad claims, as [x, y, rx, ry] — the same
// shape the grille/graphics keepout lists in the adopters speak.
function cradle_keepouts(dx, dy) =
    concat([for (p = cradle_studs(dx, dy))
                [p[0], p[1] + cr_drop()/2,
                 cr_stud_head()/2 + 4.5, (cr_drop() + cr_stud_head())/2 + 4.5]],
           [for (p = cradle_case_clips(dx, dy))
                [p[0], p[1], cr_barb_w()/2 + 4.5, cr_barb_w()/2 + 4.5]]);

// (The rounded rectangle here is the catalog's own rrect2d — canary_core_lib.
// This file drew a private copy once; a copy is a fork, see that lib's header.)

// Places a clip's local frame at a bottom corner. The clip is DRAWN with its
// arm along +y and its barb protruding +x; this lays it down so the arm runs
// inboard along the width and the barb protrudes upward. One transform, used
// by the barb, its slot AND the case-side pocket, so the three can never
// drift out of register — which they would the first time somebody adjusted
// one of them by hand.
// `ddy` is the drop offset: 0 for the plate's own barbs, cr_drop() for the
// case-side pockets that receive them once the case has dropped onto its
// studs. See cradle_case_clips().
module _cr_clip_at(dx, dy, sx, ddy = 0) {
    translate([sx*dx, -dy + ddy, 0]) mirror([sx > 0 ? 0 : 1, 0, 0])
        rotate([0, 0, 90]) children();
}

// ── THE WALL PLATE ──────────────────────────────────────────────────────────
// A skeletal ring, not a slab: it only has to connect four features and two
// screws, and a slab would blanket whatever the case's back is doing behind
// it (the 7" back is most of a QR symbol, two adhesive-rail moats and ~60
// vent eggs). Local z0 = the face that lies ON THE WALL; +z toward the case.
// Prints face-down exactly as modeled.
module cradle_plate(dx, dy, w, h, rail = 0) {
    r = rail > 0 ? rail : cr_rail_min();
    assert(w/2 - dx < r && h/2 - dy - cr_barb_w()/2 < r,
           "cradle: a dock feature floats over the ring's opening instead of standing on its band — widen `rail` (see cr_rail_min)");
    difference() {
        union() {
            // the ring itself
            linear_extrude(cr_plate_t())
                difference() {
                    rrect2d(w, h, 6);
                    rrect2d(w - 2*r, h - 2*r, 4);
                }
            // ...with a spine across the middle, so the two screws sit on
            // solid material and the ring cannot lozenge under a knock
            linear_extrude(cr_plate_t()) rrect2d(r, h - 2*r + 0.02, 0);
            // rigid T-studs, top pair
            for (p = cradle_studs(dx, dy)) translate([p[0], p[1], cr_plate_t()])
                _cr_tstud();
            // sprung clip tongues + barbs, bottom pair
            for (sx = [1, -1]) _cr_clip_at(dx, dy, sx) _cr_clip();
        }
        // the U-slots that free the tongues (cut AFTER the union, so they
        // also part the tongue from the ring it was drawn over)
        for (sx = [1, -1]) _cr_clip_at(dx, dy, sx) _cr_clip_slot();
        // two wall screws on the spine, SLOTTED in Y so the plate levels
        // without re-drilling, counterbored so the heads sit below the face
        // the case's pads land on
        for (sy = [1, -1]) translate([0, sy*(h/2 - r - 8), 0]) {
            hull() for (dyy = [-2.5, 2.5]) translate([0, dyy, -0.1])
                cylinder(d = 4.4, h = cr_plate_t() + 0.2);
            hull() for (dyy = [-2.5, 2.5]) translate([0, dyy, cr_plate_t() - 2.0])
                cylinder(d = 8.6, h = 2.1);
        }
    }
}

// T-stud — the catalog's standard mushroom (Ø4 shaft, Ø6.6 head), drawn from
// the CITED canary_mount_lib numbers. The DRAWING stays local on purpose:
// mount_tstud()'s stem carries a +0.01 union overlap into its cone where this
// one butts them at the same diameter — the same solid, but it tessellates
// differently, and a released plate must not move under a dedup (verified:
// swapping the call in moves the mesh hash). cradle_selfcheck() pins this
// drawing's layer stack to the lib's, so the two can only ever differ in
// facets, never in shape. The head's 1.3 mm overhang is a bridge, not a
// cliff: it prints off the cone below it.
module _cr_tstud() {
    cylinder(d = cr_stud_d(), h = cr_stud_h() - 2.0);
    translate([0, 0, cr_stud_h() - 2.0])
        cylinder(d1 = cr_stud_d(), d2 = cr_stud_head(), h = 1.2);
    translate([0, 0, cr_stud_h() - 0.8]) cylinder(d = cr_stud_head(), h = 0.8);
}

// One clip, drawn for the +x corner (the -x one is mirrored by the caller).
// The tongue runs UP from the barb toward the plate's middle, so its root is
// in solid ring material and its tip — the barb — sits at the corner.
// The tongue's FOOTPRINT — the tapered arm plus the pad the barb stands on.
// The U-slot is derived from this same outline (see _cr_clip_slot), which is
// the only way the two can be guaranteed to agree: the first cut drew them
// independently, put the parting cut at the barb's flare instead of at the
// arm's edge, and left a 0.3–1.0 mm strip of plate welding the "free" side
// of the tongue to the ring for its whole length. The arm could not deflect
// at all, so the barb could never have passed its own pocket throat — and
// nothing in a render shows you that, because a welded tongue looks exactly
// like a free one.
module _cr_tongue2d() {
    polygon([[-cr_arm_w()/2,      -cr_barb_w()/2],
             [ cr_arm_w()/2,      -cr_barb_w()/2],
             [ cr_arm_w_root()/2,  cr_arm_l()],
             [-cr_arm_w_root()/2,  cr_arm_l()]]);
    translate([-cr_arm_w()/2, -cr_barb_w()/2])
        square([cr_arm_w() + cr_eng(), cr_barb_w()]);
}

module _cr_clip() {
    // tapered tongue, lying in the plate's plane
    linear_extrude(cr_plate_t()) _cr_tongue2d();
    // barb: sits ON the plate's top face and rises cr_barb_h. Section in XZ,
    // read anticlockwise from the inboard base:
    //   · outboard wall straight up to cr_barb_land — the catch face
    //   · release ramp inboard-and-up (cam-out under a pull)
    //   · insertion chamfer back down inboard (what docking slides along)
    // Every one of those is a TOP surface, so the barb prints with no
    // overhang at all; the only unsupported thing on the plate is the
    // T-stud head's 1.3 mm collar, which bridges off its own cone.
    translate([0, cr_barb_w()/2, cr_plate_t()])
        rotate([90, 0, 0]) linear_extrude(cr_barb_w())
            polygon([
                [-cr_arm_w()/2, 0],
                [ cr_arm_w()/2, 0],
                // straight column through the pocket's throat…
                [ cr_arm_w()/2, cr_barb_throat()],
                // …then the hook flares outboard. This face is the CATCH:
                // sloped, so a pull cams the arm open instead of locking.
                [ cr_arm_w()/2 + cr_eng(), cr_barb_throat() + cr_barb_rise()],
                [ cr_arm_w()/2 + cr_eng(),
                  cr_barb_throat() + cr_barb_rise() + cr_barb_land()],
                // insertion chamfer, crest inboard: the descending pocket
                // wall rides this and squeezes the arm in
                [-cr_arm_w()/2, cr_barb_h()]]);
}

// The U-slot that turns the tongue into a spring: swing room on the inboard
// side (where it deflects to), a hair on the outboard side so it is parted
// from the ring, and a rounded relief at the ROOT — a square internal corner
// there is the crack starter, same doctrine as every boss root in this
// catalog.
// The U-slot: a uniform cr_arm_slot gap all the way around the tongue's own
// outline, opened everywhere EXCEPT across the root. Deriving it by offset
// rather than drawing it by hand means it tracks the taper and the barb's
// flare automatically, and it cannot be left short on one side — which is
// exactly how the tongue ended up welded to the ring the first time.
// offset() rounds the corners for free, so the root fillet the old version
// bolted on as a circle is now just what the geometry does.
module _cr_clip_slot() {
    translate([0, 0, -0.1]) linear_extrude(cr_plate_t() + 0.2)
        difference() {
            offset(r = cr_arm_slot()) _cr_tongue2d();
            _cr_tongue2d();
            // the root stays attached — this is a cantilever, not a hole
            translate([-(cr_arm_w_root()/2 + cr_arm_slot() + 1), cr_arm_l() - 0.01])
                square([cr_arm_w_root() + 2*cr_arm_slot() + 2, cr_arm_slot() + 2]);
        }
}

// The barb's mating pocket, drawn in the CLIP'S OWN local frame so it is
// placed by the same _cr_clip_at() transform the barb is — see there.
// Measuring up from the pad's tip (which rests on the plate):
//   · a NARROW throat, only the tongue's own width — the barb is wider by
//     cr_eng, so getting past this is what deflects the arm
//   · then it opens OUT by cr_eng, and the barb springs into that relief
// The ledge where narrow becomes wide is what the barb's catch face bears on
// when the case is pulled. Printed back-face-down that ledge is a 0.5 mm
// bridge over the throat — the shortest kind there is.
module _cr_clip_pocket() {
    // the THROAT: only the arm's own width, nearest the pad's tip. The hook
    // is cr_eng wider, so passing through here is what deflects the arm.
    translate([-cr_arm_w()/2 - cr_fit(), -cr_barb_w()/2 - cr_fit(), -0.02])
        cube([cr_arm_w() + 2*cr_fit(),
              cr_barb_w() + 2*cr_fit(),
              cr_barb_throat() + cr_fit() + 0.02]);
    // the RELIEF beyond it, where the hook springs back out. The step
    // between the two is the shoulder the hook's catch face pulls against.
    translate([-cr_arm_w()/2 - cr_fit(), -cr_barb_w()/2 - cr_fit(),
               cr_barb_throat() + cr_fit()])
        cube([cr_arm_w() + cr_eng() + 2*cr_fit(),
              cr_barb_w() + 2*cr_fit(),
              cr_barb_h() - cr_barb_throat() + cr_fit() + 0.6]);
}

// ── THE CASE SIDE ───────────────────────────────────────────────────────────
// Four low pads standing proud of the case's back plate. They are what the
// case rests on, so they set the shadow gap; they are also thick enough to
// host a blind pocket without ever breaking into the cavity — nothing here
// cuts through the back plate, which is the whole reason the case can carry
// a dock and stay sealed against dust behind it.
//
// UNION this into the case's back, then SUBTRACT cradle_pad_cuts().
// z0 = the case's OUTER back face; the pads grow away from the case (+z),
// so their tips at z0 + cr_pad_h are what land on the plate.
//
// They start cr_sink BELOW z0 on purpose. A pad whose base sits exactly on
// the skin shares a face with it, and a shared face is not a join: OpenSCAD
// hands CGAL two solids touching at a plane, admesh counts them as separate
// parts, and the export is a "3 parts" mesh that no slicer will treat as one
// object. (That is the same zero-volume-sheet class the 7" battery probes
// carry a `drop` for.) Sinking the pad into the plate makes the union a real
// intersection.
function cr_sink() = 0.8;
module cradle_pads(dx, dy, z0) {
    for (p = cradle_studs(dx, dy))
        translate([p[0], p[1] + cr_drop()/2, z0 - cr_sink()])
            linear_extrude(cr_pad_h() + cr_sink())
                rrect2d(cr_stud_head() + 9, cr_drop() + cr_stud_head() + 9, 4);
    // the clip pads are elongated along X, because that is the way the barb
    // protrudes and springs once the arm is laid inboard along the width
    for (p = cradle_case_clips(dx, dy))
        translate([p[0], p[1], z0 - cr_sink()])
            linear_extrude(cr_pad_h() + cr_sink())
                rrect2d(cr_barb_w() + 9, cr_barb_w() + cr_eng() + 11, 4);
}

// ...and the pockets in them. All BLIND — nothing here reaches the case's
// own skin, which is what lets a case carry a dock and still be closed
// behind it.
//
// Everything below is drawn in a frame whose z = 0 is the PAD TIP and whose
// +z runs INTO the pad. That is exactly the plate's own frame reflected, so
// a feature at (x, y, z) here meets the plate feature at (x, y, z) there —
// the two halves of the interface are written in the same numbers instead of
// in one set of numbers and somebody's mental subtraction.
module cradle_pad_cuts(dx, dy, z0) {
    translate([0, 0, z0 + cr_pad_h()]) mirror([0, 0, 1]) {
        // TOP: a blind keyhole per pad — mouth first, channel running UP, so
        // the case is offered to the studs and then DROPS to lock. Same
        // sense as every keyhole in this catalog, so the hand knows it.
        for (p = cradle_studs(dx, dy)) translate([p[0], p[1], -0.01]) {
            // the mouth: swallows the whole stud, head and all
            cylinder(d = cr_stud_head() + 2*cr_fit(),
                     h = cr_stud_h() + cr_fit() + 0.02);
            // the travel slot at the TIP face: only shaft-wide, so the head
            // cannot come back out through it — this is the trap
            hull() for (yy = [0, cr_drop()]) translate([0, yy, 0])
                cylinder(d = cr_stud_d() + 2*cr_fit(),
                         h = cr_stud_h() + cr_fit() + 0.02);
            // ...and the head's own channel, BELOW the lip, so the head has
            // somewhere to travel while the shaft rides the slot above it
            translate([0, 0, cr_lip_t()])
                hull() for (yy = [0, cr_drop()]) translate([0, yy, 0])
                    cylinder(d = cr_stud_head() + 2*cr_fit(),
                             h = cr_stud_h() - cr_lip_t() + cr_fit());
        }
        // BOTTOM: the stepped barb pocket, placed by the very same transform
        // the barb itself is — see _cr_clip_pocket().
        for (sx = [1, -1]) translate([0, 0, -0.01])
            _cr_clip_at(dx, dy, sx, cr_drop()) _cr_clip_pocket();
    }
}

// The plate, moved into its DOCKED position in the case's own coordinates,
// given the case's outer back face z0. Wrap the case's plate part in this
// and intersect with the case: the result must be EMPTY, which is the one
// check that actually proves the dock fits — every stud in its keyhole,
// every barb in its pocket, every clearance honored at once. Any tightened
// tolerance or moved feature that would bind shows up here as solid.
//
// The mirror is not decoration: the plate's +z points AT the case, so the
// two frames are reflections. cradle_pad_cuts() uses the same reflection,
// which is why the pockets and the features they swallow stay in register.
// `gap` hovers the plate off the pad tips. It defaults to a hair rather than
// to zero on purpose: seated, those two faces are meant to TOUCH, and a face
// lying exactly on another mints the zero-volume phantom sheets that CGAL
// reports as "may not be a valid 2-manifold" — the render fails instead of
// the check answering. Same reason the battery probes carry a `drop`. Pass
// gap = 0 only if you want to model the true seated contact for a picture.
module cradle_docked(z0, gap = 0.02) {
    translate([0, cr_drop(), z0 + cr_pad_h() + cr_plate_t() + gap])
        mirror([0, 0, 1]) children();
}

// The plate's own outline, for an adopter that wants to check its back has
// room before committing to a span.
function cradle_plate_w(dx) = 2*dx + cr_stud_head() + 16;
function cradle_plate_h(dy) = 2*dy + cr_drop() + cr_stud_head() + 16;

// How wide the ring's band has to be for the four features to land ON it.
// This is DERIVED, and it has to be: at the plate heights above, a stud sits
// (cr_drop + cr_stud_head + 16)/2 = 14.3 mm in from the top edge, so a
// hand-picked 12 mm band left it hanging 2.3 mm over the opening — a stud
// printing in mid-air, which reads on screen as a small scallop at the
// corner rather than as anything obviously broken. Deriving the band and
// asserting it is the difference between finding that here and finding it
// when the first plate comes off the bed with two stubs.
function cr_rail_min() = (cr_drop() + cr_stud_head() + 16)/2 + 2;
