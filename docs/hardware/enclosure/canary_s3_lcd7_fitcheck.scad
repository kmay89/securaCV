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
//    frame_glass — the one-piece frame vs the STEPPED panel (thin bare-glass
//            border + thicker LCD-module core) in the frame's own coords.
//            Non-empty = the cavity, the raised adhesive ledge or a ledge
//            wedge bites into the panel.
//    frame_ledge — INVERTED: a wafer just behind the adhesive plane must land
//            on the ledge. Empty = nothing carries the panel's adhesive
//            border — e.g. the ledge/boss datum drifted apart (the adh_t
//            stack-up bug this gate exists to catch).
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
//    seat_p — portrait bearing patch on the well ribs, probed with a wafer
//            over the portrait bottom face (see the check for why not the
//            full frame). INVERTED: non-empty.
//    frame_usb_head — a prism the size of the declared cable HEAD, swept
//            through the bottom-wall port, vs the FRAME. Non-empty = the head
//            no longer passes (opening shrank under the head knobs, or the
//            ledge/wedge relief behind it regressed).
//    frame_btn_window / frame_sd_window — the same doctrine for the other two
//            openings: a probe prism just under the declared window size,
//            swept through the wall/plate, vs the FRAME. Non-empty = the
//            opening is missing, shrunken or obstructed. These exist because
//            the *_seal gates below deliberately do NOT look at the frame —
//            each opening needs one gate against the aperture and one against
//            the fitment, or a deleted window would still "seal".
//    frame_usb_seal — INVERTED: the installed grommet must cross the bottom
//            wall's mid-plane. Empty = the grommet's waist or installed
//            transform drifted off the opening. (Fitment-vs-frame overlap
//            can't be the test: the TPU waists carry deliberate interference,
//            so they intersect the frame BY DESIGN — aperture-open is what
//            the *_window/_head gates above assert.)
//    frame_btn_plug — INVERTED: the installed BOOT/RESET plug must cross the
//            top wall's mid-plane. Empty = the plug misses its window.
//    btn_plug_glass — the installed plug (pips included) vs the glass slab.
//            Non-empty = the plug band drifted toward the panel.
//    frame_sd_seal — INVERTED: the installed SD cover must cross the back
//            plate's mid-plane over the opening. Empty = the cover misses
//            its recess/opening.
//    frame_adh_rail — the adhesive rails' promise is SMOOTH, UNINTERRUPTED
//            plate, so this one is a DIFFERENCE, not an intersection: a
//            wafer just under the outer skin across each rail zone, minus
//            the frame, must vanish. Non-empty = something cut a zone — a
//            grille slot that lost its keepout, a drifted deboss, the moat
//            itself — and an adhesive strip would land on an edge.
//    sd_tether_hole — a probe just under the anchor hole's size, swept
//            through the plate at the tether position, vs the FRAME.
//            Non-empty = the hole is missing or off-position — the cover's
//            barb would have nowhere to go (exactly the print-the-case-
//            without-the-hole mistake this gate exists to stop).
//    sd_tether_barb — INVERTED: the installed SD cover must cross the
//            plate band at the anchor hole position. Empty = the barb
//            misses its hole — the strap length, hole position or install
//            transform drifted, and the "captive" cover isn't.
//
//  The slab is modelled with the corner radius the source DECLARES (glass_r),
//  because the question being asked is whether the model is consistent with
//  the panel it claims to fit. To test a worst case instead, run the check
//  with a smaller glass_r than the cavity is rounded to — a square-cornered
//  slab (glass_r ≈ 0) against an R3 cavity binds on all four corners, which is
//  what the `glass` check is there to catch.
// ============================================================================

use <canary_s3_lcd7.scad>

