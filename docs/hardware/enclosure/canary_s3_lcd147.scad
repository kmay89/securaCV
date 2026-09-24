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
//     past the PCB's plug-end edge comes straight off that 12 mm — the
//     cavity's tol_slide AND the end wall (`wall`) both. Its outer face is
//     RELIEVED (`usb_relief`) so a receptacle recessed in a wall-wart housing
//     meets air instead of meeting this case. `usb_free` asserts what is
//     left; if it drops below 11 mm the render fails rather than shipping a
//     case that will not plug in all the way.
//     And the opening is a RECTANGLE, because series-A is a rectangle —
//     see `usb_bore_2d`, with a bridge-safe 45° top. A USB-C stadium will
//     not pass a series-A shell's square corners.
//
//  2. NO SCREWS, AND THE SNAP IS WHAT THE PRINT CAN HOLD.
//     Four beams cut into the bezel wall hook a groove in the plate's edge.
//       - RETENTION IS 45°, at all four: the plate pulls out against the
//         hooks' UNDERSIDES, an overhang on the face-down bezel. (The first
//         cut's 62° / 38° "return angles" sat on the TOP face — asymmetric
//         ENTRY, not hold.) The plate enters over a 45° top face.
//       - It releases where it is pried: the far-end scallop lifts the far
//         pair first, so the plug end is never the end that lets go.
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
//       - the PRY SCALLOP (`flick_*`): a rounded scoop through the far wall's
//         rim that puts a thumbnail under the plate's far edge and pops the
//         two far-end snaps. It is deliberately at the end AWAY from the
//         plug, so the gesture pushes the device INTO its socket rather than
//         levering it out.
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

use <canary_core_lib.scad> // rrect2d + the house constants
use <canary_mark_lib.scad>  // the house mark: bird + wordmark lockup
use <canary_port_lib.scad>  // the series-A standards + the insertion-length gate + the bridge-safe opening
use <canary_snap_lib.scad>  // snap_strain() + the cycled-snap budget the beams answer to
use <canary_board_lib.scad> // the ws147 board record the knob defaults cite
use <canary_color_lib.scad> // the colorway registry — preview spool colors

/* [What to render] */
part = "all";   // ["bezel","back","light","all","exploded","palette","fil_body","fil_accent","fil_light","fit_section"]
// board build: "none" = as Waveshare ships it (bare pads), "male" = GPIO headers soldered pointing DOWN, away from the glass
headers = "none";   // ["none","male"]

/* [Board] — ESP32-S3-LCD-1.47, from the Waveshare drawing (mm) */
board_l = 36.37;   // PCB long axis (Y), EXCLUDING the USB-A plug —
                   // brd_l("ws147"), canary_board_lib: the family outline's home
board_w = 20.32;   // PCB short axis (X) — brd_w("ws147"), canary_board_lib
pcb_t   = 1.6;     // MEASURE (drawing does not call it out; 1.6 is the usual —
                   // brd_t("ws147") carries the same reading, same caveat)

// The LCD module: same 1.47" panel as the C6 sibling, so the same numbers.
lcd_rise = 3.65;   // glass front above the PCB front face (5.10 stack - 1.45)
aa_l  = 32.35;     // active area, long (Y)
aa_w  = 17.39;     // active area, short (X)
lcm_l = 36.28;     // LCD module outline, long
lcm_w = 19.39;     // LCD module outline, short

// THE MEASURED STACK (kmay89): glass front to the back of everything on the
// board is 8.2 mm. This is the number the case's whole depth is built on, so
// it is stated once and the back-side clearance is DERIVED from it rather than
// guessed separately — a guessed back_stack is how the first cut of this case
// came out 1.65 mm fatter than the hardware needs, which on a stick you hold
// is the difference between snug and chunky.
stack_total = 8.2;   // MEASURED — glass front to board back
back_stack = stack_total - lcd_rise - pcb_t;   // derived, not guessed

/* [Headers] — the "male" build only */
// Waveshare sell this board bare and advertise "expansion of multiple
// peripherals via GPIO header": two 2.54 mm rows of pads down the long edges
// (5V/GND/3V3/GP1-GP6 on one side, TXD/RXD/GP13-GP7 on the other). Solder
// them and the board grows a second stack BEHIND it, which is a different
// case — the same way the C3 sibling's "male" build is a different case from
// its "pillars" one.
//
// The plug end does NOT move. That is the whole reason this build is cheap
// here: headers add depth behind the PCB, and the USB-A plug's insertion
// length is measured along the board's long axis, so `usb_free` and the drop
// collar are untouched. The case gets deeper, not longer.
//
// ⚠️ BOTH NUMBERS ARE MEASURE ITEMS, and one of them has already bitten this
//    project once. hdr_drop is carried over from the C3/C6 file, where 8.8 is
//    FIT-TESTED against the same 2.54 mm down-facing rows on the same PCB
//    outline — a defensible starting point, not a measurement of this board.
//    hdr_inset is the one to check first: the C3 file still lists 1.27 and
//    2.00 as unresolved drawing candidates, and the rib assert below is
//    sensitive to which it is.
hdr_drop  = 8.8;   // cavity depth below the PCB back swallowing base + pins —
                   // MEASURE (8.8 is the C3/C6's fit-tested figure)
hdr_inset = 1.6;   // PCB long edge → header row centerline — MEASURE
hdr_pin_w = 1.2;   // width the solder fillet + pin occupies across the row
hdr_body_w = 2.54; // the header's plastic body across the row — it sits on the PCB back, where the ribs land

/* [USB-A plug] — the standard, plus what the board does with it */
// USB-A series-A plug shell, per the USB 2.0 mechanical drawing. These are
// NOT guesses and should not be "adjusted to fit" — if the plug does not pass
// the opening, the opening is wrong, not the standard.
usb_shell_w = 12.00; // shell width — port_usba_shell_w(), canary_port_lib
usb_shell_h = 4.50;  // shell height — port_usba_shell_h(), canary_port_lib
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

// The plug-end wall is the shell's own `wall` — there is no separate knob
// for it (a `usb_wall` of 1.8 used to feed usb_free while the case drew 2.1,
// so the gate passed half a millimeter the plug did not have).
// Relief depth on the outer face around the opening. Bounded by how much
// wall there is above and below the plug — the shell straddles the board, so
// its bottom edge sits only a couple of millimeters above the bezel face and
// a greedy relief cuts straight through it. The two asserts below hold the
// line; if you want more relief, you need a deeper case, not a bigger number.
usb_relief = 0.5;  // relief step depth on the outer face around the opening — bounded by the wall above and below the plug

/* [Drop collar] — the reason this case exists in one piece */
collar_on = true;
collar_t = 2.6;    // ring thickness around the shell at its root
collar_l = 3.4;    // how far the ring reaches ALONG the plug from the wall
collar_gap = 0.25; // ring-to-shell gap: small enough to bottom out early,
                   // big enough that the ring is not a press fit on the shell

