// ============================================================================
//  Canary — HAMMOND INTERNAL CHASSIS PLATE  ⚠️ IN DEVELOPMENT (v0.2-dev)
// @env ip="IP66/67/68 (from the bought Hammond box)" note="the rating is the purchased box's, earned only if the gland + vent are installed correctly — not our printed plate's"
//  The build plan's harsh-outdoor path (ENC1) is a Hammond 1554/1555
//  polycarbonate box — which arrives EMPTY. This plate screws to the box's
//  internal bosses and carries the proven Canary rail/clip cradle for a
//  selectable stack, bridging our board-mounting system into the purchased
//  IP66/67 route:
//    stack = "wap"    — XIAO ESP32-S3 (Sense) board, snap clips
//    stack = "vision" — Grove Vision AI V2 + stacked XIAO (tall rails)
//    stack = "sense"  — MR60BHA2 carrier + stacked XIAO C6 (tall rails;
//                       radar must face the box's CLEAR LID — no metal above)
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//     MEASURE your box's boss spacing (Hammond publishes drawings per size;
//     defaults below are placeholders for a 1554F-class box) and your boards.
//
//  v0.2-dev (2026-08-23): canary_*_lib adoption; the Grove Vision AI V2 plate
//  arithmetic now reads the registry's measured 40 x 20 — this file still
//  carried the wrong 25 x 25 that three siblings had already corrected.
// ============================================================================

use <canary_core_lib.scad>   // rrect2d — the catalog's shared helpers
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_board_lib.scad>  // board registry — vm_l/vm_w read it; the knobs cite it

/* [What to render] */
part  = "plate";     // ["plate"]
stack = "wap";       // ["wap","vision","sense"]

/* [Hammond boss grid] — MEASURE your box / check the Hammond drawing */
boss_dx = 147.0;     // boss spacing (X)
boss_dy = 77.0;      // boss spacing (Y)
boss_screw_d = 4.2;  // Hammond lid/boss screws are typically M4/#6 into inserts
plate_l = 158.0;     // plate outline (fits inside the box walls) — MEASURE your box
plate_w = 88.0;
plate_t = 2.4;

/* [Boards] — nominal; MEASURE (same meanings as the device enclosures) */
wap_l = 21.0;  wap_w = 17.5;  wap_pcb = 1.2;  // XIAO spec — brd_l/brd_w/brd_t("xiao");
                                              // the clips' clip_clear absorbs the
                                              // measured 17.8 (brd_xiao_w_measured())
// The Grove module is what it is — a fixed arithmetic input, not a knob — so
// its size comes from the registry instead of a retype. This file carried the
// wrong 25 x 25 long after three siblings measured 40 x 20; the registry (and
// its board_selfcheck()) is why that cannot happen a fourth time.
vm_l  = brd_l("grove_v2");                   // Grove Vision AI V2, measured 40
vm_w  = brd_w("grove_v2");                   // ... x 20 (the 1x2 form, not a square)
sm_l  = 44.0;  sm_w  = 36.0;                 // MR60BHA2 carrier — brd_l/brd_w("mr60")
stack_sock_h = 11.5;                          // seated-XIAO stack height (module underside
                                              // -> XIAO underside): measured 6.5 per
                                              // canary_board_lib brd_stack_sock_measured();
                                              // 11.5 is unmeasured headroom — print-
                                              // validated as a roomier cavity, never
                                              // confirmed as a stack height — MEASURE
pcb_t = 1.0;                                  // brd_t("grove_v2") / brd_t("mr60")
standoff_h = 3.5;    // 3.5: the WAP-stack clip beam is standoff + PCB, and at 4.7 mm the measured
                     // 17.8 XIAO inserts under the 4.5 % strain budget (4.2 ran 5.5 %)
xiao_w = 17.8;       // the measured XIAO (brd_xiao_w_measured) — sets the vision stack's corner pins
xiao_below = 5.5;    // air under a stacked XIAO's USB face (shell + half a plug overmold + clearance)

/* [Board snap clips] */
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;

