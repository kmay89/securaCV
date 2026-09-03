// ============================================================================
//  Canary — SOLAR LoRa/MESHTASTIC RELAY POD  ⚠️ IN DEVELOPMENT (v0.1-dev)
// @env cer=2 ip="CER-2 (→3 after test)"
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
//
//  ⚠️ THERMAL: a dark sealed pod charging at solar noon can exceed the 18650's
//     0..45 °C charge window — print in a LIGHT color, rely on the roof shade,
//     and set a charge-temperature cutoff in firmware (see field_ratings.md).
//     bh_l = 78 suits unprotected 65 mm cells; protected cells run to 69 mm.
//  ⚠️ SEAL HONESTY: four corner screws clamp ~85 mm gasket spans — that is
//     CER-2 (splash) hardware by the project's own clamp-spacing rule; claim
//     CER-3 only after a verified W-2 pass. Antenna: keep the panel's lower
//     edge >= 2 cm above the SMA and prefer a whip whose radiating half
//     clears the roof plane — a panel 17 mm off the feedpoint detunes it.
//
//  2026-08-23: adopted the shared contract libraries (core/snap/port/board/
//              mark) — the local helper copies they replace drew the same
//              geometry. Two fixes ride along: the bridge-safe chamfered
//              service-USB opening (canary_port_lib — this wall bridged a
//              flat top) and PAN-head FLAT counterbores on the lid (the
//              Vision's print-validated lesson — the old shallow cone left
//              the head standing on the show face). New opt_mark knob
//              debosses the house wordmark (default off).
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, soft-edge lid, screw seats — the shared idiom
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_port_lib.scad>   // bridge-safe USB opening (the WAP's print-validated profile)
use <canary_board_lib.scad>  // board registry — the LoRa knob defaults cite
                             // brd_l/brd_w/brd_t("heltec_v3") (spec rung; the
                             // MEASURE-yours duty stays until calipers upgrade it)
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)

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
sma_d = 6.8;         // 1/4-36 SMA thread is Ø6.35: 6.8 with the D-flat at 5.7 (was 6.5/5.9 — 0.075 a side, under FDM hole shrink)

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
tol_slide = 0.20;  tol_press = 0.10;  tol_hole = 0.30;   // the catalog trio — core_tol_*(), canary_core_lib
post_d = 5.0;  screw_d = 1.6;  screw_head_d = 4.0;  screw_head_h = 2.0;
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;  skirt_h = 3.0;  skirt_t = 1.6;
usb_w = 12.0;  usb_h = 6.5;   // service USB opening, bottom wall (plug when deployed); 12 clears a boot
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;   // snap_boardclip defaults — canary_snap_lib runs the strain budget as an assert
standoff_h = 3.0;
lid_edge = 0.8;  lid_edge2 = 0.0;
foot_cham = 0.5;

/* [Aesthetics] */
// Placed by mark_dx/dy/rot/size/depth, gated by the library's measured type
// metrics so an unprintable size is refused before a print, not after.
opt_mark   = false;  // deboss the house wordmark on the lid (canary_mark_lib)
mark_size  = 5.0;    // wordmark cap height
mark_depth = 0.5;    // deboss depth (the lid prints face-down -> crisp first-layer voids)
mark_dx    = 0.0;    // mark center offset from the LID-plate center
mark_dy    = 0.0;    // default sits centered, clear of the gland hole and the vent spot-face
mark_rot   = 0;      // rotation (degrees)

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
e_seal   = opt_seal;
wall_eff = e_seal ? max(wall_t, gasket_w + 1.6) : wall_t;
pd = post_d;
post_corner = pd + 1.5;
clip_stack = clip_clear + clip_t;
// a 2.0 pan seat in a 2.0 lid is a through-hole the head falls through: the
// lid carries a pad under each head and the posts shorten by the same
head_pad = max(0, screw_head_h + 1.0 - lid_t);

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
usb_zc = floor_t + standoff_h + pcb_t + port_usbc_shell_h()/2;   // on the connector AXIS, not PCB-top + h/2

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
assert(!opt_mark || (mark_depth > 0 && mark_depth < lid_t),
       "mark_depth must be between 0 and lid_t");
