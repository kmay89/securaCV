// ============================================================================
//  Canary — ESP32-C6-LCD DISPLAY CASE  ⚠️ IN DEVELOPMENT (v0.5-dev)
//  A pocket portrait-display witness for the Waveshare ESP32-C6-LCD boards —
//  our newest firmware-display target. Two board variants, one file:
//    headers="none" — stripped board: header pins NOT soldered and the four
//                     factory brass M2 corner pillars removed, so the case
//                     stays shallow (the back-mounted USB shell sets depth).
//    headers="male" — the board as it ships assembled: pin headers soldered
//                     pointing DOWN (away from the glass) AND the brass M2
//                     corner pillars installed. The cavity deepens to swallow
//                     header base + pins, the press bosses land on the pillar
//                     TOPS (flat brass — the best press pads on the board),
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
//  FIT LESSONS from the first print + the board photos (don't undo these):
//    * The BOOT/RST buttons overhang the PCB edge (~1.8 mm), so the board
//      could not reach its seat — the side walls now carry full-depth
//      clearance channels ("ears"): the wall bulges outward locally and the
//      channel runs from the rear opening all the way to the seat, so the
//      button overhangs slide in. The access holes drill through the ear skin.
//    * The USB-C shell overhangs the bottom edge the same way (~1.8 mm) —
//      the bottom wall gets the same treatment (a "chin"), and the port
//      opening is now a true stadium (full-round ends, like the connector
//      itself) instead of a rectangle, sized shell + tolerance.
//    * USB-C and BOTH buttons are mounted on the BACK of the PCB (photo-
//      verified), not the front: the port center sits ~usb_h/2 behind the
//      PCB back face and the button actuators ~btn_dz behind it. That also
//      means even the stripped board needs back clearance for the shell
//      (back_stack ≥ ~4.7 keeps a printable bridge above the port), and the
//      snap skirt gets relief windows over the USB / button spans so it
//      can't land on back-mounted bodies at the board edges.
//    * Second fit test: big clearance cutouts read as sloppy gaps. The
//      button cutout now hugs the BLACK ACTUATOR only (narrow deep channel +
//      a shallow relief for the metal body edge), the actuator ends up
//      slightly recessed behind a small tool hole — pressable, not bumpable
//      — and the stadium tightened to shell + 0.2 (the wider insertion
//      notch hides behind the skin). What looked like "pins too long" was
//      the pins catching the fat Ø5.2 boss SIDES — slimming the bosses to
//      Ø4.6 fixed it at the original hdr_drop 8.8; don't deepen the case.
//    * The lid is 180°-rotation SYMMETRIC (nubs at ±nub_y0, mirrored skirt
//      reliefs, centered grille, plain plate outline) so it clicks on either
//      way and the keyhole can hang the case port-up or port-down. Keep any
//      new lid feature symmetric under (x,y)→(−x,−y), or flipping breaks.
//
//  Model presets are parametric — `model = "1.47"` is dimensioned from the
//  Waveshare mechanical drawing; the 1.69 preset lands when its drawing does.
//
//  Orientation: +Y = up (portrait), USB-C exits the BOTTOM (−Y) short wall,
//  +Z = toward the glass. All parts print flat, no supports.
//
//  ⚠️ DEV STATUS: two fit prints in — the board inserts and seats; v0.4
//     tightens the button/USB reveals from the second test and deepens the
//     headers build. The tightened windows are cut to photo measurements,
//     not calipers — MEASURE your board (btn_up, btn_proud, btn_body_p,
//     usb_dz, hdr_drop, brass_h) before committing to a long print.
//     2026-08-23 v0.5: adopt the shared libs (core/mount/snap/board) — the
//     keyhole becomes the catalog's BLIND pocket on an inside pad (the back
//     grille steps outboard of it), pcb_t takes the ws147 drawing's 1.6, the
//     snap windows derive from nub_w, and the window rim gets the C3
//     sibling's glass-relief band.
// ============================================================================

use <canary_core_lib.scad>    // rrect2d + the catalog's process floors
use <canary_mount_lib.scad>   // the stud/keyhole standard — the blind pocket
use <canary_snap_lib.scad>    // snap arithmetic — the derived-window rule
use <canary_board_lib.scad>   // the board registry — ws147 family numbers

