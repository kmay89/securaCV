// ============================================================================
//  SecuraCV Canary Vision — 3D-printable enclosure (parametric)  v0.2
//  Stack: OV5647 camera (Pi-cam v1.3 form) + Grove Vision AI V2 (25 x 25)
//         + a selectable HOST:
//    host = "xiao"   — XIAO ESP32-C3/S3 seated in the module's stacking
//                      socket (recommended; zero wiring — device guide §3).
//                      Compact single-column case; the bottom wall carries
//                      BOTH USB-C ports (module "model port" + XIAO
//                      "firmware port", stacked vertically).
//    host = "devkit" — ESP32-C3-DevKitM-1 beside the module, joined by the
//                      Grove I2C cable (two-column case).
//
//  A wall/eave camera unit: the FRONT face carries the lens aperture (with a
//  recessed clear-disc seat and an optional rain hood) and the LED light pipe;
//  the BACK shell carries the boards and the mounts. Mounting is a
//  GoPro-compatible two-prong hinge on the TOP wall (pitch-adjustable, locked
//  with an M5 thumbscrew) plus a printable wall BRACKET (three prongs,
//  screw/keyhole/tripod-nut fixing) — an upgrade over friction-only fold-out
//  stands: optional radial DETENT TEETH on the mating faces make the set
//  angle sag-proof. Weather mode reuses the Canary WAP sealing system
//  (printed TPU gasket + drip-edge front + USB plug recess).
//
//  Units: millimetres.  CAD: OpenSCAD (https://openscad.org).
//
//  ⚠️ VERIFY BEFORE PRINTING. Board dimensions are nominal (DevKitM-1
//     ~39.0 x 25.4, Vision AI V2 25 x 25, OV5647 carrier 25 x 24 with the
//     Pi-cam v1.3 21 x 12.5 mm M2 hole grid). Measure YOUR boards — DevKit
//     revisions differ, and soldered pin headers need a taller standoff.
//
//  Orientation: model +Y = up on the wall, USB opening on the BOTTOM (-Y)
//  wall, prongs on the TOP wall, +Z = toward the front face.
// ============================================================================

/* [What to render] */
part   = "all";       // ["back","front","all","gasket","bracket","knob"]

/* [Preset] — quick configs; choose "custom" to use the option checkboxes */
preset = "custom";    // ["custom","vision_indoor","vision_weather"]

/* [Options (applied when preset = custom)] */
opt_led    = true;    // status LED        -> light-pipe port on the front
opt_buzzer = false;   // piezo buzzer      -> shares the vent cluster (firmware: unpopulated on Vision)
opt_vent   = false;   // GORE vent cluster -> pressure equalisation (recommended with opt_seal)
opt_tamper = false;   // reed/Hall + magnet -> magnet pocket on the front underside
opt_hood   = false;   // rain/glare hood over the lens window
opt_seal   = false;   // perimeter TPU gasket + drip-edge front + USB plug recess (splash-resistant, NOT immersion)
opt_mount  = true;    // mounting features per mount_style
mount_style = "hinge"; // ["hinge","keyhole","both"]

// effective flags (a preset overrides the checkboxes above)
function _pre(c, i, w) = (preset == "vision_indoor") ? i
                       : (preset == "vision_weather") ? w : c;
e_led    = _pre(opt_led,    true,  true);
e_buzzer = _pre(opt_buzzer, false, false);
e_vent   = _pre(opt_vent,   false, true);
e_tamper = _pre(opt_tamper, false, false);
e_hood   = _pre(opt_hood,   false, true);
e_seal   = _pre(opt_seal,   false, true);
e_mount  = _pre(opt_mount,  true,  true);
m_style  = _pre(mount_style, "hinge", "both");

/* [Boards] — measure YOURS; these are nominal */
host     = "xiao"; // ["xiao","devkit"]  stacked XIAO (recommended) or Grove-cabled DevKitM-1
dk_l     = 39.0;   // ESP32-C3-DevKitM-1 length (Y, USB end down) — devkit host
dk_w     = 25.4;   // DevKitM-1 width (X)
vm_l     = 25.0;   // Grove Vision AI V2 (Y; USB edge down)
vm_w     = 25.0;   // Grove Vision AI V2 (X)
xiao_l   = 21.0;   // stacked XIAO (Y) — xiao host
xiao_w   = 17.5;   // stacked XIAO (X)
stack_sock_h = 11.5; // module underside -> XIAO underside when seated (socket + headers) — MEASURE
vm_front_h   = 5.0;  // module front-side component height (CSI connector etc.)
cam_w    = 25.0;   // OV5647 carrier width (X)
cam_h    = 24.0;   // OV5647 carrier height (Y)
pcb_t    = 1.0;    // PCB thickness (clips hook over this)
board_clear = 0.6; // per-side clearance around each PCB
stack_h  = 9.0;    // tallest top-side component over a PCB (Grove socket / USB boot) — devkit host

