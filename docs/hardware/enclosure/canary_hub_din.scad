// ============================================================================
//  Canary — HUB ENCLOSURE (Raspberry Pi 5, DIN rail)  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  The server side of SecuraCV (compose stack: broker, verifier, dashboards)
//  lives on a Pi-class box. This is a vented tray + cover for a Raspberry
//  Pi 5 that clips onto a standard 35 mm top-hat DIN rail (or screws flat),
//  with all port faces open, chimney venting over the SoC, and an M.2/SSD
//  HAT height budget.
//
//  2026-08-23: rrect/rrect2d now come from canary_core_lib (same geometry,
//  one home), and the DIN leaf spring's ~0.8 % strain claim is now a
//  canary_snap_lib assert instead of prose — checked on every tray render.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. The DIN
//     clip is a printed spring — PETG minimum, verify engagement on your rail.
//
//  2026-09-03 assembly review: the cover's four corner posts stood 2.7 mm
//  INSIDE the Pi's footprint (fused into its standoffs, through the board's
//  plane) — the board could not go in. The cover now screws down the Pi's own
//  hole grid, the standard Pi-case stack: M2.5 screws through the cover into
//  female standoffs (11 mm bare, 16 mm with a HAT) seated in the tray's
//  standoffs. The DIN clip is its own part (part="clip"): integral, it hung
//  9 mm under the floor and printed in neither orientation; it screws to the
//  floor with two M3s and prints rail-side down. Its lands now ride the
//  rail's flange FACES (they stood outboard of the rail and bore on nothing),
//  and the port faces open from the standoff line, not 0.5 mm off the floor.
//
//  2026-09-03 audit fix: the cover's screw-guide tubes were a fixed `ch - 2`
//  = 20 mm and drove 9.0 mm THROUGH the standoffs they were meant to meet —
//  the cover could not close, and had no Z datum when it did. The tubes are
//  now tapered boss_tower()s whose derived length (hat_h - standoff_ff) seats
//  their bottom faces exactly on the F-F standoff tops: the datum. The screw
//  callout is derived and echoed (the old comment said "M3 x 25"; the screws
//  are M2.5, and much shorter).
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d — the catalog's shared helpers
use <canary_snap_lib.scad>   // beam-strain arithmetic — the DIN leaf is a snap fit
use <canary_rib_lib.scad>    // boss_tower — the cover's screw tubes, tapered + gated

/* [What to render] */
part = "all";        // ["tray","cover","clip","all"]

/* [Board] — Raspberry Pi 5 */
pi_l = 85.0;         // X
pi_w = 56.0;         // Y
hole_dx = 58.0;      // mount hole grid (X)
hole_dy = 49.0;      // mount hole grid (Y)
hole_off_x = 3.5;    // grid corner from board corner
hole_off_y = 3.5;
pcb_t = 1.4;
standoff_h = 6.0;    // clearance under the board (PoE HAT pins / SD access)
hat_h = 22.0;        // headroom above the PCB (HAT/M.2 + fan)
standoff_ff = 11.0;  // the F-F brass standoffs on the Pi's hole grid: 11 bare,
                     // 16 with a HAT. The cover's guide tubes SEAT on their
                     // tops — this number is the cover's Z datum, so measure it

/* [Shell] */
wall_t = 2.0;  floor_t = 3.0;  lid_t = 2.0;  corner_r = 3.0;   // wall_t: catalog default shell — core_wall(), canary_core_lib
                                                                // floor 3.0: the clip screws' cone seats leave a 1.35 web
board_clear = 1.0;
tol_slide = 0.20;  tol_hole = 0.30;   // catalog defaults — core_tol_*(), canary_core_lib
screw_d = 2.2;       // M2.5 self-tap into the standoffs
screw_head_d = 5.0;
lid_screw_d = 2.8;   // M2.5 CLEARANCE: the cover's screws run down the Pi's hole grid into F-F
                     // standoffs (no corner posts — they stood inside the Pi footprint)
clip_screw_d = 3.4;  // M3 clearance through the floor into the clip's lands (self-tap 2.6 there)
lid_edge  = 0.8;   // first (45°) stage of the cover's edge break, mm — the house face edge, core_face_edge()  // [0:0.1:1.5]
lid_edge2 = 0.8;   // second (~66°) stage, mm: what makes the edge read as a roundover instead of a bevel — core_face_edge2()  // [0:0.1:1.5]

