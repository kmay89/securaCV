// ============================================================================
//  Canary — COVERT JUNCTION-BOX SHELL  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Sometimes a witness should look like street furniture. This shell holds
//  the compact WAP stack (XIAO ESP32-S3; the Sense camera by default,
//  opt_camera) inside a housing styled as a
//  utility junction box: squared body, fake conduit bosses on the sides, and
//  the camera aperture hidden inside a mock "unpunched knockout" ring on the
//  lid. Utilitarian gray ASA/PETG completes the disguise.
//
//  NOTE: this is a legitimate-use disguise for a device you own on property
//  you monitor lawfully — pair with the witness signage plate where notice
//  is required.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//
//  v0.1-dev (2026-08-23): canary_*_lib adoption (dedup, mesh-identical);
//  opt_mark knob added — the mark rides the lid's INTERIOR face, default off.
//  2026-09-24: print and drain fixes — the conduit bosses carry a 45° keel
//  (their lower quarter was an overhang the old note said to support); the
//  USB slot is the bridge-safe chamfered profile (canary_port_lib); the lid
//  seats are the house 90° cone (cs_cone90_cut) at the M2 flat head's height;
//  posts stand 0.2 INTO the walls, the corner behind them filled, webbed by
//  corner_gusset; opt_weep drains the -X (cable, down) wall.
//  2026-10-23: audit fixes, each probed in the assembled pose —
//   - THE LID COULD NOT CLOSE ON THE DEFAULT BUILD. opt_camera is the XIAO
//     Sense, whose lens stands brd_xiao_sense_cam_h() 12.7 over the PCB; the
//     cavity gave it 9.0 (stack_h 8.0 + 1.0), so an Ø8 barrel stood 108 mm3
//     into the seated lid, through all 2.4 of it, and the aperture sat over
//     the board center, 6.98 off the lens (cam_dx/dy were 0 with a MEASURE
//     note the registry has since answered). stack_h is 12.2 (the lid's inner
//     face 0.5 over the lens top, asserted from the registry), the aperture
//     and its knockout ring sit on the lens, and the conduit bosses' keels
//     reach the bed on the taller body (centered at base_h/2 their fixed
//     r*sqrt(2) point would float 1.56 over it).
//   - THE PIGTAIL'S OWN PLUG DID NOT FIT. The receptacle's mouth is 7.1 inside
//     the -X wall, shorter than any straight Type-C overmold, so the overmold
//     of whatever is plugged in stands in the slot — and the 10.5 slot's 2.5
//     chamfers took 40.2 mm3 out of a spec-max (12.35 x 6.5) one, and the
//     corners of a slim 8.4 x 4.5. The slot opens to the spec overmold plus
//     tol_slide a side and keeps its full width over the overmold's whole
//     height, the bridge chamfers above it.
//   - NOTHING LOCATED THE BOARD ALONG X (the WAP's AUD-2, never carried
//     here): the clips hold Y and Z, so a plug slid it along its rails — off
//     their 3 mm ends past 3, into the wall at 8.5. The WAP's end stops stand
//     0.3 off the board's ends, outside the overmold's envelope.
//   - THE BOX COULD NOT BE MOUNTED: the HARDWARE line billed 2 #8 wall
//     screws and no hole took them. Two now run through the floor under the
//     board (driven before it clips in), pan heads in counterbores that keep
//     the house 1.0 floor.
// ============================================================================

use <canary_core_lib.scad>   // rrect/rrect2d — the catalog's shared helpers
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_port_lib.scad>   // connector standards — the pigtail slot centers on the USB-C shell axis
use <canary_board_lib.scad>  // board registry — the XIAO numbers the knobs cite
use <canary_mark_lib.scad>   // the house wordmark (opt_mark)
use <canary_rib_lib.scad>    // corner_gusset — the constant-width post web

/* [What to render] */
part = "all";        // ["body","lid","all"]

/* [Options] */
opt_camera = true;   // aperture hidden in the knockout ring (XIAO Sense)
opt_led    = false;  // pinhole light pipe (covert: usually off)
opt_weep   = true;   // Ø2 drain through the -X wall at the floor (the cable wall hangs down), beside the USB slot
// A covert box stays street-plain outside, so the mark sits where only the
// installer sees it.
opt_mark   = false;  // deboss the house wordmark on the lid's INTERIOR face (covert: never outside)

