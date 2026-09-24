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
//  floor with four M3s (the v0.3 note below has its print pose). Its lands now ride the
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
//
//  2026-09-24 assembly + print review: the cover assembles turned over about
//  X (the only flip that keeps its tubes on the Pi grid), and its long-wall
//  port window and louvers are now cut mirrored for it — they landed on the
//  wrong long walls, the skirt closing over the USB/Ethernet edge. The pan
//  seats keep a 1.0 floor. The DIN clip (v0.3) prints hooks-down with every
//  land face a bridge of <= 7 mm; it had no supportless pose (its 37 mm leaf
//  floated 1.6 over the land).
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d — the catalog's shared helpers
use <canary_snap_lib.scad>   // beam-strain arithmetic — the DIN leaf is a snap fit
use <canary_rib_lib.scad>    // boss_tower — the cover's screw tubes, tapered + gated
use <canary_port_lib.scad>   // port_flat_span_max — the clip's land faces print as bridges no longer than it

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
// The cover's guide tubes SEAT on the F-F standoff tops, making this number
// the cover's Z datum — measure the standoffs actually fitted.
standoff_ff = 11.0;  // F-F brass standoffs on the Pi's hole grid: 11 bare, 16 with a HAT

/* [Shell] */
wall_t = 2.0;     // wall thickness — catalog default shell, core_wall(), canary_core_lib
floor_t = 3.0;    // floor thickness — 3.0: the clip screws' cone seats leave a 1.35 web
lid_t = 2.0;
corner_r = 3.0;   // catalog default — core_corner_r(), canary_core_lib
board_clear = 1.0;
tol_slide = 0.20;  // catalog default — core_tol_slide(), canary_core_lib
tol_hole  = 0.30;  // catalog default — core_tol_hole(), canary_core_lib
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
total_h = floor_t + standoff_h + pcb_t + hat_h + lid_t;
// The cover's guide tubes run from the lid's inner face DOWN TO the F-F
// standoff tops, and no further: tube bottom = standoff top is the cover's
// Z datum (it used to have none — 2.0 mm of free travel, closing by landing
// on the bare Pi PCB), and the old fixed `ch - 2` length drove the tubes
// 9.0 mm THROUGH the standoffs they were meant to meet: the cover could not
// close at all. Derived, the length tracks standoff_ff and hat_h together.
tube_h = hat_h - standoff_ff;
// pan-head seat depth: the house floor under a head is >= 1.0 (DESIGN_RULES
// §4) — the old 1.2 seat left 0.8 of the 2.0 cover under each head
head_seat = lid_t - 1.0;
assert(head_seat > 0 && lid_t - head_seat >= 1.0 - 1e-9, "hub cover: the head seat leaves under 1.0 mm of floor");
assert(tube_h >= 2.0,
       str("hub cover: standoff_ff (", standoff_ff, ") nearly fills hat_h (", hat_h,
           ") — no room for the guide tubes; raise hat_h or shorten the standoffs"));
