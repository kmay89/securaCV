// ============================================================================
//  Canary — ESP32-S3-LCD-1.47 "HALLWAY" CASE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//
//  A screwless case for the Waveshare ESP32-S3-LCD-1.47 — the USB-A STICK.
//  This is the hallway nightlight body: it plugs straight into a wall outlet
//  adapter, the 172x320 glass faces the corridor and shows the lamp, and the
//  onboard WS2812 washes the wall behind it through a window in the back.
//
//  ⚠️ NOT the C6 case. canary_c6_display.scad covers the ESP32-C6-LCD-1.47,
//     which carries the SAME 1.47" panel on a SIMILAR outline but is a
//     header board with a USB-C PORT on its short edge. This board ends in a
//     USB-A MALE PLUG, which changes the entire problem: there is no port
//     opening to seal, there is a 12 mm insertion length that must stay clear,
//     and the
//     drop case is "dropped ON the plug", not "dropped on a corner". Do not
//     merge the two files.
//
//  ── THE FOUR THINGS THIS CASE IS FOR ─────────────────────────────────────
//
//  1. THE PLUG FITS, AND KEEPS ITS FULL INSERTION LENGTH.
//     USB-A is a hard standard: the plug shell is 12.00 x 4.50 mm and needs
//     ~12 mm of clear length to seat in a receptacle. Anything the case adds
//     past the PCB's plug-end edge comes straight off that 12 mm. So the
//     plug-end wall is thin (`usb_wall`), and its outer face is CHAMFERED
//     BACK (`usb_relief`) so a receptacle recessed in a wall-wart housing
//     meets air instead of meeting this case. `usb_free` asserts what is
//     left; if it drops below 11 mm the render fails rather than shipping a
//     case that will not plug in all the way.
//     And the opening is a RECTANGLE, because series-A is a rectangle —
//     see `usb_a_2d`. The stadium that suits USB-C will not pass a series-A
//     shell's square corners.
//
//  2. NO SCREWS, AND THE SNAP FEELS DELIBERATE.
//     Four cantilever beams on the back's skirt, into undercuts in the bezel.
//     The feel is authored, not inherited:
//       - LEAD-IN 30°, so it guides itself in and closes with one push.
//       - RETURN ANGLE IS ASYMMETRIC, which is the whole trick. The two beams
//         at the PLUG end return at `snap_ret_lock` (steep — those never let
//         go, because that end takes the insertion and removal forces every
//         time the device is plugged in). The two at the FAR end return at
//         `snap_ret_free` (shallower — those are the ones a thumb releases).
//       - The beams are sized by STRAIN, not by eye: see the ε calculation at
//         `snap_beam_l`. PETG takes about 1.8% repeatedly; a beam short
//         enough to feel stiff is a beam that goes white and then snaps off
//         on the third opening.
//
//  3. IT SURVIVES BEING DROPPED, WHICH FOR A STICK MEANS ONE THING.
//     A USB stick lands on its plug. The failure is not the case cracking —
//     it is the plug levering against the PCB and tearing its solder joints.
//     So the plug root gets a COLLAR (`collar_*`): a thick ring of case that
//     surrounds the shell right where it leaves the board, sized to bottom
//     out against the shell before the shell can rotate far enough to load
//     the joint. The collar is the single most important feature in this
//     file. Everything else is comfort.
//     Secondary: the PCB is captured on COMPLIANT ribs with a small preload
//     (`preload`) rather than pinched rigid between two hard faces — a rigid
//     capture turns every impact into a bending moment across the board.
//
//  4. THE CARD COMES OUT WITHOUT TOOLS.
//     Two ways, because the microSD slot's exact mouth is a MEASURE item:
//       - A window in the back over the slot (`sd_window`), so a card can be
//         changed with the case shut, and
//       - the THUMB FLICK: the back stands a hair proud at the far end over a
//         20° cam ramp (`flick_*`). A thumb pushed along the body rides the
//         ramp and pops the two free-end snaps. It is deliberately at the end
//         AWAY from the plug, so the gesture pushes the device INTO its
//         socket rather than levering it out.
//
//  ── PRINTING ─────────────────────────────────────────────────────────────
//  Black PETG body, one YELLOW accent — the house mark (canary_mark_lib.scad)
//  inlaid into the back. Same two-filament logic the 7" frame uses and for
//  the same reason: a yellow case is a toy, a black case with one yellow mark
//  is a product. Export `fil_body` and `fil_accent` and load them as one
//  multi-part object (do NOT re-center).
//
//  PETG, not PLA, and not by accident: this thing lives in a wall outlet
//  next to a warm adapter, and PLA creeps at the temperature a charger brick
//  reaches. PETG also has the strain headroom the snap beams are sized
//  against — the numbers at `snap_beam_l` are PETG numbers.
//
//  Orientation: +Y = up (portrait), the USB-A plug exits the TOP (+Y) short
//  wall, +Z = toward the glass. Both printed parts lie flat, no supports.
//
//  ⚠️ DEV STATUS: dimensioned from the Waveshare mechanical drawing and the
//     USB-A standard, NOT from calipers on a board. Every number tagged
//     MEASURE is one to check before a long print — and on this board the
//     ones that will bite are the plug's overhang and offset (`usb_proud`,
//     `usb_dz`), the side buttons (`btn_*`) and the card slot (`sd_*`),
//     because the product photos do not dimension any of them.
//     There is also a non-stick "-LCD-1.47B" variant with a different
//     outline entirely — verify which board you have.
// ============================================================================

