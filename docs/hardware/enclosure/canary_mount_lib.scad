// ============================================================================
//  Canary MOUNT library — the catalog's stud/keyhole hanging interface
//
//  NOT A PRINTABLE PART. `use <canary_mount_lib.scad>` from a case file.
//
//  ONE interface hangs every Canary: a pair of T-studs (Ø4.0 stem, Ø6.6
//  head, 3.4 mm tall) mating blind keyhole pockets (Ø7.0 head pass, 4.2
//  shank slot, 8.0 travel, 3.5 deep behind a 1.0 face web). The stud was
//  drawn from local constants in six files and the pocket in eight, and the
//  copies had already drifted: kh_head_h 3.0 and kh_slot_l 7.0 had crept
//  into three files, which is a pocket the standard stud bottoms out in
//  0.4 mm early and a slide the head never finishes. An interface that
//  lives in one place cannot disagree with itself — that is this file.
//
//  THE CONTRACT (what mates with what):
//    * stud stem 1.4 = pocket face web 1.0 + 0.4 slide room;
//    * stud total 3.4 vs pocket depth 3.5 = 0.1 ceiling clearance;
//    * the optional click detent stands 0.25 proud of the head-channel
//      ceiling, so a parked head carries 0.15 of squeeze — slide on, CLICK,
//      and the mate stays until a firm pull back (the coupon's POCKET
//      station is this pair, print-tested at stud_gap 30);
//    * pockets are BLIND — they never breach a case cavity, so weather
//      sealing survives the wall mount;
//    * slots point so the case slides DOWN to seat: gravity is the latch.
//
//  A SECOND, drop-durable hanger lives here too: the DOVETAIL LUG
//  (mount_dovelug / mount_dovelug_pocket, below the click detent) — no
//  stem to snap, the same 8.0 drop. The doorbell hangs on it; the coupon's
//  POCKET station prints it between the keyholes.
//
//  The stud's Ø6.6 head overhangs its Ø4 stem by 1.3 mm — a bridge, not a
//  cliff: it prints off the 1.2 mm cone below it (canary_cradle_lib's dock
//  studs print the same way, and read their numbers from here).
//
//  Cases that expose kh_* Customizer knobs keep them — a printer gets dialed
//  in per machine — but the knob DEFAULTS cite these functions' values, and
//  the fit coupon is where a deviation earns its keep.
//
//  `use<>` does not run top-level statements: call mount_selfcheck() from an
//  adopter (the fit coupon does).
// ============================================================================

// ---------------------------------------------------------------------------
//  The interface constants — functions so `use<>` carries them
// ---------------------------------------------------------------------------
function mount_stud_d()      = 4.0;   // stem diameter
function mount_stud_head()   = 6.6;   // head disc diameter
function mount_stud_stem()   = 1.4;   // stem height (= face web 1.0 + 0.4 slide)
function mount_stud_cone()   = 1.2;   // stem->head cone (the head's print support)
function mount_stud_cap()    = 0.8;   // head disc thickness
function mount_stud_h()      = mount_stud_stem() + mount_stud_cone() + mount_stud_cap();  // 3.4

function mount_kh_head_d()   = 7.0;   // head pass hole (#6 / M3.5 pan head)
function mount_kh_shank_d()  = 4.2;   // shank slot width
function mount_kh_slot_l()   = 8.0;   // slot travel
function mount_kh_head_h()   = 3.5;   // total pocket depth (face web included)
function mount_kh_face()     = 1.0;   // face web the head grips behind
function mount_kh_click()    = 0.25;  // detent bump proud of the channel ceiling