/* [What to render] */
part  = "all";      // ["bezel","back","all"]
model = "1.47";     // ["1.47"]  board preset (the 1.69 lives in canary_s3_touch169.scad)
assert(model == "1.47", "the \"1.69\" preset here was a placeholder with invented dimensions — use canary_s3_touch169.scad");
// board variant: "none" = stripped (no headers, corner pillars removed), "male" = as shipped (down-facing headers + brass corner pillars)
headers = "none";   // ["none","male"]

/* [Board] — ESP32-C6-LCD-1.47. From the Waveshare drawing (mm); the 1.47
   outline is the ws147 family record in canary_board_lib. */
// long axis (portrait height, Y), short axis (X), PCB thickness
board_l = (model == "1.69") ? 36.0  : 36.37;   // 1.69 = MEASURE (placeholder)
board_w = (model == "1.69") ? 25.0  : 20.32;
pcb_t   = 1.6;       // brd_t("ws147") — the drawing-backed family thickness;
                     // the drawing does not call it out, so MEASURE stays
lcd_rise   = 3.65;   // LCD glass front above the PCB front face — the drawing's
                     // 5.10 overall stack minus the 1.45 this file used to guess
                     // for the PCB. The C3 sibling prints fine carrying the same
                     // 3.65 beside a 1.6 board, so the 0.15 disagreement rides
                     // as cavity headroom until calipers settle it — MEASURE
back_stack = 4.8;    // back-side clearance below the PCB, stripped board: the
                     // BACK-mounted USB shell (≈3.3) is the tallest thing, plus
                     // a printable bridge over the port opening — MEASURE

/* [Headers] — the "male" variant only: rows run along the two long (±X)
   edges, pins point down toward the back cover, and the factory brass M2
   pillars stand on the four corners. All are MEASURE. */
hdr_drop = 8.8;      // cavity depth below the PCB back that swallows base + pins
                     // (fit-tested: 8.8 closes fine — the earlier "pins foul the
                     // lid" was the boss Ø, fixed by the slimmer Ø4.6 bosses) — MEASURE
hdr_inset = 1.6;     // PCB edge → header row centerline — MEASURE
brass_h = 3.0;       // factory corner pillar height above the PCB back (0 =
                     // pillars removed) — the C3 sibling's pillars MEASURED 3.0
                     // (brd_ws147_brass_c3) on the same PCB family; the 5.0 this
                     // carried was a guess that left the bosses 2.0 short of the
                     // pillar tops (the C3's print-2 floating board) — MEASURE

/* [Screen] — active area = the window; the LCD module border sits under the lip */
aa_l  = (model == "1.69") ? 27.972 : 32.35;    // active-area long (Y) — 1.69 = MEASURE
aa_w  = (model == "1.69") ? 27.972 : 17.39;    // active-area short (X)
lcm_l = (model == "1.69") ? 32.0   : 36.28;    // LCD module outline long
lcm_w = (model == "1.69") ? 28.0   : 19.39;    // LCD module outline short

/* [Glass protection] — the panel's front face IS glass, and glass fails from
   stress at its edges. The bezel face touches the panel only on a LAND over
   the module's outer border: the innermost glass_relief_w of the lip is
   relieved glass_relief deep, so the window's rim never puts a contact line
   on the glass edge — a contact line at a cut edge is exactly the stress
   raiser this panel must not see (the C3 sibling's "locate, don't clamp"). */
glass_relief   = 0.25;  // face stand-off over the innermost lip band
glass_relief_w = 0.5;   // width of that relieved band, from the window edge out

/* [USB-C] — on the BOTTOM (−Y) short wall, mounted on the BACK of the PCB
   (photo-verified): the shell rests on the PCB back face, so the opening
   centers ~usb_h/2 behind it. The opening is a stadium (full-round ends,
   radius = half its height) hugging the receptacle shell — nominal shell is
   8.94 × 3.26; usb_h carries extra because the bezel prints face-down and
   holes shrink a touch along the print Z. */
usb_w  = 9.15;   // stadium opening width — shell + 0.2 (fit-tested: 9.4 showed a gap)
usb_h  = 3.45;   // stadium opening height — shell + 0.2
usb_dx = 0.0;    // sideways offset of the connector center — MEASURE
usb_dz = 0.0;    // depth offset from shell-on-back-face nominal (+ = toward the back) — MEASURE
usb_proud = 1.9; // shell overhang past the PCB edge (photo ≈1.8) — MEASURE

