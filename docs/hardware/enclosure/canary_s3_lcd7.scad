// ============================================================================
//  Canary — 7" TOUCH DASHBOARD CASE  ⚠️ IN DEVELOPMENT (v0.1-dev)
// @env env="indoor; runs hot → print in PETG/ASA"
//  Housing for the Waveshare ESP32-S3-Touch-LCD-7 (7" 800x480 IPS capacitive
//  touch, ESP32-S3, CAN/RS485/battery). The big-panel "wall dashboard" — the
//  household event timeline on a glass slab, vs the Dash 4.3" and the C6 pocket
//  displays.
//
//    bezel — front frame; the bonded touch-glass slab (192.96 x 110.76) drops
//            in face-first, the lip overlaps its border and the active-area
//            window (154.88 x 86.72) is what you see. Prints FACE-DOWN.
//    back  — deep vented rear tray; the PCB rests on moulded standoffs, and the
//            bezel screws to four outboard M3 corner lobes. A LARGE convection
//            grille (this panel's backlight + ESP32-S3 + regulators run hot —
//            heat MUST escape) plus a bottom-edge connector channel and side
//            openings for USB-C / UART / CAN / RS485 / battery.
//    stand — free-standing desk cradle, reclined, no hardware (optional).
//
//  Heat: the back grille is sized for real convection — cool air in the low
//  slots, hot air out the top. Do NOT print this in PLA for a hot-running
//  panel; PETG/ASA. Keep the grille clear.
//
//  ⚠️ CONNECTOR POSITIONS ARE NOMINAL — Waveshare's drawing dimensions the
//  glass + mount holes precisely but not every connector centre. MEASURE the
//  USB-C / UART / CAN / RS485 / battery positions on YOUR Rev before printing.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//  Orientation: landscape, +X = width, +Y = up, +Z = toward the glass.
// ============================================================================

/* [What to render] */
part = "all";        // ["bezel","back","stand","all"]

/* [Glass slab] — bonded touch panel, from the Waveshare drawing (mm) */
glass_w = 192.96;    // touch-glass width  (X)
glass_h = 110.76;    // touch-glass height (Y)
glass_t = 4.0;       // glass + LCD module thickness at the edge — MEASURE
aa_w = 154.88;       // active area width
aa_h = 86.72;        // active area height
aa_dy = -1.02;       // AA centre offset from glass centre (top border 13.04, bottom 11.00)

/* [PCB stack behind the glass] */
pcb_standoff = 5.0;  // glass back → PCB front (the white M3 standoffs) — MEASURE
pcb_t   = 1.6;
comp_h  = 11.0;      // tallest thing behind the PCB (USB-C / terminals / batt JST) — MEASURE
pcb_w   = 165.72;    // PCB outline width  (X) — from the drawing
pcb_h   = 97.60;     // PCB outline height (Y)

/* [Board mounts] — 4x M3 corner standoffs carry the PCB (nominal pattern) */
m3_dx = 165.72;      // M3 hole pattern width  — MEASURE (drawing: 165.72 span)
m3_dy = 88.0;        // M3 hole pattern height — MEASURE
m3_pilot = 2.7;      // self-tap pilot for M3 into printed bosses

/* [Screen] */
bez_lip = 3.0;       // bezel overlap onto the glass border (big slab → wider lip)

/* [Connectors] — openings, offsets from the board centre along their wall.
   NOMINAL — MEASURE. The bottom (−Y) wall carries the USB/UART cluster; the
   two short (±X) walls carry CAN/RS485/battery. Set opt_* to open them. */
opt_bottom_ports = true;
bottom_open_w = 96.0;   bottom_open_dx = 0.0;   // wide channel over the bottom cluster
opt_side_ports = true;
side_open_h = 40.0;     side_open_dy = 0.0;     // tall slot on each short wall

/* [Ventilation] — MUST let the backlight/SoC heat convect out */
vent_back = true;        // large grille in the back plate
vent_rows = 11;          // grille rows
vent_cols = 22;          // grille columns
vent_slot_w = 2.4;       // slot width
vent_slot_l = 9.0;       // slot length
vent_pitch_x = 7.0;      // column pitch
vent_pitch_y = 8.0;      // row pitch
vent_sides = true;       // extra chimney slots high on the side walls
vent_side_n = 6;

/* [Print tolerances] */
tol_slide = 0.25; tol_press = 0.12; tol_hole = 0.35;

