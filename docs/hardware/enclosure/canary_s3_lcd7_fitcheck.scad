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
//    frame_glass — the one-piece frame vs the slab in the frame's own coords.
//            Non-empty = the cavity or an adhesive-ledge wedge bites into
//            the panel.
//    frame_ledge — INVERTED: a wafer just behind the adhesive plane must land
//            on the ledge. Empty = nothing carries the panel's adhesive
//            border — e.g. the ledge/boss datum drifted apart (the adh_t
//            stack-up bug this gate exists to catch).
//    frame_usb_head — a prism the size of the declared cable HEAD, swept
//            through the bottom-wall port. Non-empty = the head no longer
//            passes (opening shrank under the head knobs, or the ledge/wedge
//            relief behind it regressed).
//    frame_usb_seal — INVERTED: the installed grommet must cross the bottom
//            wall's mid-plane. Empty = the grommet no longer fills the port
//            (its waist or installed transform drifted off the opening).
//    frame_btn_plug — INVERTED: the installed BOOT/RESET plug must cross the
//            top wall's mid-plane. Empty = the plug misses its window.
//    btn_plug_glass — the installed plug (pips included) vs the glass slab.
//            Non-empty = the plug band drifted toward the panel.
//    frame_sd_seal — INVERTED: the installed SD cover must cross the back
//            plate's mid-plane over the opening. Empty = the cover misses
//            its recess/opening.
//
//  The slab is modelled with the corner radius the source DECLARES (glass_r),
//  because the question being asked is whether the model is consistent with
//  the panel it claims to fit. To test a worst case instead, run the check
//  with a smaller glass_r than the cavity is rounded to — a square-cornered
//  slab (glass_r ≈ 0) against an R3 cavity binds on all four corners, which is
//  what the `glass` check is there to catch.
// ============================================================================

use <canary_s3_lcd7.scad>

check = "tray";   // ["tray","glass","lip","locate","frame_glass","frame_ledge","frame_usb_head","frame_usb_seal","frame_btn_plug","btn_plug_glass","frame_sd_seal"]
locate_slip = 1.5;   // mm of sideways wander the pocket must already refuse

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
else if (check == "locate")
    // inverted check — the displaced slab MUST foul the pocket wall
    intersection() {
        assembled_bezel();
        translate([locate_slip, locate_slip, 0]) glass_slab(glass_t);
    }
// The one-piece frame is modelled with the glass at z 0..glass_t in its own
// coords, so the slab needs no repositioning.
else if (check == "frame_glass")
    // frame vs the slab — the cavity and the ledge wedges must clear it
    intersection() {
        frame();
        linear_extrude(glass_t) rrect2d(glass_w, glass_h, glass_r);
    }
else if (check == "frame_ledge") {
    // inverted — a wafer just behind the adhesive plane must land on the
    // ledge, or nothing carries the panel's adhesive border. FS reads the
    // frame's real derived stack, so a datum change can't silently detach
    // the bosses from the panel (the adh_t bug this gate exists to catch).
    FS = lcd7_frame_stack();   // [ledge_z, ledge_t, ...]
    intersection() {
        frame();
        translate([0, 0, FS[0] + 0.1]) linear_extrude(0.2)
            rrect2d(glass_w, glass_h, glass_r);
    }
}
// The TPU gates read the port geometry out of the source the same way
// (lcd7_ports), and place the fitments with the source's own *_installed
// transforms — so a knob or datum change is tested, not a copy of it.
// P = [usb_dx, usb_zc, usb_head_w, usb_head_h, fr_yi, fr_yo, ledge_bot,
//      btn_dx, btn_zc, sd_dx, sd_dy, fz_plate, usb_port]
else if (check == "frame_usb_head") {
    P = lcd7_ports();
    // the declared head, swept through the port from outside to past the
    // ledge/wedge relief — if usb_port is off there is nothing to test and
    // the gate stays empty by construction
    if (P[12] > 0) intersection() {
        frame();
        translate([P[0], -P[4]/2 + P[6] + 0.4, P[1]]) rotate([90, 0, 0])
            linear_extrude(P[6] + (P[5] - P[4])/2 + 2.4)
                rotate(90) pill2d(P[2], P[3]);
    }
}
else if (check == "frame_usb_seal") {
    // inverted — the installed grommet must cross the bottom wall mid-plane
    P = lcd7_ports();
    intersection() {
        usb_grommet_installed();
        translate([P[0], -(P[4] + P[5])/4, P[1]])
            cube([P[2] + 12, 0.4, P[3] + 12], center = true);
    }
}
else if (check == "frame_btn_plug") {
    // inverted — the installed BOOT/RESET plug must cross the top wall mid-plane
    P = lcd7_ports();
    intersection() {
        button_plug_installed();
        translate([P[7], (P[4] + P[5])/4, P[8]])
            cube([40, 0.4, 25], center = true);
    }
}
else if (check == "btn_plug_glass")
    // the plug, pips included, must never reach the panel
    intersection() {
        button_plug_installed();
        linear_extrude(glass_t) rrect2d(glass_w, glass_h, glass_r);
    }
else if (check == "frame_sd_seal") {
    // inverted — the installed SD cover must cross the back plate mid-plane
    P = lcd7_ports();
    FS = lcd7_frame_stack();   // [.., fr_depth] at [5]
    intersection() {
        sd_cover_installed();
        translate([P[9], P[10], (P[11] + FS[5])/2])
            cube([60, 80, 0.4], center = true);
    }
}
else
    assert(false, "check must be one of tray / glass / lip / locate / frame_glass / frame_ledge / frame_usb_head / frame_usb_seal / frame_btn_plug / btn_plug_glass / frame_sd_seal");
