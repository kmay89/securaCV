// ============================================================================
//  Canary — HAMMOND INTERNAL CHASSIS PLATE  ⚠️ IN DEVELOPMENT (v0.1-dev)
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
// ============================================================================

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
wap_l = 21.0;  wap_w = 17.5;  wap_pcb = 1.2;
vm_l  = 25.0;  vm_w  = 25.0;                 // Grove Vision AI V2
sm_l  = 44.0;  sm_w  = 36.0;                 // MR60BHA2 carrier
stack_sock_h = 11.5;                          // seated-XIAO stack height
pcb_t = 1.0;
standoff_h = 3.0;

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
soff = (stack == "wap") ? standoff_h : stack_sock_h + 1.5;
assert(b_w + 6 < plate_w && b_l + 6 < plate_l, "board exceeds the plate — grow plate_l/w");
assert(boss_dx/2 + 1 + (boss_screw_d + 2*tol_hole)/2 + 2 <= plate_l/2 && boss_dy/2 + (boss_screw_d + 2*tol_hole)/2 + 2 <= plate_w/2,
       "boss slots too close to the plate edge (need >= 2 mm web) — grow plate_l/plate_w");
echo(str("Canary Hammond chassis v0.1-dev — plate ", plate_l, "x", plate_w,
         ", stack=", stack, " on ", soff, " mm rails  (IN DEVELOPMENT)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module edgeclip(px, py, ang) {
    bt = plate_t + soff + b_t;  tp = bt + clip_hook_h;
    pts = [ [clip_clear, plate_t], [clip_clear + clip_t, plate_t],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
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
        // rails (notched at the clips) + clips — same cradle idiom as the cases
        for (s = [1, -1]) {
            difference() {
                translate([s*(b_w/2 - 1.5) - 1.5, -(b_l - 1)/2, plate_t - 0.01])
                    cube([3, b_l - 1, soff + 0.01]);
                translate([s*(b_w/2 - 1.5), 0, plate_t + soff/2])
                    cube([5, clip_w + 2, soff + 1], center = true);
            }
            edgeclip(s*b_w/2, 0, s > 0 ? 0 : 180);
        }
    }
}

plate();
