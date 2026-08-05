// ============================================================================
//  Canary — ESP32-C3-LCD-1.47 POCKET CASE  ⚠️ IN DEVELOPMENT (v0.2-dev)
//
//  A three-color PETG case for the Waveshare ESP32-C3-LCD-1.47: BLACK body,
//  YELLOW snap-on lid, and a WHITE light band that puts the onboard RGB LED's
//  glow where the board itself puts it. Waveshare ship this board as a "clear
//  acrylic sandwich" — the LED washes the acrylic and the light leaves at the
//  edges. This case replaces the sandwich, so the band is not a decoration:
//  it is the acrylic's job, done in white PETG.
//
//  ⚠️ NOT the C6 case, and NOT the S3 stick, though it borrows from both.
//     canary_c6_display.scad covers the ESP32-C6-LCD-1.47 — the SAME PCB
//     outline and the SAME 1.47" panel, which is why that file's fit-tested
//     numbers (USB stadium, button channels, ear/chin bulges) are the
//     starting point here. What the C3 board adds: a microSD (TF) slot on
//     the back, an RGB LED at the far end, and this case's three-filament
//     look. What it lacks: the C6 file's 180°-flippable lid — see THE LID IS
//     KEYED below. Do not merge the files; boards drift apart, files that
//     serve them must be free to.
//
//  ── THE FIVE THINGS THIS CASE IS FOR ─────────────────────────────────────
//
//  1. THE BOARD GOES IN WITHOUT A FIGHT. The BOOT/RST buttons and the USB-C
//     shell all overhang the PCB edges (the C6 case's first print proved a
//     board like this cannot reach its seat past plain walls). The side
//     walls carry full-depth clearance channels ("ears") the button
//     overhangs slide down, and the USB wall carries a slot from the
//     stadium's floor to the rim the shell descends. Insertion is a
//     straight drop, glass-first. The buttons are then worked by
//     printed-in-place flexure paddles in the ears — see [Buttons].
//
//  2. THE PLUG SEATS ALL THE WAY. The USB-C opening is a true stadium
//     (full-round ends, like the connector) hugging the shell at
//     ~0.05 mm per side — print 2 (kmay89) proved the position, so the
//     opening earns the tight number. It centers on the SHELL
//     (back-mounted, resting on the PCB back face), so tightening the
//     opening can never shift it off the port. The outer rim carries a
//     CHAMFER (`usb_cham`) so a plug's overmold meets a lead-in, not a
//     wall edge. It was a recessed ring until print 2: the ring's floor
//     crossed the insertion slot at 0.4 mm thick and broke out — the
//     chamfer leaves no thin floor to break. The wall stays thin enough
//     (wall + chin ≈ 3.5 mm) that any compliant plug bottoms out on the
//     shell, never on the case.
//
//  3. THE GLASS IS LOCATED, NOT CLAMPED. The panel's front face IS glass,
//     and glass fails from stress at its edges. Three rules here:
//       - the lid's press bosses are cut to the EXACT component stack, so
//         the board is positioned forward, not preloaded into the face;
//       - the bezel face touches the panel only on a LAND over the module's
//         outer border — the innermost `glass_relief_w` of the lip is
//         relieved 0.25 mm, so the window's rim never contacts the glass
//         (a contact line at a cut edge is exactly the stress raiser this
//         panel must not see);
//       - the face is thin (`face_t` 1.2) and the window is the active area
//         with rounded corners — the smallest bezel that still retains the
//         module, which is the "minimize this" half of the requirement.
//
//  4. THE LIGHT BAND SITS WHERE THE LIGHT IS. Measured on the hardware
//     (kmay89): from the front face of the display, 1.0 mm of black, then
//     2.8 mm of white, then black to the back. `seam_dz`/`seam_h` are those
//     two numbers verbatim, referenced to the glass front plane — so the
//     band lands on the LCD/PCB edge gap the LED actually fills, and its far
//     edge reaches (nearly) the PCB front face where the light lives. The
//     band is WHITE / natural PETG — a light pipe, translucent and
//     diffusing, not a slot. It is ONE piece: up both long walls, around
//     both far corners, across the far short wall — a continuous U of
//     light around the end the LED sits on (the USB end stays black —
//     there is nothing to light there). Two print-taught rules shape it:
//       - white through the FULL wall depth along the whole run. The
//         first cut tied face to wall with hidden inboard ribs across the
//         seam, and every rib was a black slat between the LED and the
//         white — a shadow on the band at each one (kmay89's print).
//       - white around the CORNERS. The next cut stopped each strip short
//         of the corner posts, and the slicer view showed the U broken
//         into three dashes with black bends — the pipe went dark exactly
//         where it turns. The seam now wraps the corners, so the U is one
//         volume and the glow is continuous.
//     The face ring above the seam rides the solid USB wall and its two
//     corner posts; the band's leg ends stop at black wall above the ears,
//     so the U still cannot slide along its run.
//
//  5. THREE BOARD BUILDS, ONE FILE (the C6 case's contract, plus one):
//       headers="pillars" — the board AS WAVESHARE SHIPS IT: the four brass
//                        M2 corner pillars installed, header pins NOT
//                        soldered (the product photos show exactly this).
//                        The cavity is brass_h + stand_gap deep below the
//                        PCB back (both MEASURED on print 2), and the
//                        press bosses span stand_gap to land on the flat
//                        brass pillar TOPS — the best press pads on the
//                        board, lightly holding it against the bezel. The
//                        lid skirt keeps a corner notch around each pillar
//                        position (with the measured 3.0 pillars they stop
//                        just short of the skirt band; the notches cost
//                        nothing and guard taller pillar batches).
//       headers="none" — stripped board: no headers AND the pillars
//                        unscrewed; the case stays shallowest (the
//                        back-mounted USB shell and TF cage set the depth).
//       headers="male" — headers soldered pointing DOWN plus the pillars.
//                        The cavity deepens to swallow base + pins, and the
//                        snap skirt goes thin to clear the pin rows.
//     Every way the board is captured by its edges — nothing screws into it.
//
//  ── THE BACK IS THE BRAND *AND* THE MOUNT ────────────────────────────────
//  It carries both, because it turned out there was room: the plate is
//  41 mm tall and the wordmark takes four of them. The hanger sits high,
//  the "Canary" wordmark reads below it, and `lid_back` still offers
//  either one alone. What keeps them honest is not taste but asserts —
//  they must clear each other by 2 mm, and the hanger (the only cut that
//  goes THROUGH) must additionally clear the press bosses and stay out of
//  the skirt's snap band, which it would otherwise notch.
//
//  THE HANGER IS AN EGG. Not decoration: the old Ø7 head circle admitted
//  exactly one screw size, and a screw already in a wall did not fit it
//  (kmay89). An egg tapers continuously from base to crown, so ONE opening
//  serves a RANGE of heads — offer the head in at whatever width clears it,
//  let the case drop, and the flank carries the screw up to the crown,
//  which is sized to the shank and will not give the head back. The shape
//  is canary_vent_lib's egg2d — the house ovoid, one drawing for the line —
//  used in the mounted-upright orientation that library's doctrine asks
//  for: wide base DOWN (the entry, at the USB end), crown UP.
//
//  THE BIRD IS NOT ON THIS PART, and that is not an omission. The house
//  mark (canary_mark_lib.scad) has interior detail — a C spiralled into the
//  wing, a V in the tail — with a stated MINIMUM SIZE of roughly 30 mm.
//  This lid is 25.12 mm across. The bird would be wider than the part
//  before it was legible on it, so this face carries the WORDMARK ALONE,
//  which is exactly what that library's MINIMUM SIZE section prescribes.
//  The alternative — a second, simplified bird for small parts — is the one
//  thing that library exists to prevent. Same call the hallway stick's back
//  plate made, for the same reason.
//
//  ── THE LID IS KEYED — it fits ONE way, and that is load-bearing ─────────
//  The C6 case's lid is 180°-rotation symmetric. This one cannot be: the
//  board's four M2 pillars are NOT on a symmetric pattern (USB-end pair at
//  ±8.89 / 2.40 in from the end, far pair at ±6.64 / 1.97 in — read off the
//  Waveshare drawing), and the press bosses must land on them. So the skirt's
//  USB relief exists at one end only: offered the wrong way round, the solid
//  skirt segment lands on the USB shell and the lid simply will not close —
//  the error is physical, immediate, and harmless. The keyhole hangs the
//  case USB-down (the water-shedding way), slot pointing up.
//
//  ── microSD ──────────────────────────────────────────────────────────────
//  The TF slot sits on the back under the USB connector, and the card
//  inserts PARALLEL to the board with its mouth facing down-board — there is
//  no orientation of a wall window that a card could pass through. So the
//  card story is the lid: four nubs, off in a second, slot right there. No
//  window is cut for it, and that is the considered choice — a hole that
//  cannot admit a card is just a hole.
//
//  ── PRINTING ─────────────────────────────────────────────────────────────
//  Three spools, all PETG, and the SLOTS ARE ROLES — the same three roles
//  the hallway stick uses, so an operator who has printed one loads the
//  other without re-learning the spools:
//        slot 1 = BODY      → YELLOW   (bezel AND lid: the case is yellow)
//        slot 2 = MARK      → BLACK    (the "Canary" wordmark on the back)
//        slot 3 = LIGHT     → WHITE / natural (the band)
//  ⚠️ This is the INVERTED palette, and the loading changed with it. The
//     first three prints ran slot 1 BLACK / slot 2 YELLOW; the case is now
//     yellow with black branding, so slot 1 and slot 2 SWAP SPOOLS. The
//     roles did not move — only the colors in them did.
//  The band is one closed U, and that changes how it is installed:
//    - Multi-material (the first-class path, what gen_3mf.py c3 packages):
//      band_clear = 0, bezel + band as ONE object — the U fuses in.
//    - Single spool: a closed U CANNOT press in from outside — the legs
//      and the top approach in conflicting directions, so the old
//      press-in flow died with the three-strip band. Instead: print the
//      U flat on its bottom face (it is seam_h tall), PAUSE the
//      face-down bezel print at the top of the seam (z = face_t +
//      seam_dz + seam_h), drop the U into the open pocket, resume.
//      band_clear 0.10 sizes that drop-in.
//  Bezel prints FACE-DOWN, lid prints OUTER-FACE-DOWN, the U flat on its
//  bottom face. No supports anywhere. The wordmark is a DEBOSS in the lid's
//  outer face filled by the mark volume, so it prints on the build plate —
//  first layer, no overhang, crisp letters.
//
//  Orientation: +Y = up (portrait), USB-C exits the BOTTOM (−Y) short wall,
//  +Z = toward the glass. The RGB LED lives at the TOP (+Y) end.
//
//  ⚠️ DEV STATUS: dimensioned from the Waveshare outline drawing plus the
//     C6 sibling case's two fit prints (same PCB outline, same panel — its
//     stadium/button/ear numbers transfer). Every number tagged MEASURE is
//     one to check with calipers before a long print. The one that DID
//     bite: btn_up. The drawing's 11.31 was read as button center from the
//     USB edge; the first assembly (photo, kmay89) showed the ears landing
//     4.4 mm up-board of the buttons — 11.31 is measured from the BOARD
//     CENTER (18.185 − 11.31 = 6.875 ≈ the C6's fit-tested 7.0). Fixed,
//     and the USB slide channel snugged in the same round. Still open:
//     hdr_inset (drawing candidates are 1.27 and 2.00 — measure which),
//     pcb_t, usb_proud, and the pillar pattern insets.
// ============================================================================