/* [Buttons] — BOOT and RST, side-mounted near the plug end */
opt_btn = true;
btn_d = 2.6;         // access hole Ø — a fingertip cannot, a pen tip can
btn_from_usb = 7.0;  // button center down from the PCB's plug-end EDGE (not the cavity end)
                     // 7.0 is the C3 case's FIT-CONFIRMED btn_up on this same
                     // outline: its first print caught the drawing's 11.31 as
                     // a CENTER-referenced dimension (18.185 - 11.31 = 6.875),
                     // and this file had read it as edge-referenced — 4.1 mm
                     // off, which puts the board's side switches on plain wall
btn_dz = 1.0;        // actuator center behind the PCB BACK face — MEASURE
btn_proud = 1.6;     // actuator overhang past the PCB edge — MEASURE
btn_ch_w = 3.2;      // actuator channel width — hugs the nub, nothing more
ear_skin = 1.1;      // wall skin left outside a button clearance channel

/* [microSD] — the card the pry scallop is for */
// OFF by default, and that is the considered choice, not an oversight. The
// pry scallop IS the card story: the back comes off in a second and the slot
// is right there. A permanent window costs the one clean face this case has
// (the mark lives there), and it can only be cut in the right place once
// somebody has measured where the slot mouth actually is — which the product
// photos do not tell us. Turn it on after measuring `sd_from_usb`, and the
// assert below will make sure it does not land on the mark.
sd_window = false; // permanent window in the back over the SD slot — OFF by default: measure sd_from_usb first
sd_from_usb = 15.0;  // slot MOUTH center, down from the plug-end edge — MEASURE
sd_w = 13.0;         // window width (card is 11 mm + finger room)
sd_l = 9.0;          // window length along the board

/* [RGB LED] — the light SEAM, not a hole in the back */
//
// The WS2812 (GPIO38) is the ambient beacon, and where its light comes OUT is
// the thing this case has to get right.
//
// It does not fire backwards. Waveshare ship this board with a "clear acrylic
// sandwich panel for cool lighting effects": clear plates front and back, and
// the LED glows out the EDGE GAP between the LCD module and the PCB. On this
// case that gap is the band from the glass (z = face_t) back to the PCB front
// face (z = face_t + lcd_rise) — about 3.6 mm of open perimeter that a solid
// side wall seals shut.
//
// So the light gets a SEAM around the sides rather than a hole in the back:
// a slot at that band, which reads as a glowing line along the screen's edge.
// That is both the effect the board was designed for and a better one for a
// hallway — a line of color at the screen edge is legible from an angle, where
// a wall wash is only legible from in front.
//
// It also frees the back plate: the mark now gets the whole clean face, which
// is what it wanted anyway.
led_win = false;      // the old back hole. OFF — it points at nothing.
led_from_far = 6.5;  // back-window center up from the PCB's FAR edge — MEASURE
led_d = 7.0;         // back-window Ø
led_skin = 0.5;      // thickness of the diffuser plug that fills it

light_seam = true;
// THE BAND. The seam is not left as an open slot — it is filled with a strip
// of WHITE / natural PETG, which is a light pipe: the RGB enters its cut end
// and the strip glows evenly along its length instead of throwing a hard bar
// of light through a hole. White PETG is the right plastic for it (translucent
// and diffusing rather than clear), and it turns the seam from a gap in the
// case into a lit feature.
//
// Two ways to build it, and the geometry serves both:
//   - CO-PRINTED (AMS / multi-material): a third filament alongside the black
//     body and the yellow mark — the same three-spool arrangement the 7" frame
//     already uses. `python3 gen_3mf.py stick` packages exactly this, at
//     band_clear = 0 (the default) so the band fuses to the walls it fills.
//   - SEPARATE INSERTS: print part="fil_light" on its own at band_clear =
//     0.10 and press the two strips in (see band_clear for what the bezel's
//     seam roof costs in that build).
//
// Either way the outer line is CONTINUOUS — the ties across the seam are ribs
// hidden behind the strip, not breaks in it. See seam_web_ribs.
light_band = true; // fill the seam with a white PETG light-pipe strip (co-printed, or pressed in as part fil_light)
// ZERO by default: the co-print is the build. The slot's roof is ~16 mm of
// wall carried only by the hidden ribs, and on a face-down bezel it is a
// bridge unless the band is printed under it. The insert build (0.10) prints
// that roof as a bridge between ribs — a known sag, accepted by choosing it.
band_clear = 0;      // per-face clearance; 0 = co-printed (the default), 0.10 = pressed-in inserts
// Measured off the board (kmay89), stated as the SIDE ELEVATION because that
// is what you look at: 1 mm of black, then 3 mm of white, then black to the
// back. The 1 mm is the bezel face; the white starts immediately behind it
// (seam_dz = 0) and runs to 4 mm.
//
// That lands the band's far edge essentially ON the PCB front face — which is
// the point, and is why the assert below no longer holds it clear of that
// plane. The WS2812 sits on the PCB front, so the band has to reach it: the
// strip's inner face is what collects the light and pipes it along. A band
// held politely short of the board would glow a great deal less.
seam_h = 3.0;        // band thickness — the white in the 1 / 3 / black stack
seam_dz = 0.0;       // band starts level with the glass front, i.e. directly
                     // behind the 1 mm face
seam_webs = 3;       // HIDDEN ribs tying the bezel face to its walls across the
                     // seam. The seam runs most of the wall, so without them
                     // the face hangs off its corners alone — and this case is
                     // designed around being dropped. They sit behind the
                     // strip, not through it: the white line stays unbroken.
                     // 0 is legal and gives a bare continuous slot.
seam_web_w = 3.0;    // rib width along the seam
seam_web_d = 1.2;    // how far into the 2.1 mm wall a rib reaches, leaving the
                     // rest as white you can still see at that spot

/* [Branding] — one yellow mark, on the back */
mark_show = true;
// The BIRD's height; the wordmark scales off it. Sized so the WHOLE lockup
// fits the plate with real margin — see the width assert below. At 10.5 the
// accent measured 22.08 mm wide against a 20.52 mm plate, i.e. it hung off
// BOTH edges, and nothing caught it because the only assert here checked the
// mark's height.
// ⚠️  THE BIRD IS NOT ON THIS PART, and it is not an omission.
// The house mark was redrawn to the brand's monoline art — a bird with a C
// spiralled into its wing and a V nested in its tail, carrying a notepad. That
// drawing has INTERIOR detail, and interior detail has a minimum size:
// mark_min_h(0.9) is 20.1 mm, against a back plate 20.3 mm ACROSS. The bird
// would be wider than the plate before it was legible on it.
//
// So this part carries the WORDMARK alone (mark_wordmark), which is what
// canary_mark_lib.scad's MINIMUM SIZE section says to do. The alternative was
// a second, simplified bird for small parts, and a second bird in the line is
// the one thing that library exists to prevent. If this case ever gets a
// bigger face, put the mark back — it is one call.
mark_bird_ok = false;   // this plate cannot carry the mark; see above
mark_h = 8.5;        // kept as the lockup's SCALE reference so mark_word_h()
                     // and the plate asserts read the same as they always did
mark_rib = 0.9;      // stroke width. Below ~0.8 the mark stops being a mark
                     // at this size — two 0.42 lines is the floor
