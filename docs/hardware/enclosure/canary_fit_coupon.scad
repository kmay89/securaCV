// ============================================================================
//  Canary — UNIVERSAL FIT-CHECK COUPON  ⚠️ IN DEVELOPMENT (v0.3-dev)
//  ONE small print that calibrates your printer for the ENTIRE Canary
//  catalog before you commit to any case. Every fit the enclosures use is
//  exercised on a labeled station, laid out on a branded 90 × 68 plate:
//
//    BASE (part="base", rigid) — stations left→right, top→bottom:
//      EMBOSS  — raised "CANARY" wordmark (emboss_h): crisp or blobby?
//      EMBLEM  — the Canary mark in uniform strokes, raised at emboss_h in
//                the brand band. NO feature is narrower than one rib, so the
//                rib is the whole test — crisp bird, or a blob? Distinct from
//                GLYPH below: this is the small simplified mark on the TOP
//                face, that is the full-size artwork on the bed face
//                                                  -> emblem_rib/emblem_h
//      DEBOSS  — sunk "SecuraCV" wordmark (label_depth): every label's fate
//      SLIDE   — lip channel: the MATE's edge tongue slides in snugly -> tol_slide
//      PORT    — the cases' USB-C opening through a case wall: your
//                cable's head must pass                            -> usb_w/usb_h
//      PRESS   — 6 mm magnet pocket + 3 mm light-pipe hole         -> tol_press
//      GROOVE  — straight gasket groove: seat the TPU STRIP        -> gasket fit
//      SCREW   — M2 post (drive a screw ~0.3 N·m) + countersink,
//                plus a −/0/+ pilot ladder: pick the hole that
//                bites without splitting                           -> screw_d
//      CLIP    — the WAP's clip coupon, embedded: snap clips on BOTH long
//                edges of a board channel — press a 1.2 mm PCB edge (or
//                scrap) in and feel the click        -> clip_t/clip_hook/clip_clear
//      POCKET  — two blind keyhole pockets (gap 30): hang the MATE's studs,
//                slide to the CLICK — a detent parks them so the mate
//                doesn't slide back off (the doorbell-plate retention test)
//      INSERT  — heat-set boss (only if you'll use screw_insert)   -> insert_d
//      GLYPH   — the SecuraCV bird debossed into the BED-SIDE face at full
//                size: the first-layer / plate-texture test, and the one
//                that matters most, because every case A-surface in this
//                catalog prints face-down (see the knobs for why it lives
//                on the underside and not between the stations)
//    MATE (part="mate"): two T-studs + a bottom-flush slide tongue on the
//                edge (in-plane, so the stud face stays flat for the hang test)
//    STRIP (part="strip"): TPU gasket bar for the groove
//
//  If a station is tight/loose, adjust the matching tol_* / clip_* / screw
//  parameter in the case you print next. Labels are debossed beside each.
//
//  The bird glyph (securacv_bird_glyph.svg) is © ERRERLabs / SecuraCV /
//  Karl Meves — shipped with the coupon as production art, not clip art.
//  It is GENERATED from canary_mark_lib.scad by gen_mark_svg.py, so the art
//  and the geometry below are the same bird by construction. Edit the paths
//  in the library and re-run the generator; never hand-edit the SVG.
//
//  2026-08-23: the coupon now exercises the SHARED LIBRARIES' geometry —
//  pockets/click/studs from canary_mount_lib, the clip from canary_snap_lib,
//  the port profile from canary_port_lib — and runs all five lib self-checks
//  on every render. A coupon that calibrates the catalog must print the
//  catalog's actual modules, not private copies of them.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

use <canary_mark_lib.scad>   // THE BIRD — one mark for the whole
                             // catalog; see that file's header for why
                             // it is authored rather than traced
use <canary_core_lib.scad>   // rrect2d + the tolerance trio this coupon tunes
use <canary_mount_lib.scad>  // the stud/keyhole standard — the POCKET/MATE pair
use <canary_snap_lib.scad>   // the WAP clip + its strain budget — the CLIP station
use <canary_port_lib.scad>   // the bridge-safe opening — the PORT station
use <canary_board_lib.scad>  // the board registry — its self-check runs here
use <canary_color_lib.scad>  // the colorway registry — its self-check runs here

/* [What to render] */
part = "all";        // ["base","mate","strip","all"]

/* [Print tolerances under test — same trio as every case] */
tol_slide = 0.20;   // catalog default — core_tol_slide(), canary_core_lib
tol_press = 0.10;   // catalog default — core_tol_press(), canary_core_lib
tol_hole  = 0.30;   // catalog default — core_tol_hole(), canary_core_lib

