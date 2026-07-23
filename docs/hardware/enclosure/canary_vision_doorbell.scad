// ============================================================================
//  SecuraCV Canary Vision — DOORBELL enclosure (parametric)  v0.2
//  A slim vertical unit in the Wyze/Ring video-doorbell form factor, holding
//  the stacked-XIAO Vision build: OV5647 camera (top) + Grove Vision AI V2
//  with a XIAO ESP32-C3/S3 seated in its socket (middle) + a 12 mm
//  illuminated momentary button (bottom, wired to the multifunction input).
//
//  Mounting follows the doorbell pattern, not the hinge: a thin WALL PLATE
//  screws to the door frame (flat, or a printable 5–15° WEDGE for angling
//  toward the approach); the body drops onto the plate's two T-studs (the
//  same blind seal-safe pockets as the keyhole system) and locks with a
//  hidden SECURITY SCREW driven up through the plate's bottom foot — the
//  body cannot be lifted off without a tool, Ring-style.
//
//  Power: USB-C cable from the stack's ports loops through the internal
//  cable well and exits through an oval in the BACK, through the matching
//  plate hole, into the wall/under trim (use a right-angle USB-C plug).
//
//  Units: millimetres.  CAD: OpenSCAD.  Weather sealing is ON by default
//  (a doorbell lives outside): TPU gasket + drip-edge face, ~IP54.
//
//  ⚠️ VERIFY BEFORE PRINTING. Measure your seated stack (stack_sock_h,
//     usb heights) and your button's body diameter/depth before printing.
//  Orientation: +Y up on the wall, +Z toward the face.
// ============================================================================

/* [What to render] */
part = "all";        // ["body","face","plate","gasket","all"]

/* [Options] */
opt_seal   = true;   // perimeter TPU gasket + drip-edge face (doorbells live outside)
opt_vent   = true;   // GORE vent cluster on the face — ON by default: a sealed outdoor
                     // unit with no pressure path pumps moist air past the seals on
                     // every day/night thermal cycle and the condensate never leaves
opt_led    = false;  // separate light-pipe port (the 12 mm button usually has its own LED ring)
opt_tamper = false;  // reed/Hall magnet pocket on the face underside

/* [Boards] — the stacked-XIAO Vision build. Defaults measured from the
   committed vendor GLB (canary-local/boards/seeed_grove_vision_ai_v2.glb):
   the module PCB is 40 x 20 mm (Grove 1x2 form factor — NOT the 25 x 25 the
   v0.1 bay assumed), mounted VERTICALLY: 40 mm along Y, USB edge down. */
vm_l     = 40.0;     // Grove Vision AI V2 long side (Y; USB edge down) — measured
vm_w     = 20.0;     // module short side (X) — measured
xiao_l   = 21.0;     // XIAO stacked on the module's socket, lower half, centred
xiao_w   = 17.8;
stack_sock_h = 6.5;  // module underside -> XIAO underside when seated (measured 6.2)
vm_front_h   = 5.0;  // module front-side component height (measured)
cam_w    = 25.0;     // OV5647 carrier (Pi-cam v1.3 form)
cam_h    = 24.0;
pcb_t    = 1.0;
board_clear = 0.6;

/* [Camera] — Pi-cam v1.3 posts on the face */
cam_hole_x = 21.0;
cam_hole_y = 12.5;
cam_post_d = 3.6;
cam_post_h = 4.0;
cam_screw_d = 1.6;
lens_dx  = 0.0;      // lens centre offset from the camera-board centre — MEASURE
lens_dy  = 2.5;
cam_ap_d   = 10.0;
cam_disc_d = 14.0;   // clear-disc seat (0 = bare aperture)
cam_disc_t = 1.0;

/* [Button] — 12 mm panel-mount illuminated momentary (short body, IP65) */
btn_d      = 12.0;   // button thread/body diameter (hole = btn_d + 2*tol_slide)
btn_bez_d  = 16.5;   // bezel seat diameter on the face (0 = no seat)
btn_bez_t  = 1.0;    // bezel seat depth
btn_body_l = 18.0;   // body + terminals depth behind the panel — checked against the cavity

