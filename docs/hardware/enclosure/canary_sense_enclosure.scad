// ============================================================================
//  SecuraCV Canary Sense — 3D-printable RADOME enclosure (parametric)  v0.1
// @env cer=2 ip="radome (sheltered)"
//  Hardware: Seeed MR60BHA2 60 GHz mmWave kit — radar carrier board with a
//  XIAO ESP32-C6 seated in its stacking socket (design doc:
//  docs/canary_sense_mr60bha2_design.md). Ceiling or wall mounted; bedside
//  (<=1.5 m) for the wellbeing/breathing channel.
//
//  THE RADOME RULE: 60 GHz must pass through the front face. The window over
//  the antenna zone is a FLAT, UNIFORM membrane — no ribs, no label, no seams
//  and no gasket path crossing it, and the board mounts parallel to it at a
//  small air gap. Thickness is TUNED, not just thin: at 60 GHz (λ0 ≈ 5.0 mm)
//  a PETG/ASA wall (εr ≈ 2.7) is transparent at the HALF-WAVE thickness
//  λ0/(2·√εr) ≈ 1.5 mm (<0.5 % reflection) and worst near the quarter-wave
//  band ~0.7–1.1 mm (up to ~20 % reflected back into the antenna — standing
//  waves corrupt the µm-scale breathing phase). Use 1.5 mm; never put metal,
//  foil labels or CF-filled filament in front of the antenna.
//
//  Everything else reuses the proven Canary patterns: snap-clip rails with
//  the stacked XIAO hanging beneath, GoPro-compatible hinge + blind keyholes,
//  opt-in TPU gasket sealing, and the engineering kit (ribs/chamfer/inserts).
//
//  ⚠️ VERIFY BEFORE PRINTING. Board dimensions are nominal — MEASURE your
//     carrier, the seated stack height, and the antenna-zone position.
//  Orientation: +Y up on the wall, USB opening on the BOTTOM (-Y) wall,
//  prongs on the TOP wall, +Z = toward the radome face.
//
//  2026-08-23: adopted the shared contract libraries (core/mount/snap/port/
//              board/mark) — the local helper copies they replace drew the
//              same geometry. Three fixes ride along: PAN-head FLAT
//              counterbores on the front (the Vision's print-validated
//              lesson — the old shallow cone left the head standing on the
//              show face), the bridge-safe chamfered USB opening
//              (canary_port_lib), and the bracket's detent teeth cut FEMALE
//              (male/male teeth cannot nest in the fin gap — the Vision
//              bracket's fix, mirrored). New opt_mark knob debosses the
//              house wordmark (default off).
// ============================================================================

use <canary_core_lib.scad>    // rrect/rrect2d, soft-edge front, foot chamfer, screw seats, tearbore
use <canary_mount_lib.scad>   // the stud/keyhole hanging interface — the blind pockets' one home
use <canary_snap_lib.scad>    // the cantilever board clip + its strain budget
use <canary_port_lib.scad>    // bridge-safe USB opening (the WAP's print-validated profile)
use <canary_board_lib.scad>   // board registry — the MR60 carrier numbers the knobs cite
use <canary_mark_lib.scad>    // the house wordmark (opt_mark)

/* [What to render] */
part   = "all";       // ["back","front","all","gasket","bracket","knob"]

/* [Options] */
opt_led    = true;    // onboard WS2812 -> light-pipe port (outside the radome zone)
opt_lux    = true;    // BH1750 lux sensor -> small light aperture (outside the radome zone)
opt_vent   = false;   // GORE vent cluster (recommended with opt_seal)
opt_tamper = false;   // reed/Hall magnet pocket
opt_seal   = false;   // perimeter TPU gasket + drip-edge front (indoor ceilings rarely need it)
opt_mount  = true;
mount_style = "hinge"; // ["hinge","keyhole","both"]
e_seal  = opt_seal;
e_mount = opt_mount;
m_style = mount_style;

