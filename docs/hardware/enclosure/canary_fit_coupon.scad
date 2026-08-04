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
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

use <canary_mark_lib.scad>   // THE BIRD — one mark for the whole
                             // catalog; see that file's header for why
                             // it is authored rather than traced

/* [What to render] */
part = "all";        // ["base","mate","strip","all"]

/* [Print tolerances under test — same trio as every case] */
tol_slide = 0.20;
tol_press = 0.10;
tol_hole  = 0.30;

/* [Interface dims — mirror the case defaults] */
stud_gap = 30.0;
kh_head_d = 7.0;  kh_shank_d = 4.2;  kh_slot_l = 8.0;  kh_head_h = 3.5;  kh_face = 1.0;
kh_click = 0.25;  // detent bump proud of the head-channel ceiling (0 = no click)
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;
clip_bw = 17.5;   // the WAP's board width — the CLIP station is its coupon verbatim
pcb_t = 1.2;  standoff_h = 3.0;  standoff_d = 4.0;
screw_d = 1.6;  screw_head_d = 4.0;
screw_step = 0.15;   // pilot ladder: three holes at screw_d − step / screw_d / + step
insert_d = 3.5;  insert_h = 4.0;
mag_d = 6.0;  lp_d = 3.0;
lip_t = 1.2;  lip_h = 4.0;
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;   // matches the catalog gasket recipe (20 % squeeze, ~86 % fill)
usb_w = 12.0;  usb_h = 6.5;   // the WAP case's USB-C opening (clears rugged cable boots)
port_wall = 2.0;              // case-typical wall the PORT opening is cut through

/* [Coupon] */
base_t = 5.0;        // base thickness (pockets live in it)
label_depth = 0.5;
emboss_h = 0.6;      // raised-wordmark height (EMBOSS station)
label_font = "Liberation Sans:style=Bold";
brand_raised = "CANARY";     // the EMBOSS station's wordmark
brand_sunk   = "SecuraCV";   // the DEBOSS station's wordmark
emblem_h = 9.0;      // EMBLEM station: mark height (0 removes the station).
                     // The mark is ~1.23x wider than tall — at 9 it spans
                     // 11.1 mm, inside the clear air between the wordmarks.
emblem_rib = 0.7;    // stroke width, in mm. THE number this station tests:
                     // no part of the mark is drawn narrower than this, so
                     // if the rib survives, the whole emblem survives.
emblem_crown = 0.15; // how far the stroke tops are drawn in, giving the
                     // strokes a domed crown instead of a flat slab top —
                     // the difference between a printed mark that catches
                     // light like metal and one that reads as a sticker.
                     // 0 = flat top.

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
glyph_show  = true;
glyph_size  = 34.0;   // mark width. At 34 the thinnest stroke lands at
                      // ~0.44 mm — one 0.42 line. Smaller is not a style
                      // choice, it is an unprintable mark
glyph_grow  = 0.14;   // per-side thickening that buys that 0.44
glyph_depth = 0.5;    // = label_depth's sibling; one bridged layer closes it
glyph_dx    = -26.5;  // the clear rectangle on the bed face: left of the
glyph_dy    = 12.0;   // PORT through-hole, above the keyhole pockets

/* [Quality] */
$fa = 3; $fs = 0.4;

bw = 90; bh = 68;
echo(str("Canary fit coupon v0.3-dev — base ", bw, "x", bh, "  (IN DEVELOPMENT)"));
if (emblem_h > 0) {
    echo(str("EMBLEM station: mark ", emblem_h, " tall x ", emblem_h*1.23,
             " wide, ", emblem_rib, " mm ribs, ", emblem_crown, " mm crown"));
    assert(emblem_rib >= 0.4,
           "emblem_rib below 0.4 mm — thinner than one 0.4 mm extrusion, the mark will not print");
    assert(emblem_crown < emblem_rib/2,
           "emblem_crown >= half the rib — the crown would pinch the strokes off at the top");
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

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
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
    translate([px, py, 0]) union() {
        translate([0, -kh_slot_l/2, -0.1]) linear_extrude(kh_face + 0.1) {
            circle(d = kh_head_d);
            hull() { circle(d = kh_shank_d); translate([0, kh_slot_l]) circle(d = kh_shank_d); }
        }
        translate([0, -kh_slot_l/2, kh_face]) linear_extrude(kh_head_h - kh_face)
            hull() { circle(d = kh_head_d + 0.6); translate([0, kh_slot_l]) circle(d = kh_head_d + 0.6); }
    }
}
// The WAP's boardclip profile verbatim, rooted on the coupon face: beam +
// inward hook + 45° lead-in. sy = +1/-1 picks which long edge of the CLIP
// station's board channel the clip guards — one on EACH side, like the case.
module stationclip(cx, cy, sy) {
    bt = base_t + standoff_h + pcb_t;  tp = bt + clip_hook_h;
    pts = [ [clip_clear, base_t], [clip_clear + clip_t, base_t],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([cx, cy + sy*clip_bw/2, 0]) scale([1, sy, 1])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
}
// POCKET click detent: a shallow dome on the head-channel ceiling at slot
// mid-travel — cleared during stud insertion, cammed over on the slide, and
// the head parks BEHIND it (kh_click 0.25 against the head's 0.1 ceiling
// clearance = 0.15 squeeze). Slide on, CLICK, stays until a firm pull back.
module keyhole_click(px, py) {
    translate([px, py, kh_head_h]) scale([0.9, 0.9, kh_click]) sphere(1);
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
            // The through profile is the WAP's port VERBATIM (square lower corners,
            // 45°-chamfered top corners) so a boot that passes here passes the case.
            translate([2, 16, -0.1]) linear_extrude(base_t - port_wall + 0.1)
                rrect2d(usb_w + 6, usb_h + 5, 2);
            translate([2, 16, -0.1]) linear_extrude(base_t + 0.2)
                polygon([[-usb_w/2, -usb_h/2], [usb_w/2, -usb_h/2],
                         [usb_w/2, usb_h/2 - 2.5], [usb_w/2 - 2.5, usb_h/2],
                         [-usb_w/2 + 2.5, usb_h/2], [-usb_w/2, usb_h/2 - 2.5]]);
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
        if (emblem_h > 0) emblem_emb(-3, 27.5, emblem_h);
        // CLIP station — the WAP clip coupon, both sides: board-rest rails flush
        // to the board's edge lines and a snap clip on EACH long edge. Press a
        // 1.2 mm scrap (or real board edge) past the lead-ins and feel the click.
        for (sy = [1, -1]) {
            translate([-37, -16.5 + sy*(clip_bw/2 - standoff_d/2) - standoff_d/2, base_t - 0.01])
                cube([12, standoff_d, standoff_h + 0.01]);
            stationclip(-31, -16.5, sy);
        }
        // POCKET station: the click detents, one per keyhole
        if (kh_click > 0) for (s = [1, -1]) keyhole_click(pocket_cx + s*stud_gap/2, -14);
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
        // T-studs at stud_gap (hang on the base's pockets, slide down)
        for (s = [1, -1]) translate([s*stud_gap/2, 0, 3 - 0.01]) {
            cylinder(d = 4.0, h = kh_face + 0.4);
            translate([0, 0, kh_face + 0.4]) cylinder(d1 = 4.0, d2 = 6.6, h = 1.2);
            translate([0, 0, kh_face + 1.6]) cylinder(d = 6.6, h = 0.8);
        }
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

if      (part == "base")  base();
else if (part == "mate")  mate();
else if (part == "strip") strip();
else { base(); translate([-12, -50, 0]) mate(); translate([28, -50, 0]) strip(); }