/* [Camera mounting] — Pi-cam v1.3 pattern on the FRONT face */
cam_hole_x = 21.0;  // hole grid (X)
cam_hole_y = 12.5;  // hole grid (Y)
cam_post_d = 3.6;   // camera post diameter
cam_post_h = 4.0;   // post height = lens-board standoff from the front face
cam_screw_d = 1.6;  // M2 self-tap pilot in the posts
lens_dx   = 0.0;    // lens centre offset from the camera-board centre — MEASURE
lens_dy   = 2.5;    // (v1.3 lens sits ~2.5 mm above board centre)
cam_ap_d  = 10.0;   // lens aperture in the front face
cam_disc_d = 14.0;  // clear-disc seat diameter (12-16 mm disc; 0 = bare aperture)
cam_disc_t = 1.0;   // clear-disc thickness (disc sits 0.2 recessed)

/* [Shell] */
wall_t   = 2.0;    // side wall thickness (auto-thickened in seal mode)
floor_t  = 2.0;    // back thickness
lid_t    = 2.0;    // front face thickness
lip_h    = 4.0;    // front lip insertion into the back shell
lip_t    = 1.2;    // lip wall thickness
corner_r = 3.0;    // outside corner radius
standoff_h = 3.0;  // PCBs sit this high off the back (RAISE if DevKit has soldered pin headers!)
cav_extra  = 1.0;  // headroom above the tallest component

/* [Print tolerances] — per-side clearances; tune once for your printer */
tol_slide = 0.20;  // sliding fits: front lip, drip skirt, disc seat
tol_press = 0.10;  // press fits: magnet, light pipe
tol_hole  = 0.30;  // clearance holes: front screws, hinge bolt

/* [Screw posts] (front screws into the back shell corners) */
post_d       = 5.0;
screw_d      = 1.6;   // M2 self-tapping pilot
screw_head_d = 4.0;
screw_head_h = 2.0;

/* [USB-C ports] — on the BOTTOM (-Y) wall. devkit host: one opening (DevKit).
   xiao host: TWO stacked openings — module "model port" (upper) + XIAO
   "firmware port" (lower); both face the same edge (device guide §2). */
usb_w  = 10.5;
usb_h  = 6.5;
usb_z  = 0.0;          // extra lift relative to PCB-top centring
usb_dx = 0.0;          // upper/main port offset along the wall — MEASURE your boards
xiao_usb_dx   = 0.0;   // XIAO port offset along the wall
xiao_usb_drop = 10.0;  // XIAO port centre BELOW the module port centre — MEASURE the seated stack

/* [Hinge — GoPro-compatible two prongs on the TOP wall] */
prong_t     = 3.0;   // fin thickness (GoPro standard)
prong_pitch = 6.35;  // fin centre spacing (GoPro standard)
fin_r       = 7.5;   // fin end radius
hinge_off   = 13.0;  // hinge axis stand-off from the top wall face
hinge_bolt_d = 5.0;  // M5 thumbscrew (GoPro standard)
hinge_teeth = true;  // radial detent teeth on the mating faces — SAG-PROOF angle.
                     // Set false for smooth faces (full GoPro accessory compatibility).
teeth_n     = 24;    // detent positions (24 -> 15 degree steps)
teeth_h     = 0.6;   // tooth height

/* [Bracket] — wall plate with three prongs (mates the case hinge) */
br_x        = 46.0;  // plate width (along the hinge axis)
br_y        = 34.0;  // plate height
br_t        = 4.0;   // plate thickness
br_screw_d  = 4.2;   // countersunk wall screws (#8 / M4)
bracket_tripod = true; // captive 1/4-20 nut pocket behind the centre fin (tripod mount)

/* [Keyholes] — blind pockets in a thickened back (seal-safe) */
kh_extra   = 3.0;
kh_head_d  = 7.0;
kh_shank_d = 4.2;
kh_slot_l  = 8.0;
kh_head_h  = 3.5;    // total pocket depth (face web + head cavity)
kh_face    = 1.0;
kh_inset   = 12.0;   // pocket centres at y = ±(inner_y/2 − kh_inset), on the X centreline