use <canary_vent_lib.scad>  // the brand vent shape: egg2d
use <canary_mark_lib.scad>  // the house mark: bird + wordmark lockup

/* [What to render] */
part = "all";   // ["bezel","back","light","all","exploded","fil_body","fil_accent","fit_section"]

/* [Board] — ESP32-S3-LCD-1.47, from the Waveshare drawing (mm) */
board_l = 36.37;   // PCB long axis (Y), EXCLUDING the USB-A plug
board_w = 20.32;   // PCB short axis (X)
pcb_t   = 1.6;     // MEASURE (drawing does not call it out; 1.6 is the usual)

// The LCD module: same 1.47" panel as the C6 sibling, so the same numbers.
lcd_rise = 3.65;   // glass front above the PCB front face (5.10 stack - 1.45)
aa_l  = 32.35;     // active area, long (Y)
aa_w  = 17.39;     // active area, short (X)
lcm_l = 36.28;     // LCD module outline, long
lcm_w = 19.39;     // LCD module outline, short

// Back-side clearance: the tallest thing behind the PCB sets this. On this
// board that is the microSD cage / the module can, not the USB shell (the
// shell is off the end, not under the board).
back_stack = 4.6;  // MEASURE

/* [USB-A plug] — the standard, plus what the board does with it */
// USB-A series-A plug shell, per the USB 2.0 mechanical drawing. These are
// NOT guesses and should not be "adjusted to fit" — if the plug does not pass
// the opening, the opening is wrong, not the standard.
usb_w = 12.00;     // shell width
usb_h = 4.50;      // shell height
usb_insert = 12.0; // shell length that must enter a receptacle
// How far the shell overhangs the PCB's plug-end edge. MEASURE THIS FIRST —
// it is the number the whole plug end is built on, and the assert below is
// what stands between a wrong value and a case that cannot plug in. A series-A
// plug shell is ~14 mm long overall and is soldered overlapping the board, so
// ~14 mm past the edge is the realistic starting point, not the 12 mm
// insertion figure (that is what must remain AFTER the case wall).
usb_proud = 14.0;  // MEASURE
usb_dx = 0.0;      // shell center offset across the board (X) — MEASURE
usb_dz = 0.0;      // + = shell center sits further BEHIND the PCB — MEASURE
usb_clear = 0.35;  // per-side clearance around the shell in its opening

// The plug-end wall, and how far it is cut back so a recessed receptacle
// housing does not foul the case before the plug is home.
usb_wall = 1.8;
// Chamfer depth on the outer face around the opening. Bounded by how much
// wall there is above and below the plug — the shell straddles the board, so
// its bottom edge sits only a couple of millimetres above the bezel face and
// a greedy relief cuts straight through it. The two asserts below hold the
// line; if you want more relief, you need a deeper case, not a bigger number.
usb_relief = 1.0;

/* [Drop collar] — the reason this case exists in one piece */
collar_on = true;
collar_t = 2.6;    // ring thickness around the shell at its root
collar_l = 3.4;    // how far the ring reaches ALONG the plug from the wall
collar_gap = 0.25; // ring-to-shell gap: small enough to bottom out early,
                   // big enough that the ring is not a press fit on the shell

/* [Buttons] — BOOT and RST, side-mounted near the plug end */
opt_btn = true;
btn_d = 2.6;         // access hole Ø — a fingertip cannot, a pen tip can
btn_from_usb = 11.3; // button center down from the PCB's plug-end edge — MEASURE
                     // (the drawing's 11.31 dimension appears to be this span,
                     // which is exactly why it needs checking)
btn_dz = 1.0;        // actuator center behind the PCB BACK face — MEASURE
btn_proud = 1.6;     // actuator overhang past the PCB edge — MEASURE
btn_ch_w = 3.2;      // actuator channel width — hugs the nub, nothing more
ear_skin = 1.1;      // wall skin left outside a button clearance channel