mark_depth = 0.7;    // deboss depth; the accent inlay fills it flush
// The lockup is not quite symmetric about x=0: mark_cx() centers the BIRD,
// whose beak (-63) and tail (+67) are themselves off-center in design units,
// while the wordmark centers on 0. That leaves the right margin ~0.2 mm
// tighter than the left at every size. Nudge the group back.
mark_dx = -0.10;   // lockup X nudge — evens out a right margin ~0.2 mm tighter than the left
mark_dy = 0;         // CENTERED. It used to sit high to clear the LED window;
                     // with the light moved to the side seam the back is one
                     // clean face and the mark can have the middle of it.

/* [Cooling] — deliberately no vents */
// There are none, and that is the design rather than an omission.
//
// A stick plugged into a wall outlet stands VERTICAL, and this case already
// has an opening at each end of that vertical: the thumb scoop through the far
// wall's rim at the bottom, and the clearance around the USB shell in its
// opening at the top. Bottom intake, top exhaust — that is a chimney, and it
// is the orientation the device is always in.
//
// So the clutch of egg vents that used to run down both long walls was solving
// a problem the geometry had already solved, at the cost of the two faces you
// actually look at, plus a fight for wall space with the snap beams and the
// button ears. Cleaner to look at, easier to print, fewer holes to collect
// dust, and no less air.
//
// If a bench test ever shows this board genuinely needs more: put them on the
// SHORT walls, where the chimney already runs and where nobody looks.

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
// A thinner case leaves less wall to carry a beam, so the beam gets LONGER
// per unit thickness, not shorter: strain goes as t/L², so trading 0.15 mm of
// thickness buys back more than the 1.2 mm of length the shallower case costs.
// 6.0, not 6.8: the plug-end pair now stands over the light seam (the
// button ear took its old place at the plug end), and a beam must root on
// solid wall above the seam's roof, not on it — see the seam assert.
snap_beam_l = 6.0;   // free beam LENGTH — see the strain note below
snap_beam_t = 0.85;  // beam thickness (the wall is locally thinned to this)
// The hook has to FIT the 2.0 plate it hooks: its height at the plate edge
// (eng on the underside + flat + eng on the top face, at 45° each) plus
// tol_press either side is the groove, and the groove needs a lip of plate
// above and below it. 0.55 / 0.5 drew a 1.34-1.75 mm hook into a 0.8 mm
// groove — ~0.3 mm of interference at every seat, a plate that never closed.
snap_eng = 0.4;      // engagement depth (how far the hook stands past the plate edge)
snap_flat = 0.3;     // the vertical flat at the hook tip
snap_slot = 0.9;     // U-slot width freeing each side of the beam
snap_hold = 45;      // hook UNDERSIDE angle from horizontal — the retention face; 45 is the flattest a face-down print holds
snap_entry = 45;     // hook TOP face angle from horizontal — the face the plate's lead chamfer rides in over

/* [Pry scallop] — how the back comes off */
// A scoop through the far wall's rim that bares the plate's far edge, so a
// thumbnail lifts it off the two far-end hooks. It used to reach a LIFT LUG on
// the plate — a stub drawn in the wrong plane that stood 1.2 mm past the
// plate's far end and landed on the bezel rim, so the plate could not seat at
// all. The scallop was always the pry point; the lug is gone.
//
// Far end, deliberately: the gesture pushes the stick further INTO its socket
// rather than levering it out of the wall.
flick_w = 9.0;       // scallop width along the far wall
flick_scoop = 7.0;   // Ø of the scoop through the bezel rim

/* [Shell] */
wall = 2.1;   // deviates: the cantilever snap beams (beam_t/eng) were tuned against this wall — conform only with a snap re-validation
// The bezel face, and it is a LOOK as much as a thickness. The side elevation
// reads (kmay89): 1 mm black, then 3 mm of white band, then black to the back.
// So the face is 1 mm and the band starts immediately behind it — which is
// also where it should start optically, because the glass front sits on this
// face's inner ledge and the light escapes the moment the glass ends.
face_t = 1.0;        // bezel face over the glass border
back_t = 2.0;        // rear plate
r_out = 3.2;         // deviates: pre-canon — the band look was drawn on this radius; conform on the next redraw
preload = 0.25;      // compliant squeeze on the PCB (rib crush), not a clamp
// Where the compliant ribs land, measured in from the cavity wall. A knob
// rather than a literal because the headered build is what decides whether it
// is still free: the pin rows run down the long edges, and the rib has to
// stand INBOARD of them. The assert in the derived block is what checks it,
// and this is the number it tells you to change.
rib_inset = 4.2;   // where the compliant ribs land, in from the cavity wall — inboard of the header bodies and the side switches
rib_w = 1.2;         // rib thickness across the board — the crushing face
rib_l = 5.0;         // rib length along the board

/* [Tolerances] */
tol_slide = 0.20;    // board into its cavity
tol_press = 0.10;    // skirt into the bezel
tol_hole = 0.30;

/* [Colorway] — preview spools only; per-part exports carry no color */
colorway = "midnight"; // ["graphite","canary","snow","forest","midnight"] this stick's print-validated set is midnight: near-black body, canary ink, white band (canary_color_lib)

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
// The cavity corner radius is held by the BOARD, not by the outer radius: a
// square-cornered PCB clears a cavity only tol_slide bigger when
// (r - tol)·√2 <= r, i.e. r <= tol·√2/(√2 - 1) = 0.68 at 0.20 (less 0.1 for
// the arc's facets, whose chords cut inside it). r_out - wall (1.1) drew
// corners that stood 0.17 into all four PCB corners.
r_in = min(max(0.4, r_out - wall), tol_slide * sqrt(2) / (sqrt(2) - 1) - 0.1);

// The clearance behind the PCB. On the bare board that is the measured stack;
// with headers soldered it opens out to swallow the base and the pins, and
// max() rather than a swap is what keeps the bare figure the FLOOR — a
// hdr_drop mis-measured shallow can only ever make the case as tight as the
// bare one, never tighter than the hardware.
stack_eff = (headers == "male") ? max(back_stack, hdr_drop) : back_stack;
cav_d = lcd_rise + pcb_t + stack_eff;    // glass ledge -> back plate inner
bez_h = face_t + cav_d + back_t;         // bezel wall height: the RIM carries the plate. It was
                                         // face_t + cav_d with the plate seated back_t INSIDE it,
                                         // which put the plate 2.0 mm too deep — its ribs ran
                                         // through the PCB and it sat on the drop collar

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
// USB-A needs ~12 mm; below the catalog's 11.0 floor — port_insertion_min()
// in canary_port_lib, where this file's retyped copy went to live — the plug
// will not seat in a deep receptacle and the whole device is useless, so
// this is an assert and not a comment.
// ── THE HEADERED BUILD'S GATES ─────────────────────────────────────────────
// A "male" build that is no deeper than the bare one is a define that did
// nothing, and it would render clean and print a case the pins hold open.
assert(headers != "male" || hdr_drop > back_stack,
       str("headers=\"male\" but hdr_drop (", hdr_drop, ") is no deeper than ",
           "the bare board's clearance (", back_stack, ") — the pins would ",
           "hold the back plate off. MEASURE hdr_drop."));