/* [Weather sealing] */
gasket_w      = 1.2;
gasket_groove = 1.0;
gasket_proud  = 0.6;
skirt_h       = 3.0;
skirt_t       = 1.6;
usb_cover     = true;
usb_cov_pad   = 2.0;
usb_cov_dep   = 1.0;
hood_len      = 9.0;  // rain-hood protrusion from the front face
hood_t        = 1.8;  // hood wall thickness

/* [Front-face features] — offsets are measured FROM THE MODULE CENTRE so they
   stay valid for both hosts. Measure your build! */
lp_d   = 3.0;      // light-pipe diameter (hole = lp_d + 2*tol_press)
lp_dx  = 8.0;
lp_dy  = -8.0;
vent_pad_d     = 12.0;  // GORE seat (outer face)
vent_pad_depth = 0.8;
vent_hole_d    = 1.6;
vent_ring_d    = 6.0;
vent_holes     = 6;
vent_dx        = -8.0;
vent_dy        = -8.0;
mag_d  = 6.0;      // tamper MAGNET diameter (pocket = mag_d + 2*tol_press)
mag_h  = 3.2;
mag_dx = 8.0;
mag_dy = 8.0;

/* [Board snap clips] */
clip_w      = 6.0;
clip_t      = 1.5;
clip_hook   = 0.8;
clip_hook_h = 1.2;
clip_clear  = 0.25;

/* [Aesthetics] */
lid_edge    = 0.8;   // 45° chamfer around the front's top edge  // [0:0.1:1.5]
label_text  = "";    // debossed front label ("" = off; needs the font installed)
label_size  = 5.0;
label_depth = 0.5;
label_dx    = 0.0;
label_dy    = -14.0;
label_rot   = 0;
label_font  = "Liberation Sans:style=Bold";

/* [Quality] */
$fn = 64;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
wall_eff   = e_seal ? max(wall_t, gasket_w + 1.6) : wall_t;
clip_stack = clip_clear + clip_t;
post_corner = post_d + 1.5;
has_dk = (host == "devkit");

// xiao host: the module rides tall rails so the stacked XIAO hangs beneath it
vm_standoff = has_dk ? standoff_h : stack_sock_h + 1.5;

// devkit host: two columns (camera+module | DevKit); xiao host: one column.
// Posts always sit in true X-margins beside the boards.
col_cam = max(cam_w + 2*board_clear, vm_w + 2*(clip_stack + board_clear) + 0.5);
col_dk  = dk_w + 2*(clip_stack + board_clear) + 0.5;
mid_gap = 2.0;
inner_x = has_dk ? col_cam + mid_gap + col_dk + 2*post_corner
                 : col_cam + 2*post_corner;
inner_y = has_dk ? max(3 + cam_h + 2 + vm_l + 1.5 + 1.5, dk_l + 2*board_clear + 6.0)
                 : board_clear + vm_l + 2 + cam_h + 3;   // module at the USB wall, camera above

// cavity depth: xiao host is driven by the rail height + module front parts;
// devkit host by the tallest top-side component
cav_d_min = has_dk ? standoff_h + pcb_t + stack_h + cav_extra
                   : vm_standoff + pcb_t + vm_front_h + cav_extra;
usb_soff  = has_dk ? standoff_h : vm_standoff;            // standoff of the board that owns the USB wall
cav_d   = e_seal ? max(cav_d_min, usb_soff + pcb_t + usb_h + usb_z + 2.5) : cav_d_min;

out_x  = inner_x + 2*wall_eff;
out_y  = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;

cam_cx = has_dk ? -inner_x/2 + post_corner + col_cam/2 : 0;
dk_cx  =  inner_x/2 - post_corner - col_dk/2;                 // devkit host only
vm_cx  = cam_cx;
vm_cy_x = -inner_y/2 + board_clear + vm_l/2;                  // xiao host: module at the bottom wall
cam_cy = has_dk ? inner_y/2 - 3 - cam_h/2                     // camera at the top (3 mm lip margin)
                : vm_cy_x + vm_l/2 + 2 + cam_h/2;