/* [Buttons] — BOOT/RST flank the USB end on the two side (±X) walls,
   mounted on the BACK of the PCB (photo-verified) with side-facing
   actuators. They overhang the PCB edge, so each side wall carries a
   full-depth clearance channel (an "ear") the overhang slides down during
   insertion. */
opt_btn = true;
btn_d   = 3.0;                 // access hole Ø — big enough for a tool tip, small enough to shield
btn_up  = 7.0;                 // button center up from the USB (−Y) end (fit-tested: 6.0 sat 1 low) — MEASURE
btn_dz  = 1.0;                 // actuator center behind the PCB BACK face — MEASURE
btn_proud = 1.8;               // BLACK ACTUATOR overhang past the PCB edge (photo ≈1.8) — MEASURE
btn_ch_w  = 3.4;               // actuator channel width (Y) — hugs the black nub, nothing more
btn_body_w = 5.2;              // shallow relief width for the switch's metal body — MEASURE
btn_body_p = 0.4;              // metal body overhang past the PCB edge — MEASURE

/* [Mount] */
opt_keyhole = true;            // one blind keyhole in the back (wall hang)
// catalog standard — canary_mount_lib. This file had drifted to slot 7.0 /
// depth 3.0: a slide the standard stud's head never finishes and a pocket it
// bottoms out in 0.4 early. The knobs stay tunable; the lib is the default.
kh_head_d = 7.0; kh_shank_d = 4.2; kh_slot_l = 8.0; kh_head_h = 3.5; kh_face = 1.0;

/* [Ventilation] — let the backlight/regulator heat convect out (side slots +
   a back grille). Even this small board runs warm on full brightness. */
opt_vent = true;
vent_n = 5;          // slots per side wall
vent_pitch = 5.0;    // slot spacing
vent_w = 1.4;        // slot width

/* [Print tolerances] — the catalog defaults (core_tol_* in canary_core_lib);
   tune with canary_fit_coupon.scad */
tol_slide = 0.20; tol_press = 0.10; tol_hole = 0.30;

/* [Shell] */
wall   = 2.2;    // deviates: snap-shell wall — the band pocket (snap_depth 1.4) + the 0.8 web behind it
face_t = 2.0;    // bezel face (over the glass border)
back_t = 2.0;    // rear cover plate
r_out  = 3.0;    // outer corner radius
// ⚠️ NO `lid_edge` — same removal, same reason, as the C3 sibling this case
// shares its board and panel with. The cut called itself a "face edge
// chamfer" but was an untapered extrude, so it drew a square RABBET: off
// the exported mesh, the first 0.80 mm of the bezel measured 25.520 x
// 40.670 against 27.120 x 42.270 above it — a 0.80 mm ledge overhanging air
// around the whole perimeter, on the first layers of a face-down print, on
// the one face a person looks at. The slicer already compensates the
// elephant foot; modeling 0.80 mm of relief over its ~0.2 mm is a second
// helping of the same correction. Flat on the plate, full footprint. A
// softened front edge, if ever wanted, is a REAL taper — see
// foot_chamfer_cut() in the WAP and Vision cases.
ear_skin = 1.2;  // wall skin left outside a button/USB clearance channel

/* [Snap fit] — back skirt into the bezel walls. The skirt is SHALLOW in
   both variants — just deep enough to carry the nubs: the board's edge zone
   behind the PCB is busy (back-mounted USB shell and buttons on the
   stripped board; descending header pins on the assembled one), so a deep
   edge-riding skirt has nothing safe to ride on. It also gets relief
   windows over the USB and button spans, and goes THIN (1.0 wall) in the
   headers variant to stay outboard of the pin rows. The nub sits at
   back_t + snap_depth so it lands on the skirt AND lines up with the bezel
   window; keep snap_depth + snap_h/2 ≤ skirt_dep. */