// the wordmark's two gates, from the mark library's measured type metrics:
// below mark_word_min_h() a 0.4 mm bead no longer reaches the letterforms and
// the deboss prints as a smudge with the rhythm of type — the render looks
// perfect either way, which is why this is an assert and not an eyeball
assert(!opt_mark || mark_size >= mark_word_min_h(),
       str("opt_mark at mark_size ", mark_size, " mm is under the ",
           mark_word_min_h(), " mm cap height where a 0.4 mm bead still ",
           "reaches the letterforms — raise mark_size"));
assert(!opt_mark || mark_word_ink_w("securaCV", mark_size) <= plate_x - 4.0,
       str("the wordmark draws ", mark_word_ink_w("securaCV", mark_size),
           " mm at mark_size ", mark_size, " on a ", plate_x,
           " mm lid (2 mm margin per side) — shrink mark_size"));
echo(str("Canary solar relay pod v0.1-dev — ", out_x, " x ", out_y, " x ", base_d + lid_t,
         " mm, panel ", pan_w, "x", pan_l, " @ ", roof_ang, " deg  (IN DEVELOPMENT)"));

// rrect2d/rrect come from canary_core_lib; only file-specific geometry stays local
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
        offset(r = -w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
    }
}
// Cantilever snap clip on the LoRa board's edge — canary_snap_lib's beam
// (the WAP pattern), so the insertion-strain arithmetic runs as an assert on
// every render: this board sits at standoff_h 3.0, which is exactly the
// short-beam regime where 1.5/0.8-class numbers crack vertical-print PETG.
// The clips stand on the ±X edges, so the wrapper keeps the axis rotation
// and hands the drawing to the library.
module edgeclip(px, py, ang, soff) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, floor_t, floor_t + soff + pcb_t,
                       clip_w, clip_t, clip_hook, clip_hook_h, clip_clear);
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
            // SMA bulkhead, top wall, over the LoRa column. D-FLAT bore: the
            // 1/4-36 thread is Ø6.35, and nut torque on a plain round bore
            // spins the jack and chews the print. Fit an EPDM sealing washer
            // under the external nut — this is the sky-facing wall.
            translate([lb_cx, out_y/2, floor_t + cav_d/2])
                rotate([-90, 0, 0]) translate([0, 0, -wall_eff*2])
                    linear_extrude(wall_eff*4) intersection() {
                        circle(d = sma_d);
                        translate([-sma_d/2, -sma_d/2]) square([5.7 + (sma_d - 5.7)/2, sma_d]);   // D-flat: 5.7 across the flat
                    }
            // service USB, bottom wall (silicone plug when deployed):
            // 45°-chamfered top corners halve the unsupported bridge in the
            // upright-printed wall and keep any droop out of the plug envelope
            // — canary_port_lib (the WAP's print-validated profile; this wall
            // used to bridge a flat top)
            translate([lb_cx, -out_y/2 + wall_eff*1.5, usb_zc])
                rotate([90, 0, 0]) linear_extrude(wall_eff*3)
                    port_bridge_profile2d(usb_w, usb_h);
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
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - head_pad);
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
                // the plate with the catalog's two-stage soft edge — canary_core_lib
                // (this also wires up the previously inert lid_edge2 knob)
                soft_edge_plate(plate_x, plate_y, plate_r, lid_t, lid_edge, lid_edge2);
                if (head_pad > 0) for (p = post_xy())   // the 1.0 mm floor under each pan head
                // pan-head pads: the floor under each head the front cannot
                // spare — CROPPED to the cavity (canary_core_lib). Drawn as a
                // bare cylinder the pad overhung its post and landed on the
                // shell wall rim, holding the front proud so the lip never
                // entered the cavity and the screws clamped nothing.
                    cb_head_pad(p[0], p[1], head_pad,
                                cb_pad_d(screw_head_d, tol_hole),
                                inner_x, inner_y, core_cav_r(corner_r, wall_eff),
                                screw_head_d + 2*tol_hole);
            }
            // flat counterbores: the BOM's PAN-head screws seat flush on the pad's
            // floor — the canary_core_lib seat, per the Vision's lesson
            for (p = post_xy()) translate([0, 0, -head_pad])
                cb_flat_cut(p[0], p[1], lid_t + head_pad, screw_d + 2*tol_hole,
                            screw_head_d + 2*tol_hole, screw_head_h);
            // panel-lead gland hole (fit an M8 cable gland or silicone-seal)
            translate([bh_cx, inner_y/2 - 8, -1]) cylinder(d = 8.2, h = lid_t + 2);
            // pressure vent: Ø3 hole + inner spot-face for an adhesive ePTFE
            // patch (Ø10) — the most thermally-cycled design in the folder
            // pumps ~14 % of its volume past the gasket per day/night cycle
            // without a membrane (field_ratings.md rule). Roof-shaded face.
            translate([lb_cx, -inner_y/4, -1]) cylinder(d = 3.0, h = lid_t + 2);
            translate([lb_cx, -inner_y/4, lid_t - 0.9]) cylinder(d = 5.4, h = 1.0);
            // the house wordmark (opt_mark), debossed on the show face by the
            // first-layer machinery — canary_mark_lib owns the word and its
            // metrics, this file only places it (mark_dx/dy/rot/size/depth)
            if (opt_mark)
                translate([mark_dx, mark_dy, lid_t - mark_depth])
                    linear_extrude(mark_depth + 1) rotate(mark_rot)
                        mark_wordmark(mark_size);
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

