// ============================================================================
//  Canary — ESP32-C6-LCD DISPLAY CASE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  A pocket portrait-display witness for the Waveshare ESP32-C6-LCD boards —
//  our newest firmware-display target. This file covers the FLAT-BOTTOM board
//  (bare, NO male header pins soldered pointing down): the PCB back is close
//  to flat, so the case can be shallow and the board is captured by its edges,
//  not by screwing into its M2 holes.
//
//  Two printed parts:
//    bezel — front frame; the board drops in glass-first, and the face
//            overlaps the LCD module border (the active-area window is what you
//            see). Prints FACE-DOWN (the A-surface is your build plate).
//    back  — rear snap cover; a perimeter skirt clicks into the bezel walls
//            (4 nubs, no fasteners) and four corner standoffs press the PCB
//            forward against the bezel's glass ledge. A blind keyhole
//            wall-mounts it.
//
//  Model presets are parametric — `model = "1.47"` is dimensioned from the
//  Waveshare mechanical drawing; the 1.69 preset lands when its drawing does.
//
//  Orientation: +Y = up (portrait), USB-C exits the BOTTOM (−Y) short wall,
//  +Z = toward the glass. All parts print flat, no supports.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated. Measure the
//     LCD stack height and the BOOT/RST button positions before printing.
// ============================================================================

/* [What to render] */
part  = "all";      // ["bezel","back","all"]
model = "1.47";     // ["1.47","1.69"]  board preset

/* [Board] — ESP32-C6-LCD-1.47, flat-bottom. From the Waveshare drawing (mm). */
// long axis (portrait height, Y), short axis (X), PCB thickness
board_l = (model == "1.69") ? 36.0  : 36.37;   // 1.69 = MEASURE (placeholder)
board_w = (model == "1.69") ? 25.0  : 20.32;
pcb_t   = 1.45;
lcd_rise   = 3.65;   // LCD glass front above the PCB front face (5.10 stack − 1.45 PCB)
back_stack = 2.2;    // back-side component clearance below the PCB — MEASURE

/* [Screen] — active area = the window; the LCD module border sits under the lip */
aa_l  = (model == "1.69") ? 27.972 : 32.35;    // active-area long (Y) — 1.69 = MEASURE
aa_w  = (model == "1.69") ? 27.972 : 17.39;    // active-area short (X)
lcm_l = (model == "1.69") ? 32.0   : 36.28;    // LCD module outline long
lcm_w = (model == "1.69") ? 28.0   : 19.39;    // LCD module outline short

/* [USB-C] — on the BOTTOM (−Y) short wall, centred on the PCB thickness */
usb_w  = 9.2;   usb_h = 3.6;   usb_dx = 0.0;    // MEASURE your connector

/* [Buttons] — BOOT/RST flank the USB end on the two side (±X) walls */
opt_btn = true;
btn_d   = 3.6;                 // side access hole Ø
btn_up  = 6.5;                 // button centre up from the USB (−Y) end — MEASURE

/* [Mount] */
opt_keyhole = true;            // one blind keyhole in the back (wall hang)
kh_head_d = 7.0; kh_shank_d = 4.2; kh_slot_l = 7.0; kh_head_h = 3.0; kh_face = 1.0;

/* [Ventilation] — let the backlight/regulator heat convect out (side slots +
   a back grille). Even this small board runs warm on full brightness. */
opt_vent = true;
vent_n = 5;          // slots per side wall
vent_pitch = 5.0;    // slot spacing
vent_w = 1.4;        // slot width

/* [Print tolerances] — tune with canary_fit_coupon.scad */
tol_slide = 0.20; tol_press = 0.10; tol_hole = 0.30;

/* [Shell] */
wall   = 2.2;    // side wall thickness
face_t = 2.0;    // bezel face (over the glass border)
back_t = 2.0;    // rear cover plate
r_out  = 3.0;    // outer corner radius
lid_edge = 0.8;  // bezel face edge chamfer