/* [Shell] */
wall   = 3.0;    // side wall thickness (big case → thicker)
face_t = 3.0;    // bezel face
back_t = 3.0;    // rear tray floor
r_out  = 6.0;    // outer corner radius

/* [Fasteners] — M3 x 16–20 from the FRONT, through the bezel lobes, self-tapping
   into FULL-HEIGHT corner posts in the back tray (the posts bridge the whole
   cavity, so the screw threads into solid material the entire way — not across
   an empty gap). Heads sit in a counterbore on the bezel's outboard ears,
   clear of the glass. */
lob_d = 9.0;  lob_o = 3.0;   // lobe Ø / diagonal offset outboard of the cavity corner
lob_pilot = 2.7;  screw_c = 3.4;  cb_d = 6.0;  cb_h = 2.0;

/* [Stand] */
opt_stand = true;
stand_ang = 20;  stand_w = 210.0;  stand_d = 120.0;  stand_t = 5.0;

/* [Quality] */
$fa = 3; $fs = 0.5;

// ----------------------------------------------------------------------------
//  Derived. Axis: X = width, Y = height, Z = toward glass.
// ----------------------------------------------------------------------------
xc = glass_w + 2*tol_slide;   yc = glass_h + 2*tol_slide;   // glass cavity
xo = xc + 2*wall;             yo = yc + 2*wall;             // outer
cav_d = glass_t + pcb_standoff + pcb_t + comp_h;            // glass front ledge → back floor
bez_h = face_t + glass_t + 1.0;   // bezel just retains the glass (the deep box is the back)
r_in  = max(1.0, r_out - wall);

z_pcb_front = glass_t + pcb_standoff;   // from the glass ledge (back frame)
view_w = aa_w;  view_h = aa_h;

assert(bez_lip >= 2.0, "bezel lip < 2 mm won't retain a 7in slab");
assert(aa_w < glass_w && aa_h < glass_h, "active area must sit inside the glass");
assert(m3_dx + lob_d < xo && m3_dy + lob_d < yo, "M3 pattern doesn't fit the shell");
assert(cb_h + 1.0 <= bez_h, "counterbore deeper than the bezel ear");
echo(str("Canary 7in touch v0.1-dev — outer ", xo, " x ", yo, " x ", bez_h + cav_d + back_t,
         " mm, window ", view_w, " x ", view_h, ", vent area ~",
         round(vent_back ? vent_rows*vent_cols*vent_slot_w*vent_slot_l/100 : 0), " cm2",
         "  (IN DEVELOPMENT — MEASURE CONNECTORS)"));

module rrect2d(x, y, r) { offset(r = r) offset(r = -r) square([x, y], center = true); }
function lobes() = [for (sx = [1,-1], sy = [1,-1]) [sx*(xc/2 + lob_o), sy*(yc/2 + lob_o)]];
// A corner ear is HULLED into the shell body (two anchor points along the two
// nearest edges) so a solid gusset web holds the M3 boss on — not a thin neck.
module corner_ear(sx, sy) {
    hull() {
        translate([sx*(xc/2 + lob_o), sy*(yc/2 + lob_o)]) circle(d = lob_d);
        translate([sx*(xo/2 - r_out - 1),  sy*(yo/2 - r_out - 10)]) circle(d = 4);
        translate([sx*(xo/2 - r_out - 10), sy*(yo/2 - r_out - 1)])  circle(d = 4);
    }
}
module outline2d() {
    rrect2d(xo, yo, r_out);
    for (sx = [1,-1], sy = [1,-1]) corner_ear(sx, sy);
}

// ----------------------------------------------------------------------------
//  BEZEL — front frame, prints face-down (z0 = outer face)
// ----------------------------------------------------------------------------
module bezel() {
    difference() {
        linear_extrude(bez_h) outline2d();
        // active-area window (offset aa_dy)
        translate([0, aa_dy, -0.1]) linear_extrude(face_t + 0.2) rrect2d(aa_w, aa_h, 3);
        // glass cavity behind the face ledge
        translate([0, 0, face_t]) linear_extrude(bez_h) rrect2d(xc, yc, r_in);
        // M3 clearance through each ear + a head counterbore on the front face
        // (the screw threads into the back tray's full-height corner post)
        for (p = lobes()) translate([p[0], p[1], -0.1]) {
            cylinder(d = cb_d,    h = cb_h + 0.1);      // head counterbore (front)
            cylinder(d = screw_c, h = bez_h + 0.2);     // shank clearance through
        }
    }
}
module bezel_print() { bezel(); }