// The ribs press on the PCB, and the pin rows run down the same long edges.
// This is the collision that a comment would not have caught, and it bit
// twice: the first version checked only the 1.2 mm pin + fillet, while the
// header's 2.54 mm plastic body sits on the same PCB back face the ribs press
// — the ribs cleared the pins and landed 0.47 mm onto the body. Which
// hdr_inset is real (1.6 here; the C3 still lists 1.27 and 2.00) is
// unmeasured, so the arithmetic is an assert.
rib_out = xc/2 - rib_inset + rib_w/2;              // rib's outboard face
pin_in  = board_w/2 - hdr_inset - max(hdr_pin_w, hdr_body_w)/2;   // header's inboard face
assert(headers != "male" || rib_out <= pin_in - 0.15,
       str("the compliant ribs reach x=", rib_out, " and the header pin row ",
           "starts at x=", pin_in, " — the ribs would land on the pins ",
           "instead of the board. Raise rib_inset, or MEASURE hdr_inset ",
           "(1.27 and 2.00 are both still on the table)."));
// A rib is a column now, not a bump. Slenderness is what turns a compliant
// crush into something that folds over on assembly instead of pushing back.
assert(stack_eff + preload <= 9.0 * rib_w,
       str("the compliant ribs would stand ", stack_eff + preload, " mm on a ",
           rib_w, " mm section — too slender to load. Widen rib_w."));

// The shell leaves the PCB edge, crosses the tol_slide gap to the cavity end,
// then the end wall: both come off the plug.
usb_free = usb_proud - tol_slide - wall;
port_assert_insertion(usb_free, "the hallway case's series-A plug");

// The plug opening, bridge-safe (usb_bore_2d), and its outer relief have to
// stay INSIDE the end wall. If they do not, the case is open along an edge.
// The top is the chamfers' apex — the 45° corners rise from the SHELL line
// (so they never cut the shell envelope) to a flat no wider than the
// catalog's 7.0 bridge; the relief is a plain step around the shell box.
usb_ow   = usb_shell_w + 2*usb_clear;
usb_cham = port_bridge_cham_for(usb_ow);
usb_top = max(z_usb + usb_shell_h/2 + usb_cham,
              z_usb + usb_shell_h/2 + usb_clear + usb_relief);
usb_bot = z_usb - usb_shell_h/2 - usb_clear - usb_relief;
assert(usb_top <= bez_h - 0.6,
       str("The plug opening (top at ", usb_top, ") breaks out through the ",
           "rim (", bez_h, "). Its bridge-safe chamfers need ", usb_cham,
           " mm over the shell — check usb_dz and back_stack; a flat lintel ",
           "is not the fix."));
assert(usb_bot >= face_t + 0.6,
       str("The plug opening (bottom at ", usb_bot, ") breaks out through the ",
           "bezel face. Check usb_dz."));

// ── THE SNAP-BEAM STRAIN CHECK ─────────────────────────────────────────────
// Cantilever with a rectangular section (canary_snap_lib snap_strain):
//     ε = 1.5 · y · t / L²
// deflected by the hook's engagement PLUS tol_press — the plate floats that
// much in the cavity, and when it is pushed toward a beam the beam opens by
// both. PETG tolerates roughly 1.8% strain on a joint meant to be opened
// again and again (the catalog's cycled budget, snap_budget_cycle(), is 2.0;
// this file keeps its tighter 1.8). If this assert fires the fix is a LONGER
// or THINNER beam — never a shallower engagement: snap_eng is already what
// the 2.0 plate can take (the groove assert below), and less is a snap that
// lets go.
snap_defl   = snap_eng + tol_press;
snap_strain = snap_strain(snap_beam_t, snap_defl, snap_beam_l);
assert(snap_strain <= min(0.018, snap_budget_cycle()),
       str("Snap beam strain is ", snap_strain*100,
           "% — above the ~1.8% PETG can take repeatedly. Lengthen ",
           "snap_beam_l or thin snap_beam_t."));
// The print pose decides the hold: the underside is an overhang on the
// face-down bezel, so it cannot be flatter than 45°.
assert(snap_hold >= 45 && snap_entry >= 45,
       str("hook faces at ", snap_hold, "° / ", snap_entry, "° from horizontal — ",
           "below 45° they are overhangs the face-down bezel cannot print"));

// Button geometry. btn_from_usb is measured from the PCB's plug-end EDGE —
// it used to be subtracted from the cavity end (yc/2), a silent tol_slide off
// on top of the 11.31 misreading.
btn_y = board_l/2 - btn_from_usb;         // +Y is the plug end
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

// Where the snap beams sit along Y. Both long walls are crowded, and the
// button ear decides the plug-end pair: at the measured 7.0 the ear leaves
// only ~3.4 mm of wall between itself and the plug end — not a beam's worth.
// So the plug-end pair stands just INBOARD of the ear, over the light seam
// (the beams root above the seam's roof — asserted below), and the free pair
// stays down at the pry end. The asserts are what keep that arrangement true
// if anyone moves a button.
snap_half = (snap_w + 2*snap_slot)/2;
ear_half  = (ear_w + 2)/2;

snap_y_lock = btn_y - (snap_half + ear_half);
snap_y_free = -yc/2 + 6.0;

assert(abs(snap_y_lock - btn_y) >= snap_half + ear_half,
       str("The plug-end snap beam at y=", snap_y_lock, " overlaps the button ",
           "ear at y=", btn_y, ". Move snap_y_lock outboard, or check ",
           "btn_from_usb — both live on the long walls."));
assert(snap_y_lock + snap_half <= yc/2,
       "The plug-end snap beam runs off the end of the wall.");
assert(snap_y_free - snap_half >= -yc/2,
       "The free-end snap beam runs off the end of the wall.");
assert(snap_y_lock - snap_half >= snap_y_free + snap_half + 1.0,
       str("The two snap pairs overlap (plug pair at y=", snap_y_lock,
           ", free pair at y=", snap_y_free, ") — the button has moved too ",
           "far down the board for this wall."));

// The hook's shoulder sits so its flat lands in the plate's groove, which is
// cut at mid-plate. One number, derived once, used by both parts — if the
// hook and the groove are ever computed separately they will drift apart and
// the case will either rattle or refuse to close.
groove_mid_z = plate_z0 + back_t/2;

// The hook's height where it crosses the plate edge, and the groove that
// takes it: the groove is the hook's own profile grown by tol_press
// (back_groove), so the two cannot drift apart again. What is left of the
// plate above and below that groove is the lip the hook holds — asserted.
hook_hr = snap_eng * tan(snap_hold);                 // underside's rise
hook_er = snap_eng * tan(snap_entry);                // top face's rise
groove_edge_h = hook_hr + snap_flat + hook_er + tol_press/cos(snap_entry);
assert((back_t - groove_edge_h)/2 >= 0.3,
       str("the hook's groove is ", groove_edge_h, " mm tall at the edge of a ",
           back_t, " mm plate — less than 0.3 mm of plate either side of it. ",
           "Shrink snap_eng / snap_flat."));

