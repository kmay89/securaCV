// ============================================================================
//  Canary — ESP32-S3-Touch-LCD-1.69 WATCH-DISPLAY CASE  ⚠️ IN DEVELOPMENT (v0.2-dev)
//  A desk/shelf witness puck for the Waveshare ESP32-S3-Touch-LCD-1.69 — the
//  rounded-square "smartwatch" board (ESP32-S3R8, 240×280 capacitive touch,
//  QMI8658 IMU, PCF85063 RTC, ETA6098 charger, Li-battery + RTC-battery
//  connectors, buzzer). The bonded glass slab (41.13 × 33.13) overhangs the
//  smaller PCB (37.12 × 29.83) by ~2 mm all round, so the case captures the
//  board by that glass lip — no screws into the board (this board has no
//  mounting holes).
//
//  Two printed parts:
//    bezel — front frame; the board drops in glass-first, the face overlaps the
//            rounded-square glass border and the active-area window (32.634 ×
//            27.972) is what you see. Prints FACE-DOWN.
//    back  — rear snap cover; a perimeter skirt clicks into the bezel walls
//            (4 nubs, no fasteners) and four standoffs press the PCB forward
//            onto the bezel's glass ledge. Vented; a blind keyhole hangs it.
//    stand — optional reclined desk cradle (no hardware).
//
//  Orientation: landscape, +X = width (41.13), +Y = up (33.13), +Z = toward the
//  glass. USB-C exits the BOTTOM (−Y) edge; PWR/BOOT/RST flank the TOP (+Y)
//  edge; the battery / RTC / pin cluster exits the RIGHT (+X) edge. All parts
//  print flat, no supports.
//
//  Heat: the S3 + backlight + charger run warm — a back grille plus side
//  chimney slots convect it out. Keep them clear.
//
//  ⚠️ CONNECTOR POSITIONS ARE NOMINAL — the Waveshare drawing dimensions the
//  glass, AA and PCB outline precisely but not each connector center, and the
//  ~2 mm glass overhang means ports sit slightly recessed. MEASURE the USB-C /
//  button / connector positions and the stack heights on YOUR Rev before
//  printing.  DEV STATUS: render/mesh-verified only — NOT print-validated.
//
//  2026-08-23: v0.2-dev — adopted the shared contract libraries (core/mount/
//  snap/board). Four deliberate fixes ride along, so this is NOT mesh-neutral:
//    * tolerance trio unified to the catalog contract (tol_slide 0.20 /
//      tol_press 0.10 — this file ran 0.25/0.12; the coupon tunes deviations
//      per PRINTER, not per file);
//    * the keyhole is now the catalog's true BLIND pocket (slot 8.0, depth
//      3.5 behind a 1.0 face web) on a new pad inside the plate — the old
//      drawing said "blind" and cut clean through the 2.0 plate; the grille
//      now stops short of the pad instead of breaching the pocket;
//    * the snap WINDOW derives from the ridge via snap_window() — one knob
//      used to size both, drawing a 3.1 ridge in a 5.0 window (1.9 of
//      rattle: the C3's lid-slide defect exactly);
//    * the C3's glass-relief band stands the window rim 0.25 off the
//      innermost 0.5 of the lip, so the rim never contacts the glass edge.
// ============================================================================

use <canary_core_lib.scad>    // rrect2d/pill2d + the catalog tolerance trio the knobs cite
use <canary_mount_lib.scad>   // the stud/keyhole hanging standard — the blind pocket's one home
use <canary_snap_lib.scad>    // snap doctrine — the window derives from the ridge it parks
use <canary_board_lib.scad>   // board registry — the ws169 numbers the knobs cite

/* [What to render] */
part = "all";        // ["bezel","back","stand","all"]

/* [Glass slab] — bonded touch panel, from the Waveshare "Dimensions" drawing
   (mm). Registry: brd_ws169_glass_*() in canary_board_lib — the registry's
   axes follow the board's along-USB length, which is this case's Y, so its
   w/h names land crosswise here. */
glass_w = 41.13;     // rounded-square glass width  (X) — brd_ws169_glass_h()
glass_h = 33.13;     // rounded-square glass height (Y) — brd_ws169_glass_w()
glass_t = 2.6;       // glass + LCD module thickness at the edge — MEASURE
r_glass = 7.0;       // glass corner radius (watch-style) — MEASURE
aa_w = 32.634;       // active area width  (280 px long axis, landscape)
aa_h = 27.972;       // active area height (240 px)
aa_dx = 0.0; aa_dy = 0.0;   // AA center offset from glass center — MEASURE

