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
//                          bush is squeezed between the nose bore and the
//                          cable; the cap snaps over the barb and holds it
//                          down on its seat.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

use <canary_snap_lib.scad>   // snap_budget_once — the cap's one-time snap strain

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
cg_squeeze = 0.25;   // bush wall squeeze on the cable (the O-ring rule's band)  // [0.2:0.05:0.3]

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide(), canary_core_lib
tol_press = 0.10;    // catalog default — core_tol_press(), canary_core_lib

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary inserts v0.1-dev — ", part, "  (IN DEVELOPMENT)"));

// ---- cable gland arithmetic (body z: flange face at 0, nose up) ---------------
// The bush used to be a Ø6.3→5.4 cone driven into the Ø5.5 nose bore: its
// section was 2.3x the room around the cable, the cap's Ø5.7 floor hole let
// it pass straight through, and the cap's bore widened toward its mouth, so
// nothing snapped. Now the bush is a sleeve sized like an O-ring gland:
cg_bore   = cg_cable + 1.0;                 // nose bore (5.5): 0.5 radial room round the cable
cg_nose   = cg_hole - 2*tol_slide;          // nose / stem OD (7.8)
cg_barb_z = 2.0 + cg_panel + 5.4;           // barb shoulder (the flat step the cap catches)
cg_barb_d = cg_hole + 0.1;                  // barb OD at the shoulder (8.3)
cg_top    = cg_barb_z + 1.8;                // body top (barb tip)
cg_seat_z = 6.2;                            // top of the seat lip the bush stands on
cg_seat_d = cg_cable + 0.4;                 // seat lip bore: passes the cable, stops the bush
// bush: slides into the bore (0.1 diametral), wall thick enough that the cable
// squeezes it cg_squeeze: installed wall = the 0.5 room, so free wall = room / (1 - squeeze)
cg_room   = (cg_bore - cg_cable)/2;
cg_bush_od = cg_bore - 0.1;
cg_bush_id = cg_bush_od - 2*cg_room/(1 - cg_squeeze);   // 4.07 at 25 %: grips a 4.5 cable
// TPU does not compress, it flows: squeezed to the room's section the sleeve
// grows by A_free / A_room (9.9 / 7.85 = 1.26x), so its free length is set to
// fill 90 % of the cavity from the seat to the cap floor — the O-ring rule's
// fill ceiling, the 10 % left for tolerance and heat
cg_gap    = 0.2;                            // cap floor clear of the barb tip
cg_cav_l  = cg_top + cg_gap - cg_seat_z;    // 5.2: seat to the cap floor
cg_a_free = PI/4*(cg_bush_od*cg_bush_od - cg_bush_id*cg_bush_id);
cg_a_room = PI/4*(cg_bore*cg_bore - cg_cable*cg_cable);
cg_fill   = 0.90;
cg_bush_l = cg_fill * cg_cav_l * cg_a_room / cg_a_free; // 3.71 free -> 4.68 squeezed in 5.2
// cap: a floor that bears on the bush's end (its hole passes the cable, not
// the bush), a groove that houses the barb, and a lip below it that snaps
// past the barb and sits under its flat shoulder
cg_cap_floor = 1.2;
cg_cap_hole  = cg_cable + 0.3;              // 4.8 < the bush's 5.4: the floor drives the bush
cg_lip_d     = cg_nose + 0.2;               // 8.0: slides on the nose, catches the Ø8.3 barb
cg_lip_h     = 1.0;
cg_groove_d  = cg_barb_d + 0.3;             // 8.6: the barb parks free inside
cg_cap_h     = cg_cap_floor + (cg_top + cg_gap - cg_barb_z) + 0.1 + cg_lip_h;  // lip 0.1 under the shoulder
assert(cg_squeeze >= 0.2 && cg_squeeze <= 0.3, "cg_squeeze: keep the bush in the 20-30 % squeeze band");
assert(cg_bush_id < cg_cable && cg_bush_id > cg_seat_d - 1.5, "gland bush: the bore must grip the cable");
assert(cg_bush_l * cg_a_free <= cg_fill * cg_cav_l * cg_a_room + 1e-6,
       "gland bush overfills its cavity — TPU does not compress");
assert(cg_seat_z + cg_bush_l * cg_a_free / cg_a_room <= cg_top,
       "gland bush: squeezed, it rises out of the nose bore into the barb groove");