/* [microSD] — the card the thumb flick is for */
// OFF by default, and that is the considered choice, not an oversight. The
// thumb flick IS the card story: the back comes off in a second and the slot
// is right there. A permanent window costs the one clean face this case has
// (the mark lives there), and it can only be cut in the right place once
// somebody has measured where the slot mouth actually is — which the product
// photos do not tell us. Turn it on after measuring `sd_from_usb`, and the
// assert below will make sure it does not land on the mark.
sd_window = false;
sd_from_usb = 15.0;  // slot MOUTH center, down from the plug-end edge — MEASURE
sd_w = 13.0;         // window width (card is 11 mm + finger room)
sd_l = 9.0;          // window length along the board

/* [RGB LED] — the wall wash */
// The WS2812 (GPIO38) is the ambient beacon. In a hallway the back faces the
// wall, so this window turns the beacon into a wash of color on the wall
// behind the device — which is the nicest thing this board does and would be
// completely hidden by a solid back.
led_win = true;
led_from_far = 6.5;  // window center up from the PCB's FAR edge — MEASURE
led_d = 7.0;         // window Ø
led_skin = 0.5;      // thickness of the diffuser plug that fills it

/* [Branding] — one yellow mark, on the back */
mark_show = true;
mark_h = 9.0;        // the BIRD's height; the wordmark scales off it
mark_rib = 0.9;      // stroke width. Below ~0.8 the mark stops being a mark
                     // at this size — two 0.42 lines is the floor
mark_depth = 0.7;    // deboss depth; the accent inlay fills it flush
mark_dy = 2.0;       // up from the back plate's center — the LED window owns
                     // the far end, so the lockup sits above it

/* [Vents] — the brand egg, and a real thermal need */
// The board README is explicit: "S3 + octal PSRAM heat-soak into a small
// stick that plugs into an already-warm port." A sealed stick would cook.
opt_vent = true;
vent_rows = 3;
vent_cols = 2;
vent_l = 4.0;        // egg long axis
vent_w = 2.2;        // egg short axis
vent_pitch_y = 5.0;
// The clutch lives on the ONE clear stretch of long wall — below the button
// ear and above the free-end snap beam. That gap is about 12.5 mm on this
// board, and three rows at this pitch very nearly fill it; the two asserts
// below are what stop a later tweak from quietly cutting a vent through a
// snap slot, which would turn a spring into a hinge.
vent_center_y = -2.95;

/* [Snap fit] — the feel */
// WHERE THE BEAMS LIVE, and why it is not the obvious place.
// The tempting arrangement is beams hanging off the back's skirt down into
// the bezel. It does not fit: the skirt sits over the board's edges (the
// cavity is only tol_slide wider than the PCB), so any skirt deep enough to
// carry an 8 mm beam lands ON the board long before it reaches its catch.
// So the cantilevers are cut into the BEZEL WALL instead — a U-slot frees a
// beam whose hook points INWARD at the rim, and the back plate simply has a
// groove for them. The wall has the height to make the beams long, the back
// plate stays a flat plate (which is what the branding wants), and nothing
// ever reaches into the board's space.
snap_w = 4.6;        // beam width
snap_beam_l = 8.0;   // free beam LENGTH — see the strain note below
snap_beam_t = 1.0;   // beam thickness (the wall is locally thinned to this)
snap_eng = 0.55;     // engagement depth (how far the hook stands proud)
snap_flat = 0.5;     // the flat that actually seats
snap_slot = 0.9;     // U-slot width freeing each side of the beam
snap_lead = 30;      // insertion lead-in angle, degrees
snap_ret_lock = 62;  // return angle at the plug end — effectively permanent
snap_ret_free = 38;  // return angle at the free end — releases with intent

/* [Thumb flick] — how the back comes off */
// A LIFT LUG, not a pry slot. The plate carries a small tab at its far end
// that stands proud of the rim; the bezel's far wall is scooped away there so
// the tab is reachable. A thumb pushed along the body meets the tab's ramped
// face and cams the plate up out of the two shallow-return hooks.
//
// Far end, deliberately: the gesture pushes the stick further INTO its socket
// rather than levering it out of the wall.
flick_w = 9.0;       // lug width
flick_proud = 1.1;   // how far the lug stands above the rim
flick_ramp = 22;     // the ramp face a thumb climbs, degrees
flick_scoop = 7.0;   // Ø of the scoop through the bezel rim that reaches it

/* [Shell] */
wall = 2.1;
face_t = 1.8;        // bezel face over the glass border
back_t = 2.0;        // rear plate
r_out = 3.2;         // outer corner radius
preload = 0.25;      // compliant squeeze on the PCB (rib crush), not a clamp

/* [Tolerances] */
tol_slide = 0.20;    // board into its cavity
tol_press = 0.10;    // skirt into the bezel
tol_hole = 0.30;

/* [Quality] */
$fa = 3; $fs = 0.35;

// ===========================================================================
//  DERIVED — nothing below is a knob
// ===========================================================================

// Board cavity and outer shell.
xc = board_w + 2*tol_slide;
yc = board_l + 2*tol_slide;
xo = xc + 2*wall;
yo = yc + 2*wall;
r_in = max(0.6, r_out - wall);