/* [Doorbell shell] */
db_r     = 12.0;     // outside corner radius (pill look; <= half the width)
wall_t   = 2.0;      // auto-thickened in seal mode
floor_t  = 2.0;      // back thickness
lid_t    = 2.2;      // face thickness (the visible surface)
lip_h    = 4.0;
lip_t    = 1.2;
cav_extra = 1.0;     // headroom over the tallest component
zone_top = 8.0;      // margin above the camera (top posts live here)
zone_gap = 2.0;      // camera <-> module gap
zone_well = 12.0;    // cable well between module and button (USB plugs live here)
zone_btn = 21.0;     // button zone height
usb_exit_w = 12.0;   // oval cable exit through the back (into the plate hole)
usb_exit_h = 7.0;
usb_exit_dx = 5.0;   // exit offset from the centreline
usb_exit_dy = 6.0;   // exit offset above the well centre — clears the lower T-stud pocket
                     // (the oval's top reaches behind the module: harmless, it's a floor opening)

/* [Print tolerances] */
tol_slide = 0.20;
tol_press = 0.10;
tol_hole  = 0.30;

/* [Engineering] (see README "Engineering & materials") */
screw_insert = false;  // M2 brass heat-set inserts in the corner posts
insert_d     = 3.5;
insert_h     = 4.0;
lid_ribs     = true;   // perimeter rib ring under the face
lid_rib_w    = 2.5;
lid_rib_h    = 1.0;
foot_cham    = 0.5;    // 45° chamfer on the body's back edge

/* [Screw posts] (face screws — use black-oxide M2 for looks) */
post_d       = 5.0;
screw_d      = 1.6;
screw_head_d = 4.0;
screw_head_h = 2.0;

/* [Weather sealing] */
gasket_w      = 1.6;
gasket_groove = 1.2;
gasket_proud  = 0.3;
skirt_h       = 3.0;
skirt_t       = 1.6;

/* [Wall plate + T-stud hooks + security screw] */
plate_t     = 4.0;    // plate thickness at the THIN end
plate_wedge = 0;      // vertical wedge: camera tilts down the approach  // [0:5:15]
plate_wedge_x = 0;    // horizontal wedge: aims left/right (corner installs)  // [-15:5:15]
stud_y      = 34.0;   // T-stud/pocket centres at y = ±stud_y (clear of the cable exit)
kh_head_d   = 7.0;    // pocket head pass (stud head 6.6)
kh_shank_d  = 4.2;    // pocket slot (stud stem 4.0)
kh_slot_l   = 8.0;
kh_head_h   = 3.5;    // pocket depth (face web + head cavity)
kh_face     = 1.0;
kh_extra    = 3.0;    // body back thickening that hosts the pockets
sec_screw_d = 2.2;    // security screw (M2 self-tap; use a Torx/security drive)
plate_screw_d = 4.2;  // wall screws (#8 / M4 countersunk)

/* [Aesthetics] */
lid_edge    = 1.0;    // face edge chamfer
lid_edge2   = 0.8;    // second, steeper stage (~66°) — softens the face edge toward a roundover
label_text  = "";     // debossed face label ("" = off)
label_size  = 4.5;
label_depth = 0.5;
label_dx    = 0.0;
label_dy    = -26.0;
label_rot   = 0;
label_font  = "Liberation Sans:style=Bold";

/* [Front-face features] — offsets from the MODULE centre */
lp_d   = 3.0;
lp_dx  = 8.0;
lp_dy  = -8.0;
vent_pad_d     = 12.0;
vent_pad_depth = 0.8;
vent_hole_d    = 1.6;
vent_ring_d    = 6.0;
vent_holes     = 6;
vent_dx        = 0.0;    // vent/sound cluster ON the face's vertical axis — the
vent_dy        = -8.0;   // camera → grille → button rhythm of a real doorbell
                         // (off-axis it read as an accidental drill pattern)
mag_d  = 6.0;
mag_h  = 3.2;
mag_dx = 8.0;
mag_dy = 8.0;

/* [Board snap clips] */
clip_w      = 6.0;
clip_t      = 1.0;
clip_hook   = 0.5;
clip_hook_h = 1.2;
clip_clear  = 0.25;

/* [Quality] */
// curve quality: $fa/$fs give smooth big arcs (pill corners, hood) without
// exploding tiny holes into thousands of facets like a large $fn would
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry
// ----------------------------------------------------------------------------
e_seal   = opt_seal;
wall_eff = e_seal ? max(wall_t, gasket_w + 1.6) : wall_t;
pd = screw_insert ? max(post_d, insert_d + 2.4) : post_d;
clip_stack  = clip_clear + clip_t;
vm_standoff = stack_sock_h + 1.5;