/* [Radar flavor] */
// Both Seeed MR60 kits share this carrier family and 60 GHz radome physics:
//   bha2 — breathing/presence (wall/stand mount, the default build)
//   fda2 — FALL DETECTION: mount on the CEILING, 2.4-3.1 m, face straight
//          down over the fall zone (keyhole mount; check rad_dx/dy against
//          YOUR carrier — the FDA2 antenna zone may sit differently)
radar = "bha2";       // ["bha2","fda2"]

/* [Boards] — Seeed MR60BHA2 kit carrier + stacked XIAO ESP32-C6. MEASURE YOURS */
vm_l     = 44.0;   // carrier length (Y; XIAO/USB edge down) — brd_l("mr60"), canary_board_lib
vm_w     = 36.0;   // carrier width (X) — brd_w("mr60")
xiao_l   = 21.0;   // brd_l("xiao")
xiao_w   = 17.5;   // brd_w("xiao") spec; clips absorb the measured 17.8 (brd_xiao_w_measured)
stack_sock_h = 11.5; // carrier underside -> XIAO underside when seated — MEASURE.
                     // brd_stack_sock_unmeasured() kept on purpose: the registry's
                     // measured 6.5 (brd_stack_sock_measured — doorbell bench) would
                     // shrink a cavity that has survived prints as-is
vm_front_h   = 3.5;  // carrier front-side TALLEST part (connectors etc.) — MEASURE
ant_h        = 1.2;  // antenna (AiP package) top above the PCB — MEASURE; sets the radome air gap
pcb_t    = 1.0;
board_clear = 0.6;
xiao_usb_z  = 4.0;   // XIAO USB-C center above the back floor — MEASURE the seated stack

/* [Radome window] — thin flat membrane over the antenna zone */
radome_t  = 1.5;     // membrane thickness — 1.5 ≈ half-wave in PETG/ASA at 60 GHz (low-reflection
                     // optimum); AVOID 0.7–1.1 (quarter-wave band, ~20 % reflection)  // [1.4:0.1:1.6]
rad_win_x = 24.0;    // window size (X) — cover the antenna array generously
rad_win_y = 24.0;    // window size (Y)
rad_dx    = 0.0;     // antenna-zone center offset from the BOARD center — MEASURE
rad_dy    = 6.0;     //   (the array usually sits toward the top half of the carrier)
// antenna-to-radome air gap is COMPUTED below (rad_gap) and asserted >= 3.0
// for clean radar performance; raise cav_extra for more gap.

/* [Front-face features] — offsets from the BOARD center; keep them OUT of the window */
lp_d   = 3.0;        // WS2812 light pipe (press fit)
lp_dx  = 13.0;
lp_dy  = -14.0;
lux_d  = 3.5;        // BH1750 light aperture (open hole; glue a clear disc behind it when sealing)
lux_dx = -13.0;
lux_dy = -14.0;
vent_pad_d     = 12.0;
vent_pad_depth = 0.8;
vent_hole_d    = 1.0;   // fine holes — insect-resistant (see README thermal/outdoor kit)
vent_ring_d    = 6.0;
vent_holes     = 10;
vent_dx        = 0.0;
vent_dy        = -17.0;
mag_d  = 6.0;
mag_h  = 2.2;        // pocket depth — a 6 x 2 mm disc is standard; the pocket ring descends
mag_dx = 13.0;       // mag_h below the front's inner face, so parts on the carrier under
mag_dy = 14.0;       // (mag_dx, mag_dy) must stay >= 1 mm below vm_front_h — MEASURE

/* [Shell] */
wall_t   = 2.0;
floor_t  = 2.0;
lid_t    = 2.0;      // face thickness OUTSIDE the radome window
lip_h    = 4.0;
lip_t    = 1.2;
corner_r = 3.0;
cav_extra = 1.0;

/* [Print tolerances] — defaults = the catalog trio (core_tol_*(), canary_core_lib), dialed on the fit coupon */
tol_slide = 0.20;    // sliding fits: front lip, drip skirt — core_tol_slide()
tol_press = 0.10;    // press fits: magnet, light pipe — core_tol_press()
tol_hole  = 0.30;    // clearance holes: front screws — core_tol_hole()