vm_cy  = has_dk ? cam_cy - cam_h/2 - 2 - vm_l/2 : vm_cy_x;
dk_cy  = -inner_y/2 + board_clear + dk_l/2;                   // DevKit parked at the USB (bottom) wall
lens_x = cam_cx + lens_dx;
lens_y = cam_cy + lens_dy;
usb_cx = (has_dk ? dk_cx : vm_cx) + usb_dx;                   // main/upper USB opening centre (X)
usb_zc = floor_t + usb_soff + pcb_t + usb_h/2 + usb_z;        // and its centre height

mount_extra = (e_mount && (m_style == "keyhole" || m_style == "both")) ? kh_extra : 0;
kh_y  = inner_y/2 - kh_inset;
kh_ys = (kh_y >= kh_slot_l/2 + kh_head_d/2 + 2) ? [-kh_y, kh_y] : [0];
hinge_hole = hinge_bolt_d + 2*tol_hole/2 + 0.1;   // ~5.4 for M5: free pivot

skirt_gap = tol_slide + 0.2;
plate_x   = e_seal ? out_x + 2*(skirt_gap + skirt_t) : out_x;
plate_y   = e_seal ? out_y + 2*(skirt_gap + skirt_t) : out_y;
plate_r   = e_seal ? corner_r + skirt_gap + skirt_t : corner_r;

// sanity checks + a measurable size echo for scripted verification
assert(wall_t > 0 && floor_t > 0 && lid_t > 0, "shell thicknesses must be positive");
assert(standoff_h > 0, "standoff_h must be positive");
assert(lip_h < cav_d, "lip_h must be less than the cavity depth");
assert(screw_head_d > screw_d, "screw_head_d must be larger than screw_d");
assert(!e_seal || gasket_groove <= lip_h - 0.5, "gasket_groove must stay below the front lip depth");
assert(!e_seal || skirt_h < base_d, "skirt_h must be shorter than the back shell");
assert(cam_disc_t == 0 || cam_disc_t + 0.2 < lid_t, "cam_disc_t too thick for lid_t");
assert(cam_disc_d == 0 || cam_disc_d > cam_ap_d, "cam_disc_d must be larger than cam_ap_d");
assert(2*fin_r <= base_d + 0.01, "fin_r too large — prongs must not exceed the shell depth");
assert(mount_extra == 0 || kh_head_h + 1.5 <= floor_t + kh_extra, "keyhole pocket too deep — raise kh_extra");
assert(mount_extra == 0 || kh_head_h > kh_face, "kh_head_h must exceed kh_face");
assert(mount_extra == 0 || kh_head_d > kh_shank_d, "kh_head_d must be larger than kh_shank_d");
assert(lid_edge == 0 || (lid_edge >= 0.01 && lid_edge < lid_t), "lid_edge must be 0, or between 0.01 and lid_t");
assert(label_text == "" || (label_depth > 0 && label_depth < lid_t), "label_depth must be between 0 and lid_t");
assert(host == "devkit" || usb_zc - xiao_usb_drop - usb_h/2 >= floor_t + 1.0,
       "xiao_usb_drop too large — the XIAO port opening would breach the floor");
echo(str("Canary Vision enclosure v0.2 — outer ", out_x, " x ", out_y, " x ",
         base_d + lid_t + mount_extra, " mm (+", hinge_off + fin_r,
         " mm prongs)  (host=", host, ", preset=", preset, ", seal=", e_seal, ", mount=", m_style, ")"));
if (e_seal && wall_eff > wall_t)
    echo(str("seal mode: walls auto-thickened ", wall_t, " -> ", wall_eff, " mm to host the gasket groove"));

// ----------------------------------------------------------------------------
//  Helpers (shared idiom with canary_wap_enclosure.scad)
// ----------------------------------------------------------------------------
module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module rrect(l, w, r, h) { linear_extrude(height = h) rrect2d(l, w, r); }

module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
        offset(r = -w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
    }
}

function post_xy() = [
    [ inner_x/2 - post_d/2 - 0.2,  inner_y/2 - post_d/2 - 0.2],
    [-inner_x/2 + post_d/2 + 0.2,  inner_y/2 - post_d/2 - 0.2],
    [ inner_x/2 - post_d/2 - 0.2, -inner_y/2 + post_d/2 + 0.2],
    [-inner_x/2 + post_d/2 + 0.2, -inner_y/2 + post_d/2 + 0.2],
];