inner_x = max(cam_w + 2*board_clear, vm_w + 2*(clip_stack + board_clear) + 0.5);
inner_y = zone_btn + zone_well + (vm_l + board_clear) + zone_gap + cam_h + zone_top;
cav_d   = max(vm_standoff + pcb_t + vm_front_h + cav_extra, btn_body_l - lid_t + 1);

out_x  = inner_x + 2*wall_eff;
out_y  = inner_y + 2*wall_eff;
base_d = floor_t + cav_d;
rr     = min(db_r, out_x/2 - 0.1);          // pill radius, clamped to the width

btn_cy  = -inner_y/2 + zone_btn/2;
well_cy = -inner_y/2 + zone_btn + zone_well/2;      // cable well / USB plug space
vm_cy   = -inner_y/2 + zone_btn + zone_well + board_clear + vm_l/2;
cam_cy  = vm_cy + vm_l/2 + zone_gap + cam_h/2;
vm_cx   = 0;  cam_cx = 0;
lens_x  = lens_dx;  lens_y = cam_cy + lens_dy;

// four face-screw posts in the top/bottom margins, inset from the pill corners
function post_xy() = [
    [ inner_x/2 - pd/2 - 1.0,  inner_y/2 - pd/2 - 2.5],
    [-inner_x/2 + pd/2 + 1.0,  inner_y/2 - pd/2 - 2.5],
    [ inner_x/2 - pd/2 - 1.0, -inner_y/2 + pd/2 + 2.5],
    [-inner_x/2 + pd/2 + 1.0, -inner_y/2 + pd/2 + 2.5],
];

skirt_gap = tol_slide + 0.2;
plate_x   = e_seal ? out_x + 2*(skirt_gap + skirt_t) : out_x;
plate_y   = e_seal ? out_y + 2*(skirt_gap + skirt_t) : out_y;
plate_r   = e_seal ? rr + skirt_gap + skirt_t : rr;

assert(btn_bez_d == 0 || btn_bez_d > btn_d + 2, "btn_bez_d must exceed the button hole");
assert(btn_body_l - lid_t + 1 <= cav_d, "button too deep — raise btn_body_l budget or cavity");
assert(zone_btn >= btn_bez_d + 3, "zone_btn too short for the button bezel");
assert(lip_h < cav_d, "lip_h must be less than the cavity depth");
assert(!e_seal || gasket_groove <= lip_h - 0.5, "gasket_groove must stay below the lip");
assert(cam_disc_d == 0 || cam_disc_d > cam_ap_d, "cam_disc_d must exceed cam_ap_d");
assert(kh_head_h + 1.5 <= floor_t + kh_extra, "stud pocket too deep — raise kh_extra");
assert(lid_edge == 0 || (lid_edge >= 0.01 && lid_edge < lid_t), "lid_edge out of range");
assert(lid_edge2 >= 0 && (lid_edge > 0 || lid_edge2 == 0) && lid_edge + lid_edge2 < lid_t,
       "lid_edge2 requires lid_edge > 0, and their sum must stay below lid_t");
assert(abs(plate_wedge_x) <= 15 && plate_wedge <= 15, "keep wedge angles <= 15 degrees");
assert(well_cy + usb_exit_dy - usb_exit_h/2 >= -stud_y + kh_slot_l/2 + (kh_head_d + 0.6)/2 + 0.8,
       "cable exit overlaps the lower T-stud pocket — raise usb_exit_dy or stud_y");
assert(abs(usb_exit_dx) + usb_exit_w/2 <= inner_x/2 - 1, "cable exit too wide/offset for the cavity");
echo(str("Canary Vision DOORBELL v0.2 — body ", out_x, " x ", out_y, " x ", base_d + lid_t + kh_extra,
         " mm + plate ", plate_t, " mm (wedge ", plate_wedge, " deg, seal=", e_seal, ")"));