/* [Interface dims — mirror the case defaults] */
stud_gap = 30.0;  // the coupon's print-tested gap (canary_mount_lib's header cites it)
kh_head_d = 7.0;  kh_shank_d = 4.2;  kh_slot_l = 8.0;  kh_head_h = 3.5;  kh_face = 1.0;  // catalog standard — mount_kh_*(), canary_mount_lib
kh_click = 0.25;  // detent bump proud of the head-channel ceiling (0 = no click) — catalog standard, canary_mount_lib
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;  // the WAP clip — snap_boardclip defaults, canary_snap_lib
clip_bw = 17.5;   // the WAP's board width — the CLIP station is its coupon verbatim
pcb_t = 1.2;  standoff_h = 3.0;  standoff_d = 4.0;
screw_d = 1.6;  screw_head_d = 4.0;
screw_step = 0.15;   // pilot ladder: three holes at screw_d − step / screw_d / + step
insert_d = 3.5;  insert_h = 4.0;
mag_d = 6.0;  lp_d = 3.0;
lip_t = 1.2;  lip_h = 4.0;
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;   // matches the catalog gasket recipe (20 % squeeze, ~86 % fill)
usb_w = 12.0;  usb_h = 6.5;   // the WAP case's USB-C opening (clears rugged cable boots); cut via port_bridge_profile2d, canary_port_lib
port_wall = 2.0;              // case-typical wall the PORT opening is cut through

/* [Coupon] */
base_t = 5.0;        // base thickness (pockets live in it)
label_depth = 0.5;
emboss_h = 0.6;      // raised-wordmark height (EMBOSS station)
label_font = "Liberation Sans:style=Bold";
brand_raised = "CANARY";     // the EMBOSS station's wordmark
brand_sunk   = "SecuraCV";   // the DEBOSS station's wordmark
// ⚠️  EMBLEM STATION RETIRED — 0, and the reason is a measurement.
// The house mark was redrawn to the brand's monoline art: a bird carrying a
// notepad with a C spiralled into its wing and a V nested in its tail. That
// drawing has INTERIOR detail, so it has a minimum size — at this station's
// 0.7 mm rib, mark_min_h(0.7) is 15.6 mm. The air between the EMBOSS and
// DEBOSS wordmarks is 7.6 mm ("CANARY" at size 7 ends at x -7.5, "SecuraCV"
// starts at +0.1). The mark cannot go there at any size that is still the mark,
// and the library asserts rather than letting it print as a blob.
//
// The test did not disappear — it MOVED, to the GLYPH station on the bed face
// below, which is where the coupon's own header always said the full-size mark
// belonged and which had a 34 mm rectangle reserved for it. That is the better
// home anyway: every case A-surface in this catalog prints face-down, so a
// deboss against the textured plate is the question cases actually ask. What
// is genuinely gone is the CROWN test (emblem_crown domed the stroke tops on a
// RAISED mark) — and nothing in the catalog embosses the mark any more, so it
// was testing a treatment none of the cases print.
emblem_h = 0;        // set >= mark_min_h(emblem_rib) to bring the station back
emblem_rib = 0.7;
emblem_crown = 0.15;

/* [GLYPH station — the bird, debossed into the BED-SIDE face] */
// Why the underside: the mark is LINE ART, and its strokes are ~0.2 mm at
// 40 mm wide. Shrink it to fit between the stations on the top face and the
// strokes fall under half a line width — it prints as mush and libels the
// brand. The bed face is empty, so the mark gets its full size there, and
// lands on the one test this catalog most needs: EVERY case A-surface
// prints face-down, so the first layers against the textured plate ARE the
// visible finish. If the beak, the eye ring and the notepad spiral come off
// the plate crisp, your first layer is dialled for every case in the set.
// A deboss (not an emboss) so the coupon still sits flat on its own face.
// These knobs existed and NOTHING READ THEM. The station was described in this
// file's header, given six parameters and a paragraph of reasoning, and never
// cut — so the coupon has been shipping without the one test its own header
// calls "the one that matters most". It is wired up now, with the redrawn mark.
glyph_show  = true;
glyph_h     = 34.0;   // NOMINAL height (mark_span units) — draws about
                      // 30 x 32 mm, which is what the reserved rectangle on
                      // the bed face was always sized for
glyph_rib   = 0.7;    // stroke width, in mm. THE number this station tests:
                      // the mark is MONOLINE, so no part of it is narrower
                      // than this — if the rib survives, the mark survives.
                      // Below ~0.42 it is thinner than one extrusion