/* [Glass protection] — locate, don't clamp (the C3's print-proven band,
   carried here: this bezel's lip is the only thing holding the slab, and it
   must bear on the flat glass border — never bite the slab's edge) */
glass_relief   = 0.25;  // face stand-off over the innermost lip band, so the
                        // window rim never touches the glass
glass_relief_w = 0.5;   // width of that relieved band, from the window edge out

/* [PCB behind the glass] — smaller than the glass, no mounting holes.
   Registry: "ws169" in canary_board_lib (drawing evidence). */
pcb_w = 37.12;       // PCB outline width  (X) — brd_w("ws169")
pcb_h = 29.83;       // PCB outline height (Y) — brd_l("ws169"), the along-USB axis
pcb_t = 1.6;         // brd_t("ws169")
panel_gap  = 0.0;    // spacer between the display-module base and the PCB front
                     // (0 = module bonded straight on) — MEASURE
back_stack = 4.5;    // tallest thing behind the PCB (USB-C / JST / buzzer) — MEASURE

/* [USB-C] — on the BOTTOM (−Y) edge, centered on the PCB thickness */
opt_usb = true;
usb_w = 9.2;   usb_h = 3.4;   usb_dx = 0.0;   // MEASURE your connector

/* [Buttons] — PWR / BOOT / RST on the TOP (+Y) edge (side access holes) */
opt_btn = true;
btn_d = 3.4;                       // access hole Ø
btn_xs = [-9.0, 0.0, 9.0];         // button X centers on the top edge — MEASURE

/* [Side cluster] — battery / RTC / pin row exit on the RIGHT (+X) edge */
opt_side = true;
side_open_h = 16.0;   side_open_dy = 0.0;      // tall slot — MEASURE

/* [Mount] — the catalog's blind stud/keyhole standard; defaults cite
   canary_mount_lib, and deviations earn their keep on the fit coupon */
opt_keyhole = true;    // one BLIND keyhole in the back (wall hang) — the pocket
                       // never breaches the cavity; a pad inside the plate
                       // hosts it (kh_pad_* below)
kh_head_d  = 7.0;      // screw-head pass hole (#6 / M3.5 pan) — mount_kh_head_d()
kh_shank_d = 4.2;      // shank slot width — mount_kh_shank_d()
kh_slot_l  = 8.0;      // slot travel — mount_kh_slot_l() (7.0 was a drifted
                       // copy: a slide the head never finishes)
kh_head_h  = 3.5;      // total pocket depth, face web included — mount_kh_head_h()
kh_face    = 1.0;      // face web the screw head grips behind — mount_kh_face()

/* [Ventilation] — let the S3 / backlight / charger heat convect out */
opt_vent = true;
vent_n = 6;          // side slots per long wall
vent_pitch = 4.6;    // slot spacing
vent_w = 1.4;        // slot width
grille_n = 7;        // back-grille slots

/* [Print tolerances] — per-side clearances; the catalog trio (core_tol_*() in
   canary_core_lib), dialed on canary_fit_coupon.scad. Unified 2026-08-23 from
   this file's 0.25/0.12 — the coupon tunes deviations per PRINTER, not per
   file. */
tol_slide = 0.20;    // sliding fits: glass drop-in, skirt — core_tol_slide()
tol_press = 0.10;    // press fits: the skirt's snug seat — core_tol_press()
tol_hole = 0.30;     // clearance holes — core_tol_hole()

/* [Shell] */
wall   = 2.4;    // side wall thickness
face_t = 2.0;    // bezel face (over the glass border)
back_t = 2.0;    // rear cover plate
r_out  = r_glass + wall;   // outer corner radius follows the glass

/* [Snap fit] — back skirt into the bezel walls. The WINDOW is derived from
   the ridge via snap_window() (canary_snap_lib): snap_w used to be one number
   sizing both the ridge's end plates and the window it parks in, which drew a
   3.1 mm ridge inside a 5.0 mm window — 1.9 mm of rattle, the C3's lid-slide
   defect exactly. The ridge's drawn width is the parameter now. */
nub_w = 3.1;         // the ridge's DRAWN width in Y — exactly what the old
                     // arithmetic drew, so the click itself is unchanged