cav_d = lcd_rise + pcb_t + back_stack;   // glass ledge -> back plate inner
bez_h = face_t + cav_d;                  // bezel wall height

z_pcb_front = face_t + lcd_rise;
z_pcb_back  = z_pcb_front + pcb_t;
// The plug shell's center height. A series-A shell STRADDLES the board — the
// PCB tongue runs down the middle of the 4.50 mm shell, it does not sit under
// it — so the shell is centered on the PCB's mid-plane, not stacked behind its
// back face. Getting this wrong pushes the opening up into the rim and breaks
// the case open along its top edge.
z_usb = z_pcb_front + pcb_t/2 + usb_dz;   // MEASURE usb_dz
z_btn = z_pcb_back + btn_dz;

// The glass ledge: the bezel face overlaps the LCD module border, and the
// window shows the active area.
lip_l = (lcm_l - aa_l)/2;
lip_w = (lcm_w - aa_w)/2;

// ── THE INSERTION-LENGTH ASSERTION ─────────────────────────────────────────
// What is left of the plug once the case's plug-end wall has taken its cut.
// USB-A needs ~12 mm; below 11 the plug will not seat in a deep receptacle
// and the whole device is useless, so this is an assert and not a comment.
usb_free = usb_proud - usb_wall;
assert(usb_free >= 11.0,
       str("USB-A insertion length is only ", usb_free,
           " mm — the plug will not seat. Reduce usb_wall, or MEASURE ",
           "usb_proud (the shell's real overhang past the PCB edge)."));

// The plug opening, with its outer relief chamfer, has to stay INSIDE the end
// wall. If it does not, the case is open along an edge — and because the
// relief is what makes the opening look intentional, the failure is easy to
// introduce by tuning `usb_relief` alone.
usb_top = z_usb + usb_h/2 + usb_clear + usb_relief;
usb_bot = z_usb - usb_h/2 - usb_clear - usb_relief;
assert(usb_top <= bez_h - 0.6,
       str("The plug opening (top at ", usb_top, ") breaks out through the ",
           "rim (", bez_h, "). Check usb_dz and back_stack, or trim ",
           "usb_relief."));
assert(usb_bot >= face_t + 0.6,
       str("The plug opening (bottom at ", usb_bot, ") breaks out through the ",
           "bezel face. Check usb_dz."));

// ── THE SNAP-BEAM STRAIN CHECK ─────────────────────────────────────────────
// Cantilever with a rectangular section, deflected `snap_eng` at its tip:
//     ε = 1.5 · y · t / L²
// PETG tolerates roughly 1.8% strain on a joint meant to be opened again and
// again (short-term ultimate is higher, but designing to ultimate is how you
// get a case that opens three times). If this assert fires the fix is a
// LONGER or THINNER beam — never a shallower engagement, which is what makes
// a snap feel cheap.
snap_strain = 1.5 * snap_eng * snap_beam_t / (snap_beam_l * snap_beam_l);
assert(snap_strain <= 0.018,
       str("Snap beam strain is ", snap_strain*100,
           "% — above the ~1.8% PETG can take repeatedly. Lengthen ",
           "snap_beam_l or thin snap_beam_t."));

// Button geometry.
btn_y = yc/2 - btn_from_usb;              // +Y is the plug end
btn_reach = btn_proud + tol_slide;
ear_bump = max(0, btn_reach + ear_skin - wall);
// Ear width. Deliberately tight to the actuator: the C6 case's second fit
// test found that generous clearance cutouts read as sloppy gaps, and a
// narrow ear is also what leaves room on this wall for the plug-end snap
// beam (see the clash assert).
ear_w = btn_ch_w + 2.4;

// The back plate drops into the top of the cavity and its top face finishes
// flush with the bezel rim.
plate_x = xc - 2*tol_press;
plate_y = yc - 2*tol_press;
plate_z0 = bez_h - back_t;          // plate underside, in bezel coordinates

// Where the snap beams sit along Y. Both long walls are crowded — the buttons
// need an ear at btn_y and the vents need a clear run — so the pairs are
// pushed to the two ends: the locking pair OUTBOARD of the button ear (nearer
// the plug), the free pair down at the thumb end. The asserts below are what
// keep that arrangement true if anyone moves a button or a vent.
snap_y_lock = yc/2 - 4.2;
snap_y_free = -yc/2 + 6.0;

// Half-widths of the things competing for the long walls.
snap_half = (snap_w + 2*snap_slot)/2;
ear_half  = (ear_w + 2)/2;

assert(abs(snap_y_lock - btn_y) >= snap_half + ear_half,
       str("The plug-end snap beam at y=", snap_y_lock, " overlaps the button ",
           "ear at y=", btn_y, ". Move snap_y_lock outboard, or check ",
           "btn_from_usb — both live on the long walls."));