glyph_depth = 0.5;    // = label_depth's sibling; one bridged layer closes it
glyph_dx    = -26.5;  // the clear rectangle on the bed face: left of the
glyph_dy    = 12.0;   // PORT through-hole, above the keyhole pockets

/* [Quality] */
$fa = 3; $fs = 0.4;

bw = 90; bh = 68;
echo(str("Canary fit coupon v0.3-dev — base ", bw, "x", bh, "  (IN DEVELOPMENT)"));
if (emblem_h > 0) {
    echo(str("EMBLEM station: mark ", mark_h_mm(emblem_h, emblem_rib),
             " tall x ", mark_w_mm(emblem_h, emblem_rib),
             " wide, ", emblem_rib, " mm ribs, ", emblem_crown, " mm crown"));
    assert(emblem_rib >= 0.4,
           "emblem_rib below 0.4 mm — thinner than one 0.4 mm extrusion, the mark will not print");
    assert(emblem_crown < emblem_rib/2,
           "emblem_crown >= half the rib — the crown would pinch the strokes off at the top");
}
if (glyph_show) {
    echo(str("GLYPH station: the mark ", mark_h_mm(glyph_h, glyph_rib),
             " tall x ", mark_w_mm(glyph_h, glyph_rib), " wide, ", glyph_rib,
             " mm ribs, debossed ", glyph_depth,
             " mm into the BED face — print it and read the first layer"));
    // The library refuses an unprintable mark, but it refuses in terms of the
    // mark. Say it in terms of the station, since this is the station whose
    // whole job is that number.
    assert(mark_rib_ok(glyph_h, glyph_rib),
           str("GLYPH station: ", glyph_rib, " mm ribs at glyph_h ", glyph_h,
               " close the mark's interior. Raise glyph_h to at least ",
               mark_min_h(glyph_rib), " mm, or thin glyph_rib."));
    assert(glyph_dx - mark_w_mm(glyph_h, glyph_rib)/2 > -bw/2 + 2
           && glyph_dx + mark_w_mm(glyph_h, glyph_rib)/2 < -usb_w/2 - 2
           && abs(glyph_dy) + mark_h_mm(glyph_h, glyph_rib)/2 < bh/2 - 2,
           "GLYPH station: the mark runs off the bed face or into the PORT hole");
}

// ---------------------------------------------------------------------------
//  GLYPH station — the Canary mark, drawn the way it has to be drawn to
//  survive a nozzle: uniform-width strokes with round caps, no fine detail.
//  The mark's NARROWEST feature is one stroke wide — tapers stop at it rather
//  than running to a point — so there is exactly ONE number that decides
//  whether this prints (glyph_rib below, echoed at render).
//  Authored here as paths rather than traced from the brand artwork — that
//  art is line work ~0.08 mm wide at badge size, which no nozzle can lay down.
//  Chaikin corner-cutting rounds the polylines; three passes is plenty.
// ---------------------------------------------------------------------------
// (The bird itself now lives in canary_mark_lib.scad — one definition of the
// house mark for the whole line, the same way canary_vent_lib.scad owns the
// egg, so the 7" back plate and this coupon cannot drift into two slightly
// different birds. This coupon is where its printability gets PROVEN, not
// where it is defined; see the rib/crown knobs above. Everything crosses as a
// function because OpenSCAD's use<> does not carry variables.)
// ---------------------------------------------------------------------------

module lbl(x, y, s, size = 4.5) { translate([x, y, base_t - label_depth]) linear_extrude(label_depth + 0.1)
    text(s, size = size, font = label_font, halign = "center", valign = "center"); }
module emb(x, y, s, size = 7) { translate([x, y, base_t - 0.05]) linear_extrude(emboss_h + 0.05)
    text(s, size = size, font = label_font, halign = "center", valign = "center"); }
// EMBLEM station: the mark raised at the SAME height the cases' brand marks
// use (emboss_h), so the wordmark beside it and the bird ask the same question
// two ways. The strokes are built as a short stack of inward offsets on a
// quarter-circle profile, so each one carries a DOMED crown rather than a flat
// slab top — that is what makes an embossed mark catch light like metal. The
// footprint is still a full rib wide; only the crown draws in.
module emblem_emb(px, py, h) {
    translate([px, py, base_t - 0.05])
        mark_emboss(h, emboss_h + 0.05, emblem_rib, emblem_crown);
}
module keyhole_pocket(px, py) {         // blind, from the back (z=0 face)
    translate([px, py, 0])              // the catalog pocket, native y-axis slot
        mount_keyhole_pocket(0, 0, axis = "y", head_d = kh_head_d,
                             shank_d = kh_shank_d, slot_l = kh_slot_l,
                             head_h = kh_head_h, face = kh_face);
}
// The WAP's boardclip, rooted on the coupon face — snap_boardclip itself, so
// the station tests the very module the cases print, and its strain gate runs
// on every coupon render too. sy = +1/-1 picks which long edge of the CLIP
// station's board channel the clip guards — one on EACH side, like the case;
// this wrapper only turns the channel center into the lib's board-edge line.
module stationclip(cx, cy, sy) {
    snap_boardclip(cx, cy + sy*clip_bw/2, sy,
                   z_floor = base_t, z_board = base_t + standoff_h + pcb_t,
                   w = clip_w, t = clip_t, hook = clip_hook,
                   hook_h = clip_hook_h, clear = clip_clear);
}