use <canary_mark_lib.scad>   // the house mark; this part wears the wordmark
use <canary_vent_lib.scad>   // the house egg — here it is the hanger, not a vent

/* [What to render] */
part  = "all";      // ["bezel","lid","light","mark","all","exploded","palette"]
// board build: "pillars" = as Waveshare ships it (brass corner pillars on, headers not soldered), "none" = stripped (pillars unscrewed too), "male" = down-facing headers + pillars
headers = "pillars";   // ["pillars","none","male"]

/* [Board] — ESP32-C3-LCD-1.47, from the Waveshare drawing (mm) */
board_l = 36.37;   // PCB long axis (Y, portrait height)
board_w = 20.32;   // PCB short axis (X)
pcb_t   = 1.6;     // PCB thickness — MEASURE (drawing does not call it out)
lcd_rise   = 3.65; // glass front above the PCB front face (same 1.47" module as the C6)
back_stack = 4.8;  // back clearance below the PCB, stripped board: the BACK-
                   // mounted USB shell (≈3.3) is the tallest thing (the TF
                   // cage is shorter), plus a printable bridge — MEASURE

/* [Headers] — the "male" build only. Rows run along the two long (±X)
   edges, pins point down toward the lid, brass M2 pillars on the corners. */
hdr_drop  = 8.8;   // cavity depth below the PCB back swallowing base + pins
                   // (the C6 case fit-tested 8.8 on this outline) — MEASURE
hdr_inset = 1.6;   // PCB edge → header row centerline. The drawing offers two
                   // readings (1.27 if the 17.78 dim is the column span, 2.00
                   // if that is the edge inset) — MEASURE; the skirt assert
                   // below is what a wrong value trips
brass_h   = 3.0;   // factory pillar height above the PCB back — MEASURED
                   // (kmay89, print 2): the lid-inner-to-pillar-top gap is
                   // 2.8 mm in the printed case, whose cavity sits 5.8 below
                   // the PCB back — so the pillars stand 3.0, not the 5.0
                   // this file guessed. The 2.0 mm error is why print 2's
                   // board floated: the 0.8 mm bosses stopped far short.
stand_gap = 2.8;   // lid inner face → pillar top — MEASURED (kmay89, print
                   // 2), and load-bearing twice: it is the press-boss
                   // height (the stub reaches the pillar and just lightly
                   // holds the board against the bezel), and with brass_h
                   // it reproduces the pillars-build cavity depth exactly
                   // (3.0 + 2.8 = 5.8 — print 2's proven stack, so the
                   // bezel is unchanged by this fix)

/* [Screen] — active area = the window; the module border sits under the lip */
aa_l  = 32.35;     // active area, long (Y)
aa_w  = 17.39;     // active area, short (X)
lcm_l = 36.28;     // LCD module outline, long
lcm_w = 19.39;     // LCD module outline, short

/* [Glass protection] — rule 3: locate, don't clamp */
glass_relief   = 0.25;  // face stand-off over the innermost lip band, so the
                        // window rim never touches the glass
glass_relief_w = 0.5;   // width of that relieved band, from the window edge out

/* [USB-C] — bottom (−Y) short wall, mounted on the BACK of the PCB. The
   opening is a stadium hugging the receptacle shell (nominal 8.94 × 3.26).
   Print 2 (kmay89) confirmed the position dead-on, so the opening now runs
   TIGHT: shell + ~0.05 per side. */
usb_w  = 9.05;     // stadium opening width — shell + 0.11 total
usb_h  = 3.35;     // stadium opening height — shell + 0.09 total
usb_dx = 0.0;      // sideways offset of the connector center — MEASURE
usb_dz = 0.0;      // depth offset from shell-on-back-face nominal — MEASURE
usb_proud  = 1.9;  // shell overhang past the PCB edge — MEASURE
usb_cham   = 0.6;  // outer-rim CHAMFER around the stadium — the plug's
                   // lead-in. This was a 0.8-deep stadium RECESS until print
                   // 2 exposed the flaw: the recess floor over the insertion
                   // slot was a 0.4 mm skin ring, and it broke out above the
                   // shell — a ragged hole where a crisp rim belonged. A
                   // chamfer thins progressively instead of leaving a
                   // parallel thin floor, so there is nothing left to break.