snap_n = 4; snap_h = 1.6; snap_depth = 1.4; snap_proud = 0.5;
// THE WINDOW IS DERIVED FROM THE RIDGE (canary_snap_lib's rule; the C3
// sibling's lid-slide print taught it). One knob — snap_w — used to place the
// ridge's end plates AND size the window, but the end plates draw 1.0 in from
// it per side: a snap_w of 5.0 drew a 3.1 ridge in a 5.0 window, ±0.95 of
// free travel for the lid to rattle across. The ridge's DRAWN width is the
// parameter now; the window follows it through snap_window().
nub_w = 3.1;         // the ridge's drawn width in Y — exactly what the old
                     // arithmetic drew, so the click feel is unchanged
snap_play = 0.15;    // window clearance per side — the catalog default
                     // (canary_snap_lib); a printed window comes out a hair
                     // small and the lid still has to enter

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
z_pcb_back  = z_pcb_front + pcb_t;               // PCB back plane
// opening centers on the SHELL (nominal 3.26 tall, resting on the PCB back),
// not on the opening height, so tightening usb_h never shifts it off the port
z_usb       = z_pcb_back + 3.26/2 + usb_dz;
z_btn       = z_pcb_back + btn_dz;               // button actuator center (back-mounted)

// button / USB overhang clearance: how far each channel reaches past the
// cavity wall face, and how far the wall must bulge to keep ear_skin of skin
btn_y     = -board_l/2 + btn_up;         // button center (Y)
btn_reach = btn_proud + tol_slide;       // actuator channel depth past the cavity face
btn_body_reach = btn_body_p + tol_slide; // shallow body-relief depth
usb_reach = usb_proud + tol_slide;
usb_slide = usb_w + 0.35;                // insertion-notch width — a hair looser than the
                                         // visible stadium so the shell can't bind on the way in
ear_bump  = max(0, btn_reach + ear_skin - wall);   // side-wall bulge
chin_bump = max(0, usb_reach + ear_skin - wall);   // bottom-wall bulge
ear_w  = btn_ch_w + 4;                   // ear bulge width along the wall
chin_w = usb_w + 4;                      // chin bulge width along the wall

// active-area window vs module: the face overlaps the module border by
// (module − AA)/2 per side; that overlap retains the glass. The LAND is what
// is left of the lip outside the relieved band — that ring is the panel's
// only contact.
lip_l = (lcm_l - aa_l)/2;   // long-side lip (Y)
lip_w = (lcm_w - aa_w)/2;   // short-side lip (X)
land_w_x = lip_w - glass_relief_w;
land_w_y = lip_l - glass_relief_w;

// snap skirt: shallow in both variants, thin in headers mode (see [Snap fit])
skirt_wall = (headers == "male") ? 1.0 : 1.6;
skirt_dep  = snap_depth + snap_h/2 + 0.4;
skirt_x = xc - 2*tol_press;   skirt_y = yc - 2*tol_press;
// the snap WINDOW, derived from the ridge that has to sit in it — never typed
snap_w = snap_window(nub_w, snap_play);

// press bosses sit over the M2 corner pillar positions in both variants: on
// the assembled board they land on the flat brass pillar TOPS (boss length
// shortens by brass_h and widens to cover the pillar); on the stripped board
// they press the bare PCB corners
stand_ix = 2.6;                                  // inset from ±X edge (M2 pattern) — MEASURE
stand_iy = 2.6;                                  // inset from ±Y edge (M2 pattern) — MEASURE
stand_d  = (headers == "male") ? 4.6 : 4.2;      // boss Ø — covers the pillar top but stays
                                                 // clear of the last header pin of each row
stand_len = stack_eff - ((headers == "male") ? brass_h : 0);

// keyhole host arithmetic — canary_mount_lib's blind rule: the pocket is
// kh_head_h deep and wants 1.5 of web behind it so it never breaches the
// cavity. The 2.0 plate alone cannot give that; kh_pad_h is what the inside
// pad makes up. The pad's footprint wraps the pocket's widest feature (the
// head channel, head_d + 0.6) in a structural wall per side.
kh_host  = kh_head_h + 1.5;
kh_pad_h = max(0, kh_host - back_t);
kh_pad_x = kh_head_d + 0.6 + 2*core_min_wall();
kh_pad_y = kh_slot_l + kh_head_d + 0.6 + 2*core_min_wall();
// back-grille columns step outboard of the pad — a through slot into a blind
// pocket would un-blind it (and vent nothing: it would dead-end in the pad)
grille_x = opt_keyhole ? kh_pad_x/2 + 1.4 : 4.2;   // 0.4 clear + half the 2.0 slot