// ----------------------------------------------------------------------------
//  Helpers (shared idiom with the other Canary enclosures)
// ----------------------------------------------------------------------------
module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module rrect(l, w, r, h) { linear_extrude(height = h) rrect2d(l, w, r); }
module rim_ring2d(w) {
    difference() {
        offset(r =  w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, rr - wall_eff/2));
        offset(r = -w/2) rrect2d(inner_x + wall_eff, inner_y + wall_eff, max(0.1, rr - wall_eff/2));
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
module stud_pocket(yc) {              // blind T-stud pocket; slot toward +Y (body drops on)
    y0 = yc - kh_slot_l/2;  y1 = yc + kh_slot_l/2;  z0 = -kh_extra;
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
module foot_chamfer_cut() {
    z0 = -kh_extra;
    difference() {
        translate([0, 0, z0 - 0.01]) rrect(out_x + 0.04, out_y + 0.04, rr, foot_cham + 0.01);
        hull() {
            translate([0, 0, z0]) rrect(out_x - 2*foot_cham, out_y - 2*foot_cham, max(0.1, rr - foot_cham), 0.01);
            translate([0, 0, z0 + foot_cham]) rrect(out_x, out_y, rr, 0.01);
        }
    }
}

// ----------------------------------------------------------------------------
//  BODY (back shell: board rails, cable exit, stud pockets, security boss)
// ----------------------------------------------------------------------------
module body() {
    posts = post_xy();
    gusset_h = max(2, cav_d - lip_h - 1.0);
    difference() {
    union() {
        difference() {
            union() {
                rrect(out_x, out_y, rr, base_d);
                translate([0, 0, -kh_extra]) rrect(out_x, out_y, rr, kh_extra);  // stud-pocket back
            }
            translate([0, 0, floor_t]) rrect(inner_x, inner_y, max(0.1, rr - wall_eff), cav_d + 1);
            // oval cable exit through the back (aligns with the plate hole; offset
            // sideways/up so it clears the lower T-stud pocket)
            translate([usb_exit_dx, well_cy + usb_exit_dy, 0]) hull()
                for (s = [1, -1]) translate([s*(usb_exit_w - usb_exit_h)/2, 0, -kh_extra - 1])
                    cylinder(d = usb_exit_h, h = kh_extra + floor_t + 2);
            if (e_seal)
                translate([0, 0, base_d - gasket_groove])
                    linear_extrude(gasket_groove + 1) rim_ring2d(gasket_w);
            for (yc = [-stud_y, stud_y]) stud_pocket(yc);
            if (foot_cham > 0) foot_chamfer_cut();
            // Ø1.5 weep at the cavity's lowest point (mounted button-down):
            // condensate drains into the unsealed body/plate interface. Too
            // small to matter for ingress; pressure path is the vent membrane.
            // (offset in X so it stays clear of the security boss + pilot bore)
            translate([8, -inner_y/2 + 2, -0.1]) cylinder(d = 1.5, h = floor_t + 0.2);
        }
        // internal boss backing the security screw (~7 mm thread engagement);
        // kept below z=5 so it clears the button body's tip
        translate([-4, -inner_y/2 - 0.1, 0]) cube([8, 4.1, 5]);
        // face-screw posts, gusseted into the nearest walls
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
                        translate([p[0], sy*(inner_y/2 - 2.3), floor_t]) cylinder(d = 2, h = gusset_h);
                    }
                }
            }
            for (p = posts) translate([p[0], p[1], floor_t + 2.0]) cylinder(d = screw_d, h = cav_d);
            if (screw_insert)
                for (p = posts) translate([p[0], p[1], floor_t + cav_d - insert_h - 0.5])
                    cylinder(d = insert_d - 0.1, h = insert_h + 1);
        }
        // module rails + clips, TOP HALF ONLY — the stacked XIAO (17.8 wide on
        // the 20 mm module) hangs beneath the LOWER half, so full-length side
        // rails would collide with it. Two bottom-corner pins catch the module's
        // lower edge outboard of the XIAO and the down-facing USB ports.
        for (s = [1, -1]) {
            rail_l = vm_l/2 - 4;
            difference() {
                translate([vm_cx + s*(vm_w/2 - 1.5) - 1.5, vm_cy + 2, floor_t])
                    cube([3, rail_l, vm_standoff]);
                translate([vm_cx + s*(vm_w/2 - 1.5), vm_cy + 2 + rail_l/2, floor_t + vm_standoff/2])
                    cube([5, clip_w + 2, vm_standoff + 1], center = true);
            }
            edgeclip(vm_cx + s*vm_w/2, vm_cy + 2 + rail_l/2, s > 0 ? 0 : 180, vm_standoff);
            translate([vm_cx + s*(vm_w/2 + 0.1), vm_cy - vm_l/2 + 1.2, floor_t])
                cylinder(d = 2.0, h = vm_standoff);   // +0.1 outboard: pin inner edge clears the
                                                      // 17.8 mm XIAO, still catches the 20 mm module
        }
    }
    // security-screw pilot, drilled LAST so it passes through wall AND boss,
    // ending blind inside the boss (the seal envelope stays intact)
    translate([0, -out_y/2 - 0.1, 3.0]) rotate([-90, 0, 0]) cylinder(d = sec_screw_d - 0.5, h = 6.5);
    }
}