// ---------------------------------------------------------------------------
//  T-stud — the catalog's standard mushroom, +Z axis, base at the origin.
//  Callers translate/rotate into place. Geometry identical to the six local
//  drawings it replaces (stem overlaps the cone by 0.01: a shared face is
//  not a join — see the catalog's CGAL hygiene rule).
// ---------------------------------------------------------------------------
module mount_tstud(d = mount_stud_d(), head = mount_stud_head(),
                   stem = mount_stud_stem(), cone = mount_stud_cone(),
                   cap = mount_stud_cap()) {
    cylinder(d = d, h = stem + 0.01);
    translate([0, 0, stem]) {
        cylinder(d1 = d, d2 = head, h = cone);
        translate([0, 0, cone]) cylinder(d = head, h = cap);
    }
}

// ---------------------------------------------------------------------------
//  Blind keyhole pocket — subtract from a back thickened by at least
//  head_h + 1.5 (the +1.5 is the web that keeps the pocket blind; the
//  adopters assert it). Drawn NATIVELY along the chosen axis — no rotate —
//  so an adopting case renders vertex-for-vertex what its local copy drew:
//  a rotated circle tessellates differently, and released STLs must not
//  move under a dedup. Composition matters for that identity too: a local
//  copy that took its center via an outer translate([c,0,0]) must call
//  mount_keyhole_pocket(0, ...) inside that same translate — passing c here
//  as well would double the offset, and circles translated by a different
//  path can land on different vertices.
//
//    c    — pocket center along the slot axis
//    z0   — the outer back face (pocket cuts up/in from here)
//    axis — "x" (slot toward +X: WAP-family, case hangs USB-down) or
//           "y" (slot toward +Y: the doorbell plate's drop-on studs)
//  The head circle sits at c - slot_l/2, the slot runs toward +axis:
//  offer the head in, let the case drop, the shank rides to the far end.
// ---------------------------------------------------------------------------
module mount_keyhole_pocket(c, z0, axis = "x",
                            head_d = mount_kh_head_d(),
                            shank_d = mount_kh_shank_d(),
                            slot_l = mount_kh_slot_l(),
                            head_h = mount_kh_head_h(),
                            face = mount_kh_face()) {
    assert(axis == "x" || axis == "y", "mount_keyhole_pocket: axis is \"x\" or \"y\"");
    assert(head_d > shank_d, "mount_keyhole_pocket: head_d must exceed shank_d");
    assert(head_h > face,    "mount_keyhole_pocket: head_h includes the face web");
    assert(slot_l > 0,       "mount_keyhole_pocket: slot_l must be positive");
    c0 = c - slot_l/2;                 // screw-head pass hole center
    c1 = c + slot_l/2;                 // slot far end
    module at(p) { if (axis == "x") translate([p, 0]) children();
                   else             translate([0, p]) children(); }
    union() {
        // face opening: head circle + shank slot
        translate([0, 0, z0 - 0.1]) linear_extrude(face + 0.1) {
            at(c0) circle(d = head_d);
            hull() { at(c0) circle(d = shank_d);
                     at(c1) circle(d = shank_d); }
        }
        // wider head cavity behind the face web (the screw head slides here)
        translate([0, 0, z0 + face]) linear_extrude(head_h - face)
            hull() { at(c0) circle(d = head_d + 0.6);
                     at(c1) circle(d = head_d + 0.6); }
    }
}

// ---------------------------------------------------------------------------
//  Click detent — a shallow dome on the head-channel ceiling at slot
//  mid-travel: cleared during stud insertion, cammed over on the slide, and
//  the head parks BEHIND it. ADD to the part (it is a bump in the pocket's
//  void), at the pocket's ceiling plane: z = z0 + head_h measured the same
//  way the pocket was cut. The coupon's POCKET station prints this pair.
// ---------------------------------------------------------------------------
module mount_keyhole_click(px, py, z, click = mount_kh_click()) {
    if (click > 0) translate([px, py, z]) scale([0.9, 0.9, click]) sphere(1);
}