/* [Snap fit] — back skirt into the bezel walls.
   skirt_dep must NOT exceed back_stack: the cavity is only board+2·tol_slide
   wide (~0.2 mm/side over the PCB), so the skirt rides directly behind the PCB
   edge and may only reach the PCB back plane — any deeper and it drives into
   the board and the cover can't seat. The nub still lands on it as long as
   back_t + skirt_dep ≥ snap_depth. */
snap_n = 4; snap_w = 5.0; snap_h = 1.6; snap_depth = 2.6; snap_proud = 0.5; skirt_dep = back_stack;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry.  Axis convention: X = short (board_w), Y = long (board_l,
//  portrait height), Z = through-thickness (toward the glass).
// ----------------------------------------------------------------------------
xc = board_w + 2*tol_slide;   yc = board_l + 2*tol_slide;   // board cavity (X,Y)
xo = xc + 2*wall;             yo = yc + 2*wall;             // outer (X,Y)
cav_d = lcd_rise + pcb_t + back_stack;           // glass ledge → back-plate inner
bez_h = face_t + cav_d;                          // bezel wall height
r_in  = max(0.6, r_out - wall);                  // cavity corner radius

z_pcb_front = face_t + lcd_rise;                 // PCB front plane
z_usb       = face_t + lcd_rise + pcb_t/2;       // USB-C centre (mid-PCB)

// active-area window vs module: the face overlaps the module border by
// (module − AA)/2 per side; that overlap retains the glass
lip_l = (lcm_l - aa_l)/2;   // long-side lip (Y)
lip_w = (lcm_w - aa_w)/2;   // short-side lip (X)

assert(aa_l < lcm_l && aa_w < lcm_w, "active area must be inside the module outline");
assert(lip_w >= 0.8, "short-side lip < 0.8 mm won't retain the glass — check aa_w/lcm_w");
assert(lcm_l <= yc && lcm_w <= xc, "LCD module larger than the board cavity — check dims");
assert(usb_h <= pcb_t + 2*lcd_rise, "USB slot taller than the front stack — check usb_h");
assert(skirt_dep <= back_stack + 0.01, "skirt_dep > back_stack — the skirt would drive into the PCB; cap it at back_stack");
assert(back_t + skirt_dep >= snap_depth + snap_h/2, "skirt too short to carry the snap nub — raise skirt_dep or lower snap_depth");
echo(str("Canary C6 display (", model, ") v0.1-dev — outer ", xo, " x ", yo,
         " x ", bez_h + back_t, " mm, window ", aa_w, " x ", aa_l,
         " (lip X ", lip_w, " / Y ", lip_l, ")  (IN DEVELOPMENT — MEASURE)"));

module rrect2d(x, y, r) { offset(r = r) offset(r = -r) square([x, y], center = true); }

// snap windows / nubs live on the two long (±X) side walls, near each end
skirt_x = xc - 2*tol_press;   skirt_y = yc - 2*tol_press;   skirt_wall = 1.6;
function nub_ys() = [-board_l/4, board_l/4];

// ----------------------------------------------------------------------------
//  BEZEL — front frame, prints face-down (z0 = outer face)
// ----------------------------------------------------------------------------
module bezel() {
    difference() {
        linear_extrude(bez_h) rrect2d(xo, yo, r_out);                 // face + walls
        // active-area window through the face (aa_w = X, aa_l = Y)
        translate([0, 0, -0.1]) linear_extrude(face_t + 0.2) rrect2d(aa_w, aa_l, 1.5);
        // face edge chamfer (lead-in on the viewer side)
        if (lid_edge > 0)
            translate([0, 0, -0.01]) linear_extrude(lid_edge + 0.01)
                difference() {
                    rrect2d(xo + 0.1, yo + 0.1, r_out);
                    offset(delta = -lid_edge) rrect2d(xo, yo, r_out);
                }
        // board cavity behind the glass ledge
        translate([0, 0, face_t]) linear_extrude(cav_d + 0.2) rrect2d(xc, yc, r_in);
        // USB-C slot in the bottom (−Y) short wall
        translate([usb_dx, -yo/2, z_usb]) cube([usb_w, wall*3, usb_h], center = true);
        // BOOT / RST side access holes near the USB end (both ±X side walls)
        if (opt_btn) for (sx = [1, -1])
            translate([sx*xo/2, -board_l/2 + btn_up, z_pcb_front + 0.5])
                rotate([0, 90, 0]) cylinder(d = btn_d, h = wall*3, center = true);
        // heat-escape slots: a row low on each side wall (convection in the
        // bottom, out the top) — kept over the cavity, clear of the ledge
        if (opt_vent) for (sx = [1, -1], i = [0:vent_n-1])
            translate([sx*xo/2, -(vent_n-1)*vent_pitch/2 + i*vent_pitch, face_t + cav_d/2])
                cube([wall*3, vent_w, cav_d*0.6], center = true);
        // snap windows in the side walls (the back's nubs click in here)
        for (sx = [1, -1], yc0 = nub_ys())
            translate([sx*xo/2, yc0, bez_h - snap_depth])
                cube([wall*3, snap_w, snap_h], center = true);
    }
}
module bezel_print() { bezel(); }   // already in print (face-down) orientation

// ----------------------------------------------------------------------------
//  BACK — rear snap cover, prints outer-face-down (z0 = outer back face).
//  Skirt inserts into the bezel cavity; nubs sit at back-z = snap_depth so
//  they meet the bezel wall windows (bezel-z = bez_h − back-z). Nubs and the
//  vent grille run on the short (±X) skirt faces / the back plate.
// ----------------------------------------------------------------------------
module back() {
    difference() {
        union() {
            linear_extrude(back_t) rrect2d(xo, yo, r_out);                  // plate
            translate([0, 0, back_t - 0.01]) linear_extrude(skirt_dep)      // skirt ring
                difference() {
                    rrect2d(skirt_x, skirt_y, r_in);
                    rrect2d(skirt_x - 2*skirt_wall, skirt_y - 2*skirt_wall, max(0.4, r_in - skirt_wall));
                }
            // corner standoffs press the PCB forward onto the bezel glass ledge
            for (sx = [1, -1], sy = [1, -1])
                translate([sx*(board_w/2 - 2.6), sy*(board_l/2 - 2.6), back_t - 0.01])
                    cylinder(d = 4.2, h = back_stack + 0.01);
            // snap nubs on the short-wall (±X) skirt faces, chamfered both ways
            for (sx = [1, -1], yc0 = nub_ys())
                translate([sx*(skirt_x/2 - 0.3), yc0, snap_depth]) hull() {
                    for (dy = [-snap_w/2 + 1.0, snap_w/2 - 1.0])
                        translate([0, dy, 0]) cube([0.6, 0.1, snap_h - 0.4], center = true);
                    translate([sx*(snap_proud + 0.3), 0, 0]) cube([0.1, 0.1, 0.6], center = true);
                }
        }
        // heat-escape grille in the back plate, over the component zone
        if (opt_vent) for (i = [0:vent_n], sx = [1, -1])
            translate([sx*3.2, -(vent_n)*vent_pitch/2 + i*vent_pitch, -0.1])
                cube([2.0, vent_w, back_t + 0.2], center = false);
        // through keyhole in the back plate (wall hang; slot toward +Y/up):
        // head hole passes the screw head, slot captures the shank as it slides
        if (opt_keyhole) translate([0, 0, -0.1]) linear_extrude(back_t + 0.2) {
            translate([0, -kh_slot_l/2]) circle(d = kh_head_d);
            hull() {
                translate([0, -kh_slot_l/2]) circle(d = kh_shank_d);
                translate([0,  kh_slot_l/2]) circle(d = kh_shank_d);
            }
        }
    }
}

// ----------------------------------------------------------------------------
if      (part == "bezel") bezel_print();
else if (part == "back")  back();
else {
    bezel_print();
    translate([xo + 10, 0, 0]) back();
}
