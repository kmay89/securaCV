// ============================================================================
//  Canary — FUNCTIONAL INSERTS  ⚠️ IN DEVELOPMENT (v0.2-dev)
//  Three small parts referenced by the enclosure docs but never modeled:
//    part = "horn"       — exponential buzzer horn: glues INSIDE the WAP lid
//                          over the vent cluster; channels the piezo into the
//                          hole ring for a slightly more directional chirp.
//                          It needs horn_h of room over the PCB top under the
//                          vent: the camera presets leave 12.4; compact_plain
//                          leaves 6.35, so no horn this tall fits it.
//    part = "glare_ring" — matte washer behind a camera disc: kills internal
//                          reflections off glossy prints (print MATTE BLACK).
//    part = "gland_body" / "gland_bush" / "gland_cap" — three-part printed
//                          cable gland (rigid body + TPU bush + press cap)
//                          for the relay/gang cable pass-throughs. The TPU
//                          bush is squeezed between the nose bore and the
//                          cable; the cap snaps over the barb and holds it
//                          down on its seat, and its skirt clamps the panel
//                          onto the flange's O-ring.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//
//  v0.2-dev (2026-10-23): three fits measured in their cases, one print pose.
//    HORN — set in the WAP lid at the vent, its Ø20.8 flange (mouth 14 + the
//    walls + 4) met the magnet pocket ring (8.8 mm³) and the camera module
//    (8.1 mm³): the vent is 8.2 from both. The mouth is now the Ø12 vent-pad
//    recess and the glue land is the wall's own 1.4 (Ø14.8: 0.76 clear of the
//    module, 0.84 of the ring). It printed throat-down — 24 mm² on the bed
//    and the flange a 3.4 mm flat overhang — so part = "horn" now lays the
//    glue face on the bed; horn() keeps its assembled pose.
//    GLARE RING — 0.6 under a 1.0 disc in a seat cut disc_t + 0.2 deep stood
//    the disc 0.4 proud of the face, and Ø13.4 in the Ø14.4 seat floated 0.5
//    a side. It is one 0.2 layer now (the disc lands flush) at the disc's own Ø.
//    GLAND — nothing held the panel: the cap parked 4.3 above the panel's
//    inside face, so the flange's O-ring saw no squeeze and the body could
//    slide 5.4 out of its hole. A skirt carries the cap down to the panel,
//    and the O-ring groove is 0.65 deep so the cap's 0.1 of snap slack rests
//    the cord at 25 %.
// ============================================================================

use <canary_snap_lib.scad>   // snap_budget_once — the cap's one-time snap strain

/* [What to render] */
part = "horn";       // ["horn","glare_ring","gland_body","gland_bush","gland_cap"]

/* [Horn] — matches the WAP lid vent cluster defaults */
horn_throat = 4.0;   // over the buzzer port
horn_mouth  = 12.0;  // at the lid face: the WAP's Ø12 vent-pad recess, so the wall end glues on the flat round it and the mouth still covers the Ø6 vent-hole ring.
                     // (v0.2: 14, plus a 4 mm flange, put a Ø20.8 rim into the camera module and the
                     // magnet ring; Ø14.8 now clears both.) Physics honesty: a
                     // lid-scale horn cannot LOAD a ~4 kHz chirp (flare cutoff ~8.5 kHz,
                     // mouth << lambda) - expect a few dB of on-axis directivity, not "louder"
horn_h      = 8.0;
horn_wall   = 1.4;

/* [Glare ring] — matches the camera disc seats */
gr_od = 14.0;        // the disc's own Ø: the seat is cut disc + 2*tol_slide (Ø14.4), so the ring centers like the disc (12 mm seats: set 12.0)
gr_id = 10.5;        // just over the aperture
gr_t  = 0.2;         // one layer: the seats are cut disc_t + 0.2 deep, so the disc lands flush (at 0.6 it stood 0.4 proud of the face)