/* [DIN rail] — 35 mm top-hat (EN 50022) */
din = true;
din_w = 35.2;        // rail width across the flanges
din_lip = 1.2;       // hook capture depth behind each flange edge (TS35 lip ~1 mm)
din_t = 1.5;         // rail metal + spring clearance

/* [Venting] */
vent_slot_w = 2.0;
vent_slot_p = 5.0;   // pitch

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
inner_l = pi_l + 2*board_clear;
inner_w = pi_w + 2*board_clear;
out_l = inner_l + 2*wall_t;
out_w = inner_w + 2*wall_t;
tray_h = floor_t + standoff_h + pcb_t + 6;     // tray walls stop above the PCB
clip_screws = [[-6.5, 23.5], [6.5, 23.5], [-6.5, -23.5], [6.5, -23.5]];   // M3s, floor -> clip lands (between the vent slots)
total_h = floor_t + standoff_h + pcb_t + hat_h + lid_t;
// The cover's guide tubes run from the lid's inner face DOWN TO the F-F
// standoff tops, and no further: tube bottom = standoff top is the cover's
// Z datum (it used to have none — 2.0 mm of free travel, closing by landing
// on the bare Pi PCB), and the old fixed `ch - 2` length drove the tubes
// 9.0 mm THROUGH the standoffs they were meant to meet: the cover could not
// close at all. Derived, the length tracks standoff_ff and hat_h together.
tube_h = hat_h - standoff_ff;
assert(tube_h >= 2.0,
       str("hub cover: standoff_ff (", standoff_ff, ") nearly fills hat_h (", hat_h,
           ") — no room for the guide tubes; raise hat_h or shorten the standoffs"));
// the fastener, derived: through the head seat + lid + tube, then ~4 mm of
// thread into the standoff's female M2.5
echo(str("hub cover screws: 4 x M2.5 x ", ceil((lid_t - 1.2) + tube_h + 4),
         " machine, through the cover into the ", standoff_ff,
         " mm F-F standoffs (tube bottoms seat on the standoff tops = the Z datum)"));
echo(str("Canary hub (Pi 5, DIN) v0.1-dev — ", out_l, " x ", out_w, " x ", total_h,
         " mm  (IN DEVELOPMENT)"));

function holes() = [
    [-pi_l/2 + hole_off_x,           -pi_w/2 + hole_off_y],
    [-pi_l/2 + hole_off_x + hole_dx, -pi_w/2 + hole_off_y],
    [-pi_l/2 + hole_off_x,           -pi_w/2 + hole_off_y + hole_dy],
    [-pi_l/2 + hole_off_x + hole_dx, -pi_w/2 + hole_off_y + hole_dy],
];