// solar roof: an AWNING — the panel bed roots at the pod's top wall and
// descends outward over the lid at roof_ang from horizontal, so the panel
// faces UP and OUT (+Y is up the pole, +Z is away from it: the panel's normal
// is (0, cos, sin)). The v0.1 transform pointed it 60° BELOW horizontal — the
// pod's reason to exist faced the ground. The bed's far (lower) end carries
// the stop the panel rests against under gravity; the panel slides in from
// the root end before the roof goes on. Screws to the lid's top posts (swap
// the two TOP lid screws for M2 x 12 through the roof feet).
module roof() {
    bed_l = pan_l * 0.75;
    difference() {
        union() {
            // two feet matching the top lid-screw posts
            for (p = [post_xy()[0], post_xy()[1]])
                translate([p[0], p[1], 0]) cylinder(d = 8, h = 3);
            translate([0, inner_y/2 - 2, 0]) rotate([90 + roof_ang, 0, 0]) mirror([0, 0, 1]) translate([0, 0, -2]) {
                translate([-pan_w/2 - 3, 0, 0]) cube([pan_w + 6, bed_l, 2]);                // bed
                for (s = [1, -1]) translate([s*(pan_w/2 + tol_slide) + (s < 0 ? -3 : 0), 0, 2 - 0.01])
                    cube([3, bed_l, pan_t + 2]);                                              // side rails
                for (s = [1, -1]) translate([s*(pan_w/2 - 1.2) + (s < 0 ? -1.8 : 0), 0, 2 + pan_t + 0.2])
                    cube([1.8, bed_l, 1.4]);   // retaining lips: 1.2 mm OVER the panel edge, rooted in the rails
                // stop at the FAR (lower, outboard) end: gravity pulls the panel onto it
                translate([-pan_w/2 - 3, bed_l - 3, 2 - 0.01]) cube([pan_w + 6, 3, pan_t + 2.01]);
            }
            // struts from the feet up INTO the bed's underside, 12 mm along it
            // from the root (bed point (0, 12, 0) -> world (0, inner_y/2 - 2 - 12 cos(a), 12 sin(a)))
            for (p = [post_xy()[0], post_xy()[1]])
                hull() {
                    translate([p[0], p[1], 0]) cylinder(d = 8, h = 3);
                    translate([p[0]*0.8, inner_y/2 - 2 - 12*sin(roof_ang), 12*cos(roof_ang) - 2.5]) cylinder(d = 8, h = 3);
                }
        }
        for (p = [post_xy()[0], post_xy()[1]]) {
            translate([p[0], p[1], -0.1]) cylinder(d = screw_d + 2*tol_hole, h = 30);
            translate([p[0], p[1], 3 - 1.4]) cylinder(d = screw_head_d + 0.6, h = 30);
        }
        // crop everything below the foot plane: the tilted lower stop otherwise
        // protrudes ~2.7 mm below z=0 and digs into the lid on assembly
        // (crop top sits 0.01 below the feet so no coincident faces)
        translate([-100, -100, -50.01]) cube([200, 200, 50]);
    }
}

module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5); }

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