/* [Board] — XIAO ESP32-S3 (Sense) */
board_l = 21.0;      // board length — the 21 spec, brd_l("xiao"), canary_board_lib
board_w = 17.5;      // board width — the 17.5 spec, brd_w("xiao"); a real board mics 17.8
                     // (brd_xiao_w_measured()) and the clips' clip_clear absorbs the
                     // difference, so the spec default stands
board_h = 1.2;       // PCB thickness — the 1.2 spec, brd_t("xiao"); the cavity, the clip hooks and the pigtail slot all stack on it
board_clear = 0.6;   // clearance each side of the board — the cavity is the board plus 2 x this plus 2 x inner_pad
stack_h = 12.2;      // height of the stack above the PCB (the Sense camera board) — the cavity is standoff_h + board_h + this + 1.0 deep; 12.2 puts the lid's inner face 0.5 over the Sense lens top (brd_xiao_sense_cam_h() 12.7), asserted
standoff_h = 3.5;    // 3.5: the clip beam (standoff + PCB) keeps the measured
                     // 17.8 board under the 4.5 % strain budget (3.0 ran 5.5 %)

/* [Shell] */
wall_t = 2.2;        // deviates: service-box duty build — chunkier walls to match its squared corners
floor_t = 2.2;       // floor thickness — the service-box duty build, chunky like wall_t
lid_t = 2.4;         // lid thickness — the service-box duty build, chunky like wall_t
inner_pad = 8.0;     // interior margin around the board (wiring room)
corner_r = 2.0;      // deviates: squared, utilitarian — the service box reads as gear, not decor
lip_h = 3.0;  lip_t = 1.2;
floor_cove = 0.8;  // 45° cove where the floor meets the walls, inside (canary_core_lib cavity_cut); 0 = the old square corner  // [0:0.2:1.2]
                   // The sharp notch there was the crack-starter in every flat-printed shell — a corner drop
                   // hinges the floor about it along one layer boundary.
lid_key    = true; // poka-yoke: a rib on the +Y cavity wall and a slot in the lid's lip — four corner posts fit
                   // a lid two ways and every lid feature lines up one way; turned round it stands lip_h proud
boss_d = 12.0;       // fake conduit boss diameter (fits the body height; a true 1/2" boss needs a taller shell)
boss_l = 6.0;        // boss protrusion
screw_d = 1.6;  screw_head_d = 4.0;  screw_head_h = 1.2;
post_d = 5.0;        // corner post diameter — four lid-screw posts standing 0.2 into both walls, each piloted screw_d from 2 mm above the floor
pigtail_w = 12.8;    // pigtail pass-through slot width, low on the -X wall: the connector sits 7.1 inside it, so the pigtail's plug overmold stands in the slot — the Type-C spec max 12.35 + 2 x tol_slide; the weep sits beside it and the lid lip is notched over it
pigtail_h = 6.9;     // pigtail pass-through slot height at full width, centered on the USB-C connector axis — the spec max overmold 6.5 + 2 x tol_slide; the 45° bridge chamfers stand above it (canary_port_lib)

/* [Camera knockout] */
cam_ap_d   = 9.0;    // camera aperture diameter through the lid at cam_dx/cam_dy (opt_camera)
cam_dx     = -6.95;  // aperture offset from the board center along X, + = away from the USB end: the Sense camera sits over the USB end — brd_xiao_sense_cam_dx(), canary_board_lib
cam_dy     = -0.64;  // aperture offset from the board center across the width — brd_xiao_sense_cam_dy(), canary_board_lib
ko_ring_d  = 22.0;   // mock knockout ring, centered on the aperture (aperture recessed inside it)
ko_depth   = 0.8;    // depth of the mock knockout's groove in the lid's outer face (the groove is 0.8 wide, just outside ko_ring_d)

/* [Print tolerances] */
tol_slide = 0.20;  tol_hole = 0.30;

/* [Board snap clips] */
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
inner_l = board_l + 2*board_clear + 2*inner_pad;
inner_w = board_w + 2*board_clear + 2*inner_pad;
cav_h   = standoff_h + board_h + stack_h + 1.0;
out_l = inner_l + 2*wall_t;
out_w = inner_w + 2*wall_t;
base_h = floor_t + cav_h;
pd = post_d;
// the hardware, DERIVED from the same knobs that draw the holes (canary_core_lib)
hw_echo("Covert j-box", [
    hw_item(len(post_xy()), hw_screw("m2", "flat", hw_len(lid_t, 0, 6), "self-tap")),   // the lid seat is a cone: flat head
    hw_item(2, "#8 pan wall screw (box mount)"),
]);
echo(str("Canary covert j-box v0.1-dev — ", out_l, " x ", out_w, " x ", base_h + lid_t,
         " mm  (IN DEVELOPMENT)"));