/* [Engineering] (see README "Engineering & materials") */
screw_insert = false;
insert_d     = 3.5;
insert_h     = 4.0;
lid_ribs     = true;   // rib ring auto-clears the radome window
lid_rib_w    = 2.5;
lid_rib_h    = 1.0;
foot_cham    = 0.5;
kh_lock      = true;

/* [Screw posts] */
post_d       = 5.0;
screw_d      = 1.6;
screw_head_d = 4.0;
screw_head_h = 2.0;

/* [USB-C port] — the stacked XIAO's port, bottom (-Y) wall */
usb_w  = 10.5;
usb_h  = 6.5;
usb_dx = 0.0;

/* [Hinge — GoPro-compatible, top wall (aim the beam; ceiling->bed for wellbeing)] */
prong_t     = 3.0;
prong_pitch = 6.35;
fin_r       = 7.5;
hinge_off   = 13.0;
hinge_bolt_d = 5.0;
hinge_teeth = true;
teeth_n     = 24;
teeth_h     = 0.6;

/* [Bracket] */
br_x        = 46.0;
br_y        = 34.0;
br_t        = 4.0;
br_screw_d  = 4.2;
bracket_tripod = true;

/* [Keyholes] — blind, seal-safe (flush ceiling/wall mount) */
kh_extra   = 3.0;
kh_head_d  = 7.0;    // screw-head pass hole (#6 / M3.5 pan head) — mount_kh_head_d()
kh_shank_d = 4.2;    // shank slot width — mount_kh_shank_d()
kh_slot_l  = 8.0;    // slot travel (toward +Y = UP on the wall) — mount_kh_slot_l()
kh_head_h  = 3.5;    // total pocket depth (face web + head cavity) — mount_kh_head_h()
kh_face    = 1.0;    // face web the screw head grips behind — mount_kh_face()
kh_inset   = 12.0;

/* [Weather sealing] */
gasket_w      = 1.6;
gasket_groove = 1.2;
gasket_proud  = 0.3;
skirt_h       = 3.0;
skirt_t       = 1.6;
usb_cover     = true;
usb_cov_pad   = 2.0;
usb_cov_dep   = 1.0;

/* [Aesthetics] */
lid_edge    = 0.8;
lid_edge2   = 0.0;
// The wordmark sits where label_text would (label_dx/dy/rot/size/depth place
// it), gated by the mark library's measured type metrics; the radome rule
// binds it like
opt_mark    = false; // deboss the house wordmark instead of a custom label (exclusive with label_text)
                     // any label: keep it OUT of the window
label_text  = "";    // debossed label — placed at label_dx/dy; keep it OUT of the radome window
label_size  = 4.5;
label_depth = 0.5;
label_dx    = 0.0;
label_dy    = -24.0;
label_rot   = 0;
label_font  = "Liberation Sans:style=Bold";

/* [Board snap clips] */
clip_w      = 6.0;   // tab width along the carrier edge — snap_boardclip defaults, canary_snap_lib
clip_t      = 1.0;   // beam thickness — the library runs the strain budget as an assert
clip_hook   = 0.5;   // lip overhang over the carrier top
clip_hook_h = 1.2;   // lip + 45° lead-in height above the carrier top
clip_clear  = 0.25;  // beam face to carrier edge (a fit — tune on the coupon)

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
wall_eff   = e_seal ? max(wall_t, gasket_w + 1.6) : wall_t;
pd = screw_insert ? max(post_d, insert_d + 2.4) : post_d;
clip_stack  = clip_clear + clip_t;
vm_standoff = stack_sock_h + 1.5;
post_corner = pd + 1.5;

inner_x = vm_w + 2*(clip_stack + board_clear) + 0.5 + 2*post_corner;
inner_y = board_clear + vm_l + 2 + 6;      // board at the USB wall + wire room + top margin
cav_d_min = vm_standoff + pcb_t + vm_front_h + cav_extra;
usb_zc  = floor_t + xiao_usb_z;
cav_d   = e_seal ? max(cav_d_min, xiao_usb_z + usb_h/2 + 2.5) : cav_d_min;
// actual antenna-to-radome air gap: from the AiP top to the thinned window's
// inner face (cavity headroom above the tallest part + the window recess)
rad_gap = (vm_front_h - ant_h) + (cav_d - cav_d_min + cav_extra) + (lid_t - radome_t);

