// ============================================================================
//  Canary — SHOP TOOLS  ⚠️ IN DEVELOPMENT (v0.1-dev)
//    part = "insert_guide" — heat-set insert press guide: slips over a
//        fattened corner post (screw_insert mode) and keeps the M2 insert
//        and iron tip SQUARE while it melts in — crooked inserts are the
//        #1 insert failure. Print in PETG minimum (it touches hot zones
//        briefly); remove as soon as the insert seats.
//    part = "button_ring" — decorative accent ring for the doorbell button
//        bezel seat: sits under the 12 mm button flange, prints in any
//        contrast color (the Ring-faceplate delight, in 0.4 g).
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

/* [What to render] */
part = "insert_guide";   // ["insert_guide","button_ring"]

/* [Insert guide] — matches screw_insert-mode posts */
post_d   = 6.7;      // fattened post OD (insert_d 3.5 + 2.4 wall = 5.9; WAP pd max(5, 5.9))
insert_d = 3.5;
guide_h  = 12.0;

/* [Button ring] — matches the doorbell bezel seat */
bez_d = 16.5;        // bezel seat diameter
btn_d = 12.0;        // button body diameter
ring_t = 0.8;

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide(), canary_core_lib

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary shop tools v0.1-dev — ", part, "  (IN DEVELOPMENT)"));

module insert_guide() {
    difference() {
        cylinder(d = post_d + 2*tol_slide + 4.8, h = guide_h);
        // pocket that seats over the post (post top flush with the step)
        translate([0, 0, -0.1]) cylinder(d = post_d + 2*tol_slide, h = 6.1);
        // guide bore above: keeps the insert + iron tip square
        translate([0, 0, 5.9]) cylinder(d = insert_d + 0.4, h = guide_h);
        // side window to watch the insert seat
        translate([0, -(post_d + 8)/2 - 1, 7]) rotate([-90, 0, 0])
            cylinder(d = 4, h = post_d + 12);
    }
}

module button_ring() {
    difference() {
        cylinder(d = bez_d - 2*tol_slide, h = ring_t);
        translate([0, 0, -0.1]) cylinder(d = btn_d + 2*tol_slide + 0.6, h = ring_t + 0.2);
    }
}

if (part == "insert_guide") insert_guide();
else button_ring();