// ----------------------------------------------------------------------------
//  FACE (lens + disc seat, button bezel, optional vent/LED/label, lip, skirt)
// ----------------------------------------------------------------------------
module vent_cluster(x, y) {
    translate([x, y, 0]) {
        translate([0, 0, lid_t - vent_pad_depth]) cylinder(d = vent_pad_d, h = vent_pad_depth + 1);
        for (i = [0 : vent_holes - 1]) rotate([0, 0, i * 360 / vent_holes])
            translate([vent_ring_d/2, 0, -1]) cylinder(d = vent_hole_d, h = lid_t + 2);
    }
}
module face_plate() {
    // one 45° stage, plus an optional steeper cap stage (~66°) that softens the
    // edge toward a roundover — both print face-down without support
    if (lid_edge > 0) union() {
        rrect(plate_x, plate_y, plate_r, lid_t - lid_edge - lid_edge2);
        hull() {
            translate([0, 0, lid_t - lid_edge - lid_edge2]) rrect(plate_x, plate_y, plate_r, 0.01);
            translate([0, 0, lid_t - lid_edge2 - 0.01])
                rrect(plate_x - 2*lid_edge, plate_y - 2*lid_edge, max(0.1, plate_r - lid_edge), 0.01);
        }
        if (lid_edge2 > 0) hull() {
            translate([0, 0, lid_t - lid_edge2])
                rrect(plate_x - 2*lid_edge, plate_y - 2*lid_edge, max(0.1, plate_r - lid_edge), 0.01);
            translate([0, 0, lid_t - 0.01])
                rrect(plate_x - 2*lid_edge - 0.9*lid_edge2, plate_y - 2*lid_edge - 0.9*lid_edge2,
                      max(0.1, plate_r - lid_edge - 0.45*lid_edge2), 0.01);
        }
    } else rrect(plate_x, plate_y, plate_r, lid_t);
}
module face() {
    union() {
        difference() {
            face_plate();
            translate([lens_x, lens_y, -1]) cylinder(d = cam_ap_d, h = lid_t + 2);
            if (cam_disc_t > 0 && cam_disc_d > 0) {
                translate([lens_x, lens_y, lid_t - (cam_disc_t + 0.2)])
                    cylinder(d = cam_disc_d + 2*tol_slide, h = cam_disc_t + 1);
                // cosmetic 45° lead-in around the seat rim (cleaner edge, easier disc entry)
                translate([lens_x, lens_y, lid_t - 0.4])
                    cylinder(d1 = cam_disc_d + 2*tol_slide, d2 = cam_disc_d + 2*tol_slide + 1.0, h = 0.41);
            }
            // button hole + bezel seat (+ matching lead-in rim)
            translate([0, btn_cy, -1]) cylinder(d = btn_d + 2*tol_slide, h = lid_t + 2);
            if (btn_bez_d > 0) {
                translate([0, btn_cy, lid_t - btn_bez_t])
                    cylinder(d = btn_bez_d + 2*tol_slide, h = btn_bez_t + 1);
                translate([0, btn_cy, lid_t - 0.4])
                    cylinder(d1 = btn_bez_d + 2*tol_slide, d2 = btn_bez_d + 2*tol_slide + 1.0, h = 0.41);
            }
            if (opt_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -1]) cylinder(d = lp_d + 2*tol_press, h = lid_t + 2);
            if (opt_vent) vent_cluster(vm_cx + vent_dx, vm_cy + vent_dy);
            for (p = post_xy()) {
                translate([p[0], p[1], -1]) cylinder(d = screw_d + 2*tol_hole, h = lid_t + 2);
                // flat counterbore — the BOM's PAN-head M2 screws seat flush
                translate([p[0], p[1], lid_t - screw_head_h])
                    cylinder(d = screw_head_d + 2*tol_hole, h = screw_head_h + 0.1);
            }
            if (label_text != "")
                translate([label_dx, label_dy, lid_t - label_depth])
                    linear_extrude(label_depth + 1) rotate(label_rot)
                        text(label_text, size = label_size, font = label_font,
                             halign = "center", valign = "center");
        }
        // perimeter rib ring (t³ stiffening), cleared around features and posts
        if (lid_ribs) {
            ro_x = inner_x - 2*tol_slide - 2*lip_t - 0.8;
            ro_y = inner_y - 2*tol_slide - 2*lip_t - 0.8;
            difference() {
                translate([0, 0, -lid_rib_h]) linear_extrude(lid_rib_h + 0.1)
                    difference() {
                        rrect2d(ro_x, ro_y, max(0.1, rr - wall_eff - lip_t));
                        rrect2d(ro_x - 2*lid_rib_w, ro_y - 2*lid_rib_w, 0.1);
                    }
                for (p = post_xy())
                    translate([p[0], p[1], -lid_rib_h - 0.1]) cylinder(d = pd + 1.6, h = lid_rib_h + 0.2);
                translate([lens_x, lens_y, -lid_rib_h - 0.1])
                    cylinder(d = max(cam_ap_d, cam_disc_d) + 3, h = lid_rib_h + 0.2);
                for (sx = [1, -1], sy = [1, -1])
                    translate([cam_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, -lid_rib_h - 0.1])
                        cylinder(d = cam_post_d + 2, h = lid_rib_h + 0.2);
                translate([0, btn_cy, -lid_rib_h - 0.1])
                    cylinder(d = max(btn_bez_d, btn_d) + 4, h = lid_rib_h + 0.2);
                if (opt_led) translate([vm_cx + lp_dx, vm_cy + lp_dy, -lid_rib_h - 0.1])
                    cylinder(d = lp_d + 4, h = lid_rib_h + 0.2);
                if (opt_vent) translate([vm_cx + vent_dx, vm_cy + vent_dy, -lid_rib_h - 0.1])
                    cylinder(d = vent_pad_d + 3, h = lid_rib_h + 0.2);
                if (opt_tamper) translate([vm_cx + mag_dx, vm_cy + mag_dy, -lid_rib_h - 0.1])
                    cylinder(d = mag_d + 2*tol_press + 4.8, h = lid_rib_h + 0.2);
            }
        }
        // camera posts (Pi-cam v1.3 grid)
        for (sx = [1, -1], sy = [1, -1])
            translate([cam_cx + sx*cam_hole_x/2, cam_cy + sy*cam_hole_y/2, -cam_post_h])
                difference() {
                    cylinder(d = cam_post_d, h = cam_post_h + 0.1);
                    translate([0, 0, -0.1]) cylinder(d = cam_screw_d, h = cam_post_h - 0.8);
                }
        // lip, cleared at posts
        difference() {
            translate([0, 0, -lip_h])
                difference() {
                    rrect(inner_x - 2*tol_slide, inner_y - 2*tol_slide,
                          max(0.1, rr - wall_eff - tol_slide), lip_h);
                    rrect(inner_x - 2*tol_slide - 2*lip_t, inner_y - 2*tol_slide - 2*lip_t, 0.1, lip_h + 1);
                }
            for (p = post_xy())
                translate([p[0], p[1], -lip_h - 0.1]) cylinder(d = pd + 1.2, h = lip_h + 0.2);
        }
        // drip-edge skirt (no notch needed — no wall ports on a doorbell)
        if (e_seal)
            translate([0, 0, -skirt_h]) linear_extrude(skirt_h)
                difference() {
                    rrect2d(plate_x, plate_y, plate_r);
                    rrect2d(out_x + 2*skirt_gap, out_y + 2*skirt_gap, rr + skirt_gap);
                }
        if (opt_tamper)
            translate([vm_cx + mag_dx, vm_cy + mag_dy, -mag_h]) difference() {
                cylinder(d = mag_d + 2*tol_press + 2.4, h = mag_h + 0.1);
                translate([0, 0, -0.1]) cylinder(d = mag_d + 2*tol_press, h = mag_h + 0.1);
            }
    }
}