/* [Buttons] — BOOT/RST on the two side (±X) walls, mounted on the BACK of
   the PCB with side-facing actuators, flanking the TF cage. */
opt_btn = true;
// The buttons are printed-in-place FLEXURE PADDLES, not holes. Print 2's Ø6.5
// finger holes worked but bared the switch and the PCB behind it — a porthole
// onto the guts. Each ear now carries a press-pad cut free of the wall by a
// 0.55 mm slot on three sides, hinged at its USB-end edge, with a boss on its
// back that lands on the actuator. Press the pad: the beam flexes ~0.4 mm,
// the boss takes the click, the switch's own spring returns it. Nothing
// internal is visible — the slot is the only opening. Squeeze-safety is kept
// by the same recess doctrine as before: the pad face sits pad_recess below
// the ear's outer face, so a gripping finger lands on the ear rim, not the
// pad. Layer-friendly by construction: the hinge line is vertical, so the
// beam bends within print layers, never across them.
pad_l      = 8.2;  // paddle length (Y) — hinge at the USB end, press pad at
                   // the free end. Longer = softer press — TUNE on print
pad_h      = 5.4;  // paddle height (Z), centered on the actuator — a CAP,
                   // not a promise: shallow builds derive down from it
                   // (pad_h_eff), because the stripped board's case is a
                   // millimeter shallower and a fixed 5.4 ran the recess
                   // into the rim band there. CI's render of the bare build
                   // caught it; the local check that "passed" had exported
                   // to /dev/null, which OpenSCAD rejects before evaluating
                   // anything — a green that never ran. Verify with real
                   // output paths.
pad_slot   = 0.55; // flex slot around the three free sides — prints clean in
                   // a vertical wall at 0.55
pad_recess = 0.6;  // pad face below the ear's outer face: the squeeze guard,
                   // and what sets the beam: beam = ear_skin − pad_recess
                   // (0.8). Deeper recess = thinner beam = softer press
pad_boss_d = 2.6;  // press boss Ø on the pad's back — rides inside the
                   // actuator channel, lands square on the switch nub
pad_boss_l = 0.25; // boss reach: leaves free_gap (asserted) before contact
btn_up  = 7.0;     // button center up from the USB (−Y) PCB edge. The first
                   // assembly (photo, kmay89) caught the drawing's 11.31 as a
                   // CENTER-referenced dim, not edge-referenced: the board's
                   // ears hit plain wall 4.4 mm up-board of the buttons.
                   // 18.185 − 11.31 = 6.875 ≈ the C6 case's fit-tested 7.0 on
                   // this same outline, and the photo's header-pitch ruler
                   // agrees (≈6.9). Fit-confirmed by that print — not MEASURE
                   // anymore.
btn_dz  = 1.0;     // actuator center behind the PCB back face — MEASURE
btn_proud  = 1.8;  // black actuator overhang past the PCB edge — MEASURE
btn_ch_w   = 3.4;  // actuator channel width — hugs the nub, nothing more
btn_body_w = 5.2;  // shallow relief width for the switch's metal body — MEASURE
btn_body_p = 0.4;  // metal body overhang past the PCB edge — MEASURE

/* [Light band] — the white PETG that does the acrylic sandwich's job.
   Measured on the hardware (kmay89), stated as the side elevation you look
   at: from the display's front face, 1.0 mm of black, then 2.8 mm of white,
   then black to the back. Both numbers are referenced to the GLASS FRONT
   plane (z = face_t), so the band tracks the panel, not the case. */
light_seam = true;
seam_dz = 1.0;       // black between the glass front and the band's start
seam_h  = 2.8;       // the white band's thickness
band_clear = 0.10;   // per-face drop-in clearance (pause-and-insert flow);
                     // 0 for a co-printed band
// NO tie ribs across the seam — not the S3 stick's outer-skin webs (those
// chop the strip into unpressable dashes) and not this file's first cut,
// inboard ribs (those put black slats between the LED and the white: the
// first print's band carried a shadow at every rib). Any tie that crosses
// the seam sits in the light path on one face or the other; the pipe must
// be white through the full wall depth, so the seam has none.

/* [Back face] — what the lid's outer face carries */
// "both"    the hanger high on the back, the wordmark below it. There is
//           room: the plate is 41 mm tall and the wordmark takes 4 of it.
//           The two are placed by kh_cy / mark_cy and the asserts keep them
//           apart, off the press bosses, and out of the skirt band.
// "mark"    wordmark alone, centered.
// "keyhole" hanger alone, centered.
lid_back = "both";   // ["both","mark","keyhole"] back of the lid: brand, wall mount, or both
//
// THE HANGER IS AN EGG, and that is not decoration — it is the fix for a
// real failure. The old keyhole was a Ø7 head circle hulled into a slot,
// and a screw already in a wall did not fit it (kmay89). A round hole
// admits exactly ONE head size; the egg's flank tapers continuously from
// the base to the crown, so ONE opening takes a range of heads — enter at
// whatever width clears, drop, and the taper grips the shank. The shape is
// canary_vent_lib's egg2d, the house ovoid, used here in the mounted-
// upright orientation that library's doctrine asks for: wide base DOWN
// (the entry), crown UP (where the screw ends up when the case hangs).
// These two are NOMINAL SCREW dimensions — measure the screw, write it here.
// The openings are derived from them further down by adding this catalog's
// printed-hole clearance, which is the part the first cut of this egg got
// wrong: it used the nominal numbers RAW (a 9.6 opening for a 9.5 head,
// 0.05 per side), and on FDM PETG that is a hole that binds — the very
// failure the egg was drawn to fix, reintroduced one line lower down.
kh_head_d  = 9.5;   // the LARGEST screw head to hang on — the number that
                    // was 7.0 and too small for a screw already in a wall
kh_shank_d = 4.2;   // the shank the crown grips
kh_egg_l = 15.0;    // egg length along the hang axis; the taper's travel
kh_cy    = 5.0;     // hanger center, CASE frame (+Y = up when hung). Not
                    // higher: widening the egg for real screw heads pushed
                    // its footprint into the FAR press bosses at y 16.2,
                    // and a through cut there would hole the boss that
                    // holds the board down. The assert caught it at 8.0;
                    // 5.0 clears the bosses and still puts the crown —
                    // where the screw actually ends up — at y 12.5, well
                    // above the case's middle, so it hangs plumb

/* [Branding] — the wordmark, debossed and filled in the mark filament */
mark_word   = "Canary";  // the DEVICE's name (the company's is securaCV).
                         // The library's mark_wordmark() defaults to the
                         // company; this part is a Canary, so it says so
mark_word_h = 3.6;       // cap height. The width assert below is the real
                         // constraint on this narrow a plate — MEASURE by
                         // eye on the first print, it is the one number
                         // here that is a taste call rather than a fit
mark_depth  = 0.6;       // deboss depth; the inlay fills it flush
mark_dx = 0;             // sideways nudge (same in both frames)
mark_cy = -11.0;         // wordmark center, CASE frame (+Y = up when hung),
                         // so it reads BELOW the hanger. Both single-feature
                         // modes ignore this and center instead
// Per-character advance as a fraction of cap height — the wordmark's width
// cannot be MEASURED here (OpenSCAD has no text-metrics primitive), so it is
// estimated, and deliberately a hair pessimistic. Same constant the hallway
// stick calibrated against a measured render; the failure it guards is a
// mark hanging off the edge of the part, which has shipped before.
mark_adv = 0.875;

/* [Ventilation] — side-wall slots only; the row sits BEHIND the light band
   so the white line stays whole. The lid used to carry a matching grille —
   print 2's verdict (kmay89) was that tiny holes in the back read as
   confusion, not function, so the lid is now a clean plate: keyhole only. */
opt_vent = true;
vent_n = 4;          // slots per side wall
vent_pitch = 5.0;
vent_w = 1.4;