// Station anchors — one grid, every label gets clear air.
// Brand band y≈+27 · row 1 y≈+16 · row 2 y≈+1 · row 3 y≈−14 · foot y≈−30
pocket_cx = -1;                       // keyholes at pocket_cx ± stud_gap/2
ladder_x  = [31, 36, 41];             // SCREW pilot ladder columns
ladder_d  = [screw_d - screw_step, screw_d, screw_d + screw_step];
ladder_tag = ["-", "0", "+"];

module base() {
    union() {
        difference() {
            linear_extrude(base_t) rrect2d(bw, bh, 4);
            // DEBOSS station: the sunk wordmark IS the test (depth = label_depth,
            // the same cut every label on every case gets)
            lbl(22, 27, brand_sunk, 7);
            // SLIDE station: female channel for the mate's lip rib
            translate([-24, 16, base_t - lip_h + 1]) linear_extrude(lip_h)
                rrect2d(30 + 2*tol_slide, lip_t + 2*tol_slide, 0.3);
            // PORT station: the WAP's USB-C opening through a case-typical wall —
            // counterbored from the back so the web is port_wall thick, like a case.
            // The through profile is port_bridge_profile2d — the WAP's bridge-safe
            // polygon itself (square lower corners, 45°-chamfered top corners) —
            // so a boot that passes here passes the case.
            translate([2, 16, -0.1]) linear_extrude(base_t - port_wall + 0.1)
                rrect2d(usb_w + 6, usb_h + 5, 2);
            translate([2, 16, -0.1]) linear_extrude(base_t + 0.2)
                port_bridge_profile2d(usb_w, usb_h);
            // PRESS station: magnet pocket + light-pipe hole
            translate([26, 16, base_t - 2.2]) cylinder(d = mag_d + 2*tol_press, h = 2.3);   // matches mag_h 2.2
            translate([35, 16, -0.1]) cylinder(d = lp_d + 2*tol_press, h = base_t + 0.2);
            // GROOVE station: straight gasket groove
            translate([-24, 1, base_t - gasket_groove]) linear_extrude(gasket_groove + 0.1)
                rrect2d(30, gasket_w, 0.3);
            // SCREW station: countersunk clearance hole (lid side of the joint)...
            translate([23, 1, -0.1]) cylinder(d = screw_d + 2*tol_hole, h = base_t + 0.2);
            translate([23, 1, base_t - 1.6]) cylinder(d1 = screw_d + 2*tol_hole, d2 = screw_head_d, h = 1.7);
            // ...plus the −/0/+ pilot ladder: blind holes, drive the screw into each
            for (i = [0:2]) translate([ladder_x[i], 1, base_t - 4]) cylinder(d = ladder_d[i], h = 4.1);
            // POCKET station: keyhole pocket pair for the mate's studs (gap = stud_gap)
            keyhole_pocket(pocket_cx - stud_gap/2, -14);
            keyhole_pocket(pocket_cx + stud_gap/2, -14);
            // labels — grid-aligned, none touching a neighbor
            lbl(-24, 21, "EMBOSS", 3);  lbl(-3, 20.5, "EMBLEM", 3);  lbl(22, 21, "DEBOSS", 3);
            lbl(-24, 9.5, "SLIDE");     lbl(2, 9.5, "PORT");     lbl(30, 9.5, "PRESS");
            lbl(-24, -4.5, "GROOVE", 3);   lbl(19, -5.5, "SCREW");
            for (i = [0:2]) lbl(ladder_x[i], -4.5, ladder_tag[i], 3);   // on the SCREW line, under their holes
            lbl(-31, -16.5, "CLIP", 3);    // on the channel floor, between the rails
            lbl(pocket_cx, -25.5, "POCKET");  lbl(34, -21, "INSERT");
            // foot line: the small-text legibility test doubles as the colophon
//      GLYPH   — the SecuraCV bird debossed into the BED-SIDE face at full
//                size: the first-layer / plate-texture test, and the one
//                that matters most, because every case A-surface in this
//                catalog prints face-down (see the knobs for why it lives
//                on the underside and not between the stations)
//    MATE (part="mate"): two T-studs + the slide rib
        }
        // EMBOSS station: raised wordmark at the height case branding uses
        emb(-24, 27, brand_raised, 7);
        // EMBLEM station: the mark, same emboss_h, in the clear air between
        // the two wordmarks (CANARY ends ≈−11.5, SecuraCV starts ≈+5)
        // Centered in the gap between the two wordmarks, and now nearly
        // filling it: at 15.8 the mark spans 15.4 mm against 16.5 mm of air.
        // The asserts below hold it there.
        if (emblem_h > 0) emblem_emb(-3, 25.5, emblem_h);
        // GLYPH station — the mark debossed into the BED face (z = 0), at a
        // size that keeps its interior open. This is the first-layer test:
        // every case A-surface in this catalog prints face-down, so what comes
        // off the textured plate here is what comes off it on a bezel.
        if (glyph_show)
            translate([glyph_dx, glyph_dy, -0.1])
                linear_extrude(glyph_depth + 0.1)
                    mark_bird(glyph_h, glyph_rib);
        // CLIP station — the WAP clip coupon, both sides: board-rest rails flush
        // to the board's edge lines and a snap clip on EACH long edge. Press a
        // 1.2 mm scrap (or real board edge) past the lead-ins and feel the click.
        for (sy = [1, -1]) {
            translate([-37, -16.5 + sy*(clip_bw/2 - standoff_d/2) - standoff_d/2, base_t - 0.01])
                cube([12, standoff_d, standoff_h + 0.01]);
            stationclip(-31, -16.5, sy);
        }
        // POCKET station: the click detents, one per keyhole — the lib's dome
        // on the head-channel ceiling (kh_click 0.25 against the head's 0.1
        // ceiling clearance = 0.15 squeeze: slide on, CLICK, stays until a
        // firm pull back)
        if (kh_click > 0) for (s = [1, -1])
            mount_keyhole_click(pocket_cx + s*stud_gap/2, -14, kh_head_h, kh_click);
        // SCREW station: M2 self-tap post
        translate([14, 1, base_t - 0.01]) difference() {
            cylinder(d = 5, h = 8);
            translate([0, 0, 1.5]) cylinder(d = screw_d, h = 8);
        }
        // INSERT station: heat-set boss
        translate([34, -14, base_t - 0.01]) difference() {
            cylinder(d = insert_d + 2.4, h = insert_h + 2);
            translate([0, 0, 2]) cylinder(d = insert_d - 0.1, h = insert_h + 2.1);
        }
    }
}