// DIN clip on the tray underside — v0.2. (v0.1 was a solid block: the riser
// pad filled the rail channel, so hooks and "spring" were buried in plastic
// and the arm's strain math never worked.) Now: two lands ride on the flange
// FACES with the rail seated between them, a full-width fixed lip captures
// the TOP flange, and a leaf spring running PARALLEL to the rail (free length
// ~20-26 mm -> ~0.8 % PETG strain at 1 mm ride-over) latches the BOTTOM
// flange, with a finger release tab at its free end. Hook underside bridges
// print fine at this width; add supports if your bridging is poor.
module din_clip() {
    cw  = 42;                                   // clip block width along the rail
    gap = 1.6;                                  // land face -> lip top: clears 1.3 mm flange metal
    lh  = 9.5;                                  // land height: the TS35 rail's 7.5 mm crown stands
                                                // between the lands toward the tray — 6 mm of land
                                                // put it 1.5 mm into the floor
    hz  = -lh - gap - 1.6;                      // hook plates' bottom z
    // The leaf is a printed spring, so the snap doctrine applies: beam
    // arithmetic from canary_snap_lib, CYCLE budget (a rail latch is worked
    // on every install/remove). Real numbers: 1.8 mm arm flexing 1.0 mm of
    // ride-over across a free length of root -> hook (cw*0.55 - 5 = 18.1).
    // That is the "~0.8 % PETG strain" the comment above claims — now
    // checked on every tray render instead of trusted.
    arm_free = cw*0.55 - 5;
    assert(snap_strain(1.8, 1.0, arm_free) <= snap_budget_cycle(),
           str("din_clip: leaf strain ", round(snap_strain(1.8, 1.0, arm_free)*1000)/10,
               " % exceeds the ", round(snap_budget_cycle()*1000)/10,
               " % cycle budget — lengthen or thin the arm; do not deepen the hook"));
    translate([-cw/2, 0, 0]) {
        // riser lands: they ride the rail's flange FACES (from 4 mm inboard of each
        // flange edge outward) — outboard only, they bore on nothing and the clip
        // had ~4.6 mm of play toward the panel
        translate([0,  din_w/2 - 4, -lh]) cube([cw, 4 + din_t + 9, lh + 0.01]);
        translate([0, -din_w/2 - din_t - 9, -lh]) cube([cw, 4 + din_t + 9, lh + 0.01]);
        // bridges tying the two lands into ONE part across the rail channel, at
        // both ends, in the 1.5 mm above the crown (the tray floor used to do this)
        for (x = [0, cw - 3]) translate([x, -din_w/2 - din_t - 9, -1.5]) cube([3, din_w + 2*din_t + 18, 1.51]);
        // fixed hook, full width, over the TOP flange: drop plate + inward lip
        translate([0, din_w/2 + din_t, hz]) cube([cw, 1.8, gap + 3.21]);
        translate([0, din_w/2 - din_lip, hz]) cube([cw, din_lip + din_t + 1.8, 1.6]);
        // bottom latch: root block + leaf arm + mid hook + release tab
        translate([0, -din_w/2 - din_t - 5, hz]) cube([5, 5, gap + 3.21]);          // root (ties to the land)
        translate([5 - 0.01, -din_w/2 - 2.4, hz]) cube([cw - 5, 1.8, 1.6]);         // leaf arm
        translate([cw*0.55, -din_w/2 - 2.0, hz]) cube([8, din_lip + 2.0, 1.6]);     // hook behind the flange
        // 45° lead-in on the hook's flange-facing edge: the strain claim assumes a cam
        translate([cw*0.55, -din_w/2 - 2.0, hz]) rotate([0, 0, 0])
            hull() { cube([8, 0.01, 1.6]); translate([0, -1.0, 0]) cube([8, 0.01, 0.01]); }
        translate([cw - 4, -din_w/2 - 7, hz]) cube([4, 6.5, 1.6]);                  // release tab
    }
}
// the clip as a PART: rail side down on the bed (lips and leaf on the plate,
// lands on top), M3 self-tap pilots up into the lands
module clip() {
    difference() {
        rotate([180, 0, 0]) din_clip();
        for (c = clip_screws) translate([c[0], c[1], 9.5 - 6.1]) cylinder(d = 2.6, h = 7);
    }
}

module tray() {
    union() {
        difference() {
            rrect(out_l, out_w, corner_r, tray_h);
            translate([0, 0, floor_t]) rrect(inner_l, inner_w, max(0.1, corner_r - wall_t), tray_h);
            // port faces fully open ABOVE the standoff line on both short walls
            // (a centered cube reached 0.55 mm off the floor and notched the floor edge)
            for (s = [1, -1]) translate([s*(inner_l/2 + wall_t/2), 0, floor_t + standoff_h + tray_h/2])
                cube([wall_t*2, inner_w - 6, tray_h], center = true);
            // USB/eth face also open on one long wall (Pi 5 ports are on one long edge)
            translate([0, -(inner_w/2 + wall_t/2), floor_t + standoff_h + tray_h/2])
                cube([inner_l - 10, wall_t*2, tray_h], center = true);
            // clip screws through the floor: 90° cone seats for M3 flat heads (canary_core_lib)
            if (din) for (c = clip_screws) cs_cone90_cut(c[0], c[1], floor_t, clip_screw_d, 1.65);
            // floor vent slots
            for (x = [-inner_l/2 + 8 : vent_slot_p : inner_l/2 - 8])
                translate([x, 0, -0.1]) linear_extrude(floor_t + 0.2)
                    rrect2d(vent_slot_w, inner_w - 16, 1);
        }
        // Pi standoffs (M2.5 self-tap), Ø6 — inside the Pi 5's Ø6.2 keep-out ring
        for (h = holes()) translate([h[0], h[1], floor_t])
            difference() {
                cylinder(d = 6, h = standoff_h);
                translate([0, 0, 1]) cylinder(d = screw_d, h = standoff_h + 1);
            }
    }
}