// ring pedestal that supports a PCB's underside along its perimeter
module ringped(cx, cy, l, w) {
    translate([cx, cy, floor_t]) difference() {
        rrect(l - 1.5, w - 1.5, 1.5, standoff_h);
        translate([0, 0, -0.5]) rrect(l - 7.5, w - 7.5, 1.0, standoff_h + 1);
    }
}

// Cantilever snap clip at edge point (px,py); `ang` = outward normal (degrees,
// pointing AWAY from the board); `soff` = board standoff (the rail/pedestal
// height the PCB sits on). Same proven profile as the WAP enclosure:
// flat retention seat over the board + 45° under-chamfer across the gap.
module edgeclip(px, py, ang, soff = standoff_h) {
    bt = floor_t + soff + pcb_t;
    tp = bt + clip_hook_h;
    pts = [ [clip_clear, floor_t], [clip_clear + clip_t, floor_t],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
}

module keyhole_pocket(yc) {           // blind pocket; slot runs toward +Y (up)
    y0 = yc - kh_slot_l/2;            // screw-head pass hole (bottom end)
    y1 = yc + kh_slot_l/2;
    z0 = -mount_extra;
    union() {
        translate([0, 0, z0 - 0.1]) linear_extrude(kh_face + 0.1) {
            translate([0, y0]) circle(d = kh_head_d);
            hull() { translate([0, y0]) circle(d = kh_shank_d);
                     translate([0, y1]) circle(d = kh_shank_d); }
        }
        translate([0, 0, z0 + kh_face]) linear_extrude(kh_head_h - kh_face)
            hull() { translate([0, y0]) circle(d = kh_head_d + 0.6);
                     translate([0, y1]) circle(d = kh_head_d + 0.6); }
    }
}

// 12-of-24 radial castellation: interlocks with an identical facing ring
module teeth2d() {
    step = 360 / teeth_n;
    for (i = [0 : 2 : teeth_n - 1]) rotate([0, 0, i * step])
        intersection() {
            difference() { circle(r = fin_r - 0.5); circle(r = 3.5); }
            polygon([[0, 0], [2*fin_r, 0],
                     [2*fin_r*cos(step), 2*fin_r*sin(step)]]);
        }
}

// one hinge fin on the case top wall, centred at x = xc (tombstone, flat on the bed)
module case_fin(xc) {
    hull() {
        translate([xc - prong_t/2, out_y/2 - 1, 0]) cube([prong_t, 1, 2*fin_r]);
        translate([xc - prong_t/2, out_y/2 + hinge_off, fin_r])
            rotate([0, 90, 0]) cylinder(r = fin_r, h = prong_t);
    }
}

// case-side hinge: two fins + root web, detent teeth on the OUTER faces,
// M5 bolt hole through. Prints with the shell, flat side on the bed.
module case_hinge() {
    ax = [0, out_y/2 + hinge_off, fin_r];        // hinge axis point
    difference() {
        union() {
            case_fin(-prong_pitch/2);
            case_fin( prong_pitch/2);
            // root web between/behind the fins (clears the bracket fins:
            // their swing circle never comes closer than hinge_off - fin_r)
            translate([-(prong_pitch/2 + prong_t/2), out_y/2 - 1, 0])
                cube([prong_pitch + prong_t, 1 + max(1, hinge_off - fin_r - 0.5), 2*fin_r]);
            if (hinge_teeth) {
                xo = prong_pitch/2 + prong_t/2;  // fin outer faces
                translate([ xo, ax[1], ax[2]]) rotate([0,  90, 0]) linear_extrude(teeth_h) teeth2d();
                translate([-xo, ax[1], ax[2]]) rotate([0, -90, 0]) linear_extrude(teeth_h) teeth2d();
            }
        }
        translate([-out_x/2, ax[1], ax[2]]) rotate([0, 90, 0])
            cylinder(d = hinge_hole, h = out_x);
    }
}

// ----------------------------------------------------------------------------
//  BACK shell (mounts to the wall; boards click in)
// ----------------------------------------------------------------------------
module back() {
    posts = post_xy();
    gusset_h = max(2, cav_d - lip_h - 1.0);