out_x  = inner_x + 2*wall_eff;
out_y  = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;

vm_cx  = 0;
vm_cy  = -inner_y/2 + board_clear + vm_l/2;
rad_cx = vm_cx + rad_dx;                    // radome window center
rad_cy = vm_cy + rad_dy;
usb_cx = vm_cx + usb_dx;

mount_extra = (e_mount && (m_style == "keyhole" || m_style == "both")) ? kh_extra : 0;
kh_y  = inner_y/2 - kh_inset;
kh_ys = (kh_y >= kh_slot_l/2 + kh_head_d/2 + 2) ? [-kh_y, kh_y] : [0];
hinge_hole = hinge_bolt_d + 0.4;

skirt_gap = tol_slide + 0.2;
plate_x   = e_seal ? out_x + 2*(skirt_gap + skirt_t) : out_x;
plate_y   = e_seal ? out_y + 2*(skirt_gap + skirt_t) : out_y;
plate_r   = e_seal ? corner_r + skirt_gap + skirt_t : corner_r;

function post_xy() = [
    [ inner_x/2 - pd/2 - 0.2,  inner_y/2 - pd/2 - 0.2],
    [-inner_x/2 + pd/2 + 0.2,  inner_y/2 - pd/2 - 0.2],
    [ inner_x/2 - pd/2 - 0.2, -inner_y/2 + pd/2 + 0.2],
    [-inner_x/2 + pd/2 + 0.2, -inner_y/2 + pd/2 + 0.2],
];

assert(radome_t >= 0.6 && radome_t < lid_t, "radome_t must be printable (>=0.6) and thinner than lid_t");
assert(rad_gap >= 3.0, "antenna-to-radome gap < 3 mm — raise cav_extra");
assert(rad_win_x + 2*abs(rad_dx) <= inner_x - 4 && rad_win_y + 2*abs(rad_dy) <= vm_l,
       "radome window exceeds the face — shrink rad_win/rad_dx/rad_dy or grow the board zone");
assert(lip_h < cav_d, "lip_h must be less than the cavity depth");
assert(screw_head_d > screw_d, "screw_head_d must be larger than screw_d");
assert(!e_seal || gasket_groove <= lip_h - 0.5, "gasket_groove must stay below the lip");
assert(!e_seal || skirt_h < base_d, "skirt_h must be shorter than the back shell");
assert(mount_extra == 0 || kh_head_h + 1.5 <= floor_t + kh_extra, "keyhole pocket too deep");
assert(lid_edge == 0 || (lid_edge >= 0.01 && lid_edge < lid_t), "lid_edge out of range");
assert(lid_edge2 >= 0 && (lid_edge > 0 || lid_edge2 == 0) && lid_edge + lid_edge2 < lid_t,
       "lid_edge2 requires lid_edge > 0, and their sum must stay below lid_t");
assert(2*fin_r <= base_d + 0.01, "fin_r too large — prongs must not exceed the shell depth");
assert(radar == "bha2" || radar == "fda2", "radar must be \"bha2\" or \"fda2\"");
assert(!(opt_mark && label_text != ""),
       "opt_mark and label_text share the label spot — set one, not both");
// the wordmark's two gates, from the mark library's measured type metrics:
// below mark_word_min_h() a 0.4 mm bead no longer reaches the letterforms and
// the deboss prints as a smudge with the rhythm of type — the render looks
// perfect either way, which is why this is an assert and not an eyeball
assert(!opt_mark || label_size >= mark_word_min_h(),
       str("opt_mark at label_size ", label_size, " mm is under the ",
           mark_word_min_h(), " mm cap height where a 0.4 mm bead still ",
           "reaches the letterforms — raise label_size"));
assert(!opt_mark || mark_word_ink_w("securaCV", label_size) <= plate_x - 4.0,
       str("the wordmark draws ", mark_word_ink_w("securaCV", label_size),
           " mm at label_size ", label_size, " on a ", plate_x,
           " mm face (2 mm margin per side) — shrink label_size"));