snap_play = 0.15;    // window clearance per side — the catalog default (a
                     // printed window comes out a hair small and the skirt
                     // still has to enter)
snap_h = 1.6; snap_depth = 2.6; snap_proud = 0.5;
skirt_wall = 1.6; skirt_dep = back_stack;   // must not exceed back_stack (see assert)

/* [Stand] */
opt_stand = true;
stand_ang = 22;  stand_w = 70.0;  stand_d = 52.0;  stand_t = 4.0;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived. Axis: X = width, Y = height, Z = through-thickness (toward glass).
// ----------------------------------------------------------------------------
xc = glass_w + 2*tol_slide;   yc = glass_h + 2*tol_slide;   // glass cavity
xo = xc + 2*wall;             yo = yc + 2*wall;             // outer
// glass front → PCB front = the module rides glass_t proud of its base, which
// sits panel_gap above the PCB. Deriving it from glass_t means MEASUREing the
// module thickness actually re-seats the cavity, PCB plane and ports.
lcd_rise = glass_t + panel_gap;                 // glass front above PCB front
cav_d = lcd_rise + pcb_t + back_stack;          // glass ledge → back-plate inner
bez_h = face_t + cav_d;                         // bezel wall height
r_in  = max(1.0, r_out - wall);                 // cavity corner radius

z_pcb_front = face_t + lcd_rise;                // PCB front plane
z_usb       = face_t + lcd_rise + pcb_t/2;      // USB-C center (mid-PCB)

// the face overlaps the glass border by (glass − AA)/2 per side; that retains it
lip_w = (glass_w - aa_w)/2;   lip_h = (glass_h - aa_h)/2;
// the LAND is what is left of the lip outside the relieved band — that ring
// is the slab's only contact
land_x = lip_w - glass_relief_w;   land_y = lip_h - glass_relief_w;
// the PCB is inset inside the glass — that overhang is what the lip presses on
ohang_x = (glass_w - pcb_w)/2;   ohang_y = (glass_h - pcb_h)/2;

// blind keyhole: the catalog pocket wants kh_head_h + 1.5 of stock (the +1.5
// is the web that keeps it blind — canary_mount_lib), and the 2.0 plate
// cannot host that alone, so a pad inside the plate makes up the rest. The
// pad stands into the component zone: MEASURE what sits under the case
// center — only back_stack − kh_pad_h (1.5 at defaults) is left over its
// footprint.
kh_pad_wall = core_min_wall();                // lateral skin around the head cavity
kh_pad_x = kh_head_d + 0.6 + 2*kh_pad_wall;   // + 0.6 = the pocket's head-cavity growth
kh_pad_y = kh_slot_l + kh_head_d + 0.6 + 2*kh_pad_wall;
kh_pad_h = kh_head_h + 1.5 - back_t;          // pad height above the plate inner face
// grille ends: full slots span ±grille_x1; a slot that would cross the pad
// stops at grille_x0 instead (a slot through the pad would breach the
// pocket's head cavity — and the blind promise with it)
grille_x1 = pcb_w/2 - 8;
grille_x0 = kh_pad_x/2 + vent_w/2 + 0.4;

assert(aa_w < glass_w && aa_h < glass_h, "active area must sit inside the glass");
assert(lip_w >= 1.5 && lip_h >= 1.5, "glass lip < 1.5 mm won't retain the slab — check aa/glass");
assert(land_x >= 0.8 && land_y >= 0.8,
       str("glass land is ", land_x, " / ", land_y, " mm — too narrow to carry ",
           "the slab. Shrink glass_relief_w; the relief must never eat the ",
           "land that locates the glass."));
assert(glass_relief < face_t - 0.5, "glass relief eats the bezel face — check glass_relief/face_t");
assert(pcb_w < glass_w && pcb_h < glass_h, "PCB should be smaller than the glass slab — check dims");
assert(skirt_dep <= back_stack + 0.01, "skirt_dep > back_stack — the skirt would drive into the PCB; cap it at back_stack");
assert(skirt_dep >= snap_depth + snap_h/2, "skirt too short to carry the snap nub (nub sits at back_t + snap_depth) — raise skirt_dep or lower snap_depth");
assert(usb_h <= pcb_t + 2*lcd_rise, "USB slot taller than the front stack — check usb_h");
assert(!opt_keyhole || back_stack - kh_pad_h >= 1.0,
       str("the keyhole pad tops out ", back_stack - kh_pad_h, " mm under the ",
           "PCB back — nothing on the case-center footprint may be taller; ",
           "deepen back_stack or drop opt_keyhole"));
