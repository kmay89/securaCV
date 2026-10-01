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
use <canary_port_lib.scad>    // the USB-C shell height the port datum is derived from

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
aa_dx = 0.0;         // AA center X offset from glass center — MEASURE
aa_dy = 0.0;         // AA center Y offset from glass center — MEASURE

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

/* [USB-C] — on the BOTTOM (−Y) edge. The opening is the plug OVERMOLD
   channel, not a shell-sized slot: the receptacle face sits at the PCB edge,
   wall + tol_slide + glass-overhang (4.25 mm at defaults) behind the outer
   face, and a shell-sized hole stopped the cable's overmold at the outer
   wall — the shell latched on 2.25 mm of its 6.5 mm insertion. The channel
   is the Type-C spec's maximum overmold envelope (canary_port_lib) cut clear
   through to the receptacle, so any compliant cable seats and latches. */
opt_usb = true;
usb_dx = 0.0;        // channel center along the bottom edge — MEASURE

/* [Buttons] — PWR / BOOT / RST on the TOP (+Y) edge (side access holes) */
opt_btn = true;
btn_d = 3.4;                       // access hole Ø
btn_xs = [-9.0, 0.0, 9.0];         // button X centers on the top edge — MEASURE

/* [Side cluster] — battery / RTC / pin row exit on the RIGHT (+X) edge */
/* [Port / button datums] — the 1.47 family mounts USB-C and the tact switches
   on the BACK of the PCB (photo-verified on the C3 and C6); the 1.69 has not
   met calipers, so the side is a knob. At "front"/"back" the opening centers
   on the connector's own height off that face, never on the PCB's middle. */
usb_side = "back";   // ["back","front"] which PCB face carries the USB-C — MEASURE
usb_dz   = 0.0;      // measured correction to the derived USB-C center (+ = toward the back)
btn_side = "back";   // ["back","front"] which PCB face carries the buttons — MEASURE
btn_dz   = 1.0;      // actuator center off that face (side tact switches sit ~1.0) — MEASURE
opt_side = true;
side_open_h = 10.0;   // side slot height — MEASURE; capped by the snap windows either side of it (asserted)
side_open_dy = 0.0;   // side slot center offset (Y) — MEASURE

/* [Stud/keyhole interface] — the catalog's blind stud/keyhole standard */
// defaults cite canary_mount_lib, and deviations earn their keep on the fit coupon
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
wall   = 2.4;    // deviates: the glass sets this shell — r_out = r_glass + wall, and the skirt snap engages inside it; validated as a set
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
snap_h = 1.6;         // window / nub height (Z)
snap_depth = 2.6;     // window center below the bezel rim
snap_proud = 0.25;    // nub stand-proud of the skirt: 0.2 of travel over the cavity wall, which the
                      // bezel wall takes as a beam (the snap lib's cycle budget gates it; 0.5 was 4.4 %)
pry_notch = true;    // fingernail notches in the bezel's BOTTOM (-Y) wall rear rim, either side of the USB
                     // channel: the back snaps closed on a flush parting line with nothing to lift it by
                     // (0.6 into the wall, 0.8 below the rim)
pry_w = 4.0;         // notch width  // [3:0.5:8]
skirt_wall = 1.6; skirt_dep = back_stack;   // must not exceed back_stack (see assert)