// snap windows / nubs live on the two long (±X) side walls, at ±nub_y0 —
// SYMMETRIC about the center, so the lid also clicks on rotated 180° and the
// keyhole can hang the case port-up or port-down (flip the lid). Everything
// on the lid↔bezel interface must stay symmetric under (x,y)→(−x,−y):
// nubs/windows, skirt reliefs, bosses, grille. The keyhole itself may be
// asymmetric — flipping the lid is what re-aims its slot.
nub_y0 = 5.0;    // clear of the button channel below and the vents above
function nub_ys() = [-nub_y0, nub_y0];

// wall vents: keep the band between the glass ledge and the snap windows,
// and shift the row up clear of the button ears
vent_z0 = face_t + 1.0;
vent_z1 = bez_h - (snap_depth + snap_h/2) - 0.8;
vent_dy = opt_btn ? (btn_y + ear_w/2) + (vent_n-1)*vent_pitch/2 + vent_w/2 + 1.2 : 0;

assert(aa_l < lcm_l && aa_w < lcm_w, "active area must be inside the module outline");
assert(lip_w >= 0.8, "short-side lip < 0.8 mm won't retain the glass — check aa_w/lcm_w");
assert(land_w_x >= 0.4 && land_w_y >= 0.4,
       str("glass land is ", land_w_x, " / ", land_w_y, " mm — too narrow to ",
           "carry the panel. Shrink glass_relief_w; the relief must never eat ",
           "the land that locates the glass."));
assert(lcm_l <= yc && lcm_w <= xc, "LCD module larger than the board cavity — check dims");
assert(z_usb - usb_h/2 >= face_t - 0.01, "USB opening cuts into the bezel face — check usb_h/usb_dz");
assert(z_usb + usb_h/2 <= face_t + cav_d - 0.8,
       "no printable bridge left between the USB opening and the rear rim — raise back_stack/hdr_drop or lower usb_dz");
assert(z_btn + btn_d/2 <= face_t + cav_d, "button hole overruns the cavity depth — check btn_dz/back_stack");
assert(skirt_dep <= stack_eff + 0.01, "skirt_dep > component clearance — the skirt would drive into the PCB");
assert(skirt_dep >= snap_depth + snap_h/2, "skirt too short to carry the snap nub (nub sits at back_t + snap_depth)");
assert(stand_len >= 0.6, "press bosses shorter than 0.6 — brass_h nearly fills the cavity; check hdr_drop/brass_h");
assert(headers != "male" || hdr_drop >= brass_h + 0.5,
       "corner pillars taller than the cavity below the PCB — check brass_h/hdr_drop");
assert(!opt_btn || min([for (y = nub_ys()) abs(y - btn_y)]) >= max(btn_ch_w, btn_body_w)/2 + snap_w/2 + 1.0,
       "a snap window overlaps the button clearance cutouts — shift nub_y0/btn_up");
assert(!opt_vent || vent_z1 - vent_z0 >= 1.5, "no room for wall vents between ledge and snap band — set opt_vent=false");
assert(!opt_vent || vent_dy + (vent_n-1)*vent_pitch/2 + vent_w/2 <= yc/2 - 0.8,
       "vent row overruns the side wall — fewer slots (vent_n) or tighter pitch");
assert(headers != "male" || hdr_drop > back_stack, "headers=male but hdr_drop is shallower than the stripped-board clearance");
assert(headers != "male" || tol_slide + hdr_inset - 0.35 >= tol_press + skirt_wall + 0.25,
       "skirt would sit in the header pin row — thin skirt_wall or re-measure hdr_inset");
assert(board_w/2 - stand_ix - stand_d/2 > 0, "press bosses collide at the board center — check stand_ix/stand_d");
// the keyhole pad stands in the cavity, so everything around it gets a gate
assert(!opt_keyhole || stack_eff - kh_pad_h >= 1.5,
       str("the keyhole pad tops out only ", stack_eff - kh_pad_h,
           " mm short of the PCB back — the board's center-back parts live ",
           "under that footprint (the 3.3 USB shell is at the edge; the SoC ",
           "and friends are not). MEASURE before trusting less than 1.5."));
