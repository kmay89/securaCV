// ============================================================================
//  Canary — SOLAR LoRa/MESHTASTIC RELAY POD  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Off-grid backhaul for remote witnesses (LoRa-mesh adapter, PR #747): a
//  pole-mounted sealed pod for a LoRa dev board (default: Heltec V3-class)
//  + an 18650 holder, with an SMA antenna bulkhead on the TOP wall, an
//  angled SOLAR ROOF bracket that doubles as the radiation shield/rain roof,
//  and hose-clamp/zip-tie channels on the back for pole mounting.
//  Sealing reuses the Canary gasket system (sealed by default — it lives on
//  a pole). Wire the panel through the roof gland note in the README.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. Measure
//     your LoRa board, battery holder and panel.
// ============================================================================

/* [What to render] */
part = "all";        // ["body","lid","roof","gasket","all"]

/* [Options] */
opt_seal = true;

/* [LoRa board] — Heltec WiFi LoRa 32 V3-class. MEASURE yours */
lb_l  = 51.0;        // board length (Y, USB end down)
lb_w  = 26.0;        // board width (X)
pcb_t = 1.2;
lb_stack = 12.0;     // tallest top-side part (OLED/pin headers)
board_clear = 0.6;

/* [18650 holder] (single-cell holder with leads) */
bh_l = 78.0;
bh_w = 21.5;
bh_h = 15.0;

/* [Antenna] — SMA bulkhead on the TOP wall */
sma_d = 6.5;

/* [Solar roof] — bracket rails take a small 6V panel. MEASURE yours */
pan_w = 70.0;        // panel width
pan_l = 110.0;       // panel length
pan_t = 3.0;         // panel thickness (slides into the rails)
roof_ang = 30;       // panel angle  // [15:5:45]

/* [Pole mount] — two channels for hose clamps / heavy zip ties */
strap_w = 9.0;       // strap width
strap_t = 2.0;       // channel depth

/* [Shell / tolerances / fasteners] */
wall_t = 2.0;  floor_t = 2.0;  lid_t = 2.0;  lip_h = 4.0;  lip_t = 1.2;  corner_r = 3.0;
tol_slide = 0.20;  tol_press = 0.10;  tol_hole = 0.30;
post_d = 5.0;  screw_d = 1.6;  screw_head_d = 4.0;  screw_head_h = 2.0;
gasket_w = 1.2;  gasket_groove = 1.0;  gasket_proud = 0.6;  skirt_h = 3.0;  skirt_t = 1.6;
usb_w = 10.5;  usb_h = 6.5;   // service USB opening, bottom wall (plug when deployed)
clip_w = 6.0;  clip_t = 1.5;  clip_hook = 0.8;  clip_hook_h = 1.2;  clip_clear = 0.25;
standoff_h = 3.0;
lid_edge = 0.8;  lid_edge2 = 0.0;
foot_cham = 0.5;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
e_seal   = opt_seal;
wall_eff = e_seal ? max(wall_t, gasket_w + 1.6) : wall_t;
pd = post_d;
post_corner = pd + 1.5;
clip_stack = clip_clear + clip_t;

// two columns: LoRa board | battery holder (both vertical, USB/leads down)
col_lb = lb_w + 2*(clip_stack + board_clear) + 0.5;
col_bh = bh_w + 2.0;
inner_x = col_lb + 2 + col_bh + 2*post_corner;
inner_y = max(lb_l, bh_l) + 2*board_clear + 10;      // + wire room at the top
cav_d   = max(standoff_h + pcb_t + lb_stack, bh_h) + 1.5;

out_x = inner_x + 2*wall_eff;
out_y = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;
lb_cx = -inner_x/2 + post_corner + col_lb/2;
bh_cx =  inner_x/2 - post_corner - col_bh/2;
lb_cy = -inner_y/2 + board_clear + lb_l/2;
bh_cy = -inner_y/2 + board_clear + bh_l/2;
usb_zc = floor_t + standoff_h + pcb_t + usb_h/2;

skirt_gap = tol_slide + 0.2;
plate_x = e_seal ? out_x + 2*(skirt_gap + skirt_t) : out_x;
plate_y = e_seal ? out_y + 2*(skirt_gap + skirt_t) : out_y;
plate_r = e_seal ? corner_r + skirt_gap + skirt_t : corner_r;