echo(str("Canary Sense RADOME enclosure v0.1 — MR60", radar == "fda2" ? "FDA2" : "BHA2",
         ", outer ", out_x, " x ", out_y, " x ",
         base_d + lid_t + mount_extra, " mm (+", hinge_off + fin_r, " mm prongs)  (radome ",
         radome_t, " mm, ", rad_win_x, "x", rad_win_y, " window, antenna air gap ",
         rad_gap, " mm; seal=", e_seal, ", mount=", m_style, ")"));
if (radar == "fda2")
    echo("FDA2 fall-detection: CEILING mount 2.4-3.1 m facing straight down — verify rad_dx/dy against YOUR carrier");

// ----------------------------------------------------------------------------
//  Helpers — the shared idiom (rrect, tearbore, clip, keyhole, foot chamfer)
//  now comes from the contract libraries; what stays local is this file's own
// ----------------------------------------------------------------------------
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
        offset(r = -w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, corner_r - wall_eff/2));
    }
}
// Cantilever snap clip on a carrier edge — canary_snap_lib's beam (the WAP
// pattern), so the insertion-strain arithmetic runs as an assert on every
// render. This file's clips stand on the ±X edges, so the wrapper keeps the
// axis rotation and hands the drawing to the library.
module edgeclip(px, py, ang, soff) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, floor_t, floor_t + soff + pcb_t,
                       clip_w, clip_t, clip_hook, clip_hook_h, clip_clear);
}
// blind keyhole pocket (yc = center along Y; slot toward +Y = UP on the wall);
// canary_mount_lib draws it natively along the axis — no rotate — so the
// interface has one home and this mesh stays put
module keyhole_pocket(yc) {
    mount_keyhole_pocket(yc, -mount_extra, "y",
                         kh_head_d, kh_shank_d, kh_slot_l, kh_head_h, kh_face);
}
module teeth2d() {
    step = 360 / teeth_n;
    for (i = [0 : 2 : teeth_n - 1]) rotate([0, 0, i * step])
        intersection() {
            difference() { circle(r = fin_r - 0.5); circle(r = 3.5); }
            polygon([[0, 0], [2*fin_r, 0], [2*fin_r*cos(step), 2*fin_r*sin(step)]]);
        }
}
module case_fin(xc) {
    hull() {
        translate([xc - prong_t/2, out_y/2 - 1, 0]) cube([prong_t, 1, 2*fin_r]);
        translate([xc - prong_t/2, out_y/2 + hinge_off, fin_r])
            rotate([0, 90, 0]) cylinder(r = fin_r, h = prong_t);
    }
}
module case_hinge() {
    ax = [0, out_y/2 + hinge_off, fin_r];
    difference() {
        union() {
            case_fin(-prong_pitch/2);
            case_fin( prong_pitch/2);
            translate([-(prong_pitch/2 + prong_t/2), out_y/2 - 1, 0])
                cube([prong_pitch + prong_t, 1 + max(1, hinge_off - fin_r - 0.5), 2*fin_r]);
            if (hinge_teeth) {
                xo = prong_pitch/2 + prong_t/2;
                translate([ xo, ax[1], ax[2]]) rotate([0,  90, 0]) linear_extrude(teeth_h) teeth2d();
                translate([-xo, ax[1], ax[2]]) rotate([0, -90, 0]) linear_extrude(teeth_h) teeth2d();
            }
        }
        tearbore_x(-out_x/2, ax[1], ax[2], out_x, hinge_hole);
    }
}
// peripheral wedge that 45°-chamfers the bottom edge (subtract from the shell);
// canary_core_lib owns the drawing — bounded to the footprint so the hinge
// prongs lose only a root nick
module foot_chamfer_cut() {
    foot_chamfer_ring(out_x, out_y, corner_r, foot_cham, -mount_extra);
}