// ----------------------------------------------------------------------------
//  BACK — deep vented rear tray, prints outer-face-down (z0 = outer back face)
// ----------------------------------------------------------------------------
module vent_grille() {
    // staggered slot grille centred on the back plate
    for (r = [0:vent_rows-1], c = [0:vent_cols-1]) {
        x = (c - (vent_cols-1)/2) * vent_pitch_x;
        y = (r - (vent_rows-1)/2) * vent_pitch_y;
        // keep the grille inside the PCB footprint and off the lobes
        if (abs(x) < pcb_w/2 - 6 && abs(y) < pcb_h/2 - 6)
            translate([x, y, -0.1]) linear_extrude(back_t + 0.2) hull()
                for (dy = [-(vent_slot_l - vent_slot_w)/2, (vent_slot_l - vent_slot_w)/2])
                    translate([0, dy]) circle(d = vent_slot_w);
    }
}
module back() {
    total_d = cav_d + back_t;   // full tray depth (floor + cavity to glass ledge)
    difference() {
        union() {
            linear_extrude(back_t) outline2d();                       // floor + lobes
            // side walls + FULL-HEIGHT corner posts up to meet the bezel
            // (outline2d, not rrect2d(xo,yo): the corner ears run the whole
            // cavity so the case screw threads into solid material all the way)
            translate([0, 0, back_t - 0.01]) linear_extrude(cav_d + 0.01)
                difference() { outline2d(); rrect2d(xc, yc, r_in); }
            // PCB standoff bosses at the M3 pattern
            for (sx = [1,-1], sy = [1,-1])
                translate([sx*m3_dx/2, sy*m3_dy/2, back_t - 0.01])
                    cylinder(d = 7.0, h = comp_h);
        }
        // M3 boss pilots
        for (sx = [1,-1], sy = [1,-1])
            translate([sx*m3_dx/2, sy*m3_dy/2, back_t + comp_h - 6]) cylinder(d = m3_pilot, h = 6.2);
        // case-screw self-tap pilots down the TOP of each full-height corner post
        // (the bezel's counterbored ear delivers the M3 into these)
        for (p = lobes())
            translate([p[0], p[1], back_t + cav_d - 12]) cylinder(d = lob_pilot, h = 12.1);
        // HEAT: back grille
        if (vent_back) vent_grille();
        // bottom-edge connector channel (−Y wall)
        if (opt_bottom_ports)
            translate([bottom_open_dx, -yo/2, back_t + z_pcb_front + pcb_t/2])
                cube([bottom_open_w, wall*3, comp_h + 2], center = true);
        // side connector slots (±X short walls)
        if (opt_side_ports) for (sx = [1,-1])
            translate([sx*xo/2, side_open_dy, back_t + z_pcb_front + pcb_t/2])
                cube([wall*3, side_open_h, comp_h + 2], center = true);
        // side chimney vents high on the ±X walls
        if (vent_sides) for (sx = [1,-1], i = [0:vent_side_n-1])
            translate([sx*xo/2, -(vent_side_n-1)*10/2 + i*10, back_t + cav_d - 7])
                cube([wall*3, 3, 9], center = true);
    }
}

// ----------------------------------------------------------------------------
//  STAND — free-standing desk cradle (prints flat)
// ----------------------------------------------------------------------------
module stand() {
    chan_w = (bez_h + cav_d + back_t) + 2.0;
    linear_extrude(stand_t) rrect2d(stand_w, stand_d, 8);
    // reclined back fin
    translate([0, stand_d/2 - 14, stand_t - 0.01]) rotate([-stand_ang, 0, 0])
        translate([-stand_w/2 + 20, -5, 0]) cube([stand_w - 40, 10, 60]);
    // front lip + back rail form the bottom-edge channel
    translate([-stand_w/2 + 20, -stand_d/2 + 20 - 3, stand_t - 0.01]) cube([stand_w - 40, 3, 12]);
    translate([-stand_w/2 + 20, -stand_d/2 + 20 + chan_w, stand_t - 0.01]) cube([stand_w - 40, 3, 10]);
}

// ----------------------------------------------------------------------------
if      (part == "bezel") bezel_print();
else if (part == "back")  back();
else if (part == "stand") stand();
else {
    bezel_print();
    translate([xo + 16, 0, 0]) back();
    if (opt_stand) translate([0, -(yo/2 + stand_d/2 + 16), 0]) stand();
}