/* [Stand] */
opt_stand = true;
stand_ang = 22;  stand_w = 70.0;  stand_d = 52.0;  stand_t = 4.0;
// The seat's height is DERIVED from the plug: the USB-C leaves the bottom
// edge pointing stand_ang forward of straight down, so the seat has to stand
// far enough up a pedestal that a spec-max boot AND the lead behind it clear
// the desk (see the stand section). These three describe the cable.
plug_l = 20.0;       // straight Type-C boot (overmold) length past the receptacle face the seat clears — MEASURE your cable; 20 covers the common molded boot  // [12:1:30]
cable_d = 4.5;       // the lead's diameter behind the boot: sizes its room under the seat and the channel's crown — MEASURE your cable  // [3:0.5:6]
cable_bend_r = 15.0; // gentlest bend the lead makes under the seat toward the back, on its centerline (about 3 x cable_d for a molded USB-C lead); the channel's crown clears it  // [8:1:25]
cable_drop = plug_l + cable_d/2 + 3.0;   // clear height under the boot's end: the one-boot-length of straight lead a molded strain relief holds (25 mm at any recline), its radius, and 3 of desk. DERIVED, never typed: the seat's height follows from it

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
z_pcb_back  = z_pcb_front + pcb_t;              // PCB back plane
z_usb       = (usb_side == "back" ? z_pcb_back + port_usbc_shell_h()/2
                                  : z_pcb_front - port_usbc_shell_h()/2) + usb_dz;   // on the shell's axis
z_btn       = (btn_side == "back" ? z_pcb_back + btn_dz : z_pcb_front - btn_dz);

// the face overlaps the glass border by (glass − AA)/2 per side; that retains it
lip_w = (glass_w - aa_w)/2;   lip_h = (glass_h - aa_h)/2;
// the LAND is what is left of the lip outside the relieved band — that ring
// is the slab's only contact
land_x = lip_w - glass_relief_w;   land_y = lip_h - glass_relief_w;
// the PCB is inset inside the glass — that overhang is what the lip presses on
ohang_x = (glass_w - pcb_w)/2;   ohang_y = (glass_h - pcb_h)/2;
// the USB overmold channel: outer face -> receptacle face at the PCB edge,
// opened to the spec envelope so the cable seats (see the [USB-C] note)
usb_reach = wall + tol_slide + ohang_y;                 // how far the receptacle hides
usb_ch_w  = port_usbc_overmold_w() + 2*tol_hole;        // channel envelope
usb_ch_h  = port_usbc_overmold_h() + 2*tol_hole;
// the channel's crown stands past the bezel rim by this much; the back
// plate's edge band covers it and is shelved to let the overmold through
plate_relief = max(0, z_usb + usb_ch_h/2 - bez_h);

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
// the pry notches sit 1.5 outboard of the USB channel's edge (or flank the
// center when there is no USB), and must end before the corner radius begins
pry_x = (opt_usb ? abs(usb_dx) + usb_ch_w/2 + 1.5 : 4.0) + pry_w/2;
assert(!pry_notch || pry_x + pry_w/2 <= xo/2 - r_out,
       "no straight wall left for a pry notch beside the USB channel — narrow pry_w");
assert(!pry_notch || wall - 0.6 >= 1.2, "the pry notch leaves under 1.2 mm of wall");
assert(skirt_dep >= snap_depth + snap_h/2, "skirt too short to carry the snap nub (nub sits at back_t + snap_depth) — raise skirt_dep or lower snap_depth");
// the mated plug's overmold sweeps a spec-height band about the shell axis;
// it must ride OVER the glass slab (which is wider than the PCB, so it stands
// in the channel's approach for the overmold's last 1.65 mm of travel)
assert(!opt_usb || z_usb - port_usbc_overmold_h()/2 >= face_t + glass_t - 0.1,
       str("a mated plug's overmold would land on the glass slab (overmold under-edge ",
           z_usb - port_usbc_overmold_h()/2, " vs glass back plane ", face_t + glass_t,
           ") — usb_side/usb_dz put the port too low for the overmold channel"));
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
if (opt_usb)
    echo(str("USB-C: receptacle face ", usb_reach, " mm behind the outer face — ",
             "overmold channel ", usb_ch_w, " x ", usb_ch_h, " cut through wall, ",
             "skirt and plate band, so the full ", port_usbc_insertion(),
             " mm insertion survives (was latched on 2.25 mm)"));