// cover: slides over the tray walls; chimney vents over the SoC zone; screwed
// down the Pi's hole grid with M2.5 machine screws (length echoed at render)
// through guide tubes that seat on the F-F standoff tops
module cover() {
    ch = total_h - tray_h + 4;                  // cover skirt height (4 mm overlap)
    union() {
        difference() {
            union() {
                // The cover is modeled print-side down (z = 0 is the OUTER
                // face), so the house soft edge is flipped onto that face.
                // This file declared lid_edge and never read it: one
                // occurrence in 224 lines, the declaration. The Customizer
                // advertised a softened edge and the part printed a raw
                // square slab.
                translate([0, 0, lid_t]) scale([1, 1, -1])
                    soft_edge_plate(out_l + 2*tol_slide + 3.2,
                                    out_w + 2*tol_slide + 3.2,
                                    corner_r + 1.6, lid_t, lid_edge, lid_edge2);
                translate([0, 0, lid_t - 0.01]) linear_extrude(ch)
                    difference() {
                        rrect2d(out_l + 2*tol_slide + 3.2, out_w + 2*tol_slide + 3.2, corner_r + 1.6);
                        rrect2d(out_l + 2*tol_slide, out_w + 2*tol_slide, corner_r + tol_slide);
                    }
            }
            // top chimney vents
            for (x = [-25 : vent_slot_p : 25])
                translate([x, 0, -0.1]) linear_extrude(lid_t + 0.2) rrect2d(vent_slot_w, out_w - 24, 1);
            // side louvres on the skirt (rear long wall)
            for (x = [-out_l/2 + 12 : 8 : out_l/2 - 12])
                translate([x, (out_w + 3.2)/2 + 1, lid_t + 6]) rotate([90, 0, 0])
                    linear_extrude(6) rrect2d(3, ch - 10, 1.4);
            // PORT WINDOWS: the closed skirt walled off every Pi connector.
            // Notches matching the tray's three open port faces, cut OPEN to
            // the skirt's free edge so the cover drops on with cables plugged.
            // (Assembled world z 9.9..26 = cover-local total_h-26 upward.)
            for (s = [1, -1])
                translate([s*(out_l/2 + 2), 0, total_h - 26 + 15])
                    cube([6, inner_w - 6, 30], center = true);
            translate([0, -(out_w/2 + 2), total_h - 26 + 15])
                cube([inner_l - 10, 6, 30], center = true);
            // cover screws on the Pi's hole grid: through-holes + flat pan-head seats
            // (a cone under a pan head bears on its rim — canary_core_lib)
            // (the cover is modeled print-side down: z = 0 is the OUTER face, so
            // the seat is cut from that face — cut from z = lid_t it sat under
            // the tubes' roots and set them adrift)
            for (h = holes()) translate([h[0], h[1], lid_t]) mirror([0, 0, 1])
                cb_flat_cut(0, 0, lid_t, lid_screw_d, screw_head_d + 0.6, 1.2);
        }
        // screw tubes guiding the M2.5s down to the standoffs on the Pi grid.
        // boss_tower (canary_rib_lib): tapered so the root section carries the
        // side-nudge loads the bare Ø5.2 x 20 tube failed — 20 / 1.2 was 16.7,
        // past the library's 12x slenderness ceiling. tube_h lands the bottom
        // face exactly on the standoff tops (see the derivation above).
        for (h = holes())
            translate([h[0], h[1], lid_t - 0.01])
                boss_tower(lid_screw_d + 2.4, lid_screw_d, tube_h + 0.01, taper = 1.6);
    }
}

if      (part == "tray")  tray();
else if (part == "cover") cover();
else if (part == "clip")  { assert(din, "the clip needs din=true"); clip(); }
else { tray(); translate([out_l + 20, 0, 0]) cover(); if (din) translate([0, -(out_w + 30), 0]) clip(); }