assert(!opt_keyhole || (kh_pad_x/2 <= skirt_x/2 - skirt_wall - 0.3
                     && kh_pad_y/2 <= skirt_y/2 - skirt_wall - 0.3),
       "the keyhole pad runs into the snap skirt — shrink kh_head_d/kh_slot_l");
assert(!opt_keyhole || headers != "male" || kh_pad_x/2 <= board_w/2 - hdr_inset - 1.5,
       "the keyhole pad reaches into the descending header-pin rows — shrink kh_head_d");
assert(!opt_vent || grille_x + 1.0 <= skirt_x/2 - skirt_wall - 0.3,
       "the back grille runs into the skirt ring — the keyhole pad has pushed it too far out");
echo(str("Canary C6 display (", model, ", headers=", headers, ") v0.5-dev — outer ",
         xo, " x ", yo, " x ", bez_h + back_t, " mm, window ", aa_w, " x ", aa_l,
         " (lip X ", lip_w, " / Y ", lip_l, ", land ", land_w_x, " / ", land_w_y,
         "), cavity depth ", cav_d,
         "  (IN DEVELOPMENT — MEASURE)"));

// stadium: a rounded slot with full-round ends (radius = half the height) —
// the same profile as a USB-C shell (rrect2d is canary_core_lib's)
module stadium2d(w, h) { rrect2d(w, h, h/2 - 0.05); }

// bezel outline: body + the ear / chin bulges. The BACK PLATE deliberately
// does NOT share it — the bulges are asymmetric in Y, and the lid must stay
// 180°-rotation symmetric so it clicks on either way (port-up / port-down
// hanging). The bulges therefore end at the parting line as a small ledge
// that belongs to the bezel.
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
// They are hidden behind the walls / under the lid; only the small access
// hole and the stadium pierce the outside.
module cavity2d() {
    rrect2d(xc, yc, r_in);
    if (opt_btn) for (sx = [1, -1]) {
        // deep, narrow: the black actuator only
        translate([sx*xc/2, btn_y]) square([2*btn_reach, btn_ch_w], center = true);
        // shallow, wider: the switch's metal body edge
        translate([sx*xc/2, btn_y]) square([2*btn_body_reach, btn_body_w], center = true);
    }
    translate([usb_dx, -yc/2]) square([usb_slide, 2*usb_reach], center = true);
}