    union() {
        difference() {
            union() {
                rrect(out_x, out_y, corner_r, base_d);
                if (mount_extra > 0)
                    translate([0, 0, -mount_extra]) rrect(out_x, out_y, corner_r, mount_extra);
                if (e_mount && (m_style == "hinge" || m_style == "both"))
                    case_hinge();
            }
            translate([0, 0, floor_t])
                rrect(inner_x, inner_y, max(0.1, corner_r - wall_eff), cav_d + 1);
            // USB-C opening(s), bottom wall: DevKit port, or module "model port"
            translate([usb_cx, -out_y/2, usb_zc])
                cube([usb_w, wall_eff*3, usb_h], center = true);
            // xiao host: second opening below it for the XIAO "firmware port"
            if (!has_dk)
                translate([vm_cx + xiao_usb_dx, -out_y/2, usb_zc - xiao_usb_drop])
                    cube([usb_w, wall_eff*3, usb_h], center = true);
            // gasket groove in the front rim (seal mode)
            if (e_seal)
                translate([0, 0, base_d - gasket_groove])
                    linear_extrude(gasket_groove + 1) rim_ring2d(gasket_w);
            // recess framing the USB opening(s) for flanged silicone plugs
            // (xiao host: one tall recess spans both stacked ports)
            if (e_seal && usb_cover) {
                uz0 = (has_dk ? usb_zc : usb_zc - xiao_usb_drop) - usb_h/2 - usb_cov_pad;
                uz1 = min(usb_zc + usb_h/2 + usb_cov_pad, base_d - gasket_groove - 0.5);
                translate([usb_cx - (usb_w/2 + usb_cov_pad), -out_y/2 - 1, uz0])
                    cube([usb_w + 2*usb_cov_pad, 1 + usb_cov_dep, uz1 - uz0]);
            }
            // blind keyhole pockets (never reach the cavity — seal-safe)
            if (mount_extra > 0)
                for (yc = kh_ys) keyhole_pocket(yc);
        }