/* [Board pillars] — the four M2 positions, off the Waveshare drawing. NOT a
   symmetric pattern: the USB-end pair sits wide, the far pair inboard. The
   lid's press bosses land here, which is why the lid is keyed. */
hole_ix_usb = 2.00;  // USB-end pair: inset from the ±X edges — the drawing's
                     // 2.00. The rejected reading was 1.27 (17.78 as a hole
                     // span rather than the header column span, which is
                     // exactly 7 x 2.54): that would hang a Ø3.5 pillar 0.3
                     // past the PCB edge and into the bezel wall, and the
                     // product photos show no pillar overhang. The pillar
                     // clearance asserts below are what a wrong value trips
                     // — MEASURE
hole_iy_usb = 2.40;  //               inset from the USB (−Y) edge
hole_ix_far = 3.52;  // far pair: inset from the ±X edges — MEASURE
hole_iy_far = 1.97;  //           inset from the far (+Y) edge
brass_d = 3.5;       // pillar body Ø (round brass M2 standoff) — MEASURE

/* [Print tolerances] — tune with canary_fit_coupon.scad */
tol_slide = 0.20; tol_press = 0.10; tol_hole = 0.30;

/* [Shell] */
wall   = 2.2;    // side wall thickness
face_t = 1.2;    // bezel face over the glass border — thin on purpose (rule 3)
back_t = 2.0;    // lid plate
r_out  = 3.0;    // outer corner radius
lid_edge = 0.8;  // bezel face edge chamfer
ear_skin = 1.4;  // wall skin left outside a button/USB clearance channel.
                 // At the ears this is the paddle stock: beam thickness =
                 // ear_skin − pad_recess. At the chin it is the bridge band
                 // over the USB slot — 1.4 prints solid (print 2's breakout
                 // was the relief ring thinning it to 0.4, not the band)

/* [Snap fit] — lid skirt into the bezel walls. Shallow in both builds (the
   edge zone behind the PCB is busy), THIN in the headers build to stay
   outboard of the pin rows. Print 2 asked for a SATISFYING click and a
   rattle-free seat, so: the nubs are ASYMMETRIC (a gentle ramp on the
   approach side, a near-square catch face toward the plate — easy on,
   positive click, firm retention), engagement is deeper (snap_proud 0.7),
   and the skirt runs at its own snug clearance (skirt_clear) instead of
   the general press tolerance. */
snap_w = 3.2; snap_h = 1.6; snap_depth = 1.4; snap_proud = 0.7;
skirt_clear = 0.05;  // skirt-to-cavity clearance per side — snug on purpose:
                     // the skirt IS the seat, the snap only keeps it there
nub_y0 = 3.6;    // nub pair centers (±Y) on the ±X walls — the same
                 // THREE-way squeeze as ever: clear of the button ears
                 // (now WIDER — the paddle recess spans ~8.8 mm, which
                 // evicted the nubs from 5.0), inboard of the corner
                 // posts, and short of the USB-end brass pillars, whose
                 // flanks rise right behind the wall at
                 // y ±(board_l/2 − hole_iy_usb). The asserts below hold
                 // all three sides.

/* [Quality] */
$fa = 3; $fs = 0.4;

// ----------------------------------------------------------------------------
//  Derived.  X = short axis, Y = long axis (portrait), Z = toward the glass.
// ----------------------------------------------------------------------------
// pillars_in: the brass corner pillars are on the board (as-shipped and
// headers builds alike) — the bosses land on their tops and the skirt is
// notched around them.
pillars_in = (headers != "none");
// The pillars build deepens only as far as the pillars demand: brass_h plus
// the measured working gap the boss spans (stand_gap). The male build
// swallows header base + pins instead.
stack_eff = (headers == "male")    ? max(back_stack, hdr_drop)
          : (headers == "pillars") ? max(back_stack, brass_h + stand_gap)
          :                          back_stack;

xc = board_w + 2*tol_slide;   yc = board_l + 2*tol_slide;   // board cavity
xo = xc + 2*wall;             yo = yc + 2*wall;             // outer shell
cav_d = lcd_rise + pcb_t + stack_eff;            // glass ledge → lid inner
bez_h = face_t + cav_d;                          // bezel wall height
r_in  = max(0.6, r_out - wall);

z_pcb_front = face_t + lcd_rise;
z_pcb_back  = z_pcb_front + pcb_t;
// the opening centers on the SHELL (nominal 3.26, resting on the PCB back),
// not on the opening height — tightening usb_h never shifts it off the port
z_usb = z_pcb_back + 3.26/2 + usb_dz;
z_btn = z_pcb_back + btn_dz;

btn_y     = -board_l/2 + btn_up;         // button center (Y)
btn_reach = btn_proud + tol_slide;
btn_body_reach = btn_body_p + tol_slide;
usb_reach = usb_proud + tol_slide;
usb_slide = usb_w + 0.1;                 // insertion slot — a hair looser than
                                         // the stadium (0.13/side over the
                                         // shell), and flush-walled with it
z_slot0   = z_usb - usb_h/2;             // the slot starts AT the stadium's
                                         // floor and runs up to the rim: the
                                         // shell drops down it to its seat,
                                         // and the wall below the port stays
                                         // FULL thickness (print 2's chin was
                                         // needlessly hollowed to the face)
ear_bump  = max(0, btn_reach + ear_skin - wall);
chin_bump = max(0, usb_reach + ear_skin - wall);
ear_w  = pad_l + pad_slot + 2.4;         // the ear wraps the paddle recess
                                         // with 1.2 mm of wall each side
chin_w = usb_w + 4;

// paddle frame (per ±X ear): the beam hinges at its USB-end face and its
// free end carries the press dot. All in case coordinates.
pad_beam  = ear_skin - pad_recess;               // flexing skin thickness
pad_y0    = btn_y - pad_l/2;                     // hinge-end edge
pad_rec_y = pad_l + pad_slot;                    // recess span (slot at the
pad_rec_cy = btn_y + pad_slot/2;                 //  free end only)
// height fits the build: full pad_h where the case is deep enough, derived
// down where it is not (the stripped build is 1 mm shallower), always
// leaving the 1.2 rim band above the recess by construction
pad_h_eff = min(pad_h, 2*(bez_h - 1.2 - pad_slot - z_btn));
pad_rec_z = pad_h_eff + 2*pad_slot;
x_skin_in = xc/2 + btn_reach;                    // beam's inner face
free_gap  = 2*tol_slide - pad_boss_l;            // boss tip → actuator tip

// window lip: (module − active area)/2 per side; the LAND is what is left of
// it outside the relieved band — that ring is the panel's only contact
lip_l = (lcm_l - aa_l)/2;
lip_w = (lcm_w - aa_w)/2;
land_w_x = lip_w - glass_relief_w;
land_w_y = lip_l - glass_relief_w;

// snap skirt (see [Snap fit])
skirt_wall = (headers == "male") ? 1.0 : 1.6;
skirt_dep  = snap_depth + snap_h/2 + 0.4;
skirt_x = xc - 2*skirt_clear;   skirt_y = yc - 2*skirt_clear;

// press bosses: cut to the EXACT stack (no preload — rule 3; "just lightly
// press" is exact contact, not crush). With pillars on the board they span
// the measured stand_gap and land on the flat brass pillar tops — print 2
// proved the old guess left them 2.0 short and the board floating.
// Stripped, they run to the bare PCB corners over the same M2 positions.
stand_d   = pillars_in ? 4.6 : 4.2;
stand_len = stack_eff - (pillars_in ? brass_h : 0);
// the four positions in CASE frame (USB end = −Y); the lid module flips Y
stand_case = [[ board_w/2 - hole_ix_usb, -(board_l/2 - hole_iy_usb)],
              [-(board_w/2 - hole_ix_usb), -(board_l/2 - hole_iy_usb)],
              [ board_w/2 - hole_ix_far,  board_l/2 - hole_iy_far ],
              [-(board_w/2 - hole_ix_far), board_l/2 - hole_iy_far ]];

