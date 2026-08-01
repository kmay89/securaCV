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
//    locate — the slab pushed sideways by locate_slip. Also INVERTED: it must
//            be NON-empty, i.e. the bezel's pocket physically stops the panel
//            from wandering. The lip is flat and only retains the glass
//            axially, so if the pocket were ever sized to the TRAY cavity
//            instead of the slab, a board that overhangs the glass would let
//            the panel slide until the window crossed the active area. This
//            check fails the moment those two pockets get merged again.
//    stand — the FRAME, seated in the desk dock, vs the stand. Non-empty =
//            they collide and the case cannot drop in. The seated pose is
//            lifted seat_lift off the pads, because exact face-on-face
//            contact through a floating-point rotation can manufacture a
//            zero-ish sliver that fails an honest fit. This check is also
//            what proves the dock's centring keys line up with the frame's
//            keying slots — a misplaced key IS a collision.
//    seat  — the frame pressed seat_press INTO the pads. INVERTED: it must
//            be NON-empty — it is the bearing patch the case actually sits
//            on. Empty = the dock carries nothing and the case free-falls to
//            the base plate. (This output is legitimately TWO patches, one
//            per pad — only its existence is checked, not its mesh.)
//    stand_p / stand_p2 — the stand collision again, case docked in PORTRAIT
//            (turned +90° / -90°). Non-empty = a key or rib lands on solid
//            wall instead of inside a side window / between the gills, and
//            portrait cannot sit flat. Both turns are checked because the
//            side windows are not symmetric.
//    seat_p — portrait bearing patch on the well ribs. INVERTED: non-empty.
//
//  The slab is modelled with the corner radius the source DECLARES (glass_r),
//  because the question being asked is whether the model is consistent with
//  the panel it claims to fit. To test a worst case instead, run the check
//  with a smaller glass_r than the cavity is rounded to — a square-cornered
//  slab (glass_r ≈ 0) against an R3 cavity binds on all four corners, which is
//  what the `glass` check is there to catch.
// ============================================================================

use <canary_s3_lcd7.scad>

check = "tray";   // ["tray","glass","lip","locate","stand","seat","stand_p","stand_p2","seat_p"]
locate_slip = 1.5;   // mm of sideways wander the pocket must already refuse
seat_lift  = 0.2;    // dock collision check: hover this far off the pads
seat_press = 0.4;    // dock bearing check: push this far into the pads

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

// The dock stack, read out of the source the same way. The frame is modelled
// print-side (z0 = front face, +y = up); docking it means standing it up,
// reclining it by stand_ang, and setting its bottom face's centre onto the
// pad plane's origin. seat > 0 presses the case into the pads along the
// slot's own axis; seat < 0 hovers it off them.
ST = lcd7_stand_stack();
st_ang = ST[0]; st_ys = ST[1]; st_zf = ST[2]; st_fd = ST[3]; st_fy = ST[4]; st_fx = ST[5];
module docked_frame(seat = 0, portrait = 0) {   // portrait: ±1 = which way it turned
    fy = portrait == 0 ? st_fy : st_fx;
    yq = -(fy/2)*sin(st_ang) + (st_fd/2)*cos(st_ang);
    zq = -(fy/2)*cos(st_ang) - (st_fd/2)*sin(st_ang);
    translate([0, st_ys - yq - seat*sin(st_ang), st_zf - zq - seat*cos(st_ang)])
        rotate([270 - st_ang, 0, 0]) rotate([0, 0, 180])
            rotate([0, 0, 90*portrait]) frame();
}

if (check == "tray")
    intersection() { assembled_bezel(); back(); }
else if (check == "glass")
    intersection() { assembled_bezel(); glass_slab(glass_t); }
else if (check == "lip")
    // inverted check — this SHOULD produce geometry (the bearing footprint)
    intersection() { assembled_bezel(); glass_slab(0.5, glass_t); }
else if (check == "locate")
    // inverted check — the displaced slab MUST foul the pocket wall
    intersection() {
        assembled_bezel();
        translate([locate_slip, locate_slip, 0]) glass_slab(glass_t);
    }
else if (check == "stand")
    intersection() { docked_frame(-seat_lift); stand(); }
else if (check == "seat")
    // inverted check — this SHOULD produce geometry (the pads' bearing patch)
    intersection() { docked_frame(seat_press); stand(); }
else if (check == "stand_p")
    intersection() { docked_frame(-seat_lift, 1); stand(); }
else if (check == "stand_p2")
    intersection() { docked_frame(-seat_lift, -1); stand(); }
else if (check == "seat_p")
    // inverted check — portrait must bear on the well ribs
    intersection() { docked_frame(seat_press, 1); stand(); }
else
    assert(false, "check must be one of tray / glass / lip / locate / stand / seat / stand_p / stand_p2 / seat_p");