        // corner screw posts, gusseted to both walls (same pattern as the WAP case)
        difference() {
            union() {
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = post_d, h = cav_d);
                for (p = posts) {
                    sx = sign(p[0]); sy = sign(p[1]);
                    hull() {
                        translate([p[0], p[1], floor_t]) cylinder(d = post_d, h = gusset_h);
                        translate([sx*(inner_x/2 - 0.3), p[1], floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                    hull() {
                        translate([p[0], p[1], floor_t]) cylinder(d = post_d, h = gusset_h);
                        translate([p[0], sy*(inner_y/2 - 0.3), floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                }
            }
            for (p = posts) translate([p[0], p[1], floor_t + 2.0]) cylinder(d = screw_d, h = cav_d);
        }

        // board cradles + snap clips on the X edges.
        // devkit host: perimeter pedestals for both boards.
        // xiao host: two tall side rails under the module's X edges — the rail
        // gap clears the stacked XIAO hanging beneath it.
        if (has_dk) {
            ringped(dk_cx, dk_cy, dk_w, dk_l);
            ringped(vm_cx, vm_cy, vm_w, vm_l);
            for (s = [1, -1]) {
                for (dy = [-dk_l/4, dk_l/4])
                    edgeclip(dk_cx + s*dk_w/2, dk_cy + dy, s > 0 ? 0 : 180);
                edgeclip(vm_cx + s*vm_w/2, vm_cy, s > 0 ? 0 : 180);
            }
        } else {
            for (s = [1, -1]) {
                translate([vm_cx + s*(vm_w/2 - 1.5) - 1.5, vm_cy - (vm_l - 1)/2, floor_t])
                    cube([3, vm_l - 1, vm_standoff]);
                edgeclip(vm_cx + s*vm_w/2, vm_cy, s > 0 ? 0 : 180, vm_standoff);
            }
        }
    }
}

// ----------------------------------------------------------------------------
//  FRONT face (lens aperture + disc seat + hood, LED, vent, label; camera
//  board screws to posts on the inside)
// ----------------------------------------------------------------------------
module vent_cluster(x, y) {
    translate([x, y, 0]) {
        translate([0, 0, lid_t - vent_pad_depth]) cylinder(d = vent_pad_d, h = vent_pad_depth + 1);
        for (i = [0 : vent_holes - 1]) rotate([0, 0, i * 360 / vent_holes])
            translate([vent_ring_d/2, 0, -1]) cylinder(d = vent_hole_d, h = lid_t + 2);
    }
}

module front_plate() {
    if (lid_edge > 0) union() {
        rrect(plate_x, plate_y, plate_r, lid_t - lid_edge);
        hull() {
            translate([0, 0, lid_t - lid_edge]) rrect(plate_x, plate_y, plate_r, 0.01);
            translate([0, 0, lid_t - 0.01])
                rrect(plate_x - 2*lid_edge, plate_y - 2*lid_edge, max(0.1, plate_r - lid_edge), 0.01);
        }
    } else rrect(plate_x, plate_y, plate_r, lid_t);
}

module front() {
    union() {
        difference() {
            front_plate();
            // lens aperture + recessed clear-disc seat
            translate([lens_x, lens_y, -1]) cylinder(d = cam_ap_d, h = lid_t + 2);
            if (cam_disc_t > 0 && cam_disc_d > 0)
                translate([lens_x, lens_y, lid_t - (cam_disc_t + 0.2)])
                    cylinder(d = cam_disc_d + 2*tol_slide, h = cam_disc_t + 1);
            if (e_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -1]) cylinder(d = lp_d + 2*tol_press, h = lid_t + 2);
            if (e_vent || e_buzzer) vent_cluster(vm_cx + vent_dx, vm_cy + vent_dy);
            for (p = post_xy()) {
                translate([p[0], p[1], -1]) cylinder(d = screw_d + 2*tol_hole, h = lid_t + 2);
                translate([p[0], p[1], lid_t - screw_head_h])
                    cylinder(d1 = screw_d + 2*tol_hole, d2 = screw_head_d, h = screw_head_h + 0.1);
            }
            if (label_text != "")
                translate([label_dx, label_dy, lid_t - label_depth])
                    linear_extrude(label_depth + 1) rotate(label_rot)
                        text(label_text, size = label_size, font = label_font,
                             halign = "center", valign = "center");
        }

        // rain/glare hood: ~220° collar over the window, open at the bottom
        if (e_hood)
            translate([lens_x, lens_y, lid_t - 0.1]) linear_extrude(hood_len + 0.1)
                difference() {
                    circle(d = cam_disc_d + 3 + 2*hood_t);
                    circle(d = cam_disc_d + 3);
                    translate([-(cam_disc_d/2 + hood_t + 2), -2*(cam_disc_d + hood_t)])
                        square([cam_disc_d + 2*hood_t + 4, 2*(cam_disc_d + hood_t) - cam_disc_d*0.18]);
                }

        // camera-board posts on the inner face (Pi-cam v1.3 21 x 12.5 grid)
        for (sx = [1, -1], sy = [1, -1])
            translate([cam_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, -cam_post_h])
                difference() {
                    cylinder(d = cam_post_d, h = cam_post_h + 0.1);  // 0.1 embeds into the plate
                    translate([0, 0, -0.1]) cylinder(d = cam_screw_d, h = cam_post_h - 0.8);
                }

        // front lip nesting into the back shell, cleared at posts + USB notch
        difference() {
            translate([0, 0, -lip_h])
                difference() {
                    rrect(inner_x - 2*tol_slide, inner_y - 2*tol_slide,
                          max(0.1, corner_r - wall_eff - tol_slide), lip_h);
                    rrect(inner_x - 2*tol_slide - 2*lip_t, inner_y - 2*tol_slide - 2*lip_t,
                          0.1, lip_h + 1);
                }
            for (p = post_xy())
                translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = post_d + 1.2, h = lip_h + 0.2);
            translate([usb_cx, -inner_y/2, -lip_h/2])
                cube([usb_w + 4, lip_t*4, lip_h + 0.2], center = true);
        }

        // drip-edge skirt (seal mode), notched over the USB opening
        if (e_seal)
            difference() {
                translate([0, 0, -skirt_h]) linear_extrude(skirt_h)
                    difference() {
                        rrect2d(plate_x, plate_y, plate_r);
                        rrect2d(out_x + 2*skirt_gap, out_y + 2*skirt_gap, corner_r + skirt_gap);
                    }
                translate([usb_cx, -(out_y/2 + skirt_gap + skirt_t/2), -skirt_h/2])
                    cube([usb_w + 6, skirt_t*3, skirt_h + 0.4], center = true);
            }

        // tamper magnet pocket (press fit; embedded 0.1 so the export is one shell)
        if (e_tamper)
            translate([vm_cx + mag_dx, vm_cy + mag_dy, -mag_h]) difference() {
                cylinder(d = mag_d + 2*tol_press + 2.4, h = mag_h + 0.1);
                translate([0, 0, -0.1]) cylinder(d = mag_d + 2*tol_press, h = mag_h + 0.1);
            }
    }
}

// ----------------------------------------------------------------------------
//  GASKET — TPU seal ring matching the back-shell groove (seal mode)
// ----------------------------------------------------------------------------
module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.1); }

// ----------------------------------------------------------------------------
//  BRACKET — wall plate with three prongs; countersunk screws, keyhole slots,
//  optional captive 1/4-20 nut for tripods. Modelled in print orientation.
// ----------------------------------------------------------------------------
module bracket_fin(xc) {
    hull() {
        translate([xc - prong_t/2, -fin_r, br_t - 0.5]) cube([prong_t, 2*fin_r, 0.5]);
        translate([xc - prong_t/2, 0, br_t + hinge_off])
            rotate([0, 90, 0]) cylinder(r = fin_r, h = prong_t);
    }
}

module bracket() {
    az = br_t + hinge_off;                  // hinge axis height above the bed
    difference() {
        union() {
            rrect(br_x, br_y, 3, br_t);
            bracket_fin(0);
            bracket_fin(-prong_pitch);
            bracket_fin( prong_pitch);
            if (bracket_tripod)             // boss merges into the centre-fin root
                translate([0, 0, br_t - 0.1]) rrect(18, 18, 2, 2.6);
            if (hinge_teeth) {              // teeth on the OUTER fins' INNER faces
                xi = prong_pitch - prong_t/2;
                translate([ xi, 0, az]) rotate([0, -90, 0]) linear_extrude(teeth_h) teeth2d();
                translate([-xi, 0, az]) rotate([0,  90, 0]) linear_extrude(teeth_h) teeth2d();
            }
        }
        // M5 bolt bore through all three fins
        translate([-br_x/2, 0, az]) rotate([0, 90, 0]) cylinder(d = hinge_hole, h = br_x);
        // countersunk wall screws at the corners
        for (sx = [1, -1], sy = [1, -1]) {
            translate([sx*(br_x/2 - 5), sy*(br_y/2 - 5), -0.1])
                cylinder(d = br_screw_d, h = br_t + 0.2);
            translate([sx*(br_x/2 - 5), sy*(br_y/2 - 5), br_t - 2])
                cylinder(d1 = br_screw_d, d2 = br_screw_d + 4, h = 2.1);
        }
        // keyhole slots (through-plate; hang-and-slide-down)
        for (sx = [1, -1]) translate([sx*14, 0, 0]) {
            translate([0, -3, -0.1]) cylinder(d = 7.5, h = br_t + 0.2);
            translate([-2.1, -3, -0.1]) cube([4.2, 9, br_t + 0.2]);
            translate([0, 6, -0.1]) cylinder(d = 4.2, h = br_t + 0.2);
        }
        // captive 1/4-20 nut pocket (insert from the wall side) + stud bore
        if (bracket_tripod) {
            translate([0, 0, -0.1]) rotate([0, 0, 30]) cylinder(d = 11.4/cos(30), h = 5.2, $fn = 6);
            translate([0, 0, -0.1]) cylinder(d = 6.8, h = br_t + 3);
        }
    }
}

// ----------------------------------------------------------------------------
//  KNOB — printable M5 thumbscrew head (captive hex nut), or buy a GoPro
//  M5 knurled thumbscrew. Pair with an M5 x 25 bolt.
// ----------------------------------------------------------------------------
module knob() {
    difference() {
        cylinder(d = 22, h = 8);
        for (i = [0 : 11]) rotate([0, 0, i*30])
            translate([12.6, 0, -1]) cylinder(d = 5, h = 10);          // grip scallops
        translate([0, 0, -0.1]) rotate([0, 0, 30])
            cylinder(d = 8.1/cos(30), h = 4.3, $fn = 6);               // M5 nut pocket
        translate([0, 0, -0.1]) cylinder(d = hinge_bolt_d + 0.4, h = 10);
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
if      (part == "back")    back();
else if (part == "front")   translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
else if (part == "gasket") {
    assert(e_seal, "the gasket needs opt_seal=true (or preset=vision_weather) so its ring matches the groove");
    gasket();
}
else if (part == "bracket") bracket();
else if (part == "knob")    knob();
else {
    back();
    translate([0, -(out_y/2 + plate_y/2 + hinge_off + fin_r + 10), 0])
        translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
    translate([out_x/2 + br_x/2 + 14, 0, 0]) bracket();
    translate([out_x/2 + br_x/2 + 14, br_y/2 + 22, 0]) knob();
    if (e_seal) translate([-(out_x/2 + br_x/2 + 16), 0, 0]) gasket();
}