// the fastener, derived: through the head seat + lid + tube, then ~4 mm of
// thread into the standoff's female M2.5
echo(str("hub cover screws: 4 x M2.5 x ", ceil((lid_t - head_seat) + tube_h + 4),
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

// DIN clip on the tray underside — v0.3. The TS35 rail's crown is screwed to
// the panel and its two 1 mm flanges stand 7.5 mm off it, toward the tray: the
// lands bear on the flange FACES, a full-width fixed lip reaches behind the TOP
// flange, and a leaf spring running PARALLEL to the rail (free length root ->
// hook ~18 mm, strain asserted below) latches the BOTTOM flange, with a release
// tab at its free end standing out past the land. (v0.1 was a solid block; v0.2
// modeled the crown as standing between the lands toward the tray, which left
// no room behind the flanges for the hooks it drew — and printed hooks-up with
// a 37 mm leaf floating 1.6 over the land.)
//
// v0.3 PRINTS HOOKS-DOWN, as assembled: the lip, leaf, hook and tab are the
// first layers, and every land face above them is anchored on both sides so
// it prints as a bridge no longer than port_flat_span_max():
//   - a KEEL drops 3.2 mm into the rail's recess between the flange roots
//     (|y| <= keel_hw, clear of the crown's 25 mm inner width) and carries the
//     middle of the land plate down to the bed;
//   - on the fixed side the flange face spans keel -> drop plate (7.0);
//   - on the latch side POSTS stand in the flange-edge clearance, beside the
//     leaf and clear of the hook, so the face spans keel -> post (5.7) and
//     post -> outer wall over the leaf's 1.6 relief (4.0); over the hook,
//     where no post can stand, the face is a 45° gable between the posts.
// Clip frame: z = 0 is the tray's underside, the clip hangs below it.
// (Hidden: the clip's own geometry, not Customizer knobs.)
/* [Hidden] */
cw      = 42;                  // clip block width along the rail
din_gap = 1.6;                 // land face -> lip top: clears 1.3 mm flange metal
din_lh  = 9.5;                 // land height: the tray's stand-off from the flange faces
din_hz  = -din_lh - din_gap - 1.6;   // hook plates' bottom z: the print bed
din_crown_hw = 12.5;           // TS35 crown inner half-width (27 outside, 1.0 metal)
keel_hw = din_w/2 + din_t - port_flat_span_max();   // the fixed-side span keel -> drop plate is 7.0
land_o  = din_w/2 + din_t + 10;                     // lands' outer edge (|y|)
post_y0 = din_w/2 + 0.2;                            // latch posts: 0.2 outboard of the flange edge ...
post_y1 = post_y0 + 1.2;                            // ... 1.2 wide (three lines)
leaf_t  = 1.8;                                      // leaf arm thickness (it flexes in y)
leaf_y0 = post_y1 + 0.6;                            // leaf inner face: 0.6 off the posts (no fused gap)
leaf_y1 = leaf_y0 + leaf_t;
leaf_rel = 1.6;                                     // relief outboard of the leaf (it rides over 1.0)
hook_x  = cw*0.55;                                  // hook position along the leaf
hook_l  = 6.0;                                      // hook length along the rail: the posts' span over it is hook_l + 0.8
tab_w   = 4.0;
assert(keel_hw <= din_crown_hw - 0.3, "din_clip: the keel does not clear the rail's crown walls");
assert((hook_l + 0.8)/2 < din_lh - 1.0, "din_clip: the gable over the hook reaches through the land");
assert(post_y1 + 0.6 + leaf_t + leaf_rel - post_y1 <= port_flat_span_max(), "din_clip: the land over the leaf spans more than a bridge");
function clip_screws() = [[-6.5, 25.5], [6.5, 25.5], [-6.5, -25.5], [6.5, -25.5]];   // M3s, floor -> clip lands (between the vent slots)
assert(clip_screws()[0][1] - 1.3 >= leaf_y1 + leaf_rel + 0.8 - 1e-9 && clip_screws()[0][1] + 1.3 <= land_o - 1.2,
       "din_clip: an M3 pilot breaks into the leaf relief or out of the land");

module din_clip() {
    // The leaf is a printed spring, so the snap doctrine applies: beam
    // arithmetic from canary_snap_lib, CYCLE budget (a rail latch is worked
    // on every install/remove). Real numbers: a 1.8 mm arm flexing the hook's
    // whole reach behind the flange (din_lip — the old gate assumed 1.0 of a
    // 1.2 lip) across a free length of root -> hook (cw*0.55 - 5 = 18.1).
    arm_free = hook_x - 5;
    assert(din_lip + 0.4 <= leaf_rel, "din_clip: the leaf's relief is shallower than its ride-over");
    assert(snap_strain(leaf_t, din_lip, arm_free) <= snap_budget_cycle(),
           str("din_clip: leaf strain ", round(snap_strain(leaf_t, din_lip, arm_free)*1000)/10,
               " % exceeds the ", round(snap_budget_cycle()*1000)/10,
               " % cycle budget — lengthen or thin the arm; do not deepen the hook"));
    hz = din_hz;
    translate([-cw/2, 0, 0]) union() {
      difference() {
        union() {
            // the land plate: one face on both flanges and across the recess
            translate([0, -land_o, -din_lh]) cube([cw, 2*land_o, din_lh]);
            // keel into the rail's recess: the plate's middle reaches the bed
            translate([0, -keel_hw, hz]) cube([cw, 2*keel_hw, -hz - din_lh + 0.01]);
            // FIXED side (+y): the outer land reaches the bed (it is the drop
            // plate), the lip reaches behind the top flange
            translate([0, din_w/2 + din_t, hz]) cube([cw, land_o - din_w/2 - din_t, -hz - din_lh + 0.01]);
            translate([0, din_w/2 - din_lip, hz]) cube([cw, din_lip + din_t + 0.01, 1.6]);
            // LATCH side (-y): outer wall to the bed outboard of the leaf relief
            translate([0, -land_o, hz]) cube([cw, land_o - leaf_y1 - leaf_rel, -hz - din_lh + 0.01]);
            // root block: anchors the leaf and ties outer wall to posts
            translate([0, -post_y1 - 0.01, hz]) mirror([0, 1, 0]) cube([5, land_o - post_y1, -hz - din_lh + 0.01]);
            // posts in the flange-edge clearance, clear of the hook by 0.4
            for (seg = [[5, hook_x - 0.4], [hook_x + hook_l + 0.4, cw]])
                translate([seg[0] - 0.01, -post_y1, hz]) cube([seg[1] - seg[0] + 0.01, post_y1 - post_y0, -hz - din_lh + 0.01]);
        }
        // the tab's slot: the land stands clear of the leaf's free end and the
        // tab, bed to tray face (a slot, not a roof over them)
        translate([cw - tab_w - leaf_rel, -(land_o + 3), hz - 0.1]) cube([tab_w + leaf_rel + 0.1, land_o + 3 - post_y1, -hz + 0.2]);
        // over the hook the land has no post under it (the hook passes there),
        // so the face is vaulted instead of flat: a 45° gable along the rail,
        // rising from the post ends — the flange bears on the land either side
        translate([hook_x - 0.4, -keel_hw, -din_lh - 0.01]) rotate([90, 0, 0])
            linear_extrude(leaf_y1 + leaf_rel - keel_hw)
                polygon([[0, 0], [hook_l + 0.8, 0], [(hook_l + 0.8)/2, (hook_l + 0.8)/2]]);
        // lighten the keel from the tray side (a pocket opens upward: no overhang)
        translate([2, -(keel_hw - 2), hz + 2]) cube([cw - 4, 2*(keel_hw - 2), -hz]);
      }
            // leaf arm, hook behind the flange, release tab out past the land
            translate([5 - 0.01, -leaf_y1, hz]) cube([cw - 5 + 0.01, leaf_t, 1.6]);
            translate([hook_x, -leaf_y0 - 0.01, hz]) difference() {
                cube([hook_l, leaf_y0 - (din_w/2 - din_lip) + 0.01, 1.6]);
                // 45° lead-in on the hook's panel-side inboard edge: the flange
                // edge cams the leaf outward (the strain claim assumes a cam);
                // on the bed it is a 45° overhang, not a flat
                translate([hook_l/2, leaf_y0 - (din_w/2 - din_lip) + 0.01, 0]) rotate([45, 0, 0])
                    cube([hook_l + 0.2, 1.2*sqrt(2), 1.2*sqrt(2)], center = true);
            }
            translate([cw - tab_w, -(land_o + 2), hz]) cube([tab_w, land_o + 2 - leaf_y0, 1.6]);
    }
}
// the clip as a PART, printed hooks-down (z = 0 the bed = the hook plates'
// panel side), M3 self-tap pilots down into the lands from the tray face
module clip() {
    difference() {
        translate([0, 0, -din_hz]) din_clip();
        for (c = clip_screws()) translate([c[0], c[1], -din_hz - 6.1]) cylinder(d = 2.6, h = 7);
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
            if (din) for (c = clip_screws()) cs_cone90_cut(c[0], c[1], floor_t, clip_screw_d, 1.65);
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
            // The cover is modeled in its PRINT pose and assembles turned over
            // about X (rotate([180, 0, 0]): y -> -y), the one flip that keeps
            // the tubes on the Pi's hole grid (it is symmetric in Y, not in X).
            // So everything that must land on a given LONG wall is cut
            // mirrored in Y here: drawn at -Y, the port window assembled at +Y
            // and the skirt walled off the Pi's USB/Ethernet edge (811.6 mm3 of
            // skirt in the connector envelope), while the louvers opened over
            // it; flipped about Y instead, the ports opened and the tubes
            // missed the standoffs.
            mirror([0, 1, 0]) {
                // side louvers on the skirt (rear long wall: +Y assembled)
                for (x = [-out_l/2 + 12 : 8 : out_l/2 - 12])
                    translate([x, (out_w + 3.2)/2 + 1, lid_t + 6]) rotate([90, 0, 0])
                        linear_extrude(6) rrect2d(3, ch - 10, 1.4);
                // the long-wall PORT WINDOW (-Y assembled, over the tray's open face)
                translate([0, -(out_w/2 + 2), total_h - 26 + 15])
                    cube([inner_l - 10, 6, 30], center = true);
            }
            // PORT WINDOWS: the closed skirt walled off every Pi connector.
            // Notches matching the tray's open short port faces, cut OPEN to
            // the skirt's free edge so the cover drops on with cables plugged.
            // (Assembled world z 9.9..26 = cover-local total_h-26 upward.)
            for (s = [1, -1])
                translate([s*(out_l/2 + 2), 0, total_h - 26 + 15])
                    cube([6, inner_w - 6, 30], center = true);
            // cover screws on the Pi's hole grid: through-holes + flat pan-head seats
            // (a cone under a pan head bears on its rim — canary_core_lib)
            // (the cover is modeled print-side down: z = 0 is the OUTER face, so
            // the seat is cut from that face — cut from z = lid_t it sat under
            // the tubes' roots and set them adrift)
            for (h = holes()) translate([h[0], h[1], lid_t]) mirror([0, 0, 1])
                cb_flat_cut(0, 0, lid_t, lid_screw_d, screw_head_d + 0.6, head_seat);
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