// ---------------------------------------------------------------------------
//  DOVETAIL LUG — the drop-durable hanger (the doorbell's, 2026-10).
//
//  The T-stud above is a Ø4 stem printed upright: its whole hold is one
//  12.6 mm² layer interface at the stem root, under a sharp step into the
//  cone, and a knock on the hung case loads exactly that interface in peel.
//  A dropped doorbell plate snapped its studs off there. The lug designs that
//  failure out instead of thickening it:
//    * NO NECK. The lug is a dovetail prism: its narrowest section is the
//      6.0 x 7.0 root (42 mm², 3.3x the stem), and a 45° root chamfer blends
//      it into the plate so no sharp re-entrant corner concentrates the load;
//    * NO KNIFE EDGE in the pocket. The lip the lug hooks under is a 1.2 mm
//      straight land and a 45° flank, rooted along the pocket's whole length
//      in the part — a wedge of the part, not a tab sticking out of it;
//    * the same 8.0 drop as the keyhole (mount_kh_slot_l): offer the window
//      over the lug, drop, the lug runs under the lips. Gravity is the latch.
//  Profile (XZ, the slide along Y), lug base on z = 0:
//      ___________  z = h = neck_h + flare   (top_w = neck_w + 2*flare)
//      \         /  45° flare — the lips bear here
//       |       |   neck_h straight land
//      /         \  45° root chamfer (root)
//  It prints upright off a plate with 45° flanks (self-supporting) and its
//  pocket prints back-face down with a flat 9.6 mm bridge for a ceiling.
// ---------------------------------------------------------------------------
function mount_dt_neck_w()  = 6.0;   // lug width at the root, above the root chamfer
function mount_dt_neck_h()  = 1.2;   // straight land height (the pocket lip's thickness at the face)
function mount_dt_flare()   = 1.6;   // 45° flare above the land: each lip overlaps the lug by this
function mount_dt_root()    = 0.5;   // 45° root chamfer, each side
function mount_dt_len()     = 7.0;   // lug length along the slide
function mount_dt_travel()  = 8.0;   // the drop — mount_kh_slot_l(), the catalog's one slide
function mount_dt_clear()   = 0.2;   // pocket clearance per face (core_tol_slide)
function mount_dt_relief()  = 0.3;   // pocket ceiling over the lug top
function mount_dt_top_w()   = mount_dt_neck_w() + 2*mount_dt_flare();      // 9.2
function mount_dt_h()       = mount_dt_neck_h() + mount_dt_flare();        // 2.8
function mount_dt_depth()   = mount_dt_h() + mount_dt_relief();            // 3.1
function mount_dt_window_w(clear = mount_dt_clear()) = mount_dt_top_w() + 2*clear + 0.4;
function mount_dt_window_l(len = mount_dt_len(), clear = mount_dt_clear()) = len + 2*clear + 0.4;
// the pocket's footprint along the slide, from the parked lug center:
// [window start, channel end] — what an adopter asserts clear of its other cuts
function mount_dt_pocket_y(cy, len = mount_dt_len(), travel = mount_dt_travel(),
                           clear = mount_dt_clear()) =
    [cy - travel - mount_dt_window_l(len, clear)/2, cy + len/2 + 0.6];

// the lug's section (XZ); `sink` runs the root on down into the plate so the
// lug fuses (CGAL hygiene: a shared face is not a join)
module mount_dt_profile2d(sink = 0.3) {
    nw = mount_dt_neck_w()/2; tw = mount_dt_top_w()/2; r = mount_dt_root();
    nh = mount_dt_neck_h();  h = mount_dt_h();
    polygon([[-nw - r, -sink], [nw + r, -sink], [nw + r, 0], [nw, r], [nw, nh],
             [tw, h], [-tw, h], [-nw, nh], [-nw, r], [-nw - r, 0]]);
}

// ADD: the lug, slide along Y, centered on (0, cy), base on z = 0, +Z up
module mount_dovelug(cy = 0, len = mount_dt_len(), sink = 0.3) {
    translate([0, cy + len/2, 0]) rotate([90, 0, 0])
        linear_extrude(len) mount_dt_profile2d(sink);
}

