// ============================================================================
//  Canary — UNIVERSAL FIT-CHECK COUPON  ⚠️ IN DEVELOPMENT (v0.2-dev)
//  ONE small print that calibrates your printer for the ENTIRE Canary
//  catalog before you commit to any case. Every fit the enclosures use is
//  exercised on a labelled station, laid out on a branded 90 × 68 plate:
//
//    BASE (part="base", rigid) — stations left→right, top→bottom:
//      EMBOSS  — raised "CANARY" wordmark (emboss_h): crisp or blobby?
//      DEBOSS  — sunk "SecuraCV" wordmark (label_depth): every label's fate
//      SLIDE   — lip channel: the MATE's rib must slide in snugly  -> tol_slide
//      PORT    — the cases' USB-C opening through a case wall: your
//                cable's head must pass                            -> usb_w/usb_h
//      PRESS   — 6 mm magnet pocket + 3 mm light-pipe hole         -> tol_press
//      GROOVE  — straight gasket groove: seat the TPU STRIP        -> gasket fit
//      SCREW   — M2 post (drive a screw ~0.3 N·m) + countersink,
//                plus a −/0/+ pilot ladder: pick the hole that
//                bites without splitting                           -> screw_d
//      CLIP    — snap clip + rail: press a 1.2 mm PCB edge (or scrap) in
//      POCKET  — two blind keyhole pockets (gap 30): hang the MATE's studs
//      INSERT  — heat-set boss (only if you'll use screw_insert)   -> insert_d
//      GLYPH   — the SecuraCV bird debossed into the BED-SIDE face at full
//                size: the first-layer / plate-texture test, and the one
//                that matters most, because every case A-surface in this
//                catalog prints face-down (see the knobs for why it lives
//                on the underside and not between the stations)
//    MATE (part="mate"): two T-studs + the slide rib
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

/* [What to render] */
part = "all";        // ["base","mate","strip","all"]

/* [Print tolerances under test — same trio as every case] */
tol_slide = 0.20;
tol_press = 0.10;
tol_hole  = 0.30;

/* [Interface dims — mirror the case defaults] */
stud_gap = 30.0;
kh_head_d = 7.0;  kh_shank_d = 4.2;  kh_slot_l = 8.0;  kh_head_h = 3.5;  kh_face = 1.0;
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;
pcb_t = 1.2;  standoff_h = 3.0;
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
echo(str("Canary fit coupon v0.2-dev — base ", bw, "x", bh, "  (IN DEVELOPMENT)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module lbl(x, y, s, size = 4.5) { translate([x, y, base_t - label_depth]) linear_extrude(label_depth + 0.1)
    text(s, size = size, font = label_font, halign = "center", valign = "center"); }
module emb(x, y, s, size = 7) { translate([x, y, base_t - 0.05]) linear_extrude(emboss_h + 0.05)
    text(s, size = size, font = label_font, halign = "center", valign = "center"); }
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
module edgeclip(px, py, ang) {
    bt = base_t + standoff_h + pcb_t;  tp = bt + clip_hook_h;
    pts = [ [clip_clear, base_t], [clip_clear + clip_t, base_t],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
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
            // labels — grid-aligned, none touching a neighbour
            lbl(-24, 21, "EMBOSS", 3);  lbl(22, 21, "DEBOSS", 3);
            lbl(-24, 9.5, "SLIDE");     lbl(2, 9.5, "PORT");     lbl(30, 9.5, "PRESS");
            lbl(-24, -5.5, "GROOVE");   lbl(19, -5.5, "SCREW");
            for (i = [0:2]) lbl(ladder_x[i], -4.5, ladder_tag[i], 3);   // on the SCREW line, under their holes
            lbl(-31, -23, "CLIP");      lbl(pocket_cx, -25.5, "POCKET");  lbl(34, -21, "INSERT");
            // foot line: the small-text legibility test doubles as the colophon
            lbl(-2, -31, "securacv.com · fit coupon v0.2-dev", 3);
            // GLYPH station: the bird, into the bed-side face. Mirrored in x
            // because this face is read from BELOW — unmirrored art would
            // come off the plate back-to-front, notepad in the wrong wing.
            if (glyph_show)
                translate([glyph_dx, glyph_dy, -0.1]) mirror([1, 0, 0])
                    linear_extrude(glyph_depth + 0.1)
                        offset(delta = glyph_grow)
                            resize([glyph_size, 0], auto = true)
                                import("securacv_bird_glyph.svg", center = true);
        }
        // EMBOSS station: raised wordmark at the height case branding uses
        emb(-24, 27, brand_raised, 7);
        // CLIP station: rail + snap clip (press a 1.2 mm scrap under the hook)
        translate([-37, -18, base_t - 0.01]) cube([12, 3, standoff_h + 0.01]);
        edgeclip(-31, -15, 90);
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
        // slide rib (fits the base's SLIDE channel)
        translate([-15, 6.5, 3 - 0.01]) cube([30, lip_t, lip_h - 1]);
    }
}

module strip() {   // TPU gasket bar for the GROOVE station
    linear_extrude(gasket_groove + gasket_proud) rrect2d(29.4, gasket_w - 0.5, 0.3);   // strip 0.5 narrower, like the case gaskets
}

if      (part == "base")  base();
else if (part == "mate")  mate();
else if (part == "strip") strip();
else { base(); translate([-12, -50, 0]) mate(); translate([28, -50, 0]) strip(); }
