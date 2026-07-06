// ============================================================================
//  Canary — DASHBOARD DISPLAY CASE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Housing for the Waveshare ESP32-S3-Touch-LCD-4.3 — the "step-up dashboard"
//  option from docs/hardware/display_research.md (Option B): a 4.3" 800x480
//  IPS touch panel for an event-timeline view, vs the Watch Station's glance
//  puck. Three parts:
//
//    frame — front bezel shell; the panel drops in face-first, the bezel lip
//            overlaps the glass 2.5 mm all round (prints face-down: the
//            A-surface is your textured build plate)
//    back  — vented rear cover, M2 self-tap corner bosses; carries TWO
//            through-keyholes AND a 75 mm horizontal M4 pair (a VESA-75
//            square can't fit this panel height)
//    stand — free-standing desk cradle, 25 deg recline, no hardware
//
//  ⚠️ DIMENSIONS ARE NOMINAL — Waveshare doesn't publish a full mechanical
//  drawing. MEASURE your board (glass outline, rear stack depth, USB
//  position) before printing. USB-C exits the bottom wall; the CAN/RS485
//  terminal zone gets an optional opening.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

/* [What to render] */
part = "all";        // ["frame","back","stand","all"]

/* [Panel] — Waveshare ESP32-S3-Touch-LCD-4.3. MEASURE YOURS */
panel_l = 106.3;     // glass outline X — MEASURE
panel_w = 66.2;      // glass outline Y — MEASURE
glass_t = 3.2;       // glass + panel module thickness — MEASURE
stack_t = 8.0;       // rear stack: PCB + connectors behind the glass — MEASURE
bez_lip = 2.5;       // bezel overlap onto the glass edge

/* [USB / terminals] — positions along the BOTTOM wall, from panel centre */
usb_dx   = 0.0;      // USB-C slot centre offset — MEASURE
usb_w    = 12.0;  usb_h = 6.5;
term_open = false;   // also open the CAN/RS485 terminal zone
term_dx  = -30.0;  term_w = 24.0;  term_h = 8.0;

/* [Print tolerances] — tune with canary_fit_coupon.scad */
tol_slide = 0.20;  tol_press = 0.10;  tol_hole = 0.30;

/* [Shell] */
frame_w = 3.5;       // side wall thickness
face_t  = 2.4;       // bezel face thickness
back_t  = 2.4;
r_out   = 5.0;

/* [Fasteners] — M2 x 8 self-tappers into corner bosses */
boss_d = 6.0;  pilot_d = 1.7;  screw_c = 2.4;  cb_d = 4.4;  cb_h = 1.4;

/* [Rear mounts] */
// A true VESA-75 SQUARE cannot fit this panel (the shell is only ~74 mm
// tall) — the back carries a 75 mm HORIZONTAL M4 pair on the centreline
// (matches Waveshare's case-back spacing) plus two through-keyholes.
mnt_pair = 75.0;     // horizontal M4 pair spacing
kh_head_d = 7.0;  kh_shank_d = 4.2;  kh_slot_l = 8.0;

/* [Vents] */
vent_n = 8;  vent_w = 1.2;  vent_l = 16.0;

/* [Stand] */
stand_ang = 25;      // recline
stand_w = 120.0;  stand_d = 78.0;  stand_t = 4.0;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ---- derived ----------------------------------------------------------------
inner_l = panel_l + 2*tol_slide;
inner_w = panel_w + 2*tol_slide;
out_l = inner_l + 2*frame_w;
out_w = inner_w + 2*frame_w;
cav_t = glass_t + stack_t;              // frame interior depth
frame_h = face_t + cav_t;               // frame total height
total_t = frame_h + back_t;
view_l = panel_l - 2*bez_lip;
view_w = panel_w - 2*bez_lip;
boss_off = frame_w + boss_d/2 - 1.0;    // boss centre inset from outer edge

assert(bez_lip >= 1.5, "bezel lip < 1.5 mm won't retain the glass");
assert(mnt_pair + 8 <= out_l, "M4 pair doesn't fit — grow the shell or shrink mnt_pair");
assert(usb_h <= stack_t, "usb_h exceeds the rear stack depth");
assert(cb_h + 1.0 <= back_t, "counterbore through the back plate");

