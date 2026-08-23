// ============================================================================
//  Canary — FUNCTIONAL INSERTS  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Three small parts referenced by the enclosure docs but never modeled:
//    part = "horn"       — exponential buzzer horn: glues INSIDE the WAP lid
//                          over the vent cluster; channels the piezo into the
//                          hole ring for a slightly more directional chirp.
//    part = "glare_ring" — matte washer behind a camera disc: kills internal
//                          reflections off glossy prints (print MATTE BLACK).
//    part = "gland_body" / "gland_bush" / "gland_cap" — three-part printed
//                          cable gland (rigid body + TPU bush + press cap)
//                          for the relay/gang cable pass-throughs. The TPU
//                          bush compresses around the cable as the cap is
//                          pressed home over the barbs.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

/* [What to render] */
part = "horn";       // ["horn","glare_ring","gland_body","gland_bush","gland_cap"]

/* [Horn] — matches the WAP lid vent cluster defaults */
horn_throat = 4.0;   // over the buzzer port
horn_mouth  = 14.0;  // at the lid face (covers the vent hole ring). Physics honesty: a
                     // lid-scale horn cannot LOAD a ~4 kHz chirp (flare cutoff ~8.5 kHz,
                     // mouth << lambda) - expect a few dB of on-axis directivity, not "louder"
horn_h      = 8.0;
horn_wall   = 1.4;

/* [Glare ring] — matches the camera disc seats */
gr_od = 13.4;        // fits inside the 14 mm disc seat (12 mm seats: set 11.4)
gr_id = 10.5;        // just over the aperture
gr_t  = 0.6;

/* [Cable gland] */
cg_hole   = 8.2;     // panel hole it fits (relay lid default)
cg_cable  = 4.5;     // cable diameter the bush grips
cg_panel  = 2.0;     // panel thickness
cg_flange = 14.0;

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide(), canary_core_lib
tol_press = 0.10;    // catalog default — core_tol_press(), canary_core_lib

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary inserts v0.1-dev — ", part, "  (IN DEVELOPMENT)"));

// exponential-ish horn: stacked conic sections from throat to mouth
module horn() {
    n = 5;
    difference() {
        union() for (i = [0 : n - 1]) {
            d0 = horn_throat + (horn_mouth - horn_throat) * pow(i/n, 2) + 2*horn_wall;
            d1 = horn_throat + (horn_mouth - horn_throat) * pow((i + 1)/n, 2) + 2*horn_wall;
            translate([0, 0, horn_h * i/n]) cylinder(d1 = d0, d2 = d1, h = horn_h/n + 0.01);
        }
        union() for (i = [0 : n - 1]) {
            d0 = horn_throat + (horn_mouth - horn_throat) * pow(i/n, 2);
            d1 = horn_throat + (horn_mouth - horn_throat) * pow((i + 1)/n, 2);
            translate([0, 0, horn_h * i/n - 0.01]) cylinder(d1 = d0, d2 = d1, h = horn_h/n + 0.03);
        }
        // glue flange face at the mouth stays flat (lid side)
    }
    // mouth flange that glues to the lid underside around the vent ring
    translate([0, 0, horn_h - 1.2]) difference() {
        cylinder(d = horn_mouth + 2*horn_wall + 4, h = 1.2);
        translate([0, 0, -0.1]) cylinder(d = horn_mouth, h = 1.5);
    }
}

module glare_ring() {
    difference() {
        cylinder(d = gr_od, h = gr_t);
        translate([0, 0, -0.1]) cylinder(d = gr_id, h = gr_t + 0.2);
    }
}

// gland body: flanged tube through the panel, barbed nose for the cap
module gland_body() {
    difference() {
        union() {
            cylinder(d = cg_flange, h = 2.0);                            // outer flange
            translate([0, 0, 2.0 - 0.01]) cylinder(d = cg_hole - 2*tol_slide, h = cg_panel + 0.5);
            translate([0, 0, 2.0 + cg_panel + 0.4]) cylinder(d = cg_hole - 2*tol_slide, h = 5);  // nose (>=0.7 mm wall)
            translate([0, 0, 2.0 + cg_panel + 5.4]) cylinder(d1 = cg_hole - 0.2, d2 = cg_hole - 1.2, h = 1.8); // barb at the
                                                                 // nose tip, aligned with the seated cap's relief
        }
        // two-step bore sized so every wall stays >= 0.7 mm (mesh-check catch:
        // a wide full-depth bore hollowed the stem and nose into a floating ring)
        translate([0, 0, -0.1]) cylinder(d = cg_cable + 1.9, h = 5.6);
        translate([0, 0, 5.4]) cylinder(d = cg_cable + 1.0, h = 12);
    }
}
// TPU bush: cone that squeezes onto the cable inside the body
module gland_bush() {
    difference() {
        cylinder(d1 = cg_cable + 1.8, d2 = cg_cable + 0.9, h = 5);
        translate([0, 0, -0.1]) cylinder(d = cg_cable - 0.4, h = 6);
    }
}
// press cap: rides over the barb, drives the bush home
module gland_cap() {
    difference() {
        cylinder(d = cg_hole + 3.2, h = 5.5);
        translate([0, 0, 1.2]) cylinder(d = cg_hole - 0.6, h = 6);   // 0.4 diametral interference over the
                                                                     // barb (~2.5 % hoop strain) — actually snaps
        translate([0, 0, -0.1]) cylinder(d = cg_cable + 1.2, h = 6);
        translate([0, 0, 3.4]) cylinder(d1 = cg_hole - 0.4, d2 = cg_hole + 0.6, h = 2.2); // barb relief
    }
}

if      (part == "horn")       horn();
else if (part == "glare_ring") glare_ring();
else if (part == "gland_body") gland_body();
else if (part == "gland_bush") gland_bush();
else if (part == "gland_cap")  gland_cap();
