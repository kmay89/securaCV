// ============================================================================
//  Canary — SOLAR LoRa/MESHTASTIC RELAY POD  ⚠️ IN DEVELOPMENT (v0.2-dev)
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
//  2026-09-08: DRAINAGE (v0.2-dev) — the audit found a pole pod with no drain:
//              its only floor opening was the USB the README says to plug.
//              Now: opt_weep drains (Ø2, angled down through the bottom wall —
//              one in the LoRa column beside the rails, one under the 18650
//              bay); the ePTFE vent's spot-face is on the INNER face (it was
//              cut on the show face — a 0.9 mm cup on the membrane); a raised
//              sealing-washer land around the SMA on the sky wall (sma_boss)
//              so the thread stands above the water film; the roof's panel
//              bed is as long as the panel (bed_l = pan_l + stop — at 75 % of
//              pan_l a 110 mm panel overhung the root by 27 mm, into the top
//              wall and the antenna), its stop sits below the panel's top
//              face so water sheds over it, three drain notches at bed level
//              through the stop, and rib_lib ribs under the bed.
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d, soft-edge lid, screw seats — the shared idiom
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_port_lib.scad>   // bridge-safe USB opening (the WAP's print-validated profile)
use <canary_board_lib.scad>  // board registry — the LoRa knob defaults cite
                             // brd_l/brd_w/brd_t("heltec_v3") (spec rung; the
                             // MEASURE-yours duty stays until calipers upgrade it)
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)
use <canary_rib_lib.scad>    // plate_ribs under the roof's panel bed

/* [What to render] */
part = "all";        // ["body","lid","roof","gasket","all"]

/* [Options] */
opt_seal = true;
opt_weep = true;         // Ø2 drains, angled down through the BOTTOM wall (the pod hangs USB-down on
                         // its pole): condensate leaves the floor corners instead of pooling under the
                         // LoRa board and the 18650. The pressure path is the lid's ePTFE vent.
sma_boss = true;         // raised Ø(sma_d + 6) x 1.0 land around the SMA on the sky wall: the EPDM
                         // sealing washer seats on it above the water film, not in it
roof_ribs = true;        // rib_lib loop + two spines under the panel bed (a 76 x 114 x 2 plate on two struts)
seal_mid_posts = true;   // one extra lid screw mid-way along each long (±X) wall: four corner screws
                         // cannot hold 20 % gasket squeeze across 84 mm of 2 mm lid

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
roof_stop_t = 3.0;   // the stop at the bed's low end the panel rests on under gravity
roof_drain_w = 6.0;  // three drain notches at bed level through that stop (0 = none)  // [0:1:12]

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
lid_edge  = 0.8;  // first (45°) stage of the show-face edge, mm — core_face_edge()  // [0:0.1:1.5]
lid_edge2 = 0.8;  // second (~66°) stage of the show-face edge, mm — ON is the house look (core_face_edge2()); it is what reads as a roundover instead of a bevel. 0 leaves the plain 45° facet any CAD default gives you  // [0:0.1:1.5]
foot_cham = 0.5;
floor_cove = 0.8;  // 45° cove where the floor meets the walls, inside (canary_core_lib cavity_cut): the sharp
                   // notch there was the crack-starter in every flat-printed shell — a corner drop hinges the
                   // floor about it along one layer boundary. 0 = the old square corner  // [0:0.2:1.2]
lid_key    = true; // poka-yoke: a rib on the +Y cavity wall and a slot in the lid's lip — four corner posts fit
                   // a lid two ways and every lid feature lines up one way; turned round it stands lip_h proud

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
// drains: one in the gap between the LoRa rail and the battery bay wall, one
// under the 18650 bay (its ring wall is a dam: the bay is a cup without it)
weep_x_gap = lb_cx + lb_w/2 + 0.75 + weep_d()/2;
function weep_xs() = [weep_x_gap, bh_cx];
assert(!opt_weep || weep_x_gap + weep_d()/2 + 0.5 <= bh_cx - (bh_w + 3)/2,
       "the gap weep runs into the battery bay wall — widen post_corner or move it");