assert(snap_y_lock + snap_half <= yc/2,
       "The plug-end snap beam runs off the end of the wall.");
assert(snap_y_free - snap_half >= -yc/2,
       "The free-end snap beam runs off the end of the wall.");

// The hook's shoulder sits so its flat lands in the plate's groove, which is
// cut at mid-plate. One number, derived once, used by both parts — if the
// hook and the groove are ever computed separately they will drift apart and
// the case will either rattle or refuse to close.
groove_mid_z = plate_z0 + back_t/2;

// The beam runs DOWN from the rim; its root is snap_beam_l below the rim.
beam_root_z = bez_h - snap_beam_l;
assert(beam_root_z > face_t + 1.0,
       str("Snap beams (", snap_beam_l, " mm) are longer than the bezel wall ",
           "can carry — shorten snap_beam_l or deepen the case."));

// ── The glass has to be RETAINED, not merely framed ──────────────────────
// The bezel face overlaps the LCD module's border by this much on each side.
// It is the only thing holding the board forward against the ribs, so if it
// ever goes to nothing the board is free to walk out through its own window.
assert(lip_w >= 0.8 && lip_l >= 0.8,
       str("The bezel face overlaps the LCD module by only ", lip_w, " / ",
           lip_l, " mm — too little to retain the glass. The window (aa_*) ",
           "has grown past the module outline (lcm_*)."));

// ── Nothing on the back face may land on the mark ────────────────────────
// The back is the one clean surface this case has. These check the two
// windows against the lockup's real footprint rather than trusting the
// numbers to stay compatible when somebody measures the board and moves them.
mark_half   = mark_lockup_h(mark_h)/2;
mark_lo     = mark_dy - mark_half;
mark_hi     = mark_dy + mark_half;
led_y       = -yc/2 + led_from_far;
assert(!led_win || led_y + led_d/2 + 0.8 <= mark_lo,
       str("The LED window (top at y=", led_y + led_d/2,
           ") runs into the mark (bottom at y=", mark_lo,
           "). Raise mark_dy, shrink mark_h, or move led_from_far."));
assert(!sd_window || (yc/2 - sd_from_usb) - sd_l/2 - 0.8 >= mark_hi,
       str("The card window runs into the mark. Lower mark_dy or move ",
           "sd_from_usb — and remember sd_from_usb is a MEASURE item."));
assert(mark_hi <= plate_y/2 - 1.0 && mark_lo >= -plate_y/2 + 1.0,
       "The mark runs off the edge of the back plate — shrink mark_h.");

// ===========================================================================
//  PRIMITIVES
// ===========================================================================

module rrect(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }

// The USB-A plug's cross-section: a RECTANGLE, 12.00 x 4.50, with barely
// broken corners.
//
// This is worth being blunt about because the mistake is easy and expensive:
// USB-C is a stadium (full-round ends) and USB-A is not. Cutting a stadium
// for a series-A plug leaves the shell's four square corners with nowhere to
// go — the plug simply does not pass, and the instinct is then to open the
// hole up until it does, which ends with a sloppy oval around a square
// connector. Cut the right shape and the clearance can stay tight.
//
// `usb_r` is the corner break only. A series-A shell's corners are close to
// sharp; a few tenths keeps the printed opening from needing an elephant's
// foot allowance and is invisible against the shell.
usb_r = 0.35;
module usb_a_2d(w, h, r = usb_r) {
    offset(r = r) offset(r = -r) square([w, h], center = true);
}

// ===========================================================================
//  THE BEZEL — front frame, prints FACE-DOWN
// ===========================================================================

module bezel_solid() {
    linear_extrude(bez_h) rrect(xo, yo, r_out);
}

// The board cavity, plus the side "ears" that let the overhanging side
// buttons slide past the walls to their seat. Without the ears the board
// simply cannot reach the glass ledge — the same lesson the C6 case learned
// on its first print, and the buttons here are on the LONG edges.
module bezel_cavity() {
    translate([0, 0, face_t])
        linear_extrude(bez_h) rrect(xc, yc, r_in);

    if (opt_btn) for (sx = [-1, 1])
        translate([sx * (xc/2 + btn_reach/2 - 0.01), btn_y, face_t])
            linear_extrude(bez_h)
                square([btn_reach + 0.02, ear_w], center = true);
}

// The window: the bezel face overlaps the module border and shows the active
// area, with a slight inward draft so the frame reads thin from the front
// without being thin where it matters.
module bezel_window() {
    translate([0, 0, -0.1])
        linear_extrude(face_t + 0.2)
            rrect(aa_w, aa_l, 1.6);
}

// The plug opening + its relief chamfer + the drop collar.
module bezel_usb() {
    // The through opening, sized shell + clearance, as a stadium.
    translate([usb_dx, yc/2 + wall/2, z_usb]) rotate([90, 0, 0])
        linear_extrude(wall*3, center = true)
            usb_a_2d(usb_w + 2*usb_clear, usb_h + 2*usb_clear);

