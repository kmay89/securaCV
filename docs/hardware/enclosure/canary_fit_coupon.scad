// ============================================================================
//  Canary — UNIVERSAL FIT-CHECK COUPON  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  ONE ~25-minute print that calibrates your printer for the ENTIRE Canary
//  catalog before you commit to any case. Every fit the enclosures use is
//  exercised on a labelled station:
//
//    BASE (part="base", rigid):
//      CLIP    — snap clip + rail: press a 1.2 mm PCB edge (or scrap) in
//      POCKET  — two blind keyhole pockets (gap 30): hang the MATE's studs
//      SLIDE   — lip channel: the MATE's rib must slide in snugly  -> tol_slide
//      GROOVE  — straight gasket groove: seat the TPU STRIP        -> gasket fit
//      PRESS   — 6 mm magnet pocket + 3 mm light-pipe hole         -> tol_press
//      SCREW   — M2 post (drive a screw ~0.3 N·m) + countersink    -> screw_d
//      INSERT  — heat-set boss (only if you'll use screw_insert)   -> insert_d
//    MATE (part="mate"): two T-studs + the slide rib
//    STRIP (part="strip"): TPU gasket bar for the groove
//
//  If a station is tight/loose, adjust the matching tol_* / clip_* / screw
//  parameter in the case you print next. Labels are debossed beside each.
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
insert_d = 3.5;  insert_h = 4.0;
mag_d = 6.0;  lp_d = 3.0;
lip_t = 1.2;  lip_h = 4.0;
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;   // matches the catalog gasket recipe (20 % squeeze, ~86 % fill)

/* [Coupon] */
base_t = 5.0;        // base thickness (pockets live in it)
label_depth = 0.5;
label_font = "Liberation Sans:style=Bold";

/* [Quality] */
$fa = 3; $fs = 0.4;

bw = 74; bh = 58;
echo(str("Canary fit coupon v0.1-dev — base ", bw, "x", bh, "  (IN DEVELOPMENT)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module lbl(x, y, s) { translate([x, y, base_t - label_depth]) linear_extrude(label_depth + 0.1)
    text(s, size = 4.5, font = label_font, halign = "center", valign = "center"); }
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

module base() {
    union() {
        difference() {
            linear_extrude(base_t) rrect2d(bw, bh, 4);
            // POCKET station: keyhole pocket pair for the mate's studs
            keyhole_pocket(-24, -15 + stud_gap);                 // upper (gap = stud_gap, matches the mate)
            keyhole_pocket(-24, -15);                            // lower (gap = stud_gap)
            // SLIDE station: female channel for the mate's lip rib
            translate([2, 18, base_t - lip_h + 1]) linear_extrude(lip_h)
                rrect2d(30 + 2*tol_slide, lip_t + 2*tol_slide, 0.3);
            // GROOVE station: straight gasket groove
            translate([2, 8, base_t - gasket_groove]) linear_extrude(gasket_groove + 0.1)
                rrect2d(30, gasket_w, 0.3);
            // PRESS station: magnet pocket + light-pipe hole
            translate([24, -4, base_t - 2.2]) cylinder(d = mag_d + 2*tol_press, h = 2.3);   // matches mag_h 2.2
            translate([32, -4, -0.1]) cylinder(d = lp_d + 2*tol_press, h = base_t + 0.2);
            // SCREW station: countersunk clearance hole (lid side of the joint)
            translate([16, -16, -0.1]) cylinder(d = screw_d + 2*tol_hole, h = base_t + 0.2);
            translate([16, -16, base_t - 1.6]) cylinder(d1 = screw_d + 2*tol_hole, d2 = screw_head_d, h = 1.7);
            // labels
            lbl(-24, -26, "POCKET");  lbl(2, 24.5, "SLIDE");  lbl(2, 12.5, "GROOVE");
            lbl(28, 2, "PRESS");      lbl(16, -22, "SCREW");  lbl(30, -22, "INSERT");
            lbl(-2, -12, "CLIP");
        }
        // CLIP station: rail + snap clip (press a 1.2 mm scrap under the hook)
        translate([-8, -16, base_t - 0.01]) cube([12, 3, standoff_h + 0.01]);
        edgeclip(-2, -13 + 0, 90);
        // SCREW station: M2 self-tap post
        translate([16, -8, base_t - 0.01]) difference() {
            cylinder(d = 5, h = 8);
            translate([0, 0, 1.5]) cylinder(d = screw_d, h = 8);
        }
        // INSERT station: heat-set boss
        translate([30, -8, base_t - 0.01]) difference() {
            cylinder(d = insert_d + 2.4, h = insert_h + 2);
            translate([0, 0, 2 - insert_h + insert_h]) cylinder(d = insert_d - 0.1, h = insert_h + 2.1);
        }
    }
}

module mate() {
    union() {
        difference() {
            linear_extrude(3) rrect2d(46, 20, 3);
            translate([0, -6.5, 3 - label_depth]) linear_extrude(label_depth + 0.1)
                text("MATE", size = 3.2, font = label_font, halign = "center", valign = "center");
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
else { base(); translate([0, -44, 0]) mate(); translate([44, -44, 0]) strip(); }
