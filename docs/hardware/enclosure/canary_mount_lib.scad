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
//  move under a dedup.
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
//  Self-check — the mating arithmetic the header promises. Call once from an
//  adopter (the fit coupon does).
// ---------------------------------------------------------------------------
module mount_selfcheck() {
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