// ----------------------------------------------------------------------------
//  GASKET
// ----------------------------------------------------------------------------
module gasket() { linear_extrude(gasket_groove + gasket_proud) rim_ring2d(gasket_w - 0.5); }

// ----------------------------------------------------------------------------
//  WALL PLATE — flat or wedge; T-studs the body drops onto; countersunk wall
//  screws; cable pass; bottom L-foot with the security-screw hole.
//  Modelled in print orientation (back on the bed).
// ----------------------------------------------------------------------------
module tstud(yc, zbase) {
    translate([0, yc, zbase]) {
        cylinder(d = 4.0, h = kh_face + 0.4);                       // stem (rides the 4.2 slot)
        translate([0, 0, kh_face + 0.4]) cylinder(d1 = 4.0, d2 = 6.6, h = 1.2);  // 45° under-head
        translate([0, 0, kh_face + 1.6]) cylinder(d = 6.6, h = 0.8);             // head
    }
}
// top-surface height of the (possibly wedged) plate at a given y —
// thin at the bottom (-Y), thick at the top: the camera tilts DOWN toward
// the walk-up, the usual doorbell wedge direction
function plate_z(y) = plate_t + (plate_wedge > 0 ? (y + out_y/2) * tan(plate_wedge) : 0);
// extra height the horizontal wedge adds at the plate edge
function plate_zx() = out_x/2 * tan(abs(plate_wedge_x));