check = "tray";   // ["tray","glass","lip","locate","frame_glass","frame_ledge","stand","seat","stand_p","stand_p2","seat_p","frame_usb_head","frame_btn_window","frame_sd_window","frame_usb_seal","frame_btn_plug","btn_plug_glass","frame_sd_seal","frame_adh_rail","sd_tether_hole","sd_tether_barb"]
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
st_drop = ST[6];   // portrait seats this far below the pad plane, on the ribs
module docked_frame(seat = 0, portrait = 0) {   // portrait: ±1 = which way it turned
    fy = portrait == 0 ? st_fy : st_fx;
    dn = seat + (portrait == 0 ? 0 : st_drop);   // rib drop is part of the pose
    yq = -(fy/2)*sin(st_ang) + (st_fd/2)*cos(st_ang);
    zq = -(fy/2)*cos(st_ang) - (st_fd/2)*sin(st_ang);
    translate([0, st_ys - yq - dn*sin(st_ang), st_zf - zq - dn*cos(st_ang)])
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
// The one-piece frame is modelled with the panel at z 0.. in its own coords,
// so the slab needs no repositioning. The panel is STEPPED, and the check
// must model both steps: a bare-glass BORDER (glass_edge_t thin — the band
// the adhesive ledge rises to within adh_t of) around the thicker LCD-module
// core (glass_t). A uniform glass_t slab would false-fail against the raised
// ledge; a uniform border-thin slab would let the cavity close onto the
// module. The core outline comes from lcd7_panel_core() — the MEASURED can,
// stated independently of the ledge geometry — so widening a ledge cannot
// shrink the probe in lockstep and sneak past this gate.
else if (check == "frame_glass") {
    ge = lcd7_frame_stack()[6];   // bare-glass border thickness
    PC = lcd7_panel_core();       // [core_w, core_h, core_dy]
    intersection() {
        frame();
        union() {
            linear_extrude(ge) rrect2d(glass_w, glass_h, glass_r);
            translate([0, PC[2], 0]) linear_extrude(glass_t)
                rrect2d(PC[0], PC[1], 2);
        }
    }
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
    // inverted check — portrait must bear on the well ribs. Probed with a
    // WAFER over the portrait bottom face's footprint rather than the full
    // frame: the stand_p/stand_p2 checks already prove the real case clears
    // everything, and the full-frame intersection here trips CGAL's
    // non-manifold export warning on coincident slot/blade edges, which the
    // CI gate correctly refuses to distinguish from a broken render.
    intersection() {
        stand_seatframe()
            translate([-st_fy/2, -st_fd/2, -st_drop - seat_press])
                cube([st_fy, st_fd, seat_press]);
        stand();
    }
// The TPU gates read the port geometry out of the source the same way
// (lcd7_ports), and place the fitments with the source's own *_installed
// transforms — so a knob or datum change is tested, not a copy of it.
// P = [usb_dx, usb_zc, usb_head_w, usb_head_h, fr_yi, fr_yo, ledge_bot,
//      btn_dx, btn_zc, sd_dx, sd_dy, fz_plate, usb_port, btn_w, btn_h,
//      sd_w, sd_l]
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
else if (check == "frame_btn_window") {
    // a probe just under the declared window size, swept from outside the
    // top wall all the way through the ledge-wedge band behind it, vs the
    // frame — non-empty means the BOOT/RESET window is missing, shrunken or
    // obstructed (the *_seal gates never look at the frame). The inboard
    // reach matters: a mouth-deep probe would miss the top-edge wedge that
    // originally stood behind the window's lower half — the obstruction
    // this gate caught on its first run.
    P = lcd7_ports();
    intersection() {
        frame();
        translate([P[7], P[4]/2 - P[17] - 0.5, P[8]]) rotate([-90, 0, 0])
            linear_extrude(P[17] + 0.5 + (P[5] - P[4])/2 + 1.2)
                rrect2d(P[13] - 0.4, P[14] - 0.4, 2.8);
    }
}
else if (check == "frame_sd_window") {
    // same doctrine through the back plate for the SD opening
    P = lcd7_ports();
    FS = lcd7_frame_stack();   // [.., fr_depth] at [5]
    intersection() {
        frame();
        translate([P[9], P[10], P[11] - 0.5]) linear_extrude(FS[5] - P[11] + 1.5)
            rrect2d(P[15] - 0.4, P[16] - 0.4, 4.8);
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
else if (check == "frame_adh_rail") {
    // a wafer just under the outer skin across each adhesive-rail zone,
    // MINUS the frame, must vanish — anything that cuts a zone survives the
    // difference and fails the gate. The wafer stops 0.02 below the skin
    // (so it never pokes out of an intact plate) and reaches 0.5 deep (so
    // any deboss-depth cut into the zone intersects it). If adh_rails is
    // off there is nothing to test and the gate stays empty by construction.
    A = lcd7_adh_rails();   // [on, dx, w, l, fr_depth]
    if (A[0] > 0) difference() {
        for (sx = [1, -1]) translate([sx*A[1], 0, A[4] - 0.5])
            linear_extrude(0.48) rrect2d(A[2] - 0.4, A[3] - 0.4, 2);
        frame();
    }
}
else if (check == "sd_tether_hole") {
    // probe just under the anchor hole's size, swept from mid-plate out
    // past the skin (the strap channel above the hole is open by design,
    // so the probe proves hole + channel both) — vs the FRAME, must vanish
    T = lcd7_sd_tether();   // [on, x, y, hole_d, fz_plate]
    FS = lcd7_frame_stack();
    if (T[0] > 0) intersection() {
        frame();
        translate([T[1], T[2], T[4] - 0.5])
            cylinder(d = T[3] - 0.4, h = FS[5] - T[4] + 1.5);
    }
}
else if (check == "sd_tether_barb") {
    // inverted — the installed cover's barb must cross the plate band at
    // the anchor hole position, or the leash anchors nothing
    T = lcd7_sd_tether();
    FS = lcd7_frame_stack();
    if (T[0] > 0) intersection() {
        sd_cover_installed();
        translate([T[1], T[2], (T[4] + FS[5])/2])
            cube([6, 6, 0.4], center = true);
    }
    else translate([0, 0, 0]) cube(1);   // tether off: gate must stay solid-true
}
else
    assert(false, "check must be one of tray / glass / lip / locate / frame_glass / frame_ledge / stand / seat / stand_p / stand_p2 / seat_p / frame_usb_head / frame_btn_window / frame_sd_window / frame_usb_seal / frame_btn_plug / btn_plug_glass / frame_sd_seal / frame_adh_rail / sd_tether_hole / sd_tether_barb");
