// ============================================================================
//  Canary — ESP32-C6-LCD DISPLAY CASE  ⚠️ IN DEVELOPMENT (v0.2-dev)
//  A pocket portrait-display witness for the Waveshare ESP32-C6-LCD boards —
//  our newest firmware-display target. Two board variants, one file:
//    headers="none" — flat-bottom board (no male header pins soldered): the
//                     PCB back is close to flat, so the case is shallow.
//    headers="male" — factory pin headers soldered pointing DOWN (away from
//                     the glass). The cavity deepens to swallow base + pins,
//                     the press standoffs move inboard of the header rows,
//                     and the snap skirt goes shallow/thin to clear the pins.
//  Either way the board is captured by its edges, not by screwing into its
//  M2 holes.
//
//  Two printed parts:
//    bezel — front frame; the board drops in glass-first, and the face
//            overlaps the LCD module border (the active-area window is what you
//            see). Prints FACE-DOWN (the A-surface is your build plate).
//    back  — rear snap cover; a perimeter skirt clicks into the bezel walls
//            (4 nubs, no fasteners) and four standoffs press the PCB
//            forward against the bezel's glass ledge. A blind keyhole
//            wall-mounts it.
//
//  FIT LESSONS from the first print (don't undo these):
//    * The BOOT/RST buttons overhang the PCB edge, so the board could not
//      reach its seat — the side walls now carry full-depth clearance
//      channels ("ears"): the wall bulges outward locally and the channel
//      runs from the rear opening all the way to the seat, so the button
//      overhangs slide in. The round access holes drill through the ear skin.
//    * The USB-C shell overhangs the bottom edge the same way — the bottom
//      wall gets the same treatment (a "chin"), and the port opening is now a
//      true stadium (full-round ends, like the connector itself) instead of a
//      rectangle, sized shell + tolerance.
//
//  Model presets are parametric — `model = "1.47"` is dimensioned from the
//  Waveshare mechanical drawing; the 1.69 preset lands when its drawing does.
//
//  Orientation: +Y = up (portrait), USB-C exits the BOTTOM (−Y) short wall,
//  +Z = toward the glass. All parts print flat, no supports.
//
//  ⚠️ DEV STATUS: the flat cavity fits are print-tested once; the button /
//     USB overhangs and everything in the headers="male" branch are drawn
//     from nominal connector dimensions — MEASURE your board (btn_proud,
//     btn_up, usb_proud, usb_dz, hdr_drop, hdr_inset) before printing.
// ============================================================================

/* [What to render] */
part  = "all";      // ["bezel","back","all"]
model = "1.47";     // ["1.47","1.69"]  board preset
// board variant: "none" = flat-bottom, "male" = factory down-facing pin headers
headers = "none";   // ["none","male"]

/* [Board] — ESP32-C6-LCD-1.47. From the Waveshare drawing (mm). */
// long axis (portrait height, Y), short axis (X), PCB thickness
board_l = (model == "1.69") ? 36.0  : 36.37;   // 1.69 = MEASURE (placeholder)
board_w = (model == "1.69") ? 25.0  : 20.32;
pcb_t   = 1.45;
lcd_rise   = 3.65;   // LCD glass front above the PCB front face (5.10 stack − 1.45 PCB)
back_stack = 2.2;    // back-side component clearance below the PCB (flat board) — MEASURE

/* [Headers] — the "male" variant only: rows run along the two long (±X)
   edges, pins point down toward the back cover. All four are MEASURE. */
hdr_drop = 8.8;      // cavity depth below the PCB back that swallows base + pins — MEASURE
hdr_inset = 1.6;     // PCB edge → header row centreline — MEASURE
hdr_keepout = 4.2;   // no-press strip along each ±X edge (header body + margin)

/* [Screen] — active area = the window; the LCD module border sits under the lip */
aa_l  = (model == "1.69") ? 27.972 : 32.35;    // active-area long (Y) — 1.69 = MEASURE
aa_w  = (model == "1.69") ? 27.972 : 17.39;    // active-area short (X)
lcm_l = (model == "1.69") ? 32.0   : 36.28;    // LCD module outline long
lcm_w = (model == "1.69") ? 28.0   : 19.39;    // LCD module outline short