function post_xy() = [
    [ inner_x/2 - pd/2 - 0.2,  inner_y/2 - pd/2 - 0.2],
    [-inner_x/2 + pd/2 + 0.2,  inner_y/2 - pd/2 - 0.2],
    [ inner_x/2 - pd/2 - 0.2, -inner_y/2 + pd/2 + 0.2],
    [-inner_x/2 + pd/2 + 0.2, -inner_y/2 + pd/2 + 0.2],
];
assert(lip_h < cav_d, "lip_h vs cavity");
echo(str("Canary solar relay pod v0.1-dev — ", out_x, " x ", out_y, " x ", base_d + lid_t,
         " mm, panel ", pan_w, "x", pan_l, " @ ", roof_ang, " deg  (IN DEVELOPMENT)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module rrect(l, w, r, h) { linear_extrude(h) rrect2d(l, w, r); }
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
        offset(r = -w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
    }
}
module edgeclip(px, py, ang, soff) {
    bt = floor_t + soff + pcb_t;  tp = bt + clip_hook_h;
    pts = [ [clip_clear, floor_t], [clip_clear + clip_t, floor_t],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
}

module body() {
    posts = post_xy();
    union() {
        difference() {
            union() {
                rrect(out_x, out_y, corner_r, base_d);
                // thickened back hosts the strap channels WITHOUT breaching the floor
                translate([0, 0, -strap_t]) rrect(out_x, out_y, corner_r, strap_t + 0.01);
            }
            translate([0, 0, floor_t]) rrect(inner_x, inner_y, max(0.1, corner_r - wall_eff), cav_d + 1);
            // SMA bulkhead, top wall, over the LoRa column
            translate([lb_cx, out_y/2, floor_t + cav_d/2])
                rotate([-90, 0, 0]) translate([0, 0, -wall_eff*2]) cylinder(d = sma_d, h = wall_eff*4);
            // service USB, bottom wall (silicone plug when deployed)
            translate([lb_cx, -out_y/2, usb_zc]) cube([usb_w, wall_eff*3, usb_h], center = true);
            if (e_seal)
                translate([0, 0, base_d - gasket_groove])
                    linear_extrude(gasket_groove + 1) rim_ring2d(gasket_w);
            // pole strap channels, cut only within the added back slab (seal-safe)
            for (sy = [1, -1]) translate([-out_x/2 - 1, sy*inner_y/4 - strap_w/2, -strap_t - 0.1])
                cube([out_x + 2, strap_w, strap_t + 0.1]);
            // bottom-edge chamfer
            if (foot_cham > 0) difference() {
                translate([0, 0, -strap_t - 0.01]) rrect(out_x + 0.1, out_y + 0.1, corner_r, foot_cham + 0.01);
                hull() {
                    translate([0, 0, -strap_t]) rrect(out_x - 2*foot_cham, out_y - 2*foot_cham, max(0.1, corner_r - foot_cham), 0.01);
                    translate([0, 0, -strap_t + foot_cham]) rrect(out_x, out_y, corner_r, 0.01);
                }
            }
        }
        // posts + gussets
        difference() {
            union() {
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d);
                for (p = posts) {
                    sx = sign(p[0]); sy = sign(p[1]);
                    hull() { translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - lip_h - 1);
                             translate([sx*(inner_x/2 - 0.3), p[1], floor_t]) cylinder(d = 2, h = cav_d - lip_h - 1); }
                    hull() { translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - lip_h - 1);
                             translate([p[0], sy*(inner_y/2 - 0.3), floor_t]) cylinder(d = 2, h = cav_d - lip_h - 1); }
                }
            }
            for (p = posts) translate([p[0], p[1], floor_t + 2]) cylinder(d = screw_d, h = cav_d);
        }
        // LoRa rails + clips; battery-holder bay walls
        for (s = [1, -1]) {
            difference() {
                translate([lb_cx + s*(lb_w/2 - 1.5) - 1.5, lb_cy - (lb_l - 1)/2, floor_t])
                    cube([3, lb_l - 1, standoff_h]);
                translate([lb_cx + s*(lb_w/2 - 1.5), lb_cy, floor_t + standoff_h/2])
                    cube([5, clip_w + 2, standoff_h + 1], center = true);
            }
            edgeclip(lb_cx + s*lb_w/2, lb_cy, s > 0 ? 0 : 180, standoff_h);
        }
        translate([bh_cx, bh_cy, floor_t]) difference() {
            rrect(bh_w + 3, bh_l + 3, 1.5, 4);
            translate([0, 0, -0.5]) rrect(bh_w + 0.6, bh_l + 0.6, 1, 5);
        }
    }
}