module mate() {
    union() {
        difference() {
            linear_extrude(3) rrect2d(46, 20, 3);
            translate([0, -6.5, 3 - label_depth]) linear_extrude(label_depth + 0.1)
                text("CANARY MATE", size = 3.2, font = label_font, halign = "center", valign = "center");
        }
        // T-studs at stud_gap (hang on the base's pockets, slide down) — the
        // catalog stud itself, its stem tracking kh_face per the mount
        // contract (stem = pocket face web + 0.4 slide room)
        for (s = [1, -1]) translate([s*stud_gap/2, 0, 3 - 0.01])
            mount_tstud(stem = kh_face + 0.4);
        // slide tongue — bottom-flush on the +y EDGE, in the plate's plane, so
        // the stud face stays dead flat and the hang test seats fully (the v0.2
        // face rib propped the mate 3 mm off the base). Hold the mate on edge
        // and push the tongue into the SLIDE channel: same lip_t vs tol_slide
        // gauge, and at 3 mm proud it bottoms out flush with the coupon face.
        translate([-15, 10 - 0.01, 0]) cube([30, lip_h - 1 + 0.01, lip_t]);
    }
}

module strip() {   // TPU gasket bar for the GROOVE station
    linear_extrude(gasket_groove + gasket_proud) rrect2d(29.4, gasket_w - 0.5, 0.3);   // strip 0.5 narrower, like the case gaskets
}

// The libraries' load-time gates: use<> does not run top-level statements,
// so the coupon — the catalog's calibration authority — runs every lib's
// self-check on every render of every part. Five OK echoes, or no STL.
core_selfcheck();
mount_selfcheck();
snap_selfcheck();
port_selfcheck();
board_selfcheck();
color_selfcheck();

if      (part == "base")  base();
else if (part == "mate")  mate();
else if (part == "strip") strip();
else { base(); translate([-12, -50, 0]) mate(); translate([28, -50, 0]) strip(); }