// The beam runs DOWN from the rim; its root is snap_beam_l below the rim.
beam_root_z = bez_h - snap_beam_l;
assert(beam_root_z > face_t + 1.0,
       str("Snap beams (", snap_beam_l, " mm) are longer than the bezel wall ",
           "can carry — shorten snap_beam_l or deepen the case."));
// The plug-end pair stands over the light seam, so a beam must root on solid
// wall ABOVE the seam's roof, never in it: a U-slot that reaches the slot
// leaves the beam hanging off the band.
assert(!light_seam || beam_root_z >= face_t + seam_dz + seam_h + 1.0,
       str("the snap beams root at z=", beam_root_z, ", within 1.0 mm of the ",
           "light seam's roof (", face_t + seam_dz + seam_h, ") — shorten ",
           "snap_beam_l."));

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
// The wordmark alone is one line of type, so the group's height is the cap
// height — not mark_lockup_h(), which measures a stack this part no longer has.
mark_word_h = mark_h * mark_word_ratio();
mark_half   = (mark_bird_ok ? mark_lockup_h(mark_h) : mark_word_h)/2;
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
       "The mark runs off the TOP or BOTTOM of the back plate — shrink mark_h.");

// ── ...and the width, which is the one that actually bit ──────────────────
// OpenSCAD has no text-metrics primitive, so the wordmark's width cannot be
// measured here — it has to be ESTIMATED. `mark_adv` is a per-character
// advance as a fraction of cap height, calibrated against a measured render:
// at mark_h = 8.5 the exported accent spans 17.84 mm and this returns 17.85.
// Deliberately a hair pessimistic, because the failure it guards against is a
// mark hanging off the plate — which is exactly what shipped before it existed,
// unnoticed, because the only check here was vertical.
mark_adv = 0.875;  // wordmark per-character advance, as a fraction of cap height — an estimate, a hair pessimistic
mark_word_chars = 8;                       // "securaCV"
function mark_word_w(h) = mark_word_chars * mark_adv * h * mark_word_ratio();
// The bird's width comes from the library's own bbox, stroke caps included.
// It used to be `h * 130 / mark_span()` — the old drawing's beak-to-tail span
// typed into this file, which would have been silently WRONG the first time
// the mark moved. It moved. (With mark_bird_ok false the bird is not drawn,
// so the wordmark alone sets the width — but the term stays in the max() so
// putting the bird back cannot skip this check.)
function mark_bird_w(h) = mark_w_mm(h, mark_rib);
mark_w = max(mark_bird_ok ? mark_bird_w(mark_h) : 0, mark_word_w(mark_h));
assert(mark_w <= plate_x - 2.0,
       str("The mark is ", mark_w, " mm wide on a ", plate_x,
           " mm plate — it will hang off the sides. Shrink mark_h; the ",
           "wordmark, not the bird, is what sets this width."));

// ===========================================================================
//  PRIMITIVES
// ===========================================================================

// The 2D rounded rect, deliberately still LOCAL: canary_core_lib owns this
// shape as rrect2d, but under the name `rrect` that library draws the 3D
// prism — a `use` here would leave every call one shadowed name away from a
// silent 2D/3D mix-up. That redraw happened: every call site now names
// rrect2d, and the module is canary_core_lib's — the copy is retired.

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
usb_r = 0.35;      // corner break of the series-A opening only — the shell's corners are close to sharp
module usb_a_2d(w, h, r = usb_r) {
    offset(r = r) offset(r = -r) square([w, h], center = true);
}

// The bore the shell passes through, drawn in the (x, z) plane about the
// shell's center: the shell box grown by `clr` at the sides and bottom, and
// a BRIDGE-SAFE top (canary_port_lib port_bridge_profile2d). The bezel prints
// face-down, so the top of any horizontal bore is a bridge — this one was a
// 12.0 mm flat lintel in the wall and an 11.8 mm flat roof in the collar.
// The 45° chamfers start at the SHELL's top line, not at the clearance box's,
// so they pass `clr` over the shell's corners and never cut its envelope;
// port_bridge_cham_for() sizes them to leave the catalog's 7.0 mm flat.
module usb_bore_2d(clr) {
    w = usb_shell_w + 2*clr;
    c = port_bridge_cham_for(w);
    h = usb_shell_h + clr + c;
    translate([0, h/2 - (usb_shell_h/2 + clr)]) port_bridge_profile2d(w, h, c);
}

// ===========================================================================
//  THE BEZEL — front frame, prints FACE-DOWN
// ===========================================================================

module bezel_solid() {
    linear_extrude(bez_h) rrect2d(xo, yo, r_out);
}

// The board cavity, plus the side "ears" that let the overhanging side
// buttons slide past the walls to their seat. Without the ears the board
// simply cannot reach the glass ledge — the same lesson the C6 case learned
// on its first print, and the buttons here are on the LONG edges.
module bezel_cavity() {
    translate([0, 0, face_t])
        linear_extrude(bez_h) rrect2d(xc, yc, r_in);

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
            rrect2d(aa_w, aa_l, 1.6);
}

// The plug opening + its relief step. (The drop collar is ADDED, after the
// cuts — see bezel().)
module bezel_usb() {
    // The through opening: shell + clearance, bridge-safe top.
    translate([usb_dx, yc/2 + wall/2, z_usb]) rotate([90, 0, 0])
        linear_extrude(wall*3, center = true)
            usb_bore_2d(usb_clear);

    // Relief on the OUTER face: a receptacle recessed in a wall-wart housing
    // meets air here instead of meeting the case. A plain step round the
    // shell box — 0.5 deep, so its ceiling is an overhang, not a bridge.
    translate([usb_dx, yo/2 + 0.01, z_usb]) rotate([90, 0, 0])
        linear_extrude(usb_relief, scale = 1.0)
            usb_a_2d(usb_shell_w + 2*usb_clear + 2*usb_relief,
                     usb_shell_h + 2*usb_clear + 2*usb_relief, usb_r + usb_relief);
}

module bezel_collar() {
    // The drop buttress. It reaches INWARD from the plug-end wall, never
    // outward — this is the whole subtlety of the feature. An outward collar
    // is the obvious shape and it is wrong: every millimeter outside the end
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
    //
    // It is ADDED to the bezel after the cavity is cut (bezel()), and that
    // is not a detail: drawn inside the difference, the cavity cut it
    // straight back out and the case shipped with no collar at all. Its bore
    // is the bridge-safe usb_bore_2d, so its crown bears on the shell's top
    // CORNERS through the 45° chamfers (collar_gap over them) — on the
    // stock stick the chamfers' 7.0 flat lies above the plate clip, and the
    // "U" is two flanks. Its underside is a 45° ramp up from the end wall,
    // not a flat shelf: printed face-down it hangs off that wall, and a flat
    // 3.4 mm cantilever is an overhang.
    intersection() {
        translate([usb_dx, yc/2 + 0.01, z_usb]) rotate([90, 0, 0])
            linear_extrude(collar_l)
                difference() {
                    offset(delta = collar_t) usb_bore_2d(collar_gap);
                    usb_bore_2d(collar_gap);
                }
        // clipped in Z only: behind the board, and 0.2 UNDER the plate's
        // underside (the plate used to sit on the collar's crown) ...
        translate([-xo, -yo, z_pcb_back])
            cube([2*xo, 2*yo, plate_z0 - 0.2 - z_pcb_back]);
        // ... and the 45° underside, rising inward from the end wall
        translate([-xo, 0, 0]) rotate([90, 0, 90]) linear_extrude(2*xo)
            polygon([[yc/2 + 0.02, z_pcb_back - 0.01],
                     [yc/2 + 0.02, bez_h],
                     [yc/2 - collar_l, bez_h],
                     [yc/2 - collar_l, z_pcb_back + collar_l]]);
    }
}
// The flanks have to stand past the shell's crown line, or the chamfers that
// carry the drop load have nothing to bear on.
assert(plate_z0 - 0.2 - (z_usb + usb_shell_h/2) >= 1.0,
       str("the drop collar stands only ", plate_z0 - 0.2 - (z_usb + usb_shell_h/2),
           " mm above the shell's crown line under the plate — thin back_t or deepen back_stack"));