// ----------------------------------------------------------------------------
//  BACK shell
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
            translate([0, 0, floor_t]) rrect(inner_x, inner_y, max(0.1, corner_r - wall_eff), cav_d + 1);
            // XIAO USB-C opening, bottom wall: 45°-chamfered top corners halve
            // the unsupported bridge in the upright-printed wall and keep any
            // droop out of the plug envelope — canary_port_lib (the WAP's
            // print-validated profile; this wall used to bridge a flat top)
            translate([usb_cx, -out_y/2 + wall_eff*1.5, usb_zc])
                rotate([90, 0, 0]) linear_extrude(wall_eff*3)
                    port_bridge_profile2d(usb_w, usb_h);
            if (e_seal)
                translate([0, 0, base_d - gasket_groove])
                    linear_extrude(gasket_groove + 1) rim_ring2d(gasket_w);
            if (e_seal && usb_cover) {
                uz0 = usb_zc - usb_h/2 - usb_cov_pad;
                uz1 = min(usb_zc + usb_h/2 + usb_cov_pad, base_d - gasket_groove - 0.5);
                translate([usb_cx - (usb_w/2 + usb_cov_pad), -out_y/2 - 1, uz0])
                    cube([usb_w + 2*usb_cov_pad, 1 + usb_cov_dep, uz1 - uz0]);
            }
            if (mount_extra > 0)
                for (yc = kh_ys) keyhole_pocket(yc);
            if (mount_extra > 0 && kh_lock)
                for (sx = [1, -1]) translate([sx*12, inner_y/2 - 5, 0]) {
                    translate([0, 0, -mount_extra + 0.6]) cylinder(d = 3.2, h = mount_extra + floor_t);
                    translate([0, 0, floor_t - 1.2]) cylinder(d1 = 3.2, d2 = 6.0, h = 1.21);
                }
            if (foot_cham > 0) foot_chamfer_cut();
        }
        // corner screw posts, gusseted
        difference() {
            union() {
                for (p = posts) translate([p[0], p[1], floor_t]) cylinder(d = pd, h = cav_d);
                for (p = posts) {
                    sx = sign(p[0]); sy = sign(p[1]);
                    hull() {
                        translate([p[0], p[1], floor_t]) cylinder(d = pd, h = gusset_h);
                        translate([sx*(inner_x/2 - 0.3), p[1], floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                    hull() {
                        translate([p[0], p[1], floor_t]) cylinder(d = pd, h = gusset_h);
                        translate([p[0], sy*(inner_y/2 - 0.3), floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                }
            }
            for (p = posts) translate([p[0], p[1], floor_t + 2.0]) cylinder(d = screw_d, h = cav_d);
            if (screw_insert)
                for (p = posts) translate([p[0], p[1], floor_t + cav_d - insert_h - 0.5])
                    cylinder(d = insert_d - 0.1, h = insert_h + 1);
        }
        // carrier rails (notched at the clips); the stacked XIAO hangs beneath
        for (s = [1, -1]) {
            difference() {
                translate([vm_cx + s*(vm_w/2 - 1.5) - 1.5, vm_cy - (vm_l - 1)/2, floor_t])
                    cube([3, vm_l - 1, vm_standoff]);
                translate([vm_cx + s*(vm_w/2 - 1.5), vm_cy, floor_t + vm_standoff/2])
                    cube([5, clip_w + 2, vm_standoff + 1], center = true);
            }
            edgeclip(vm_cx + s*vm_w/2, vm_cy, s > 0 ? 0 : 180, vm_standoff);
        }
    }
}

// ----------------------------------------------------------------------------
//  FRONT face — the RADOME: thinned window over the antenna, features outside it
// ----------------------------------------------------------------------------
module vent_cluster(x, y) {
    translate([x, y, 0]) {
        translate([0, 0, lid_t - vent_pad_depth]) cylinder(d = vent_pad_d, h = vent_pad_depth + 1);
        for (i = [0 : vent_holes - 1]) rotate([0, 0, i * 360 / vent_holes])
            translate([vent_ring_d/2, 0, -1]) cylinder(d = vent_hole_d, h = lid_t + 2);
    }
}
module front() {
    union() {
        difference() {
            // the plate with the catalog's two-stage soft edge — canary_core_lib
            soft_edge_plate(plate_x, plate_y, plate_r, lid_t, lid_edge, lid_edge2);
            // RADOME window: blind thinning from the INSIDE, leaving a flat
            // uniform radome_t membrane. Rounded corners avoid stress risers.
            translate([rad_cx, rad_cy, -1])
                linear_extrude(lid_t - radome_t + 1)
                    rrect2d(rad_win_x, rad_win_y, 3);
            if (opt_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -1]) cylinder(d = lp_d + 2*tol_press, h = lid_t + 2);
            if (opt_lux) translate([vm_cx + lux_dx, vm_cy + lux_dy, -1]) cylinder(d = lux_d, h = lid_t + 2);
            if (opt_vent) vent_cluster(vm_cx + vent_dx, vm_cy + vent_dy);
            // flat counterbores: the BOM's PAN-head screws seat flush — the
            // canary_core_lib seat, per the Vision's print-validated lesson
            // (a cone this shallow left the head standing on the show face)
            for (p = post_xy())
                cb_flat_cut(p[0], p[1], lid_t, screw_d + 2*tol_hole,
                            screw_head_d + 2*tol_hole, screw_head_h);
            if (label_text != "")
                translate([label_dx, label_dy, lid_t - label_depth])
                    linear_extrude(label_depth + 1) rotate(label_rot)
                        text(label_text, size = label_size, font = label_font,
                             halign = "center", valign = "center");
            // the house wordmark (opt_mark), debossed exactly where the label
            // would sit and by the same first-layer machinery — canary_mark_lib
            // owns the word and its face, this file only places it
            if (opt_mark)
                translate([label_dx, label_dy, lid_t - label_depth])
                    linear_extrude(label_depth + 1) rotate(label_rot)
                        mark_wordmark(label_size);
        }
        // rib ring, auto-cleared around the RADOME window and every feature
        if (lid_ribs) {
            ro_x = inner_x - 2*tol_slide - 2*lip_t - 0.8;
            ro_y = inner_y - 2*tol_slide - 2*lip_t - 0.8;
            difference() {
                translate([0, 0, -lid_rib_h]) linear_extrude(lid_rib_h + 0.1)
                    difference() {
                        rrect2d(ro_x, ro_y, max(0.1, corner_r - wall_eff - lip_t));
                        rrect2d(ro_x - 2*lid_rib_w, ro_y - 2*lid_rib_w, 0.1);
                    }
                for (p = post_xy())
                    translate([p[0], p[1], -lid_rib_h - 0.1]) cylinder(d = pd + 1.6, h = lid_rib_h + 0.2);
                translate([rad_cx, rad_cy, -lid_rib_h - 0.1])
                    linear_extrude(lid_rib_h + 0.2) rrect2d(rad_win_x + 3, rad_win_y + 3, 3);
                if (opt_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -lid_rib_h - 0.1])
                    cylinder(d = lp_d + 4, h = lid_rib_h + 0.2);
                if (opt_lux) translate([vm_cx + lux_dx, vm_cy + lux_dy, -lid_rib_h - 0.1])
                    cylinder(d = lux_d + 3, h = lid_rib_h + 0.2);
                if (opt_vent) translate([vm_cx + vent_dx, vm_cy + vent_dy, -lid_rib_h - 0.1])
                    cylinder(d = vent_pad_d + 3, h = lid_rib_h + 0.2);
                if (opt_tamper) translate([vm_cx + mag_dx, vm_cy + mag_dy, -lid_rib_h - 0.1])
                    cylinder(d = mag_d + 2*tol_press + 4.8, h = lid_rib_h + 0.2);
                translate([usb_cx, -inner_y/2, 0]) cube([usb_w + 4, 14, 3*lid_rib_h], center = true);
            }
        }
        // lip, cleared at posts + USB notch
        difference() {
            translate([0, 0, -lip_h])
                difference() {
                    rrect(inner_x - 2*tol_slide, inner_y - 2*tol_slide,
                          max(0.1, corner_r - wall_eff - tol_slide), lip_h);
                    rrect(inner_x - 2*tol_slide - 2*lip_t, inner_y - 2*tol_slide - 2*lip_t, 0.1, lip_h + 1);
                }
            for (p = post_xy())
                translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
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
        if (opt_tamper)
            translate([vm_cx + mag_dx, vm_cy + mag_dy, -mag_h]) difference() {
                cylinder(d = mag_d + 2*tol_press + 2.4, h = mag_h + 0.1);
                translate([0, 0, -0.1]) cylinder(d = mag_d + 2*tol_press, h = mag_h + 0.1);
            }
    }
}

// ----------------------------------------------------------------------------
//  GASKET / BRACKET / KNOB (same system as the Vision case)
// ----------------------------------------------------------------------------
module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5); }
module bracket_fin(xc) {
    hull() {
        translate([xc - prong_t/2, -fin_r, br_t - 0.5]) cube([prong_t, 2*fin_r, 0.5]);
        translate([xc - prong_t/2, 0, br_t + hinge_off])
            rotate([0, 90, 0]) cylinder(r = fin_r, h = prong_t);
    }
}
module bracket() {
    az = br_t + hinge_off;
    difference() {
        union() {
            rrect(br_x, br_y, 3, br_t);
            bracket_fin(0);  bracket_fin(-prong_pitch);  bracket_fin(prong_pitch);
            if (bracket_tripod) translate([0, 0, br_t - 0.1]) rrect(18, 18, 2, 2.6);
        }
        tearbore_x(-br_x/2, 0, az, br_x, hinge_hole);
        // detent tooth POCKETS cut into the outer fins' inner faces — the case
        // fins carry the male teeth. One side must be female: two protruding
        // rings can't nest in the 0.175 mm fin gap, so male/male meshing would
        // permanently spring the fins apart (creep -> detents loosen). The
        // Vision bracket's fix, mirrored.
        if (hinge_teeth) {
            xi = prong_pitch - prong_t/2;   // outer fins' inner faces
            translate([ xi - 0.05, 0, az]) rotate([0,  90, 0])
                linear_extrude(teeth_h + 0.15) offset(delta = 0.12) teeth2d();
            translate([-xi + 0.05, 0, az]) rotate([0, -90, 0])
                linear_extrude(teeth_h + 0.15) offset(delta = 0.12) teeth2d();
        }
        for (sx = [1, -1], sy = [1, -1]) {
            translate([sx*(br_x/2 - 5), sy*(br_y/2 - 5), -0.1]) cylinder(d = br_screw_d, h = br_t + 0.2);
            translate([sx*(br_x/2 - 5), sy*(br_y/2 - 5), br_t - 2])
                cylinder(d1 = br_screw_d, d2 = br_screw_d + 4, h = 2.1);
        }
        for (sx = [1, -1]) translate([sx*14, 0, 0]) {
            translate([0, -3, -0.1]) cylinder(d = 7.5, h = br_t + 0.2);
            translate([-2.1, -3, -0.1]) cube([4.2, 9, br_t + 0.2]);
            translate([0, 6, -0.1]) cylinder(d = 4.2, h = br_t + 0.2);
        }
        if (bracket_tripod) {
            translate([0, 0, -0.1]) rotate([0, 0, 30]) cylinder(d = 11.4/cos(30), h = 5.2, $fn = 6);
            translate([0, 0, -0.1]) cylinder(d = 6.8, h = br_t + 3);
        }
    }
}
module knob() {
    difference() {
        cylinder(d = 22, h = 8);
        for (i = [0 : 11]) rotate([0, 0, i*30]) translate([12.6, 0, -1]) cylinder(d = 5, h = 10);
        translate([0, 0, -0.1]) rotate([0, 0, 30]) cylinder(d = 8.1/cos(30), h = 4.3, $fn = 6);
        translate([0, 0, -0.1]) cylinder(d = hinge_bolt_d + 0.4, h = 10);
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
if      (part == "back")    back();
else if (part == "front")   translate([0, 0, lid_t]) rotate([180, 0, 0]) front();
else if (part == "gasket") { assert(e_seal, "gasket needs opt_seal=true"); gasket(); }
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