/* [Extras] */
tie_slots = true;    // zip-tie slots for cable dressing / battery strap
tol_hole  = 0.30;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
b_l  = (stack == "wap") ? wap_l : (stack == "vision") ? vm_l : sm_l;
b_w  = (stack == "wap") ? wap_w : (stack == "vision") ? vm_w : sm_w;
b_t  = (stack == "wap") ? wap_pcb : pcb_t;
soff = (stack == "wap") ? standoff_h : stack_sock_h + xiao_below;
assert(b_w + 6 < plate_w && b_l + 6 < plate_l, "board exceeds the plate — grow plate_l/w");
assert(boss_dx/2 + 1 + (boss_screw_d + 2*tol_hole)/2 + 2 <= plate_l/2 && boss_dy/2 + (boss_screw_d + 2*tol_hole)/2 + 2 <= plate_w/2,
       "boss slots too close to the plate edge (need >= 2 mm web) — grow plate_l/plate_w");
echo(str("Canary Hammond chassis v0.2-dev — plate ", plate_l, "x", plate_w,
         ", stack=", stack, " on ", soff, " mm rails  (IN DEVELOPMENT)"));

// (rrect2d comes from canary_core_lib — the local copy is gone)
// Cantilever snap clip at edge point (px,py); `ang` = outward normal. The
// drawing is canary_snap_lib's beam (the WAP pattern), so the insertion-strain
// arithmetic runs as an assert on every render. This file's clips stand on the
// ±X board edges, so the wrapper keeps the axis rotation and hands the drawing
// to the library — same route the Sense case takes.
module edgeclip(px, py, ang) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, plate_t, plate_t + soff + b_t,
                       clip_w, clip_t, clip_hook, clip_hook_h, clip_clear,
                       over = (stack == "wap") ? (brd_xiao_w_measured() - wap_w)/2 : 0);
}

module plate() {
    union() {
        difference() {
            linear_extrude(plate_t) rrect2d(plate_l, plate_w, 6);
            // boss screws (slotted for boss-grid tolerance)
            for (sx = [1, -1], sy = [1, -1]) hull() for (d = [-1, 1])
                translate([sx*boss_dx/2 + d, sy*boss_dy/2, -0.1])
                    cylinder(d = boss_screw_d + 2*tol_hole, h = plate_t + 0.2);
            // zip-tie slots flanking the board zone
            if (tie_slots) for (sy = [1, -1], i = [-1, 0, 1])
                translate([i*(b_l/2 + 12), sy*(b_w/2 + 8), -0.1])
                    linear_extrude(plate_t + 0.2) rrect2d(4, 8, 1.5);
            // lightening/vent grid outside the board zone
            for (sx = [1, -1], i = [0, 1, 2])
                translate([sx*(plate_l/2 - 14), (i - 1)*18, -0.1])
                    linear_extrude(plate_t + 0.2) rrect2d(10, 10, 3);
        }
        // rails (notched at the clips) + clips — same cradle idiom as the cases.
        // The vision stack's rails run beside the TOP HALF only: the 17.8 XIAO
        // hangs under the lower half of the 20 mm module, and full-length rails
        // ran through it (the Vision case's fix); corner pins catch the low edge
        for (s = [1, -1]) {
            half = (stack == "vision");
            rl = half ? b_l/2 - 4 : b_l - 1;
            y0 = half ? 2 : -(b_l - 1)/2;
            cyc = half ? 2 + rl/2 : 0;
            difference() {
                translate([s*(b_w/2 - 1.5) - 1.5, y0, plate_t - 0.01])
                    cube([3, rl, soff + 0.01]);
                translate([s*(b_w/2 - 1.5), cyc, plate_t + soff/2])
                    cube([5, clip_w + 2, soff + 1], center = true);
            }
            edgeclip(s*b_w/2, cyc, s > 0 ? 0 : 180);
            if (half) translate([s*(xiao_w/2 + 1.1), -b_l/2 + 1.2, plate_t - 0.01]) cylinder(d = 2.0, h = soff + 0.01);
        }
    }
}

plate();