echo(str("Canary dash display v0.1-dev — ", out_l, " x ", out_w, " x ", total_t,
         " (view ", view_l, " x ", view_w, ")  (IN DEVELOPMENT — MEASURE YOUR PANEL)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
function bosses() = [for (sx = [1, -1], sy = [1, -1])
    [sx*(out_l/2 - boss_off), sy*(out_w/2 - boss_off)]];

// ---- frame (z0 = face; print face-down) --------------------------------------
module frame() {
    difference() {
        linear_extrude(frame_h) rrect2d(out_l, out_w, r_out);
        // view window through the face
        translate([0, 0, -0.1]) linear_extrude(face_t + 0.2) rrect2d(view_l, view_w, 2);
        // panel cavity behind the face
        translate([0, 0, face_t]) linear_extrude(cav_t + 0.1) rrect2d(inner_l, inner_w, 2);
        // USB-C slot in the bottom wall (glass side down = -Y wall), at stack depth
        translate([usb_dx, -out_w/2 - 0.1, face_t + glass_t + 0.5])
            cube([usb_w, frame_w + 0.2, usb_h]);
        // optional CAN/RS485 terminal opening
        if (term_open)
            translate([term_dx, -out_w/2 - 0.1, face_t + glass_t + 0.5])
                cube([term_w, frame_w + 0.2, term_h]);
        // top-wall chimney vents
        for (i = [0:vent_n - 1])
            translate([-((vent_n - 1)*vent_l*1.4)/2 + i*vent_l*1.4 - vent_w/2,
                       out_w/2 - frame_w - 0.1, face_t + glass_t + 1])
                cube([vent_w, frame_w + 0.2, stack_t - 2]);
    }
    // corner bosses (M2 self-tap), fused to the walls
    for (p = bosses()) translate([p[0], p[1], face_t - 0.01]) difference() {
        cylinder(d = boss_d, h = cav_t + 0.01);
        translate([0, 0, cav_t - 6]) cylinder(d = pilot_d, h = 6.1);
    }
}

// ---- back (z0 = outer face; print outer-face-down) ---------------------------
module back() {
    difference() {
        linear_extrude(back_t) rrect2d(out_l, out_w, r_out);
        // screw counterbores + clearance (screws from the back into the bosses)
        for (p = bosses()) translate([p[0], p[1], 0]) {
            translate([0, 0, -0.1]) cylinder(d = cb_d, h = cb_h + 0.1);
            translate([0, 0, -0.1]) cylinder(d = screw_c, h = back_t + 0.2);
        }
        // 75 mm horizontal M4 pair on the centreline
        for (sx = [1, -1])
            translate([sx*mnt_pair/2, 0, -0.1]) cylinder(d = 4.4, h = back_t + 0.2);
        // two through-keyholes on the vertical centreline (hang direction: up)
        for (yc = [18, -18]) translate([0, yc, 0]) {
            translate([0, -kh_slot_l/2, -0.1]) cylinder(d = kh_head_d, h = back_t + 0.2);
            translate([0, 0, -0.1]) linear_extrude(back_t + 0.2) hull() {
                translate([0, -kh_slot_l/2]) circle(d = kh_shank_d);
                translate([0,  kh_slot_l/2]) circle(d = kh_shank_d);
            }
        }
        // vent slots (top third, mirrored bottom third)
        for (i = [0:vent_n - 1], sy = [1, -1])
            translate([-((vent_n - 1)*8)/2 + i*8 - vent_w/2, sy*(out_w/2 - 12) - vent_l/2, -0.1])
                cube([vent_w, vent_l, back_t + 0.2]);
    }
}

// ---- stand (free-standing cradle; prints flat) -------------------------------
module stand() {
    chan_w = total_t + 2.0;   // channel for the tilted display's bottom edge
    linear_extrude(stand_t) rrect2d(stand_w, stand_d, 6);          // base
    // back rest fin, leaning stand_ang from vertical (corner embeds in base)
    translate([0, stand_d/2 - 12, stand_t - 0.01])
        rotate([-stand_ang, 0, 0]) translate([-stand_w/2 + 15, -4, 0])
            cube([stand_w - 30, 8, 42]);
    // front lip + back rail form the bottom-edge channel (additive — the
    // subtractive tilted groove cut the base into two shells)
    translate([-stand_w/2 + 15, -stand_d/2 + 16 - 3, stand_t - 0.01])
        cube([stand_w - 30, 3, 8]);
    translate([-stand_w/2 + 15, -stand_d/2 + 16 + chan_w, stand_t - 0.01])
        cube([stand_w - 30, 3, 6]);
}

// ---- selector -----------------------------------------------------------------
if      (part == "frame") frame();
else if (part == "back")  back();
else if (part == "stand") stand();
else {
    frame();
    translate([0, out_w + 14, 0]) back();
    translate([0, -(out_w/2 + stand_d/2 + 16), 0]) stand();
}