// the mark library's measured type metrics gate the interior wordmark the same
// way they gate an exterior one — a smudge inside the box is still a smudge
assert(!opt_mark || mark_word_ink_w("securaCV", 4.0) <= inner_l - 6,
       str("opt_mark: the wordmark draws ", mark_word_ink_w("securaCV", 4.0),
           " mm wide — more than the lid interior offers; shrink the cap height"));

// (rrect2d/rrect come from canary_core_lib — the local copies are gone)
// corner posts stand 0.2 INTO both walls (the catalog's post seat); at -0.2
// they stood free of both, touching nothing but the floor
function post_xy() = [
    [ inner_l/2 - pd/2 + 0.2,  inner_w/2 - pd/2 + 0.2],
    [-inner_l/2 + pd/2 - 0.2,  inner_w/2 - pd/2 + 0.2],
    [ inner_l/2 - pd/2 + 0.2, -inner_w/2 + pd/2 - 0.2],
    [-inner_l/2 + pd/2 - 0.2, -inner_w/2 + pd/2 - 0.2],
];
// the lid seat is the house 90° cone for a flat head: screw_head_h deep, it
// opens to the head plus the hole tolerance
assert(abs((screw_d + 2*tol_hole) + 2*screw_head_h - (screw_head_d + 2*tol_hole)) < 1e-6,
       "screw_head_h: the 90° seat does not open to the flat head's diameter");
assert(lid_t - screw_head_h >= 1.0, "the lid seat leaves under 1.0 mm under the head");
usb_zc = floor_t + standoff_h + board_h + port_usbc_shell_h()/2;
// the board's end stops stand outboard of the Type-C overmold across the board
stop_y0 = port_usbc_overmold_w()/2 + 0.2;
assert(board_w/2 - stop_y0 >= core_min_wall(), "the board's end stops would be thinner than a wall across the board");
// the drain: -X wall at the floor, beside the pigtail slot
weep_y = pigtail_w/2 + weep_d()/2 + 1.5;
assert(!opt_weep || weep_y + weep_d()/2 + 0.5 <= inner_w/2 - pd, "the weep runs into a corner post");
// the Sense lens under the lid: the cavity's top is the lid's inner face, and
// the lens top must stand under it — 0.2 is the least a print's height error
// leaves (the WAP's floor for the same lens); 8.0 left it 3.7 INTO the lid
lens_gap = stack_h + 1.0 - brd_xiao_sense_cam_h();
assert(!opt_camera || lens_gap >= 0.2 - 1e-9,
       str("the Sense lens top stands ", -lens_gap, " mm over the lid's inner face — raise stack_h to at least ",
           brd_xiao_sense_cam_h() - 1.0 + 0.2));
// the pigtail slot passes a spec-max Type-C overmold across its whole height
assert(pigtail_w >= port_usbc_overmold_w() && pigtail_h >= port_usbc_overmold_h(),
       "the pigtail slot is smaller than the Type-C spec overmold that stands in it");
// two #8 wall screws through the floor under the board (2026-10-23): pan
// heads in counterbores that keep the house 1.0 floor (DESIGN_RULES §4)
mount_d  = 4.17 + 2*tol_hole;                     // #8 clearance: the 4.17 major + 2 x tol_hole (4.77)
mount_hd = 7.9 + 2*tol_hole;                      // the #8 pan head's 7.9 + 2 x tol_hole (8.5)
mount_cb = floor_t - 1.0;                         // counterbore depth: the house 1.0 floor stays under the head
mount_y  = mount_hd/2 + 0.25;                     // ± across the board's centerline: the two seats 0.5 apart
assert(mount_y - mount_hd/2 >= 0.2, "the two wall-screw counterbores run into each other");
assert(mount_y + mount_hd/2 <= board_w/2 + clip_clear - 0.2, "a wall-screw counterbore runs under a board clip's root");
// Cantilever snap clip on a board long edge — the compact-WAP idiom, so the
// drawing now comes from canary_snap_lib like the WAP's own: the insertion-
// strain arithmetic runs as an assert on every render instead of trusting a
// copied number. `cx` = position along the edge; `sy` = +1/-1 picks the edge.
module boardclip(cx, sy) {
    snap_boardclip(cx, sy * board_w/2, sy,
                   floor_t, floor_t + standoff_h + board_h,
                   clip_w, clip_t, clip_hook, clip_hook_h, clip_clear,
                   over = (brd_xiao_w_measured() - board_w)/2);
}