assert(cg_cap_hole < cg_bush_od && cg_cap_hole > cg_cable, "gland cap floor hole must pass the cable and stop the bush");
assert(cg_seat_d < cg_bush_od, "gland seat lip must stop the bush");
assert(cg_lip_d > cg_nose && cg_lip_d < cg_barb_d, "gland cap lip must slide on the nose and catch the barb");
assert((cg_barb_d - cg_lip_d)/cg_lip_d <= snap_budget_once(),
       "gland cap: the lip's hoop strain over the barb exceeds the one-time snap budget");
assert((cg_nose - cg_bore)/2 >= 0.7 && (cg_nose - cg_seat_d)/2 >= 0.7, "gland body: a nose wall is under 0.7 mm");

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
            // barb at the nose tip: a STEP over the nose (0.5 proud of it), tapering to
            // the tip so it enters the panel hole and the cap; the cap's bore passes
            // the nose free and snaps only over this step
            translate([0, 0, 2.0 + cg_panel + 5.4]) cylinder(d1 = cg_hole + 0.1, d2 = cg_hole - 1.2, h = 1.8);
        }
        // O-ring groove in the flange's panel face (Ø1 cord, 25 % squeeze):
        // a flat flange sealed nothing — this is what makes it a gland
        translate([0, 0, 2.0 - 0.75]) difference() {
            cylinder(d = cg_hole + 2*1.0 + 1.3, h = 0.8);
            translate([0, 0, -0.1]) cylinder(d = cg_hole + 0.7, h = 1.0);
        }
        // two-step bore sized so every wall stays >= 0.7 mm (mesh-check catch:
        // a wide full-depth bore hollowed the stem and nose into a floating ring)
        translate([0, 0, -0.1]) cylinder(d = cg_cable + 1.9, h = 5.6 - 0.75);
        // seat lip for the bush: a 45° cone in (prints unsupported), a 0.8
        // lip that passes the cable, then the nose bore the bush lives in
        translate([0, 0, 4.75]) cylinder(d1 = cg_cable + 1.9, d2 = cg_seat_d, h = (cg_cable + 1.9 - cg_seat_d)/2);
        translate([0, 0, 4.7]) cylinder(d = cg_seat_d, h = cg_seat_z - 4.7 + 0.01);
        translate([0, 0, cg_seat_z]) cylinder(d = cg_bore, h = 12);
    }
}
// TPU bush: a sleeve that slides into the nose bore and is squeezed onto the
// cable (the arithmetic above); it stands on the seat lip, the cap holds it there
module gland_bush() {
    difference() {
        cylinder(d = cg_bush_od, h = cg_bush_l);
        translate([0, 0, -0.1]) cylinder(d = cg_bush_id, h = cg_bush_l + 0.2);
    }
}
// snap cap (prints floor-down): the lip cams over the barb's taper, 3.75 %
// hoop once, and parks under its flat shoulder; the floor bears on the bush
module gland_cap() {
    difference() {
        cylinder(d = cg_hole + 3.2, h = cg_cap_h);
        translate([0, 0, -0.1]) cylinder(d = cg_cap_hole, h = cg_cap_h + 0.2);
        translate([0, 0, cg_cap_floor]) cylinder(d = cg_groove_d, h = cg_cap_h - cg_cap_floor - cg_lip_h);  // barb groove
        translate([0, 0, cg_cap_h - cg_lip_h - 0.01]) cylinder(d = cg_lip_d, h = cg_lip_h + 0.1);         // snap lip
        translate([0, 0, cg_cap_h - 0.3]) cylinder(d1 = cg_lip_d, d2 = cg_lip_d + 0.6, h = 0.31);          // mouth lead-in
    }
}
// the three in place (body frame), for the fit check: bush on its seat, cap snapped
module gland_assembly() {
    gland_body();
    translate([0, 0, cg_seat_z]) gland_bush();
    translate([0, 0, cg_top + cg_gap + cg_cap_floor]) mirror([0, 0, 1]) gland_cap();
}

if      (part == "horn")       horn();
else if (part == "glare_ring") glare_ring();
else if (part == "gland_body") gland_body();
else if (part == "gland_bush") gland_bush();
else if (part == "gland_cap")  gland_cap();
else if (part == "gland_fit")  gland_assembly();   // fit check only (all three in place) — not a printable
