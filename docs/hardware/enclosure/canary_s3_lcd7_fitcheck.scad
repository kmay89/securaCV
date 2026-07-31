// ============================================================================
//  Canary 7" dashboard — ASSEMBLY FIT CHECK (not a printable part)
//
//  Why this file exists: every part of this case passed the mesh gate while
//  the case was wrong. v0.1 counted the glass thickness in BOTH the tray depth
//  and the bezel height, so the tray came out 4 mm too deep and the bezel's
//  retaining lip floated 5 mm clear of the glass — three watertight,
//  single-part STLs that assemble into a frame gripping nothing. admesh cannot
//  see that; only putting the parts together can.
//
//  So this renders the parts IN THEIR ASSEMBLED POSITIONS and intersects them.
//  Each check is a solid that MUST come out empty. OpenSCAD writes no file for
//  an empty top-level object, so the CI test is simply "no file appeared":
//
//      openscad --export-format binstl -o /tmp/c.stl -D 'check="tray"' \
//          canary_s3_lcd7_fitcheck.scad && test ! -f /tmp/c.stl
//
//  checks:
//    tray  — bezel vs back tray. Non-empty = the two collide and the case
//            cannot close.
//    glass — bezel vs the glass slab. Non-empty = the bezel is crushing the
//            panel; usually the cavity is rounded more than the slab's corners
//            (raise/lower glass_r to the MEASURED radius).
//    lip   — bezel vs a thin wafer on the glass's FRONT face. This one is
//            INVERTED: it must be NON-empty, because it is the contact patch
//            the lip actually bears on. Empty = nothing retains the glass,
//            which is exactly the v0.1 failure.
//
//  The slab is modelled with the corner radius the source DECLARES (glass_r),
//  because the question being asked is whether the model is consistent with
//  the panel it claims to fit. To test a worst case instead, run the check
//  with a smaller glass_r than the cavity is rounded to — a square-cornered
//  slab (glass_r ≈ 0) against an R3 cavity binds on all four corners, which is
//  what the `glass` check is there to catch.
// ============================================================================

use <canary_s3_lcd7.scad>

check = "tray";   // ["tray","glass","lip"]

// Read the real derived stack out of the source — no duplicated arithmetic.
S = lcd7_stack();
back_t = S[0]; cav_d = S[1]; bez_h = S[2]; glass_t = S[3];
glass_w = S[4]; glass_h = S[5]; glass_r = S[6]; z_glass = S[7];

// The bezel prints face-down (z0 = outer face), so it flips onto the tray:
// its top face (z = bez_h) is the glass-back datum the tray's walls end at.
module assembled_bezel() {
    translate([0, 0, z_glass + bez_h]) scale([1, 1, -1]) bezel_print();
}
module glass_slab(t, dz = 0) {
    translate([0, 0, z_glass + dz]) linear_extrude(t)
        rrect2d(glass_w, glass_h, glass_r);
}

if (check == "tray")
    intersection() { assembled_bezel(); back(); }
else if (check == "glass")
    intersection() { assembled_bezel(); glass_slab(glass_t); }
else if (check == "lip")
    // inverted check — this SHOULD produce geometry (the bearing footprint)
    intersection() { assembled_bezel(); glass_slab(0.5, glass_t); }
else
    assert(false, "check must be one of tray / glass / lip");