module body() {
    union() {
        difference() {
            union() {
                rrect(out_l, out_w, corner_r, base_h);
                // fake conduit bosses: two per long wall, half-cylinders lying on
                // the wall, CENTERED on the body height so they stay inside the
                // print envelope (a Ø21 boss poked 1.7 mm below the bed plane).
                // A horizontal cylinder's lower quarter is an overhang, so each
                // carries a 45° keel down to the bed (a teardrop, point down,
                // cropped at z = 0): it prints unsupported and reads as the
                // cast web under a real conduit hub. The point drops at least
                // 0.55 past the bed (2026-10-23): the camera's taller body put
                // base_h/2 over r*sqrt(2), and the keel floated 1.56 off the bed
                for (sy = [1, -1], i = [-1, 1])
                    translate([i*out_l/4, sy*(out_w/2 - 0.01), base_h/2])
                        mirror([0, sy > 0 ? 1 : 0, 0]) rotate([90, 0, 0]) linear_extrude(boss_l)
                            intersection() {
                                hull() {
                                    circle(d = boss_d);
                                    translate([0, -max(boss_d/2*sqrt(2), base_h/2 + 0.55)]) square(0.01, center = true);
                                }
                                translate([-boss_d, -base_h/2]) square([2*boss_d, boss_d + base_h]);
                            }
            }
            translate([0, 0, floor_t])   // the cavity, floor cove left standing (canary_core_lib)
                cavity_cut(inner_l, inner_w, max(0.1, corner_r - wall_t), cav_h + 1, floor_cove);
            // the pigtail pass-through slot, low on the -X short wall: route a pigtail
            // through it and dress the cable as conduit. Sheltered mounting only -
            // the slot is open.
            // centered on the connector AXIS (shell/2 above the PCB), not PCB-top + h/2:
            // the pigtail's lower half landed in the wall
            // 45°-chamfered top corners keep the upright wall's bridge to the
            // print-validated span (canary_port_lib; the slot bridged 10.5 flat).
            // The receptacle's mouth is 7.1 inside the wall, so the pigtail plug's
            // overmold stands in the slot (2026-10-23): the full width spans the
            // overmold's whole height, usb_zc ± pigtail_h/2, and the chamfers sit
            // above it — centered, they cut 40.2 mm3 out of a spec-max overmold
            translate([-out_l/2, 0, usb_zc + port_bridge_cham_for(pigtail_w)/2]) rotate([90, 0, 90])
                linear_extrude(wall_t*3, center = true)
                    port_bridge_profile2d(pigtail_w, pigtail_h + port_bridge_cham_for(pigtail_w));
            // drain at the low point: the -X wall's floor corner, angled down
            // and out (canary_core_lib weep_cut)
            if (opt_weep)
                weep_cut(-inner_l/2, weep_y, floor_t + weep_d()/2 + 0.2, "-x", wall_t, weep_d());
            // the two #8 wall screws the HARDWARE line bills (2026-10-23: no hole
            // took them): through the floor under the board, driven before it
            // clips in, pan heads in flat counterbores (canary_core_lib)
            for (sy = [1, -1]) cb_flat_cut(0, sy*mount_y, floor_t, mount_d, mount_hd, mount_cb);
        }
        // lid key (canary_core_lib): a rib on the +Y wall, centered, inside the lip zone
        if (lid_key) lid_key_rib(0, inner_w/2, 270, base_h, lip_h);
        // posts + board clips (compact-WAP idiom)
        difference() {
            union() {
                for (p = post_xy()) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_h);
                // the corner behind each post filled solid into both walls (a
                // round post in a square corner leaves a closed sliver there)
                for (p = post_xy()) let (sx = sign(p[0]), sy = sign(p[1]))
                    translate([min(p[0], sx*(inner_l/2 + 0.3)), min(p[1], sy*(inner_w/2 + 0.3)), floor_t])
                        cube([abs(sx*(inner_l/2 + 0.3) - p[0]), abs(sy*(inner_w/2 + 0.3) - p[1]), cav_h]);
                // constant-width webs into both walls (canary_rib_lib corner_gusset)
                for (p = post_xy()) translate([0, 0, floor_t]) {
                    sx = sign(p[0]); sy = sign(p[1]);
                    gw = min(2.0, rib_t_max(wall_t));
                    corner_gusset(p[0], p[1], sx*(inner_l/2 + 0.5), p[1], cav_h - lip_h - 1, wall_t, pd, gw);
                    corner_gusset(p[0], p[1], p[0], sy*(inner_w/2 + 0.5), cav_h - lip_h - 1, wall_t, pd, gw);
                }
            }
            for (p = post_xy()) translate([p[0], p[1], floor_t + 2]) cylinder(d = screw_d, h = cav_h);
        }
        // support rails under the board's short edges (clips alone don't set the
        // PCB height — review catch); board rests at standoff_h, clips retain it
        for (s = [1, -1])
            translate([s*(board_l/2 - 1.5) - 1.5, -(board_w - 1)/2, floor_t - 0.01])
                cube([3, board_w - 1, standoff_h + 0.01]);
        for (s = [1, -1]) for (cx = [-board_l/4, board_l/4]) boardclip(cx, s);
        // END STOPS along X (2026-10-23 — the WAP's AUD-2 board_stops, carried):
        // the clips hold Y and Z only, so a plug slid the board 8.5 mm along its
        // rails. A core_min_wall() column off each end of each long edge, 0.3 off
        // the drawn ends (a real board mics 21.14: 0.23 a side), up to the PCB's
        // top face, outside a spec-max Type-C overmold across the connector axis
        for (sx = [1, -1], sy = [1, -1])
            translate([sx*(board_l/2 + 0.3) - (sx > 0 ? 0 : core_min_wall()),
                       sy > 0 ? stop_y0 : -board_w/2, floor_t - 0.01])
                cube([core_min_wall(), board_w/2 - stop_y0, standoff_h + board_h + 0.01]);
    }
}