    // Relief on the OUTER face: a receptacle recessed in a wall-wart housing
    // meets air here instead of meeting the case.
    translate([usb_dx, yo/2 + 0.01, z_usb]) rotate([90, 0, 0])
        linear_extrude(usb_relief, scale = 1.0)
            usb_a_2d(usb_w + 2*usb_clear + 2*usb_relief,
                     usb_h + 2*usb_clear + 2*usb_relief, usb_r + usb_relief);
}

module bezel_collar() {
    // The drop buttress. It reaches INWARD from the plug-end wall, never
    // outward — this is the whole subtlety of the feature. An outward collar
    // is the obvious shape and it is wrong: every millimetre outside the end
    // wall comes straight off the plug's usable insertion length, and 3 mm of
    // handsome ring is 3 mm the plug no longer reaches into the socket.
    //
    // Built inward, it costs nothing and does the same job better: it extends
    // the length of shell the case BEARS ON, so when the stick is dropped on
    // its plug the shell pivots against a long bearing in the case instead of
    // against its own solder joints.
    //
    // Clipped to sit ABOVE the PCB's back face, because below that plane is
    // where the board is. So it is an inverted U over the shell's top and
    // upper flanks, not a closed ring — which is also the half that matters,
    // since a stick dropped on its plug levers the shell toward the glass.
    intersection() {
        translate([usb_dx, yc/2 + 0.01, z_usb]) rotate([90, 0, 0])
            linear_extrude(collar_l)
                difference() {
                    usb_a_2d(usb_w + 2*collar_gap + 2*collar_t,
                             usb_h + 2*collar_gap + 2*collar_t, usb_r + collar_t);
                    usb_a_2d(usb_w + 2*collar_gap, usb_h + 2*collar_gap);
                }
        // above the board, inside the shell
        translate([0, 0, z_pcb_back]) linear_extrude(bez_h - z_pcb_back)
            rrect(xc, yc, r_in);
    }
}

// Button access, through the ear skin.
module bezel_buttons() {
    for (sx = [-1, 1])
        translate([sx * (xo/2 + 1), btn_y, z_btn]) rotate([0, -90*sx, 0])
            cylinder(h = wall + ear_bump + 2, d = btn_d, center = true);
}

// Side vents: the house egg, in offset rows (the clutch), on the clear
// stretch of long wall between the two snap pairs.
//
// Upright on a vertical face is the point — a drip running down the skin
// meets the crown and parts around the opening instead of pooling on a flat
// slot top. Nothing here is rated for anything; it is drip logic, and it is
// free, so it may as well be right.
module bezel_vents() {
    for (sx = [-1, 1], r = [0 : vent_rows-1], c = [0 : vent_cols-1]) {
        y = vent_center_y + (r - (vent_rows-1)/2) * vent_pitch_y;
        z = face_t + cav_d*0.30 + c * (vent_l + 1.5)
            + (r % 2) * (vent_l + 1.5)/2;
        translate([sx * (xo/2 + 1), y, z]) rotate([0, 90*sx, 0])
            linear_extrude(wall + ear_bump + 2, center = true)
                egg2d(vent_l, vent_w);
    }
}

// The vents must not eat a snap slot or a button ear.
vent_span_lo = vent_center_y - (vent_rows-1)/2*vent_pitch_y - vent_w/2;
vent_span_hi = vent_center_y + (vent_rows-1)/2*vent_pitch_y + vent_w/2;
assert(!opt_vent || vent_span_hi <= min(btn_y - ear_half,
                                        snap_y_lock - snap_half),
       str("The vent clutch (up to y=", vent_span_hi, ") runs into the button ",
           "ear or the plug-end snap. Lower vent_center_y or tighten ",
           "vent_pitch_y."));
assert(!opt_vent || vent_span_lo >= snap_y_free + snap_half,
       str("The vent clutch (down to y=", vent_span_lo, ") runs into the ",
           "free-end snap beam. Raise vent_center_y."));

// ── The snap beams, cut into the bezel wall ───────────────────────────────
// Each is freed by a U-slot (two vertical slots through the wall, joined at
// the beam's root) and locally thinned on the inside to snap_beam_t so its
// strain stays in budget. The hook stands INWARD at the rim end.

// Hook cross-section, in (inward, up) with z=0 at the hook's shoulder. The
// BOTTOM face is the shallow lead-in — that is the one the plate's edge rides
// on the way in — and the TOP face is the steep return that holds it there.
module hook_profile(ret) {
    lead_run = snap_eng / tan(snap_lead);
    ret_run  = snap_eng / tan(ret);
    polygon([[0, -lead_run],
             [snap_eng, 0],
             [snap_eng, snap_flat],
             [0, snap_flat + ret_run]]);
}