// ----------------------------------------------------------------------------
//  BEZEL — front frame, prints face-down (z0 = outer face)
// ----------------------------------------------------------------------------
module bezel() {
    difference() {
        linear_extrude(bez_h) shell_outline2d();                      // face + walls
        // active-area window through the face (aa_w = X, aa_l = Y)
        translate([0, 0, -0.1]) linear_extrude(face_t + 0.2) rrect2d(aa_w, aa_l, 1.5);
        // (no foot relief on the face — see [Shell]. Flat and full-size on
        //  the plate; the slicer's elephant-foot compensation is the only
        //  relief this part gets or needs.)
        // glass relief: stand the face off the innermost lip band, so the
        // window's rim never touches the panel (see [Glass protection])
        translate([0, 0, face_t - glass_relief]) linear_extrude(glass_relief + 0.02)
            rrect2d(aa_w + 2*glass_relief_w, aa_l + 2*glass_relief_w, 1.5 + glass_relief_w);
        // board cavity (with overhang channels) behind the glass ledge
        translate([0, 0, face_t]) linear_extrude(cav_d + 0.2) cavity2d();
        // USB-C stadium opening through the bottom (−Y) wall + chin
        translate([usb_dx, -yo/2, z_usb]) rotate([90, 0, 0])
            linear_extrude(2*(wall + chin_bump + 1), center = true) stadium2d(usb_w, usb_h);
        // BOOT / RST side access holes near the USB end, through the ear
        // skin — the actuators sit BEHIND the PCB (back-mounted switches)
        if (opt_btn) for (sx = [1, -1])
            translate([sx*xo/2, btn_y, z_btn])
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
            linear_extrude(back_t) rrect2d(xo, yo, r_out);   // plate — symmetric, flippable
            translate([0, 0, back_t - 0.01]) linear_extrude(skirt_dep)      // skirt ring
                difference() {
                    rrect2d(skirt_x, skirt_y, r_in);
                    rrect2d(skirt_x - 2*skirt_wall, skirt_y - 2*skirt_wall, max(0.4, r_in - skirt_wall));
                    // relief windows: the back-mounted USB shell / buttons
                    // occupy the edge zone the skirt passes through. Cut
                    // MIRRORED pairs so the lid still seats rotated 180°.
                    for (sy = [1, -1])
                        translate([sy*usb_dx, -sy*skirt_y/2]) square([usb_w + 2, 3*skirt_wall], center = true);
                    if (opt_btn) for (sx = [1, -1], sy = [1, -1])
                        translate([sx*skirt_x/2, sy*btn_y]) square([3*skirt_wall, btn_body_w + 2], center = true);
                }
            // press bosses over the M2 corner pillar positions push the board
            // forward onto the bezel glass ledge — onto the flat brass pillar
            // tops on the assembled board, onto bare PCB on the stripped one
            for (sx = [1, -1], sy = [1, -1])
                translate([sx*(board_w/2 - stand_ix), sy*(board_l/2 - stand_iy), back_t - 0.01])
                    cylinder(d = stand_d, h = stand_len + 0.01);
            // the keyhole's host: a kh_head_h-deep blind pocket wants
            // kh_host of stock (the mount lib's web rule) and the plate is
            // back_t — the difference is a pad on the plate's INSIDE face,
            // the ear/chin local-thickening move aimed inward (outward would
            // cost the flat outer face this part prints on). The pad is
            // CENTERED, so the lid interface stays 180°-symmetric; only the
            // pocket inside it points one way, and flipping the lid is what
            // re-aims the slot.
            if (opt_keyhole)
                translate([0, 0, back_t - 0.01])
                    linear_extrude(kh_pad_h + 0.01) rrect2d(kh_pad_x, kh_pad_y, 2.0);
            // snap nubs on the short-wall (±X) skirt faces, chamfered both
            // ways (symmetric — the C3's ramp-and-catch nub would break the
            // flippable lid). The end plates span nub_w — the SAME number the
            // window is sized from, so the two cannot drift apart again.
            for (sx = [1, -1], yc0 = nub_ys())
                translate([sx*(skirt_x/2 - 0.3), yc0, back_t + snap_depth]) hull() {
                    for (dy = [-nub_w/2 + 0.05, nub_w/2 - 0.05])
                        translate([0, dy, 0]) cube([0.6, 0.1, snap_h - 0.4], center = true);
                    translate([sx*(snap_proud + 0.3), 0, 0]) cube([0.1, 0.1, 0.6], center = true);
                }
        }
        // heat-escape grille in the back plate, over the component zone —
        // CENTERED in both axes (it reads as the lid's face), columns stepped
        // outboard of the keyhole pad (a through slot into a blind pocket
        // would un-blind it), skipping any slot pair that would undermine a
        // press-boss foot (symmetric skip, so the grille stays centered)
        if (opt_vent) for (i = [0:vent_n-1], sx = [1, -1])
            let (gy = -(vent_n-1)*vent_pitch/2 + i*vent_pitch)
            if (abs(gy) + vent_w/2 <= board_l/2 - stand_iy - stand_d/2 - 0.2)
                translate([sx*grille_x, gy, back_t/2])
                    cube([2.0, vent_w, back_t + 0.4], center = true);
        // BLIND keyhole (wall hang; slot toward +Y/up) — the catalog pocket
        // from canary_mount_lib, drawn natively along Y: the head circle
        // passes the screw head behind a kh_face web and the shank rides to
        // the slot's far end as the case drops on. Blind means blind: it
        // never breaches the cavity, so nothing shows through the back. (It
        // cut clean through the 2.0 plate before the pad existed — hung on a
        // wall, the screw head sat INSIDE the case against the board.)
        if (opt_keyhole)
            mount_keyhole_pocket(0, 0, axis = "y",
                                 head_d = kh_head_d, shank_d = kh_shank_d,
                                 slot_l = kh_slot_l, head_h = kh_head_h,
                                 face = kh_face);
    }
}

// ----------------------------------------------------------------------------
if      (part == "bezel") bezel_print();
else if (part == "back")  back();
else {
    bezel_print();
    translate([xo + 10, 0, 0]) back();
}