assert(!opt_weep || weep_x_gap - weep_d()/2 >= lb_cx + usb_w/2 + 1.0,
       "the gap weep merges with the USB opening");

skirt_gap = tol_slide + 0.2;
plate_x = e_seal ? out_x + 2*(skirt_gap + skirt_t) : out_x;
plate_y = e_seal ? out_y + 2*(skirt_gap + skirt_t) : out_y;
plate_r = e_seal ? corner_r + skirt_gap + skirt_t : corner_r;

function post_xy() = concat([
    [ inner_x/2 - pd/2 - 0.2,  inner_y/2 - pd/2 - 0.2],
    [-inner_x/2 + pd/2 + 0.2,  inner_y/2 - pd/2 - 0.2],
    [ inner_x/2 - pd/2 - 0.2, -inner_y/2 + pd/2 + 0.2],
    [-inner_x/2 + pd/2 + 0.2, -inner_y/2 + pd/2 + 0.2],
], (e_seal && seal_mid_posts) ? [[inner_x/2 - pd/2 - 0.2, 0], [-inner_x/2 + pd/2 + 0.2, 0]] : []);
// the mid posts stand outboard of the LoRa rails and the battery bay walls
assert(!(e_seal && seal_mid_posts) || (inner_x/2 - pd + 0.2 > bh_cx + (bh_w + 3)/2 + 0.5
                                       && -(inner_x/2 - pd + 0.2) < lb_cx - lb_w/2 - clip_stack - 0.5),
       "a mid-span post lands on a cradle — widen post_corner");