function hook_h(ret) = snap_eng/tan(snap_lead) + snap_flat + snap_eng/tan(ret);

// The material REMOVED to free a beam: the two slots, and the inside relief
// that thins the wall down to snap_beam_t over the beam's length.
module bezel_snap_relief() {
    for (sy = [1, -1]) {
        yy = sy > 0 ? snap_y_lock : snap_y_free;
        for (sx = [-1, 1]) {
            // The two U-slots, through the wall, from the rim down to the root.
            for (sw = [-1, 1])
                translate([sx * (xo/2 - wall/2),
                           yy + sw * (snap_w + snap_slot)/2,
                           beam_root_z + snap_beam_l/2 + 0.6])
                    cube([wall + ear_bump + 2, snap_slot, snap_beam_l + 1.2],
                         center = true);
            // Thin the wall behind the beam so it can actually flex. Left at
            // full wall thickness the beam is a rib, not a spring.
            translate([sx * (xc/2 + (wall - snap_beam_t)/2 - 0.01),
                       yy, beam_root_z + snap_beam_l/2 + 0.6])
                cube([wall - snap_beam_t + 0.02, snap_w,
                      snap_beam_l + 1.2], center = true);
        }
    }
}

// The hooks themselves, added back on the beams' inner faces.
module bezel_hooks() {
    for (sy = [1, -1]) {
        yy  = sy > 0 ? snap_y_lock : snap_y_free;
        ret = sy > 0 ? snap_ret_lock : snap_ret_free;
        for (sx = [-1, 1])
            translate([sx * (xc/2), yy, groove_mid_z - snap_flat/2])
                rotate([90, 0, 0])
                    linear_extrude(snap_w, center = true)
                        scale([-sx, 1]) hook_profile(ret);
    }
}

module bezel_flick_scallop() {
    // The scoop through the far wall's rim that reaches the plate's lift lug.
    // Rounded on purpose: a square notch cut into a rim is a stress raiser,
    // and this is the corner a dropped stick lands on second.
    translate([0, -yo/2 - 0.6, bez_h + flick_scoop/2 - 1.6])
        rotate([0, 90, 0])
            cylinder(h = flick_w + 1.0, d = flick_scoop, center = true);
}

module bezel() {
    union() {
        difference() {
            union() {
                bezel_solid();
                if (opt_btn) for (sx = [-1, 1])
                    translate([sx * (xo/2 + ear_bump/2 - 0.01), btn_y, face_t])
                        linear_extrude(bez_h - face_t)
                            rrect(ear_bump + 0.02, ear_w + 2, 0.8);
                if (collar_on) bezel_collar();
            }
            bezel_cavity();
            bezel_window();
            bezel_usb();
            if (opt_btn) bezel_buttons();
            if (opt_vent) bezel_vents();
            bezel_snap_relief();
            bezel_flick_scallop();
        }
        // Hooks go on AFTER the reliefs are cut, or the slots would eat them.
        bezel_hooks();
    }
}

// ===========================================================================
//  THE BACK — snaps in, carries the mark, releases with a thumb
// ===========================================================================

// The plate is authored in its OWN frame: z = 0 is its underside (the face
// that looks into the case), z = back_t its outer face (the one that wears
// the mark). The renderer places it.
module back_plate() {
    difference() {
        linear_extrude(back_t) rrect(plate_x, plate_y, r_in);

        // Lead-in chamfer on the bottom outer edge — this is the face that
        // rides the hooks' shallow lead-in on the way down, so the plate
        // guides itself in rather than needing to be aimed.
        lead = snap_eng + 0.3;
        translate([0, 0, -0.01]) linear_extrude(lead + 0.01, scale =
                (plate_x) / (plate_x - 2*lead))
            rrect(plate_x - 2*lead, plate_y - 2*lead, max(0.4, r_in - lead));
    }
}

// The groove the hooks seat in: a shallow rectangular slot around the whole
// perimeter, cut at mid-plate. Running it right around (rather than four
// local pockets) means the plate has NO orientation to get wrong — it drops
// in either way up-the-long-axis, which matters for a part a user takes off
// in a dark hallway.
module back_groove() {
    g_h = snap_flat + 0.3;
    g_d = snap_eng + 0.15;
    translate([0, 0, back_t/2 - g_h/2])
        linear_extrude(g_h)
            difference() {
                rrect(plate_x + 1, plate_y + 1, r_in);
                rrect(plate_x - 2*g_d, plate_y - 2*g_d, max(0.4, r_in - g_d));
            }
}