// snap windows / nubs live on the two long (±Y is top/bottom → use ±X side) walls.
// Put them on the ±X (left/right) walls, near top & bottom.
skirt_x = xc - 2*tol_press;   skirt_y = yc - 2*tol_press;
// the snap WINDOW, derived from the ridge that has to sit in it — never typed
// (snap_window(): ridge + play per side; one number cannot size two features
// that are not the same size)
snap_w = snap_window(nub_w, snap_play);
// the snap pair sits at the ends of the side walls' STRAIGHT run, 0.4 inboard
// of where the r_in corner arc begins. At ±(glass_h/2 − 5) the windows landed
// on the arcs (±11.565 against a straight wall that ends at ±9.765): a nub
// facing a curved wall meets it at an angle and the window is a slot cut
// through a bend. (At ±glass_h/4, before that, they shared their wall with the
// vent slots and the +X side slot.)
function nub_ys() = [-(yc/2 - r_in - snap_w/2 - 0.4), yc/2 - r_in - snap_w/2 - 0.4];
assert(max([for (yy = nub_ys()) abs(yy)]) + snap_w/2 <= yc/2 - r_in,
       "a snap window runs onto the cavity's corner arc — the nub must face straight wall");
// the snap is worked at every service, so the lib's CYCLE budget binds. The
// skirt is a closed ring rooted snap_depth off its plate — effectively rigid —
// so the bezel side wall is the beam that takes the nub's travel: rooted at
// the face plate, loaded at the window center, deflected by what the nub tip
// stands past the cavity wall (the tip cube's outer face is snap_proud + 0.05
// off the skirt face, and the skirt runs tol_press inside the cavity)
snap_defl  = skirt_x/2 + snap_proud + 0.05 - xc/2;
snap_lever = bez_h - snap_depth - face_t;
assert(snap_defl > 0, "the snap nub does not reach the bezel wall — raise snap_proud");
assert(snap_strain(wall, snap_defl, snap_lever) <= snap_budget_cycle(),
       str("bezel wall snap strain ", round(snap_strain(wall, snap_defl, snap_lever)*1000)/10,
           " % — over the ", round(snap_budget_cycle()*1000)/10,
           " % cycle budget: shrink snap_proud, or raise the window toward the rim (a smaller snap_depth lengthens the lever)"));