// SUBTRACT: the blind pocket, cut +Z from the outer face z0 of the hung part.
// cy is where the lug PARKS; the drop-in window sits `travel` below it (-Y),
// so the part is offered `travel` high and slides down onto the lug. A 1 mm
// funnel leads the lug from the window into the dovetail; the channel runs
// 0.6 past the parked lug so the case's stop is never the lug.
module mount_dovelug_pocket(cy, z0, len = mount_dt_len(), travel = mount_dt_travel(),
                            clear = mount_dt_clear()) {
    assert(travel >= mount_dt_window_l(len, clear),
           "mount_dovelug_pocket: the drop must carry the lug wholly out of its window");
    d  = mount_dt_depth();
    ww = mount_dt_window_w(clear);  wl = mount_dt_window_l(len, clear);
    y_we = cy - travel + wl/2;                   // window's upper end = channel start
    y_ce = cy + len/2 + 0.6;                     // channel end
    module section2d() {
        offset(delta = clear) mount_dt_profile2d(sink = 1.0);
        translate([-mount_dt_top_w()/2 - clear, 0]) square([mount_dt_top_w() + 2*clear, d]);
    }
    translate([0, 0, z0]) {
        // the drop-in window, full depth
        translate([-ww/2, cy - travel - wl/2, -0.1]) cube([ww, wl, d + 0.1]);
        // the dovetail channel
        translate([0, y_ce, 0]) rotate([90, 0, 0]) linear_extrude(y_ce - y_we + 0.01) section2d();
        // the funnel from window to channel
        hull() {
            translate([-ww/2, y_we - 0.01, -0.1]) cube([ww, 0.01, d + 0.1]);
            translate([0, y_we + 1.0, 0]) rotate([90, 0, 0]) linear_extrude(0.01) section2d();
        }
    }
}

// ---------------------------------------------------------------------------
//  Self-check — the mating arithmetic the header promises. Call once from an
//  adopter (the fit coupon does).
// ---------------------------------------------------------------------------
module mount_selfcheck() {
    // the dovetail lug: it must drop clear of its window, fit the depth a
    // keyhole pocket already budgets, and out-hold the stud it replaces
    assert(mount_dt_travel() >= mount_dt_window_l(),
           "mount: the dovetail drop must carry the lug wholly out of its window");
    assert(mount_dt_depth() <= mount_kh_head_h(),
           "mount: the dovetail pocket must fit the depth a keyhole pocket budgets");
    assert(mount_dt_neck_w()*mount_dt_len() >= 3*PI*pow(mount_stud_d()/2, 2),
           "mount: the dovetail's root section must be >= 3x the T-stud stem's");
    assert(mount_dt_neck_h() > mount_dt_root() + 0.5,
           "mount: the pocket lip needs a straight land above the root chamfer — no knife edge");
    // tolerance compares: 1.4 + 1.2 + 0.8 is not bit-equal to 3.4 in floats
    assert(abs(mount_stud_h() - 3.4) < 1e-6, "mount: stud stack must total 3.4");
    assert(abs(mount_stud_stem() - (mount_kh_face() + 0.4)) < 1e-6,
           "mount: stud stem = pocket face web + 0.4 slide room");
    assert(abs(mount_kh_head_h() - mount_stud_h() - 0.1) < 1e-6,
           "mount: parked head wants 0.1 ceiling clearance — the click's 0.25 rides on it");
    assert(mount_stud_head() + 0.4 <= mount_kh_head_d() + 0.6,
           "mount: stud head must pass the head cavity with slide room");
    assert(mount_stud_head() > mount_kh_shank_d() + 1.5,
           "mount: the head must overhang the slot enough to bear, or the case pulls off the wall");
    assert(mount_kh_click() < mount_kh_head_h() - mount_stud_h() + 0.2,
           "mount: the click must be a detent, not a wall — keep it near the 0.1 clearance");
    echo("canary_mount_lib: self-check OK");
}