function nub_ys() = [-nub_y0, nub_y0];

// the wordmark's estimated width (see mark_adv) — the assert below is the
// only thing standing between a taste change and type running off the part
mark_w = len(mark_word) * mark_adv * mark_word_h;

// ── The back face's two features, placed in CASE coordinates ───────────────
// Only "both" honors the offsets; a single feature centers itself, so
// switching modes never leaves the one thing on the plate sitting off-center.
lid_has_mark = (lid_back == "both" || lid_back == "mark");
lid_has_kh   = (lid_back == "both" || lid_back == "keyhole");
mark_y = (lid_back == "both") ? mark_cy : 0;
kh_y   = (lid_back == "both") ? kh_cy   : 0;
// BOTH openings are a nominal screw dimension PLUS this catalog's
// printed-hole clearance — the same `2*tol_hole` (0.3 per side) every screw
// hole in this directory uses, and which this file declared but never spent
// until the review caught it. A cutter drawn at the nominal size is a hole
// that binds once the nozzle has had its say.
kh_egg_w   = kh_head_d  + 2*tol_hole;   // the base: where the head enters
kh_crown_w = kh_shank_d + 2*tol_hole;   // the crown: where the shank grips
// The egg's tip ratio is DERIVED, not styled: it is exactly the crown over
// the base, so the egg's crown IS the keyhole's slot. Overriding the
// library's brand tip is deliberate and this is the reason (its header asks
// that an override say why).
kh_tip = kh_crown_w / kh_egg_w;
// egg2d spans y ∈ [−w/2, l − w/2]; this is the shift that centers it
kh_shift = (kh_egg_l - kh_egg_w)/2;
kh_lo = kh_y - kh_egg_l/2;   kh_hi = kh_y + kh_egg_l/2;
// the wordmark's visual band — cap height plus a descender's worth
mark_lo = mark_y - mark_word_h*0.75;   mark_hi = mark_y + mark_word_h*0.75;
// the skirt ring stands inside these; a THROUGH cut may not reach it
skirt_free_y = skirt_y/2 - skirt_wall;
skirt_free_x = skirt_x/2 - skirt_wall;

// ── The light band's volume, derived, never composed ────────────────────────
// The S3 stick's hard lesson verbatim: a band slot parked by wall arithmetic
// left a 0.65 mm black curtain between the LED and the white — a light pipe
// with no light in it, invisible in every render. So the U is built from the
// SAME outline and cavity the bezel uses, overreaching 1 mm past the outer
// face and 1 mm into the cavity — a curtain is impossible by construction.
seam_z0 = face_t + seam_dz;                // glass front is at z = face_t
side_lo = btn_y + ear_w/2 + 1.2;           // the U's legs start above the ears

// wall vents: behind the band, in front of the snap window band
vent_z0 = seam_z0 + seam_h + 0.8;
vent_z1 = bez_h - (snap_depth + snap_h/2) - 0.8;
vent_dy = opt_btn ? (btn_y + ear_w/2) + (vent_n-1)*vent_pitch/2 + vent_w/2 + 1.2 : 0;

// ── Asserts — the gates, not the documentation ──────────────────────────────
assert(aa_l < lcm_l && aa_w < lcm_w, "active area must be inside the module outline");
assert(lip_w >= 0.8, "short-side lip < 0.8 mm won't retain the glass — check aa_w/lcm_w");
assert(land_w_x >= 0.4 && land_w_y >= 0.4,
       str("glass land is ", land_w_x, " / ", land_w_y, " mm — too narrow to ",
           "carry the panel. Shrink glass_relief_w; the relief must never eat ",
           "the land that locates the glass."));
assert(lcm_l <= yc && lcm_w <= xc, "LCD module larger than the board cavity — check dims");
assert(z_usb - usb_h/2 >= face_t - 0.01, "USB opening cuts into the bezel face — check usb_h/usb_dz");
assert(z_usb + usb_h/2 <= bez_h - 0.8,
       "no printable bridge left between the USB opening and the rear rim — raise back_stack/hdr_drop or lower usb_dz");
assert(z_usb + usb_h/2 + usb_cham <= bez_h - 0.5,
       str("the USB rim chamfer (top at ", z_usb + usb_h/2 + usb_cham,
           ") breaks out through the rim (", bez_h, ") — trim usb_cham"));
assert(z_usb - usb_h/2 - usb_cham >= face_t - 0.01,
       "the USB rim chamfer undercuts the bezel face — trim usb_cham");
assert(usb_cham <= ear_skin - 0.6,
       str("usb_cham ", usb_cham, " leaves under 0.6 mm of skin at the chamfer's ",
           "deepest ring over the slot — the exact thin-floor breakout print 2 ",
           "showed. Keep usb_cham <= ear_skin - 0.6."));
assert(skirt_dep <= stack_eff + 0.01, "skirt_dep > component clearance — the skirt would drive into the PCB");
assert(skirt_dep >= snap_depth + snap_h/2, "skirt too short to carry the snap nub");
assert(stand_len >= 0.6, "press bosses shorter than 0.6 — brass_h nearly fills the cavity; check hdr_drop/brass_h");
assert(headers != "male" || hdr_drop >= brass_h + 0.5,
       "corner pillars taller than the cavity below the PCB — check brass_h/hdr_drop");
assert(!pillars_in || brass_h > 0.5,
       "a pillars build with brass_h ~0 makes no sense — measure the pillars or set headers=\"none\"");
// The pillars are real cylinders standing on the board, and two case features
// live right beside them. Both of these fired for real during the assembled-
// fit checks — they are not hypothetical.
assert(!pillars_in || board_w/2 - hole_ix_usb + brass_d/2 <= xc/2 - 0.15,
       str("a Ø", brass_d, " pillar at hole_ix_usb=", hole_ix_usb,
           " reaches x=", board_w/2 - hole_ix_usb + brass_d/2,
           ", into the bezel wall at ", xc/2, " — re-measure hole_ix_usb ",
           "and brass_d (the 1.27 reading of the drawing does this)"));
assert(!pillars_in || nub_y0 + snap_w/2 - 0.95 <=
       (board_l/2 - hole_iy_usb) - brass_d/2 - 0.4,
       str("a snap nub reaches y=", nub_y0 + snap_w/2 - 0.95,
           ", into the USB-end pillar's flank at y=",
           (board_l/2 - hole_iy_usb) - brass_d/2,
           " — pull nub_y0 in or slim snap_w"));
assert(headers != "male" || hdr_drop > back_stack, "headers=male but hdr_drop is shallower than the stripped-board clearance");
assert(headers != "male" || tol_slide + hdr_inset - 0.35 >= skirt_clear + skirt_wall + 0.25,
       "skirt would sit in the header pin row — thin skirt_wall or re-measure hdr_inset");
// the paddle: a real mechanism, so real gates
assert(!opt_btn || min([for (y = nub_ys()) abs(y - btn_y)]) >= pad_l/2 + pad_slot + snap_w/2 + 1.0,
       "a snap window overlaps the paddle recess — shift nub_y0/btn_up or shorten pad_l");
assert(!opt_btn || btn_y - ear_w/2 >= -(yo/2 - r_out) + 0.4,
       "the ear bulge runs into the USB-end outer corner — check btn_up/ear_w/pad_l");
assert(!opt_btn || (pad_beam >= 0.6 && pad_beam <= 1.0),
       str("paddle beam is ", pad_beam, " mm — under 0.6 won't survive handling, ",
           "over 1.0 won't flex to the click. Tune ear_skin/pad_recess."));
assert(!opt_btn || (free_gap >= 0.08 && free_gap <= 0.3),
       str("boss free gap is ", free_gap, " mm — under 0.08 preloads the switch ",
           "(a case that holds its own button down), over 0.3 wastes travel. ",
           "Tune pad_boss_l against btn_proud."));