// Button access, through the ear skin.
module bezel_buttons() {
    for (sx = [-1, 1])
        translate([sx * (xo/2 + 1), btn_y, z_btn]) rotate([0, -90*sx, 0])
            cylinder(h = wall + ear_bump + 2, d = btn_d, center = true);
}

// ── The light seam ────────────────────────────────────────────────────────
// A slot through both long walls at the LCD/PCB sandwich gap, so the RGB
// escapes as a line along the screen's edge instead of being sealed in by a
// solid wall.
//
// THE SEAM MUST OPEN INTO THE CAVITY, and that is not automatic. This slot
// was first written as a fixed-width cube parked near the outer face; the
// numbers worked out to an inner face at x = 11.01 against a cavity wall at
// 10.36, so it left a 0.65 mm curtain of black plastic between the LED and
// the band. It looked perfect in every render — a clean white line down each
// side — and would have shipped a light pipe with no light in it. So the x
// span is DERIVED from the cavity and outer faces, and asserted, rather than
// composed out of wall thicknesses that happen to add up.

// The run: from just past the free pair, centered on the glass. The plug-end
// pair no longer bounds it — those beams root above the seam's roof (the
// assert at beam_root_z) — so the line is placed for the eye: symmetric
// about the screen's center rather than wherever a snap happened to stop.
seam_y_lo = snap_y_free + snap_half + 1.0;
seam_y_hi = -seam_y_lo;

seam_x_in  = xc/2 - 1.0;                // starts inside the cavity: no lip, no curtain
seam_x_out = xo/2 + ear_bump + 1.0;     // clears the outer face, button ear included
assert(!light_seam || seam_x_in < xc/2,
       str("The light seam's inner face is at x=", seam_x_in, ", outside the ",
           "cavity wall at ", xc/2, ". The band would be a white inlay that ",
           "never sees the LED."));
assert(!light_seam || seam_x_out > xo/2 + ear_bump,
       str("The light seam stops at x=", seam_x_out, ", short of the outer ",
           "surface at ", xo/2 + ear_bump, ". It would be a buried pocket."));

// The seam volume. `shrink` insets the visible faces so the same geometry
// serves as the CUT (0) and as the BAND that fills it (band_clear) — they can
// never drift apart into a strip that does not fit its own slot.
module seam_prisms(shrink = 0) {
    z0 = face_t + seam_dz;
    y_lo = seam_y_lo;
    y_hi = seam_y_hi;
    for (sx = [-1, 1])
        translate([sx * (seam_x_in + seam_x_out)/2,
                   (y_lo + y_hi)/2,
                   z0 + seam_h/2])
            cube([seam_x_out - seam_x_in,
                  (y_hi - y_lo) - 2*shrink,
                  seam_h - 2*shrink], center = true);
}

// The ties that keep the bezel face bolted to its walls, and the reason they
// are RIBS rather than webs. The seam runs most of the wall, so something has
// to carry load across it or the face hangs off the corners alone — and this
// case is designed around being dropped. The first version broke the slot
// itself into segments, which does tie the face down but also chops the white
// line into a row of 2.3 mm dashes: unpressable as inserts, and it reads as a
// dotted line rather than a lit edge.
//
// So the tie moves INBOARD. The slot stays continuous through the outer skin
// — one unbroken white line per side, which is the look — and the ribs live in
// the inner `seam_web_d` of the wall, hidden behind the strip. Load still
// crosses: face rim -> rib -> wall, through solid material the whole way.
// `grow` swells them so the white strip gets notches it slides over rather
// than an interference fit against them.
module seam_web_ribs(grow = 0) {
    z0 = face_t + seam_dz;
    span = seam_y_hi - seam_y_lo;
    // Ribs sit at the interior boundaries of (webs + 1) equal stretches.
    step = span / (seam_webs + 1);
    if (seam_webs > 0)
        for (sx = [-1, 1], i = [1 : seam_webs])
            translate([sx * (xc/2 + seam_web_d/2),
                       seam_y_lo + i * step,
                       z0 + seam_h/2])
                cube([seam_web_d + 2*grow,
                      seam_web_w + 2*grow,
                      seam_h + 2], center = true);
}

// A rib that ate the whole wall would leave no white to see; one that ate none
// would not tie anything. Hold it to a real ligament on both sides.
assert(!light_seam || seam_webs == 0 || (seam_web_d >= 0.6 && seam_web_d <= wall - 0.8),
       str("Seam rib depth ", seam_web_d, " mm has to sit between 0.6 mm (a ",
           "tie worth having) and ", wall - 0.8, " mm (leaving 0.8 mm of white ",
           "still visible in front of it)."));

module bezel_light_seam() {
    difference() { seam_prisms(0); seam_web_ribs(0); }
}

// The band: exactly the wall material the seam removed, minus clearance. Built
// by intersecting the seam prism with the bezel's own shell, so a strip is
// always precisely as deep as the wall it sits in — including where the wall
// thickens into a button ear. The ribs are subtracted with clearance, so each
// side prints as ONE continuous strip carrying its own notches.
// The band exists only where the seam does. `light_band` on its own is not
// enough: with light_seam off, bezel() never cuts the pocket, so a band built
// anyway would be a strip of white occupying wall the bezel still has — two
// solids claiming the same material, which the 3MF packager would hand the
// AMS as an unresolvable overlap and the configurator would happily export.
// Same rule, same reason, as light_plug and led_win: a part that fills a
// feature does not get to outlive the feature.
band_on = light_seam && light_band;

module light_band() {
    if (band_on) difference() {
        intersection() {
            seam_prisms(band_clear);
            difference() {
                union() {
                    bezel_solid();
                    if (opt_btn) for (sx = [-1, 1])
                        translate([sx * (xo/2 + ear_bump/2 - 0.01), btn_y, face_t])
                            linear_extrude(bez_h - face_t)
                                rrect2d(ear_bump + 0.02, ear_w + 2, 0.8);
                }
                bezel_cavity();
            }
        }
        seam_web_ribs(band_clear);
    }
}

