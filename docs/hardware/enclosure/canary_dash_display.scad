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

/* [Fasteners] — M2 x 8 self-tappers into corner lobes OUTSIDE the cavity
   (bosses inside the frame would collide with the panel's sharp glass corners) */
lob_d = 7.0;  lob_o = 2.2;   // lobe Ø / diagonal offset outboard of the cavity corner
pilot_d = 1.7;  screw_c = 2.4;  cb_d = 4.4;  cb_h = 1.4;

/* [Rear mounts] */
// A true VESA-75 SQUARE cannot fit this panel (the shell is only ~74 mm
// tall) — the back carries a 75 mm HORIZONTAL M4 pair on the centreline
// (matches Waveshare's case-back spacing) plus two through-keyholes.
mnt_pair = 75.0;     // horizontal M4 pair spacing
kh_head_d = 8.0;  kh_shank_d = 4.2;  kh_slot_l = 8.0;   // Ø8 passes a real #6/#8 pan head after FDM shrink

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
cav_t = glass_t + stack_t + 3.0;        // frame interior depth: +3 mm rear clearance so the
                                        // back plate, keyhole screw heads and VESA-arm screw
                                        // tips land in AIR, not on the electronics stack
frame_h = face_t + cav_t;               // frame total height
total_t = frame_h + back_t;
view_l = panel_l - 2*bez_lip;
view_w = panel_w - 2*bez_lip;

assert(bez_lip >= 1.5, "bezel lip < 1.5 mm won't retain the glass");
// lobe must clear the cavity corner (cavity corner radius = 2)
assert(sqrt(2)*(lob_o + 2) >= lob_d/2 + 2 + 0.2,
       "screw lobe intrudes into the panel cavity — raise lob_o or shrink lob_d");
assert(mnt_pair + 8 <= out_l, "M4 pair doesn't fit — grow the shell or shrink mnt_pair");
assert(usb_h <= stack_t, "usb_h exceeds the rear stack depth");
assert(cb_h + 1.0 <= back_t, "counterbore through the back plate");

echo(str("Canary dash display v0.1-dev — ", out_l, " x ", out_w, " x ", total_t,
         " (view ", view_l, " x ", view_w, ")  (IN DEVELOPMENT — MEASURE YOUR PANEL)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
function bosses() = [for (sx = [1, -1], sy = [1, -1])
    [sx*(inner_l/2 + lob_o), sy*(inner_w/2 + lob_o)]];
module outline2d() {                    // shell + corner screw lobes
    rrect2d(out_l, out_w, r_out);
    for (p = bosses()) translate(p) circle(d = lob_d);
}

// ---- frame (z0 = face; print face-down) --------------------------------------
module frame() {
    difference() {
        linear_extrude(frame_h) outline2d();
        // view window through the face
        translate([0, 0, -0.1]) linear_extrude(face_t + 0.2) rrect2d(view_l, view_w, 2);
        // panel cavity behind the face
        translate([0, 0, face_t]) linear_extrude(cav_t + 0.1) rrect2d(inner_l, inner_w, 2);
        // corner reliefs: the panel has SHARP glass corners (see header) and a
        // rounded pocket corner (r2) interferes 0.55 mm — drill the corners out
        for (sx = [1, -1], sy = [1, -1])
            translate([sx*inner_l/2, sy*inner_w/2, face_t])
                cylinder(d = 3.4, h = cav_t + 0.1);
        // USB-C slot in the bottom wall (glass side down = -Y wall), at stack
        // depth — cubes are corner-anchored, so centre them on their _dx
        translate([usb_dx - usb_w/2, -out_w/2 - 0.1, face_t + glass_t + 0.5])
            cube([usb_w, frame_w + 0.2, usb_h]);
        // optional CAN/RS485 terminal opening
        if (term_open)
            translate([term_dx - term_w/2, -out_w/2 - 0.1, face_t + glass_t + 0.5])
                cube([term_w, frame_w + 0.2, term_h]);
        // top-wall chimney vents (pitch 8 keeps all of them inside the shell)
        for (i = [0:vent_n - 1])
            translate([-((vent_n - 1)*8)/2 + i*8 - vent_w/2,
                       out_w/2 - frame_w - 0.1, face_t + glass_t + 1])
                cube([vent_w, frame_w + 0.2, stack_t - 2]);
        // M2 pilots down into the lobes
        for (p = bosses())
            translate([p[0], p[1], frame_h - 6]) cylinder(d = pilot_d, h = 6.1);
    }
}

// ---- back (z0 = outer face; print outer-face-down) ---------------------------
module back() {
    difference() {
        linear_extrude(back_t) outline2d();
        // screw counterbores + clearance (screws from the back into the bosses)
        for (p = bosses()) translate([p[0], p[1], 0]) {
            translate([0, 0, -0.1]) cylinder(d = cb_d, h = cb_h + 0.1);
            translate([0, 0, -0.1]) cylinder(d = screw_c, h = back_t + 0.2);
        }
        // 75 mm horizontal M4 pair on the centreline. NOTE: arm screws max
        // M4 x (arm plate + 2.4) — longer tips enter the cavity; the +3 mm
        // rear clearance in cav_t is the buffer, not an invitation
        for (sx = [1, -1])
            translate([sx*mnt_pair/2, 0, -0.1]) cylinder(d = 4.5, h = back_t + 0.2);
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
    chan_back = -stand_d/2 + 16 + chan_w;      // back-rail front face
    // fin position DERIVED so its tilted front face contains the plane the
    // display's back face lies in when its bottom rear edge sits at the rail
    // (0.8 lands the face on that plane within 0.01 mm; the old 0.6 left the
    // fin 0.17 mm proud of it — a real, if tiny, interference fit)
    fin_y = chan_back + 4*cos(stand_ang) + 0.8;
    linear_extrude(stand_t) rrect2d(stand_w, stand_d, 6);          // base
    // back rest fin, leaning stand_ang from vertical (corner embeds in base)
    translate([0, fin_y, stand_t - 0.01])
        rotate([-stand_ang, 0, 0]) translate([-stand_w/2 + 15, -4, 0])
            cube([stand_w - 30, 8, 42]);
    // front lip + back rail form the bottom-edge channel (additive — the
    // subtractive tilted groove cut the base into two shells)
    translate([-stand_w/2 + 15, -stand_d/2 + 16 - 3, stand_t - 0.01])
        cube([stand_w - 30, 3, 8]);
    // back rail: a reclined panel's back plane retreats tan(ang) per mm of
    // height, so a rail flush at the panel's bottom edge digs its top corner
    // ~h·tan(ang) into the glass-back. Set the rail back by exactly that
    // (+0.4 clearance) so it backstops the edge without touching the panel.
    // ⚠️ regenerate enclosures/preview/canary_dash_display_stand.stl.
    translate([-stand_w/2 + 15, chan_back + 6*tan(stand_ang) + 0.4, stand_t - 0.01])
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