assert(!opt_btn || pad_boss_d <= btn_ch_w - 0.6,
       "the press boss won't clear the actuator channel — shrink pad_boss_d");
assert(!opt_btn || pad_h_eff >= pad_boss_d + 1.4,
       str("this build's cavity leaves the paddle only ", pad_h_eff,
           " mm tall — not enough to wrap the Ø", pad_boss_d, " boss. ",
           "Shrink pad_boss_d or deepen the build."));
assert(!opt_btn || z_btn - pad_rec_z/2 >= face_t + 0.8,
       "the paddle recess undercuts the bezel face — shrink pad_h or check btn_dz");
assert(nub_y0 + snap_w/2 <= yc/2 - r_out, "snap windows run into the corner posts — pull nub_y0 in");
// the band: placed on the panel, open to the cavity, whole enough to matter
assert(!light_seam || seam_dz >= 0,
       "the light band starts in FRONT of the glass — it would undercut the face");
assert(!light_seam || seam_dz + seam_h <= lcd_rise + 0.25,
       str("the light band spans ", seam_dz, " .. ", seam_dz + seam_h,
           " mm behind the glass, past the PCB front at ", lcd_rise,
           " — it would collide with the board"));
assert(!light_seam || yc/2 - r_out - side_lo >= 8,
       str("the U's leg run before the corner is only ", yc/2 - r_out - side_lo,
           " mm — check btn_up/ear_w"));
assert(!light_seam || bez_h - snap_depth - snap_h/2 >= seam_z0 + seam_h + 0.6,
       "the snap window band overlaps the light band — deepen the case");
assert(!opt_vent || vent_z1 - vent_z0 >= 1.5, "no room for wall vents behind the band — set opt_vent=false");
assert(!opt_vent || vent_dy + (vent_n-1)*vent_pitch/2 + vent_w/2 <= yc/2 - 0.8,
       "vent row overruns the side wall — fewer slots (vent_n) or tighter pitch");
// ── The back face: two features, and everything they can run into ─────────
assert(lid_back == "both" || lid_back == "mark" || lid_back == "keyhole",
       str("lid_back is \"", lid_back, "\" — it must be \"both\", \"mark\" ",
           "or \"keyhole\"."));
// the two of them, against each other
assert(lid_back != "both" || kh_lo - mark_hi >= 2.0,
       str("the hanger (down to y=", kh_lo, ") and the wordmark (up to y=",
           mark_hi, ") are closer than 2 mm — separate kh_cy and mark_cy, ",
           "or shrink kh_egg_l/mark_word_h."));
// the hanger is a THROUGH cut, so unlike the deboss it can hit real structure
assert(!lid_has_kh || kh_hi <= skirt_free_y - 0.8,
       str("the hanger reaches y=", kh_hi, ", into the lid skirt's band at ",
           skirt_free_y, " — it would cut a notch out of the snap wall. ",
           "Lower kh_cy or shorten kh_egg_l."));
assert(!lid_has_kh || -kh_lo <= skirt_free_y - 0.8,
       "the hanger reaches the far skirt band — raise kh_cy or shorten kh_egg_l");
assert(!lid_has_kh || kh_egg_w/2 <= skirt_free_x - 0.8,
       str("the hanger is ", kh_egg_w, " wide and the skirt walls stand at ±",
           skirt_free_x, " — narrow kh_egg_w."));
assert(!lid_has_kh || len([for (p = stand_case)
           if (abs(p[0]) - stand_d/2 - 0.6 < kh_egg_w/2
               && p[1] + stand_d/2 + 0.6 > kh_lo
               && p[1] - stand_d/2 - 0.6 < kh_hi) 1]) == 0,
       str("the hanger's footprint (y ", kh_lo, "..", kh_hi, ", x ±",
           kh_egg_w/2, ") overlaps a press boss — the through cut would open ",
           "a hole in the boss that holds the board. Move kh_cy."));
// the egg has to behave like a keyhole: swallow the head, then refuse it
assert(!lid_has_kh || kh_crown_w < kh_head_d,
       str("the crown is ", kh_crown_w, " mm across and the head is ",
           kh_head_d, " — a crown the head fits through captures nothing ",
           "and the case falls off the wall. Check kh_shank_d/kh_head_d."));
assert(!lid_has_kh || (kh_tip > 0 && kh_tip < 1),
       str("derived egg tip is ", kh_tip, " — kh_shank_d must sit strictly ",
           "between 0 and kh_egg_w for the crown to be a slot"));
assert(!lid_has_mark || mark_w <= xo - 4.0,
       str("the wordmark is ", mark_w, " mm wide on a ", xo, " mm lid — it ",
           "would hang off the sides (2 mm margin required). Shrink ",
           "mark_word_h, or shorten mark_word."));
assert(!lid_has_mark || mark_word_h <= yo - 6.0,
       "the wordmark is taller than the lid — shrink mark_word_h");
assert(!lid_has_mark || (mark_depth >= 0.3 && mark_depth <= back_t - 0.8),
       str("mark_depth ", mark_depth, " must sit between 0.3 (a deboss worth ",
           "having) and ", back_t - 0.8, " (leaving plate under the letters)"));
echo(str("Canary C3-LCD-1.47 (headers=", headers, ") v0.2-dev — outer ",
         xo, " x ", yo, " x ", bez_h + back_t, " mm, window ", aa_w, " x ", aa_l,
         " (land X ", land_w_x, " / Y ", land_w_y, "), band ", seam_h,
         " mm white starting ", seam_dz, " mm behind the glass",
         "  (IN DEVELOPMENT — MEASURE)"));

// The wordmark as it must be DRAWN in the lid's own frame.
//
// PRE-FLIPPED IN Y, and that is the whole subtlety. The lid is authored in
// its own frame and reaches the case through
//     rotate([180, 0, 0])  →  (x, y, z) ↦ (x, −y, −z)
// so every glyph's Y is inverted on the way to the assembled back face.
// Type laid out the normal way therefore lands upside down on the surface a
// person actually reads. Pre-flipping Y here cancels the assembly's flip
// exactly, and it happens ONCE so the recess, the inlay and the preview all
// get the same handedness with no caller left to remember.
//
// ⚠️ It is a Y mirror, not an X mirror. The X mirror is the intuitive
// guess — the outer face is the one you view from below in this frame —
// and it is wrong: it composes with the assembly's Y flip into a clean 180°
// rotation, which renders as upside-down type that still LOOKS like a
// legitimate wordmark at a glance. That is exactly what the first cut did.
// The only check that catches this is rendering the letters in their
// assembled orientation under a plain top view and READING them.
module lid_mark2d() {
    mirror([0, 1, 0]) mark_wordmark(mark_word_h, mark_word);
}

// The hanger, in the lid's frame — the house egg, centered on the origin and
// pre-flipped by the same rule as the wordmark, so after the assembly's Y
// inversion its CROWN points up on the hung case and its wide BASE is the
// low entry. egg2d draws crown-up spanning y ∈ [−w/2, l − w/2]; kh_shift
// re-centers that span so kh_cy means what it says.
module kh_egg2d() {
    mirror([0, 1, 0])
        translate([0, -kh_shift]) egg2d(kh_egg_l, kh_egg_w, kh_tip);
}

module rrect2d(x, y, r) { offset(r = r) offset(r = -r) square([x, y], center = true); }
// stadium: full-round ends, the USB-C shell's own profile
module stadium2d(w, h) { rrect2d(w, h, h/2 - 0.05); }

// bezel outline: body + ear/chin bulges (the bulges belong to the bezel; the
// lid plate keeps the plain outline)
module shell_outline2d() {
    rrect2d(xo, yo, r_out);
    if (opt_btn && ear_bump > 0) for (sx = [1, -1])
        translate([sx*(xo/2 + ear_bump/2 - 1.2), btn_y])
            rrect2d(ear_bump + 2.4, ear_w, 1.0);
    if (chin_bump > 0)
        translate([usb_dx, -(yo/2 + chin_bump/2 - 1.2)])
            rrect2d(chin_w, chin_bump + 2.4, 1.0);
}