// The band must clear the LCD module at the front and REACH the PCB front at
// the back. Those are the two real constraints and they point in opposite
// directions:
//   - start too early and the band fouls the display module's own edge;
//   - stop short of the PCB and the strip's end never sees the LED, so the
//     whole feature is a white line that does not light up.
// So the far edge is allowed to land on the PCB plane (within a printing
// tolerance), not held clear of it.
// The band may start level with the glass (that is the design) but never in
// FRONT of it — there it would undercut the face that retains the board.
assert(!light_seam || seam_dz >= 0.0,
       str("The light band starts ", seam_dz,
           " mm behind the glass — negative means it undercuts the face."));
assert(!light_seam || seam_dz + seam_h <= lcd_rise + 0.25,
       str("The light band spans ", seam_dz, " .. ", seam_dz + seam_h,
           " mm behind the glass, past the PCB front face at ", lcd_rise,
           " mm. It would collide with the board."));

// ── The snap beams, cut into the bezel wall ───────────────────────────────
// Each is freed by a U-slot (two vertical slots through the wall, joined at
// the beam's root) and locally thinned on the inside to snap_beam_t so its
// strain stays in budget. The hook stands INWARD at the rim end.

// Hook cross-section, in (inward, up) with z=0 at the hook's shoulder. The
// plate comes DOWN onto it from the rim, so the TOP face is the entry — the
// plate's chamfered lower edge rides it on the way in — and the BOTTOM face
// is what holds the plate against being pulled back out. That underside is
// an overhang on the face-down bezel, which is why the hold is 45°.
//
// THE PEDESTAL, and why it is not optional. The beam is what survives the
// inside relief, which means the beam is the wall's OUTER skin — it sits
// `wall - snap_beam_t` further out than the cavity face. A hook drawn from the
// cavity face would therefore float in mid-air, attached to nothing: the mesh
// gate in render.sh catches it as extra disconnected parts, which is exactly
// how this was found. So the hook starts at the BEAM's inner face and carries
// a pedestal across the gap to the plate's edge; only the last `snap_eng`
// beyond that edge is engagement, which is what the plate's groove and the
// strain figure are both sized against.
hook_ped = (wall - snap_beam_t) + tol_press;   // beam face -> plate edge
kJoin = 0.3;   // overlap into the parent so the union is ONE solid

module hook_profile() {
    // the pedestal's underside runs 45° from the beam face to its tip (a flat
    // 1.65 mm shelf printed in air on the face-down bezel)
    polygon([[-kJoin, -hook_hr - hook_ped - kJoin],
             [hook_ped, -hook_hr],
             [hook_ped + snap_eng, 0],
             [hook_ped + snap_eng, snap_flat],
             [hook_ped, snap_flat + hook_er],
             [-kJoin, snap_flat + hook_er]]);
}

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
        for (sx = [-1, 1])
            translate([sx * (xc/2 + wall - snap_beam_t), yy,
                       groove_mid_z - snap_flat/2])
                rotate([90, 0, 0])
                    linear_extrude(snap_w, center = true)
                        scale([-sx, 1]) hook_profile();
    }
}

module bezel_flick_scallop() {
    // The scoop through the far wall's rim that bares the plate's far edge.
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
                            rrect2d(ear_bump + 0.02, ear_w + 2, 0.8);
            }
            bezel_cavity();
            bezel_window();
            bezel_usb();
            if (opt_btn) bezel_buttons();
            bezel_snap_relief();
            if (light_seam) bezel_light_seam();
            bezel_flick_scallop();
        }
        // Hooks go on AFTER the reliefs are cut, or the slots would eat them —
        // and the collar for the same reason: it stands IN the cavity, so
        // inside the difference the cavity cut removed it whole.
        bezel_hooks();
        if (collar_on) bezel_collar();
    }
}

// ===========================================================================
//  THE BACK — snaps in, carries the mark, releases with a thumb
// ===========================================================================

// The plate is authored in its OWN frame: z = 0 is its underside (the face
// that looks into the case), z = back_t its outer face (the one that wears
// the mark). The renderer places it.
module back_plate() {
    // Lead-in chamfer on the bottom OUTER edge — this is the face that rides
    // the hooks' top (entry) faces on the way down, so the plate guides itself
    // in rather than needing to be aimed. snap_eng deep, no more: the plate
    // below the groove is the lip the hooks HOLD, and every tenth of chamfer
    // past the hook tip is a tenth of that lip gone.
    //
    // Built as a hull from a smaller bottom profile up to the full outline,
    // NOT as a subtracted taper. Subtracting one removes the middle of the
    // plate's underside rather than its edge, which both guts the plate and
    // leaves the PCB ribs standing on air — the mesh gate counts those as
    // extra parts, which is how the mistake surfaced.
    lead = snap_eng;
    union() {
        hull() {
            linear_extrude(0.01)
                rrect2d(plate_x - 2*lead, plate_y - 2*lead, max(0.4, r_in - lead));
            translate([0, 0, lead]) linear_extrude(0.01)
                rrect2d(plate_x, plate_y, r_in);
        }
        translate([0, 0, lead]) linear_extrude(back_t - lead)
            rrect2d(plate_x, plate_y, r_in);
    }
}

// The groove the hooks seat in, around the whole perimeter. Running it right
// around (rather than four local pockets) means the plate has NO orientation
// to get wrong — it drops in either way up-the-long-axis, which matters for a
// part a user takes off in a dark hallway.
//
// Its section IS the hook's section grown by tol_press — the tip line moved
// in, the top face moved off along its normal — not a rectangle sized by
// eye. The rectangle it replaces was 0.8 tall against a hook 1.34-1.75 tall
// where it crosses the plate edge: every seat was ~0.3 mm of interference.
// The UNDERSIDE is not grown: it is the seat. The ribs' preload pushes the
// plate out against the four hold faces, so the plate rests on them flush
// with the rim, and a clearance there would only be play spent out of the
// rib crush (0.14 of the 0.25 at tol_press).
// Built as a stack of hulls between inset outlines, so the sloped faces are
// true 45° ruled surfaces round the corners too.
module back_groove() {
    v0 = back_t/2 - snap_flat/2;                   // hook shoulder, plate frame
    D  = snap_eng + tol_press;                     // groove depth at the tip
    lo = v0 - hook_hr;                             // underside line at the edge — the seat
    hi = v0 + snap_flat + hook_er + tol_press/cos(snap_entry);
    vb = lo + D * tan(snap_hold);                  // the tip's bottom corner
    vc = hi - D * tan(snap_entry);                 // ... and its top corner
    o  = 1.0;                                      // run the faces past the edge
    module ring_at(z, d)
        translate([0, 0, z]) linear_extrude(0.01)
            rrect2d(plate_x - 2*d, plate_y - 2*d, max(0.01, r_in - d));
    difference() {
        translate([0, 0, lo - o*tan(snap_hold)])
            linear_extrude(hi - lo + o*(tan(snap_hold) + tan(snap_entry)))
                rrect2d(plate_x + 2*o + 1, plate_y + 2*o + 1, r_in + o);
        // what the groove leaves of the plate: full outline outside the
        // band, inset to the tip line D inside it
        // (the lower hull's slices hang BELOW their z: the kept surface is
        // their top, and it has to be the seat line exactly)
        hull() { ring_at(lo - o*tan(snap_hold) - 0.01, -o); ring_at(vb - 0.01, D); }
        hull() { ring_at(vb, D); ring_at(vc, D); }
        hull() { ring_at(vc, D); ring_at(hi + o*tan(snap_entry), -o); }
    }
}