assert(!opt_vent || !opt_keyhole || grille_x1 - grille_x0 >= 1.0,
       "grille segments vanish beside the keyhole pad — check pcb_w / kh knobs");
echo(str("Canary S3-Touch-1.69 watch display v0.2-dev — outer ", xo, " x ", yo,
         " x ", bez_h + back_t, " mm, window ", aa_w, " x ", aa_h,
         " (glass lip X ", lip_w, " / Y ", lip_h, ", PCB overhang X ", ohang_x,
         " / Y ", ohang_y, ")  (IN DEVELOPMENT — MEASURE CONNECTORS)"));

// snap windows / nubs live on the two long (±Y is top/bottom → use ±X side) walls.
// Put them on the ±X (left/right) walls, near top & bottom.
skirt_x = xc - 2*tol_press;   skirt_y = yc - 2*tol_press;
// the snap WINDOW, derived from the ridge that has to sit in it — never typed
// (snap_window(): ridge + play per side; one number cannot size two features
// that are not the same size)
snap_w = snap_window(nub_w, snap_play);
function nub_ys() = [-glass_h/4, glass_h/4];

// stagger the PCB-pressing standoffs just inside the PCB corners
function standoff_pts() = [for (sx = [1,-1], sy = [1,-1])
                             [sx*(pcb_w/2 - 3.0), sy*(pcb_h/2 - 3.0)]];

// ----------------------------------------------------------------------------
//  BEZEL — front frame, prints face-down (z0 = outer face)
// ----------------------------------------------------------------------------
module bezel() {
    difference() {
        linear_extrude(bez_h) rrect2d(xo, yo, r_out);                 // face + walls
        // active-area window through the face (rounded)
        translate([aa_dx, aa_dy, -0.1]) linear_extrude(face_t + 0.2) rrect2d(aa_w, aa_h, 3.0);
        // glass relief: stand the face off the innermost band of the lip so
        // the window's rim never contacts the glass edge — locate, don't
        // clamp (the C3's print-proven band; the slab bears on the LAND ring
        // outside this recess, never on its own edge)
        translate([aa_dx, aa_dy, face_t - glass_relief])
            linear_extrude(glass_relief + 0.02)
                rrect2d(aa_w + 2*glass_relief_w, aa_h + 2*glass_relief_w,
                        3.0 + glass_relief_w);
        // glass cavity behind the face ledge
        translate([0, 0, face_t]) linear_extrude(cav_d + 0.2) rrect2d(xc, yc, r_in);
        // USB-C slot in the bottom (−Y) wall
        if (opt_usb)
            translate([usb_dx, -yo/2, z_usb]) cube([usb_w, wall*3, usb_h], center = true);
        // PWR/BOOT/RST access holes in the top (+Y) wall
        if (opt_btn) for (bx = btn_xs)
            translate([bx, yo/2, z_pcb_front + 0.5])
                rotate([90, 0, 0]) cylinder(d = btn_d, h = wall*3, center = true);
        // battery / RTC / pin cluster slot in the right (+X) wall
        if (opt_side)
            translate([xo/2, side_open_dy, z_pcb_front + pcb_t/2])
                cube([wall*3, side_open_h, back_stack + 1.0], center = true);
        // heat-escape slots: a row low on each ±X side wall, over the cavity
        if (opt_vent) for (sx = [1, -1], i = [0:vent_n-1])
            translate([sx*xo/2, -(vent_n-1)*vent_pitch/2 + i*vent_pitch, face_t + cav_d/2])
                cube([wall*3, vent_w, cav_d*0.6], center = true);
        // snap windows in the ±X side walls (the back's nubs click in here) —
        // sized by snap_window(), never by the ridge's own number
        for (sx = [1, -1], yy = nub_ys())
            translate([sx*xo/2, yy, bez_h - snap_depth])
                cube([wall*3, snap_w, snap_h], center = true);
    }
}
module bezel_print() { bezel(); }