// board cavity + full-depth button overhang channels (rule 1). The USB
// slide slot is NOT here anymore: it is a 3D cut in the bezel, z-limited to
// start at the stadium's floor, so the chin below the port stays solid.
module cavity2d() {
    rrect2d(xc, yc, r_in);
    if (opt_btn) for (sx = [1, -1]) {
        translate([sx*xc/2, btn_y]) square([2*btn_reach, btn_ch_w], center = true);       // actuator
        translate([sx*xc/2, btn_y]) square([2*btn_body_reach, btn_body_w], center = true); // metal body
    }
}

// the USB insertion slot: from the stadium's floor up through the rim, the
// shell's path down to its seat during the glass-first drop
module usb_slot() {
    translate([usb_dx, -yc/2, (z_slot0 + bez_h + 0.2)/2])
        cube([usb_slide, 2*usb_reach, bez_h + 0.2 - z_slot0], center = true);
}

// the wall material as stock — what the band is allowed to occupy. Built
// from the same outline and cavity the bezel uses, so band and slot can
// never drift apart.
module wall_stock() {
    difference() {
        linear_extrude(bez_h) shell_outline2d();
        translate([0, 0, face_t]) linear_extrude(cav_d + 0.2) cavity2d();
    }
}

// the seam volume: ONE U — up both long walls, around both far corners,
// across the far short wall. A ring slab (outline grown 1 mm minus cavity
// shrunk 1 mm — the overreach on both faces) masked to y ≥ side_lo, so the
// legs, the corner bends, and the top run are a single connected volume.
// `shrink` insets the mating faces (seam top/bottom, the two leg ends) so
// one geometry serves as the CUT (0) and the BAND that fills it
// (band_clear); the radial faces come from wall_stock exactly.
module seam_solid(shrink = 0) {
    translate([0, 0, seam_z0 + shrink])
        linear_extrude(seam_h - 2*shrink)
            intersection() {
                difference() {
                    offset(delta =  1.0) shell_outline2d();
                    offset(delta = -1.0) cavity2d();
                }
                translate([0, (side_lo + shrink + yo)/2])
                    square([xo + 2*ear_bump + 4, yo - (side_lo + shrink)], center = true);
            }
}

module bezel_light_seam() { seam_solid(0); }

// the white band: exactly the wall material the seam removed, minus
// clearance — intersected with the wall stock so the U is precisely as
// deep as the wall it fills, corners included. Full depth, no notches:
// the pipe's back face is the cavity wall, white all the way to the LED's
// side of it, and white all the way around the bends.
module light_band() {
    if (light_seam)
        intersection() { seam_solid(band_clear); wall_stock(); }
}

// ----------------------------------------------------------------------------
//  BEZEL — black body, prints face-down (z0 = outer face)
// ----------------------------------------------------------------------------
// paddle footprints in the ear's Y-Z plane (drawn at the origin, translated
// into place by the caller): the recess pocket, the paddle itself, and the
// flex slot — the slot is their difference, so paddle and pocket can never
// disagree about where the slot is. The hinge is the paddle's −Y end face:
// there the recess edge meets the paddle edge and no slot exists.
module pad_pocket2d() {
    translate([pad_rec_cy - btn_y, 0]) rrect2d(pad_rec_y, pad_rec_z, 1.2);
}
module pad_face2d() { rrect2d(pad_l, pad_h_eff, 1.0); }

module bezel() {
  union() {
    difference() {
        linear_extrude(bez_h) shell_outline2d();
        // active-area window through the face
        translate([0, 0, -0.1]) linear_extrude(face_t + 0.2) rrect2d(aa_w, aa_l, 1.5);
        // face edge chamfer
        if (lid_edge > 0)
            translate([0, 0, -0.01]) linear_extrude(lid_edge + 0.01)
                difference() {
                    offset(delta = 0.1) shell_outline2d();
                    offset(delta = -lid_edge) shell_outline2d();
                }
        // glass relief: stand the face off the innermost lip band, so the
        // window's rim never touches the panel (rule 3)
        translate([0, 0, face_t - glass_relief]) linear_extrude(glass_relief + 0.02)
            rrect2d(aa_w + 2*glass_relief_w, aa_l + 2*glass_relief_w, 1.5 + glass_relief_w);
        // board cavity + overhang channels
        translate([0, 0, face_t]) linear_extrude(cav_d + 0.2) cavity2d();
        // USB insertion slot (stadium floor → rim; solid wall below)
        usb_slot();
        // USB-C stadium through the bottom wall + chin, tight on the shell
        translate([usb_dx, -yo/2, z_usb]) rotate([90, 0, 0])
            linear_extrude(2*(wall + chin_bump + 1), center = true) stadium2d(usb_w, usb_h);
        // …and the rim chamfer: the plug's lead-in, no thin floor anywhere
        hull() {
            translate([usb_dx, -(yo/2 + chin_bump) + 0.005, z_usb]) rotate([90, 0, 0])
                linear_extrude(0.01) stadium2d(usb_w + 2*usb_cham, usb_h + 2*usb_cham);
            translate([usb_dx, -(yo/2 + chin_bump) + usb_cham + 0.005, z_usb]) rotate([90, 0, 0])
                linear_extrude(0.01) stadium2d(usb_w, usb_h);
        }
        // BOOT / RST paddles, three cuts each (see [Buttons]):
        if (opt_btn) for (sx = [1, -1]) {
            // 1 — the outer recess pocket the pad sits in (the squeeze guard)
            translate([sx*(xo/2 + ear_bump - pad_recess), btn_y, z_btn])
                rotate([0, sx*90, 0]) linear_extrude(pad_recess + 1)
                    rotate(90) pad_pocket2d();
            // 2 — the flex slot: pocket minus paddle, through the beam
            translate([sx*(x_skin_in - 0.1), btn_y, z_btn])
                rotate([0, sx*90, 0]) linear_extrude(ear_skin)
                    rotate(90) difference() { pad_pocket2d(); pad_face2d(); }
            // 3 — the flex chamber behind the whole pocket footprint, so the
            //     pad swings free and the slot is truly open
            translate([sx*(x_skin_in - 1.0), btn_y, z_btn])
                rotate([0, sx*90, 0]) linear_extrude(1.01)
                    rotate(90) pad_pocket2d();
        }
        // heat-escape slots, BEHIND the light band
        if (opt_vent) for (sx = [1, -1], i = [0:vent_n-1])
            translate([sx*xo/2, vent_dy - (vent_n-1)*vent_pitch/2 + i*vent_pitch, (vent_z0 + vent_z1)/2])
                cube([wall*3, vent_w, vent_z1 - vent_z0], center = true);
        // snap windows in the side walls
        for (sx = [1, -1], yc0 = nub_ys())
            translate([sx*xo/2, yc0, bez_h - snap_depth])
                cube([wall*3, snap_w, snap_h], center = true);
        // the light seam
        if (light_seam) bezel_light_seam();
    }
    // the paddle's working ends, added back onto the flexing beam:
    if (opt_btn) for (sx = [1, -1]) {
        // press boss on the pad's back — lands square on the actuator
        translate([sx*(x_skin_in - pad_boss_l), btn_y, z_btn])
            rotate([0, sx*90, 0]) cylinder(d = pad_boss_d, h = pad_boss_l + 0.01);
        // press dot on the pad's face at the free end: says "here", stays
        // below the ear surface (dot 0.3 < pad_recess 0.6)
        translate([sx*(xo/2 + ear_bump - pad_recess - 0.01), btn_y + pad_l/2 - 1.8, z_btn])
            rotate([0, sx*90, 0]) cylinder(d1 = 2.6, d2 = 1.6, h = 0.31);
    }
  }
}