// the vent slots stop 0.8 below the snap windows' bottom edge, so the two
// never share wall wherever the row runs in y
vent_z0 = face_t + 1.0;
vent_z1 = bez_h - snap_depth - snap_h/2 - 0.8;
assert(vent_z1 - vent_z0 >= 2.0, "no wall left for vent slots below the snap windows — raise back_stack or drop opt_vent");
assert(!opt_side || len([for (yy = nub_ys()) if (abs(yy - side_open_dy) < side_open_h/2 + snap_w/2 + 0.8) 1]) == 0,
       "a +X snap window shares its wall with the side cluster slot");

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
        // USB-C overmold channel through the bottom (−Y) wall — the full
        // Type-C plug envelope, because the receptacle face sits usb_reach
        // behind the outer face (see the [USB-C] note). Its crown clips the
        // wall's rear rim; the back plate covers that and is shelved to match.
        if (opt_usb)
            translate([usb_dx, -yo/2, z_usb]) rotate([90, 0, 0])
                linear_extrude(wall*3, center = true) pill2d(usb_ch_w, usb_ch_h);
        // PWR/BOOT/RST access holes in the top (+Y) wall
        if (opt_btn) for (bx = btn_xs)
            translate([bx, yo/2, z_btn])
                rotate([90, 0, 0]) cylinder(d = btn_d, h = wall*3, center = true);
        // battery / RTC / pin cluster slot in the right (+X) wall
        if (opt_side)
            translate([xo/2, side_open_dy, z_pcb_back + back_stack/2])   // behind the PCB only — it used to open onto the glass edge
                cube([wall*3, side_open_h, back_stack + 0.5], center = true);
        // heat-escape slots: a row low on each ±X side wall, over the cavity
        if (opt_vent) for (sx = [1, -1], i = [0:vent_n-1])
            translate([sx*xo/2, -(vent_n-1)*vent_pitch/2 + i*vent_pitch, (vent_z0 + vent_z1)/2])
                cube([wall*3, vent_w, vent_z1 - vent_z0], center = true);
        // snap windows in the ±X side walls (the back's nubs click in here) —
        // sized by snap_window(), never by the ridge's own number
        for (sx = [1, -1], yy = nub_ys())
            translate([sx*xo/2, yy, bez_h - snap_depth])
                cube([wall*3, snap_w, snap_h], center = true);
        // pry notches: the ±Y walls' rear rim at both ends, inboard of the
        // corner radius and outboard of the USB channel / button holes
        if (pry_notch) for (sx = [1, -1])
            translate([sx*pry_x, -yo/2, bez_h]) cube([pry_w, 2*0.6, 2*0.8], center = true);
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
        // the USB overmold channel continues THROUGH the back part: the
        // skirt ring crosses the plug's path between the wall and the PCB
        // edge, and the plate's edge band stands where the channel's crown
        // clips the bezel rim. (Back-local x mirrors bezel x — the part
        // flips about Y to assemble, which is what points the keyhole
        // slot up.) Cut as a MIRRORED pair under (x,y)→(−x,−y), the C6's
        // flippable-lid rule: the lid has no key, and turned 180° it seated
        // just as well with its skirt standing square across the overmold
        // path (96 mm³ of plug blocked). Now either way round passes the
        // plug; turning the lid only re-aims the keyhole (port-up hanging).
        if (opt_usb) for (s = [1, -1]) {
            // full-height skirt notch over the channel's footprint
            translate([-s*usb_dx, -s*(skirt_y/2 - skirt_wall/2), back_t + skirt_dep/2 + 0.05])
                cube([usb_ch_w + 0.2, skirt_wall + 0.6, skirt_dep + 0.3], center = true);
            // shelf in the plate's inner face where the crown passes over it
            if (plate_relief > 0)
                translate([-s*usb_dx, -s*(yo/2 - (usb_reach + 0.3)/2 + 0.1), back_t - plate_relief/2 + 0.1])
                    cube([usb_ch_w + 0.2, usb_reach + 0.3, plate_relief + 0.2], center = true);
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
// The slab (thickness slab_t = bez_h + back_t) rests on its bottom REAR edge
// on the SEAT at (seat_y, seat_z) — the foot of the fin, on top of a pedestal
// — and leans back stand_ang; everything else is derived from that line: the
// fin's front face contains the slab's back plane, and the front lip's back
// face is a wedge parallel to the slab's front face, 0.2 clear. (v0.1 typed a
// 14.7 channel for a 12.7 slab: it wedged at ~11° and the fin never touched
// the slab at all.)
//
// No back rail. There was one — a 3 × 9 block with its front face at the rest
// line — but the reclined back plane retreats tan(stand_ang) per mm of height,
// so the block's full height stood inside the slab (561.6 mm³ of it). It was
// also redundant: the fin's own foot is the backstop at the rest line.
//
// SEAT HEIGHT (the pedestal): the USB plug leaves the slab's bottom edge
// pointing stand_ang forward of straight down, so with the slab seated on the
// base top a spec-max boot ended 13.9 mm BELOW the desk — the slot through
// the base (the last pass's move, the Watch stand's chin slot) only let it
// into the desk. The seat now stands cable_drop above where the boot ends:
// the receptacle's axis sits usb_reach above the bottom edge and zu ahead of
// the back plane, so reclined it stands rx_z above the seat; the boot reaches
// plug_l down the axis and usb_ch_h/2 across it, and its lowest corner is
// boot_z below the receptacle.
//
// THE LEAD'S WAY OUT: past the boot it either runs on straight (cable_drop =
// plug_l + cable_d/2 + 3 clears 25 mm of it at any recline) or bends
// cable_bend_r toward the back and leaves under the seat through the CHANNEL:
// the slot through base, pedestal and lip in front of the plug's rear edge,
// its rear wall reclined with the plug, continued as a bridged tunnel under
// the seat to the rear edge — the Watch stand's channel under the base. The
// tunnel's crown follows the bend: the lead's centerline turns about (bend_y,
// bend_z), one radius behind the boot's axis, and its REAR edge — the circle
// cable_bend_r - cable_d/2 about that center — meets the slot's rear wall
// (parallel to the axis, usb_ch_h/2 + 0.4 behind it) after turning bend_in,
// i.e. (cable_bend_r - wall)·tan(bend_in) down the axis; everything of the
// lead behind the wall lies below that point.
cable_w = port_usbc_overmold_w() + 2*1.0;   // 1.0 of play a side: the plug arrives tilted
slab_t  = bez_h + back_t;                                     // the slab the stand carries
zu      = slab_t - z_usb;                                     // the plug's axis ahead of the back plane
rx_y    = usb_reach*sin(stand_ang) - zu*cos(stand_ang);       // the receptacle's axis from the seat: y…
rx_z    = usb_reach*cos(stand_ang) + zu*sin(stand_ang);       // …and z
boot_z  = plug_l*cos(stand_ang) + usb_ch_h/2*sin(stand_ang);  // the boot's lowest corner below the receptacle
// the rest line: where the reclined slab's CG — its center, yo/2 up the slab
// and slab_t/2 out from the back plane — stands over the base's center, so
// the push to tip is the same fore and aft. (v0.2 typed it 20 from the front
// edge, which put the CG 4.7 ahead of center; that was a fifth of the front
// margin once the seat rose and the tip lever grew from 45 to 80 mm.)
seat_y  = -(yo/2*sin(stand_ang) - slab_t/2*cos(stand_ang));
seat_z  = cable_drop + boot_z - rx_z;                         // the bottom edge's height when seated
ped_h   = seat_z - stand_t;                                   // the pedestal under it
assert(ped_h >= 0, str("1.69 stand: seat_z ", seat_z, " is under the base top — cable_drop cannot be that negative"));
// the plug's channel envelope reaches usb_ch_h/2 either side of its axis; its
// rear edge runs parallel to the back plane, so it crosses the seat
// (zu − usb_ch_h/2)/cos(a) ahead of the rest line — the slot's rear wall
// stands 0.4 behind that crossing and leans with the plug. World x mirrors
// the bezel's x: the case faces −Y on the stand, so bezel +X is the viewer's
// left.
slot_y1 = seat_y - (zu - usb_ch_h/2)/cos(stand_ang) + 0.4;
// the bend, in world y/z on the plug's x: from the boot's end (boot_y, boot_zz)
// the lead's centerline turns about (bend_y, bend_z) and bottoms out
// cable_bend_r below it, then runs back at that height
boot_y   = seat_y + rx_y - plug_l*sin(stand_ang);
boot_zz  = seat_z + rx_z - plug_l*cos(stand_ang);
bend_y   = boot_y + cable_bend_r*cos(stand_ang);
bend_z   = boot_zz - cable_bend_r*sin(stand_ang);
lead_low = bend_z - cable_bend_r - cable_d/2;                 // the bent lead's underside at its lowest
assert(!opt_usb || lead_low >= 0, str("1.69 stand: a lead bent ", cable_bend_r, " toward the back bottoms out ",
                                      -lead_low, " mm under the desk — raise cable_drop or bend tighter"));
slot_wall = usb_ch_h/2 + 0.4;                                 // the slot's rear wall behind the plug's axis
assert(cable_d/2 < slot_wall, "1.69 stand: cable_d is fatter than the boot channel the lead follows");
assert(cable_bend_r > slot_wall, "1.69 stand: cable_bend_r turns inside the slot's own width — no crown to derive");
bend_in  = acos((cable_bend_r - slot_wall)/(cable_bend_r - cable_d/2));
chan_h   = max(boot_zz - (cable_bend_r - slot_wall)*tan(bend_in)*cos(stand_ang) - slot_wall*sin(stand_ang),
               bend_z - cable_bend_r + cable_d/2) + 1.0;      // the crown: 1.0 over the lead's highest point behind the wall
// TIP-OVER (2D, about the base's front and rear edges). The slab alone, as a
// uniform block xo x yo x slab_t on a weightless stand — the stand's own
// weight only adds restoring moment, so leaving it out is conservative. The
// rest line above puts the slab's CG over the base's center (cg_y = 0), so
// the margin is stand_d/2 both ways; the slab's top-front corner is
//   top_z = seat_z + yo·cos(a) + slab_t·sin(a)   (79.7 at defaults)
// A horizontal push F at top_z tips the assembly about an edge m from cg_y
// when F·top_z > W·m, so the push it survives is W·m/top_z — 26/79.7 = 0.33
// of the slab's weight either way at defaults. The rule is tip_push_min();
// stand_d is the lever.
function tip_push_min() = 0.30;   // push at the slab's top edge it must survive, in slab weights
cg_y  = seat_y + yo/2*sin(stand_ang) - slab_t/2*cos(stand_ang);
top_z = seat_z + yo*cos(stand_ang) + slab_t*sin(stand_ang);
tip_back  = (stand_d/2 - cg_y)/top_z;
tip_front = (stand_d/2 + cg_y)/top_z;
assert(!opt_stand || (tip_back >= tip_push_min() && tip_front >= tip_push_min()),
       str("1.69 stand: a push of ", round(min(tip_back, tip_front)*100)/100,
           " x the slab's weight at its top edge tips it (rule ", tip_push_min(), ") — deepen stand_d"));
if (opt_stand)
    echo(str("Canary 1.69 stand — seat y ", round(seat_y*100)/100, " z ", round(seat_z*100)/100,
             " (pedestal ", round(ped_h*100)/100, "); boot ends ", cable_drop,
             " over the desk, bent lead ", round(lead_low*100)/100, "; channel crown ",
             round(chan_h*100)/100, "; tip push back ", round(tip_back*100)/100,
             " / front ", round(tip_front*100)/100, " x slab weight"));

module stand() {
    T   = slab_t;
    a   = stand_ang;
    fw  = stand_w - 16;
    yr  = seat_y;                            // the rest line = the slab's rear-bottom edge
    cy  = yr - T*cos(a);  cz = seat_z + T*sin(a);   // the slab's lifted front-bottom corner
    // front face of the slab at height z (z >= cz): y = cy + (z - cz)*tan(a)
    function yf(z) = cy + (z - cz)*tan(a) - 0.2;
    lip_h = 10;
    // the pedestal: from the lip's foot to the fin's back, the seat its top
    ped_front = yf(seat_z) - 3;
    ped_back  = yr + 8;
    assert(!opt_usb || slot_y1 <= yr - 0.5,
           "the cable slot reaches the rest line — the slab's bottom edge would lose its land over the slot");
    difference() {
        union() {
            linear_extrude(stand_t) rrect2d(stand_w, stand_d, 6);
            translate([-fw/2, ped_front, stand_t - 0.01])                  // pedestal
                cube([fw, ped_back - ped_front, ped_h + 0.01]);
            // front lip: a wedge whose back face follows the slab's front face
            hull() {
                translate([-fw/2, yf(seat_z) - 3, seat_z - 0.01]) cube([fw, 3, 0.02]);
                translate([-fw/2, yf(seat_z + lip_h) - 3, seat_z + lip_h - 0.02]) cube([fw, 3, 0.02]);
            }
            // reclined fin: front face through the rear-bottom edge, leaning stand_ang
            hull() {
                translate([-fw/2, yr - 0.01, seat_z - 0.01]) cube([fw, 8, 0.02]);
                translate([-fw/2, yr - 0.01 + 34*tan(a), seat_z + 34 - 0.02]) cube([fw, 8, 0.02]);
            }
        }
        if (opt_usb) cable_channel(seat_z + lip_h);
    }
    assert(yr + 3 + 34*tan(a) + 8 <= stand_d/2, "the fin's top runs off the stand's back edge — deepen stand_d or lower stand_ang");
    assert(ped_front >= -stand_d/2 + 2, "the front lip runs off the stand's front edge — deepen stand_d");
}

// The cable channel on the plug's x, front edge to rear edge. In front of the
// plug's rear edge a slot through base, pedestal and lip, open to the front
// and the top, its rear wall reclined with the plug so the pedestal keeps
// everything the plug does not sweep. Behind that wall a tunnel under the
// seat: cable_w wide, chan_h to its crown, bridged on the catalog's
// chamfered-top profile (port_bridge_profile2d — 7.0 of flat, the
// print-validated ceiling; the Watch stand's channel takes the same profile).
module cable_channel(top) {
    a = stand_ang;
    translate([-usb_dx, 0, 0]) {
        intersection() {
            translate([0, slot_y1, seat_z]) rotate([-a, 0, 0])
                translate([-cable_w/2, -200, -100]) cube([cable_w, 200, 300]);
            translate([-cable_w/2 - 1, -stand_d/2 - 1, -0.1])
                cube([cable_w + 2, stand_d + 2, top + 1.1]);
        }
        translate([0, stand_d/2 + 1, (chan_h + 0.1)/2 - 0.1]) rotate([90, 0, 0])
            linear_extrude(stand_d + 2) port_bridge_profile2d(cable_w, chan_h + 0.1);
    }
}

// CABLE GATE — a spec-max Type-C boot and its lead, seated as they leave the
// port, against the stand AND the desk (the half-space under z = 0):
//   cable_probe   — must be EMPTY: the boot (usb_ch_w x usb_ch_h x plug_l from
//                   the receptacle face, reclined with the slab), 25 mm of
//                   straight lead past it, and the same lead bent cable_bend_r
//                   toward the back and run out the rear edge at its lowest
//   cable_control — the same with the boot 6 mm longer: must NOT be empty
//                   (the straight lead reaches the desk), so the gate can fail
module lead(extra = 0) {
    a = stand_ang;
    L = plug_l + extra;
    translate([-usb_dx, seat_y + rx_y, seat_z + rx_z]) rotate([-a, 0, 0]) {
        translate([-usb_ch_w/2, -usb_ch_h/2, -L]) cube([usb_ch_w, usb_ch_h, L]);   // the boot
        translate([0, 0, -L - 25]) cylinder(d = cable_d, h = 25.01);             // straight lead
    }
    // the bend: from the boot's end at angle 180 − a about (bend_y, bend_z),
    // swept 90 + a to straight down (the boot's extra shifts it down the axis)
    translate([-usb_dx, bend_y - extra*sin(a), bend_z - extra*cos(a)]) {
        rotate([90, 0, 90]) rotate([0, 0, 180 - a])
            rotate_extrude(angle = 90 + a) translate([cable_bend_r, 0]) circle(d = cable_d);
        translate([0, 0, -cable_bend_r]) rotate([-90, 0, 0])
            cylinder(d = cable_d, h = stand_d/2 + 5 - bend_y);                   // the run out the back
    }
}
module stand_and_desk() {
    stand();
    translate([-stand_w, -stand_d, -50]) cube([2*stand_w, 2*stand_d, 50]);   // the desk, below z = 0
}

// ----------------------------------------------------------------------------
if      (part == "bezel") bezel_print();
else if (part == "back")  back();
else if (part == "stand") stand();
else if (part == "cable_probe")   intersection() { lead();  stand_and_desk(); }
else if (part == "cable_control") intersection() { lead(6); stand_and_desk(); }
else {
    bezel_print();
    translate([xo + 12, 0, 0]) back();
    if (opt_stand) translate([0, -(yo/2 + stand_d/2 + 12), 0]) stand();
}