// Compliant PCB ribs: thin standing ribs that crush slightly rather than a
// hard boss. `preload` is the interference. A rigid clamp would make every
// drop a bending moment across the board; a rib that gives 0.25 mm turns the
// same impact into a squeeze the PCB does not care about.
// On the headered build these grow from 3.0 mm to 8.55 — and that is a
// gentler preload, not a harsher one. A rib loaded along its length is an
// axial spring, k = EA/L, so tripling the length cuts the force the same 0.25
// mm of interference produces to a third of it. The compliance argument above
// gets stronger with depth; what needs watching is the rib's slenderness, and
// the assert below watches it.
// Where the ribs stand along the board. The far pair sits 4.5 in from the
// cavity end as it always has. The plug-end pair used to mirror it — and so
// stood exactly where the drop collar is, once the collar was actually in
// the bezel: it is moved inboard to clear the collar's inner end by 0.3.
rib_ys = [-(yc/2 - 4.5),
          collar_on ? yc/2 - collar_l - rib_l/2 - 0.3 : yc/2 - 4.5];
// Nothing on the back may stand past the plate's own outline — the lift lug
// that did stood 1.2 mm past the far end, onto the bezel rim, and held the
// plate off its seat. The ribs are the only thing below the plate now.
assert(max([for (y = rib_ys) abs(y) + rib_l/2]) <= plate_y/2
       && xc/2 - rib_inset + rib_w/2 <= plate_x/2,
       "a compliant rib stands past the back plate's outline");

module back_ribs() {
    // Extruded a hair PAST z=0 into the plate: a rib that merely touches the
    // underside is a separate solid to CGAL, and the mesh gate counts it as
    // another part. Same reason as the hook pedestal above.
    // plate underside -> PCB back is stack_eff; the rib reaches preload PAST
    // the PCB plane (it was stack_eff - preload: a 0.25 gap, not a 0.25 crush)
    rib_h = stack_eff + preload;
    for (y = rib_ys, sx = [-1, 1])
        translate([sx * (xc/2 - rib_inset), y, -rib_h])
            linear_extrude(rib_h + kJoin) square([rib_w, rib_l], center = true);
}


module back_cutouts() {
    // The card window.
    if (sd_window)
        translate([0, yc/2 - sd_from_usb, -1])
            linear_extrude(back_t + 2) rrect2d(sd_w, sd_l, 1.4);

    // The LED wall-wash window.
    if (led_win)
        translate([0, -yc/2 + led_from_far, -1])
            cylinder(h = back_t + 2, d = led_d);
}

// The mark, as a deboss in the back's outer face. `grow` lets the ACCENT part
// be generated a hair larger so the yellow inlay actually touches the walls
// of the pocket it sits in rather than rattling inside it.
module back_mark(grow = 0) {
    translate([mark_dx, mark_dy, back_t - mark_depth])
        linear_extrude(mark_depth + 0.02)
            offset(r = grow)
                if (mark_bird_ok) mark_lockup(mark_h, mark_rib);
                else              mark_wordmark(mark_word_h);
}

module back_body() {
    difference() {
        union() {
            back_plate();
            back_ribs();
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
//
// It only exists when the window does: with led_win off there is no hole for
// it to fill, and a part list that keeps offering it is a part list that gets
// one printed.
module light_plug() {
    if (led_win) translate([0, 0, 0]) {
        cylinder(h = led_skin, d = led_d - 0.15);
        // A retaining flange on the inside face.
        translate([0, 0, led_skin]) cylinder(h = 0.8, d = led_d + 1.4);
    }
}

// ===========================================================================
//  RENDER
// ===========================================================================

module back_assembly() { color(cw_body(colorway)) back_body(); color(cw_ink(colorway)) back_accent(); }

// The bezel as it looks assembled: black body, white light band.
//
// ⚠️ DO NOT JUDGE THE PALETTE FROM THIS, or from any part that composites it
// ("all", "exploded", "fit_section"). The band sits exactly in the pocket it
// was cut from, and OpenCSG merges the two into one product and paints it with
// the LAST color — it renders the entire bezel white. The same caveat is on
// the 7" frame's frame_color and in README §"Preview renders". These views are
// good for silhouette and for fit; use part="palette" to see the colors.
module bezel_assembly() { color(cw_body(colorway)) bezel(); if (band_on) color(cw_light(colorway)) light_band(); }

// The three filaments, spread far enough apart that no two solids overlap —
// which is the whole point, because non-overlapping groups are the ONE case
// OpenCSG colors reliably. This is the view that answers "what does it look
// like in black, white and yellow", and it cannot lie the way an assembly can.
module palette_row() {
    color(cw_body(colorway)) bezel();
    if (band_on) translate([xo + 8, 0, 0]) color(cw_light(colorway)) light_band();
    translate([2*(xo + 8), 0, 0]) {
        color(cw_body(colorway)) back_body();
        color(cw_ink(colorway)) back_accent();
    }
}

// PRINT ORIENTATION for the back is mark-face-DOWN (so the ribs point up and
// print in air-free order, and the A-surface takes the textured plate's
// finish — the same rule every case in this catalog follows). The assembly
// views below seat it the other way up, which is how it actually sits.
module back_printed() { rotate([180, 0, 0]) translate([0, 0, -back_t]) back_assembly(); }

if (part == "bezel") bezel();
else if (part == "back") back_printed();
else if (part == "light") light_plug();
else if (part == "fil_light") light_band();
else if (part == "palette") palette_row();
else if (part == "fil_body") rotate([180, 0, 0]) translate([0, 0, -back_t]) back_body();
else if (part == "fil_accent") rotate([180, 0, 0]) translate([0, 0, -back_t]) back_accent();
else if (part == "all") {
    bezel_assembly();
    translate([xo + 6, 0, 0]) back_printed();
    translate([xo + 6, yo/2 + 8, 0]) light_plug();
}
else if (part == "exploded") {
    bezel_assembly();
    translate([0, 0, plate_z0 + 15]) back_assembly();
}
else if (part == "fit_section") {
    // Half-section down the long axis — the view that shows whether the
    // collar, the plug opening, the board seat and the snap engagement
    // actually agree with each other.
    difference() {
        union() { bezel_assembly(); translate([0, 0, plate_z0]) back_assembly(); }
        translate([-xo, -yo, -5]) cube([xo, 2*yo, bez_h + 40]);
    }
}
else bezel();

echo(str("USB-A insertion length left clear: ", usb_free, " mm (need >= 11)"));
echo(str("Snap beam strain: ", snap_strain*100, " % (PETG budget 1.8)"));
echo(str("Outer shell: ", xo, " x ", yo, " x ", bez_h,
         " mm (the back plate seats FLUSH with the rim, so the bezel ",
         "height is the whole thickness); plug adds ", usb_proud, " mm"));