// ----------------------------------------------------------------------------
//  BACK — rear snap cover, prints outer-face-down (z0 = outer back face).
//  Skirt inserts into the bezel cavity (it rides in the ~2 mm glass overhang,
//  clear of the PCB); nubs at back-z = snap_depth meet the bezel wall windows.
// ----------------------------------------------------------------------------
module back() {
    difference() {
        union() {
            linear_extrude(back_t) rrect2d(xo, yo, r_out);                  // plate
            translate([0, 0, back_t - 0.01]) linear_extrude(skirt_dep)      // skirt ring
                difference() {
                    rrect2d(skirt_x, skirt_y, r_in);
                    rrect2d(skirt_x - 2*skirt_wall, skirt_y - 2*skirt_wall, max(0.6, r_in - skirt_wall));
                }
            // keyhole pad: the stock the blind pocket lives in (the plate
            // alone is back_t = 2.0; the pocket needs kh_head_h + 1.5)
            if (opt_keyhole)
                translate([0, 0, back_t - 0.01])
                    linear_extrude(kh_pad_h + 0.01) rrect2d(kh_pad_x, kh_pad_y, 2.0);
            // standoffs press the PCB forward onto the bezel glass ledge
            for (p = standoff_pts())
                translate([p[0], p[1], back_t - 0.01]) cylinder(d = 4.4, h = back_stack + 0.01);
            // snap nubs on the ±X skirt faces, chamfered both ways; the ridge
            // spans nub_w in Y — the SAME number the window derives from.
            // The back assembles outer-face-out, so a skirt point at back-z
            // maps to bezel-z = bez_h + back_t − back-z; placing the nub at
            // back_t + snap_depth lands it at bez_h − snap_depth = the window.
            for (sx = [1, -1], yy = nub_ys())
                translate([sx*(skirt_x/2 - 0.3), yy, back_t + snap_depth]) hull() {
                    for (dy = [-nub_w/2 + 0.05, nub_w/2 - 0.05])
                        translate([0, dy, 0]) cube([0.6, 0.1, snap_h - 0.4], center = true);
                    translate([sx*(snap_proud + 0.3), 0, 0]) cube([0.1, 0.1, 0.6], center = true);
                }
        }
        // heat-escape grille in the back plate, over the component zone; a
        // slot that would cross the keyhole pad stops short of it on both
        // sides instead (splitting keeps the airflow the skip would lose)
        if (opt_vent) for (i = [0:grille_n-1]) {
            gy = -(grille_n-1)*3.4/2 + i*3.4;
            if (!opt_keyhole || abs(gy) - vent_w/2 >= kh_pad_y/2 + 0.4)
                translate([0, gy, -0.1]) linear_extrude(back_t + 0.2)
                    pill2d(2*grille_x1 + vent_w, vent_w);
            else for (sx = [1, -1])
                translate([sx*(grille_x0 + grille_x1)/2, gy, -0.1])
                    linear_extrude(back_t + 0.2)
                        pill2d(grille_x1 - grille_x0 + vent_w, vent_w);
        }
        // BLIND keyhole pocket, cut into plate + pad from the outer face
        // (slot toward +Y = UP on the wall, so the case slides DOWN to seat —
        // gravity is the latch). canary_mount_lib draws it natively along the
        // axis — no rotate — and keeps it blind: 1.5 of web stays between the
        // head cavity and the case interior, so hanging never opens the case.
        if (opt_keyhole)
            mount_keyhole_pocket(0, 0, "y",
                                 kh_head_d, kh_shank_d, kh_slot_l, kh_head_h, kh_face);
    }
}

// ----------------------------------------------------------------------------
//  STAND — free-standing desk cradle (prints flat)
// ----------------------------------------------------------------------------
module stand() {
    chan_w = (bez_h + back_t) + 2.0;
    linear_extrude(stand_t) rrect2d(stand_w, stand_d, 6);
    // reclined back fin
    translate([0, stand_d/2 - 10, stand_t - 0.01]) rotate([-stand_ang, 0, 0])
        translate([-stand_w/2 + 8, -4, 0]) cube([stand_w - 16, 8, 34]);
    // front lip + back rail form the bottom-edge channel
    translate([-stand_w/2 + 8, -stand_d/2 + 12 - 3, stand_t - 0.01]) cube([stand_w - 16, 3, 10]);
    translate([-stand_w/2 + 8, -stand_d/2 + 12 + chan_w, stand_t - 0.01]) cube([stand_w - 16, 3, 9]);
}

// ----------------------------------------------------------------------------
if      (part == "bezel") bezel_print();
else if (part == "back")  back();
else if (part == "stand") stand();
else {
    bezel_print();
    translate([xo + 12, 0, 0]) back();
    if (opt_stand) translate([0, -(yo/2 + stand_d/2 + 12), 0]) stand();
}