module lid() {
    union() {
        difference() {
            rrect(out_l, out_w, corner_r, lid_t);
            // mock knockout: shallow circular groove ring; aperture recessed inside it,
            // so the ring rides on the aperture (2026-10-23: the aperture moved onto
            // the lens, 6.98 off center, and would have cut the ring)
            translate([cam_dx, cam_dy, lid_t - ko_depth]) difference() {
                cylinder(d = ko_ring_d + 1.6, h = ko_depth + 0.1);
                translate([0, 0, -0.1]) cylinder(d = ko_ring_d, h = ko_depth + 0.3);
            }
            if (opt_camera) translate([cam_dx, cam_dy, -1]) cylinder(d = cam_ap_d, h = lid_t + 2);
            if (opt_led) translate([out_l/4, 0, -1]) cylinder(d = 1.8, h = lid_t + 2);
            // the house wordmark (opt_mark), debossed into the INTERIOR face (z=0):
            // the disguise forbids exterior branding, but a marked interior still
            // identifies the device to whoever opens it. 0.5 deep at cap 4.0 —
            // above mark_word_min_h(), clear of the aperture (y), the post
            // counterbores (y) and the lip ring (which lives below z=0).
            if (opt_mark) translate([0, -9, -0.1])
                linear_extrude(0.6) mark_wordmark(4.0);
            // flat-head seats: the house 90° cone (canary_core_lib) — the old
            // cone opened to the bare head Ø over 1.6, a ~64° seat a 90° head
            // bears on by its rim
            for (p = post_xy()) cs_cone90_cut(p[0], p[1], lid_t, screw_d + 2*tol_hole, screw_head_h);
        }
        difference() {   // lip
            lip_ring(inner_l - 2*tol_slide, inner_w - 2*tol_slide, 0.5, lip_h, lip_t);   // lead-in on the tip (canary_core_lib)
            for (p = post_xy()) translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
            if (lid_key) lid_key_slot(0, inner_w/2, 270, lip_h, lip_t);
            translate([-inner_l/2, 0, -lip_h/2]) cube([lip_t*4, pigtail_w + 4, lip_h + 0.2], center = true);
        }
    }
}

if      (part == "body") body();
else if (part == "lid")  translate([0, 0, lid_t]) rotate([180, 0, 0]) lid();
else { body(); translate([out_l + 16, 0, lid_t]) rotate([180, 0, 0]) lid(); }