module lid() {
    union() {
        difference() {
            union() {
                rrect(plate_x, plate_y, plate_r, lid_t - lid_edge);
                hull() {
                    translate([0, 0, lid_t - lid_edge]) rrect(plate_x, plate_y, plate_r, 0.01);
                    translate([0, 0, lid_t - 0.01])
                        rrect(plate_x - 2*lid_edge, plate_y - 2*lid_edge, max(0.1, plate_r - lid_edge), 0.01);
                }
            }
            for (p = post_xy()) {
                translate([p[0], p[1], -1]) cylinder(d = screw_d + 2*tol_hole, h = lid_t + 2);
                translate([p[0], p[1], lid_t - screw_head_h])
                    cylinder(d1 = screw_d + 2*tol_hole, d2 = screw_head_d, h = screw_head_h + 0.1);
            }
            // panel-lead gland hole (fit an M8 cable gland or silicone-seal)
            translate([bh_cx, inner_y/2 - 8, -1]) cylinder(d = 8.2, h = lid_t + 2);
        }
        difference() {   // lip
            translate([0, 0, -lip_h]) difference() {
                rrect(inner_x - 2*tol_slide, inner_y - 2*tol_slide, max(0.1, corner_r - wall_eff - tol_slide), lip_h);
                rrect(inner_x - 2*tol_slide - 2*lip_t, inner_y - 2*tol_slide - 2*lip_t, 0.1, lip_h + 1);
            }
            for (p = post_xy()) translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
            translate([lb_cx, -inner_y/2, -lip_h/2]) cube([usb_w + 4, lip_t*4, lip_h + 0.2], center = true);
        }
        if (e_seal) difference() {   // drip skirt
            translate([0, 0, -skirt_h]) linear_extrude(skirt_h) difference() {
                rrect2d(plate_x, plate_y, plate_r);
                rrect2d(out_x + 2*skirt_gap, out_y + 2*skirt_gap, corner_r + skirt_gap);
            }
            translate([lb_cx, -(out_y/2 + skirt_gap + skirt_t/2), -skirt_h/2])
                cube([usb_w + 6, skirt_t*3, skirt_h + 0.4], center = true);
        }
    }
}

// solar roof: angled panel rails on a mast that screws to the lid's posts
// (swap the two TOP lid screws for M2 x 12 through the roof feet)
module roof() {
    ph = pan_l * sin(roof_ang) + 14;
    difference() {
        union() {
            // two feet matching the top lid-screw posts
            for (p = [post_xy()[0], post_xy()[1]])
                translate([p[0], p[1], 0]) cylinder(d = 8, h = 3);
            // A-frame: angled panel bed with side rails
            translate([0, inner_y/2 - 2, 0]) rotate([90 - roof_ang, 0, 0]) translate([0, 0, -2]) {
                translate([-pan_w/2 - 3, 0, 0]) cube([pan_w + 6, pan_l * 0.75, 2]);       // bed
                for (s = [1, -1]) translate([s*(pan_w/2 + tol_slide) + (s < 0 ? -3 : 0), 0, 2 - 0.01])
                    cube([3, pan_l * 0.75, pan_t + 2]);                                     // side rails
                for (s = [1, -1]) translate([s*(pan_w/2 + 1.5) + (s < 0 ? -1.5 - 1.2 : 1.5 - 1.8), 0, 2 + pan_t + 0.2])
                    cube([1.8, pan_l * 0.75, 1.4]);                                         // retaining lips
                translate([-pan_w/2 - 3, -2, 0]) cube([pan_w + 6, 2, 8]);                   // lower stop
            }
            // struts from the feet up to the bed
            for (p = [post_xy()[0], post_xy()[1]])
                hull() {
                    translate([p[0], p[1], 0]) cylinder(d = 8, h = 3);
                    translate([p[0]*0.8, inner_y/2 - 4, 12]) cylinder(d = 8, h = 3);
                }
        }
        for (p = [post_xy()[0], post_xy()[1]]) {
            translate([p[0], p[1], -0.1]) cylinder(d = screw_d + 2*tol_hole, h = 30);
            translate([p[0], p[1], 3 - 1.4]) cylinder(d = screw_head_d + 0.6, h = 30);
        }
    }
}

module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.1); }

if      (part == "body")   body();
else if (part == "lid")    translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();
else if (part == "roof")   roof();
else if (part == "gasket") gasket();
else {
    body();
    translate([out_x + 20, 0, 0]) translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();
    translate([0, out_y + 30, 0]) roof();
    if (e_seal) translate([-(out_x + 16), 0, 0]) gasket();
}