/* [USB-C] — on the BOTTOM (−Y) short wall. The opening is a stadium (full-
   round ends, radius = half its height) hugging the receptacle shell —
   nominal shell is 8.94 × 3.26, so the defaults leave ~0.15–0.3 a side. */
usb_w  = 9.2;    // stadium opening width — shell + tolerance
usb_h  = 3.6;    // stadium opening height — shell + tolerance
usb_dx = 0.0;    // sideways offset of the connector centre — MEASURE
usb_dz = 0.0;    // depth offset from mid-PCB (+ = toward the back) — MEASURE
usb_proud = 1.4; // shell overhang past the PCB edge (insertion channel depth) — MEASURE

/* [Buttons] — BOOT/RST flank the USB end on the two side (±X) walls. They
   overhang the PCB edge, so each side wall carries a full-depth clearance
   channel (an "ear") the overhang slides down during insertion. */
opt_btn = true;
btn_d   = 3.6;                 // side access hole Ø (through the ear skin)
btn_up  = 6.5;                 // button centre up from the USB (−Y) end — MEASURE
btn_proud = 1.8;               // button overhang past the PCB edge — MEASURE
btn_ch_w  = 7.0;               // clearance channel width along the wall (Y)

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
ear_skin = 1.2;  // wall skin left outside a button/USB clearance channel

/* [Snap fit] — back skirt into the bezel walls.
   Flat board: skirt_dep = back_stack — the skirt rides directly behind the
   PCB edge and may only reach the PCB back plane (the cavity is only
   board+2·tol_slide wide, ~0.2 mm/side over the PCB) — any deeper and it
   drives into the board and the cover can't seat.
   headers="male": the header pins descend right at the board edges, through
   the zone the skirt occupies — so the skirt goes SHALLOW (just enough to
   carry the nubs) and THIN (1.0 wall), staying outboard of the pin rows.
   The nub sits at back_t + snap_depth so it lands on the skirt AND lines up
   with the bezel window; keep snap_depth + snap_h/2 ≤ skirt_dep. */