// ----------------------------------------------------------------------------
//  LID — yellow, prints outer-face-down (z0 = outer back face).
//  Authored in its own frame; the assembled mapping is
//      case = translate([0,0,bez_h+back_t]) · rotate([180,0,0]) · lid
//  i.e. case (x, y) = lid (x, −y). KEYED — see the header: the skirt's USB
//  relief exists at one end only, so the wrong orientation cannot close.
// ----------------------------------------------------------------------------
module lid() {
    difference() {
        union() {
            linear_extrude(back_t) rrect2d(xo, yo, r_out);                 // plate
            translate([0, 0, back_t - 0.01]) linear_extrude(skirt_dep)     // skirt
                difference() {
                    rrect2d(skirt_x, skirt_y, r_in);
                    rrect2d(skirt_x - 2*skirt_wall, skirt_y - 2*skirt_wall, max(0.4, r_in - skirt_wall));
                    // USB relief — ONE end only (lid +Y = case −Y): the key.
                    // usb_w + 4, not + 2: the extra millimeter per side makes
                    // this cut MEET the pillar notches beside it. At + 2 a
                    // 0.05 mm whisker of skirt survived between the relief's
                    // edge and the notch circle's reach — one triangle wide,
                    // full skirt height, found by slicing the STL, not by
                    // admesh (it was watertight and connected).
                    translate([usb_dx, skirt_y/2]) square([usb_w + 4, 3*skirt_wall], center = true);
                    // button-body reliefs at the real button positions
                    if (opt_btn) for (sx = [1, -1])
                        translate([sx*skirt_x/2, -btn_y]) square([3*skirt_wall, btn_body_w + 2], center = true);
                    // pillar notches, one per pillar position. With the
                    // MEASURED 3.0 pillars the tops stop ~0.2 below the
                    // skirt band, so the notches are no longer strictly
                    // needed — they stay because they cost nothing and a
                    // taller pillar batch (the guessed 5.0 was not absurd)
                    // would land in the ring exactly where they are. In the
                    // deeper male build the pillars stop well below the
                    // skirt and the notches are merely harmless.
                    //
                    // HULLED OUTWARD, not a bare circle. A circle tangent to
                    // the ring leaves a crescent of skirt standing outboard
                    // of it — a ~0.3 mm blade, full skirt height, at every
                    // corner. Watertight, one part, invisible in admesh, and
                    // it printed as four fragile wisps in the preview. The
                    // hull sweeps the notch radially out past the ring so
                    // the corner cut runs clean through.
                    if (pillars_in) for (p = stand_case)
                        let (n = norm([p[0], p[1]]), s = 1 + 6/n)
                        hull() {
                            translate([p[0], -p[1]]) circle(d = stand_d + 2.2);
                            translate([p[0]*s, -p[1]*s]) circle(d = stand_d + 2.2);
                        }
                }
            // press bosses over the four M2 pillar positions (lid y = −case y).
            // TRIMMED to the cavity footprint: the USB-end pillar pair sits
            // only hole_ix_usb (1.27) off the board edge, so a full round
            // boss there would bury itself 0.6 mm into the bezel wall — the
            // assembled-fit intersection check is what caught it. The flat
            // costs nothing: the trimmed face still covers the pillar top.
            intersection() {
                union() for (p = stand_case)
                    translate([p[0], -p[1], back_t - 0.01])
                        cylinder(d = stand_d, h = stand_len + 0.01);
                linear_extrude(back_t + stand_len + 0.1)
                    rrect2d(xc - 2*tol_press, yc - 2*tol_press, r_in);
            }
            // snap nubs on the ±X skirt faces — ASYMMETRIC: the ridge sits
            // toward the skirt TIP, so the face the bezel rim meets on the
            // way in is a long gentle ramp, and the face toward the plate is
            // near-square. Easy on, a real CLICK as the nub drops into its
            // window, and a catch face that resists working loose.
            for (sx = [1, -1], yc0 = nub_ys())
                translate([sx*(skirt_x/2 - 0.3), yc0, back_t + snap_depth]) hull() {
                    for (dy = [-snap_w/2 + 1.0, snap_w/2 - 1.0])
                        translate([0, dy, 0]) cube([0.6, 0.1, snap_h - 0.4], center = true);
                    translate([sx*(snap_proud + 0.3), 0, -(snap_h/2 - 0.5)])
                        cube([0.1, 0.1, 0.2], center = true);
                }
        }
        // (no grille: print 2's verdict — tiny back holes read as confusion,
        //  not function. The lid is a clean plate; the walls carry the vents.)
        // the wordmark, debossed into the OUTER face (lid z = 0). The inlay
        // that fills it is the SAME 2D shape (lid_mark2d), so recess and
        // fill cannot drift — the band's doctrine, applied to type.
        if (lid_has_mark)
            translate([mark_dx, -mark_y, -0.01])
                linear_extrude(mark_depth + 0.01) lid_mark2d();
        // the egg hanger, cut clean through: offer the screw head into the
        // wide base, let the case drop, and the flank taper carries it up to
        // the crown where the shank is gripped and the head cannot return.
        if (lid_has_kh)
            translate([0, -kh_y, -0.1]) linear_extrude(back_t + 0.2) kh_egg2d();
    }
}

// ----------------------------------------------------------------------------
//  MARK — the wordmark inlay, black. Exactly the volume the lid's deboss
//  removed: same 2D shape, same depth, no clearance. Zero clearance is the
//  co-print doctrine this file already applies to the light band — the AMS
//  fuses filaments that meet, and a shrunk inlay just asks it to bridge a
//  gap that should not exist. Authored in the LID's frame so the two
//  volumes are registered by construction and gen_3mf.py can hand them over
//  without moving either.
// ----------------------------------------------------------------------------
module lid_mark() {
    if (lid_has_mark)
        translate([mark_dx, -mark_y, 0])
            linear_extrude(mark_depth) lid_mark2d();
}

// ----------------------------------------------------------------------------
//  RENDER
// ----------------------------------------------------------------------------
module lid_assembled() {
    translate([0, 0, bez_h + back_t]) rotate([180, 0, 0]) lid();
}

// ⚠️ Do not judge the palette from "all"/"exploded" — the band sits exactly
// in the pocket it was cut from, and OpenCSG merges and repaints composited
// products (the same caveat as the S3 stick and the 7" frame). Silhouette
// and fit only; part="palette" is the view that answers "what do the three
// spools look like".
module bezel_assembly() {
    color("#f5c518") bezel();
    if (light_seam) color("#f2f2f2") light_band();
}

// the lid as it reads: yellow plate, black letters sitting in their deboss
module lid_branded() {
    color("#f5c518") lid();
    color("#1a1a1a") lid_mark();
}

module palette_row() {
    color("#f5c518") bezel();
    translate([xo + 8, 0, 0]) color("#f2f2f2") light_band();
    translate([2*(xo + 8), 0, 0]) lid_branded();
}

if      (part == "bezel") bezel();
else if (part == "lid")   lid();
else if (part == "light") light_band();
else if (part == "mark")  lid_mark();
else if (part == "palette") palette_row();
else if (part == "exploded") {
    bezel_assembly();
    translate([0, 0, 16]) color("#f5c518") lid_assembled();
    // The wordmark floats ABOVE its deboss rather than sitting in it, and
    // that is what makes this preview readable. Committed previews render
    // through CGAL, which merges and repaints composited products — so an
    // inlay lying flush in its own pocket disappears into the lid and the
    // branding cannot be inspected at all. Lifted, the letters read as
    // geometry and the empty deboss reads under them, in a single-color
    // render, exactly like every other preview in this catalog. An inlay
    // shown apart from its recess is what "exploded" means anyway.
    translate([0, 0, 22]) color("#1a1a1a")
        translate([0, 0, bez_h + back_t]) rotate([180, 0, 0]) lid_mark();
}
else {  // "all": both prints side by side, as they come off the plate
    bezel_assembly();
    translate([xo + 10, 0, 0]) lid_branded();
}