module plate() {
    hmax = plate_z(out_y/2) + 2*plate_zx() + 0.1;   // covers the HIGH side of the x-wedge too
    foot_z = plate_t + kh_extra + 3.0;         // security bore height = body pilot height
    difference() {
        union() {
            // slab with a sloped top: straight prism cut by the wedge plane
            difference() {
                rrect(out_x, out_y, rr, hmax);
                translate([0, -out_y/2, plate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0])
                    translate([-out_x - 1, -1, 0]) cube([2*out_x + 2, out_y + rr + 2, hmax + out_y + out_x]);
            }
            // studs, L-foot and (below) the security bore all live in the TILTED
            // frame so they stay aligned with the body resting on the wedge face
            translate([0, -out_y/2, plate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0]) {
                tstud(out_y/2 - stud_y, -0.2);
                tstud(out_y/2 + stud_y, -0.2);
                // bottom L-foot: sits under the body's bottom wall, carries the security screw
                translate([-6, -4, -plate_t - plate_zx()]) cube([12, 4.5, foot_z + plate_zx() + 4]);
            }
        }
        // wall screws: through-holes + flat counterbores at a CONSTANT 3 mm from
        // the wall side, so standard-length screws work at any wedge angle
        for (sy = [1, -1], sx = [1, -1]) {
            translate([sx*(out_x/2 - 8), sy*(out_y/2 - 14), -0.1])
                cylinder(d = plate_screw_d, h = hmax + 1);
            translate([sx*(out_x/2 - 8), sy*(out_y/2 - 14), 3.0])
                cylinder(d = plate_screw_d + 4.4, h = hmax + 1);
        }
        // cable pass (a roomier match for the body's oval exit)
        translate([usb_exit_dx, well_cy + usb_exit_dy, -0.1]) hull()
            for (s = [1, -1]) translate([s*(usb_exit_w - usb_exit_h)/2, 0, 0])
                cylinder(d = usb_exit_h + 4, h = hmax + 1);
        // security-screw bore: up through the foot into the body's pilot (tilted frame)
        translate([0, -out_y/2, plate_t + plate_zx()]) rotate([plate_wedge, plate_wedge_x, 0])
            translate([0, -4.1, foot_z - plate_t]) rotate([-90, 0, 0])
                cylinder(d = sec_screw_d + 0.4, h = 5);
        // flatten anything the compound wedge tips below the wall plane (z < 0)
        translate([-out_x, -out_y/2 - 12, -10]) cube([2*out_x, out_y + 24, 10]);
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
if      (part == "body")   body();
else if (part == "face")   translate([0, 0, lid_t]) rotate([180, 0, 0]) face();
else if (part == "gasket") { assert(e_seal, "gasket needs opt_seal=true"); gasket(); }
else if (part == "plate")  plate();
else {
    body();
    translate([0, 0, 0]) translate([out_x/2 + plate_x/2 + 10, 0, lid_t]) rotate([180, 0, 0]) face();
    translate([-(out_x + 14), 0, 0]) plate();
    if (e_seal) translate([0, out_y + 12, 0]) gasket();
}