assert(lip_h < cav_d, "lip_h vs cavity");
key_x = inner_x/2 - post_corner - 2.5;   // lid key: +Y wall, inboard of the +X corner post (battery side)
assert(roof_drain_w == 0 || 3*roof_drain_w + 12 <= pan_w, "roof_drain_w: three notches do not fit across the stop");
assert(!sma_boss || sma_d + 6 <= cav_d, "the SMA washer land is taller than the sky wall — shrink it");
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
echo(str("Canary solar relay pod v0.2-dev — ", out_x, " x ", out_y, " x ", base_d + lid_t,
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
    // the drains are cut LAST, from the finished body: the 18650 bay's ring
    // wall lands on the bottom wall's inner face after the shell is cut, and a
    // weep subtracted before it was plugged by 1.2 mm of that ring (review
    // catch) — the bay stayed the cup the second drain exists to empty
    difference() {
        body_solid();
        // drains at the floor corners of the BOTTOM wall, angled down and out
        // (canary_core_lib weep_cut) — the wall the pod hangs on
        if (opt_weep) for (x = weep_xs())
            weep_cut(x, -inner_y/2, floor_t + weep_d()/2 + 0.2, "-y", wall_eff, weep_d());
    }
}
module body_solid() {
    posts = post_xy();
    union() {
        difference() {
            union() {
                rrect(out_x, out_y, corner_r, base_d);
                // thickened back hosts the strap channels WITHOUT breaching the floor
                translate([0, 0, -strap_t]) rrect(out_x, out_y, corner_r, strap_t + 0.01);
                // SMA sealing-washer land, 1.0 proud of the sky wall: a bulkhead
                // nut torqued onto a flat wall seats its washer IN the film of
                // water that wall carries; on a land it seats above it
                if (sma_boss)
                    translate([lb_cx, out_y/2 - 0.01, floor_t + cav_d/2]) rotate([-90, 0, 0])
                        cylinder(d = sma_d + 6, h = 1.01);
            }
            translate([0, 0, floor_t])   // the cavity, floor cove left standing (canary_core_lib)
                cavity_cut(inner_x, inner_y, max(0.1, corner_r - wall_eff), cav_d + 1, floor_cove);
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
            // bottom-edge chamfer — the LIBRARY's ring (canary_core_lib), not
            // a local re-draw of it. This was a hand copy of foot_chamfer_ring
            // in a file that already imports the library, structurally
            // identical and differing only in the cutter's outer envelope
            // (+0.1 here, +0.04 there), which lands outside the part either
            // way. So it rendered the SAME mesh — which is exactly what made
            // it dangerous: a fork that currently agrees is one nothing will
            // notice when the library's ring changes and this copy does not.
            if (foot_cham > 0)
                foot_chamfer_ring(out_x, out_y, corner_r, foot_cham, -strap_t);
        }
        // lid key (canary_core_lib): a rib on the +Y (sky) wall inside the lip zone,
        // on the battery side — the SMA lives on the LoRa side of that wall
        if (lid_key) lid_key_rib(key_x, inner_y/2, 270, base_d, lip_h);
        // posts + gussets
        difference() {
            union() {
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - head_pad);
                for (p = posts) {
                    sx = sign(p[0]); sy = sign(p[1]);
                    hull() { translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - lip_h - 1);
                             translate([sx*(inner_x/2 - 0.3), p[1], floor_t]) cylinder(d = 2, h = cav_d - lip_h - 1); }
                    if (sy != 0) hull() { translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d - lip_h - 1);
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
            // the spot-face is on the INNER face (z=0 side): the patch is bonded
            // inside, the bore faces the weather. Cut on the show face (as it
            // was) it left a 0.9 mm cup pooling water ON the membrane.
            translate([lb_cx, -inner_y/4, -1]) cylinder(d = 3.0, h = lid_t + 2);
            translate([lb_cx, -inner_y/4, -0.1]) cylinder(d = 10.6, h = 0.6 + 0.1);
            // the house wordmark (opt_mark), debossed on the show face by the
            // first-layer machinery — canary_mark_lib owns the word and its
            // metrics, this file only places it (mark_dx/dy/rot/size/depth)
            if (opt_mark)
                translate([mark_dx, mark_dy, lid_t - mark_depth])
                    linear_extrude(mark_depth + 1) rotate(mark_rot)
                        mark_wordmark(mark_size);
        }
        difference() {   // lip
            lip_ring(inner_x - 2*tol_slide, inner_y - 2*tol_slide, max(0.1, corner_r - wall_eff - tol_slide), lip_h, lip_t);
            for (p = post_xy()) translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
            translate([lb_cx, -inner_y/2, -lip_h/2]) cube([usb_w + 4, lip_t*4, lip_h + 0.2], center = true);
            if (lid_key) lid_key_slot(key_x, inner_y/2, 270, lip_h, lip_t);
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
    // the bed is as long as the panel plus its stop: at 0.75 x pan_l the panel
    // overhung the ROOT end, straight into the top wall and the antenna
    bed_l = pan_l + tol_slide + roof_stop_t;
    stop_h = pan_t - 0.4;   // BELOW the panel's top face: water sheds over the stop, not against it
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
                // stop at the FAR (lower, outboard) end: gravity pulls the panel onto
                // it; drain notches at bed level let what runs under the panel out
                difference() {
                    translate([-pan_w/2 - 3, bed_l - roof_stop_t, 2 - 0.01]) cube([pan_w + 6, roof_stop_t, stop_h + 0.01]);
                    if (roof_drain_w > 0) for (x = [-pan_w/3, 0, pan_w/3])
                        translate([x - roof_drain_w/2, bed_l - roof_stop_t - 1, 2 - 0.02]) cube([roof_drain_w, roof_stop_t + 2, 1.2]);
                }
                // stiffening under the bed (rib_lib): a peripheral loop and two
                // spines along the slope, 4 mm on 1.6 — the bed is a 2 mm plate
                // on two struts with a panel and the wind on it
                if (roof_ribs)
                    translate([0, bed_l/2, 0.01]) mirror([0, 0, 1]) plate_ribs(pan_w + 6, bed_l, 2, 4.0, r = 1.0, inset = 2.0, n = 2);
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