/* [Cable gland] */
cg_hole   = 8.4;     // panel hole it fits: the relay lid's (its M8 thread + 2 x tol_slide since the 2026-10-22 audit; at 8.2 the barb no longer caught there)
cg_cable  = 4.5;     // cable diameter the bush grips
cg_panel  = 2.0;     // panel thickness
cg_flange = 14.0;
cg_squeeze = 0.25;   // bush wall squeeze on the cable (the O-ring rule's band)  // [0.2:0.05:0.3]

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide(), canary_core_lib
tol_press = 0.10;    // catalog default — core_tol_press(), canary_core_lib

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary inserts v0.2-dev — ", part, "  (IN DEVELOPMENT)"));

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
// the panel clamp: a skirt below the lip carries the cap down to the panel's
// inside face, so the snap pulls the flange's O-ring onto the panel (v0.2: with
// no skirt the cap parked 4.3 above the panel and nothing clamped the body)
cg_skirt     = cg_barb_z - 0.1 - cg_lip_h - (2.0 + cg_panel);   // 4.3: the lip's underside to the panel
cg_cap_h     = cg_cap_floor + (cg_top + cg_gap - cg_barb_z) + 0.1 + cg_lip_h + cg_skirt;  // lip 0.1 under the shoulder
cg_oring_d   = 1.0;                         // the flange O-ring's cord
cg_oring_dep = 0.65;                        // its groove: + the cap's 0.1 of snap slack, the cord rests at 0.75 — 25 %
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
assert(cg_skirt > 0, "gland cap: the barb sits too close to the panel for the cap's skirt to reach it");
assert(1 - (cg_oring_dep + 0.1)/cg_oring_d >= 0.2 && 1 - (cg_oring_dep + 0.1)/cg_oring_d <= 0.3,
       "gland O-ring: the groove + the cap's 0.1 of snap slack must rest the cord in the 20-30 % squeeze band");

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
    // mouth collar: the glue land on the lid underside round the vent-pad recess
    // (v0.2: a flange 2 mm proud of the wall, Ø20.8, ran into the camera module
    // and the magnet ring)
    translate([0, 0, horn_h - 1.2]) difference() {
        cylinder(d = horn_mouth + 2*horn_wall, h = 1.2);
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
        translate([0, 0, 2.0 - cg_oring_dep]) difference() {
            cylinder(d = cg_hole + 2*cg_oring_d + 1.3, h = cg_oring_dep + 0.05);
            translate([0, 0, -0.1]) cylinder(d = cg_hole + 0.7, h = cg_oring_dep + 0.25);
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
// snap cap (prints floor-down): the barb passes the skirt free, the lip cams
// over its taper, 3.75 % hoop once, and parks under its flat shoulder; the
// floor bears on the bush and the skirt's end on the panel
module gland_cap() {
    lip_top = cg_cap_h - cg_skirt;   // the lip's skirt-side face
    difference() {
        cylinder(d = cg_hole + 3.2, h = cg_cap_h);
        translate([0, 0, -0.1]) cylinder(d = cg_cap_hole, h = cg_cap_h + 0.2);
        translate([0, 0, cg_cap_floor]) cylinder(d = cg_groove_d, h = lip_top - cg_cap_floor - cg_lip_h);  // barb groove
        translate([0, 0, lip_top - cg_lip_h - 0.01]) cylinder(d = cg_lip_d, h = cg_lip_h + 0.1);           // snap lip
        translate([0, 0, lip_top - 0.3]) cylinder(d1 = cg_lip_d, d2 = cg_lip_d + 0.6, h = 0.31);            // lead-in onto the lip
        translate([0, 0, lip_top]) cylinder(d = cg_groove_d, h = cg_skirt + 0.1);                            // skirt bore: the barb passes free
    }
}
// the three in place (body frame), for the fit check: bush on its seat, cap snapped
module gland_assembly() {
    gland_body();
    translate([0, 0, cg_seat_z]) gland_bush();
    translate([0, 0, cg_top + cg_gap + cg_cap_floor]) mirror([0, 0, 1]) gland_cap();
}

if      (part == "horn")       translate([0, 0, horn_h + 0.01]) mirror([0, 0, 1]) horn();   // print pose (v0.2): the glue face on the bed (the cone runs 0.01 past it)
else if (part == "glare_ring") glare_ring();
else if (part == "gland_body") gland_body();
else if (part == "gland_bush") gland_bush();
else if (part == "gland_cap")  gland_cap();
else if (part == "gland_fit")  gland_assembly();   // fit check only (all three in place) — not a printable