snap_n = 4; snap_w = 5.0; snap_h = 1.6; snap_depth = 1.4; snap_proud = 0.5;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived geometry.  Axis convention: X = short (board_w), Y = long (board_l,
//  portrait height), Z = through-thickness (toward the glass).
// ----------------------------------------------------------------------------
stack_eff = (headers == "male") ? max(back_stack, hdr_drop) : back_stack;

xc = board_w + 2*tol_slide;   yc = board_l + 2*tol_slide;   // board cavity (X,Y)
xo = xc + 2*wall;             yo = yc + 2*wall;             // outer (X,Y)
cav_d = lcd_rise + pcb_t + stack_eff;            // glass ledge → back-plate inner
bez_h = face_t + cav_d;                          // bezel wall height
r_in  = max(0.6, r_out - wall);                  // cavity corner radius

z_pcb_front = face_t + lcd_rise;                 // PCB front plane
z_usb       = face_t + lcd_rise + pcb_t/2 + usb_dz;   // USB-C opening centre

// button / USB overhang clearance: how far each channel reaches past the
// cavity wall face, and how far the wall must bulge to keep ear_skin of skin
btn_y     = -board_l/2 + btn_up;         // button centre (Y)
btn_reach = btn_proud + tol_slide;       // channel depth past the cavity face
usb_reach = usb_proud + tol_slide;
ear_bump  = max(0, btn_reach + ear_skin - wall);   // side-wall bulge
chin_bump = max(0, usb_reach + ear_skin - wall);   // bottom-wall bulge
ear_w  = btn_ch_w + 4;                   // ear bulge width along the wall
chin_w = usb_w + 4;                      // chin bulge width along the wall

// active-area window vs module: the face overlaps the module border by
// (module − AA)/2 per side; that overlap retains the glass
lip_l = (lcm_l - aa_l)/2;   // long-side lip (Y)
lip_w = (lcm_w - aa_w)/2;   // short-side lip (X)

// snap skirt: shallow/thin in headers mode (see the [Snap fit] note)
skirt_wall = (headers == "male") ? 1.0 : 1.6;
skirt_dep  = (headers == "male") ? snap_depth + snap_h/2 + 0.4 : back_stack;
skirt_x = xc - 2*tol_press;   skirt_y = yc - 2*tol_press;

// press standoffs: inboard of the header rows when the board has them
stand_ix = (headers == "male") ? hdr_keepout + 2.1 : 2.6;   // inset from ±X edge
stand_iy = 2.6;                                             // inset from ±Y edge

// snap windows / nubs live on the two long (±X) side walls; the bottom pair
// shifts up above the button channel so nub and channel never overlap
function nub_ys() = opt_btn
    ? [btn_y + btn_ch_w/2 + snap_w/2 + 2.5, board_l/4]
    : [-board_l/4, board_l/4];

// wall vents: keep the band between the glass ledge and the snap windows,
// and shift the row up clear of the button ears
vent_z0 = face_t + 1.0;
vent_z1 = bez_h - (snap_depth + snap_h/2) - 0.8;
vent_dy = opt_btn ? (btn_y + ear_w/2) + (vent_n-1)*vent_pitch/2 + vent_w/2 + 1.2 : 0;

assert(aa_l < lcm_l && aa_w < lcm_w, "active area must be inside the module outline");
assert(lip_w >= 0.8, "short-side lip < 0.8 mm won't retain the glass — check aa_w/lcm_w");
assert(lcm_l <= yc && lcm_w <= xc, "LCD module larger than the board cavity — check dims");
assert(z_usb - usb_h/2 >= face_t - 0.01, "USB opening cuts into the bezel face — check usb_h/usb_dz");
assert(z_usb + usb_h/2 <= face_t + cav_d, "USB opening overruns the cavity depth — check usb_h/usb_dz");
assert(skirt_dep <= stack_eff + 0.01, "skirt_dep > component clearance — the skirt would drive into the PCB");
assert(skirt_dep >= snap_depth + snap_h/2, "skirt too short to carry the snap nub (nub sits at back_t + snap_depth)");
assert(!opt_btn || min([for (y = nub_ys()) abs(y - btn_y)]) >= btn_ch_w/2 + snap_w/2 + 1.5,
       "a snap window overlaps the button clearance channel — shift nub_ys()/btn_up");
assert(!opt_vent || vent_z1 - vent_z0 >= 1.5, "no room for wall vents between ledge and snap band — set opt_vent=false");
assert(!opt_vent || vent_dy + (vent_n-1)*vent_pitch/2 + vent_w/2 <= yc/2 - 0.8,
       "vent row overruns the side wall — fewer slots (vent_n) or tighter pitch");
assert(headers != "male" || hdr_drop > back_stack, "headers=male but hdr_drop is shallower than the flat clearance");
assert(headers != "male" || tol_slide + hdr_inset - 0.35 >= tol_press + skirt_wall + 0.25,
       "skirt would sit in the header pin row — thin skirt_wall or re-measure hdr_inset");
assert(board_w/2 - stand_ix - 2.1 > 0, "press standoffs collide at the board centre — check hdr_keepout");
echo(str("Canary C6 display (", model, ", headers=", headers, ") v0.2-dev — outer ",
         xo, " x ", yo, " x ", bez_h + back_t, " mm, window ", aa_w, " x ", aa_l,
         " (lip X ", lip_w, " / Y ", lip_l, "), cavity depth ", cav_d,
         "  (IN DEVELOPMENT — MEASURE)"));

module rrect2d(x, y, r) { offset(r = r) offset(r = -r) square([x, y], center = true); }

// stadium: a rounded slot with full-round ends (radius = half the height) —
// the same profile as a USB-C shell
module stadium2d(w, h) { rrect2d(w, h, h/2 - 0.05); }

// outer shell outline shared by the bezel and the back plate, so the ear /
// chin bulges carry through the parting line without a step
module shell_outline2d() {
    rrect2d(xo, yo, r_out);
    if (opt_btn && ear_bump > 0) for (sx = [1, -1])
        translate([sx*(xo/2 + ear_bump/2 - 1.2), btn_y])
            rrect2d(ear_bump + 2.4, ear_w, 1.0);
    if (chin_bump > 0)
        translate([usb_dx, -(yo/2 + chin_bump/2 - 1.2)])
            rrect2d(chin_w, chin_bump + 2.4, 1.0);
}

// board cavity + the overhang clearance channels. The channels are part of
// the cavity profile, so they run the FULL depth — from the rear opening all
// the way to the seat — and the overhanging button / USB shells slide in.
module cavity2d() {
    rrect2d(xc, yc, r_in);
    if (opt_btn) for (sx = [1, -1])
        translate([sx*xc/2, btn_y]) square([2*btn_reach, btn_ch_w], center = true);
    translate([usb_dx, -yc/2]) square([usb_w, 2*usb_reach], center = true);
}

// ----------------------------------------------------------------------------
//  BEZEL — front frame, prints face-down (z0 = outer face)
// ----------------------------------------------------------------------------
module bezel() {
    difference() {
        linear_extrude(bez_h) shell_outline2d();                      // face + walls
        // active-area window through the face (aa_w = X, aa_l = Y)
        translate([0, 0, -0.1]) linear_extrude(face_t + 0.2) rrect2d(aa_w, aa_l, 1.5);
        // face edge chamfer (lead-in on the viewer side)
        if (lid_edge > 0)
            translate([0, 0, -0.01]) linear_extrude(lid_edge + 0.01)
                difference() {
                    offset(delta = 0.1) shell_outline2d();
                    offset(delta = -lid_edge) shell_outline2d();
                }
        // board cavity (with overhang channels) behind the glass ledge
        translate([0, 0, face_t]) linear_extrude(cav_d + 0.2) cavity2d();
        // USB-C stadium opening through the bottom (−Y) wall + chin
        translate([usb_dx, -yo/2, z_usb]) rotate([90, 0, 0])
            linear_extrude(2*(wall + chin_bump + 1), center = true) stadium2d(usb_w, usb_h);
        // BOOT / RST side access holes near the USB end, through the ear skin
        if (opt_btn) for (sx = [1, -1])
            translate([sx*xo/2, btn_y, z_pcb_front + 0.5])
                rotate([0, 90, 0]) cylinder(d = btn_d, h = 2*(wall + ear_bump + 1), center = true);
        // heat-escape slots: a row on each side wall (convection in the
        // bottom, out the top) — kept clear of the ledge, the ears and the
        // snap-window band
        if (opt_vent) for (sx = [1, -1], i = [0:vent_n-1])
            translate([sx*xo/2, vent_dy - (vent_n-1)*vent_pitch/2 + i*vent_pitch, (vent_z0 + vent_z1)/2])
                cube([wall*3, vent_w, vent_z1 - vent_z0], center = true);
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
            linear_extrude(back_t) shell_outline2d();                       // plate
            translate([0, 0, back_t - 0.01]) linear_extrude(skirt_dep)      // skirt ring
                difference() {
                    rrect2d(skirt_x, skirt_y, r_in);
                    rrect2d(skirt_x - 2*skirt_wall, skirt_y - 2*skirt_wall, max(0.4, r_in - skirt_wall));
                }
            // standoffs press the PCB forward onto the bezel glass ledge —
            // inboard of the header rows when the board has them (MEASURE:
            // the pads must land on bare PCB; move via stand_ix / stand_iy)
            for (sx = [1, -1], sy = [1, -1])
                translate([sx*(board_w/2 - stand_ix), sy*(board_l/2 - stand_iy), back_t - 0.01])
                    cylinder(d = 4.2, h = stack_eff + 0.01);
            // snap nubs on the short-wall (±X) skirt faces, chamfered both ways
            for (sx = [1, -1], yc0 = nub_ys())
                translate([sx*(skirt_x/2 - 0.3), yc0, back_t + snap_depth]) hull() {
                    for (dy = [-snap_w/2 + 1.0, snap_w/2 - 1.0])
                        translate([0, dy, 0]) cube([0.6, 0.1, snap_h - 0.4], center = true);
                    translate([sx*(snap_proud + 0.3), 0, 0]) cube([0.1, 0.1, 0.6], center = true);
                }
        }
        // heat-escape grille in the back plate, over the component zone —
        // skipping any slot that would undermine a standoff foot (the
        // headers variant pulls the standoffs inboard, over the grille)
        if (opt_vent) for (i = [0:vent_n], sx = [1, -1])
            let (gy = -(vent_n)*vent_pitch/2 + i*vent_pitch,
                 over_feet = board_w/2 - stand_ix - 2.1 < 5.4)
            if (!over_feet || gy + vent_w <= board_l/2 - stand_iy - 2.2)
                translate([sx*3.2, gy, -0.1])
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