// Compliant PCB ribs: thin standing ribs that crush slightly rather than a
// hard boss. `preload` is the interference. A rigid clamp would make every
// drop a bending moment across the board; a rib that gives 0.25 mm turns the
// same impact into a squeeze the PCB does not care about.
module back_ribs() {
    rib_h = back_stack - preload;
    for (sy = [-1, 1], sx = [-1, 1])
        translate([sx * (xc/2 - 3.2), sy * (yc/2 - 4.5), -rib_h])
            linear_extrude(rib_h) square([1.2, 5.0], center = true);
}

// The thumb-flick ramp. The plate's far end carries a wedge that stands proud
// of the bezel rim; a thumb pushed ALONG the body meets the wedge's 20° face
// and cams the plate up out of the two shallow-return hooks.
//
// It is at the far end on purpose: the gesture pushes the stick further INTO
// its socket rather than levering it out of the wall.
module back_flick() {
    run = flick_proud / tan(flick_ramp);
    // The lug: a tab on the plate's far edge standing `flick_proud` above the
    // outer face, its outward face raked back at `flick_ramp` so a thumb
    // sliding along the body climbs it instead of stubbing on it.
    translate([0, -plate_y/2 + 0.6, back_t])
        rotate([90, 0, 0]) rotate([0, 0, 0])
            translate([0, 0, -flick_w/2])
                linear_extrude(flick_w)
                    polygon([[0, 0],
                             [1.2, 0],
                             [1.2, -flick_proud],
                             [-run, -flick_proud]]);
}

module back_cutouts() {
    // The card window.
    if (sd_window)
        translate([0, yc/2 - sd_from_usb, -1])
            linear_extrude(back_t + 2) rrect(sd_w, sd_l, 1.4);

    // The LED wall-wash window.
    if (led_win)
        translate([0, -yc/2 + led_from_far, -1])
            cylinder(h = back_t + 2, d = led_d);
}

// The mark, as a deboss in the back's outer face. `grow` lets the ACCENT part
// be generated a hair larger so the yellow inlay actually touches the walls
// of the pocket it sits in rather than rattling inside it.
module back_mark(grow = 0) {
    translate([0, mark_dy, back_t - mark_depth])
        linear_extrude(mark_depth + 0.02)
            offset(r = grow)
                mark_lockup(mark_h, mark_rib);
}

module back_body() {
    difference() {
        union() {
            back_plate();
            back_ribs();
            back_flick();
        }
        back_groove();
        back_cutouts();
        if (mark_show) back_mark(0);
    }
}

// The yellow inlay: exactly the pocket the body left. Printed in the accent
// filament and loaded as a second part — never re-centered.
module back_accent() {
    if (mark_show) back_mark(-0.08);
}

// The diffuser plug for the LED window. Natural / translucent PETG. A press
// fit, because a nightlight whose window falls out onto the hallway floor at
// 3 a.m. is worse than no window.
module light_plug() {
    translate([0, 0, 0]) {
        cylinder(h = led_skin, d = led_d - 0.15);
        // A retaining flange on the inside face.
        translate([0, 0, led_skin]) cylinder(h = 0.8, d = led_d + 1.4);
    }
}

// ===========================================================================
//  RENDER
// ===========================================================================

module back_assembly() { color("#1a1a1a") back_body(); color("#f5c518") back_accent(); }

// PRINT ORIENTATION for the back is mark-face-DOWN (so the ribs point up and
// print in air-free order, and the A-surface takes the textured plate's
// finish — the same rule every case in this catalog follows). The assembly
// views below seat it the other way up, which is how it actually sits.
module back_printed() { rotate([180, 0, 0]) translate([0, 0, -back_t]) back_assembly(); }

if (part == "bezel") bezel();
else if (part == "back") back_printed();
else if (part == "light") light_plug();
else if (part == "fil_body") rotate([180, 0, 0]) translate([0, 0, -back_t]) back_body();
else if (part == "fil_accent") rotate([180, 0, 0]) translate([0, 0, -back_t]) back_accent();
else if (part == "all") {
    bezel();
    translate([xo + 6, 0, 0]) back_printed();
    translate([xo + 6, yo/2 + 8, 0]) light_plug();
}
else if (part == "exploded") {
    bezel();
    translate([0, 0, plate_z0 + 15]) back_assembly();
}
else if (part == "fit_section") {
    // Half-section down the long axis — the view that shows whether the
    // collar, the plug opening, the board seat and the snap engagement
    // actually agree with each other.
    difference() {
        union() { bezel(); translate([0, 0, plate_z0]) back_assembly(); }
        translate([-xo, -yo, -5]) cube([xo, 2*yo, bez_h + 40]);
    }
}
else bezel();

echo(str("USB-A insertion length left clear: ", usb_free, " mm (need >= 11)"));
echo(str("Snap beam strain: ", snap_strain*100, " % (PETG budget 1.8)"));
echo(str("Outer shell: ", xo, " x ", yo, " x ", bez_h,
         " mm (the back plate seats FLUSH with the rim, so the bezel ",
         "height is the whole thickness); plug adds ", usb_proud, " mm"));
