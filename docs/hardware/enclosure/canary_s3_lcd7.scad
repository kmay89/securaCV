// ============================================================================
//  Canary — 7" TOUCH DASHBOARD CASE  ⚠️ IN DEVELOPMENT (v0.9-dev)
// @env env="indoor; runs hot → print in PETG/ASA"
//  Housing for the Waveshare ESP32-S3-Touch-LCD-7 (7" 800x480 IPS capacitive
//  touch, ESP32-S3, CAN/RS485/battery). The big-panel "wall dashboard" — the
//  household event timeline on a glass slab, vs the Dash 4.3" and the C6 pocket
//  displays.
//
//    bezel — front frame; the bonded touch-glass slab (192.96 x 110.76) drops
//            in face-first, the lip overlaps its border and the active-area
//            window (154.88 x 86.72) is what you see. Prints FACE-DOWN.
//    back  — deep vented rear tray; the PCB rests on molded standoffs, and the
//            bezel screws to four outboard M3 corner lobes. A LARGE convection
//            grille (this panel's backlight + ESP32-S3 + regulators run hot —
//            heat MUST escape) plus a bottom-edge connector channel and side
//            openings for USB-C / UART / CAN / RS485 / battery.
//    gauge — TWO SMALL CORNER BLOCKS (a bezel corner + its tray corner). Print
//            these FIRST: ~16.5 g of filament against the case pair's ~158 g
//            (10 %), assembled around the panel's corner with one M3. They prove
//            the four things that make or break the big print — the glass
//            seats in the cavity, the lip lands on the border and clears the
//            active area, the screw threads, and the closed stack height is
//            what the echo says. See bambu_p2s_bringup.md §7.
//    stand — free-standing desk DOCK for the FRAME case (optional, no
//            hardware). Reclined drop-in slot sized to the frame's derived
//            outer depth; the case rests on two tilted seat pads (the cheek
//            tops) with an open well between them, so the bottom-wall intake
//            vents keep their convection path AND the USB power lead that
//            leaves through the frame's bottom cable port has plug headroom,
//            then routes out the back through a desk-level cable channel.
//            Two chamfered CENTERING KEYS on the keyed ribs rise into the
//            case's ±dock_key_dx openings — intake slots in landscape, the
//            side-wall keying slots in portrait — so either way up, the
//            case finds its own center and cannot slide out sideways.
//            PORTRAIT: the keyed ribs across the well seat the 115 mm-wide
//            slab, and the base is deep enough that both orientations pass
//            the tip-over asserts. Vented back fin (its ±|sd_dx| pills keep
//            the back-plate microSD opening reachable while docked), entry
//            flares, foot-chamfered base, rubber-foot recesses, branding
//            front and back. Narrower than the case on purpose (in portrait
//            the cable port faces sideways — the lead drapes beside the
//            dock instead).
//    stand_gauge — one cheek's slice of the dock (~15 % of its filament).
//            Print FIRST: it proves the slot width against your frame print,
//            the recline, the seat height and the lip capture.
//    frame — ONE-PIECE drop-in case, the layout a fitting reference print
//            validated: the slab enters face-first through the front opening,
//            the board hangs on the panel's OWN white M3 standoffs, and
//            4x M3x8 from the back thread into those standoffs — the
//            screws, not a ledge, set the glass glass_guard below the
//            front rim — -0.2 today, i.e. the glass stands 0.2 PROUD of
//            the rim (raise glass_guard to trade that for a recess).
//            BOOT/RESET window in the top wall with debossed labels; gill
//            vents down each side wall; top-wall exhaust; back grille;
//            a microSD access opening through the BACK PLATE covering the
//            socket, the card's slide travel and a fingertip ("SD" deboss);
//            keyhole wall mounts in all FOUR corners (hang on the top pair
//            or all four, slide down — the bottom pair pins the case flat
//            against the wall); an
//            adhesive LEDGE behind the glass matched to the panel's own
//            adhesive strips (10 mm sides, 6 mm button edge, 2 mm over the
//            FPC), each with a 45° back-slope wedge to its wall; a USB
//            PASS-THROUGH centered on the bottom wall sized to pass the power
//            cable's overmold head, with the brand words flanking it as
//            CRISP DEBOSS (v0.6 — the slat-stencil vents' tie bands read as
//            horizontal scan lines on the first print; a deboss needs no
//            ties, so counters stay attached and the letters print clean)
//            while a SHADOW GILL intake row tucked against the back plate
//            keeps the bottom edge breathing. Branding stays debossed on
//            the back plate. PRINTS BACK-PLATE-DOWN — the orientation the
//            ledge wedges self-support in.
//            v0.6 hardens mounting and finish: every keyhole bears on a
//            DOUBLER PAD (back_t + khm_pad_t of material under the screw
//            head, lead-in chamfer at the mouth), the M3 bosses and the
//            tray's PCB bosses carry 45° root fillets, two outlined
//            ADHESIVE RAILS on the back plate stay smooth and uninterrupted
//            for 15.9 x 70 mm interlocking picture-hanging strip pairs
//            (wall mounting with no screws — gated by the frame_adh_rail
//            fit check; the echo reports the grille slots they cost), and the back
//            deboss floors sit DEEPER than the rim-chamfer band, so a single
//            filament swap prints an accent back skin with the words showing
//            through in the body color. Exact swap heights are echoed.
//    TPU fitments (print in TPU 90–95 A from an EXTERNAL spool — never the
//            AMS; see bambu_p2s_bringup.md §0):
//      grommet_usb  — slit wire grommet for the USB pass-through: feed the
//            head through the port, plug it, wrap the grommet around the
//            wire and press it in. Inner flange carries tug loads so cable
//            yanks fatigue the frame, not the board's connector.
//      plug_buttons — captive plug for the BOOT/RESET window: cap nests the
//            window's bevel, a 45° lip snaps in behind the wall (can't fall
//            out, can't be pushed inside), and two press TOWERS under the
//            face dimples run all the way down to the button caps and
//            actuate BOOT/RESET THROUGH the plug. The towers' reach is
//            derived from the panel record (the caps sit at the board
//            edge); the 2 mm pips they replace were measured ~5 mm short
//            on the first fitted case. Leashed like the SD cover — strap
//            into a channel in the top wall's skin, arrowhead barb through
//            an anchor beside the window (the innermost -x exhaust slot
//            gave up its station for the anchor) — because a plug you pop
//            out to check the towers is a plug you can drop.
//      plug_sd      — peel-open cover for the SD opening: countersunk cap
//            flush in a 45° rim (the flat-floored recess it replaces left a
//            cantilevered ring over the opening that drooped on the first
//            real print), a LEASH at the top end — flat strap into a skin
//            channel, arrowhead barb through an anchor hole in the plate,
//            mushroomed inside, so the peeled cover dangles captive (v0.6;
//            the v0.5 hinge tongue only hooked the plate edge and could
//            slide free) — and a fingernail scoop biting the rim at the
//            card end to peel it by the cap edge. The button plug carries
//            the same leash (see above); only the grommet goes without —
//            it lives wrapped on the cable itself.
//    frame_gauge — one corner of the frame including one boss and a wall
//            keyhole. Print FIRST (~10 % of the frame's filament): it proves
//            the glass corner radius, the boss offset signs and screw reach.
//    radius_gauge — four corner sockets at glass_r −0.4/−0.2/+0/+0.2 (each
//            opened by frame_reveal, exactly as the frame's opening is).
//            ~6 g: drop the panel's corner in each — the right radius hugs
//            it with no daylight and no bind. Settle the shape here, never
//            on the slab.
//
//  THE Z STACK-UP (read before changing any depth). Datum = the GLASS BACK
//  face, which is exactly where the tray's wall top ends and the bezel begins:
//
//      tray floor .......... back_t
//      components .......... comp_h      (rear-side connectors live in here —
//      PCB ................. pcb_t        this band is what the port cutouts
//      air gap ............. pcb_standoff open, floor -> PCB underside)
//      ---- glass back = tray wall top ----
//      glass ............... glass_t     (sits INSIDE the bezel's pocket)
//      bezel face .......... face_t
//
//  So cav_d counts everything BELOW the glass and bez_h counts the glass plus
//  the face. Counting glass_t in both (as v0.1 did) makes the tray 4 mm too
//  deep and leaves the lip floating 5 mm clear of the glass, clamping nothing.
//
//  Heat: cool air in through the bottom-wall intake, up past the board, out
//  the top-wall exhaust, with the back grille radiating in between. Do NOT
//  print this in PLA for a hot-running panel; PETG/ASA. Keep the vents clear.
//  The material call is also the DROP call: PLA is brittle exactly where a
//  handling drop loads this case (wall corners, the plate rim), and heat
//  ages it harder. PETG bends where PLA snaps; ASA adds UV life. Print the
//  case in PETG/ASA with 4 wall loops, and treat every crack as a material
//  answer before a geometry one.
//
//  ⚠️ CONNECTOR POSITIONS ARE NOMINAL — Waveshare's drawing dimensions the
//  glass + mount holes precisely but not every connector center. MEASURE the
//  USB-C / UART / CAN / RS485 / battery positions on YOUR Rev before printing.
//  The old pcb_h dispute (97.60 vs 126.20) is RESOLVED: 126.20 is the M3
//  mount-hole X SPAN, measured off a reference case print that fits the real
//  panel — a published summary had recorded the hole span as an outline
//  height. pcb_h stays 97.60 nominal (MEASURE); the tray cavity still sizes
//  itself to the larger of glass and board, so an oversized board grows the
//  case rather than jamming it.
//
//  ⚠️ DEV STATUS: FIRST REAL PRINT DONE (2026-08). It corrected three things
//  the drawings could not: the M3 offset signs (pattern matched only with the
//  panel upside down — signs flipped), the glass corner radius (r2.0 was the
//  wrong direction; r3.2, verify on the radius_gauge), and the SD cover's
//  flat-floored recess (unprintable cantilever — now a 45° countersink).
//  v0.6 is a durability/finish pass on top — keyhole doublers, boss root
//  fillets, adhesive rails, two-color swap bands, and the bottom-edge
//  brand went from slat-stencil vents to crisp deboss + a shadow gill
//  intake row (print feedback: the tie bands read as scan lines) — with NO
//  fit knob moved. v0.7 moves ONE fit knob, from print feedback: the
//  adhesive ledge sat at 4.5 mm because glass_t lumped the LCD module into
//  the border thickness, so the ledge floated 3.2 mm clear of the panel and
//  the adhesive touched nothing. The border is bare glass, far thinner —
//  glass_edge_t below — and the ledge face now sits glass_edge_t + adh_t
//  behind the glass face, matching the reference case print. The rear
//  stack (boss face 17.5, head seat 20.5, depth 23.5 — all confirmed against
//  that same reference) does NOT move: it chains from the module stack, not
//  the border. v0.8 is a DROP-PROTECTION pass, three additive moves: a
//  guard rim (glass_guard — the front rim stands proud of the glass, so
//  a face-down drop lands on plastic; the whole panel stack sits that much
//  deeper, nothing moves relative to the panel — but see v1.0: it is now
//  set to 0), a 45° fillet ring where
//  the walls meet the back plate (plate_fillet — corner-drop crack starter,
//  boss-root doctrine applied to the perimeter), and root reinforcement on
//  the SD leash (cap flare + shaft cone, both inside envelopes that were
//  already asserted). v0.9 is the HATCHERY pass: the brand vent pattern
//  (canary_vent_lib.scad — upright eggs in offset rows, a clutch in a
//  nest) replaces the slat grid on BOTH back grilles and the frame's side
//  gills. Open area holds or rises (echoes exact, via egg_area);
//  the dock-key slots, top exhaust, bottom intake and the whole
//  bottom-in → top-out convection doctrine are untouched, so the case
//  breathes the same wall-mounted (back blocked) and better on the
//  dock. v0.10 is the FIRST-FIT pass, everything measured off the
//  assembled case: (1) the -x top keyhole's inboard portrait slide opened
//  into an M3 screw-head pocket — that ONE leg is no longer cut
//  (khm_slide_ok derives which; that mount keeps two slides and the one
//  orientation that wanted the third hangs on three screws); (2) the
//  BOOT/RESET press pips became TOWERS whose reach derives from the panel
//  record — the caps sit at the BOARD EDGE, ~5 mm past the old pips — and
//  the plug grew the standard leash, its anchor taking the innermost -x
//  exhaust slot's station; (3) the side exit came back ON at the microSD
//  side, so a portrait wall hang runs the cable out the bottom; (4) the
//  hatchery egg grew true tangent-arc flanks (canary_vent_lib.scad — the
//  straight-flank capsule read as a slot, not an egg).
//  Still NOT fully print-validated; expect iteration.
//  Orientation: landscape, +X = width, +Y = up, +Z = toward the glass.
//  MOUNTING DOCTRINE: the panel mounts in its NATIVE orientation — no image
//  rotation in firmware — which puts BOOT/RESET at the TOP edge in use. The
//  frame's button window, labels and wall keyholes are all drawn for that:
//  window and labels on the top wall (back view: BOOT left, RESET right),
//  keyhole catches pointing up toward the button edge so the case hangs on
//  two screws and slides DOWN to seat. aa_dy and m3_ox/m3_oy are stated in
//  this orientation; a buttons-down build (panel rotated 180°, image flipped
//  in firmware) negates their signs and mirrors the frame's features.
// ============================================================================

use <canary_panel_lib.scad> // THE PANEL REGISTRY — every panel/board number
                            // this file uses comes from there, not from here
use <canary_vent_lib.scad>  // the brand vent shape: egg2d / egg_area
use <canary_mark_lib.scad>  // THE BIRD — mark_bird, shared with the coupon
use <canary_s3_lcd7_stamp.scad>   // GENERATED build stamp — see gen_stamp.py
// Downloaded this file on its own? It CUTS ITS VENTS with that library — a
// missing lib would render a sealed, overheating case with only a console
// warning. This guard turns that into a hard stop instead:
assert(is_num(egg_area(7, 4)),
       "canary_vent_lib.scad is MISSING — this case cuts its grille and gills with it. Download canary_vent_lib.scad from the same folder and keep the two files side by side.");

/* [What to render] */
part = "all";        // ["bezel","back","frame","frame_gauge","gauge","gauge_bezel","gauge_tray","stand","stand_gauge","radius_gauge","grommet_usb","plug_port","plug_buttons","plug_sd","bat_probe_fit","bat_probe_seat","bat_probe_grip","dock_probe_fit","dock_probe_seat","dock_probe_p","dock_probe_p2","ring_gauge","port_teth_hole","port_teth_barb","port_barb_proud","plug_teth_hole","plug_teth_barb","back_flush","fil_body","fil_ink","fil_accent","coupon_body","coupon_ink","coupon_accent","coupon_qr_body","coupon_qr_fill","frame_color","fil_overlap","fil_gap","all"]

/* [Board] */
// The board selector gets its own group because it is the FIRST decision and
// it used to be filed under [Glass slab], where nobody looked — there was even
// an empty [Panel variant] group further down, so the one heading that sounded
// right had nothing under it. Everything the case needs then follows from this
// one id; the groups below are what it derived, shown for reading.
// ── WHICH BOARD ─────────────────────────────────────────────────────────────
// The ONE knob that selects a panel. Every dimension below is read from that
// record in canary_panel_lib.scad — none of them are typed here any more.
// Adding a board is a record there plus an id in this enum; nothing in this
// file changes. The old scheme kept the numbers here and flipped signs with a
// `pv` multiplier at each point of use, which meant every new consumer had to
// remember to apply it, and a board that differed by anything OTHER than a
// half turn could not be expressed at all.
// One id, because one board has been measured. The "lcd7b" option that used to
// sit beside it was removed: it could not build — it hard-asserted on its own
// port map and then blocked every later render in the session, so the dropdown
// was offering a choice that only ever failed. See the registry's tail comment
// for the two defects it carried and what to measure to add it back properly.
panel_variant = "lcd7";   // ["lcd7"]
PANEL   = panel(panel_variant);
panel_ok = panel_check(PANEL) && panel_assert_printable(PANEL);

/* [Glass slab] */
// Bonded touch panel, in mm. READ-ONLY: every value here is derived from the
// board record above, so none of it appears as an editable knob. Shown because
// these are the numbers a person checks against calipers.
glass_w = pnl_glass_w(PANEL);    // touch-glass width  (X)
glass_h = pnl_glass_h(PANEL);    // touch-glass height (Y)
glass_t = pnl_glass_t(PANEL);       // glass + LCD module thickness where the module reaches —
                     // sets the cavity depth and the rear stack datum. MEASURE
glass_edge_t = pnl_edge_t(PANEL);  // MEASURED on the real panel (was 0.8, which sank the
                     // slab 0.4 deeper than it sits).
                     // the BARE-GLASS border the adhesive strips land on —
                     // much thinner than the module stack, and what the
                     // adhesive ledge must rise to meet. Set from the
                     // reference case print that fits the real panel: its
                     // ledge face sits 1.3 mm behind the front face, i.e.
                     // this + adh_t. (v0.6 chained the ledge off glass_t and
                     // left it floating 3.2 mm clear of the panel.)
// The LCD module can behind the glass — stated INDEPENDENTLY of the ledge
// geometry, deliberately: the frame_glass fit gate probes the frame against
// THIS outline, so a ledge widened past the bare border collides in CI
// instead of shrinking its own probe (and the insertion asserts below keep
// the can's path through the ledge ring open). Nominal from the 7" module
// family + the Rev1.2 adhesive photos (~10 mm bare sides / ~6 mm button
// edge); put calipers on the can — MEASURE.
// MEASURED off the Waveshare drawing, and the drawing proves itself: the M3
// insets sum to the outline exactly — 19.76 + 126.20 + 19.76 = 165.72 and
// 18.31 + 65.65 + 13.64 = 97.60 — so both dimensions bracket the CAN, not the
// PCB. (The board is visibly smaller and sits on standoffs above it.)
panel_core_w  = pnl_core_w(PANEL); // module can width  (X)
panel_core_h  = pnl_core_h(PANEL);  // module can height (Y) — 2.40 SMALLER than the
                        // old nominal, which was eating the ledge's clearance
panel_core_dy = pnl_core_dy(PANEL); // can center offset from glass center, NATIVE pose.
                        // DERIVED, and it needed no calipers: the drawing puts
                        // the M3 pattern 18.31 from the module's FPC end and
                        // 13.64 from its button end, so the pattern center sits
                        // 2.335 off the module center. m3_oy (+0.9) is already
                        // print-validated in the native pose, and native is the
                        // drawing turned half a turn, so
                        //     panel_core_dy = m3_oy - 2.335 = -1.435
                        // Cross-check that it is not nonsense: aa_dy is +1.02,
                        // which puts the LIT AREA 2.46 toward the button edge
                        // WITHIN the module — exactly where the FPC and driver
                        // eat module height. The two offsets disagreeing in
                        // sign is the physically expected result, not an error.
                        // (I briefly flagged this sign as suspect on the theory
                        // that it must track aa_dy. It does not, and the ledge
                        // assert was right to reject the flip.)
glass_r = pnl_glass_r(PANEL);       // corner radius of the glass slab — MEASURED against the
                     // real panel on a screen comparator, calibrated against
                     // the PANEL OUTLINE itself (192.96 mm) rather than a
                     // bank card: the longest reference to hand more than
                     // doubles the precision of the px/mm step, and that step
                     // — not the corner match — is where the error lives.
                     // d = 3.40 mm at the corner diagonal (r = 2.414*d).
                     // A first pass calibrated on a card read 9.05; the two
                     // agreed to 1% on the ON-SCREEN arc (40.7 vs 41.2 px)
                     // and disagreed only through the calibration. The cavity is never rounded
                     // more than this: a pocket rounder than the glass binds
                     // (or gaps) on all four corners.
                     //
                     // The road here is the cautionary tale. 2.0 -> 3.0 ->
                     // 3.2, three revisions each stepping ROUNDER by the only
                     // increment the gauge could resolve, while the truth sat
                     // 5.85 mm away the whole time. Every one of those prints
                     // showed the same gap in all four sockets, which reads as
                     // "close, nudge it" and actually means "you are nowhere
                     // near". A gauge whose range is derived from the value it
                     // is checking can only ever confirm small errors.
                     // Measure first, print to confirm — never to search.
// ⚠️  IF EVERY SOCKET ON THE GAUGE SHOWS THE SAME OBVIOUS GAP, do not nudge
// glass_r by 0.2 and reprint. The gauge is telling you the answer is nowhere
// near its window — and by default that window is only 1.2 mm wide. Sweep
// COARSE first, fine second:
//     openscad --export-format binstl -o rg_coarse.stl \
//              -D 'part="radius_gauge"' -D rg_center=8 -D rg_step=1 \
//              canary_s3_lcd7.scad              # sockets 6.5/7.5/8.5/9.5
//     ...then with rg_center=<winner> and rg_step=<coarse step / 3>.
//
// THE /3 IS NOT A ROUNDING — it is what makes the second pass provably
// cover the first. The four sockets sit at center ± 0.5·step and ± 1.5·step,
// so they span 3·step, SYMMETRIC about the center. A coarse winner W only
// tells you the truth lies within ±step/2 of W, so the fine pass must span
// at least that: 3·fine ≥ coarse, i.e. fine ≥ coarse/3. Coarse 1 → fine
// 0.34 covers ±0.51 around W, which contains the whole ±0.5 the coarse pass
// left open. (An earlier draft of this note said "re-run with rg_step=0.2",
// which spans only ±0.3 — a true 8.4 would have escaped BOTH passes. Exactly
// the failure this whole knob is about, one level up.)
// FASTER, before printing anything: measure it. Sit a square into the slab's
// corner to find where the sharp bounding-box corner WOULD be, and measure
// the shortest distance d from that point to the glass. A radius r pulls the
// arc back along the 45° diagonal by r(sqrt(2) − 1), so
//     r = d / 0.4142 = 2.414 * d
// (r 3.2 gives d 1.33; r 8 gives d 3.31 — both easy caliper reads.)
// The history of this number is the warning: 2.0 -> 3.0 -> 3.2, every
// revision moving ROUNDER in 0.2 steps, because 0.2 steps were all the gauge
// could ever see. A tool that can only resolve the error it already assumes
// will walk toward the answer forever without arriving.
rg_center = glass_r;  // radius-gauge sweep center — override it to hunt wide
rg_step   = 0.2;      // ...and its step. Four sockets at center ± 0.5·step
                      // and ± 1.5·step: SYMMETRIC, spanning 3·step. The
                      // symmetry is the point — an asymmetric sweep leaves
                      // more room on one side of the center than the other,
                      // so "the winner is within half a step" stops being
                      // true and a two-pass search can skip the answer.
/* [Panel variant] */
// ════════════════════════════════════════════════════════════════════════════
//  ESP32-S3-Touch-LCD-7  vs  -7B  — one case, half a turn apart
//
//  The two boards share their entire MECHANICAL interface, to the hundredth:
//    glass 192.96 x 110.76      active area 154.88 x 86.72
//    PCB   165.72 x 97.60       M3 pattern  126.20 x 65.65
//    (M3 inset 19.76 from the PCB's side edges, 18.31 / 13.64 top and bottom)
//
//  What differs is where the COMPONENTS sit, and every case-relevant one is a
//  180 deg rotation of its opposite number:
//
//    feature        LCD-7 (800x480)     LCD-7B (1024x600)
//    ESP32 module   bottom-left         top-right
//    microSD        top-left            bottom-right
//    USB-C pair     left edge           right edge
//    BOOT / RESET   bottom-center       top-center
//
//  So the same printed case serves both: seat the 7B a half-turn round and
//  every boss, screw and wall lands where it already is. Two consequences,
//  both handled:
//    1. The asymmetric offsets flip sign — the M3 pattern is NOT centered on
//       the glass, and neither is the SD socket. That is what pv does below.
//    2. The image comes up upside down. Rotate the display in firmware; the
//       hardware does not care which way up it is bolted.
//
//  This is not a new discovery so much as a named one: m3_ox/m3_oy already
//  carry a note that the FIRST REAL PRINT flipped them 180 deg from what a
//  reference case recorded. That reference was a 7B. The flip WAS the variant
//  difference, found the expensive way before anyone knew there were two.
// ════════════════════════════════════════════════════════════════════════════

aa_w = pnl_aa_w(PANEL);       // active area width
aa_h = pnl_aa_h(PANEL);        // active area height
aa_dy = pnl_aa_dy(PANEL);        // AA center offset from glass center — NATIVE mounting,
                     // i.e. buttons at the TOP: borders 11.00 top / 13.04
                     // bottom, so the lit area sits 1.02 TOWARD the button
                     // edge. (A buttons-down build — panel rotated 180°,
                     // image flipped in firmware — negates this.)
                     // SIGN CORRECTED: this read -1.02 with a comment
                     // claiming "13.04 top / 11.00 bottom", which is the
                     // DRAWING's orientation — and the drawing shows the
                     // buttons at the BOTTOM. Native mounting turns the panel
                     // half a turn, so the 11.00 border is the one that ends
                     // up on top. Caught by calipers on a real panel: the
                     // BOOT/RESET edge measured 10.90 against 13.10 opposite,
                     // the reverse of what the model asserted. Same family as
                     // the m3_ox/m3_oy flip — an offset copied straight off a
                     // drawing without turning it into the mounting pose.

/* [PCB stack behind the glass] */
pcb_standoff = pnl_standoff(PANEL);  // glass back → PCB front (the white M3 standoffs) — MEASURE.
                     // This one decides whether the bezel lip actually reaches
                     // the glass: too small and the case won't close, too big
                     // and the lip never touches it.
pcb_t   = pnl_pcb_t(PANEL);
comp_h  = pnl_comp_h(PANEL);      // tallest thing behind the PCB (USB-C / terminals / batt JST) — MEASURE.
                     // Also sets the port band: cutouts open floor → PCB underside.
pcb_w   = pnl_pcb_w(PANEL);    // PCB outline width  (X) — from the drawing
pcb_h   = pnl_pcb_h(PANEL);     // PCB outline height (Y) — MEASURE (see the header warning)

/* [Board mounts] — 4x M3 corner standoffs carry the PCB (measured pattern) */
m3_dx = pnl_m3_dx(PANEL);      // M3 hole pattern width — MEASURED from a reference case
                     // print that fits the real panel (v0.2 shipped 165.72
                     // here — the outline width, which put every boss under
                     // the board's edge; see the header for how 126.20 also
                     // resolves the old pcb_h dispute). Verify on your board.
m3_dy = pnl_m3_dy(PANEL);       // M3 hole pattern height — measured with m3_dx; verify
m3_ox = pnl_m3_ox(PANEL);         // pattern center offset from the GLASS center, stated in
m3_oy = pnl_m3_oy(PANEL);         // FRONT view, panel mounted NATIVE (buttons at the top):
                     // +x = right, +y = up. SIGNS SETTLED BY THE FIRST REAL
                     // PRINT: with the v0.4 signs (−1.5/−0.9) the panel only
                     // matched the printed pattern upside down — the offsets
                     // are flipped 180° from what the reference-case reading
                     // recorded. The pattern is NOT symmetric about the glass
                     // center, which is exactly why that mistake is visible
                     // at all — and why a panel can't be flipped inside a
                     // case drawn for the other orientation.
m3_pilot = 2.7;      // self-tap pilot for M3 into printed bosses
                     // frame screw LENGTH: the head seats on the plate, then
                     // frame_boss_h of boss, then the panel's standoff_len stud
                     // — M3 x 8 engages 5.0 of the 6.9 mm stud; x10 reaches
                     // 7.0 and bottoms in a blind stud. Say "M3 x 8"; x10 only
                     // with through-threaded standoffs

/* [Screen] */
bez_lip = 3.0;       // MINIMUM acceptable overlap of the lip onto the glass border.
                     // The real overlap is derived from the glass/AA borders and
                     // win_margin below; the assert fires if it drops under this.
win_margin = 1.0;    // window opened this much beyond the active area on each
                     // side, so print tolerance + panel placement can't eat
                     // visible pixels (the lip pays for it out of the border).
                     // Was 0.6: after the pocket's tol_slide float (0.25) and a
                     // typical ±0.2 XY print error that left 0.15 mm before the
                     // bezel edge covered pixels, while the lip had 10 mm to
                     // spare — 1.0 spends 0.4 of that spare on the picture

/* [Connectors] */
// openings, offsets from the board center along their wall.
// NOMINAL — MEASURE. The bottom (−Y) wall carries the USB/UART cluster; the
// two short (±X) walls carry CAN/RS485/battery. Set opt_* to open them.
opt_bottom_ports = true;
bottom_open_w = 96.0;   bottom_open_dx = 0.0;   // wide channel over the bottom cluster
opt_side_ports = true;
side_open_h = 40.0;     side_open_dy = 0.0;     // tall slot on each short wall

/* [Ventilation] */
// MUST let the backlight/SoC heat convect out.
// A grille alone only radiates; convection needs a low inlet and a high
// outlet, so the bottom wall takes air in and the top wall lets it out.
vent_back = true;        // grille in the TWO-PART TRAY's back (part="back").
                         // Separate from plate_grille below, and the split is
                         // deliberate: the tray carries no badge, no symbol
                         // and no small print, so there is nothing on it for a
                         // lattice to compete with and no reason it should
                         // lose its vents because the one-piece frame's plate
                         // became a composition. One knob for two parts would
                         // have quietly stripped a part nobody was editing.
plate_grille = false;    // large grille in the ONE-PIECE FRAME's back plate.
                         // OFF — ⚠️ READ THIS BEFORE TURNING IT BACK ON, and
                         // read the honest-thermals note under vent_top too.
                         // That plate is the product's face when it hangs on a
                         // wall, and it is composed as one: badge, lockup,
                         // symbol, small print, and NO holes that nothing goes
                         // through. Forty-odd eggs of lattice across it was the
                         // version this replaced. So this switch is a real
                         // trade and not a tidy-up.
                         // ⚠️ IT IS ALSO NOT THE WHOLE RECOVERY. On its own it
                         // restores the field with the BADGE COLUMN still
                         // reserved — about a quarter of what the plate gave
                         // up. badge_column = false is the other half of the
                         // knob, and the echo prices both so the difference
                         // cannot be quoted from memory.
                         // What it does NOT cost: the convection path itself,
                         // which is bottom-wall intake to top-wall exhaust
                         // (vent_bottom / vent_top, both still on) and never
                         // ran through this plate. NOTHING IN THIS REPO
                         // MEASURES THE REQUIREMENT — no thermal model, no
                         // logged die temp — so "it will be fine" is not
                         // something this file is entitled to say.
vent_rows = 8;           // grille rows
vent_cols = 17;          // grille columns
vent_slot_w = 6.5;       // egg base width — the grille is the HATCHERY
                         // pattern (canary_vent_lib.scad): upright eggs in
                         // offset rows, a clutch in a nest. One egg passes
                         // 44% more air than the slat it replaced (29.4 vs
                         // 20.4 mm2 per cell — the tangent-arc flanks added
                         // ~2% over the old straight-flank capsule) with no
                         // slot corners to shed vortices or collect dust lines
vent_slot_l = 9.0;       // egg length (upright: sheds drips on a vertical
                         // face; see the lib header for limits)
vent_tip    = egg_tip(); // DERIVED from canary_vent_lib.scad — never a typed
                         // number. The tip ratio is the line-wide brand
                         // constant; its one home is egg_tip() in the library,
                         // so changing it there moves this case with it. A
                         // local copy would read identically today and silently
                         // strand the 7" wearing the old mark the first time
                         // the line changed — which is exactly what an earlier
                         // draft of this line did.
                         //
                         // 0.28 was the FEATHER the library shipped with;
                         // 0.72 is the EGG. Same construction either way (a
                         // tangent-arc ovoid, wide end down — the lib header
                         // has the geometry) — the ratio alone decides whether
                         // it reads as a teardrop with a point or an egg with
                         // a rounded crown. 0.58 still reads as a pear. Judged
                         // off a render of all three side by side at $fn 96,
                         // because at the preview's default facet count every
                         // one looks like a polygon.
                         //
                         // Still self-supporting: the crown is a circle, so
                         // the top of the hole is an arch, not a bridge — the
                         // same reason the feather's point was safe.
vent_pitch_x = 9.2;      // column pitch
vent_pitch_y = 10.2;     // row pitch

/* [Frame — the clutch] */
// ════════════════════════════════════════════════════════════════════════════
//  THE GRILLE IS A CLUTCH OF EGGS, and the badge is what hatched out of it.
//
//  ONE SIZE. An earlier cut of this graded the egg SIZE with distance from the
//  mark — small near the badge, opening up toward the edges — on the theory
//  that a size ramp reads as depth. On the part it reads as a mistake: eggs
//  are a known object, and a field of them at nine different sizes looks like
//  a misprint rather than like perspective. Every egg here is the same egg.
//  That is also the only version of this that can claim to be a clutch.
//
//  BELIEVABLE, therefore BIGGER. The old egg was 7.2 x 5.2 mm at a 7.6 x 8.0
//  pitch, which is as large as that pitch can carry (the web asserts were both
//  at their floor). At that size and that density it reads as perforation —
//  the shape is an egg but nothing about the field says so. This one is
//  9.0 x 6.5 on a 9.2 x 10.2 pitch: the same 1.38 proportion, half again the
//  area each, and enough plate between them that you see the shape instead of
//  the pattern.
//
//  AND IT HAS A RHYTHM. A uniform lattice is a grille; a clutch is grouped.
//  vent_beat is a repeating pattern of hits and rests read across each row and
//  ROTATED one step per row, so the field breaks into little groups of two and
//  three with clear plate between them, and no two rows line up into stripes.
//  It costs the count exactly what it says it costs — the echo reports the
//  open area either way, and the rests are where "as many as we need and no
//  more" is actually spent.
//
//  THE STORY, since it is the point and not decoration: the plate reads eggs,
//  eggs, eggs across the machine half, and resolves on the left into one
//  hatched canary at forty times the size. Nothing is drawn to say that. The
//  rhythm and the badge do it, which is the only way it stays true when
//  somebody changes a number.
// ════════════════════════════════════════════════════════════════════════════
// Hits and rests, one character per station, read left to right across a row.
// Any two characters would do; '#' and '.' are chosen because the pattern is
// legible AS a pattern in the source, which is where it gets edited.
vent_beat  = "##.###.";
vent_beat_shift = 1;  // stations the pattern rotates per row.
                      // ONE, and the reason is what it does to the rows above
                      // and below rather than anything about the row itself.
                      // At 3 the groups walk sideways and the field reads as a
                      // diagonal drift — a scatter, not a clutch. At 1 each
                      // row's group overlaps its neighbors' by all but one
                      // station, so the hits pile into compact blobs: a nest
                      // of seven low on the machine half and a trio up by the
                      // spec block. Two groups, which is the rhythm this was
                      // asked for — egg, egg, egg … egg, egg. Set 0 for the
                      // same beat every row, which stripes.

// ── THE ONE KNOB TO TURN IF IT RUNS HOT ────────────────────────────────────
// The badge owns the left half of the plate and carries no vents at all. That
// is most of what this revision costs thermally — far more than the grade —
// and it is one word to undo, so the trade is a choice and not a fait
// accompli. With it false the grille fills the whole field again, graded, and
// the mark keeps only its own margin (bird_vent_gap). The plate gets busier;
// the echo will tell you exactly what you bought.
badge_column = true;  // false = let the grille back into the badge's half

vent_top = true;         // EXHAUST — slots through the top (+Y) wall
vent_top_n = 14;
vent_bottom = true;      // INTAKE — slots through the bottom (−Y) wall, placed
                         // outboard of the connector channel so they stay open
vent_bottom_n = 5;       // per side
vent_sides = true;       // side (±X) chimneys, kept clear of the port slots
vent_side_n = 3;         // per side, stacked toward the top corners

/* [Print tolerances] */
// only the two this case actually has fits for. There is
// no press fit anywhere in this design (no magnet pocket, no light pipe), so
// tol_press is deliberately absent rather than declared and ignored: a knob
// that tunes nothing sends you through reprints that cannot change the part.
tol_slide = 0.25;    // the glass/board pockets — the SLIDE station on the coupon
tol_hole  = 0.35;    // M3 clearance through the bezel ears — the SCREW station

/* [Shell] */
wall   = 3.0;    // side wall thickness (big case → thicker)
face_t = 3.0;    // bezel face
back_t = 3.0;    // rear tray floor
r_out  = 6.0;    // outer corner radius

/* [Fasteners] */
// M3 x 16–20 from the FRONT, through the bezel lobes, self-tapping
// into FULL-HEIGHT corner posts in the back tray (the posts bridge the whole
// cavity, so the screw threads into solid material the entire way — not across
// an empty gap). Heads sit in a counterbore on the bezel's outboard ears,
// clear of the glass.
lob_d = 9.0;  lob_o = 3.0;   // lobe Ø / diagonal offset outboard of the cavity corner
m3_nom = 3.0;                // M3 shank
lob_pilot = 2.7;  cb_d = 6.0;  cb_h = 2.0;
// Clearance is DERIVED from the coupon's SCREW tolerance, so dialing tol_hole
// after a coupon print actually moves this hole. (Self-tap pilots are not:
// lob_pilot/m3_pilot are undersize on purpose, so the thread forms.)
screw_c = m3_nom + tol_hole;

/* [Frame — one-piece drop-in case (part="frame")] */
frame_wall   = 2.0;  // sleeve wall (also the visible front rim around the glass)
frame_reveal = 0.15; // per-side glass↔opening clearance. The opening AND its
                     // corner radius both track the slab by this much, so the
                     // reference case's corner gap cannot come back.
glass_guard  = -0.2; // WIPE RELIEF, from the first fitting print: the rim
                     // sits ONE LAYER BELOW the glass face, not level with it.
                     //
                     // Why not 0. Flush is flush in CAD and proud in plastic —
                     // the printed rim stood above the glass, and a cloth
                     // dragged across the screen caught on it. A touch panel
                     // gets wiped more often than it gets dropped, so the last
                     // 0.2 mm belongs to the cleaning cloth: run a finger off
                     // the glass and it steps DOWN onto the case, never up
                     // into an edge.
                     //
                     // Sign convention, now that all three states are real:
                     //   > 0  glass recessed behind the rim — drop protection
                     //   = 0  nominally flush (and proud once printed)
                     //   < 0  rim below the glass — wipe relief, what we ship
                     //
                     // ⚠️ TRADE-OFF, stated once so it is a choice and not an
                     // accident: this knob began as drop protection. At 0.6 a
                     // face-down drop landed on the case's rim; at -0.2 it
                     // lands on the panel, and the panel now stands slightly
                     // proud, so it lands a little harder. That is the price
                     // of a screen you can wipe. Nothing else changes — the
                     // rear stack chains from the glass, so the whole assembly
                     // moves with it and fr_depth follows.
                     // Set it back to 0.6 to restore the raised lip.
                     // GUARD RIM: the front rim stands this proud of the
                     // glass, so a face-down drop lands on the case's rim,
                     // not the panel — the raised-lip trick every phone case
                     // uses, executed as trim: the rim keeps its 45° entry
                     // chamfer, so it reads as a picture-frame reveal, not a
                     // bumper. Internally the whole panel stack just sits
                     // this much deeper (fr_depth grows by it; the rear
                     // stack still chains from the glass, so nothing moves
                     // RELATIVE to the panel). usb_zc and edge_vent_z are
                     // measured from the GLASS face, so their values survive
                     // this knob. 0 restores a flush face.
standoff_len = pnl_stud_len(PANEL);  // the panel's own white M3 standoffs, PCB back → tip — MEASURE
frame_boss_h = 3.0;  // boss standing proud of the back plate's inner face
frame_boss_d = 8.0;

/* [Frame — BOOT/RESET button window] */
btn_w  = 25.0;       // BOOT/RESET access window through the TOP wall (measured)
btn_h  = 13.5;       // window height across the wall's depth
btn_dx = 0.0;        // window center offset along the top wall
btn_lbl_dx = 9.0;    // BOOT/RESET label centers, ± of the window center

/* [Frame — wall mounting (keyholes)] */
mount_keyholes = true;  // wall-mount keyholes through the back plate, one in
khm_dx = 78.5;          // EACH of the four corners (first-print feedback: two
khm_y  = 34.0;          // held the case but let the bottom float off the wall).
khm_head_d  = 9.5;      // All four share one orientation — head hole LOW,
khm_slide_w = 4.5;      // slide running UP, catch at the button edge — so the
khm_len = 13.0;         // case hangs over the screws and slides DOWN to seat.
                        // khm_y sets the TOP pair (head-hole center); the
                        // bottom pair mirrors the feature about y=0. The head
                        // hole passes a #8 / M4 pan head; the slide, its
                        // shank; khm_len is head-hole center → catch center.
mount_portrait = true;  // PORTRAIT hanging: each keyhole gains slides in
                        // BOTH ±x — a rigid case translates one way only, so
                        // a single-direction slide would let just one column
                        // engage and 4-screw portrait could not seat at all.
                        // With both, every hole has a catch path for whichever
                        // way the case drops. Rotate the case 90°
                        // and a SIDE pair of keyholes becomes the level top
                        // pair — mouths 81 mm apart (khm_y + khm_y + khm_len)
                        // — with the outboard slides then pointing world-up,
                        // so the same hang-and-drop works in BOTH portrait
                        // directions (and 4-screw portrait pins flat, same
                        // as landscape: the second row lands 157 mm below).
                        // ONE exception, cut by geometry not by hand: a slide
                        // that would open into an M3 screw-head pocket is not
                        // cut at all (at stock dims that is the -x top
                        // keyhole's inboard leg — the mirrored M3 offset puts
                        // that boss 3 mm closer). That mount carries two
                        // slides instead of three; the one orientation that
                        // misses it (+x wall up) pins on the other THREE
                        // screws. See khm_slide_ok at fr_bosses.
                        // Geometry only — the UI stays landscape until the
                        // firmware grows a mount_rotation setting.
khm_plen = 10.0;        // the portrait slides run SHORTER than the vertical
                        // ones — the side walls are close, and 10 mm of drop
                        // still parks the shank on a full catch (asserted
                        // clear of the wall fillet ring below).
khm_pad_t = 2.0;        // keyhole DOUBLER pads on the plate's inner face —
khm_pad_w = 3.5;        // the screw head bears on back_t + khm_pad_t of
                        // material (5 mm, not 3) and the slide's catch shears
                        // a wider section; khm_pad_w is the pad's reach beyond
                        // the keyhole outline. The pads live in the clear band
                        // behind the plate (asserted against the component
                        // band below).
plate_fillet = 1.6;     // 45° fillet ring where the walls meet the back
                        // plate's inner face — the drop-load path. A corner
                        // drop flexes the walls against the plate; a square
                        // internal corner there is a crack starter (same
                        // doctrine as the boss root fillets). Sized to stay
                        // clear of the button window's back edge and the
                        // keyhole pads' band (both asserted below); the back
                        // grille never reaches the perimeter, so no vent is
                        // lost. 0 disables.
khm_mouth_c = 0.8;      // lead-in chamfer around each keyhole's mouth on the

/* [Frame — glass bonding ledge] */
                        // outer skin, so the case slips over the screw heads
                        // without catching on an elephant-footed rim
// Adhesive ledge — the panel ships with adhesive strips on its BACK border
// (≈10 mm down each side, ≈6 mm along the button edge, none over the FPC at
// the bottom — see the Rev1.2 photos). The ledge is the landing for them:
// the glass drops in from the front, rests on the ledge, the adhesive bonds
// it, and the 4 screws become backup instead of the only thing setting the
// glass depth. The glass only sweeps the front glass_t of the case going in,
// so a rear ledge never blocks insertion.
adh_t      = 0.5;   // adhesive pad thickness — MEASURE; sets the glass-back datum
ledge_t    = 2.5;   // ledge thickness behind the panel
ledge_side = 10.0;  // ledge width along each short (±x) edge
ledge_top  = 6.0;   // along the button (top) edge
ledge_bot  = 2.0;   // along the FPC (bottom) edge — keep small, the FPC lives there
ledge_slope_max = max(ledge_side, ledge_top, ledge_bot);  // deepest back-slope
ledge_slope_n   = 20;  // steps in the 45° back-slope. Each step overhangs by

/* [Frame — back-plate branding] */
                       // its own height, so ANY count self-supports; this only
                       // sets how smooth the sloped face reads.
// Every ledge carries a 45° back-slope wedge down to its wall, so the frame
// prints BACK-PLATE-DOWN with the ledges fully self-supporting — solid
// material under the adhesive landing, no overhang, no sacrificial geometry.
// (Face-down would hang the ledges over the glass pocket; don't.)
brand_back = "CANARY";   // the back lockup's HERO line — centered,
                                 // tracked caps (the 7" is gone: the case
                                 // family is one Display line, the panel
                                 // size is a spec, not a name)
brand_sub  = "SECURACV";         // the small tracked company line beneath
// THE MAKER LINE, and where it moved from. It used to be the fourth row of
// the RATING BLOCK — filed with "5V = 2A" and "INDOOR USE ONLY" in the
// upper-right corner, at rating type size, which said the company name was a
// specification of the power supply. It belongs to the lockup: product name,
// company, maker. So it is the bottom row of the lockup now, in the lockup's
// column, and the rating block is three lines of electrical facts with nothing
// else mixed into them.
//
// The YEAR still comes from the build stamp rather than being typed — see
// lcd7_stamp_year() at the rating block for why that is not a style choice.
brand_maker = "ERRERlabs";       // the maker row, under the hero line
brand_edge = "SecuraCV Canary";               // debossed on the visible bottom edge
// THE LOCKUP HAS A BAND NOW, not a leftover. The three rows used to be jammed
// into the last 13 mm above the rim because everything above them was grille —
// 3.3 mm of leading between rows whose caps are 4 mm tall, which is what
// "squished" looks like when you measure it. With the vents cleared out of the
// mark's field the whole bottom of the plate is free, so the rows get real
// leading, the product name gets to be bigger than the company line above it,
// and there is 3 mm of air under the last row instead of a hairline.
//
// ⚠️ POSITIONS HANG OFF THE MARK, not off the plate's bottom rim, and that
// change is the whole of "put the logo proper under the bird". Measured up
// from the rim, the three rows were an independent object that happened to be
// on the same plate: the mark could move, grow or shrink and the type stayed
// where it was, which is exactly how it ended up stranded in the last 13 mm
// with the badge floating 30 mm above it. Measured DOWN from the mark's box,
// mark + type is ONE lockup — move bird_dx/dy/h and the words follow, and
// bird_dy becomes a knob that positions the whole logo rather than half of it.
//
// The two gaps are DERIVED from these drops rather than typed beside them, so
// a row that moves cannot leave a stale gap comment behind. The asserts below
// check both gaps, the rim clearance and the gap to the mark.
brand_drop_sub   =  7.8; // from the mark's box bottom down to each row's
brand_drop_hero  = 16.8; // center. sub is the company line (tracked, smaller),
brand_drop_maker = 25.0; // hero the product name, maker the footnote.
brand_sz        =  6.0;  // hero cap height
brand_sub_sz    =  3.8;  // company line — deliberately UNDER the hero: this
                         // is the product's plate, and the tracking is what
                         // gives the company line its presence, not its size
brand_maker_sz  =  3.0;  // the maker row is a footnote, not a third shout
// (The lockup rides the MARK's column — brand_dx is set beside bird_dx, below,
// because OpenSCAD reads assignments in order and a forward reference here is
// silently undefined rather than an error.)

/* [Frame — adhesive wall rails] */
// Adhesive WALL mounting — the no-screws alternative to the keyholes. Two
// outlined rails on the back plate are kept smooth and uninterrupted: no
// grille slot, keyhole pad, boss pocket or deboss ever lands inside one
// (asserted below, and gated in CI by the frame_adh_rail fit check). Sized
// for 15.9 x 70 mm INTERLOCKING picture-hanging strip PAIRS (e.g. Command
// Medium) — pairs, not single stretch-release foam strips, deliberately:
// the mounted case fully covers its strips, so a single strip's pull tab
// would be sealed behind it, unreachable, and "damage-free removal" would
// mean prying. With pairs the case pulls straight off its wall halves
// first (grip it by the side gills / bottom port), and THEN every wall
// tab is exposed for its stretch release — removal by the product's own
// doctrine. Mount tabs DOWN, strips vertical, inside the moat outlines;
// wipe the zone with IPA first. The zone's finish IS the build plate's
// finish — a smooth sheet bonds best; the foam also bonds through light
// texture. A hairline moat outlines each zone so the strip lands in the
// right place; it is label_back_depth deep, so it reads in the body
// color on a two-color print.
// THE TRADE: the rails' keepouts cost the back grille 6 of its columns
// (66 slots ≈ 14 cm² at stock dims — the echo computes the exact numbers
// for your config from the same predicate that cuts the slots). The
// convection path proper — bottom-wall intake → top-wall exhaust — is
// untouched, and relocation was checked and loses as much or more: the
// plate has no other clear 70 mm column (the SD zone, boss pockets and
// keyhole pads own the rest). Screw-mount builds can set adh_rails=false
// and reclaim every slot.
// OFF, and the mark took the space. The two smooth 17 x 74 zones were the
// only clear full-height columns on the plate, and they cost the grille 64
// slots (~18 cm2) to buy a no-drill mount most builds do not use — while
// making the back read as two blank rectangles. The bird now sits in that
// clear middle instead. Set true for a Command-strip build and the bird
// keeps out of the rails' way automatically (its keepout is asserted).
adh_rails   = false;
adh_rail_dx = 12.0;  // rail centers at ±this — the only clear full-height
                     // columns on the plate: inboard of the SD mouth, the
                     // boss head pockets and the keyhole pads (all asserted)
adh_rail_w  = 17.0;  // zone width  — 15.9 strip + placement slack
adh_rail_l  = 74.0;  // zone length — 70 strip + placement slack
adh_mark_w  = 0.8;   // outline moat width

/* [Frame — the Canary mark] */
// THE BIRD, debossed into the middle of the back plate where the adhesive
// rails used to be. Geometry comes from canary_mark_lib.scad so the coupon's
// emblem station and this are the same bird — that library's header explains
// why it is authored rather than traced from the brand art.
//
// DEBOSSED, not raised, and that is not a style choice: the frame prints
// BACK-PLATE-DOWN, so this face is against the build plate. A raised mark
// there would have to protrude below z = 0. Every other graphic on this face
// (brand words, rating block, QR) is a deboss for the same reason, and they
// all share one floor depth so a single tool change serves the lot.
back_bird   = true;
// THE PLATE IS THE BADGE'S, and everything else is laid out around it. That
// is the reordering this revision is: the mark used to be sized to whatever
// gap the QR and the SD mouth left over (32 mm, "the clear middle runs about
// x ±21"), which is how you get a logo that looks like it was fitted in
// afterwards — because it was. Now the mark is placed first, at the size it
// wants, and the vents, the spec block and the help line take what is left.
//
// 70 nominal draws about 62 x 68 mm: the largest mark that keeps its tail
// clear of the microSD mouth and its crown clear of the top vent band, on a
// plate whose usable field is 197 x 115 minus four keyholes, four boss
// pockets and a 40 mm card window. Every one of those clearances is asserted.
bird_h      = 70.0;  // NOMINAL height (mark_span units) — see mark_h_mm() for
                     // what actually gets drawn; the design span carries
                     // headroom the drawing does not use
bird_dx     = -18.5; // LEFT of center, and not by taste: the SD mouth owns
bird_dy     = 13.2;  // x 23..48, so a centered mark would put its tail through
                     // the card window. Offsetting the badge and letting the
                     // right-hand field carry the spec block is the
                     // composition that admits the window exists.
                     // dx moved in from -24: with the grille gone the LEFT
                     // WING is empty plate, and the help QR wants it (qr_back).
                     // The badge had to give back 5.5 mm for the symbol plus
                     // its quiet zone to clear both the mark and the rim —
                     // asserted below, and the assert is 0.6 mm from firing,
                     // so this pair of numbers is a fit, not a preference.
                     // dy is NOT the mark's taste either: the lockup now hangs
                     // off the mark (brand_drop_*, below) instead of off the
                     // plate's bottom rim, so mark + type is ONE object with a
                     // known height. 13.2 is what centers that object on the
                     // plate — equal air above the crown and under the maker
                     // row. Move the mark and the type follows it; move it too
                     // far and the rim asserts catch the object, not the row.
// The lockup rides the MARK's column, not the plate's. The badge sits left of
// center because the card window owns the right, and type centered under a
// badge that is not centered reads as a mistake in both places at once.
brand_dx    = bird_dx;
bird_rib    = 2.6;   // stroke width. The mark is MONOLINE — one weight for
                     // the outline, the C, the V and the pad alike — so this
                     // one number decides whether the whole drawing prints.
                     // 2.6 at h = 70 is 4.1 design units against the 4.9 the
                     // drawing can carry (mark_rib_ok, asserted in the
                     // library): the proportion the brand art uses, with the
                     // shoulder notch and the C's mouth still open.
// The mark's real footprint, straight off the library, stroke caps included.
// Hoisted here because the grille keepouts are assembled long before the
// branding asserts run, and both have to mean the same box.
bird_half_w = back_bird ? mark_w_mm(bird_h, bird_rib)/2 : 0;
bird_half_h = back_bird ? mark_h_mm(bird_h, bird_rib)/2 : 0;
bird_vent_gap = 3.0;  // plate left between the mark and the nearest egg. The
                      // old 0.6 was measured to the keepout box, and with the
                      // box oversized the visible gap was whatever the tail
                      // happened to leave — about 1 mm on the right and less
                      // under the feet. Now the box IS the bird, so this
                      // number is the gap you actually see.
// microSD access — the card slides DOWNWARD out of its push-push socket (the
// purple-rectangle zone on the Rev1.2 board photo: right side, below center,
// in-use back view). The opening in the BACK PLATE covers the socket, the
// card's slide travel, and room for a fingertip to keep hold of the card so
// it never drops inside the case. Position corrected by the FIRST REAL PRINT:
// the photo-derived 42.0 sat 1/4" too far outboard — the print lined up with
// the socket 6.35 mm nearer the plate's center.
sd_dx = pnl_port_u(PANEL, "microSD");  // opening center, + = back-view right (42.0 − 6.35, measured)
sd_dy = pnl_port_v(PANEL, "microSD");

/* [Frame — microSD window] */
sd_w  = 18.0;   // width — fingertip-sized, not card-sized
sd_l  = 40.0;   // length along the slide direction

/* [Frame — radio window (FCC/IC marking)] */
// A plain rectangle straight through the plate over the ESP32-S3-WROOM-1's
// shield can, so the module's own printed grant numbers read on the finished
// case — the one opening in this design that would exist for PAPERWORK rather
// than for a person or for air.
//
// OFF. The back plate's rule is now that every hole in it is a hole something
// goes through: the card window, the four keyholes, and nothing else. A
// rectangle floating in the top-right corner is the one thing on this face
// that fails that rule, and it reads as a rectangle rather than as a reason.
//
// Turning it on is one word, and here is what that buys and costs, so it is a
// decision and not a rediscovery:
//   IT DOES let a module-level FCC/IC grant be cited the way the grant expects
//   — the marking legible on the exterior of the end product — instead of the
//   case burying it and the product needing its own label printed and applied.
//   IT DOES NOT make anything certified. An end product built on a certified
//   module still has its own obligations, and nothing in this repo has been
//   through a lab. A window is a window.
//   IT IS NOT MEASURED. The position comes from the PANEL RECORD (pnl_soc_*),
//   not from a number typed here — a property of the board, so a second board
//   is a second record — but that record's entry is scaled off the vendor
//   drawing rather than calipered, ±2 mm, which is why soc_grow is not tight.
//   A BATTERY BUILD CAN STILL TAKE IT AWAY (soc_bat_clash, below).
soc_win  = false;
soc_grow = 1.2;  // plate cut back beyond the can on every side. Two jobs: it
                 // is the tolerance on a ±2 mm position (see the panel
                 // record's own warning), and it keeps the cut off the can's
                 // edge so the window frames the marking instead of clipping
                 // it. Shrink it only after somebody has calipered the module.
soc_r    = 1.2;  // corner round. "Plain rectangle" is the brief and this still
                 // is one — 1.2 is under a nozzle's turn radius as a visible
                 // feature, and a truly square inside corner on a through-cut
                 // prints with a fillet the slicer chose anyway. Choosing it
                 // here means the drawing and the part agree.
soc_dx = pnl_has_soc(PANEL) ? pnl_soc_u(PANEL) : 0;
soc_dy = pnl_has_soc(PANEL) ? pnl_soc_v(PANEL) : 0;
soc_w  = pnl_has_soc(PANEL) ? pnl_soc_w(PANEL) + 2*soc_grow : 0;
soc_h  = pnl_has_soc(PANEL) ? pnl_soc_h(PANEL) + 2*soc_grow : 0;
// A window over a module the record cannot locate is a hole over whatever
// happens to be there, which is worse than no window at all.
assert(!soc_win || pnl_has_soc(PANEL),
       str("frame: soc_win is on but panel \"", pnl_id(PANEL), "\" has no ",
           "radio-can record (P_SOC). Add [u, v, w, h] in back-plate coords ",
           "to canary_panel_lib.scad, or set soc_win = false."));

/* [Frame — side gills & dock keys] */
// 7 per side, not 8. The r9.05 corner round eats 5.85 mm off each end of the
// side wall's flat, and the portrait dock needs a rib landing outboard of the
// last gill: at 8 gills that wanted 95.8 mm of flat against 92.7 mm available.
// Dropping one slot per side buys back 11 mm and leaves 7.9 mm of margin, at
// a cost of 12% of the side vent area (3.46 -> 3.02 cm2 across both walls).
// The back grille and the bottom-intake/top-exhaust path are untouched, and
// the row is re-centered (gill_y0) so both ends clear their rib equally.
gill_n  = 0;         // SIDE GILLS OFF — they printed ugly on the first case
                     // and the case is not short of air: the convection path
                     // is bottom-wall intake -> top-wall exhaust, and the back
                     // grille carries the rest. The PORTRAIT dock keys are
                     // their own slots "past the end of the gill row", so they
                     // survive this untouched — verified, not assumed.
                     // Was 7. Set it back if a build needs the side area.
gill_y0 = -33.0;     // carry vents only; SD access is through the back plate
gill_w = 2.4;  gill_l = 9.0;  gill_rake = 0;    // rake 0: vertical slots print
                     // cleanest; the raked look read as slashes and bought
                     // nothing thermally. gill_w still sizes the DOCK KEY
                     // slots (the stand's studs mate them — do not move);
                     // the visible side gills themselves are eggs:
gill_vw = 3.6;       // side-gill egg base width — area matches the old
                     // 2.4-wide pill (21.2 vs 20.4 mm2), upright when
                     // wall-mounted so a drip running down the wall parts
                     // around the opening. (gill_n = 0 today: the gills are
                     // off — they printed ugly and the back grille carries
                     // the ventilation. Kept sized so they can come back.)
frame_vent_flank_n = 4;  // exhaust slots per side, flanking the button window
// Dock keying — chamfered centering keys on the desk dock rise into gill-style
// slots the case walls carry, so the docked case self-centers and cannot
// slide sideways — the same doctrine as hanging the board on the panel's own
// standoffs: let the part's own features locate it.
//   landscape: the BOTTOM wall gets one keying slot each side at
//              ±dock_key_bx — outboard of the brand deboss words, over the
//              dock's cheek pads, whose studs rise into them. (They double
//              as two more intake slots on a wall-mount build.)
//   portrait:  the ±x walls get one keying slot each at dy = ±dock_key_dx,
//              past the end of the gill row (asserted), engaged by studs on
//              the dock's well ribs — which sit stand_rib_drop below the pad
//              plane so those studs clear the solid bottom wall in landscape.
dock_keys   = true;
dock_key_dx = 40.4;   // tracks stand_rib_x (asserted within half a rib) — both
                      // came in from 46.5/47.7 when the r9.05 corner shrank
                      // the side wall's flat span.
                      // portrait slots' ±dy on the side walls (past the gills;
                      // pulled inboard when glass_r 3.2 grew the corner —
                      // the slot must stay on the wall's flat span)
dock_key_bx = 84.0;   // landscape slots' ±dx on the bottom wall (on the pads)

/* [Frame — label rendering] */
label_depth = 0.5;
label_back_depth = 1.2;  // BACK-plate deboss depth (BOOT/RESET/SD/brand and
                     // the rail moats) — deliberately DEEPER than the
                     // frame_rim chamfer band. That is the two-color hook:
                     // the frame prints back-plate-down, so one filament
                     // swap at z = frame_rim prints the whole back skin and
                     // edge chamfer in an accent color while every deboss
                     // floor, sitting above the swap, prints in the body
                     // color — the words show through. Exact swap heights
                     // are echoed at render time.
label_font  = "Liberation Sans:style=Bold";

/* [Print colors — the AMS palette] */
// ════════════════════════════════════════════════════════════════════════════
//  PRINT COLORS — three filaments, and why it costs almost nothing
// ════════════════════════════════════════════════════════════════════════════
//
//  THE PALETTE
//    BODY   black   the case itself — every surface you touch
//    INK    white   the help QR's modules
//    ACCENT yellow  the MARK: the bird, and the three-row lockup under it
//                   (SECURACV / CANARY / ERRERlabs)
//
//  The accent is deliberately the BRAND and nothing else. A yellow case is a
//  toy; a black case with a yellow mark on it is a product. Everything
//  functional — BOOT/RESET, SD, the rating block — stays a plain deboss in
//  the body color, so the one thing wearing a second filament is the one
//  thing that is supposed to be looked at. It costs a few tool changes on
//  three layers.
//
//  (This block used to say "the SECURACV company line, and nothing else",
//  which stopped being true when the bird and the product name joined the
//  accent group, and was two edits stale by the time anyone read it. The
//  authority is `accent_groups` below; this paragraph describes it.)
//
//  WHY THIS IS CHEAP (the part that matters on a P2S)
//  Every AMS tool change purges filament, so the cost of a multi-color print
//  is set by HOW MANY LAYERS mix colors — not by how many colors you use.
//  This part is laid out so that almost none of them do. It prints
//  BACK-PLATE-DOWN, and in that orientation:
//
//    print z          what is there                        filaments in play
//    ───────────────────────────────────────────────────────────────────────
//    0 .. 1.2         back skin + every deboss floor       BODY + INK + ACCENT
//    1.2 .. 23.5      the shell. Nothing but wall.         BODY only
//    23.5 .. 24.1     the front bezel ring + edge chamfer  INK only
//
//  So the three-color work is confined to the first 1.2 mm of a 24 mm part —
//  about 6 layers at 0.2 — and the bezel is a single clean swap with no
//  in-layer changes at all. The other ~110 layers never change tools. That is
//  why the bezel is the whole RING rather than "top and bottom bands": the
//  front rim is a uniform 2 mm all the way round (outer 197.26 x 115.06 vs a
//  193.26 x 111.06 opening), so bands would not be a visible distinction —
//  they would just add tool changes to every one of those last layers.
//
//  ⚠️  QR POLARITY IS NOW A DELIBERATE TRADE, not a rule this file enforces.
//  The spec wants DARK modules on a LIGHT field. This build ships qr_style =
//  "bare": the modules print in INK straight onto the black plate, so the
//  symbol is WHITE ON BLACK — inverted — because a light plaque on a black
//  case is a bright rectangle that dominates the back and the symbol is
//  wanted to sit IN the case, not on it. That is an appearance decision made
//  with open eyes:
//    · many current phone cameras decode inverted symbols; many older and
//      embedded readers do not, and the standard does not require it.
//    · nothing here can test it. print part="coupon_qr_body"/"coupon_qr_fill"
//      and scan it with the phones you actually care about.
//    · the way back is one word: qr_style = "plaque".
//  Whichever style is chosen, assigning the modules their OWN filament is
//  what makes the symbol possible at all — on the single-extruder z-swap
//  recipe the module floors print in the BODY filament, so the modules and
//  the field cannot differ no matter what swap height is used.
//
//  ⚠️  THE MODULES ARE NOW ACCENT, AND THIS PARAGRAPH USED TO FORBID THAT.
//  It read: "the modules must stay INK — yellow against either body color is
//  far too low a contrast to decode." That was written when the body could be
//  WHITE, where yellow-on-white is hopeless and the warning was simply right.
//  The body is black now and the call was made deliberately for looks, so the
//  rule is restated rather than deleted, because the concern has not gone
//  away — it has only shrunk:
//    · RAL 1003 is luma 0.68; the old white ink was 0.95; the black field is
//      0.11. Contrast against the field drops by roughly a third.
//    · the polarity is UNCHANGED in kind — light modules on a dark field,
//      inverted, exactly as the white build was. This is not a new risk
//      category, it is more of the existing one.
//    · a WHITE body with accent modules remains genuinely unscannable. The
//      qr_dark_on_light computation below is derived from the two colors that
//      actually meet, so it says so on its own rather than relying on this
//      comment being read.
//  SCAN THE COUPON. At this pairing that is not a formality, and it is the
//  only instrument in the project that can settle it.
//
//  HOW TO RE-COLOR
//  Regrouping the palette is a one-word edit in back_graphics()'s `ink`
//  argument — move a label between the "text" and "mark" groups and it
//  changes filament. Nothing here is positional; no number has to be kept in
//  sync anywhere else, because each inlay is cut from the same solid as the
//  recess it fills.
//
//  PRINTING IT — Bambu Studio, P2S + AMS
//    1. Export the parts the ACTIVE palette actually uses. With the shipped
//       two-color default that is fil_body and fil_accent ONLY: ink_groups is
//       empty, so fil_ink is an empty object and OpenSCAD writes NO FILE for
//       it — following a three-part recipe here just sends you looking for a
//       file that was never created. The render-time echo names the live list,
//       so trust that over this comment if the palette has been edited.
//    2. Load fil_body, then right-click → Add part → Load, and add the rest.
//       They arrive already in position: every part is exported in the SAME
//       coordinate frame, so do NOT re-center or drop-to-bed any of them.
//    3. Assign a filament to each part.
//    4. Slice. Expect the purge tower to be short — see the table above.
//  Single-extruder fallback (no AMS): print part="frame" — the WHOLE part —
//  and use the z-swap recipe echoed at render time. Not fil_body: that is the
//  body's SHARE of the split, with the bezel band subtracted out of it because
//  that material belongs to fil_ink, so on its own it is a case with no front
//  bezel. The fallback gets you the black bezel and a body-color back skin,
//  but NOT the scannable QR — see the polarity note above.
//
//  ⚠️  TPU fitments are NEVER part of this. 90-95A must come off an EXTERNAL
//  spool: it buckles in the AMS's long PTFE path and jams the hub
//  (bambu_p2s_bringup.md §0). The AMS carries the case's three rigid
//  filaments and nothing else.
// ════════════════════════════════════════════════════════════════════════════
print_colors = true;   // build the per-filament parts and color the preview
// Names are what you load in the slicer; they appear in the render-time echo
// so the printed part and the recipe cannot disagree about what goes where.
pal_body   = "Black";
pal_ink    = "White";
pal_accent = "Signal Yellow";
// Preview RGB only — these never reach the mesh, they just make `frame_color`
// look like the real thing so a palette can be judged before it is printed.
//
// The accent tracks its NAME: RAL 1003 Signal Yellow is #F9A800, a warm amber
// gold. It used to be #FFC70A here, which is lighter and lemon-ier — so the
// preview had been quietly showing a different yellow from the one the recipe
// asks you to load, and a palette you cannot trust the preview of is the one
// thing this block exists to prevent.
//
// These are EYEBALLED, and it matters that you know that: a filament photo is
// white-balanced and saturation-boosted, and PETG extrudes slightly lighter
// and more translucent than it looks on the spool. The color coupon is the
// real instrument — print it, hold it next to the render, and set the numbers
// from the part rather than from a picture of a part.
pal_body_rgb   = [0.11, 0.11, 0.12];
pal_ink_rgb    = [0.95, 0.95, 0.93];
pal_accent_rgb = [0.976, 0.659, 0.000];   // RAL 1003 — #F9A800
// WHICH FILAMENT IS LIGHT. Asked, not assumed — the body was white and is now
// black, and the QR's polarity has to follow that or the symbol stops
// scanning. Rec. 709 luma; the comparison is all that matters, not the units.
function pal_lum(c) = 0.2126*c[0] + 0.7152*c[1] + 0.0722*c[2];
// (There was a body_is_dark here, body against INK, and the QR's polarity was
//  computed from it. It is gone rather than left sitting unused: with the ink
//  list empty it compared the body to a spool this palette does not load, and
//  a value that names the wrong pair is how the descriptions drifted in the
//  first place. Polarity now comes from qr_mod_rgb/qr_field_rgb below — the
//  two colors that actually meet.)

// Which back-plate groups take which filament. Edit these, not the geometry.
// A group in NEITHER list is still cut — it just gets no inlay, so it reads as
// a plain deboss in the body color. That is the "emboss, no color" setting,
// and it is the default for "text" (BOOT/RESET/SD and the rating block) and
// "moat" (the adhesive-rail outlines): on a black case those were two white
// rectangles and four lines of white type competing with the one word that is
// supposed to carry the accent.
//
// TWO COLORS, not three. Everything that is not the case is the accent: the
// bird, the lockup, and the QR. The ink list is deliberately EMPTY — a white
// QR next to a yellow mark made the back plate a three-way argument, and the
// symbol read as the loudest thing on a face whose subject is the bird.
// Leaving the list empty (rather than deleting the mechanism) keeps the
// three-filament build one edit away for anyone who wants it back.
ink_groups    = [];
accent_groups = ["mark", "bird", "qr"];

// Which filament a back-plate group actually lands on, and what that spool is
// called. Derived, because the QR's own description below used to name pal_ink
// in prose while the group lists decided the truth — move "qr" between the
// lists and the sentence would have gone on claiming the old spool.
function grp_fil(g) = search([g], accent_groups)[0] != []
                        ? "accent"
                        : (search([g], ink_groups)[0] != [] ? "ink" : "body");
function grp_pal(g) = let(f = grp_fil(g))
    f == "accent" ? pal_accent : (f == "ink" ? pal_ink : pal_body);
function grp_rgb(g) = let(f = grp_fil(g))
    f == "accent" ? pal_accent_rgb : (f == "ink" ? pal_ink_rgb : pal_body_rgb);

// These sit ABOVE the QR block on purpose: OpenSCAD hoists function
// DEFINITIONS but not variable ASSIGNMENTS, so qr_fill_rgb's call to grp_rgb()
// would resolve accent_groups to undef if the lists were declared after it.
// That is not a hypothetical — it warned "Ignoring unknown variable
// 'accent_groups'" and silently rendered the color parts empty.

// HOW THE QR IS BUILT — and it is now a LOOK decision, not only a scan one.
//   "plaque" lays an INK field over the plate and punches the modules THROUGH
//           it, so the modules read in the BODY color. On a dark body that
//           is dark-on-light: the classic, spec-conformant polarity.
//   "bare"  prints the modules themselves in INK straight onto the plate, so
//           the field IS the case. On a dark body that is light-on-dark —
//           INVERTED — and the symbol disappears into the case instead of
//           sitting in a bright rectangle on it.
//   "auto"  picks whichever of the two yields dark-on-light.
qr_style = "bare";   // ["auto","plaque","bare"] help-QR look
// The QR's non-body color is whichever filament the "qr" group takes — accent
// now that the plate is two colors, ink when it was three. DERIVED, because
// the polarity and every sentence about it used to assume ink: move the group
// and a yellow-on-black symbol would have gone on being described as white.
qr_fill_rgb  = grp_rgb("qr");
qr_plaque    = qr_style == "auto"
                 ? pal_lum(pal_body_rgb) < pal_lum(qr_fill_rgb)
                 : qr_style == "plaque";
// Plaque puts BODY in the modules and the fill in the field; bare puts the
// fill in the modules and leaves the case itself as the field.
qr_mod_rgb   = qr_plaque ? pal_body_rgb : qr_fill_rgb;
qr_field_rgb = qr_plaque ? qr_fill_rgb  : pal_body_rgb;
// Computed from the two colors that actually meet, so the warning below cannot
// drift from the geometry OR from the palette.
qr_dark_on_light = pal_lum(qr_mod_rgb) < pal_lum(qr_field_rgb);

// How deep the INK reaches in from the FRONT face. This is a visible-surface
// depth, not a structural one: 0.6 is three layers at 0.2 and matches the
// existing front-ring swap band exactly, so the AMS build and the
// single-extruder build put their color boundary in the same place.
bezel_ink_t = 0.6;
// (ink_groups / accent_groups — the back-plate group lists — live UP at the
//  palette, above the QR block: OpenSCAD hoists function definitions but not
//  variable assignments, and the QR's derived polarity has to read them.)
// The two surfaces that are NOT back-plate groups and so cannot be moved by
// the lists above — each picks a filament by name instead. "body" means the
// surface is simply not partitioned: no inlay is added and nothing is
// subtracted, so it prints in the case color with no tool change at all.
bezel_color     = "body";   // ["body","ink","accent"] front bezel ring
vent_ring_color = "body";   // ["body","ink","accent"] egg-vent mouth rings

/* [Frame — bottom USB port, edge brand, TPU fitments] */
usb_port   = true;   // pass-through for the power cable, centered on the bottom wall.
                     // Feed the head through BEFORE the panel goes in, plug it,
                     // leave a service loop, then wrap the grommet on the wire.
usb_dx     = 0.0;    // port center along the bottom wall — 0 = centered
usb_zc     = 11.0;   // port axis across the wall band (front face → back). The
                     // brand deboss words center on this same line.
usb_head_w = 13.0;   // widest overmold head that must PASS — MEASURE your cable
usb_head_h = 7.0;    // overmold head thickness — MEASURE
usb_pass_c = 0.3;    // per-side clearance around the head through the opening
usb_wire_d = 3.8;    // cable jacket Ø the grommet grips — MEASURE
grom_lip   = 2.0;    // grommet flange width per side (both flanges)
grom_bell  = 3.2;    // strain-relief bell beyond the outer flange
// SIDE EXIT — the same pass-through cut into a short wall, so the cable can
// leave sideways instead of down. It is deliberately the SAME stadium as the
// bottom port: one grommet part serves either exit, and whichever exit you
// do not use takes plug_port (the same body, no bore) so the case never sits
// with an open hole. Which side is right depends on where your outlet is;
// in the desk dock the bottom exit routes through the dock's own channel,
// while the side exit clears the dock entirely — that is the case for having
// both. The dock's PORTRAIT pose already exits sideways, so a side-exit case
// docks portrait with no cable bend at all.
side_exit = "right";  // ["none","left","right"] — extra cable exit wall.
                      // ON by default at "right" (+x, back-view right): that
                      // is the wall the microSD is closer to (sd_dx is
                      // positive), and hung PORTRAIT with that wall down it
                      // IS the bottom — the cable leaves straight down with
                      // no bend, leash anchor and peel scoop included (the
                      // port_cut module carries all three to whichever wall).
                      // The first print turned this off because an open hole
                      // on a connector-less wall read as a port that does not
                      // exist — the answer to that is the leashed BLANK
                      // (plug_port fills whichever exit the cable does not
                      // use; the case never sits with an open hole), not
                      // giving up the portrait hang. "none" if you truly
                      // never hang portrait.
side_dy   = 0.0;      // its center along that wall, + = toward the top edge.
                      // 0 = centered: clear of the ±dock_key_dx keying slots
                      // (asserted) and over the dock well's open span when
                      // docked portrait, so the exit works there too.

/* [Port — labels & rating block] */
// Port labels: which opening is which, embossed on the outer skin beside it
// (deboss floors read in the body color through the accent skin, so on a
// two-color print the words come out colored for free — no extra swap).
port_labels = true;
// A label names what is BEHIND the wall, not what the cable happens to be.
// The bottom exit sits on the panel's real USB-C, so "USB" there is true and
// the assert below holds it to the panel record. The side exit is a cable
// pass-through aligned to no connector at all — it said "USB" on the first
// print, beside a wall with nothing behind it, and it read as a socket
// someone could plug into. A word molded into plastic is not a caption you
// can revise later; leave a pass-through unlabeled.
//
// The earlier reasoning here was that both carry the same cable so both may
// say USB. That is true about the CABLE and wrong about the WALL, which is
// what a person standing in front of the case is actually reading.
// OFF. It said "USB" and it was never beside the port: the code places it at
// vword_x + vword_half + 6.0, which is outboard of the BRAND WORDS, so on the
// real wall it landed ~30 mm from the opening and past SECURACV — reading as a
// third brand word near the corner rather than as a caption for anything. The
// comment above it claimed "just outboard of each opening's flange", which is
// what it should have done and never did.
//
// Not relocated, removed. A USB-C hole in the bottom of a device does not need
// the word USB beside it, the rating block two rows up already says USB-C
// INPUT, and a word molded into plastic is not a caption you can revise
// later. Set it back to "USB" if a future wall genuinely needs disambiguating
// — and fix the placement formula at the same time.
port_lbl_a  = "";       // beside the bottom exit — see above
port_lbl_b  = "";       // beside the side exit — a hole, so it says nothing
// RATING STAMP — the little spec block every mains-adjacent thing should
// carry. It goes on the BACK plate's lower band: hidden behind the case on a
// wall mount, hidden behind the dock's fin when docked, and right there when
// you pick the case up — which is exactly when you want it. Deboss, so it
// never wears off and never needs a sticker.
rating_stamp = true;
// The maker line's YEAR is taken from the build stamp, not typed. Both end up
// molded into the same part, so a hand-typed year is a second thing that can
// disagree with the first — and it would go stale on 1 January without anyone
// touching the file. lcd7_stamp_rev() is CalVer ("2026.09f"), generated, and
// already the thing this design is named by; the year is just its first four
// characters. Bump the design, the molded year follows.
function lcd7_stamp_year() = let (r = lcd7_stamp_rev())
    str(r[0], r[1], r[2], r[3]);
// Electrical facts only. The maker line lives with the lockup now — see
// brand_maker — so this block is what a person checks before plugging the
// thing in, and every row of it answers that question.
rating_lines = ["5V = 2A", "USB-C INPUT", "INDOOR USE ONLY"];

/* [TPU fitments (grommet, plugs, SD cover)] */
// TPU fit system — these two do for the TPU parts what tol_slide/tol_hole do
// for the rigid ones. TPU seats by squeeze, so its knobs are interferences,
// not clearances.
tpu_squeeze = 0.2;   // per-side interference each TPU waist carries in its opening
tpu_grip    = 0.4;   // how much smaller the grommet bore is than the cable jacket
// BOOT/RESET plug — the buttons are pressed THROUGH the installed plug: a
// TPU TOWER under each face dimple runs from the plug body down to the
// button cap and carries a fingertip press across the gap. The 2 mm pips
// these towers replace assumed the caps sat just behind the wall; measuring
// the fitted case put them ~5 mm further — at the BOARD EDGE, exactly where
// the panel record says (BOOT/RESET carry d = 0: flush with the PCB
// outline). So the reach is now DERIVED from that record and the towers
// land ON the caps: resting contact, one soft press through the dimple,
// still one part.
btn_reach  = 0;      // 0 = AUTO: tower reach derived from the panel record
                     // (outer skin → pcb edge — see btn_tower_len). Only set
                     // a measured value (lip plane → button cap, mm) if YOUR
                     // board's caps sit off the record.
btn_pip_d  = 5.0;    // tower shaft Ø (face dimples mark them outside). The
                     // tip steps -1.2 to land inside the 4 mm caps; the root
                     // flares +2.4 into the plug body so the long column
                     // presses instead of folding at its root.
btn_pip_dz = 0.0;    // tower center offset across the wall band, from the window center
// PLUG LEASH — the same strap + arrowhead barb as the SD cover and the port
// fitments (one barb spec: one thumb-push feel, one hole size — see the SD
// tether block below for the spec's reasoning). The plug pops out for tower
// checks and services, and a popped plug with no leash is the part you drop
// behind the desk. The strap lies FLUSH in a shallow channel cut into the
// top wall's outer skin; the barb pushes IN through an anchor hole beside
// the window and mushrooms in the open cavity behind the wall. The anchor
// takes the station of the innermost -x exhaust slot — the flank-vent loop
// drops any slot the anchor needs (one, at stock dims), so the leash costs
// exactly one exhaust slot and the echo says so.
plug_tether  = true;
plug_teth_dx = -21.5;  // anchor center from the window center — the innermost
                       // -x flank slot's station (btn_w/2 + 9). Negative =
                       // the -x side, matching the SD leash's "anchor at the
                       // quiet end" doctrine (BOOT sits that side; the scoop-
                       // free end of the plug is the peel end).
plug_chan_d  = 1.2;    // strap channel depth into the frame_wall skin. 1.2,
                       // not the SD channel's 1.4: the wall is 2.0 where the
                       // plate was 3.0, and the bottom-edge deboss doctrine
                       // (assert below) wants >= 0.8 of web left standing.
                       // The 1.2 strap rides flush instead of 0.2 recessed —
                       // nothing ever presses on the TOP wall's skin.
// SD cover — peel scoop at the card (fingertip) end, TETHER at the other.
sd_lip    = 1.2;     // countersunk rim: 45° reach AND depth around the opening.
                     // The flush flat-floored recess this replaces left a
                     // cantilevered ring hanging over the opening — unprintable
                     // back-plate-down (drooped on the first real print). A 45°
                     // countersink prints clean and self-centers the cap.
// The cover stays on a LEASH: a flat strap off the cap's top end carries an
// ARROWHEAD BARB that pushes through a small anchor hole in the plate and
// mushrooms on the inside — peel the cover and it dangles, captive; the
// barb's 45° cam faces free it only under a deliberate yank (service), never
// a dangle. This replaces the v0.5 hinge tongue, which only HOOKED under the
// plate edge — a full peel could slide it out and lose the cover — and whose
// anti-push-in job the countersink already does (the cap's taper seats on
// the rim: it cannot pass inward). The strap lies in a shallow channel cut
// below the outer skin, so a wall or mounting strip never pinches it, and
// the anchor hole is what the FRAME must carry — print the case AFTER this
// change or the barb has nowhere to go (gated: sd_tether_hole/sd_tether_barb).
sd_tether    = true;
sd_teth_gap  = 3.5;  // countersink rim outer edge → anchor hole center
sd_teth_hole = 3.2;  // anchor hole Ø through the plate (the Ø2.8 shaft rides
                     // loose in it; the Ø4.6 arrowhead squeezes through once)
sd_teth_head = 4.6;  // arrowhead Ø — 1.4x the hole: firm thumb-push in, stays
                     // put dangling, yanks free for service
// PORT LEASH — the same leash, on the port fitments. A grommet or a blank
// that is only friction-held is a part you drop behind the desk the first
// time you service the cable, so both carry a strap and the same arrowhead
// barb, anchored beside their own port. Deliberately the SAME hole and head
// as the SD cover's: one barb spec means one thumb-push feel, one hole size
// to print, and a fitment that fits either port's anchor.
//   Material: TPU 90-95A, EXTERNAL SPOOL — never the AMS. The strap is the
//   reason: 1.2 mm of 90-95A through the AMS's long PTFE path buckles and
//   jams the hub (see bambu_p2s_bringup.md §0). Only "TPU for AMS" 68D
//   feeds through, and that is too stiff to be a leash.
port_tether  = true;
port_teth_dx = -12.6;  // anchor hole center along the wall, from the port
                       // center — asserted clear of the brand words below
port_teth_cb = 1.4;    // counterbore depth at the anchor's OUTER mouth. The

/* [Port — cable scoop] */
                       // barb's mushroom has to flare INSIDE this pocket: the
                       // shaft stops at its floor, so the head seats flush
                       // with the skin instead of standing proud of the face
                       // you look at. It stood 0.6..2.8 mm proud before —
                       // the shaft ran 1.0 mm PAST the wall and the whole
                       // mushroom flared in open air, with this counterbore
                       // sitting uselessly behind it. Gated by
                       // port_barb_proud, which must render empty.
                       // Leaves frame_wall - port_teth_cb of full-diameter
                       // land for the shaft, so keep it under frame_wall.
// PEEL SCOOP — a fingernail dish in the skin just outboard of the grommet's
// flange, so the grommet and the blank can be got back out of a port you
// opened to service a cable. The SD cover has had one since v0.6; the port
// fitments had nothing, and a flush-ish TPU part in a 2 mm wall is not
// something fingers alone win against. Same sphere cut as the SD mouth's.
// It sits on the side AWAY from the leash anchor, so the nail peels the
// fitment off its strap rather than against it — and that is the only side
// with room: the anchor leaves 2.2 mm on its own side, this one has ~12.
port_scoop    = true;
port_scoop_dx = 11.0;  // scoop center from the port center — just past the
                       // flange edge at usb_open_w/2 + grom_lip = 8.8, so
                       // the dish undercuts the flange's rim by ~1.2 mm
port_scoop_r  = 7.8;   // sphere radius / stand-off: r - port_scoop_d sets the
port_scoop_d  = 7.0;   // dish depth (0.8 mm), sqrt(r^2 - d^2) its footprint

/* [Edge brand & bottom intake vents] */
// Bottom-edge brand — CRISP DEBOSS into the wall's outer skin. v0.6: the
// slat-stencil vents this replaces cut the letters THROUGH the wall, which
// forced tie bands across every glyph (or the counters fall out) — and on
// the first print those ties read as horizontal scan lines. A deboss stays
// attached everywhere by the wall web behind it: no ties, no lines, clean
// letters. The intake the stencil carried moves to a SHADOW GILL row in
// the wall band's last few mm before the back plate — invisible against a
// wall or over the dock's well, roughly the stencil's open area, still
// feeding the same bottom-in → top-out convection path.
// With an AMS: Color Painting → Smart Fill floods each deboss floor with
// an accent filament in one click — see bambu_p2s_bringup.md §7b′.
edge_text_l = "SECURACV";  // front-view left of the port
edge_text_r = "CANARY";    // front-view right
edge_text_size = 7.0;
edge_lbl_depth = 1.0;   // deboss depth into the frame_wall skin (web asserted)
edge_vent_n = 16;       // shadow gills: count / pitch / pill w x h / band
edge_vent_pitch = 8.0;
edge_vent_w = 2.6;
edge_vent_h = 3.0;
edge_vent_z = 18.7;     // band center across the wall — behind the grommet's
                        // outer flange, shy of the back plate (both asserted)
vent_gap  = 8.0;     // clear space between the port flange and each word

/* [Stand] */
// desk dock for the FRAME case (see the header). The slot is sized
// to the frame's DERIVED outer depth, so editing the frame's stack re-sizes
// the dock with it. A reference frame STL measures 23.5 mm against the
// derived 24.0 — stand_clear covers both without rattle.
opt_stand = true;
stand_ang     = 20;    // recline from vertical
stand_w       = 174.0; // dock width — DELIBERATELY narrower than the case, so
                       // the side-wall gills stay in clear air and the case
                       // lifts straight out by its overhanging ends
stand_d       = 126.0; // base plate depth — sized so the base reaches well
                       // behind the reclined case's center line in PORTRAIT
                       // too (a 197 mm slab on its side), not just landscape;
                       // both orientations are tip-checked by the asserts
stand_plate_t = 6.0;   // base plate thickness
stand_cheek_t = 16.0;  // side cheek thickness; the cheeks' tilted tops ARE the
                       // seat pads the case rests on in landscape
stand_lip_h   = 9.0;   // front lip capture up the case face — must stay under
                       // the window's bottom border in BOTH orientations
                       // (asserted; landscape border 12.6, portrait 20.6)
stand_lip_t   = 6.0;   // front lip blade thickness
stand_fin_h   = 78.0;  // back-fin support height along the case back (68 % of
                       // the landscape height, 40 % of portrait; stays under
                       // the landscape keyholes at 91.5)
stand_fin_t   = 8.0;   // back fin blade thickness
stand_gusset_h = 52.0; // cheek back edges buttress the fin up to this height
stand_rib_x   = 40.4;  // moved in from 47.7 with the r9.05 corner: the wall's
                       // flat half-span fell 52.18 -> 46.33, so the old
                       // station sat ON the round. 40.4 is the middle of what
                       // is left between the gill row and the corner — the
                       // asserts below bound both ends, and they are the
                       // thing that caught this rather than a print.
                       // ± KEYED RIBS: blades across the well that carry the
stand_rib_w   = 8.0;   // case in PORTRAIT (its 115 mm width misses the cheeks
                       // entirely) and carry the PORTRAIT centering keys on
                       // top. LOAD PATH, precisely — a blade CROSSES the
                       // case's full depth, while the keying slot it rises
                       // into is only a gill_w-wide band at the wall's
                       // mid-depth: the rib bears full-width solid wall on
                       // both sides of that NARROW band (~21 of 23.5 mm of
                       // depth), so the case cannot tilt and the key stud
                       // never carries weight. Along the slot's LONG axis
                       // the rib sits mostly within the slot's span by
                       // construction — the gill row pins the rib's inner
                       // edge, so both-sides bearing IN the band was never
                       // available at any rib width; it is not the load
                       // path and not required. Clears the gill row and the
                       // wall's corner flat (both asserted).
stand_rib_drop = 2.0;  // rib tops sit this far below the pad plane, so the
                       // rib keys (1.5 proud) clear the LANDSCAPE case's
                       // solid bottom wall — in landscape the case rests on
                       // the cheek pads and their ±dock_key_bx studs center
                       // it; portrait simply seats this much lower (the lip
                       // margin pays for it — asserted)
stand_floor_h = 26.0;  // seat height: case bottom edge -> desk. This is the
                       // headroom for the USB power plug leaving the hole in
                       // the case's bottom wall — a straight plug + strain
                       // relief needs ~20 mm before the cable can bend away
stand_slot_y  = 32.0;  // seat centerline, measured from the plate's front edge
stand_clear   = 0.5;   // per-face case<->slot clearance (drop-in, not press)
stand_cable_w = 16.0;  // desk-level cable channel width (through plate + fin foot)
stand_feet    = true;  // 4x shallow recesses for adhesive rubber feet
barb_vents = true;     // shape the fin vents as BARBS (a pointed vesica)
                       // instead of plain stadium pills — self-supporting at
                       // the apex on this near-vertical fin. false restores
                       // the pills.
                       //
                       // These are NOT the house mark and are not meant to
                       // be. The hatchery egg is a PROPORTION (~1.4:1, the
                       // grille's 7.2 x 5.2) as much as a tip ratio; a fin
                       // vent is 52 x 16, over 3:1, and the same ratio there
                       // reads as a tapered slot, not an egg. Nor can it be
                       // broken into a column of small eggs: the +/-|sd_dx|
                       // pair sits ON the microSD opening so the card stays
                       // reachable while docked, and webs across it would
                       // trap the card. Structural opening, structural shape.

include <canary_s3_lcd7_qr.scad>   // qr_url() / qr_bits() — generated, committed
qr_n = len(qr_bits());             // symbol size — defined HERE, above every use

/* [Help QR — dock deck] */
// A scannable help code debossed into the flat deck BEHIND the fin, on the
// right wing of the cable channel — the one place on either part with a big
// bare plane, zero vent cost, and a sight line while the dock sits on the
// desk. The bit matrix lives in canary_s3_lcd7_qr.scad, GENERATED by
// gen_qr.py (change the content there, rerun it, commit both). The bare
// deck around the field IS the spec quiet zone, so nothing else may be
// debossed or cut inside it (asserted below).
// Module cells overlap by qr_bleed instead of meeting face-to-face: two
// cutter squares sharing an exact edge union badly. That is only half the
// problem, and the smaller half — DIAGONAL neighbors meet at a single
// POINT, which no bleed can fix and no nozzle can trace. Both are handled
// in qr_field2d by a morphological opening; this knob is the union overlap
// that feeds it. See that module for the decode tests.
qr_bleed = 0.02;
qr_help = true;   // deboss the help QR into the deck
qr_cell = 1.6;    // module size — 4 line-widths at a 0.4 mm nozzle
qr_dx   = 43.5;   // field center, plate coords (+x = front-view right wing)
qr_dy   = 39.0;   // field center, +y = toward the back edge (a battery
                  // dock's deck grows rearward — the field rides along,
                  // see qr_dy_eff)
// ...and the same code on the CASE BACK, for wall-mounted cases that never
// meet the dock: debossed into the outer skin exactly like BOOT/RESET and
// the brand line (floors at label_back_depth read in the body color
// through the accent back skin — the existing single-swap recipe, zero
// extra waste), and printed as the FIRST layers on the textured plate, so
// the modules come out crisp. Placed on the plate's left band as the SD
// opening's visual counterweight, outboard of the left rail's moat; the
// smooth keepout it claims from the grille IS its quiet zone.
// ⚠️ POLARITY IS A SCANNING REQUIREMENT, not a taste. A reader needs DARK
// modules on a LIGHT field and refuses the inverse (verified: invert this
// symbol and cv2 will not read it at any size). The modules here are the
// deboss FLOORS, so the floor color must be the DARK one:
//   two-color  — floors print in the BODY filament, skin in the ACCENT, so
//                 the BODY must be darker than the ACCENT. The house pairing
//                 (white body + canary-yellow accent) is BACKWARDS for this
//                 and gives white-on-yellow: unscannable, and every other
//                 back label is nearly invisible too. Use a dark body
//                 (graphite/black) with a light skin.
//   one color  — the 1.2 deep floors read dark by SHADOW alone, which does
//                 scan in decent light; it is the two-color build that can
//                 silently come out inverted.
// The module SHAPE is safe either way: the opening in qr_field2d was decode-
// tested at 0.22/0.30/0.40 rounding and reads at all three.
// ⚠️  OFF, and this is the one thing in this revision that COSTS you something.
// The symbol plus its quiet zone is a 37.7 mm square — after the card window's
// 40 mm it is the second-largest object on the plate, and the two of them plus
// a mark worth looking at do not fit on a 197 x 115 field that also carries
// four keyholes and four boss pockets. Every arrangement tried put something
// within 2 mm of something else, which is not a canvas, it is a packing
// problem with a bird in it.
//
// So the plate carries the help URL as TYPE instead (help_line, below the spec
// block). You read it and type it rather than scanning it — worse, and
// honestly worse, not "equivalent". What is unaffected: the DOCK still carries
// a scannable symbol, and so do the coupons, so the QR is still proven and
// still reachable — just not molded into the hero face.
//
// Turning it back on is one word, and the asserts will then tell you exactly
// what it collides with rather than letting it overlap the mark. If you want
// both, the mark has to come down to about 45 mm and the composition changes.
qr_back      = true;   // deboss the help QR into the back PLATE
// ⚠️ WHAT CHANGED, since the paragraph above says this does not fit: THE
// GRILLE LEFT. Sixty eggs used to cover the plate's left wing, so the symbol
// and the mark were competing for the same field. With plate_grille off, the
// wing outboard of the badge is bare plate, and 37.7 mm of bare plate is
// exactly what the symbol needs. The mark did not have to come down to 45 — it
// stayed at 70 and moved 5.5 mm right instead. The asserts below are the proof,
// and they are within a millimeter of firing, which is the honest description
// of this layout: it fits, with nothing to spare.
//
// IT IS NOT ABOVE THE CARD WINDOW, which is where it was asked to go, and the
// reason is arithmetic rather than composition. The symbol plus its quiet zone
// is 37.7 mm tall. Above the card window the clear run is bounded below by the
// socket (the window's top edge cannot come down past it — the card has to be
// grippable) and above by the radio window, which is over the module and
// cannot move at all. That run is 31.8 mm. The only ways to close a 5.9 mm gap
// there are to drop the module size to ~1.1 mm, which is under what this
// printer resolves and would ship a symbol that photographs fine and does not
// scan, or to window only the top half of the radio can and gamble that the
// grant numbers are in that half. Neither is worth a nicer arrangement, so the
// symbol took the empty wing and the card window kept its column.
// ...but the SCAN COUPON still carries one, and that distinction is the whole
// point of this line. `qr_back` answers "does the finished plate wear the
// symbol"; the coupon answers "would a symbol printed by THIS machine, at
// THIS cell size, in THIS polarity, actually scan" — which is the question you
// have to answer BEFORE you can decide the first one. Gating the coupon on the
// plate's setting inverts that: it makes the test unavailable exactly when you
// need it, and it does so silently, because gen_3mf.py drops a volume that
// renders empty and packages a blank body-colored plaque instead.
// (That is not hypothetical — it is what this file did for one commit, and
// the coupon's second volume exported nothing at all.)
//
// The second volume is `coupon_qr_fill`, not `coupon_qr_ink`: it follows
// whichever filament the "qr" group takes rather than naming a spool. Both
// names have to be listed here or nowhere — a part missing from this test
// draws no symbol, renders empty, and trips gen_3mf.py's REQUIRE_ALL, which
// is the same failure this line exists to prevent, one rename later.
qr_coupon = part == "coupon_qr_body" || part == "coupon_qr_fill";
qr_draw   = qr_back || qr_coupon;
qr_back_cell = 1.3;    // module size — 3.1 line-widths at 0.42, up from 1.2
                       // (2.9): printable in theory at 1.2, marginal in fact,
                       // and this symbol is the one read off a wall. 1.34 is
                       // the ceiling — past it the quiet zone cannot clear
                       // BOTH the rail moat and the portrait keyhole keepout,
                       // and the asserts below say so
qr_back_dx   = -75.4;  // field center, back-view coords (+x = back-view right).
                       // THE OUTER left wing, not the inner one. The old -41.3
                       // was the middle of a band the grille left free; with
                       // the grille gone the constraint is the pair of real
                       // objects on either side, the rim chamfer outboard and
                       // the mark inboard, and the symbol is centered between
                       // them (asserted both ways, ~1.9 mm each side)
qr_back_dy   = 0.0;    // mid-LEFT wing, mirroring the SD cover's mass on the
                       // right. Deliberately NOT dropped to the SD's row:
                       // that squeezes the bottom band, and the band is the
                       // brand lockup's — one centered thing, nothing beside it
qr_back_reach = qr_n*qr_back_cell/2 + 4*qr_back_cell;   // field/2 + quiet zone,
                  // sized from the GENERATED matrix — a bigger symbol grows it

/* [Build stamp — inside the case, on the plate's inner face] */
// The case carries its own revision where it cannot spoil the outside:
// debossed into the INNER face of the back plate, found only when someone
// opens it. REV is human CalVer; SRC is a digest of the geometry sources,
// both from canary_s3_lcd7_stamp.scad (generated — see gen_stamp.py for
// why neither can rot, and for what SRC does and does not prove: it names
// a DESIGN, it authenticates nothing).
// Prints as the last layers of the plate, opening upward into the cavity —
// no bridge, no support. Mirrored in x because this face is read from the
// cavity side.
stamp_show  = true;
stamp_depth = 0.5;   // of back_t; the plate keeps 2.5 under it
stamp_dy    = 49.0;  // the clear band above the grille. Its floor is not a
                     // constant: the top egg row's center is at
                     // (vent_rows-1)/2*vent_pitch_y and the egg reaches
                     // half its length past that, so the assert below derives
                     // the bound instead of quoting a number that would rot
stamp_size  = 2.4;

/* [Battery bay — click-in, no hardware] */
// A molded bay on the back plate's inner face for the MakerFocus 1S packs
// (PH2.0 lead to the panel's battery header). The pack lies against the
// PLATE — the cool side, away from the backlight — held bat_rib off it on
// rails so the grille slots directly beneath become its own convection
// channel, with bat_air of open air on the board side: air on BOTH faces.
// Retention is printed, not bought: a low curb boxes it in XY (cut back at
// the four boss corners, opened at the short ends for the lead) and one
// cantilever finger per long edge clicks over the pack — wedge-faced, so
// the vendor's ±2 mm thickness stays preloaded against the rails and the
// pack cannot shift in ANY case orientation. Selecting a pack DEEPENS the
// case by exactly what the stack needs (echoed); print the desk dock with
// the SAME battery setting and its slot follows automatically.
battery  = "none";  // ["none","3000","10000"]
// MakerFocus nominals — 10000: model 9065115, 112-115 x 65 x 8.8-9.0;
// 3000: the 1S 3C pack, 65 x 35 x 10. Both quoted ±2 — MEASURE YOURS.
bat_dims = battery == "10000" ? [115, 65, 9.0] : [65, 35, 10.0];
bat_tol  = 2.0;   // the vendor's stated thickness tolerance, held as margin
bat_over = 3.0;   // tallest PCB component under the bay footprint — MEASURE
bat_air  = 2.0;   // battery ↔ component air gap (the board-side channel)
bat_rib  = 1.2;   // battery ↔ plate air channel (the grille-side channel)
bat_clr  = 1.0;   // per-side XY pocket clearance


/* [Quality] */
$fa = 3; $fs = 0.5;

/* [Hidden] */
// ↑ EVERYTHING BELOW THIS LINE IS HIDDEN FROM THE CUSTOMIZER, deliberately.
// "[Hidden]" is OpenSCAD's own mechanism, not a naming convention: variables
// under it are not offered as knobs. Without it, every derived value in the
// rest of this file — and there are ~68 of them — piles into whichever group
// happened to be declared last, which was [Quality]. A panel of 68 computed
// intermediates labeled "Quality" is not a settings page, it is a place
// people give up. If you add a REAL knob, put it in a group ABOVE this line.

// ----------------------------------------------------------------------------
//  Derived. Axis: X = width, Y = height, Z = toward glass.
// ----------------------------------------------------------------------------
// TWO pockets, not one. The tray cavity takes whichever is bigger, the glass or
// the board, so an overhanging PCB still drops in. The bezel's pocket is always
// sized to the GLASS — a shared cavity sized to a taller PCB would leave the
// slab centimeters of lateral play, and the lip is flat: it retains the glass
// axially but cannot center it, so the panel could slide until the window
// crossed the active area. The glass pocket lives in the glass band, above the
// PCB plane, so it never fouls the board however big the board is.
xc = max(glass_w, pcb_w) + 2*tol_slide;
yc = max(glass_h, pcb_h) + 2*tol_slide;
xg = glass_w + 2*tol_slide;   // bezel glass pocket — always the slab
yg = glass_h + 2*tol_slide;
xo = xc + 2*wall;             yo = yc + 2*wall;             // outer
// Depths, per the stack-up in the header: cav_d is everything BELOW the glass
// back, bez_h is the glass and the face above it. glass_t belongs to exactly
// one of them — counting it twice is what left v0.1's lip clamping air.
cav_d = comp_h + pcb_t + pcb_standoff;   // tray floor top → glass back (= wall top)
bez_h = face_t + glass_t;                // bezel outer face → glass back
r_in  = max(1.0, r_out - wall);
r_cav = min(r_in, glass_r);   // cavity corners can't out-round the slab

z_floor     = back_t;                 // tray floor top face
z_pcb_under = back_t + comp_h;        // PCB underside — top of the connector band
z_pcb_top   = z_pcb_under + pcb_t;
z_glass     = back_t + cav_d;         // glass back = tray wall top
view_w = aa_w + 2*win_margin;  view_h = aa_h + 2*win_margin;

// Port band: exactly the space the rear-side connectors live in.
port_h = comp_h;
port_z = z_floor + comp_h/2;
// Wall vents: the clear air column between the floor and the glass. The board
// is narrower and shorter than the cavity, so slots in the walls open beside
// it rather than into its edge.
wall_vent_h = cav_d - 3;
wall_vent_z = z_floor + cav_d/2;
vent_span_x = xc - 40;                            // keep clear of the corner ears
side_vent_y0 = (opt_side_ports ? side_open_dy + side_open_h/2 : 0) + 5;

// Actual lip overlap onto the glass border, per edge. aa_dy shifts the window,
// so the top and bottom borders differ — the tightest one is what matters.
lip_x   = (glass_w - view_w)/2;
lip_top = (glass_h - view_h)/2 - aa_dy;
lip_bot = (glass_h - view_h)/2 + aa_dy;
lip_min = min(lip_x, lip_top, lip_bot);

// Frame (one-piece) derived. NOTE the frame is modeled PRINT-SIDE: z0 = the
// front face on the build plate, +z toward the back — so viewed from the BACK
// +x is right (from the front, +x is left). Board-relative x features
// therefore use -m3_ox where the two-part tray uses +m3_ox.
fr_xi = glass_w + 2*frame_reveal;  fr_yi = glass_h + 2*frame_reveal;
fr_ri = glass_r + frame_reveal;    // opening corners track the slab
fr_xo = fr_xi + 2*frame_wall;      fr_yo = fr_yi + 2*frame_wall;
fr_ro = fr_ri + frame_wall;
// TWO front datums, one panel: the LEDGE chains from the bare-glass BORDER
// (glass_edge_t — the strips bond glass to ledge across adh_t), while the
// REAR stack chains from the full module thickness (glass_t) because the PCB
// hangs from the panel on its own standoffs. adh_t only sets where the ledge
// face sits, and the adhesive fills the gap between glass back and ledge.
// (Chaining the bosses from the ledge instead would open a screw gap equal
// to adh_t, or peel the adhesive closing it.)
// (both offset by glass_guard: the guard rim pushes the whole panel stack
// that far behind the front face, without moving anything relative to it)
ledge_z  = glass_guard + glass_edge_t + adh_t;  // ledge front face
bat_on  = battery != "none";
bat_l   = bat_dims[0];  bat_w = bat_dims[1];
bat_t   = bat_dims[2] + bat_tol;               // worst-case thick pack

// ── THE RADIO WINDOW vs THE BATTERY BAY ────────────────────────────────────
// A BATTERY BUILD PUTS A LITHIUM PACK BEHIND THAT WINDOW. The bay is centered
// on the plate and the 10000 pack is 115 x 65, which reaches the window's
// corner; the pack lies against the plate on its rails, so the window would
// look straight at a cell in a case whose whole point is that it hangs on a
// wall. The 3000 pack (65 x 35) clears it — by 1.2 mm, which is why this is
// computed rather than remembered.
//
// It DROPS THE WINDOW rather than asserting. An assert here reads as the
// careful choice and is the wrong one: it makes `battery = "10000"` a
// selection that cannot produce geometry, which is the exact failure this
// project deleted the -7B record over — "a choice that can only fail is worse
// than no choice", and the operator experiences it as the model breaking when
// they changed something unrelated. Nothing is silent about it: the echo says
// the window was dropped, says why, and says what to do instead.
// Declared here and not up with soc_win because it needs bat_l/bat_w, and
// OpenSCAD reads assignments in order.
soc_bat_clash = soc_win && bat_on
    && max(abs(soc_dx) - soc_w/2 - (bat_l/2 + bat_clr),
           abs(soc_dy) - soc_h/2 - (bat_w/2 + bat_clr)) <= 0;
soc_cut = soc_win && !soc_bat_clash;   // what actually gets cut
fr_depth0 = glass_guard + glass_t + pcb_standoff + pcb_t + standoff_len
          + frame_boss_h + back_t;
// The battery stack, front face -> plate inner face: component ceiling
// under the bay, board-side air, the pack itself, the rail channel. The
// case deepens only by what that stack needs beyond the stock cavity.
bat_extra = bat_on
    ? max(0, glass_guard + glass_t + pcb_standoff + pcb_t + bat_over
             + bat_air + bat_t + bat_rib - (fr_depth0 - back_t))
    : 0;
fr_depth = fr_depth0 + bat_extra;
// The wall-band knobs usb_zc / edge_vent_z are measured from the GLASS face
// (= the front face when glass_guard is 0); these are their absolute z.
usb_z    = glass_guard + usb_zc;
edge_vz  = glass_guard + edge_vent_z;
// The boss FACES live at the PANEL's datum — the standoff tips — which a
// battery case must NOT move: only the towers lengthen to reach the deeper
// plate. (Chaining fz_boss off fr_depth would leave the screws clamping
// bat_extra of air.)
fz_boss  = fr_depth0 - back_t - frame_boss_h;  // boss face the standoffs land on
// Depth (from the FRONT face) of the wall gills and the dock's keying slots.
// The dock aims its key studs at this same number — see stand_keystud. It is
// pinned to the PANEL datum, so it does NOT move when a battery deepens the
// case; the studs must therefore track the case's depth in the SLOT, which is
// the whole reason this is a shared derived value and not two constants.
key_gz   = (glass_guard + glass_t + fz_boss)/2;
fz_plate = fr_depth - back_t;                  // inner face of the back plate
fr_bosses = [for (sx = [1,-1], sy = [1,-1]) [-m3_ox + sx*m3_dx/2, m3_oy + sy*m3_dy/2]];
// WHICH portrait slides actually get cut. The TOP pair's portrait slides run
// at khm_y — the same line as the top boss row (m3_oy + m3_dy/2, 0.275 mm
// apart at stock dims) — and the M3 pattern's x offset is MIRRORED on this
// print-side part, so the -x bosses sit 2*m3_ox = 3 mm closer to their
// keyhole than the +x bosses do. That 3 mm is the whole story: the top-left
// (-x) keyhole's INBOARD slide tip lands 3.9 mm from the M3 screw-head
// pocket's center against 5.45 mm of combined cut radius, so the slide, its
// mouth chamfer and the pocket merge into one opening (first render: the
// hatched z-fight where the two cuts share a face). The fix is subtraction,
// not surgery: that ONE leg is not cut. The keyhole keeps its vertical slide
// and its outboard portrait slide, and the one portrait orientation that
// wanted the missing leg (+x wall up) hangs on the other three keyholes —
// three screws still pin the case flat. The predicate is geometry, not a
// corner list, so a future move of khm_* or the M3 pattern drops (or
// restores) exactly the legs the numbers say — and the echo reports it.
function _khm_seg_d(p, a, b) =       // point→segment distance, for the slide's stadium
    let (ab = b - a, t = ab*ab == 0 ? 0 : max(0, min(1, ((p - a)*ab)/(ab*ab))))
    norm(p - (a + t*ab));
function khm_slide_ok(sx, sy, px) =  // true = this portrait slide clears every
    let (hy = sy > 0 ? khm_y : -(khm_y + khm_len))   // M3 head pocket + chamfer + web
    min([for (p = fr_bosses)
        _khm_seg_d(p, [sx*khm_dx, hy], [sx*khm_dx + px*khm_plen, hy])])
    > khm_slide_w/2 + (cb_d + 0.4)/2 + khm_mouth_c + 0.3;
khm_dropped = mount_keyholes && mount_portrait
    ? [for (sx = [1,-1], sy = [1,-1], px = [1,-1])
        if (!khm_slide_ok(sx, sy, px)) [sx, sy, px]] : [];
// A portrait slide may be dropped; the head hole and the VERTICAL slide may
// not — without them the corner is not a mount at all. If either reaches a
// pocket the answer is moving khm_*, and this fails the build until someone
// does.
assert(!mount_keyholes || min([for (sx = [1,-1], sy = [1,-1], p = fr_bosses)
        let (hy = sy > 0 ? khm_y : -(khm_y + khm_len))
        min(_khm_seg_d(p, [sx*khm_dx, hy], [sx*khm_dx, hy + khm_len])
                - khm_slide_w/2,
            norm([sx*khm_dx, hy] - p) - khm_head_d/2)])
        > (cb_d + 0.4)/2 + khm_mouth_c + 0.3,
       "frame: a keyhole's head hole or vertical slide reaches an M3 head pocket — move khm_dx/khm_y");
// The frame's grille keepouts, hoisted so the open-area echo below is
// computed from the SAME lists the cutter uses — the reported number cannot
// drift from the geometry. Split so the echo can also say what the rails
// cost (their keepouts are the one deliberate vent trade in this case).
sd_teth_y = sd_dy + sd_l/2 + sd_lip + sd_teth_gap;   // tether anchor hole center
// the rating stamp's block, and the smooth keepout it claims from the grille
// SIZE. This block is the one graphic on the case a person reads for a
// reason — what voltage, what connector, indoors or out — and at 2.6 mm it
// was the smallest type on the plate, below the BOOT/RESET labels, debossed
// in the body color on a black part. It is now the size the gap can actually
// carry, which is not a free choice: rating_w grows with the longest line and
// the block is boxed between the mark on its left and the +x boss pocket on
// its right, so the asserts below are what set this ceiling.
rating_sz = 3.2;    // sized to its gap — see the width asserts below
rating_lh = rating_sz*1.45;
// width from the LONGEST line, the same way the edge words size themselves,
// so editing rating_lines can never silently overrun the gap it sits in
rating_w  = 2*0.392*rating_sz*max([for (l = rating_lines) len(l)]);
rating_h  = rating_lh*len(rating_lines) + 2;
// THE RIGHT-HAND COLUMN. With the badge holding the left of the plate and the
// card window holding the bottom right, the strip above that window is the one
// place a spec block can sit without crowding either — so the plate reads as
// three things (badge, small print, window) stacked down the right instead of
// four things scattered. It used to live in the top-right corner, which is
// where a fourth thing goes when nobody has decided where things go.
rating_dx = 70.0;   // AT THE PORT. What a spec block is for is answering "what
rating_dy = 0.0;    // do I plug in here", so it belongs beside the thing you
                    // plug into rather than in whatever gap is left over. The
                    // side exit comes through the +x wall at dy 0 — so does
                    // this, on the far right wing, level with the hole, in the
                    // clear band between the two right-hand keyholes.
                    // It used to sit at (37, 21), squeezed into the strip
                    // above the card window because the two 1D asserts below
                    // read as "the block may not share a COLUMN with a
                    // keyhole" and "may not share a ROW with the card window",
                    // which between them left one gap on the whole plate. Both
                    // are box tests now (see them for why), and the block
                    // moved to where the thing it describes actually is.
// The help URL is a SYMBOL again, not type — see qr_back, which is on. The
// typed line existed only because the plate could not carry the QR; carrying
// both would be printing the same address twice on one face.
help_line = "";
help_sz   = 2.8;
help_dy   = 6.5;    // (unused while help_line is empty — kept so turning the
                    // line back on is one word, the same way qr_back is)

// ── Type metrics ───────────────────────────────────────────────────────────
// OpenSCAD has no text-metrics primitive, so every layout number that depends
// on how wide or tall a line renders has to be ESTIMATED — and estimated in
// ONE place, or the estimate drifts between the thing that draws the type and
// the thing that keeps vents off it. 0.392 per character per mm of size is the
// advance the rating block was calibrated against; `sp` is text()'s spacing
// multiplier, which scales the advance and therefore the width.
//
// Deliberately a hair pessimistic. The failure these guard against is type
// running into a vent hole or off a chamfer, and both are only visible on a
// printed part.
function _lbl_w(sz, n, sp = 1) = 2*0.392*sz*n*sp;
// text(valign="center") centers the FONT box, which runs about -0.21 to +0.73
// of the size around the baseline — so the half-height is 0.47*size, not the
// cap height. Using cap height here under-reports a gap by a third of a line.
function _lbl_half(sz) = sz*0.47;

help_w = _lbl_w(help_sz, len(help_line));
help_h = help_sz*1.45 + 2;

// THE LOCKUP'S BAND — derived from the rows, so it cannot describe a stack
// that has moved. Top of the company line down to the bottom of the maker row.
// The lockup's anchor: the bottom of the mark's box. With no mark on the plate
// there is nothing to hang from, so the rows fall back to the rim datum they
// used to own — a wordmark-only plate is a different composition and should not
// silently inherit a gap measured from an object that is not there.
brand_anchor  = back_bird ? bird_dy - bird_half_h : -(fr_yi/2 - 29.4);
brand_sub_y   = brand_anchor - brand_drop_sub;
brand_hero_y  = brand_anchor - brand_drop_hero;
brand_maker_y = brand_anchor - brand_drop_maker;
brand_band_hi = brand_sub_y + _lbl_half(brand_sub_sz);
brand_band_lo = brand_maker_y - _lbl_half(brand_maker_sz);
brand_band_y  = (brand_band_hi + brand_band_lo)/2;
brand_band_h  = brand_band_hi - brand_band_lo;
brand_band_w  = max(_lbl_w(brand_sub_sz, len(brand_sub), 2.0),
                    _lbl_w(brand_sz, len(brand_back), 1.15),
                    _lbl_w(brand_maker_sz, len(brand_maker) + 5, 1.0));

// The keepouts SPLIT by who owns them, so the open-area echo can attribute
// the loss instead of lumping it. fr_keep_fixed is the hardware and the
// small print — things the grille never had a claim on. fr_keep_badge is
// what this revision spends on the mark, and it is the number worth arguing
// with when the case runs warm.
fr_keep_fixed = concat(
    // grown with the doubler pads + mouth chamfers
    mount_keyholes ? [for (sx = [1,-1], sy = [1,-1])
        [sx*khm_dx, sy*(khm_y + khm_len/2),
         10 + (mount_portrait ? khm_plen : 0), 20]] : [],
    // the SD keepout covers the countersunk mouth and the nail scoop — plus
    // the egg's own half-extents, because the cell list stores CENTERS and an
    // egg whose center clears the mouth by a hair still hangs its body over
    // it. One did, on the window's top corner, and it read as a chip rather
    // than as a vent.
    [[sd_dx, sd_dy, sd_w/2 + 3.4 + vent_slot_w/2 + 1.5,
                    sd_l/2 + 6.7 + vent_slot_l/2 + 1.5]],
    // ...and the tether zone: strap channel + anchor hole past the rim
    sd_tether ? [[sd_dx, (sd_dy + sd_l/2 - 1 + sd_teth_y + 2)/2, 4.3,
                  (sd_teth_y + 2 - (sd_dy + sd_l/2 - 1))/2 + 5.1]] : [],
    // the back QR's field + quiet zone: smooth skin, no slot may enter
    // the QR keepout is grown by the egg's own half-extents: the cell
    // list stores CENTERS, and an egg whose center is just outside the
    // quiet zone would otherwise lay its body across it — at worst sharing
    // a face with a module cell, which CGAL rightly calls non-manifold
    qr_draw ? [[qr_back_dx, qr_back_dy,
                qr_back_reach + vent_slot_w/2 + 0.4,
                qr_back_reach + vent_slot_l/2 + 0.4]] : [],
    // the radio window is a THROUGH cut, so it claims its own footprint plus
    // the egg's half-extents for the same reason the card window does: the
    // cell list stores centers, and an egg centered just outside the window
    // still lays its body across the edge and reads as a chipped corner
    soc_cut ? [[soc_dx, soc_dy, soc_w/2 + vent_slot_w/2 + 1.5,
                                soc_h/2 + vent_slot_l/2 + 1.5]] : [],
    // the rating stamp wants unbroken plate under it, same as any deboss
    rating_stamp ? [[rating_dx, rating_dy, rating_w/2 + 3.5, rating_h/2 + 3]] : [],
    // ...and so does the help line under it
    help_line == "" ? [] : [[rating_dx, help_dy, help_w/2 + 2, help_h/2 + 2]],
    // THE LOCKUP'S BAND — the whole width of it, not just the type's own box.
    // Two reasons, and the second is the one that matters. First, the lockup
    // never had a keepout at all: it got away with it because the grille's
    // bottom egg row happened to stop 0.8 mm above the company line, a
    // clearance nothing checked and nothing owned, and the moment the rows
    // moved for their new leading that row landed on them. Second, a keepout
    // sized to the type leaves the eggs BESIDE it — five at one end of the
    // band, four at the other, marooned under the lockup like crumbs. A band
    // either belongs to the type or it belongs to the grille. This one is the
    // type's, all the way across, and the plate is quieter for it.
    [[0, brand_band_y, pcb_w/2,
      brand_band_h/2 + vent_slot_l/2 + 1.5]],
    // THE BADGE'S COLUMN, all of it. The grille used to leave a stranded block
    // of eggs out past the mark's left shoulder — legal, since it cleared
    // every keepout, and wrong, because a dozen small holes in the corner of
    // an otherwise empty half read as debris rather than as ventilation. The
    // plate is two halves now: the left is the badge's and carries nothing
    // else, the right is the machine's and carries the spec block, the help
    // line, the card window and every vent. Splitting it here rather than by
    // hand-placing the field means the split MOVES with the mark.
    // THE GUTTER between the badge and the card window, below the spec block.
    // "SD" and the help line stand in it, and a single column of eggs was
    // standing in it too — one column, four eggs, wedged between the mark's
    // tail and the window's mouth, which is the exact place a plate can least
    // afford noise. Vents belong in a field; this is a margin.
    [[(bird_dx + bird_half_w + sd_dx - sd_w/2 - 3.4)/2,
      (help_dy + help_h/2 - fr_yo/2)/2,
      (sd_dx - sd_w/2 - 3.4 - bird_dx - bird_half_w)/2 + vent_slot_w/2,
      (help_dy + help_h/2 + fr_yo/2)/2]]);
// What the badge claims: its own box plus its margin, and — unless you turn
// badge_column off — the rest of its half of the plate.
fr_keep_badge = !back_bird ? [] : concat(
    [[bird_dx, bird_dy,
      bird_half_w + vent_slot_w/2 + bird_vent_gap,
      bird_half_h + vent_slot_l/2 + bird_vent_gap]],
    badge_column ? [[-pcb_w/2 - 1, 0,
                     pcb_w/2 + 1 + bird_dx + bird_half_w + bird_vent_gap,
                     pcb_h]] : []);
fr_keep_base = concat(fr_keep_fixed, fr_keep_badge);
fr_keep_rails = adh_rails ? [for (sx = [1,-1])
    [sx*adh_rail_dx, 0,
     adh_rail_w/2 + adh_mark_w + vent_slot_w/2 + 0.6,
     adh_rail_l/2 + adh_mark_w + vent_slot_l/2 + 0.6]] : [];
fr_keepouts = concat(fr_keep_base, fr_keep_rails);
btn_z0 = glass_guard + glass_t + 1;  btn_z1 = fz_boss + 1;   // the band the buttons live in
btn_zc = (btn_z0 + btn_z1)/2;                  // window center across the wall
// Tower reach, from the plug's LIP PLANE (local z 3.6 — where the pips used
// to start) to the button caps. AUTO derives it from the panel record: the
// caps sit flush with the PCB outline (BOOT/RESET carry d = 0), the board
// edge sits at pcb_h/2, and the plug's local z 0 is the outer skin at
// fr_yo/2 — so outer skin → caps is fr_yo/2 - pcb_h/2 and the towers carry
// what the lip plane doesn't. RESTING contact by construction: the tower
// tips park ON the caps and the dimple's flex is the press travel.
btn_tower_len = btn_reach > 0 ? btn_reach : fr_yo/2 - pcb_h/2 - 3.6;
assert(btn_tower_len > 0.4 && btn_tower_len <= fr_yo/2 - pcb_h/2 - 3.6 + 0.6,
       str("frame: btn_reach drives the towers past the button caps (max ",
           fr_yo/2 - pcb_h/2 - 3.6 + 0.6, " incl 0.6 squeeze) — they would ",
           "hold BOOT/RESET pressed. Measure again."));
// USB pass-through: the opening passes the HEAD; the grommet then fills it
// around just the wire, so the port needs no relation to where the connector
// actually is — the cable routes inside, and the case never has to know.
usb_open_w = usb_head_w + 2*usb_pass_c;
usb_open_h = usb_head_h + 2*usb_pass_c;
// Brand deboss words: each centered between the port flange and the desk
// dock's well edge — the placement the stencil vents established (and the
// first print validated visually); as deboss they no longer need to stay
// inside the well's breathing span. Width is estimated (no textmetrics in
// the render pipeline's OpenSCAD): bold caps advance ≈ 0.70 em × 1.12
// tracking.
vword_x    = ((usb_open_w/2 + grom_lip + vent_gap)
              + ((stand_w - 2*stand_cheek_t)/2 - 2))/2;
vword_half = 0.392 * edge_text_size * max(len(edge_text_l), len(edge_text_r));

// Stand derived. The dock is drawn in ITS print orientation (base on the
// plate, +z up, +y toward the back fin); the "seat frame" is the tilted
// coordinate system of the docked case — origin on the seat pads' plane at
// the seat centerline, local +z running up the reclined case.
std_cd   = fr_depth + 2*stand_clear;      // slot gap: the frame's outer depth + slack
// A battery case is DEEPER, so its dock grows with it: the base gains
// bat_extra split across both ends, and the seat centerline shifts back by
// the front half — the lip's front-edge margin stays exactly what the
// stock dock has, and the rear anti-tip margin only ever improves.
bat_dg = bat_on ? ceil(bat_extra/2) : 0;
std_d  = stand_d + 2*bat_dg;
std_ys = -std_d/2 + stand_slot_y + bat_dg;   // seat centerline in plate coords
std_open = stand_w - 2*stand_cheek_t;     // clear span between the seat pads
// outermost bottom-wall intake opening on the frame — the shadow gill row's
// far end (the brand words are deboss now, not intake, and the ±dock_key_bx
// slots sit under the pads BY DESIGN: a key fills each, so they are not
// intake the well must span) — the well must clear it
std_intake_x = (edge_vent_n - 1)/2*edge_vent_pitch + edge_vent_w/2;
// where the lip's outer face meets the desk (the dock's front-most point)
std_front_foot = std_ys - (std_cd/2 + stand_lip_t)*cos(stand_ang)
    - (stand_floor_h + (std_cd/2 + stand_lip_t)*sin(stand_ang))*tan(stand_ang);

assert(bez_lip >= 2.0, "bezel lip < 2 mm won't retain a 7in slab");
assert(lip_min >= bez_lip, str("bezel lip is only ", lip_min,
       " mm at its tightest edge — widen the glass border, shrink win_margin, ",
       "or lower bez_lip"));
assert(aa_w < glass_w && aa_h < glass_h, "active area must sit inside the glass");
assert(m3_dx + 2*abs(m3_ox) + lob_d < xo && m3_dy + 2*abs(m3_oy) + lob_d < yo,
       "M3 pattern doesn't fit the shell");
assert(m3_dx/2 + abs(m3_ox) <= pcb_w/2 && m3_dy/2 + abs(m3_oy) <= pcb_h/2,
       "M3 pattern falls outside the PCB outline");
assert(frame_reveal > 0 && frame_reveal <= 0.6, "frame_reveal out of sane range");
assert(min([for (p = fr_bosses) min(fr_xi/2 - abs(p[0]), fr_yi/2 - abs(p[1]))])
       > frame_boss_d/2 + 1, "frame: a boss lands under the sleeve wall — check m3_* / offsets");
assert(abs(btn_dx) + btn_w/2 + 3 < fr_xi/2 - fr_ri,
       "frame: button window overruns the bottom wall's flat span");
assert((btn_z0 + btn_z1)/2 + btn_h/2 < fz_plate - 0.5,
       "frame: button window cuts into the back plate");
// the plug leash: anchor past the window's bevel but on the wall's flat
// span, and the strap channel must respect the same >= 0.8 web floor the
// bottom-edge deboss lives by
assert(!plug_tether || (abs(plug_teth_dx) - sd_teth_hole/2 - 0.45 > btn_w/2 + 1.3
       && abs(btn_dx + plug_teth_dx) + sd_teth_hole/2 + 1.5 < fr_xi/2 - fr_ri),
       "frame: plug leash anchor lands on the window bevel or off the top wall's flat span");
assert(!plug_tether || plug_chan_d <= frame_wall - 0.8,
       "frame: plug leash channel leaves under 0.8 of top-wall web — shallow plug_chan_d");
assert(!mount_keyholes || (khm_dx + khm_head_d/2 + khm_pad_w + 1.5 < fr_xi/2
       && khm_y + khm_len + khm_slide_w/2 + khm_pad_w + 1.5 < fr_yi/2
       && khm_y - khm_head_d/2 - khm_pad_w > 2),
       "frame: a keyhole (or its doubler pad) runs off the back plate (the bottom pair mirrors the top, so one bound covers all four)");
assert(khm_slide_w < khm_head_d, "frame: keyhole slide wider than its head hole");
// count knobs that feed `for (i = [0 : n - 1])`: at n = 0 OpenSCAD 2021.01
// iterates {0, -1} AND prints a DEPRECATED warning, which the render gate
// reads as a failure; ledge_slope_n = 0 divides the staircase by zero. Say
// so here, in the knob's own words, instead of in a CI log
assert(vent_rows >= 1 && vent_cols >= 1, "vent_rows / vent_cols must be at least 1 (use plate_grille=false to drop the grille)");
assert(vent_top_n >= 1 && vent_bottom_n >= 1 && vent_side_n >= 1,
       "vent_top_n / vent_bottom_n / vent_side_n must be at least 1");
assert(frame_vent_flank_n >= 1, "frame_vent_flank_n must be at least 1");
assert(edge_vent_n >= 1, "edge_vent_n must be at least 1");
assert(ledge_slope_n >= 1, "ledge_slope_n must be at least 1 (it divides the staircase)");
// the frame chains its depth through standoff_len only, so it never asks
// whether the tallest REAR component (comp_h, the registry's one MEASURE
// number) clears the plate. With the record as written it does not: the
// component ceiling sits comp_h above the PCB back while the plate's inner
// face is fz_plate. A reference print closed, so the record is the likelier
// error — but a number that has never met calipers is not a number to move
// the validated frame by, so this is a CHECK, not an assert, until it has
frame_comp_ceiling = glass_guard + glass_t + pcb_standoff + pcb_t + comp_h;
if (frame_comp_ceiling + 0.5 > fz_plate)
    echo(str("CHECK: the registry's comp_h (", comp_h, " mm) puts the tallest rear component ",
             round((frame_comp_ceiling + 0.5 - fz_plate)*100)/100,
             " mm INTO the back plate (ceiling ", frame_comp_ceiling, " vs plate ", fz_plate,
             "). A reference print closed, so MEASURE comp_h and correct the panel record; ",
             "if it really is that tall, raise frame_boss_h by the difference."));
assert(!mount_keyholes || fz_plate - khm_pad_t >= fz_boss + 0.5,
       "frame: keyhole doubler pads reach into the component band behind the board — thin khm_pad_t");
// the adhesive rails must own their columns outright: clear of the SD mouth
// (lip included), the boss head pockets, the keyhole pads, the brand deboss
// line under the grille, and each other across the center
assert(!adh_rails || (adh_rail_dx - adh_rail_w/2 - adh_mark_w > 1
       && adh_rail_dx + adh_rail_w/2 + adh_mark_w + 1 < abs(sd_dx) - sd_w/2 - sd_lip
       && adh_rail_dx + adh_rail_w/2 + adh_mark_w + 1
          < min([for (p = fr_bosses) abs(p[0])]) - (cb_d + 0.4)/2
       && (!mount_keyholes
           || adh_rail_dx + adh_rail_w/2 + adh_mark_w + 1
              < khm_dx - khm_head_d/2 - khm_pad_w)
       && adh_rail_l/2 + adh_mark_w + 1 < fr_yi/2 - 9),
       "frame: an adhesive rail collides with the SD mouth, a boss pocket, a keyhole pad, the brand deboss or the plate edge");
assert(label_back_depth > frame_rim + 0.3 && label_back_depth <= back_t - 1.2,
       "frame: label_back_depth must clear the rim-chamfer band (the two-color swap) yet leave 1.2 mm of plate");
assert(sd_dx + sd_w/2 + sd_lip + 2 < fr_xi/2
       && sd_dy + sd_l/2 + sd_lip + 2 < fr_yi/2
       && (!sd_tether || sd_teth_y + sd_teth_hole/2 + 3 < fr_yi/2)
       && sd_dy - sd_l/2 - sd_lip - 6 > -fr_yo/2 + frame_rim,
       "frame: SD cover (mouth, tether anchor or nail scoop) runs off the back plate");
// retention is geometry, not hope: the shaft must ride loose in the hole,
// the arrowhead must carry real interference over it (or the "captive"
// cover pulls straight back through — both gates would still pass), and
// the head must fit down the 5.0 strap channel to be inserted at all
assert(!sd_tether || (sd_teth_hole >= 3.1
       && sd_teth_head >= sd_teth_hole + 1.0 && sd_teth_head <= 5.0),
       "frame: tether retention broken — need hole >= 3.1 (Ø2.8 shaft clearance), head >= hole + 1.0, head <= 5.0 (channel width)");
// the barb pops out just inside the plate and must stop short of the
// component band; its head must also ride clear of the nearest boss fillet
assert(!sd_tether || fr_depth - 5.2 >= fz_boss + 0.5,
       "frame: tether barb reaches the component band — shorten it or thicken the stack");
assert(!sd_tether || min([for (p = fr_bosses)
       max(abs(sd_dx - p[0]), abs(sd_teth_y - p[1]))])
       > sd_teth_head/2 + frame_boss_d/2 + 2.5,
       "frame: tether anchor hole lands on a boss");
assert(min([for (p = fr_bosses) max(abs(sd_dx - p[0]) - sd_w/2 - sd_lip - frame_boss_d/2,
                                    abs(sd_dy - p[1]) - sd_l/2 - sd_lip - 2 - frame_boss_d/2)]) > 1,
       "frame: SD cover countersink collides with a boss");
assert(!usb_port || (usb_zc - usb_open_h/2 - grom_lip > glass_t + 0.4
       && usb_z + usb_open_h/2 + grom_lip < fz_boss - 0.2),
       "frame: USB port (plus its grommet flange) runs out of the bottom wall band");
// the side exit shares the z band above; along the wall it must keep its
// whole service zone — flange, leash anchor, peel scoop — on the flat span
// and clear of the portrait dock keying slots
assert(side_exit == "none"
       || abs(side_dy) + usb_open_w/2 + grom_lip + 0.5 < fr_yi/2 - fr_ri,
       "frame: side exit (plus grommet flange) runs off the side wall's flat span");
assert(side_exit == "none" || !dock_keys
       || abs(side_dy) + max(usb_open_w/2 + grom_lip,
                             abs(port_teth_dx) + sd_teth_head/2 + 1,
                             port_scoop_dx + 3.5)
          < dock_key_dx - gill_l/2 - 1,
       "frame: side exit's port/leash/scoop zone reaches the portrait dock keying slot");
assert(!usb_port || usb_wire_d - tpu_grip >= 1.0,
       "frame: grommet bore closes up — usb_wire_d vs tpu_grip");
assert(!usb_port || usb_wire_d + 2 < usb_open_h,
       "frame: cable jacket barely fits the opening the grommet must fill");
assert(edge_lbl_depth <= frame_wall - 0.8 && edge_lbl_depth >= 0.4,
       "frame: bottom-edge deboss out of range — leave at least 0.8 of wall web");
assert(vword_x - vword_half > usb_open_w/2 + grom_lip + 2
       && vword_x + vword_half + 2 < fr_xi/2 - fr_ri,
       "frame: brand deboss words don't fit between the USB port and the wall's flat span");
assert(!usb_port || edge_vent_z - edge_vent_h/2
       > usb_zc + usb_open_h/2 + grom_lip + 0.3,
       "frame: shadow gill row collides with the grommet's outer flange");
assert(edge_vz + edge_vent_h/2 < fz_plate - 0.2,
       "frame: shadow gill row cuts into the back plate");
assert((edge_vent_n - 1)/2*edge_vent_pitch + edge_vent_w/2 < fr_xi/2 - fr_ri - 2,
       "frame: shadow gill row runs off the bottom wall's flat span");
assert(btn_reach == 0 || (btn_reach >= 0.5 && btn_reach < 12),
       "frame: btn_reach out of sane range — 0 = AUTO (derive from the panel record), else measure lip plane to button cap");
// the whole panel assembly (glass + standoffs + board) enters through the
// ledge opening, so the opening must clear the BOARD, not just the glass
assert(fr_xi - 2*ledge_side > pcb_w + 2 && fr_yi - ledge_top - ledge_bot > pcb_h + 2,
       "frame: adhesive ledge blocks the board's insertion path");
assert(ledge_z + ledge_t < glass_guard + glass_t + pcb_standoff,
       "frame: ledge intrudes into the board plane");
assert(glass_edge_t > 0 && glass_edge_t < glass_t,
       "frame: glass_edge_t is the panel's bare-glass border — it must be thinner than the full module stack (glass_t) and non-zero");
assert(vent_pitch_x - vent_slot_w >= 2.39 && vent_pitch_y - vent_slot_l >= 0.79,
       "grille: egg webs too thin — grow the pitches or shrink the egg");
// The band moved with the mark. 0.15..0.6 described a FEATHER — below 0.15 the
// crown closes to a needle that will not print, above 0.6 the taper stops
// reading as a point. The egg lives above that, so the band spans both motifs
// rather than being one number that quietly means "feather".
assert(vent_tip > 0.15 && vent_tip < 0.85,
       str("grille: vent_tip ", vent_tip, " is outside 0.15..0.85. Below 0.15 ",
           "the crown closes to a needle; above 0.85 the shape is a stadium ",
           "with no taper at all and is no longer a mark. Feather territory is ",
           "~0.28, egg ~0.72. This case does not pick either — vent_tip is ",
           "egg_tip() from canary_vent_lib.scad, the line-wide constant. If ",
           "this fired, the LIBRARY moved out of band: fix it there, so every ",
           "adopter follows instead of just this one."));
// The mark must stay inside the clear middle: clear of the QR's quiet zone on
// the left, the SD mouth on the right, and the lockup below. Derived from the
// same numbers those features use, so moving any of them moves this bound too
// rather than leaving a stale constant behind.
// (bird_half_w / bird_half_h are hoisted to the mark's knob block — the
// grille keepouts need them, and they are assembled well above here.)
// THE MARK AND THE RAILS WANT THE SAME PLATE. That is not a coincidence to
// guard against — it is the whole reason the bird could move in: the rails
// were the only clear full-height columns, so the middle they vacated is
// exactly where the mark now sits. Rails at +/-12 with a 17 wide zone span
// x 3.5..20.5 either side of center; a 32 mm bird spans +/-19.8. They
// overlap almost completely, and a Customizer user who turns the rails back
// on gets a mark debossed across both adhesive landing zones — which is both
// ugly and a worse bond, since the strip needs smooth plate.
//
// Mutually exclusive, then, and said out loud rather than silently resolved:
// auto-suppressing one would change the geometry without telling anyone.
assert(!(back_bird && adh_rails),
       str("frame: the Canary mark and the adhesive rails both need the ",
           "middle of the back plate — the rails' landing zones (x 3.5..20.5 ",
           "each side) sit under a ", 2*bird_half_w, " mm mark. Pick one: ",
           "back_bird=false for a Command-strip build, or adh_rails=false ",
           "(the default) to keep the mark and reclaim 64 grille slots."));
// THE BADGE OWNS THE PLATE, so these are the checks that decide how big it is
// allowed to get. Each one names the thing it hit, because "the mark is too
// big" is not actionable and "the mark's tail is 1.2 mm into the card window"
// is. The QR clause is conditional now: with qr_back off there is no quiet
// zone to stay out of, and asserting one anyway would reserve 38 mm of plate
// for geometry that is not cut — the same mistake the rating block's left
// bound made against the adhesive rails.
assert(!back_bird || bird_dx + bird_half_w < sd_dx - sd_w/2 - 2,
       str("frame: the mark (", mark_w_mm(bird_h, bird_rib),
           " mm wide at bird_dx ", bird_dx, ", right edge ",
           bird_dx + bird_half_w, ") runs into the card window at ",
           sd_dx - sd_w/2, " — shrink bird_h or move bird_dx left"));
assert(!back_bird || !qr_back
       || bird_dx - bird_half_w > qr_back_dx + qr_back_reach + 2,
       str("frame: the mark runs into the help QR's quiet zone. These two are ",
           "the plate's biggest objects and they do not both fit beside a ",
           "40 mm card window at this mark size — bring bird_h down to about ",
           "45, or leave qr_back off and keep the URL as type (help_line)."));
// ...and it has to stay ON the plate, clear of the rim chamfer and of the four
// M3 boss head pockets, which are real holes and not just keepouts.
assert(!back_bird
       || (abs(bird_dx) + bird_half_w < fr_xo/2 - frame_rim - 2
           && abs(bird_dy) + bird_half_h < fr_yo/2 - frame_rim - 2),
       "frame: the mark hangs off the plate's flat — shrink bird_h");
assert(!back_bird || min([for (sx = [1,-1], sy = [1,-1])
           max(abs(bird_dx - (-m3_ox + sx*m3_dx/2)) - bird_half_w - cb_d/2 - 1.5,
               abs(bird_dy - ( m3_oy + sy*m3_dy/2)) - bird_half_h - cb_d/2 - 1.5)]) > 0,
       str("frame: the mark's box reaches an M3 boss head pocket. The four of ",
           "them sit at (±", m3_dx/2, ", ±", m3_dy/2, ") off the pattern ",
           "center and are counterbores, not keepouts — move bird_dx/dy or ",
           "shrink bird_h."));
stamp_half = stamp_size*0.9 + stamp_size*0.39 + 0.6;   // text block half-height
assert(!stamp_show || (stamp_depth < back_t - 1.5
       && stamp_dy - stamp_half
            > (vent_rows - 1)/2*vent_pitch_y + vent_slot_l/2 + 1
       && stamp_dy + stamp_half < fr_yi/2 - plate_fillet - 0.5),
       "frame: build stamp collides with the top egg row, the plate rim/fillet, or thins the plate");
// Negative is legal and is what ships (wipe relief), but only just: past a
// layer or two the glass is standing on its own edge with no case around it,
// which is a chipped corner waiting to happen. Positive is still a trim
// reveal, not a bumper.
// A PORT LABEL MUST NAME SOMETHING THAT IS ACTUALLY BEHIND THAT WALL.
// The panel registry knows where every connector sits, so this is checkable
// rather than a thing to remember: if the bottom exit claims "USB", the
// record had better put a USB connector on the bottom face. It shipped wrong
// once in the other direction — a labeled opening on a wall with no
// connector behind it — and a molded word is not a caption you can correct
// after the fact.
// It also catches something bigger than a label. This case cuts ONE cable
// opening, in the bottom wall, so the panel's USB-C had better be on the
// bottom face — and for the -7B the record says it is on the TOP, because
// that board is mounted half a turn round. The case does not have an opening
// where that board's connector is. That is a real gap, exposed the moment the
// port map became data instead of an assumption, and it is the second thing
// the registry has caught about the -7B (see the can/AA offset note in
// canary_panel_lib.scad). Fixing it properly means cutting the opening on
// whichever wall the record names, along with its grommet, scoop and label —
// not suppressing this line.
assert(!usb_port || !port_labels || port_lbl_a == ""
       || (pnl_has_port(PANEL, "USB-C") && pnl_port_face(PANEL, "USB-C") == "bottom"),
       str("frame: the case cuts its cable opening in the BOTTOM wall and ",
           "labels it \"", port_lbl_a, "\", but panel \"", pnl_id(PANEL),
           "\" puts USB-C on the \"", pnl_port_face(PANEL, "USB-C"),
           "\" face. The opening is in the wrong wall for this board — the ",
           "case needs its port moved to follow the panel record, which is ",
           "not done yet. Build lcd7 until it is."));

assert(glass_guard >= -0.6 && glass_guard <= 1.2,
       str("frame: glass_guard ", glass_guard, " out of range. Positive = ",
           "recessed glass (drop protection, keep under 1.2); 0 = nominally ",
           "flush; negative = rim below the glass for wiping, and -0.6 is as ",
           "far as that can go before the glass edge is unprotected."));
// the fillet ring lives in the same clear band as the keyhole pads; it must
// stop short of the button window's back edge or the window loses its corner
assert(plate_fillet == 0
       || (plate_fillet <= khm_pad_t + 1 && fz_plate - plate_fillet >= btn_z1 + 0.3),
       "frame: plate_fillet reaches the button window band — shrink it");
// With the ledge at the bare-glass border, the module can passes THROUGH the
// ledge ring going in (it did not when the ring sat behind the whole module
// stack) — so the ring's opening must clear the can, with insertion room.
assert(fr_xi - 2*ledge_side > panel_core_w + 0.6,
       "frame: side ledges close on the LCD module can — panel cannot insert");
assert(fr_yi/2 - ledge_top > panel_core_dy + panel_core_h/2 + 0.3
    && -fr_yi/2 + ledge_bot < panel_core_dy - panel_core_h/2 - 0.3,
       "frame: top/bottom ledge closes on the LCD module can — panel cannot insert");

// Exported for canary_s3_lcd7_fitcheck.scad — the frame's own derived stack,
// so the frame fit gates read the real values instead of copies.
// (index 6 is the bare-glass border thickness the frame_glass gate probes
//  with; 7..9 are the ledge reaches, exported for completeness)
// (index 10 is the guard rim height — the panel probes in the fit gates sit
//  this far behind the front face)
function lcd7_frame_stack() = [ledge_z, ledge_t, fr_xi, fr_yi, fr_ri, fr_depth,
                               glass_edge_t, ledge_side, ledge_top, ledge_bot,
                               glass_guard];
// The LCD module can, stated independently of the ledge geometry — the
// frame_glass gate's core probe, so an enclosure edit can never shrink the
// panel it is checked against.
function lcd7_panel_core() = [panel_core_w, panel_core_h, panel_core_dy];
// WHICH record this build resolved. Exported so a consumer that cannot see
// panel_variant (a -D does not cross a use<> boundary) can look up the SAME
// record rather than rebuild the panel's shape from loose numbers — which is
// what the fit gates used to do, and it meant a change to how the panel is
// MODELED never reached the gates that check the case against it.
function lcd7_panel_id() = panel_variant;
// ...and the port/fitment geometry the TPU fit gates need, same doctrine.
// [usb_dx, usb_zc, usb_head_w, usb_head_h, fr_yi, fr_yo, ledge_bot,
//  btn_dx, btn_zc, sd_dx, sd_dy, fz_plate, usb_port, btn_w, btn_h, sd_w,
//  sd_l, ledge_top]
function lcd7_ports() = [usb_dx, usb_z, usb_head_w, usb_head_h, fr_yi, fr_yo,
                         ledge_bot, btn_dx, btn_zc, sd_dx, sd_dy, fz_plate,
                         usb_port ? 1 : 0, btn_w, btn_h, sd_w, sd_l, ledge_top];
// ...and the adhesive-rail zones for the frame_adh_rail gate.
// [on, adh_rail_dx, adh_rail_w, adh_rail_l, fr_depth]
function lcd7_adh_rails() = [adh_rails ? 1 : 0, adh_rail_dx, adh_rail_w,
                             adh_rail_l, fr_depth];
// ...and the SD tether anchor for the sd_tether_hole / sd_tether_barb gates.
// [on, hole_x, hole_y, hole_d, fz_plate]
function lcd7_sd_tether() = [sd_tether ? 1 : 0, sd_dx, sd_teth_y,
                             sd_teth_hole, fz_plate];
assert(cb_h + 1.0 <= bez_h, "counterbore deeper than the bezel ear");
assert(cav_d > comp_h + pcb_t, "no air gap left between the PCB and the glass");
// the lip climbs the case's front border — it must never reach the window,
// in landscape OR portrait (portrait's border is the short-edge border, and
// portrait seats stand_rib_drop lower on the well ribs)
assert(stand_lip_h + stand_clear <= fr_yo/2 - view_h/2 + aa_dy - 1.0,
       "stand: the front lip would cover the bottom of the screen window");
assert(stand_lip_h + stand_rib_drop + stand_clear <= fr_xo/2 - view_w/2 - 1.0,
       "stand: the front lip would cover the window in portrait");
// the well between the pads must span every bottom-wall intake opening, or
// the dock smothers the convection path the case depends on
assert(std_open/2 >= std_intake_x + 2,
       "stand: the seat pads sit on the case's intake vents — widen stand_w or thin the cheeks");
assert(std_front_foot > -std_d/2 + 2,
       "stand: the front lip runs off the base plate — deepen stand_d or raise stand_slot_y");
// anti-tip, BOTH orientations: the base must extend well behind the reclined
// case's center line — portrait stands a 197 mm slab on its side, so it gets
// the bigger margin (a fingertip pressing the top of the touchscreen pries
// against exactly this lever)
assert(std_d/2 - (std_ys + sin(stand_ang)*fr_yo/2) >= 12,
       "stand: base too short behind the reclined case — it will tip backward");
assert(std_d/2 - (std_ys + sin(stand_ang)*fr_xo/2) >= 20,
       "stand: base too short for PORTRAIT — deepen stand_d");
assert(stand_floor_h >= 20,
       "stand: no headroom under the case for the USB power plug");
assert(stand_cable_w + 8 < std_open, "stand: cable channel wider than the well");
assert(stand_gusset_h < stand_fin_h, "stand: cheek gusset overruns the fin");
// Help-QR field + its quiet zone (4 modules of bare deck all round, per the
// QR spec) must land wholly on the flat deck: clear of the cable channel,
// the plate edges, the back-corner radius, and — for a straight-down scan —
// clear of the RECLINED fin's overhang, which reaches fin_h*sin(a) behind
// its own foot.
qr_field = qr_n*qr_cell;  qr_reach = qr_field/2 + 4*qr_cell;
qr_dy_eff = qr_dy + bat_dg;   // the deck's rear half-growth, when a battery deepens the dock
assert(!qr_help || (qr_dx - qr_reach > stand_cable_w/2
    && qr_dx + qr_reach < stand_w/2
    && qr_dy_eff + qr_reach < std_d/2
    // the quiet corner must lie ON the plate: either clear of the corner
    // square entirely, or inside the corner arc with margin
    && (qr_dx + qr_reach <= stand_w/2 - 10
        || qr_dy_eff + qr_reach <= std_d/2 - 10
        || norm([qr_dx + qr_reach - (stand_w/2 - 10),
                 qr_dy_eff + qr_reach - (std_d/2 - 10)]) <= 9.5)
    && qr_dy_eff - qr_reach > std_ys + (std_cd/2 + stand_fin_t)*cos(stand_ang)
                          + stand_fin_h*sin(stand_ang)),
       "stand: help QR (field + quiet zone) runs off the deck — into the cable channel, an edge, the back-corner round, or under the fin's overhang");
// The back-plate QR's quiet zone is smooth ACCENT skin, so nothing that
// reads in the body color may enter it: the rail moats, the brand line,
// the keyhole keepout features. The grille dodges it via its keepout.
assert(!qr_back || (qr_back_dx + qr_back_reach
                        < -(adh_rail_dx + adh_rail_w/2 + adh_mark_w + 0.6)
                    || qr_back_dx - qr_back_reach
                        > adh_rail_dx + adh_rail_w/2 + adh_mark_w + 0.6),
       "frame: back QR quiet zone crosses an adhesive rail moat line");
// Does a [±hx, ±hy] box at (cx, cy) sit on the back plate's FLAT — the plate's
// rounded-rectangle outline, inset by the rim chamfer and a margin?
//
// The rounded corner is only binding when BOTH coordinates run past the
// straight edges, and that is precisely what the bound this replaces got
// wrong. It read `abs(x) + reach < fr_xi/2 - fr_ri`: the full corner radius
// subtracted from the x limit, i.e. the entire long edge treated as if it were
// a corner. On a 197 mm plate that declares ~10 mm of perfectly flat skin per
// side unusable, and it declared exactly the strip the help QR needed. It also
// measured off fr_xi (the GLASS opening) for a graphic on the OUTER face,
// which is a different rectangle 2 mm bigger each way.
// A pessimistic bound is not free just because it is pessimistic: this one
// cost the plate its symbol, and the paragraph at qr_back explaining that the
// symbol does not fit was written around it.
function on_back_flat(cx, cy, hx, hy, m = 1.5) =
    let (A = fr_xo/2 - frame_rim - m,
         B = fr_yo/2 - frame_rim - m,
         R = max(0, fr_ro - frame_rim),
         px = abs(cx) + hx,
         py = abs(cy) + hy)
    px <= A && py <= B
    && (px <= A - R || py <= B - R || norm([px - (A - R), py - (B - R)]) <= R);

assert(!qr_back || on_back_flat(qr_back_dx, qr_back_dy,
                                qr_back_reach, qr_back_reach),
       str("frame: the back QR (field + quiet zone, ", 2*qr_back_reach,
           " mm square at (", qr_back_dx, ", ", qr_back_dy,
           ")) runs off the plate's flat or onto a corner round — move ",
           "qr_back_dx inboard or shrink qr_back_cell"));
// ...and off the four M3 boss head pockets, which are real holes. The old rim
// bound kept the symbol so far inboard that it could never reach one, so this
// check did not have to exist; loosening that bound is what makes it load-
// bearing, and a loosened bound whose companion check is missing is how a
// quiet zone acquires a counterbore in it.
assert(!qr_back || min([for (p = fr_bosses)
           max(abs(qr_back_dx - p[0]) - qr_back_reach - cb_d/2 - 1.5,
               abs(qr_back_dy - p[1]) - qr_back_reach - cb_d/2 - 1.5)]) > 0,
       str("frame: the back QR's quiet zone reaches an M3 boss head pocket ",
           "(the four sit at ", fr_bosses,
           " and are counterbores, not keepouts) — move qr_back_dx/dy"));
// ...and off the lockup's band, checked against the TYPE's real box rather
// than against a row number, so moving a brand row moves this with it.
assert(!qr_back
       || qr_back_dy - qr_back_reach > brand_band_hi + 2
       || qr_back_dx + qr_back_reach < brand_dx - brand_band_w/2 - 2
       || qr_back_dx - qr_back_reach > brand_dx + brand_band_w/2 + 2,
       str("frame: the back QR's quiet zone crosses the lockup's band (top at ",
           brand_band_hi, ", ", brand_band_w,
           " mm wide about brand_dx) — raise qr_back_dy"));
// the leash anchor must not land on a brand word, and must stay on the
// wall's flat span (the corner round is not a hole you can push a barb into)
assert(!port_tether || !usb_port
       || abs(usb_dx + port_teth_dx) + sd_teth_hole/2 + 1 < vword_x - vword_half,
       "frame: the port leash anchor lands on a brand word — move port_teth_dx inboard");
assert(!port_tether || abs(port_teth_dx) + sd_teth_hole/2 + 2 < fr_xi/2 - fr_ri,
       "frame: the port leash anchor runs off the wall's flat span");
// the counterbore must leave real land for the shaft, or the anchor is a
// funnel the barb pulls straight back out of
assert(!port_tether || port_teth_cb < frame_wall - 0.4,
       "frame: the anchor counterbore eats the wall — no land left to hold the barb's shaft");
// The peel scoop is placed against the geometry, not by eye: it has to clear
// the brand words, clear the leash anchor's own mouth, and stay on the flat
// span. port_scoop_ftp is the dish's radius WHERE IT MEETS THE SKIN — the
// sphere is bigger than its own footprint, and using the radius here would
// reserve room the scoop never touches.
port_scoop_ftp = sqrt(port_scoop_r*port_scoop_r - port_scoop_d*port_scoop_d);
assert(!port_scoop || port_scoop_r > port_scoop_d,
       "frame: the peel scoop's sphere never reaches the skin — port_scoop_d exceeds its radius");
assert(!port_scoop || !usb_port
       || abs(usb_dx + port_scoop_dx) + port_scoop_ftp + 1 < vword_x - vword_half,
       "frame: the peel scoop runs into a brand word — move port_scoop_dx inboard");
assert(!port_scoop || !port_tether
       || abs(port_scoop_dx - port_teth_dx) > port_scoop_ftp + sd_teth_head/2 + 1,
       "frame: the peel scoop breaks into the leash anchor's mouth — they are on the same wall");
assert(!port_scoop || abs(port_scoop_dx) + port_scoop_ftp + 2 < fr_xi/2 - fr_ri,
       "frame: the peel scoop runs off the wall's flat span onto the corner round");
// and it must stay a DISH, not a hole: the wall keeps most of its section
assert(!port_scoop || port_scoop_r - port_scoop_d < frame_wall - 0.8,
       "frame: the peel scoop cuts too deep — it would breach the port wall");
// the bottom-wall port label sits beyond the brand words; it must still land
// on the wall's flat span, not run onto the corner round
assert(!port_labels || !usb_port
       || vword_x + vword_half + 6.0 + 2.2*len(port_lbl_a) < fr_xi/2 - fr_ri,
       "frame: the bottom port label runs off the wall's flat span — shorten port_lbl_a");
assert(!rating_stamp || rating_dy - rating_h/2 > -(fr_yi/2 - 6) + 5,
       "frame: rating stamp overlaps the brand line's row — raise rating_dy");
// The block's LEFT neighbor depends on the build, and asserting against the
// wrong one costs real type size. With the rails on it is the outer moat;
// with them off (the default) there is no moat on the plate at all and the
// nearest thing is the mark. The old line asserted the moat unconditionally,
// so a case with no rails was still laying out around where they would have
// been — 5 mm of gap reserved for geometry that is not cut.
rating_left_lim = adh_rails ? adh_rail_dx + adh_rail_w/2 + adh_mark_w + 2
                            : bird_dx + bird_half_w + 2;
assert(!rating_stamp || rating_dx - rating_w/2 > rating_left_lim,
       str("frame: the rating stamp (left edge ", rating_dx - rating_w/2,
           ") runs into ", adh_rails ? "an adhesive rail moat" : "the Canary mark",
           " at ", rating_left_lim, " — shrink rating_sz or move rating_dx"));
// The block against the keyholes and against the card window, both as BOX
// tests. Each of these used to be a one-axis bound — "not past the keyholes'
// x", "not within a radius of the window's center" — and each was written when
// the block lived in the one strip left over between them. Together they
// fenced the block into that strip and made it look like the plate's only
// gap. The keyholes are at the four corners and the window is a tall column;
// the right wing's WAIST is clear of both, which a box test can see and a
// column test cannot.
assert(!rating_stamp || !mount_keyholes
       || min([for (sx = [1,-1], sy = [1,-1])
              max(abs(rating_dx - sx*khm_dx)
                      - rating_w/2 - (10 + (mount_portrait ? khm_plen : 0)),
                  abs(rating_dy - sy*(khm_y + khm_len/2))
                      - rating_h/2 - 20)]) > 0,
       str("frame: the spec block (", rating_w, " x ", rating_h, " at (",
           rating_dx, ", ", rating_dy, ")) overlaps a keyhole's claim at (±",
           khm_dx, ", ±", khm_y + khm_len/2, ") — move rating_dy toward the ",
           "plate's waist, or shrink rating_sz"));
assert(!rating_stamp
       || max(abs(rating_dx - sd_dx) - rating_w/2 - sd_w/2 - 3.4 - 1.5,
              abs(rating_dy - sd_dy) - rating_h/2 - sd_l/2 - 3.4 - 1.5) > 0,
       str("frame: the spec block reaches the card window's countersunk mouth ",
           "(window ", sd_w, " x ", sd_l, " at (", sd_dx, ", ", sd_dy,
           "), mouth 3.4 beyond) — move rating_dx outboard or rating_dy up"));
assert(!rating_stamp || on_back_flat(rating_dx, rating_dy,
                                     rating_w/2, rating_h/2, 2.0),
       "frame: the spec block runs off the plate's flat — move rating_dx inboard");
// THE LOCKUP'S LEADING. "Not squished" is a look, so it is checked as one:
// every gap between rows, and the air under the last row, against the type's
// real extents. The rows are free to move — the asserts are what stop them
// closing up again the next time something else needs the space.
brand_lead_min = 2.6;   // ink-to-ink between rows. Below this the stack reads
                        // as one block of type rather than three lines
assert(brand_hero_y + _lbl_half(brand_sz)
       < brand_sub_y - _lbl_half(brand_sub_sz) - brand_lead_min,
       str("frame: \"", brand_back, "\" (top at ",
           brand_hero_y + _lbl_half(brand_sz), ") crowds \"", brand_sub,
           "\" (bottom at ", brand_sub_y - _lbl_half(brand_sub_sz),
           ") — under the ", brand_lead_min,
           " mm the lockup's rows are supposed to keep. Spread brand_drop_* ",
           "or ",
           "shrink a size"));
assert(brand_maker_y + _lbl_half(brand_maker_sz)
       < brand_hero_y - _lbl_half(brand_sz) - brand_lead_min,
       str("frame: the maker line (top at ",
           brand_maker_y + _lbl_half(brand_maker_sz), ") crowds \"",
           brand_back, "\" (bottom at ", brand_hero_y - _lbl_half(brand_sz),
           ") — lower brand_drop_maker or shrink brand_maker_sz"));
assert(brand_maker_y - _lbl_half(brand_maker_sz) > -(fr_yo/2 - frame_rim) + 2.0,
       str("frame: the maker line (bottom at ",
           brand_maker_y - _lbl_half(brand_maker_sz),
           ") is under 2 mm off the back rim's chamfer at ",
           -(fr_yo/2 - frame_rim),
           " — raise the mark (bird_dy) or tighten brand_drop_maker. A row ",
           "that hangs onto the chamfer prints ",
           "as a smeared half-deboss and reads as a slicing fault"));
// ...and the whole band must stay in the plate's own column, clear of the card
// window's side. The widest row is what this is about, whichever one that is.
assert(brand_dx + brand_band_w/2 < sd_dx - sd_w/2 - 2
       || abs(brand_band_y - sd_dy) > sd_l/2 + 6.7 + brand_band_h/2,
       str("frame: the lockup (right edge ", brand_dx + brand_band_w/2,
           ") reaches the card window's column at ", sd_dx - sd_w/2 - 2,
           " — narrow the tracking, shrink a size, or move brand_dx"));
// The badge and its lockup are one thing; if the rows drift up into the mark
// they stop being a caption and start being a collision.
assert(!back_bird || brand_band_hi < bird_dy - bird_half_h - 2.0,
       str("frame: the lockup's top row (", brand_band_hi,
           ") runs into the mark's box (bottom at ", bird_dy - bird_half_h,
           ") — raise brand_drop_sub or shrink bird_h"));
// The spec block and the help line share the right-hand column with the card
// window. Both directions checked: the column has a top (the vent band) and a
// bottom (the window's countersunk mouth).

assert(help_line == "" || help_dy - help_h/2 > sd_dy + sd_l/2 + 3.4 + 1.5,
       str("frame: the help line (bottom at ", help_dy - help_h/2,
           ") sits on the card window's mouth at ", sd_dy + sd_l/2 + 3.4,
           " — raise help_dy"));
assert(help_line == "" || !rating_stamp
       || help_dy + help_h/2 < rating_dy - rating_h/2 - 1.5,
       "frame: the help line runs into the spec block above it — lower help_dy");
assert(help_line == "" || !back_bird
       || rating_dx - help_w/2 > bird_dx + bird_half_w + 1.5,
       "frame: the help line reaches the mark's box — shorten it or move rating_dx");
// THE QR AGAINST THE KEYHOLES, as a box test and no longer as a column test.
// The line this replaces compared |qr_back_dx| against the keyholes' x reach
// alone, which says "the symbol may not share a COLUMN with a keyhole". That
// was a fair shorthand while the symbol lived in a band the grille left free
// near the plate's middle, and it is much too strong now: the keyholes are at
// the plate's four CORNERS and the symbol is on its waist, so they share a
// column and miss each other by their rows. A 1D test that forbids a
// non-overlap is the same defect as a keepout for geometry that is not cut —
// it reserves plate nothing is using, and here it reserved the only 37.7 mm
// of plate the symbol fits on.
// Boxes, not circles, because that is what both claims are: the keyhole's is
// the grown keepout the grille already dodges (head hole + portrait slides +
// doubler pad), the symbol's is its field plus its quiet zone.
assert(!qr_back || !mount_keyholes
       || min([for (sx = [1,-1], sy = [1,-1])
              max(abs(qr_back_dx - sx*khm_dx)
                      - qr_back_reach - (10 + (mount_portrait ? khm_plen : 0)),
                  abs(qr_back_dy - sy*(khm_y + khm_len/2))
                      - qr_back_reach - 20)]) > 0,
       str("frame: the back QR's field + quiet zone (", 2*qr_back_reach,
           " mm square at (", qr_back_dx, ", ", qr_back_dy,
           ")) overlaps a keyhole's claim. The four sit at (±", khm_dx, ", ±",
           khm_y + khm_len/2, ") — move qr_back_dy toward the plate's waist, ",
           "or shrink qr_back_cell."));

// ── THE RADIO WINDOW'S NEIGHBORS ───────────────────────────────────────────
// It is a through cut in the middle of the plate's busiest quarter — the
// board's own M3 boss is 15.6 mm from the module's center on the real board,
// so the case's boss TOWER (frame_boss_d plus its root fillet, which is the
// widest thing there, not the counterbore) is the binding neighbor and not the
// keyholes. Checked against the fillet's radius, because a window cut into a
// boss root removes exactly the material the boss is spread into the floor to
// gain.
soc_half = [soc_w/2, soc_h/2];
assert(!soc_cut || min([for (p = fr_bosses)
           norm([max(0, abs(soc_dx - p[0]) - soc_half[0]),
                 max(0, abs(soc_dy - p[1]) - soc_half[1])])])
       > (frame_boss_d + 3)/2 + 1.2,
       str("frame: the radio window (", soc_w, " x ", soc_h, " at (", soc_dx,
           ", ", soc_dy, ")) cuts into an M3 boss root fillet. The four towers ",
           "sit at ", fr_bosses, " and spread to Ø", frame_boss_d + 3,
           " where they meet the floor — shrink soc_grow, or re-measure the ",
           "module (the record's position is scaled off a drawing, ±2 mm)."));
assert(!soc_cut || on_back_flat(soc_dx, soc_dy, soc_w/2, soc_h/2, 2.0),
       "frame: the radio window hangs off the plate's flat — shrink soc_grow");
// vs the card window and the spec block, which share its half of the plate
assert(!soc_cut || max(abs(soc_dx - sd_dx) - soc_w/2 - sd_w/2 - sd_lip - 2,
                       abs(soc_dy - sd_dy) - soc_h/2 - sd_l/2 - sd_lip - 2) > 0,
       "frame: the radio window reaches the card window's countersunk mouth");
assert(!soc_cut || !rating_stamp
       || max(abs(soc_dx - rating_dx) - soc_w/2 - rating_w/2 - 2,
              abs(soc_dy - rating_dy) - soc_h/2 - rating_h/2 - 2) > 0,
       "frame: the radio window reaches the spec block — move rating_dy down");
// vs the badge, which is the other thing on this plate that can move
assert(!soc_cut || !back_bird
       || max(abs(soc_dx - bird_dx) - soc_w/2 - bird_half_w - 2,
              abs(soc_dy - bird_dy) - soc_h/2 - bird_half_h - 2) > 0,
       "frame: the radio window reaches the mark's box — move bird_dx left");
// Portrait slides: cut + doubler pad must stay inside the plate's flat
// field, clear of the wall fillet ring.
assert(!mount_portrait || !mount_keyholes
       || khm_dx + khm_plen + khm_slide_w/2 + khm_pad_w
          < fr_xi/2 - plate_fillet - 0.5,
       "frame: portrait slide (pad included) reaches the wall fillet ring — shorten khm_plen");
// Battery bay: every furniture span must clear the four M3 boss towers
// (root fillets included), and the pack's own stack must have grown the
// case instead of eating the board-side air gap.
// Long-edge curbs stop at |x| ≤ 48 and short-end curbs at |y| ≤ 24; both
// caps must clear the nearest boss tower footprint (root fillet = d+3).
assert(!bat_on || (48 < min([for (p = fr_bosses) abs(p[0])]) - (frame_boss_d + 3)/2 - 1
                   && 24 < min([for (p = fr_bosses) abs(p[1])]) - (frame_boss_d + 3)/2 - 1),
       "frame: battery curb reaches a boss tower — shorten the curb caps");
assert(!bat_on || bat_l/2 + bat_clr + 2.8 < fr_xi/2 - plate_fillet - 1
                  && bat_w/2 + bat_clr + 2.8 + 1.6 < fr_yi/2 - plate_fillet - 1,
       "frame: battery bay (curb/fingers included) runs into the wall fillet ring");
// keyed ribs: each blade must straddle its portrait key's side-wall slot with
// bearing both sides, clear of the gill row, and its key must clear the
// LANDSCAPE case's solid bottom wall (the rib drop covers the key's 1.5
// proud plus 0.5 of air)
assert(!dock_keys || abs(dock_key_dx - stand_rib_x) < stand_rib_w/2 - 2.5,
       "stand: the centering key overhangs its rib — re-center stand_rib_x on dock_key_dx");
assert(stand_rib_drop >= 1.5 + 0.5,
       "stand: rib drop too small — the portrait keys would foul the landscape case's bottom wall");
assert(stand_rib_x - stand_rib_w/2 > gill_y0 + (gill_n - 1)*11 + gill_w/2 + 0.2
    && -(stand_rib_x - stand_rib_w/2) < gill_y0 - gill_w/2,
       "stand: keyed rib lands on the gill row in portrait");
assert(stand_rib_x + stand_rib_w/2 < fr_yo/2 - fr_ro && stand_rib_x > fr_yo/4,
       "stand: keyed ribs miss the portrait case's flat or stand too narrow a stance");
// the portrait keying slots on the ±x walls must sit past the gill row and
// off the corner
assert(!dock_keys
    || (dock_key_dx - gill_w/2 > gill_y0 + (gill_n - 1)*11 + gill_w/2 + 1
     && -(dock_key_dx + gill_w/2) < gill_y0 - gill_w/2 - 1
     && dock_key_dx + gill_l/2 + 0.5 < fr_yi/2 - fr_ri),
       "frame: portrait keying slot collides with the gill row or the wall corner");
// the landscape keying slots: between the brand words and the wall corner
// on the case, and on the cheek pads (not the well, not off the dock) on the
// stand — and the portrait case must still miss the cheeks entirely, or the
// ribs no longer carry it
assert(!dock_keys || (dock_key_bx - gill_w/2 - 2 > vword_x + vword_half
    && dock_key_bx + gill_w/2 + 2 < fr_xi/2 - fr_ri),
       "frame: landscape keying slot lands on the brand words or off the wall's flat span");
assert(!dock_keys || (dock_key_bx - 2 > std_open/2 && dock_key_bx + 2 < stand_w/2),
       "stand: landscape key misses the cheek pad — move dock_key_bx over it");
assert(fr_yo/2 + 2 < std_open/2,
       "stand: the portrait case no longer misses the cheeks — the well ribs cannot carry it");
echo(str("Canary 7in touch v0.9-dev — outer ", xo, " x ", yo, " x ", bez_h + cav_d + back_t,
         " mm, window ", view_w, " x ", view_h, ", lip ", lip_min,
         " mm, tray grille ~",
         round(vent_back ? grille_area(grille_cells())/100 : 0),
         " cm2 open (computed, boss dodges included)",
         "  (IN DEVELOPMENT — MEASURE CONNECTORS)"));
// The panel this case is for, read straight off the registry record — so a
// build log says which BOARD it was cut for, and how strong that claim is.
echo(str("  panel: ", panel_summary(PANEL),
         pnl_status(PANEL) == "measured" ? ""
                : str("  <-- status \"", pnl_status(PANEL),
                      "\": not confirmed on hardware")));
echo(str("  stack: floor ", z_floor, " | PCB under ", z_pcb_under, " | PCB top ",
         z_pcb_top, " | glass back ", z_glass, " | closed height ",
         back_t + cav_d + bez_h, " mm"));
echo(str("  frame: ", fr_xo, " x ", fr_yo, " x ", fr_depth,
         " mm one-piece; glass opening ", fr_xi, " x ", fr_yi, " r", fr_ri,
         "; front face ",
         glass_guard < 0
           ? str(abs(glass_guard), " mm BELOW the glass — wipe relief. Pass: a ",
                 "finger run off the glass steps DOWN onto the case, never up ",
                 "into an edge, and a cloth does not catch")
           : glass_guard == 0
             ? "FLUSH with the glass (fingernail test: no step either way)"
             : str("proud of the glass by ", glass_guard,
                   " mm — a recess IS the pass criterion"),
         ", plate fillet ",
         plate_fillet, "; adhesive ledge face ", ledge_z,
         " mm behind the front (bare-glass ",
         "border ", glass_edge_t, " + adhesive ", adh_t,
         "); 4x M3x8 from the back into the panel standoffs (boss face at ",
         fz_boss, ", head seat at ", fz_plate, ")"));
echo(str("  frame mounting: 4x keyhole, ", back_t + khm_pad_t,
         " mm bearing under each screw head (", back_t, " plate + ", khm_pad_t,
         " doubler), chamfered mouths; adhesive rails ",
         adh_rails ? str("2x ", adh_rail_w, " x ", adh_rail_l, " at x = ±",
                         adh_rail_dx, " — fits 15.9 x 70 mm interlocking ",
                         "picture-hanging strip pairs, kept smooth inside ",
                         "the moat outlines (case pulls off its wall halves ",
                         "first; the tabs are then exposed)")
                   : "off"));
// The grille's honest number, and what the grade cost it. `flat` is the SAME
// cell list with the gradient switched off, so the comparison is like for
// like rather than against a remembered figure — and it is the number to
// argue with if the case runs hot.
// THE GRILLE'S HONEST NUMBER, and what the badge cost it. Two lists, one pass
// each, so the attribution comes out of the same predicate the cutter uses
// rather than out of a memory of what the plate used to be:
//   fr_cells      what is actually cut
//   ..._nobadge   the same beat and the same keepouts, with the badge's claim
//                 dropped — i.e. the most this field could give
fr_cells         = grille_cells(-m3_ox, m3_oy, fr_keepouts);
fr_cells_nobadge = grille_cells(-m3_ox, m3_oy,
                                concat(fr_keep_fixed, fr_keep_rails));
// How many of the stations the BEAT itself declines. This is where "as many as
// we need and no more" is actually spent, so it is reported as a count rather
// than left implicit in the pattern string.
vent_beat_hits = len([for (ch = vent_beat) if (ch == "#") 1]);
// With the grille off the plate is a COMPOSITION, so it gets reported as one —
// and the report still has to name the thermal cost, priced by the same
// predicate, or "we took the vents out" becomes a decision nobody can audit.
// fr_cells_nobadge is what the field would give with the badge's claim
// dropped: the ceiling, not a promise.
if (!plate_grille)
    echo(str("  frame back plate: NO HOLES AT ALL — no grille, no vent eggs, ",
             "no radio window. It carries the badge, the lockup locked under ",
             "it",
             qr_back ? str(", the help QR (", qr_n, "x", qr_n, " at ",
                           qr_back_cell, " mm, ", 2*qr_back_reach,
                           " mm square with its quiet zone, at (",
                           qr_back_dx, ", ", qr_back_dy, "))") : "",
             " and the small print. The only openings left in this face are ",
             "the card window and the four keyholes, all of which something ",
             "goes through.",
             // ── the thermal cost, priced by the SAME predicate that cuts ──
             // Two figures, because ONE of them was misleading. The number
             // worth quoting is what the field would give with the badge's
             // claim dropped, and the recovery advice used to name only
             // plate_grille — which restores the field WITH the badge column
             // still reserved, i.e. a quarter of what the sentence promised.
             // Advice that leaves a warm build under the area it advertised
             // is worse than no advice.
             " WHAT THAT COSTS: turning the grille back on (plate_grille) is ",
             len(fr_cells), " eggs ≈ ", round(grille_area(fr_cells)/100),
             " cm2, because the badge still owns its half of the plate; ",
             "plate_grille AND badge_column = false is ",
             len(fr_cells_nobadge), " eggs ≈ ",
             round(grille_area(fr_cells_nobadge)/100),
             " cm2, which is the whole field and the number this plate is ",
             "giving up. The convection path (bottom-wall intake to top-wall ",
             "exhaust) is not on this plate and is untouched, and NOTHING IN ",
             "THIS REPO MEASURES THE REQUIREMENT — if a built case runs warm, ",
             "those are the two knobs, in that order."));
if (plate_grille)
echo(str("  frame back grille: ", len(fr_cells), " eggs at ", vent_slot_l,
         " x ", vent_slot_w, " ≈ ", round(grille_area(fr_cells)/100),
         " cm2 open. Beat \"", vent_beat, "\" — ", vent_beat_hits, " of ",
         len(vent_beat), " stations carry an egg, rotated ", vent_beat_shift,
         " per row, so the clutch groups instead of tiling",
         !back_bird ? "" : str(
           ". The BADGE owns the other half of the plate: the same beat with ",
           "its claim dropped would be ", len(fr_cells_nobadge), " eggs ≈ ",
           round(grille_area(fr_cells_nobadge)/100), " cm2, so the mark is ",
           "worth ", round((grille_area(fr_cells_nobadge)
                            - grille_area(fr_cells))/100),
           " cm2 of back-plate open area. If this case runs warm, bird_h and ",
           "badge_column are the knobs, and the beat is the cheap one — every ",
           "'.' you turn into a '#' is ",
           round(egg_area(vent_slot_l, vent_slot_w, vent_tip)),
           " mm2 back. The convection path itself (bottom-wall intake to ",
           "top-wall exhaust) is not on this plate and is untouched."),
         adh_rails ? str(" — the adhesive rails cost ",
             len(grille_cells(-m3_ox, m3_oy, fr_keep_base))
             - len(fr_cells), " eggs (adh_rails=false reclaims them)") : ""));
echo(str("  frame two-color (optional, single extruder): prints back-plate-",
         "down — ACCENT BACK SKIN: start in the accent color, swap to the ",
         "body color at z = ", frame_rim, " mm (every deboss floor sits at ",
         label_back_depth, " mm, so BOOT/RESET/SD, the brand line and the ",
         "rail moats read in the body color); ACCENT FRONT RING: swap back ",
         "to the accent at z = ", fr_depth - frame_foot, " mm — the last ",
         frame_foot, " mm of the print is only the front rim and its entry ",
         "chamfer"));
// Which per-filament parts actually EXIST for the active palette. An empty
// group list means that filament's part renders as an empty object and
// OpenSCAD writes no file at all, so naming it in the recipe is worse than
// useless. Derived from the same lists the geometry partitions on.
ink_live    = len(ink_groups) > 0 || bezel_color == "ink" || vent_ring_color == "ink";
accent_live = len(accent_groups) > 0 || bezel_color == "accent"
              || vent_ring_color == "accent";
fil_live    = str("fil_body (", pal_body, ")",
                  ink_live    ? str(", fil_ink (", pal_ink, ")") : "",
                  accent_live ? str(", fil_accent (", pal_accent, ")") : "");
fil_live_n  = 1 + (ink_live ? 1 : 0) + (accent_live ? 1 : 0);
if (print_colors)
    echo(str("  frame ", fil_live_n, "-COLOR (P2S + AMS): export ", fil_live,
             " — and ONLY those. A filament with no groups renders an EMPTY",
             " object and OpenSCAD writes NO FILE for it, so a recipe naming",
             " all three sends you hunting for a file that was never created.",
             " They share part=\"frame\"'s orientation, so load ",
             "fil_body then Add part → Load the rest and do NOT re-center",
             " or drop-to-bed. INK takes the front bezel ring (the last ",
             bezel_ink_t, " mm of the print) plus ", ink_groups,
             "; ACCENT takes ", accent_groups, ". Tool changes are confined to",
             " print z 0..", label_back_depth, " (the back skin, ~",
             ceil(label_back_depth/0.2), " layers at 0.2) and the single swap",
             " at z = ", fr_depth - bezel_ink_t, " — the ",
             fr_depth - label_back_depth - bezel_ink_t,
             " mm of shell between them never changes filament, so the purge ",
             "tower stays short. The QR reads ",
             qr_plaque
               ? str(pal_body, " modules on a ", grp_pal("qr"), " plaque (that ",
                     "filament prints as the FIELD and the modules are ",
                     "punched through it)")
               : str(grp_pal("qr"), " modules straight onto the ", pal_body,
                     " plate (that filament prints as the MODULES; the field",
                     " is the case)"),
             qr_dark_on_light
               ? " — dark-on-light, the polarity the spec asks for"
               : " — LIGHT-ON-DARK, inverted from the spec and chosen for looks",
             // This used to end "do not put the accent on the finder
             // patterns", which is now self-contradictory: the accent IS the
             // symbol. The real rule it was reaching for survives — the
             // modules and the field must be ONE filament each, never mixed
             // within the symbol — and that holds by construction, since the
             // whole "qr" group takes a single filament.
             ". Modules and field are one filament each by construction; scan",
             " the coupon (part=\"coupon_qr_body\"/\"coupon_qr_fill\") before",
             " committing a frame"));
echo(str("  stand: ", stand_w, " x ", std_d, " base, ", stand_ang,
         "° recline, slot ", std_cd, " mm for the ", fr_depth,
         " mm frame, seat ", stand_floor_h, " mm over the desk (plug room), ",
         "well ", std_open - 6, " mm across the intake vents; landscape on the",
         " cheek pads keyed at ±", dock_key_bx, ", portrait on ribs at ±",
         stand_rib_x, " keyed at ±", dock_key_dx,
         " (cable exits sideways in portrait)"));
if (qr_help)
    echo(str("  stand help QR: \"", qr_url(), "\" — ", qr_n, "x", qr_n, " at ",
         qr_cell, " mm (", qr_field, " mm field) on the deck at (", qr_dx,
         ", ", qr_dy_eff, "); the bare deck is the quiet zone. TWO-COLOR (single",
         " extruder, zero purge): print base-down in the BODY color, swap to",
         " the ACCENT at z = ", stand_plate_t - 0.4, " — the deck skin prints",
         " light and the module floors stay dark. Swap back at z = ",
         stand_plate_t, " to keep the blades in the body color (two swaps),",
         " or ride the accent to the top for a two-tone dock (one swap);",
         " either way the only filament spent is the filament in the part"));
if (mount_portrait && mount_keyholes)
    echo(str("  portrait hanging: keyholes carry ", khm_plen,
         " mm side slides — rotate the case 90° either way and a side",
         " pair becomes the level top pair, mouths ", 2*khm_y + khm_len,
         " mm apart (second row ", khm_dx*2, " mm below for 4-screw).",
         " Same screws, same hang-and-drop. Screen rotation is a firmware",
         " setting the UI does not have yet — the hardware is ready first",
         len(khm_dropped) == 0 ? "" : str(". ", len(khm_dropped),
         " slide(s) NOT cut (would open into an M3 head pocket — see",
         " khm_slide_ok): ", [for (d = khm_dropped)
             str(d[0] > 0 ? "+x" : "-x", d[1] > 0 ? " top" : " bottom",
                 " keyhole, ", d[2] > 0 ? "+x" : "-x", " leg")],
         " — that portrait orientation pins on the other three keyholes")));
if (bat_on)
    echo(str("  battery bay: MakerFocus ", battery, " mAh (", bat_dims[0],
         " x ", bat_dims[1], " x ", bat_dims[2], " nominal, thickness held at +",
         bat_tol, " tol) against the back plate — case deepened ", bat_extra,
         " mm to ", fr_depth, ". Air BOTH faces: ", bat_rib,
         " mm rail channel over the grille slots, ", bat_air,
         " mm to the board over a ", bat_over,
         " mm component ceiling (MEASURE yours). Click in over the two",
         " wedge-faced fingers, press both to release; PH2.0 lead exits at",
         " either short-end curb gap to the panel's battery header.",
         " Print the desk dock with the SAME battery setting — its slot",
         " tracks fr_depth. Heat/care: 1C pack, ≤3 A draw, charge ≤2 A;",
         " if the pack area runs warm to the touch, stop and re-measure",
         " bat_over — the board-side gap is the one that matters"));
if (qr_draw)
    echo(str("  frame help QR: \"", qr_url(), "\" — ", qr_n, "x", qr_n, " at ",
         qr_back_cell, " mm (", qr_n*qr_back_cell, " mm field) on the back",
         " plate at (", qr_back_dx, ", ", qr_back_dy, "), its grille keepout",
         " doubling as the quiet zone. Prints in the FIRST layers on the",
         " textured plate. Style \"", qr_style, "\": ",
         qr_plaque
           ? str("the ", grp_pal("qr"), " inlay prints as the PLAQUE — a field",
                 " with the modules punched through it, showing ", pal_body)
           : str("the ", grp_pal("qr"), " inlay prints as the MODULES",
                 " themselves, straight onto the ", pal_body, " plate, so the",
                 " field is the case and there is no rectangle around it"),
         qr_dark_on_light
           ? ". Polarity: dark modules on a light field — what the spec asks"
           : str(". Polarity: LIGHT MODULES ON A DARK FIELD — inverted from",
                 " the spec, chosen for looks. Plenty of current phone",
                 " cameras decode an inverted symbol and plenty of older",
                 " readers refuse outright; nothing in this repo can settle",
                 " it for your phones. If it will not scan, set qr_style =",
                 " \"plaque\""),
         ". It costs",
         " no extra swap: the back skin is already a tool change. Rehearse it",
         " with part=\"coupon_qr_body\"/\"coupon_qr_fill\" and SCAN THE COUPON",
         " before committing a frame — at this polarity that is not a",
         " formality, and cell size is the other thing only a phone can",
         " settle"));
if (usb_port)
    echo(str("  USB port: ", usb_open_w, " x ", usb_open_h,
             " stadium at (", usb_dx, ", z ", usb_z, ") — passes a ",
             usb_head_w, " x ", usb_head_h, " head; grommet grips a Ø",
             usb_wire_d, " jacket. TPU fitments (grommet_usb / plug_buttons / ",
             "plug_sd): TPU 90-95A, EXTERNAL spool, never the AMS"));
if (side_exit != "none")
    echo(str("  side exit: same ", usb_open_w, " x ", usb_open_h,
             " stadium through the ", side_exit,
             side_exit == "right" ? " (+x, microSD's side)" : " (-x)",
             " wall at dy ", side_dy, " — hung portrait with that wall down",
             " it is the bottom exit; one grommet serves either port, the",
             " leashed BLANK (plug_port) fills the other"));
echo(str("  BOOT/RESET towers: ", btn_tower_len, " mm from the lip plane (",
         btn_reach > 0 ? "MEASURED override" : "AUTO from the panel record",
         ") — tips REST on the caps at the board edge",
         plug_tether ? str("; plug leashed to the top wall at dx ",
             plug_teth_dx, " (cost: one exhaust slot — its station is the anchor)")
                     : " (plug_tether OFF: no leash, no slot dropped)"));
// When the board is the bigger part, the tray cavity opens up to clear it and
// stops being what locates the slab — the bezel's glass pocket takes that job.
// Worth saying out loud, because it changes which part you check first if the
// panel ends up sitting crooked.
// (Phrased without the word CI greps for; this is a note, not a build failure.)
if (pcb_w > glass_w || pcb_h > glass_h)
    echo(str("  NOTE: PCB overhangs the glass by ",
             max(pcb_w - glass_w, 0)/2, " mm X / ",
             max(pcb_h - glass_h, 0)/2, " mm Y, so the tray cavity is board-sized; ",
             "the bezel's glass pocket is what centers the slab"));

// Exported for canary_s3_lcd7_fitcheck.scad, so the assembly check reads the
// real derived stack instead of a copy that can drift out of step with it.
// [back_t, cav_d, bez_h, glass_t, glass_w, glass_h, glass_r, z_glass]
function lcd7_stack() = [back_t, cav_d, bez_h, glass_t,
                         glass_w, glass_h, glass_r, z_glass];
// Same idea for the dock, so the fitcheck can seat the real frame in the real
// stand instead of trusting a re-derived copy of either. fr_xo rides along so
// the portrait pose (case on its side) can be checked too.
// [stand_ang, std_ys, stand_floor_h, fr_depth, fr_yo, fr_xo, stand_rib_drop]
// (the last entry is how far below the pad plane PORTRAIT seats, on the ribs)
function lcd7_stand_stack() = [stand_ang, std_ys, stand_floor_h, fr_depth,
                               fr_yo, fr_xo, stand_rib_drop];

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
        // viewing window: the active area opened by win_margin all round
        translate([0, aa_dy, -0.1]) linear_extrude(face_t + 0.2) rrect2d(view_w, view_h, 3);
        // glass pocket behind the face ledge — sized to the SLAB, not the tray
        // cavity, so the bezel locates the panel laterally as well as holding it
        translate([0, 0, face_t]) linear_extrude(bez_h) rrect2d(xg, yg, r_cav);
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
// ox/oy: where the M3 boss pattern actually sits (the measured pattern lands
// INSIDE the grille field, so slots must now dodge the bosses — the v0.2
// pattern sat outside it and never could collide). keepouts: extra [x,y,hw,hh]
// rectangles to dodge (the frame passes its cable slots).
// The grille's surviving slot centers — a FUNCTION, shared by the cutter
// below and the open-area echoes, so the area the console reports is
// computed by the same predicate that cuts the slots and cannot drift.
// A slot survives if it sits inside the PCB footprint, off the bosses, and
// out of every caller-supplied [x, y, half_w, half_h] keepout rectangle.
// Does this station carry an egg? The beat is read across the row and rotated
// by the row index, so the groups walk sideways as you go up the plate instead
// of stacking into columns of rests.
//
// Rotation, not a hash: a pseudo-random field would also avoid stripes, and it
// would look like a fault. A clutch is grouped, and grouping is periodic.
function vent_hit(c, r) =
    let (n = len(vent_beat),
         i = ((c + r*vent_beat_shift) % n + n) % n)
    vent_beat[i] == "#";

function grille_cells(ox = m3_ox, oy = m3_oy, keepouts = []) =
    [for (r = [0:vent_rows-1], c = [0:vent_cols-1])
        let (x = (c - (vent_cols-1)/2) * vent_pitch_x
                 + (r % 2 == 1 ? vent_pitch_x/2 : 0) - vent_pitch_x/4,
             y = (r - (vent_rows-1)/2) * vent_pitch_y)
        if (vent_hit(c, r) && abs(x) < pcb_w/2 - 6 && abs(y) < pcb_h/2 - 6
            && min([for (sx = [1,-1], sy = [1,-1])
                   max(abs(x - (ox + sx*m3_dx/2)) - 6,
                       abs(y - (oy + sy*m3_dy/2)) - 9)]) > 0
            && (len(keepouts) == 0 ||
                min([for (k = keepouts)
                    max(abs(x - k[0]) - k[2], abs(y - k[1]) - k[3])]) > 0))
        [x, y]];
// Open area of a cell list, in mm2 — every egg the same egg, so this is the
// count times the shoelace area of the SAME polygon the cutter draws.
function grille_area(cells) =
    len(cells) * egg_area(vent_slot_l, vent_slot_w, vent_tip);
// The radio can's window. A plain through rectangle — no countersink, no lip,
// no cover: the card window has all three because a human works it, and this
// one is read, never touched.
module soc_window(mx = false) {
    translate([(mx ? -1 : 1)*soc_dx, soc_dy, -0.1])
        linear_extrude(back_t + 0.2) rrect2d(soc_w, soc_h, soc_r);
}

module vent_grille(ox = m3_ox, oy = m3_oy, keepouts = []) {
    for (p = grille_cells(ox, oy, keepouts))
        translate([p[0], p[1] - vent_slot_l/2 + vent_slot_w/2, -0.1])
            linear_extrude(back_t + 0.2)
                egg2d(vent_slot_l, vent_slot_w, vent_tip);
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
                difference() { outline2d(); rrect2d(xc, yc, r_cav); }
            // PCB standoff bosses at the M3 pattern (offset — see m3_ox/m3_oy),
            // each with a 45° root fillet: a self-tapped boss fails by
            // shearing at its root, so spread that section into the floor
            for (sx = [1,-1], sy = [1,-1])
                translate([m3_ox + sx*m3_dx/2, m3_oy + sy*m3_dy/2, back_t - 0.01]) {
                    cylinder(d = 7.0, h = comp_h);
                    cylinder(d1 = 7.0 + 3, d2 = 7.0, h = 1.51);
                }
        }
        // M3 boss pilots
        for (sx = [1,-1], sy = [1,-1])
            translate([m3_ox + sx*m3_dx/2, m3_oy + sy*m3_dy/2, back_t + comp_h - 6]) cylinder(d = m3_pilot, h = 6.2);
        // case-screw self-tap pilots down the TOP of each full-height corner post
        // (the bezel's counterbored ear delivers the M3 into these)
        for (p = lobes())
            translate([p[0], p[1], back_t + cav_d - 12]) cylinder(d = lob_pilot, h = 12.1);
        // HEAT: back grille. It takes the RADIO WINDOW's keepout for the
        // same reason the frame's does: the two cuts overlap near the window's
        // corner, and a grille egg landing on the boundary turns a plain
        // rectangle into a rectangle with bites out of it — and takes plate
        // beyond the shield can's footprint while it is there. The tray built
        // this without a keepout for one commit; the window is off by default
        // now, so the defect was invisible in the shipped part and would have
        // surfaced on whoever turned soc_win on first.
        if (vent_back) vent_grille(keepouts = soc_cut
            ? [[-soc_dx, soc_dy, soc_w/2 + vent_slot_w/2 + 1.5,
                                 soc_h/2 + vent_slot_l/2 + 1.5]] : []);
        // FCC/IC marking window. x is negated because this part is modeled
        // front-side while the plate coordinates every graphic is authored in
        // are BACK-view — the same mirror the frame expresses as -m3_ox. The
        // tray carries no badge, so it gets the window and not the nest.
        if (soc_cut) soc_window(mx = true);
        // Connector openings span the band the rear-side connectors actually
        // occupy: the tray floor up to the PCB underside. (v0.1 measured this
        // from the glass instead, and left 3.3 mm of wall across their bottoms.)
        if (opt_bottom_ports)
            translate([bottom_open_dx, -yo/2, port_z])
                cube([bottom_open_w, wall*3, port_h], center = true);
        if (opt_side_ports) for (sx = [1,-1])
            translate([sx*xo/2, side_open_dy, port_z])
                cube([wall*3, side_open_h, port_h], center = true);
        // CONVECTION: intake low, exhaust high. The board is narrower than the
        // cavity, so the wall slots open into clear air beside/above it.
        if (vent_top) for (i = [0:vent_top_n-1])
            translate([(i - (vent_top_n-1)/2) * (vent_span_x/vent_top_n), yo/2, wall_vent_z])
                cube([vent_slot_w, wall*3, wall_vent_h], center = true);
        if (vent_bottom) for (sx = [1,-1], i = [0:vent_bottom_n-1])
            translate([sx*(bottom_open_w/2 + 6 + i*(vent_slot_w + 3.5)), -yo/2, wall_vent_z])
                cube([vent_slot_w, wall*3, wall_vent_h], center = true);
        // Side chimneys, parked above the port slot so they stay separate
        // openings rather than merging into one ragged hole.
        if (vent_sides) for (sx = [1,-1], i = [0:vent_side_n-1])
            translate([sx*xo/2, side_vent_y0 + i*(vent_slot_w + 4), wall_vent_z])
                cube([wall*3, vent_slot_w, wall_vent_h], center = true);
    }
}

// ----------------------------------------------------------------------------
//  GAUGE — print THIS before the slab. One corner of the bezel and the matching
//  corner of the tray, cut out of the real parts by intersection (not re-drawn,
//  so they cannot drift from what you are about to print). Assemble the pair
//  around the panel's own corner with one M3 and check, in order:
//    1. the glass corner drops into the cavity without forcing
//    2. the lip lands on the black border and the window clears the pixels
//    3. the M3 threads into the post, and the PCB boss meets its mount hole
//    4. calipers across the closed pair read the echoed closed height
//  Anything wrong here is a 20-minute mistake instead of a multi-hour one.
// ----------------------------------------------------------------------------
gauge_in = 34;   // how far inward from the cavity corner the sample reaches

module gauge_corner(what) {
    intersection() {
        if (what == "bezel") bezel(); else back();
        translate([xc/2 - gauge_in, yc/2 - gauge_in, -1])
            cube([gauge_in + lob_d + wall + 6, gauge_in + lob_d + wall + 6,
                  back_t + cav_d + bez_h + 2]);
    }
}
// "gauge" lays the pair out for preview; the two halves also export
// individually, because each STL has to be a single watertight body.
module gauge() {
    gauge_corner("back");
    translate([0, -(gauge_in + lob_d + wall + 10), 0]) gauge_corner("bezel");
}

// ----------------------------------------------------------------------------
//  FRAME — one-piece drop-in case (prints FACE-DOWN: z0 = front outer face,
//  +z toward the back; +x = BACK-view right). No ledge holds the glass: the
//  4 screws pull the panel's standoffs onto the boss faces, and that stack —
//  glass_t + pcb_standoff + pcb_t + standoff_len — is exactly what sets the
//  glass glass_guard below the front rim. Get standoff_len right or the
//  glass face is off by the same error. glass_guard is -0.2 today (proud).
// ----------------------------------------------------------------------------
module pill2d(l, w) { hull() for (d = [-1, 1]) translate([0, d*(l - w)/2]) circle(d = w); }
// FEATHER BARB — the house pattern motif, and the reason it is a motif at all
// is that it prints better than the stadium it replaces. Two big circles
// intersected give a vesica: a leaf pointed at BOTH ends. On the dock's
// near-vertical fin that top apex is self-supporting, where a stadium's flat
// crown is a bridge the width of the vent. Same envelope (l x w) as pill2d,
// so it drops in wherever a pill was. r is the circle radius whose lens is
// exactly l tall and w wide: r = (l^2 + w^2) / 4w.
// NB intersection_for, not intersection(){ for ... } — a for loop is ONE
// child (a group), so intersection() over it quietly returns the UNION, and
// the two circles then swallow the entire fin. This builtin exists for
// exactly this case.
// (named barb2d, not egg2d: the brand grille's mark — round base, rounded
// crown — lives in canary_vent_lib.scad, and two different shapes cannot
// share a name. A local definition silently shadows a use<>d one, which is
// how the grille's 3-argument calls started warning.)
module barb2d(l, w) {
    r = (l*l + w*w)/(4*w);
    intersection_for (s = [1, -1]) translate([s*(r - w/2), 0]) circle(r);
}
// The vent shape the dock actually cuts — one switch for the whole pattern.
module vent2d(l, w) { if (barb_vents) barb2d(l, w); else pill2d(l, w); }
// Back-plate deboss: label_back_depth, not label_depth — the floors must sit
// above the two-color swap band (see the knob's comment).
// QR modules, shaped for a NOZZLE. Cutting one square per dark module
// creates two things a 0.42 line cannot trace: diagonally adjacent
// modules meet at a SINGLE POINT (non-manifold in 3D, a knife edge in
// 2D — and every QR has diagonal neighbors), and every corner is a
// hard 90°, which the outer wall whips around at speed and PETG blobs.
// One morphological OPENING fixes both: erode by rr, then dilate by rr.
// The erosion breaks every zero-width neck; the dilation restores each
// module to full size with rr-radius corners, so nothing narrower than
// a line survives anywhere in the field. Scannability is unharmed: the
// three FINDER patterns a reader locks onto are 7x7 blocks, and a
// radius this small leaves them square; the data modules keep their
// centers and their area, which is what the sampling grid reads.
qr_round = 0.22;   // module corner radius, as a fraction of the module
module qr_field2d(cell, round = qr_round) {
    rr = round*cell;
    offset(r = rr) offset(r = -rr)
        for (r = [0:qr_n-1], c = [0:qr_n-1])
            if (qr_bits()[r][c] == 1)
                translate([c*cell, -(r+1)*cell])
                    square(cell + qr_bleed);   // orthogonal neighbors
}                                              // union without a seam

module frame_lbl(x, y, s, size = 4.0, spacing = 1.0) {
    translate([x, y, fr_depth - label_back_depth])
        linear_extrude(label_back_depth + 0.1)
        text(s, size = size, font = label_font, spacing = spacing,
             halign = "center", valign = "center");
}

// ----------------------------------------------------------------------------
//  BACK-PLATE GRAPHICS — ONE tool, two consumers.
//
//  frame() SUBTRACTS this to cut the debosses. The color parts INTERSECT the
//  same volume to fill them. That is the whole trick: an inlay cut from the
//  same solid as its recess cannot drift from it, so "the yellow part fits the
//  yellow hole" is true by construction rather than by a number kept in sync
//  in two places.
//
//  `ink` selects a color group, so regrouping the palette is a one-word edit
//  here rather than a hunt through frame():
//    "text"  BOOT / RESET / SD and the rating block — the functional labels
//    "mark"  the lockup: SECURACV / CANARY / the maker row
//    "bird"  the house mark itself
//    "qr"    the help symbol's modules
//    "moat"  the adhesive rails' outline hairlines
//    "all"   every group (what frame() cuts)
//
//  Everything here lands in the same z band — the outer skin, label_back_depth
//  deep — which is why a three-color print costs so little: every tool change
//  is confined to the first ~1.2 mm of a 24 mm part. See PRINT COLORS.
// ----------------------------------------------------------------------------
module back_graphics(ink = "all") {
    all = ink == "all";
    // THE QR COUPON CARRIES THE SYMBOL AND NOTHING ELSE. Everything below
    // except the QR is suppressed for it, and that is not tidiness — the
    // coupon is a 37.7 mm square cut out of the plate at the symbol's
    // position, and at this mark size the badge's own strokes run straight
    // through that square. Left in, they print as recesses across the finder
    // patterns (the coupon has no accent volume to fill them), and a failed
    // scan would be telling you about the bird rather than about the cell size
    // or the polarity you printed it to test. A test that can fail for the
    // wrong reason is not a test.
    if (!qr_coupon && (all || ink == "moat"))
        if (adh_rails) for (sx = [1, -1])
            translate([sx*adh_rail_dx, 0, fr_depth - label_back_depth])
                linear_extrude(label_back_depth + 0.1) difference() {
                    rrect2d(adh_rail_w + 2*adh_mark_w, adh_rail_l + 2*adh_mark_w,
                            2 + adh_mark_w);
                    rrect2d(adh_rail_w, adh_rail_l, 2);
                }
    if (!qr_coupon && (all || ink == "text")) {
        // back view, buttons at the TOP: BOOT on the left (-x here), RESET on
        // the right, as on the board; "SD" beside the card window so nobody
        // hunts for the socket
        frame_lbl(btn_dx - btn_lbl_dx, fr_yo/2 - 6.5, "BOOT");
        frame_lbl(btn_dx + btn_lbl_dx, fr_yo/2 - 6.5, "RESET");
        // "SD" sits beside the tether channel (its old spot above the mouth
        // is exactly where the strap now runs)
        frame_lbl(sd_dx - 8, sd_teth_y, "SD");
        // spec block — stacked lines in the right-hand column, above the card
        // window; the help URL closes the column under it
        if (rating_stamp) for (i = [0:len(rating_lines)-1])
            frame_lbl(rating_dx, rating_dy + rating_h/2 - 1
                                 - (i + 0.5)*rating_lh,
                      rating_lines[i], rating_sz);
        if (help_line != "")
            frame_lbl(rating_dx, help_dy, help_line, help_sz);
    }
    if (!qr_coupon && (all || ink == "bird"))
        // The mark, in the clear middle the adhesive rails used to own.
        // mark_bird() does the design-unit bookkeeping — the scale off the
        // library's own span, and the shift by its bbox center — so bird_dx/dy
        // place the MARK's middle rather than the paths' arbitrary origin.
        if (back_bird)
            translate([bird_dx, bird_dy, fr_depth - label_back_depth])
                linear_extrude(label_back_depth + 0.1)
                    mark_bird(bird_h, bird_rib);
    if (!qr_coupon && (all || ink == "mark")) {
        // THE LOCKUP — all three rows, and with the bird the only thing on
        // the plate that takes the accent. The maker row joined it when it
        // left the rating block: product, company, maker are one mark and
        // therefore one filament, the same argument that moved the hero line
        // in. The hero product name used to live in "text" alongside
        // BOOT/RESET/SD; that was fine when "text" was ink and the accent was
        // one word, but it meant CANARY and SECURACV could not be colored
        // together without dragging the functional labels along with them.
        // They are one mark, so they are one group.
        //
        // All three rows ride brand_dx — the MARK's column, not the plate's.
        // The company line is tracked wide and set smaller than the product
        // name: on the back of a product the product's name is the headline.
        frame_lbl(brand_dx, brand_sub_y, brand_sub,
                  size = brand_sub_sz, spacing = 2.0);
        frame_lbl(brand_dx, brand_hero_y, brand_back,
                  size = brand_sz, spacing = 1.15);
        // ...and the maker, closing the stack. Narrow and un-tracked so it
        // reads as the footnote it is rather than as a third brand line.
        frame_lbl(brand_dx, brand_maker_y,
                  str(brand_maker, " ", lcd7_stamp_year()),
                  size = brand_maker_sz, spacing = 1.0);
    }
    if (all || ink == "qr")
        // In back-view coords, no mirror: viewed from the back, +x is right.
        //
        // POLARITY FOLLOWS THE PALETTE. A reader wants DARK modules on a LIGHT
        // field, and which of our two filaments is light stopped being a
        // constant the moment the body went from white to black. On a light
        // plate the INK is the modules and the bare plate is the field. On a
        // dark plate that is backwards — white modules on black is the one
        // arrangement a scanner is entitled to refuse — so the ink becomes the
        // FIELD instead, a light plaque with the modules punched out of it in
        // body color. Same two filaments, same inlay machinery, opposite
        // assignment, and it is decided by measuring the palette rather than by
        // remembering to think about it.
        if (qr_draw)
            translate([qr_back_dx - qr_n*qr_back_cell/2,
                       qr_back_dy + qr_n*qr_back_cell/2,
                       fr_depth - label_back_depth])
                linear_extrude(label_back_depth + 0.1)
                    if (qr_plaque)
                        difference() {
                            // the light plaque: the symbol PLUS its quiet zone,
                            // which is qr_back_reach by definition, recentered
                            // on the field this translate is anchored off
                            translate([qr_n*qr_back_cell/2, -qr_n*qr_back_cell/2])
                                square([2*qr_back_reach, 2*qr_back_reach],
                                       center = true);
                            qr_field2d(qr_back_cell);   // modules punched out
                        }
                    else
                        qr_field2d(qr_back_cell);
}

// ----------------------------------------------------------------------------
//  PER-FILAMENT PARTS — see PRINT COLORS for the palette and the recipe.
//
//  The three parts PARTITION the frame: no overlap, no gap, and their union is
//  frame() exactly. That is not a claim, it is gated (fil_overlap / fil_gap).
//  It matters because a gap prints as a void inside a wall and an overlap
//  prints twice — neither is visible in a slicer preview until it is a part.
// ----------------------------------------------------------------------------

// The INK band at the front face. ONE slab serves both sides of the split, so
// the bezel and the body are exact complements — the same plane, evaluated
// once, rather than two numbers that agree until someone edits one.
module bezel_slab() {
    translate([-fr_xo/2 - 1, -fr_yo/2 - 1, -1])
        cube([fr_xo + 2, fr_yo + 2, 1 + bezel_ink_t]);
}

// A deboss group, filled flush with the skin. Clipped to the skin band because
// the cut tools deliberately overshoot by 0.1 to keep the difference clean —
// an inlay that inherited that overshoot would stand proud of the plate.
module back_inlay(groups) {
    intersection() {
        union() { for (g = groups) back_graphics(g); }
        translate([-fr_xo/2 - 1, -fr_yo/2 - 1, fr_depth - label_back_depth])
            cube([fr_xo + 2, fr_yo + 2, label_back_depth]);
    }
}

// The color coupon's clip — the bottom-center band. See the part dispatch
// for what it is for and why a corner coupon cannot do the same job.
// The clip reaches from the center lockup OUT TO A CORNER, deliberately
// asymmetric. A centered band proves the colors but nothing about fit; a
// corner proves the fit but has no accent word on it (SECURACV lives at
// x = 0). Spanning both makes one print answer both questions:
//   · center end — the lockup: INK product name over ACCENT company line
//   · corner end — the glass pocket's radius and the front bezel band, so
//     the panel's own corner can be offered up to it
// It runs the full depth, so the front rim and the back plate are both on it.
coupon_x0 = -40;              // just past the lockup's center
coupon_x1 = fr_xo/2 + 1;      // ...out through the corner
// Tall enough for BOTH jobs, and the lockup's reach is DERIVED rather than
// typed. It was a flat 34, which is right only for as long as the wordmark
// stays pinned to the plate's bottom edge: lift the lockup and the band ends
// up containing no accent geometry at all, and the color coupon — whose entire
// job is to rehearse the palette — silently becomes a single-color part.
// gen_3mf.py does say so out loud ("EMPTY: the palette puts no accent on this
// object"), which is that warning earning its place, but the coupon should not
// need a human to notice. So the band asks the lockup where its top row is.
//
// At stock dims the CORNER's requirement still wins — the arc needs 34 and the
// lockup only needs ~18.5 — so this changes no geometry today. It is here for
// the day the rows move: a lockup lifted mid-plate would want ~53, a ~55%
// taller coupon, and at that point the QR's note below applies — past some
// height a second small plaque is cheaper than the plate a taller band drags
// along. The band's virtue is answering registration AND corner fit in ONE
// print, and that trade is worth re-reading before growing it.
coupon_h_corner = 34;         // the corner arc's own requirement, unchanged
coupon_h  = max(coupon_h_corner,
                (brand_sub_y + _lbl_half(brand_sz) + 3) - (-fr_yo/2 - 1));
module coupon_clip() {
    translate([coupon_x0, -fr_yo/2 - 1, -1])
        cube([coupon_x1 - coupon_x0, coupon_h, fr_depth + 2]);
}

// The QR SCAN coupon's clip — a separate plaque, not part of the band above.
//
// Why separate. The help QR sits at (qr_back_dx, qr_back_dy) on the mid-left
// wing; the color band runs along the bottom edge. They do not overlap in y
// at all, so growing the band to swallow the QR would add ~40 mm of frame
// height — nearly all of it blank plate — to capture a 37.7 mm square that
// lives nowhere near the lockup. A separate plaque asks the same question for
// roughly a third of the filament, and keeps the band coupon's two jobs
// (three-color registration, corner fit) uncluttered.
//
// Why it is only the BACK PLATE and not the full depth. The frame prints
// back-plate-down, so the QR is in the first layers; a full-depth clip would
// print 23.5 mm of shell to test a graphic that is finished 3 mm in. The
// plaque is the plate itself, which is exactly the substrate the real QR
// prints on — same thickness, same first layer, same deboss floors.
//
// What it tests, none of which a slicer preview answers:
//   · whether the symbol SCANS at qr_back_cell — the real question, and one
//     only a phone can settle. 1.3 mm modules are ~3 line widths at 0.42.
//   · the polarity — AND ON THIS BUILD THAT IS THE WHOLE POINT OF THE
//     COUPON, not a formality. qr_style is "bare": the modules print in INK
//     straight onto the black plate, so the symbol is WHITE ON BLACK. That
//     is INVERTED from the polarity the spec asks for, chosen deliberately
//     for looks. Many current phone cameras decode an inverted symbol; it is
//     not guaranteed, and no gate in this repo can settle it. Scan this
//     coupon with the phones that actually matter to you. If it fails, set
//     qr_style = "plaque" and the symbol goes back to dark-on-light inside
//     a light rectangle.
//     Do not restate the assignment as a fixed one; it rotted once
//   · purge bleed on the deboss floors, where black-into-white shows first
//     and where it would cost the symbol its contrast
// Print it, scan it with a phone, and only then commit a frame.
// The plaque stops EXACTLY at qr_back_reach — do not add a margin "for a nicer
// border". qr_back_reach is the same radius the asserts above guarantee is
// clear of rail moats, grille slots and keyholes; anything past it is plate
// the model never promised was empty. A 1.5 mm border was the first thing
// tried here and it clipped an adhesive-rail moat, putting a detached 0.8 mm
// hairline of INK at the plaque's edge — a loose sliver in the wrong filament,
// and a fragment of a feature that has nothing to do with the QR.
coupon_qr_margin = 0;
module coupon_qr_clip() {
    s = qr_back_reach + coupon_qr_margin;
    translate([qr_back_dx, qr_back_dy, fr_depth - back_t])
        linear_extrude(back_t + 1)      // +1 overshoots into free space; the
            rrect2d(2*s, 2*s, 3);       // frame bounds the intersection anyway
}

// VENT ACCENT — the yellow you catch when you look into the back grille.
//
// This is a COLOR PARTITION, not a geometry change, and that distinction is
// the whole reason it is safe. frame() is bit-for-bit what it was: no new
// void, no counterbore, no overhang, no change to open area or to any airflow
// assert. All that moves is which filament prints a ring of plate that was
// already solid — so there is nothing here that can fail as a PRINT that was
// not already failing.
//
// It is free in tool changes too, which is the other half of the argument.
// The back skin (print z 0 … label_back_depth) is ALREADY a three-filament
// band — every deboss floor lives there — so accent is being laid on these
// layers regardless. Adding area to it costs no swap and no purge.
//
// WHY NOT THE SIDE GILLS. They sit in the shell band, print z 1.2 … 22.9:
// 21.7 mm of continuous body-only printing with zero swaps, and that unbroken
// run is exactly what keeps the purge tower short. Accenting a gill would buy
// two tool changes at a mid-shell layer and pay purge for both. Same for the
// top exhaust — it is wall, not plate. The back grille is the only vent set
// that is already inside a color band.
//
// The ring is flush at the surface AND lines the first vent_accent_d of the
// bore, which is what sells it: straight on it reads as a thin gold outline,
// at an angle you see into the hole and the inside is yellow.
//
// COST, honestly: 66 slots means 66 small isolated accent islands on each of
// these layers. Not filament and not swaps — travel moves and stringing risk,
// on two layers. That is the thing to watch on the first print;
// vent_ring_color = "body" is one switch if it misbehaves, and a WIDER ring
// is the fix to try before abandoning it (two fat extrusions beat many thin).
// Ring color is vent_ring_color, up in the palette block with the other
// filament routing. "body" (the default) makes this module empty, so the
// mouths are simply part of the case and no ring geometry exists at all.
vent_accent_w = 0.8;    // ring width outward from the hole edge — 2 lines at 0.4
vent_accent_d = 0.4;    // how far down the bore it runs — 2 layers at 0.2
vent_accent_eps = 0.05; // seam offset INTO the hole — see the note at the cut

module vent_accent_rings() {
    if (vent_ring_color != "body" && plate_grille)
        translate([0, 0, fr_depth - vent_accent_d])
            linear_extrude(vent_accent_d + 0.1)   // overshoots; frame() bounds it
                for (p = grille_cells(-m3_ox, m3_oy, fr_keepouts))
                    translate([p[0], p[1] - vent_slot_l/2 + vent_slot_w/2])
                        difference() {
                            offset(r = vent_accent_w)
                                egg2d(vent_slot_l, vent_slot_w, vent_tip);
                            // NOT the bare egg. The vent hole is cut with
                            // exactly this profile, so a bare inner boundary
                            // lands on the hole's own wall — the same plane,
                            // twice — and CGAL answers with zero-area sheets:
                            // "2 edges not shared by exactly two faces", which
                            // is a mesh a slicer refuses. Shrinking the
                            // subtrahend pushes the seam a hair INTO the hole,
                            // where there is no material either way, so the
                            // partition is unchanged and the surfaces are not
                            // coplanar. Same fix as the dock's phantom
                            // over-lip walls (#1373).
                            offset(r = -vent_accent_eps)
                                egg2d(vent_slot_l, vent_slot_w, vent_tip);
                        }
}

// Everything a non-body filament claims BEYOND its deboss groups: the bezel
// band and the vent-mouth rings, each routed by name. Written once and used by
// all three parts so the split cannot drift — a surface claimed here is the
// same surface subtracted in frame_bodycol(), because it is the same module.
//
// Intersecting with frame() is what keeps it exact where a ring runs into a
// deboss void or off the plate edge: the claiming filament takes only material
// that is actually there, and body subtracts the identical tool.
module claimed_by(which) {
    if (bezel_color == which)     intersection() { frame(); bezel_slab(); }
    if (vent_ring_color == which) intersection() { frame(); vent_accent_rings(); }
}
module frame_ink()    { union() { back_inlay(ink_groups);    claimed_by("ink"); } }
module frame_accent() { union() { back_inlay(accent_groups); claimed_by("accent"); } }
// The filament the QR's modules actually land on, so the scan coupon can be
// cut from it without naming a spool that the group lists may have moved.
module frame_qrfill() {
    f = grp_fil("qr");
    if (f == "accent")   frame_accent();
    else if (f == "ink") frame_ink();
    else                 frame_bodycol();
}
// Subtract only what another filament actually claimed. With both knobs on
// "body" nothing is subtracted at all and the case is one solid color — which
// is the point: "black bezel" must cost zero tool changes, not a black inlay.
module frame_bodycol(){
    difference() {
        frame();
        if (bezel_color != "body")     bezel_slab();
        if (vent_ring_color != "body") vent_accent_rings();
    }
}

// A rough color key. PREVIEW ONLY — never export from this module.
//
// ⚠️  READ BEFORE TRUSTING A RENDER OF THIS. OpenSCAD's OpenCSG preview does
// not reliably attribute color to these inlays: they sit inside the recesses
// they were cut from, sharing side walls with the body, and the preview pass
// hands the whole set whichever color was applied last. Renders of this
// module have shown every back-plate group in the ACCENT color when only the
// company line is accent. Raising pal_preview_lift to stand the inlays clear
// of their recesses does NOT fix it — tried at 0.02, 0.35 and 4.0.
//
// The exported meshes are unaffected and correct: render part="fil_ink" and
// part="fil_accent" separately and each contains exactly its own groups.
// Those per-part renders — not this one — are what to check a palette against,
// and they are what the fil_overlap / fil_gap gates prove.
//
// Kept because the silhouette and the bezel band do read correctly, which is
// enough to judge proportion. Do not use it to judge which word is yellow.
pal_preview_lift = 0.02;
module frame_color() {
    color(pal_body_rgb)   frame_bodycol();
    color(pal_ink_rgb)    translate([0, 0, pal_preview_lift]) frame_ink();
    color(pal_accent_rgb) translate([0, 0, pal_preview_lift]) frame_accent();
}

// The shell, finished at both ends: a modeled foot chamfer at the plate (the
// catalog's standing elephant-foot allowance — slicer compensation stays 0)
// and a matching chamfer around the back rim so the printed part reads as a
// finished object from every side.
frame_foot = 0.6;  frame_rim = 0.8;
module frame_body() {
    hull() {
        linear_extrude(0.01)
            rrect2d(fr_xo - 2*frame_foot, fr_yo - 2*frame_foot, max(1, fr_ro - frame_foot));
        translate([0, 0, frame_foot]) linear_extrude(0.01) rrect2d(fr_xo, fr_yo, fr_ro);
    }
    translate([0, 0, frame_foot])
        linear_extrude(fr_depth - frame_foot - frame_rim) rrect2d(fr_xo, fr_yo, fr_ro);
    hull() {
        translate([0, 0, fr_depth - frame_rim]) linear_extrude(0.01)
            rrect2d(fr_xo, fr_yo, fr_ro);
        translate([0, 0, fr_depth - 0.01]) linear_extrude(0.01)
            rrect2d(fr_xo - 2*frame_rim, fr_yo - 2*frame_rim, max(1, fr_ro - frame_rim));
    }
}

// Battery bay furniture on the back plate's inner face. The frame prints
// back-plate-down, so every piece here rises straight off the bed side —
// self-supporting, like the bosses. All spans are cut to clear the four
// M3 boss towers (asserted) and the plate fillet ring.
module frame_bat_bay() {
    px = bat_l/2 + bat_clr;  py = bat_w/2 + bat_clr;   // pocket half-extents
    curb_h = bat_rib + 2.5;
    xr = min(px - 2, 48);    // long-edge curb reach (stops short of the bosses)
    yr = min(py - 2, 24);    // short-end curb reach (ditto)
    // rails: the pack rides these, the grille beneath breathes through
    for (rx = [-0.35*bat_l, 0, 0.35*bat_l])
        translate([rx - 2, -py + 1, fz_plate - bat_rib])
            cube([4, 2*py - 2, bat_rib + 0.01]);
    // curb: two segments per long edge (finger gap at center), one per
    // short end split by the PH2.0 lead gap
    for (sy = [1, -1], sx = [1, -1])
        translate([sx == 1 ? 6 : -xr, sy*(py + 0.2) - (sy == 1 ? 0 : 2),
                   fz_plate - curb_h])
            cube([xr - 6, 2, curb_h + 0.01]);
    for (sx = [1, -1], sy = [1, -1])
        translate([sx*(px + 0.2) - (sx == 1 ? 0 : 2), sy == 1 ? 8 : -yr,
                   fz_plate - curb_h])
            cube([2, yr - 8, curb_h + 0.01]);
    // one cantilever finger per long edge: 45° lead-in below the tip, then
    // a shallow wedge grip face — a thick pack meets it near the tip, a
    // thin one rides 2 mm further up the same wedge, so the vendor's ±2
    // stays preloaded against the rails. Press both fingers to release.
    grip = 1.6;
    zh   = fz_plate - bat_rib - bat_t;         // thickest pack's front face
    for (sy = [1, -1]) scale([1, sy, 1])
        translate([-5, 0, 0]) rotate([90, 0, 90]) linear_extrude(10)
            polygon([[py + 0.4, fz_plate],            // root, inner face
                     [py + 0.4, zh + 3.5],            // wedge start
                     [py + 0.4 - grip, zh],           // hook tip
                     [py + 0.4, zh - grip],           // 45° lead-in
                     [py + 0.4, zh - 2.5],            // post tip, inner
                     [py + 2.8, zh - 2.5],            // post tip, outer
                     [py + 2.8, fz_plate]]);          // root, outer
}
// The pack itself, seated: shared by the three battery fit gates below.
// `drop` hovers the brick below the rail plane — the fit probe needs it,
// because a face lying EXACTLY on the rails' undersides makes CGAL emit
// zero-volume phantom sheets (the #1373 failure class), which read as a
// collision that holds no material.
module bat_brick(inset = 0, t = bat_t, drop = 0)
    translate([-(bat_l/2 - inset), -(bat_w/2 - inset),
               fz_plate - bat_rib - t - drop])
        cube([bat_l - 2*inset, bat_w - 2*inset, t]);

// ONE cable pass-through, cut into a chosen wall — the reason it is a module
// is that the bottom exit and the side exit must be the SAME opening, or the
// one grommet part stops fitting both. edge: 0 = bottom (-y), +1 = right
// (+x), -1 = left (-x); pos runs along that wall. Every wall gets the same
// stadium, the same 45° mouth bevel, and the same ledge relief behind it.
module port_cut(edge = 0, pos = 0) {
    // rotate(+90) maps +local x to +world y, rotate(-90) maps it to -world y
    // — so without this the documented "+ = toward the top edge" silently
    // reversed on the left wall. One sign, applied to the port and to its
    // leash anchor together, keeps both walls reading the same way.
    sgn = edge == 0 ? 1 : edge;
    // leash anchor: a plain hole through the wall beside the port. The barb
    // is pushed in from OUTSIDE with a thumb and mushrooms behind the wall,
    // so the hole needs no counterbore — the wall's own inner face is the
    // shoulder. This is what must exist in the printed case for the fitment
    // to be captive at all: print the case AFTER this change, or the barb
    // has nowhere to go (gated by port_teth_hole / port_teth_barb).
    ri  = edge == 0 ? fr_yi/2 : fr_xi/2;      // inner wall face
    ro  = edge == 0 ? fr_yo/2 : fr_xo/2;      // outer skin
    lg  = edge == 0 ? ledge_bot : ledge_side; // the ledge this wall carries
    rot = edge == 0 ? 0 : edge*90;            // -y swings to ±x
    rotate([0, 0, rot]) {
        translate([pos*sgn, -ri + lg + 0.6, usb_z])
            rotate([90, 0, 0]) linear_extrude(frame_wall + lg + 1.2)
                rotate(90) pill2d(usb_open_w, usb_open_h);
        // 45° mouth bevel at the skin — the grommet's cone lands on it, and
        // an unused port still reads finished
        hull() {
            translate([pos*sgn, -ro + 0.01, usb_z]) rotate([90, 0, 0])
                linear_extrude(0.02) rotate(90)
                    pill2d(usb_open_w + 1.6, usb_open_h + 1.6);
            translate([pos*sgn, -ro + 0.81, usb_z]) rotate([90, 0, 0])
                linear_extrude(0.02) rotate(90) pill2d(usb_open_w, usb_open_h);
        }
        // relieve the ledge ring + wedge behind the port so the grommet's
        // inner flange lands on a flat wall face
        translate([pos*sgn, -ri + (lg + 1)/2 - 0.01, ledge_z + (ledge_t + lg)/2])
            cube([usb_open_w + 2*grom_lip + 2, lg + 1,
                  ledge_t + lg + 0.6], center = true);
        if (port_tether) {
            translate([(pos + port_teth_dx)*sgn, -ri - 1, usb_z])
                rotate([-90, 0, 0])
                    cylinder(d = sd_teth_hole, h = frame_wall + lg + 3);
            // and a counterbore at the OUTER mouth that the mushroom seats
            // INSIDE — the barb's shaft is cut to stop on this pocket's
            // floor, so the head parks flush with the skin. (It used to run
            // past the wall and flare in open air, 2.8 mm proud of the face
            // in plain view, with this pocket behind it doing nothing.)
            translate([(pos + port_teth_dx)*sgn, -ro - 0.01, usb_z])
                rotate([-90, 0, 0])
                    cylinder(d = sd_teth_head + 0.8, h = port_teth_cb + 0.01);
        }
        // fingernail peel dish, opposite the anchor — the nail lands under
        // the flange's rim and levers the fitment out. sgn carries it to
        // whichever wall the port is in, keeping it opposite the anchor on
        // both.
        if (port_scoop)
            translate([(pos + port_scoop_dx)*sgn, -ro - port_scoop_d, usb_z])
                sphere(r = port_scoop_r);
    }
}

module frame() {
    gz = key_gz;                                // center of the clear air band
    gh = fz_boss - glass_guard - glass_t - 4;   // wall-vent height inside it
    difference() {
        union() {
            difference() {
                frame_body();
                // one straight cavity, snug to the SLAB (the widest thing in
                // the stack — the board hangs inboard of it on its standoffs)
                translate([0, 0, -0.1]) linear_extrude(fz_plate + 0.1)
                    rrect2d(fr_xi, fr_yi, fr_ri);
            }
            // bosses hanging from the back plate's inner face, each with a
            // 45° root fillet — the screws' clamp load spreads into the
            // plate through a wider section instead of a sharp corner
            for (p = fr_bosses) translate([p[0], p[1], fz_boss]) {
                cylinder(d = frame_boss_d, h = fz_plate - fz_boss + 0.01);
                translate([0, 0, fz_plate - fz_boss - 1.5])
                    cylinder(d1 = frame_boss_d, d2 = frame_boss_d + 3, h = 1.51);
            }
            if (bat_on) frame_bat_bay();
            // keyhole DOUBLER pads on the plate's inner face: the wall
            // screw's head clamps back_t + khm_pad_t of material and the
            // slide's catch shears a wider section. They sit in the clear
            // band behind the plate, above the component band (asserted).
            if (mount_keyholes) for (sx = [1, -1], sy = [1, -1])
                translate([sx*khm_dx, sy > 0 ? khm_y : -(khm_y + khm_len),
                           fz_plate - khm_pad_t])
                    linear_extrude(khm_pad_t + 0.01) hull() {
                        circle(d = khm_head_d + 2*khm_pad_w);
                        translate([0, khm_len])
                            circle(d = khm_slide_w + 2*khm_pad_w);
                        if (mount_portrait) for (px = [1, -1])
                            if (khm_slide_ok(sx, sy, px))
                                translate([px*khm_plen, 0])
                                    circle(d = khm_slide_w + 2*khm_pad_w);
                    }
            // 45° fillet ring where the walls meet the back plate — the
            // drop-load path: a corner drop flexes the walls against the
            // plate, and the square internal corner there is the crack
            // starter (boss-root doctrine, applied to the whole perimeter).
            // It lives in the clear band with the keyhole pads, behind the
            // component tip plane (fz_boss), and the wall openings are cut
            // AFTER this union, so every vent/port that crosses the band
            // pierces it — nothing seals. Prints plate-down: the ring
            // tapers as it rises, so it needs no support.
            if (plate_fillet > 0) difference() {
                translate([0, 0, fz_plate - plate_fillet])
                    linear_extrude(plate_fillet + 0.01)
                        rrect2d(fr_xi + 0.04, fr_yi + 0.04, fr_ri);
                hull() {
                    translate([0, 0, fz_plate - plate_fillet - 0.02])
                        linear_extrude(0.01) rrect2d(fr_xi + 0.1, fr_yi + 0.1, fr_ri);
                    translate([0, 0, fz_plate + 0.02]) linear_extrude(0.01)
                        rrect2d(fr_xi - 2*plate_fillet, fr_yi - 2*plate_fillet,
                                max(0.8, fr_ri - plate_fillet));
                }
            }
            // adhesive ledge behind the glass: a full ring at the glass-back
            // datum, widened where the panel's adhesive strips are (10 mm
            // sides, 6 mm button edge, 2 mm over the FPC)
            translate([0, 0, ledge_z]) linear_extrude(ledge_t) difference() {
                rrect2d(fr_xi, fr_yi, fr_ri);
                translate([0, (ledge_bot - ledge_top)/2])
                    rrect2d(fr_xi - 2*ledge_side, fr_yi - ledge_top - ledge_bot, 2);
            }
            // ...and the ledge's 45° back-slope to its wall: solid support
            // under the adhesive landing, self-supporting when the frame
            // prints back-plate-down.
            //
            // Built by GROWING the ledge's inner opening outward as we step
            // back, rather than as three straight prisms. The prisms ran
            // linear_extrude(fr_yi - 2*fr_ri) and friends — i.e. the straight
            // spans only — so all four CORNERS had a bare flat ledge back face
            // hanging over the cavity with nothing under it. A slicer flags it
            // as a floating cantilever, correctly; it bridges, but it did not
            // need to exist. offset() grows the opening uniformly, corners
            // included, so the 45° face is now continuous all the way round.
            //
            // Stepped rather than swept because the three edges are different
            // widths (10 / 6 / 2), so there is no single profile to revolve.
            // Each step is ledge_slope_step tall and overhangs by the same,
            // which is a 45° staircase — self-supporting at any step size; the
            // step only sets how smooth the face looks.
            for (i = [0 : ledge_slope_n - 1]) {
                h = ledge_slope_max * i / ledge_slope_n;
                translate([0, 0, ledge_z + ledge_t + h])
                    linear_extrude(ledge_slope_max/ledge_slope_n + 0.01)
                        difference() {
                            rrect2d(fr_xi, fr_yi, fr_ri);
                            offset(r = h)
                                translate([0, (ledge_bot - ledge_top)/2])
                                    rrect2d(fr_xi - 2*ledge_side,
                                            fr_yi - ledge_top - ledge_bot, 2);
                        }
            }
        }
        // glass entry chamfer around the front rim
        hull() {
            translate([0, 0, -0.01]) linear_extrude(0.02)
                rrect2d(fr_xi + 1.2, fr_yi + 1.2, fr_ri + 0.6);
            translate([0, 0, 0.6]) linear_extrude(0.02) rrect2d(fr_xi, fr_yi, fr_ri);
        }
        // M3 clearance through each boss; head pocket through the plate. The
        // head bears on the step where the two meet — that is what closes the
        // stack (see the module header).
        for (p = fr_bosses) {
            translate([p[0], p[1], fz_boss - 0.1]) cylinder(d = screw_c, h = frame_boss_h + 0.2);
            translate([p[0], p[1], fz_plate - 0.01]) cylinder(d = cb_d + 0.4, h = back_t + 0.11);
        }
        // BOOT/RESET window through the TOP wall (the button edge in native
        // mounting), with a 45° bevelled surround on the outer face
        translate([btn_dx, fr_yi/2 - 0.1, btn_zc]) rotate([-90, 0, 0])
            linear_extrude(frame_wall + 0.3) rrect2d(btn_w, btn_h, 3);
        hull() {
            translate([btn_dx, fr_yo/2 - 0.01, btn_zc]) rotate([-90, 0, 0])
                linear_extrude(0.02) rrect2d(btn_w + 2.4, btn_h + 2.4, 4);
            translate([btn_dx, fr_yo/2 - 1.2, btn_zc]) rotate([-90, 0, 0])
                linear_extrude(0.02) rrect2d(btn_w, btn_h, 3);
        }
        // relieve the ledge landing + wedge behind the window's span — the
        // top-edge wedge (ledge_z up to ledge_z+ledge_t+ledge_top) otherwise
        // stands right behind the window's lower half, blocking finger and
        // plug-pip access to the buttons (the frame_btn_window gate caught
        // this). The landing goes with the wedge: kept alone it would print
        // as an unsupported shelf back-plate-down. The panel's button-edge
        // adhesive strip simply skips the window span, same trade as the
        // USB port's relief on the FPC edge.
        translate([btn_dx, fr_yi/2 - (ledge_top + 1)/2 + 0.01,
                   ledge_z + (ledge_t + ledge_top)/2])
            cube([btn_w + 4, ledge_top + 1, ledge_t + ledge_top + 0.6],
                 center = true);
        // gill vents down each ±x wall — cut deep enough to pass through the
        // ledge wedge behind the wall, so they vent the cavity, not the wedge
        if (gill_n > 0) for (sx = [1, -1], i = [0 : gill_n - 1])
            translate([sx*(fr_xo/2 - frame_wall/2), gill_y0 + i*11, gz])
                rotate([sx*gill_rake, 0, 0]) rotate([0, 90, 0])
                    translate([0, 0, -(ledge_side + frame_wall)])
                        linear_extrude(2*(ledge_side + frame_wall))
                            translate([0, -gill_l/2 + gill_vw/2])
                                egg2d(gill_l, gill_vw, vent_tip);
        // exhaust through the top wall, flanking the button window. A slot
        // whose station the plug leash anchor needs is NOT cut — one, at
        // stock dims: the innermost -x slot sits exactly at plug_teth_dx —
        // and the echo reports the trade. Distance predicate, not an index,
        // so moving the anchor drops whichever slot it actually lands on.
        for (sx = [1, -1], i = [0 : frame_vent_flank_n - 1])
            if (!plug_tether || abs(sx*(btn_w/2 + 9 + i*6.5) - plug_teth_dx)
                                > gill_w/2 + sd_teth_hole/2 + 1.5)
                translate([btn_dx + sx*(btn_w/2 + 9 + i*6.5), fr_yi/2 - 0.1, gz])
                    rotate([-90, 0, 0]) linear_extrude(frame_wall + 0.3) pill2d(gh, gill_w);
        // plug leash anchor beside the window: flush strap channel into the
        // outer skin (from the window's bevel surround to just past the
        // anchor), the anchor hole through the remaining wall, and a 45°
        // entry chamfer at the channel floor that the barb's root cone
        // nests into — the SD tether's cut set, transplanted to the wall.
        // The barb mushrooms in the open cavity behind (the exhaust band is
        // clear air inside; nothing to relieve).
        if (plug_tether) {
            sT = plug_teth_dx < 0 ? -1 : 1;
            x0 = btn_dx + sT*(btn_w/2 + 1.0);
            x1 = btn_dx + plug_teth_dx + sT*2.5;
            translate([min(x0, x1), fr_yo/2 - plug_chan_d, btn_zc - 2.5])
                cube([abs(x1 - x0), plug_chan_d + 0.1, 5.0]);
            translate([btn_dx + plug_teth_dx, fr_yi/2 - 1, btn_zc])
                rotate([-90, 0, 0]) cylinder(d = sd_teth_hole, h = frame_wall + 2);
            translate([btn_dx + plug_teth_dx, fr_yo/2 - plug_chan_d - 0.451, btn_zc])
                rotate([-90, 0, 0])
                    cylinder(d1 = sd_teth_hole, d2 = sd_teth_hole + 0.9, h = 0.46);
        }
        // dock keying, PORTRAIT: one gill-style slot in each ±x wall at
        // dy = ±dock_key_dx, past the end of the gill row — the studs on the
        // desk dock's well ribs rise into them, so whichever side wall faces
        // down in portrait, the case self-centers.
        if (dock_keys) for (sx = [1, -1], sy = [1, -1])
            translate([sx*(fr_xo/2 - frame_wall/2), sy*dock_key_dx, gz])
                rotate([0, 90, 0]) translate([0, 0, -(ledge_side + frame_wall)])
                    linear_extrude(2*(ledge_side + frame_wall))
                        pill2d(gill_l, gill_w);
        // ...and LANDSCAPE: one through the bottom wall at ±dock_key_bx,
        // outboard of the brand words, engaged by the studs on the dock's
        // cheek pads. Cut through the FPC-edge ledge band like the shadow
        // gills, so on a wall-mount build they breathe as two more intakes.
        if (dock_keys) for (sx = [1, -1])
            translate([sx*dock_key_bx, -fr_yi/2 + ledge_bot + 0.6, gz])
                rotate([90, 0, 0]) linear_extrude(frame_wall + ledge_bot + 1.2)
                    pill2d(gh, gill_w);
        // USB pass-through, centered on the bottom wall: a true stadium sized
        // to pass the power cable's overmold head, cut through the wall AND
        // the FPC-edge ledge/wedge behind it. The grommet fills it afterwards.
        if (usb_port) port_cut(0, usb_dx);
        if (side_exit != "none") port_cut(side_exit == "right" ? 1 : -1, side_dy);
        // port labels — same deboss depth as the brand words, set just
        // outboard of each opening's flange so a fitted grommet never
        // covers them
        if (port_labels && usb_port)
            translate([vword_x + vword_half + 6.0,
                       -fr_yo/2 + label_depth, usb_z])
                rotate([90, 0, 0]) linear_extrude(label_depth + 0.2)
                    text(port_lbl_a, size = 3.6, font = label_font,
                         halign = "left", valign = "center");
        // (after the ±90° swing the wall in play is the ±x one, so its skin is
        // at fr_xo/2 — fr_yo/2 put the label 41 mm inside the cavity, unseen
        // only because port_lbl_b ships empty)
        if (port_labels && side_exit != "none")
            rotate([0, 0, side_exit == "right" ? 90 : -90])
                translate([(side_exit == "right" ? 1 : -1)
                               *(side_dy + usb_open_w/2 + grom_lip + 3.5),
                           -fr_xo/2 + label_depth, usb_z])
                    rotate([90, 0, 0]) linear_extrude(label_depth + 0.2)
                        text(port_lbl_b, size = 3.6, font = label_font,
                             halign = "left", valign = "center");
        // the brand words flank the port as CRISP DEBOSS — the slat-stencil
        // vents this replaces needed tie bands across every glyph, which
        // read as horizontal scan lines on the first print. A deboss stays
        // attached by the wall web behind it: no ties, no lines. Readable
        // standing below and in front, letter tops toward the screen.
        for (s = [1, -1])
            translate([0, -fr_yo/2 + edge_lbl_depth, usb_z])
                rotate([90, 0, 0]) rotate([0, 0, 180]) translate([-s*vword_x, 0])
                    linear_extrude(edge_lbl_depth + 0.1)
                        text(s > 0 ? edge_text_l : edge_text_r,
                             size = edge_text_size, font = label_font,
                             spacing = 1.12, halign = "center",
                             valign = "center");
        // ...and the intake the stencil carried moves to a SHADOW GILL row
        // tucked into the wall band's last few mm before the back plate:
        // invisible against a wall or over the dock's well, roughly the
        // stencil's open area, clear of the grommet's flange and the plate
        // (all asserted). Cut through the FPC-edge ledge band like every
        // bottom-wall opening, so it vents the cavity.
        for (i = [0 : edge_vent_n - 1])
            translate([(i - (edge_vent_n - 1)/2)*edge_vent_pitch,
                       -fr_yi/2 + ledge_bot + 0.6, edge_vz])
                rotate([90, 0, 0]) linear_extrude(frame_wall + ledge_bot + 1.2)
                    pill2d(edge_vent_h, edge_vent_w);
        // back grille (dodging bosses and the keyholes — note -m3_ox: this
        // part is modeled print-side, x mirrored vs the two-part tray)
        // (keepouts hoisted to fr_keepouts, where the open-area echo reads
        // the same lists — see the derived section)
        // ...gated on plate_grille, which the frame did NOT honor until this
        // revision: it cut this field unconditionally while the tray honored
        // vent_back and the open-area echo honored vent_back. It never showed,
        // because nothing had ever switched the field off — the first time
        // anything did, the console reported a bare plate and the part printed
        // a lattice. A knob the geometry ignores is worse than no knob, because
        // every other surface agrees with you.
        if (plate_grille) translate([0, 0, fz_plate]) vent_grille(-m3_ox, m3_oy,
            keepouts = fr_keepouts);
        // the radio can's window, so the module's FCC/IC marking reads on the
        // outside of the finished case
        if (soc_cut) translate([0, 0, fz_plate]) soc_window();
        // microSD access through the back plate: socket + slide travel +
        // fingertip, so the card comes out without being dropped inside
        translate([sd_dx, sd_dy, fz_plate - 0.1])
            linear_extrude(back_t + 0.2) rrect2d(sd_w, sd_l, 5);
        // countersunk seat for the SD cover: a 45° rim from the outer skin
        // down to the opening. Prints back-plate-down with ZERO bridges —
        // the flat-floored recess this replaces left a cantilevered ring
        // hanging over the opening, which drooped on the first real print.
        hull() {
            translate([sd_dx, sd_dy, fr_depth - sd_lip]) linear_extrude(0.01)
                rrect2d(sd_w + 0.2, sd_l + 0.2, 5.1);
            translate([sd_dx, sd_dy, fr_depth - 0.01]) linear_extrude(0.02)
                rrect2d(sd_w + 2*sd_lip + 0.2, sd_l + 2*sd_lip + 0.2, 5 + sd_lip);
        }
        // fingernail scoop biting the mouth's card end — the nail lands
        // under the cover's tapered cap edge to peel it
        translate([sd_dx, sd_dy - sd_l/2 - sd_lip - 2.2, fr_depth + 7.0])
            sphere(r = 7.8);
        // tether anchor (see the sd_tether knobs): a shallow channel below
        // the outer skin, from the countersink's top rim to past the anchor
        // hole — the strap lies in it flush, so a wall or mounting strip
        // never pinches it — then the hole itself, entry-chamfered at the
        // channel floor so the arrowhead finds it; the barb mushrooms
        // against the plate's inner face
        if (sd_tether) {
            translate([sd_dx, (sd_dy + sd_l/2 - 1 + sd_teth_y + 2)/2,
                       fr_depth - 1.4]) linear_extrude(1.5)
                square([5.0, (sd_teth_y + 2) - (sd_dy + sd_l/2 - 1)],
                       center = true);
            translate([sd_dx, sd_teth_y, fz_plate - 0.1])
                cylinder(d = sd_teth_hole, h = back_t + 0.2);
            translate([sd_dx, sd_teth_y, fr_depth - 1.4 - 0.45])
                cylinder(d1 = sd_teth_hole, d2 = sd_teth_hole + 0.9, h = 0.46);
        }
        // wall-mount keyholes through the back plate, one per corner — the
        // bottom pair mirrors the top pair's POSITION but keeps the same
        // orientation (head hole low, slide running up, catch at the button
        // edge), so the case still hangs over the screws and slides DOWN,
        // now pinned flat to the wall at all four corners
        if (mount_keyholes) for (sx = [1, -1], sy = [1, -1])
            translate([sx*khm_dx, sy > 0 ? khm_y : -(khm_y + khm_len),
                       fz_plate - khm_pad_t - 0.1])
                linear_extrude(back_t + khm_pad_t + 0.2) {
                circle(d = khm_head_d);
                hull() { circle(d = khm_slide_w);
                         translate([0, khm_len]) circle(d = khm_slide_w); }
                // portrait slide: outboard, so the side pair that becomes
                // the TOP pair in a rotated hang gets its world-up slides.
                // khm_slide_ok drops the one leg that would open into an M3
                // head pocket (see the predicate at fr_bosses) — that mount
                // carries TWO slides instead of three, and the orientation
                // that misses it hangs on three screws.
                if (mount_portrait) for (px = [1, -1])
                    if (khm_slide_ok(sx, sy, px))
                        hull() { circle(d = khm_slide_w);
                                 translate([px*khm_plen, 0]) circle(d = khm_slide_w); }
            }
        // ...each with a lead-in chamfer around its mouth on the outer skin,
        // so the case slips over the screw heads without catching
        if (mount_keyholes) for (sx = [1, -1], sy = [1, -1])
            translate([sx*khm_dx, sy > 0 ? khm_y : -(khm_y + khm_len), 0]) {
                translate([0, 0, fr_depth - khm_mouth_c])
                    cylinder(d1 = khm_head_d,
                             d2 = khm_head_d + 2*(khm_mouth_c + 0.05),
                             h = khm_mouth_c + 0.05);
                hull() for (e = [0, 1])
                    translate([0, 0, fr_depth - khm_mouth_c
                                     + e*(khm_mouth_c + 0.05)])
                        linear_extrude(0.01) hull()
                            for (dy = [0, khm_len]) translate([0, dy])
                                circle(d = khm_slide_w + e*2*(khm_mouth_c + 0.05));
                if (mount_portrait) for (px = [1, -1])
                    if (khm_slide_ok(sx, sy, px))
                        hull() for (e = [0, 1])
                            translate([0, 0, fr_depth - khm_mouth_c
                                             + e*(khm_mouth_c + 0.05)])
                                linear_extrude(0.01) hull()
                                    for (dx = [0, px*khm_plen]) translate([dx, 0])
                                        circle(d = khm_slide_w + e*2*(khm_mouth_c + 0.05));
            }
        // Every deboss on the back skin — rail moats, labels, the lockup, the
        // rating block and the help QR — in one tool, so the color inlays
        // can be cut from the same solid. The layout notes live at the module.
        back_graphics();
        // build stamp, into the plate's INNER face (see the knobs)
        if (stamp_show) translate([0, stamp_dy, fz_plate - 0.01])
            mirror([1, 0, 0]) linear_extrude(stamp_depth + 0.01) {
                translate([0, stamp_size*0.9])
                    text("CANARY 7IN", size = stamp_size,
                         font = label_font, spacing = 1.1,
                         halign = "center", valign = "center");
                translate([0, -stamp_size*0.9])
                    text(str("REV ", lcd7_stamp_rev(), "  SRC ", lcd7_stamp_src()),
                         size = stamp_size*0.78, font = label_font,
                         halign = "center", valign = "center");
            }
    }
}

// One corner of the frame, cut from the real geometry by intersection — the
// (+x,+y) corner, chosen because it contains a boss AND a wall keyhole. Assemble
// it on the panel's corner with one M3x8: the glass corner proves glass_r,
// the screw only threads home if the m3 offsets have the right SIGNS, and the
// glass sits exactly glass_guard below the rim only if standoff_len is right.
// READ THE PASS CRITERION OFF glass_guard, not off memory. It is NEGATIVE
// today (wipe relief), so the pass is: run a finger from the glass out onto
// the case and it steps DOWN, never up into an edge. A rim you can feel as a
// ridge is a fail — that is the thing the first fitting print got wrong.
// The criterion moves with the knob, and has been all three things:
//   glass_guard > 0  -> a recess is the pass, flush means the stack is off
//   glass_guard = 0  -> flush is the pass, any step is a fail
//   glass_guard < 0  -> a step DOWN onto the case is the pass
// This comment has been stale twice. Read the variable, or read the echo,
// which computes the sentence rather than repeating it.
module frame_gauge() {
    bx = -m3_ox + m3_dx/2;  by = m3_oy + m3_dy/2;
    intersection() {
        frame();
        translate([bx - 12, by - 22, -1])
            cube([fr_xo/2 - bx + 14, fr_yo/2 - by + 24, fr_depth + 2]);
    }
}

// ----------------------------------------------------------------------------
//  TPU FITMENTS — print in TPU 90–95 A from an EXTERNAL spool (never the AMS;
//  it jams the hub — bambu_p2s_bringup.md §0). Each part exports in its print
//  orientation: A-face down on the textured plate, every retention feature
//  chamfered so nothing needs support. Their fits are tuned by tpu_squeeze
//  (waist interference) and tpu_grip (grommet bore), not tol_slide/tol_hole —
//  TPU seats by squeeze, rigid parts by clearance.
// ----------------------------------------------------------------------------
module stadium2d(w, h) { rotate(90) pill2d(w, h); }   // width w along x

// USB wire grommet. Local z: 0 = the inner flange's bearing face (on the bed).
// Install: unplugged, feed the cable head in through the port; plug it; open
// the grommet at its slit, wrap it around the wire, slide it to the wall and
// press the inner flange through the opening. A tug on the cable pulls the
// inner flange flat against the wall's inner face — the frame takes the load,
// not the board's connector. The slit faces the back plate when installed.
module usb_grommet(bore = true) {
    fw = usb_open_w + 2*grom_lip;  fh = usb_open_h + 2*grom_lip;
    ww = usb_open_w + 2*tpu_squeeze;  wh = usb_open_h + 2*tpu_squeeze;
    difference() {
        union() {
            // LEASH — the same strap-and-arrowhead the SD cover carries, so
            // neither the grommet nor the blank can be dropped and lost the
            // first time you service the cable.
            // The strap runs along the WALL — which is the grommet's local x,
            // not y: installed, the part is rotated 90 deg about x, so local
            // x stays world x while local y becomes the wall's depth. (It ran
            // up the wall the first time.) Everything lies on the bed at
            // z 0..1.2, so the leash prints flat with no support, and the
            // barb rises +z to pass outward through the anchor hole; the
            // frame counterbores that mouth so the mushroom sits flush
            // instead of proud on the edge you actually look at.
            if (port_tether) {
                hull() {
                    translate([-fw/4, -2, 0]) cube([fw/2, 4, 1.2]);   // root
                    translate([port_teth_dx - 2, -2, 0]) cube([4, 4, 1.2]);
                }
                // Shaft stops on the counterbore FLOOR so the mushroom flares
                // inside the pocket. The datum matters: installed, this part's
                // local z = 0 sits 1.6 mm inside the wall's inner face (the
                // inner flange's thickness), so the wall spans z 1.6 .. 1.6 +
                // frame_wall and the pocket floor is port_teth_cb below its
                // outer skin. The old h = 1.2 + frame_wall + 1.0 was measured
                // from no datum at all: it put the shaft's top 0.6 mm PAST the
                // skin and left the entire head standing in open air.
                translate([port_teth_dx, 0, 0]) {
                    cylinder(d = sd_teth_hole - 0.4,
                             h = 1.6 + frame_wall - port_teth_cb);
                    translate([0, 0, 1.6 + frame_wall - port_teth_cb - 0.01])
                        cylinder(d1 = sd_teth_head, d2 = 0.8,
                                 h = port_teth_cb);
                }
            }
            // inner flange — flat bearing face on the bed: this is the face
            // that carries cable tugs, so it is square, not chamfered
            linear_extrude(1.6) stadium2d(fw, fh);
            // waist, interference-fit through the wall
            translate([0, 0, 1.6 - 0.01])
                linear_extrude(frame_wall + 0.02) stadium2d(ww, wh);
            // 45° cone up to the outer flange — its tip lands in the port's
            // mouth bevel, so the two 45° faces meet in a clean shadow line
            hull() {
                translate([0, 0, 1.6 + frame_wall - 0.01])
                    linear_extrude(0.01) stadium2d(ww, wh);
                translate([0, 0, 1.6 + frame_wall + grom_lip - 0.2])
                    linear_extrude(0.01) stadium2d(fw, fh);
            }
            translate([0, 0, 1.6 + frame_wall + grom_lip - 0.21])
                linear_extrude(1.0) stadium2d(fw, fh);        // outer flange
            // strain-relief bell: supports the exiting wire's bend radius
            hull() {
                translate([0, 0, 1.6 + frame_wall + grom_lip + 0.78])
                    linear_extrude(0.01)
                        stadium2d(usb_open_w - 1, usb_open_h - 1);
                translate([0, 0, 1.6 + frame_wall + grom_lip + 0.79 + grom_bell])
                    linear_extrude(0.01) circle(d = usb_wire_d + 2.6);
            }
        }
        // bore grips the jacket; the slit lets the grommet open like a C and
        // wrap the wire after the head is already plugged in. bore = false
        // makes the BLANK for whichever exit the cable does not use — same
        // body, same squeeze, same install, no hole.
        if (bore) {
            cylinder(d = usb_wire_d - tpu_grip, h = 40, center = true);
            translate([-0.2, 0, -0.1]) cube([0.4, fh/2 + 1, 20]);
        }
    }
}

// BOOT/RESET plug. Local z: 0 = cap face (on the bed). The cap nests the
// window's 45° bevel with a hairline reveal; the waist squeezes the straight
// land; the 45° lip snaps out behind the wall — push it in from outside and
// it is captive: the cap can't pass inward, the lip won't fall out, and the
// buttons are pressed THROUGH it: two TOWERS under the face dimples run to
// the button caps (btn_tower_len — derived to REST on them) and carry the
// press. Pop it out to check a tower and the leash keeps it dangling from
// its wall anchor instead of on the floor.
module button_plug() {
    ww = btn_w + 2*tpu_squeeze;  wh = btn_h + 2*tpu_squeeze;
    difference() {
        union() {
            hull() {   // bevel-nesting cap
                linear_extrude(0.01) rrect2d(btn_w + 2.2, btn_h + 2.2, 4.1);
                translate([0, 0, 1.2]) linear_extrude(0.01) rrect2d(ww, wh, 3.1);
            }
            // waist through the wall's remaining straight land
            translate([0, 0, 1.2 - 0.01]) linear_extrude(0.82) rrect2d(ww, wh, 3.1);
            hull() {   // captive lip, ≤45° so it prints cap-down unsupported.
                // The flare is asymmetric on purpose: 1.2/side along the
                // window's length, but only 0.6/side across the wall band —
                // the glass slab's edge lives 0.15 off the wall's inner face,
                // and a full flare there would press on the panel's border
                // (the btn_plug_glass gate catches exactly this).
                translate([0, 0, 2.0 - 0.01]) linear_extrude(0.01) rrect2d(ww, wh, 3.1);
                translate([0, 0, 3.2]) linear_extrude(0.4)
                    rrect2d(ww + 2.4, wh + 1.2, 3.7);
            }
            // press towers — root flare so the column presses instead of
            // folding at its root, straight shaft, stepped tip narrow where
            // it lands inside the button cap. Prints cap-down: the towers
            // stand straight up, no support. Reach is derived (or measured
            // via btn_reach) to REST on the caps — see btn_tower_len.
            for (sx = [1, -1]) translate([sx*btn_lbl_dx, btn_pip_dz, 3.6 - 0.01]) {
                cylinder(d1 = btn_pip_d + 2.4, d2 = btn_pip_d, h = 1.21);
                cylinder(d = btn_pip_d, h = max(0.4, btn_tower_len - 1.0));
                cylinder(d = btn_pip_d - 1.2, h = btn_tower_len);
            }
            // leash: flat strap off the cap's anchor-side end lying flush in
            // the wall's skin channel, arrowhead barb at the anchor — the SD
            // cover's leash, transplanted (same barb spec, same 45° cam
            // faces: yanks free for service, never under a dangle).
            if (plug_tether) {
                sT = plug_teth_dx < 0 ? -1 : 1;
                cw = (btn_w + 2.2)/2 - 1.0;   // strap root overlaps the cap edge
                translate([min(plug_teth_dx, sT*cw), -2, 0])
                    cube([abs(plug_teth_dx) - cw, 4, 1.2]);      // strap
                hull() {   // trumpet the cap junction (fatigue doctrine,
                           // same as the SD strap's root flare)
                    translate([sT*cw - 0.005, -2.4, 0]) cube([0.01, 4.8, 1.2]);
                    translate([sT*(cw + 1.6) - 0.005, -2, 0]) cube([0.01, 4, 1.2]);
                }
                translate([plug_teth_dx, 0, 0]) {
                    // root cone nests the anchor's entry chamfer, so the
                    // strap lies flush and the barb's reach is unchanged
                    translate([0, 0, 1.2 - 0.01]) cylinder(d1 = 4.0, d2 = 2.8, h = 0.6);
                    cylinder(d = 2.8, h = frame_wall + 0.2);       // shaft
                    translate([0, 0, frame_wall + 0.2 - 0.01])     // 45° under-flare
                        cylinder(d1 = 2.8, d2 = sd_teth_head, h = 0.9);
                    translate([0, 0, frame_wall + 1.1 - 0.02])     // insertion tip
                        cylinder(d1 = sd_teth_head, d2 = 1.6, h = 1.0);
                }
            }
        }
        // finger-findable dimples on the cap face, one over each tower
        for (sx = [1, -1])
            translate([sx*btn_lbl_dx, btn_pip_dz, -7.31]) sphere(r = 7.81);
    }
}

// SD peel cover. Local z: 0 = cap face (on the bed). The cap is COUNTERSUNK:
// its 45° tapered edge nests the plate's 45° rim, flush at the skin — the
// taper self-centers it and both sides print without a single bridge (the
// flat-floored recess + tab this replaces drooped on the first real print).
// Install: push the arrowhead barb through the plate's anchor hole with a
// thumb (it mushrooms inside — the cover is now captive), lay the strap in
// its channel, press the cap home. To open, get a nail into the plate's
// scoop — it bites the rim at the card end, so the nail lands directly
// under the cap's tapered edge — and peel: the shallow lip pops
// progressively and the cover dangles on its leash, still attached. The
// countersink itself stops the cap being pushed inside; a deliberate yank
// on the cover cams the barb's 45° faces out of the hole for service.
module sd_cover() {
    ww = sd_w + 2*tpu_squeeze;  wl = sd_l + 2*tpu_squeeze;
    union() {
        hull() {   // countersunk cap: flush face on the bed, 45° edge
            linear_extrude(0.01)
                rrect2d(sd_w + 2*sd_lip - 0.2, sd_l + 2*sd_lip - 0.2, 5 + sd_lip);
            translate([0, 0, sd_lip]) linear_extrude(0.01) rrect2d(ww, wl, 5.1);
        }
        translate([0, 0, sd_lip - 0.01])
            linear_extrude(3.0 - sd_lip + 0.02) rrect2d(ww, wl, 5.2);   // waist
        hull() {   // shallow snap lip, 45° — pops out under a peel
            translate([0, 0, 3.0 - 0.01]) linear_extrude(0.01) rrect2d(ww, wl, 5.2);
            translate([0, 0, 4.2]) linear_extrude(0.4)
                rrect2d(ww + 1.8, wl + 1.8, 6.1);
        }
        // leash: flat strap off the cap's top end, arrowhead barb at its
        // end. Printed cap-face-down everything stays 45°-safe: the strap
        // lies on the bed, the arrowhead's under-flare is the 45° cam face
        // (a deliberate yank frees it, a dangle never does) and the tip
        // tapers steeply for the thumb-push through the hole.
        if (sd_tether) {
            translate([-2, sd_l/2 + sd_lip - 0.6, 0])
                cube([4, sd_teth_gap + 2.2, 1.2]);              // strap
            // root flare into the cap: the fold concentrates at stiffness
            // steps, and a square plan corner there is where a fatigue
            // crack would start — trumpet the junction instead (both roots
            // stay inside the 5.0 strap channel, asserted with the head)
            hull() {
                translate([-2.4, sd_l/2 + sd_lip - 0.6, 0]) cube([4.8, 0.01, 1.2]);
                translate([-2, sd_l/2 + sd_lip + 1.0, 0]) cube([4, 0.01, 1.2]);
            }
            translate([0, sd_l/2 + sd_lip + sd_teth_gap, 0]) {
                // ...and a root cone where the strap meets the shaft — sized
                // to NEST inside the anchor hole's 45° mouth chamfer, so the
                // strap still lies flush and the barb's reach is unchanged
                translate([0, 0, 1.2 - 0.01]) cylinder(d1 = 4.0, d2 = 2.8, h = 0.6);
                cylinder(d = 2.8, h = 3.2);                     // shaft
                translate([0, 0, 3.2 - 0.01])                   // 45° under-flare
                    cylinder(d1 = 2.8, d2 = sd_teth_head, h = 1.0);
                translate([0, 0, 4.2 - 0.02])                   // insertion tip
                    cylinder(d1 = sd_teth_head, d2 = 1.6, h = 1.0);
            }
        }
    }
}

// The fitments in their INSTALLED positions in the frame's coordinates —
// used by canary_s3_lcd7_fitcheck.scad's TPU gates, so the gates test the
// exact transforms these comments claim.
module usb_grommet_installed() {
    translate([usb_dx, -fr_yi/2 + 1.6, usb_z]) rotate([90, 0, 0]) usb_grommet();
}
module button_plug_installed() {
    translate([btn_dx, fr_yo/2, btn_zc]) rotate([90, 0, 0]) button_plug();
}
module sd_cover_installed() {
    translate([sd_dx, sd_dy, fr_depth]) rotate([0, 180, 0]) sd_cover();
}

// ----------------------------------------------------------------------------
//  RADIUS GAUGE — the ~6 g answer to "what is the slab's corner radius,
//  really". Four corner sockets bracketing glass_r (−0.4 / −0.2 / +0 / +0.2),
//  each opened by frame_reveal exactly as the frame's opening is, walls tall
//  enough to read daylight against. Drop the panel's corner in each: the
//  right radius hugs it with no daylight at the arc and no bind on the flats.
//  The first print showed the radius argument must be settled here — a wrong
//  guess on the slab costs 150 g and a day.
// ----------------------------------------------------------------------------
module rg_corner2d(rr) {   // the glass corner at this radius, opened by the reveal
    offset(r = frame_reveal) translate([20, 20]) rrect2d(40, 40, rr);
}
// ----------------------------------------------------------------------------
//  RING GAUGE — the whole outline, in a few minutes and ~5 g.
//
//  The radius gauge answers "what is the corner?" and the frame gauge answers
//  "does one corner assemble?". Neither answers the question you actually want
//  settled before a 110 g print: is the OPENING right, all the way round.
//
//  This is that check as a flat closed loop — inner edge exactly the frame's
//  glass opening (fr_xi x fr_yi at fr_ri, i.e. the slab plus frame_reveal per
//  side). Lay the panel into it, or hold it over the panel, and every error
//  shows at once and in one place:
//    · slab too wide/tall  -> it will not drop in, or rattles
//    · corner radius wrong -> daylight at the arcs with the straights touching
//    · reveal wrong        -> uniform slop or uniform bind all round
//  A corner coupon cannot show the first of those at all, because it has no
//  opposite edge to measure against.
//
//  Flat, four layers at 0.3, no supports, no brim. Print it FIRST — it is the
//  cheapest part in the catalog and it gates the most expensive one.
// ----------------------------------------------------------------------------
ring_gauge_w = 6.0;   // radial width — wide enough to stay flat and be handled
ring_gauge_t = 1.2;   // thickness; a 2 mm-wide ring at 200 mm long curls
module ring_gauge() {
    linear_extrude(ring_gauge_t) difference() {
        rrect2d(fr_xi + 2*ring_gauge_w, fr_yi + 2*ring_gauge_w,
                fr_ri + ring_gauge_w);
        rrect2d(fr_xi, fr_yi, fr_ri);
    }
    // the part says what it is, so a loose ring on a bench is never a mystery
    translate([0, -fr_yi/2 - ring_gauge_w/2, ring_gauge_t - 0.35])
        linear_extrude(0.36)
            text(str("GLASS ", glass_w, " x ", glass_h, "  r", glass_r,
                     "  rev", frame_reveal),
                 size = 3.0, font = label_font, halign = "center",
                 valign = "center");
}

module radius_gauge() {
    assert(rg_step > 0, "radius_gauge: rg_step must be positive");
    assert(rg_center - 1.5*rg_step > 0.4,
           "radius_gauge: the sweep reaches a radius at or below zero — raise rg_center or shrink rg_step");
    assert(rg_center + 1.5*rg_step < 19,
           "radius_gauge: the sweep exceeds what a 40 mm corner block can round — the socket would stop being a corner");
    for (i = [0 : 3]) {
        rr = rg_center + (i - 1.5)*rg_step;
        translate([i*34, 0, 0]) {
            // base pad — 1 mm wider than the station pitch, so the four pads
            // fuse into one printable strip (and one watertight STL)
            linear_extrude(1.6) rrect2d(35, 30, 3);
            // L-walls following the pocket boundary — the concave corner is
            // the true fr_ri of a frame built at this radius
            translate([0, 0, 1.59]) linear_extrude(8) intersection() {
                difference() { offset(r = 2.4) rg_corner2d(rr); rg_corner2d(rr); }
                square([26, 26], center = true);
            }
            translate([7, -9, 1.6 - label_depth])
                linear_extrude(label_depth + 0.1)
                    text(str(rr), size = 4, font = label_font,
                         halign = "center", valign = "center");
        }
    }
}

// ----------------------------------------------------------------------------
//  STAND — desk dock for the FRAME case (prints flat, base on the plate)
//
//  Structure: a foot-chamfered base plate; two sculpted side CHEEKS whose
//  tilted tops are the seat pads; a full-width front LIP and back FIN, both
//  raked at the recline angle, forming the drop-in slot between them. The
//  case is wider than the dock, so the slot runs the dock's full width and
//  the case overhangs each cheek — that is what keeps the side service
//  windows reachable. Between the pads the base is cut open into a WELL:
//  the bottom-wall intake vents draw through it, and the USB power lead
//  drops into it (stand_floor_h of plug headroom) and leaves through a
//  desk-level channel under the fin's foot, out the back.
//
//  Everything slot-shaped is built in the tilted seat frame and trimmed at
//  the desk plane afterwards, so changing stand_ang cannot open a gap
//  between a blade's foot and the plate.
// ----------------------------------------------------------------------------
module stand_seatframe() {
    translate([0, std_ys, stand_floor_h]) rotate([-stand_ang, 0, 0]) children();
}
// Full-width blade outline in the rake plane (x = across, y = up the rake):
// square feet running long below the desk trim, top corners rounded to the
// base plate's r10 so blades and base share one radius family.
module stand_blade2d(h, r = 10) hull() {
    for (sx = [1, -1]) translate([sx*(stand_w/2 - r), h - r]) circle(r);
    translate([0, -59.5]) square([stand_w, 1], center = true);
}
// A blade standing in the well, from the desk up to `drop` below the tilted
// pad plane: the portrait seat ribs. It overruns the slot by 2 mm at each
// end so it welds into the lip's and fin's feet.
module stand_wellblade(x0, w, drop = 0) {
    difference() {
        stand_seatframe() translate([x0 - w/2, -std_cd/2 - 2, -45])
            cube([w, std_cd + 4, 45 - drop]);
        translate([0, 0, -50]) cube([stand_w + 40, std_d + 300, 100], center = true);
    }
}
// A centering-key stud: a chamfered wedge rising key_h proud of z0 in the seat
// frame, at local y = -1 — the docked case's wall-band center (gz), which is
// where every keying slot is cut, whichever wall faces down. The taper is
// what makes the case FIND center: drop it anywhere close and it slides home.
// A centering key stud. Its position ACROSS the slot is derived, not fixed:
// the case's keying slot sits key_gz behind the case's FRONT face, and the
// case's front face sits at -std_cd/2, so the slot's center lands here. With
// a fixed y this silently walked off the slot as soon as a battery deepened
// the case — the 3000 build put the stud 4.4 mm out and it hit solid wall.
// key_trim: the studs historically sat 0.2 forward of the pure derivation,
// and the PORTRAIT pose is tuned to that — moving them onto the derived
// center by itself made stand_p collide (32 slivers at ±47.3, the portrait
// studs). So keep the trim: this change is meant to make the position TRACK
// the case's depth, not to re-tune a pose that already worked. Landscape had
// 4.35 mm of error before it failed, so 0.2 is well inside its margin.
module stand_keystud(x0, z0) {
    key_trim = 0.2;
    ky = -std_cd/2 + key_gz + key_trim;
    stand_seatframe() hull() {
        translate([x0 - 0.9, ky - 0.95, z0 - 0.5]) cube([1.8, 1.9, 0.5]);
        translate([x0 - 0.3, ky - 0.35, z0 + 1.5]) cube([0.6, 0.7, 0.02]);
    }
}
module stand() {
    a = stand_ang;
    // cheek gusset point: where the cheek's back edge meets the fin's BACK
    // face, stand_gusset_h up the fin — the back edge then runs desk-ward in
    // that same raked plane, so cheek and fin read as one surface
    gy = std_ys + (std_cd/2 + stand_fin_t)*cos(a) + stand_gusset_h*sin(a);
    gz = stand_floor_h - (std_cd/2 + stand_fin_t)*sin(a) + stand_gusset_h*cos(a);
    intersection() {
    union() {
    difference() {
        union() {
            // base plate with a modeled foot chamfer (slicer compensation 0)
            hull() {
                linear_extrude(0.01) rrect2d(stand_w - 1.2, std_d - 1.2, 9.4);
                translate([0, 0, 0.6]) linear_extrude(0.01) rrect2d(stand_w, std_d, 10);
            }
            translate([0, 0, 0.6]) linear_extrude(stand_plate_t - 0.6)
                rrect2d(stand_w, std_d, 10);
            // lip + fin: full-width raked blades, run long below the seat so
            // they fuse with the plate at any recline (trimmed at the desk).
            // Each blade's top corners carry the SAME r10 the base plate
            // wears — one radius family across the whole silhouette, no
            // square-cornered blade ends over a rounded base. Printable by
            // construction: the blades stand near-vertical and a convex
            // top-corner arc only ever SHORTENS each layer as it climbs,
            // so the rounds are self-supporting everywhere.
            stand_seatframe() {
                translate([0, -std_cd/2, 0]) rotate([90, 0, 0])
                    linear_extrude(stand_lip_t) stand_blade2d(stand_lip_h);
                translate([0, std_cd/2 + stand_fin_t, 0]) rotate([90, 0, 0])
                    linear_extrude(stand_fin_t) stand_blade2d(stand_fin_h);
            }
            // side cheeks: front edge co-planar with the lip's outer face,
            // back edge co-planar with the fin's back face, drawn tall — the
            // seat pocket and the over-lip sky cut do the sculpting
            for (sx = [1, -1]) translate([sx*(stand_w - stand_cheek_t)/2, 0, 0])
                rotate([90, 0, 90]) linear_extrude(stand_cheek_t, center = true)
                    polygon([[std_front_foot, 0], [gy - gz*tan(a), 0],
                             [gy, gz], [std_front_foot + 60*tan(a), 60]]);
        }
        // one trim for every blade/cheek foot: everything below the desk
        translate([0, 0, -50]) cube([stand_w + 40, std_d + 200, 100], center = true);
        // the seat pocket: everything above the tilted pad plane, between
        // the blades, the FULL width — the case overhangs both cheeks
        stand_seatframe() translate([-stand_w/2 - 1, -std_cd/2, 0])
            cube([stand_w + 2, std_cd, 200]);
        // sky cut: in front of the pocket, everything above the lip's capture.
        // It reaches 0.04 PAST the slot's front plane, INTO the pocket's
        // territory — already void, so no fit surface moves — because two
        // cuts ending on the exact same plane leave a zero-volume facet
        // sheet in the CGAL export: phantom "walls" over the cheeks that
        // render (and confuse a slicer) but hold no material. Same 0.04
        // overlap on both entry flares below, which shared the slot planes
        // the same way.
        stand_seatframe() translate([-stand_w/2 - 1, -300 - std_cd/2, stand_lip_h])
            cube([stand_w + 2, 300.04, 300]);
        // entry flares: lead-in chamfers on the lip top and the fin top, so
        // the case finds a ~30 mm mouth instead of a 25 mm slot
        stand_seatframe() {
            hull() {
                translate([-stand_w/2 - 1, -std_cd/2 - 0.04, stand_lip_h - 2.5])
                    cube([stand_w + 2, 0.08, 2.6]);
                translate([-stand_w/2 - 1, -std_cd/2 - 2.5, stand_lip_h - 0.04])
                    cube([stand_w + 2, 2.54, 0.04]);
            }
            hull() {
                translate([-stand_w/2 - 1, std_cd/2 - 0.04, stand_fin_h - 2.5])
                    cube([stand_w + 2, 0.08, 2.6]);
                translate([-stand_w/2 - 1, std_cd/2 - 0.04, stand_fin_h - 0.04])
                    cube([stand_w + 2, 2.54, 0.04]);
            }
            // the WELL: a clear shaft under the seated case, walls parallel
            // to the slot, kept 1.5 mm inside the case footprint so it never
            // nicks a blade's foot into a floating bridge
            translate([-(std_open/2 - 3), -std_cd/2 + 1.5, -100])
                cube([std_open - 6, std_cd - 3, 200]);
            // fin vents: tall pills, not one big window — a window's top
            // edge would be a 100+ mm bridge on this near-vertical print,
            // while a pill's top is a self-supporting arc. The case's back
            // grille breathes through them, and the ±|sd_dx| pair is placed
            // ON the back plate's microSD opening (whichever way the case
            // mirrors), so the card stays reachable while docked.
            for (px = [[14, 16], [abs(sd_dx), 16], [63, 10]], sx = [1, -1])
                translate([sx*px[0], std_cd/2 + stand_fin_t + 1,
                           16 + (stand_fin_h - 26)/2])
                    rotate([90, 0, 0]) linear_extrude(stand_fin_t + 2)
                        vent2d(stand_fin_h - 26, px[1]);
            // branding: on the lip's front face, read standing in front...
            translate([0, -(std_cd/2 + stand_lip_t) + label_depth, -8])
                rotate([90, 0, 0]) linear_extrude(label_depth + 0.2)
                    text(brand_edge, size = 6, font = label_font,
                         halign = "center", valign = "center");
            // ...and on the fin's back face under the window, read from behind
            translate([0, std_cd/2 + stand_fin_t + 0.2, 8])
                rotate([90, 0, 0]) linear_extrude(label_depth + 0.2)
                    mirror([1, 0, 0]) text("SecuraCV", size = 6, font = label_font,
                         halign = "center", valign = "center");
        }
        // cable channel: desk-level, from the well out the back edge,
        // tunnelling under the fin's foot
        translate([-stand_cable_w/2, std_ys, -1])
            cube([stand_cable_w, std_d/2 - std_ys + 2, 10]);
        // rubber-foot recesses in the corners, clear of well and channel
        if (stand_feet) for (sx = [1, -1], sy = [1, -1])
            translate([sx*(stand_w/2 - 14), sy*(std_d/2 - 12), -0.1])
                cylinder(d = 10.5, h = 0.9);
        // help QR: module cells debossed 0.4 into the deck skin. Dark-on-
        // light polarity comes from the color swap (see the echo): the
        // floors are the last body-color layer, the surrounding skin the
        // accent — and the bare deck around the field is the quiet zone.
        if (qr_help)
            translate([qr_dx - qr_field/2, qr_dy_eff + qr_field/2,
                       stand_plate_t - 0.4])
                linear_extrude(0.5)
                    qr_field2d(qr_cell);
    }
    // key furniture, added AFTER the cuts (the seat-pocket cut would
    // otherwise carve it away):
    // — the two KEYED RIBS, their tops stand_rib_drop below the pad plane:
    //   each blade seats the PORTRAIT case and carries its centering key,
    //   a stud rising into the side-wall keying slot at ±dock_key_dx, 1.5
    //   proud of the dropped rib so it enters the 2 mm wall without
    //   bottoming out — and stays 0.5 BELOW the pad plane, clear of the
    //   landscape case's solid bottom wall. The blade bears on solid wall
    //   both sides of the slot, so nothing sits tilted on a key.
    // — the two LANDSCAPE keys, studs rising straight off the cheek pads
    //   at ±dock_key_bx into the bottom wall's keying slots (the portrait
    //   case is narrower than the well and never reaches them).
    for (sx = [1, -1]) {
        stand_wellblade(sx*stand_rib_x, stand_rib_w, stand_rib_drop);
        if (dock_keys) {
            stand_keystud(sx*dock_key_dx, -stand_rib_drop);
            stand_keystud(sx*dock_key_bx, 0);
        }
    }
    }
    // one plan-silhouette trim for everything: the blades' and cheeks' feet
    // follow the base plate's rounded corners instead of overhanging them
    translate([0, 0, -1]) linear_extrude(300) rrect2d(stand_w, std_d, 10);
    }
}

// One cheek's slice of the dock, cut from the real geometry by intersection —
// slot width, recline, seat height, lip capture and entry flares, for ~15 %
// of the dock's filament. Drop your printed FRAME's bottom corner in: it
// should seat on the pad with visible clearance front and back, and the lip
// should stop 2+ mm short of the screen window.
// The frame in its DOCKED pose. It lives HERE, not in the fitcheck, for one
// concrete reason: -D only reaches the file it is given, so a pose defined in
// the fitcheck can never be evaluated for `battery=...` — the dock gates would
// silently test the no-battery case forever. The fitcheck delegates to this.
//   seat > 0 presses the case INTO the pads; seat < 0 hovers it off them.
//   portrait = ±1 turns the case that way and drops it onto the well ribs.
module lcd7_docked(seat = 0, portrait = 0) {
    fy = portrait == 0 ? fr_yo : fr_xo;
    dn = seat + (portrait == 0 ? 0 : stand_rib_drop);
    yq = -(fy/2)*sin(stand_ang) + (fr_depth/2)*cos(stand_ang);
    zq = -(fy/2)*cos(stand_ang) - (fr_depth/2)*sin(stand_ang);
    translate([0, std_ys - yq - dn*sin(stand_ang),
               stand_floor_h - zq - dn*cos(stand_ang)])
        rotate([270 - stand_ang, 0, 0]) rotate([0, 0, 180])
            rotate([0, 0, 90*portrait]) frame();
}

module stand_gauge() {
    intersection() {
        stand();
        translate([std_open/2 - 6, -std_d/2 - 1, -1])
            cube([stand_cheek_t + 8, std_d + 60, 120]);
    }
}

// ----------------------------------------------------------------------------
// ── ASSEMBLY VIEWS — NOT PRINTABLE, and never exported as a part ────────────
// The case has always been drawable on its own and meaningless on its own: a
// hollow shell tells you nothing about which edge the cable leaves from or
// which way up the image is. These put the REGISTRY'S panel in it, so the
// thing you look at is the assembly rather than half of it.
//
// The mock is built from the same record the case reads, so it cannot drift:
// there is no second description of the panel to keep in step. That is the
// whole reason the registry exists — before it, the fit-check file and the
// section view each built their own panel out of the case's variables, and
// "they agree" was a habit rather than a property.
//
//   openscad -o assembly.png --imgsize 1600,1100 --autocenter --viewall \
//       --camera=0,0,0,62,0,25,320 -D 'part="assembly"' canary_s3_lcd7.scad
//   ...and part="exploded" for the same view pulled apart along the insertion
//   axis. asm_blow sets how far; the panel leaves through the FRONT, because
//   that is how it goes in.
asm_blow = 26;   // exploded separation, mm
module lcd7_assembly(blow = 0) {
    // the case, held back so the panel reads through it
    color([0.88, 0.88, 0.86, blow > 0 ? 1.0 : 0.35]) frame();
    // the panel, seated: its front face lands glass_guard behind the rim,
    // which is the same number the frame's own stack is built on
    translate([0, 0, glass_guard - blow]) panel_mock(PANEL, blow * 0.18);
}

if      (part == "bezel") bezel_print();
else if (part == "back")  back();
// the frame and its gauge EXPORT back-plate-down: the print orientation the
// ledge wedges are self-supporting in (face-down would hang them over the
// glass pocket)
else if (part == "frame")       rotate([180, 0, 0]) translate([0, 0, -fr_depth]) frame();
else if (part == "frame_gauge") rotate([180, 0, 0]) translate([0, 0, -fr_depth]) frame_gauge();
// Per-filament parts, in the SAME frame as part="frame" — that shared
// orientation is what lets the slicer take all three without re-aligning
// anything. Do not drop-to-bed them individually.
// COLOR COUPON — the three-filament rehearsal, ~6% of the frame's filament.
//
// Print this before committing the real frame. It is cut from the bottom-
// center band, which is the ONLY place on the part where all three colors
// meet: white plate, INK lockup and bezel ring, ACCENT company line. The
// corner gauge cannot do this job — the accent word lives at x = 0 and the
// gauge is a corner, so a corner coupon would come out two-color and prove
// nothing about the third filament.
//
// What it actually tests, none of which a slicer preview will tell you:
//   · the AMS purge is tuned — black bleeding into white shows here first,
//     on the deboss floors, at 1/16th the filament cost
//   · the three parts really do register when loaded as one object (they
//     share part="frame"'s frame; move one and the lettering misses)
//   · the bezel swap lands where the echo says, on a real first layer
//   · the USB port and its grommet, since the port is in this band
else if (part == "coupon_body")
    rotate([180, 0, 0]) translate([0, 0, -fr_depth])
        intersection() { frame_bodycol(); coupon_clip(); }
else if (part == "coupon_ink")
    rotate([180, 0, 0]) translate([0, 0, -fr_depth])
        intersection() { frame_ink(); coupon_clip(); }
else if (part == "coupon_accent")
    rotate([180, 0, 0]) translate([0, 0, -fr_depth])
        intersection() { frame_accent(); coupon_clip(); }
// QR SCAN COUPON — two filaments, back plate only. See coupon_qr_clip() for
// why this is its own plaque instead of a longer band.
//
// The second volume FOLLOWS THE GROUP rather than naming a filament. It used
// to intersect frame_ink() and be called coupon_qr_ink, which was true only
// while "qr" lived in ink_groups: the moment it moved to the accent, this
// part rendered EMPTY, OpenSCAD wrote no file, and the packer produced a
// body-only plaque — a scan coupon with no symbol on it, handed to someone
// told to scan it before committing a frame. The coupon has to print the two
// colors that actually meet on the plate, so it asks which those are.
else if (part == "coupon_qr_body")
    rotate([180, 0, 0]) translate([0, 0, -fr_depth])
        intersection() { frame_bodycol(); coupon_qr_clip(); }
else if (part == "coupon_qr_fill")
    rotate([180, 0, 0]) translate([0, 0, -fr_depth])
        intersection() { frame_qrfill(); coupon_qr_clip(); }
else if (part == "fil_body")   rotate([180, 0, 0]) translate([0, 0, -fr_depth]) frame_bodycol();
else if (part == "fil_ink")    rotate([180, 0, 0]) translate([0, 0, -fr_depth]) frame_ink();
else if (part == "fil_accent") rotate([180, 0, 0]) translate([0, 0, -fr_depth]) frame_accent();
else if (part == "frame_color") rotate([180, 0, 0]) translate([0, 0, -fr_depth]) frame_color();
// PARTITION GATES. A three-part object is only correct if the parts tile the
// solid: fil_overlap catches material claimed by two filaments (prints twice,
// and the slicer picks one arbitrarily), fil_gap catches material claimed by
// none (prints as a void sealed inside a wall). Both must be EMPTY.
else if (part == "fil_overlap")
    union() {
        intersection() { frame_bodycol(); frame_ink(); }
        intersection() { frame_bodycol(); frame_accent(); }
        intersection() { frame_ink();     frame_accent(); }
    }
else if (part == "fil_gap")
    difference() { frame(); frame_bodycol(); frame_ink(); frame_accent(); }
else if (part == "gauge")       gauge();
else if (part == "gauge_tray")  gauge_corner("back");
else if (part == "gauge_bezel") gauge_corner("bezel");
else if (part == "stand") stand();
else if (part == "stand_gauge") stand_gauge();
// Battery fit gates — parts, not fitcheck checks, deliberately: -D only
// reaches the file it is given, so gating a battery VARIANT means the
// probe must live where -D 'battery=...' lands. Same doctrine as the
// fitcheck: file-appears/file-empty is the verdict.
//   bat_probe_fit  — the pack, inset under the hook reach, vs the frame:
//                    must be EMPTY (cavity, curb, rails all clear it)
//   bat_probe_seat — a wafer at the rail plane vs the frame: must be
//                    SOLID (the rails actually carry the pack)
//   bat_probe_grip — the full pack grown past the hook plane vs the
//                    frame: must be SOLID (the fingers actually retain it)
// Dock gates — parts, not fitcheck checks, for the same -D reason as the
// battery probes: these must be evaluable for EVERY battery build, since a
// deeper case means a deeper slot and a deeper base.
//   dock_probe_fit  — seated frame vs stand, hovered: must be EMPTY
//   dock_probe_seat — pressed in: must be SOLID (the pads carry the case)
//   dock_probe_p / _p2 — the same collision check, portrait either way
// Leash gates, same doctrine as the SD tether's pair: one proves the FRAME
// really carries the anchor hole (print the case without it and the fitment
// can never be made captive), the other proves the barb actually reaches it.
else if (part == "port_teth_hole") {
    assert(port_tether && usb_port, "needs port_tether and usb_port");
    intersection() {
        frame();
        translate([usb_dx + port_teth_dx, -fr_yo/2 - 1, usb_z])
            rotate([-90, 0, 0]) cylinder(d = sd_teth_hole - 0.6, h = frame_wall + 8);
    }
}
else if (part == "port_teth_barb") {
    assert(port_tether && usb_port, "needs port_tether and usb_port");
    intersection() {
        usb_grommet_installed();
        translate([usb_dx + port_teth_dx, -fr_yi/2 - 0.4, usb_z])
            rotate([-90, 0, 0]) cylinder(d = sd_teth_head + 2, h = 0.8);
    }
}
// ...and a THIRD, because the pair above still passed a barb that reached
// its hole and then kept going: nothing of the fitment may sit outside the
// skin in the anchor's column. Sliced to that column on purpose — the
// grommet's strain-relief bell is deliberately proud, and a whole-part probe
// would be masked by it.
else if (part == "port_barb_proud") {
    assert(port_tether && usb_port, "needs port_tether and usb_port");
    // The column is DERIVED, not guessed: wide enough to hold the whole
    // mushroom, but stopping short of the flange edge at usb_open_w/2 +
    // grom_lip. A fixed +-4 reached 0.2 mm onto that flange and reported a
    // failure against geometry that is deliberately proud — the probe has to
    // exclude the strain relief to say anything about the barb.
    col = abs(port_teth_dx) - (usb_open_w/2 + grom_lip) - 0.8;
    assert(col > sd_teth_head/2,
           "port_barb_proud: no clear column between the anchor and the flange — the probe cannot see the head without clipping the strain relief");
    intersection() {
        usb_grommet_installed();
        translate([usb_dx + port_teth_dx - col, -fr_yo/2 - 20, usb_z - 6])
            cube([2*col, 20, 12]);
    }
}
// ...and the same pair for the BUTTON PLUG's leash: the frame must carry
// the anchor hole through the top wall (probe just under hole size, must
// be EMPTY), and the installed plug's barb must actually cross the wall's
// inner face at the anchor (wafer probe, must be SOLID).
else if (part == "plug_teth_hole") {
    assert(plug_tether, "needs plug_tether");
    intersection() {
        frame();
        translate([btn_dx + plug_teth_dx, fr_yo/2 + 1, btn_zc])
            rotate([90, 0, 0]) cylinder(d = sd_teth_hole - 0.6, h = frame_wall + 8);
    }
}
else if (part == "plug_teth_barb") {
    assert(plug_tether, "needs plug_tether");
    intersection() {
        button_plug_installed();
        translate([btn_dx + plug_teth_dx, fr_yi/2 - 0.4, btn_zc])
            rotate([90, 0, 0]) cylinder(d = sd_teth_head + 2, h = 0.8);
    }
}
// WALL-MOUNT FLUSHNESS: hung on its keyholes the case bears on the back
// plate, so nothing — no fitment, no leash head, no strap — may stand proud
// of it, or the case rocks on that point instead of lying flat. Must be
// EMPTY. The slab starts 0.02 clear of the plate so a coplanar face cannot
// mint the zero-volume sheets that fail the render instead of the check.
else if (part == "back_flush")
    intersection() {
        union() {
            frame();
            usb_grommet_installed();
            button_plug_installed();
            sd_cover_installed();
        }
        translate([-fr_xo/2 - 5, -fr_yo/2 - 5, fr_depth + 0.02])
            cube([fr_xo + 10, fr_yo + 10, 10]);
    }
else if (part == "dock_probe_fit")
    intersection() { lcd7_docked(-0.2); stand(); }
else if (part == "dock_probe_seat")
    intersection() { lcd7_docked(0.4); stand(); }
else if (part == "dock_probe_p")
    intersection() { lcd7_docked(-0.2, 1); stand(); }
else if (part == "dock_probe_p2")
    intersection() { lcd7_docked(-0.2, -1); stand(); }
else if (part == "bat_probe_fit") {
    assert(bat_on, "battery gates need -D battery=\"3000\" or \"10000\"");
    intersection() { frame(); bat_brick(2.4, bat_t - 0.1, 0.05); }
}
else if (part == "bat_probe_seat") {
    assert(bat_on, "battery gates need -D battery=\"3000\" or \"10000\"");
    intersection() {
        frame();
        translate([-bat_l/2 + 2, -bat_w/2 + 2, fz_plate - bat_rib])
            cube([bat_l - 4, bat_w - 4, 0.2]);
    }
}
else if (part == "bat_probe_grip") {
    assert(bat_on, "battery gates need -D battery=\"3000\" or \"10000\"");
    // drop 0.1 — a top face EXACTLY on the rail undersides mints phantom
    // zero-volume sheets (the #1373 class) and their manifold warning
    // fails the gate; the hooks live well below the rail plane anyway
    intersection() { frame(); bat_brick(0, bat_t + 0.5, 0.1); }
}
else if (part == "assembly") lcd7_assembly(0);
else if (part == "exploded") lcd7_assembly(asm_blow);
else if (part == "panel")    panel_mock(PANEL);
else if (part == "radius_gauge") radius_gauge();
else if (part == "ring_gauge")   ring_gauge();
// TPU fitments export in their print orientation (A-face down), as modeled
else if (part == "plug_port")    usb_grommet(false);
else if (part == "grommet_usb")  usb_grommet();
else if (part == "plug_buttons") button_plug();
else if (part == "plug_sd")      sd_cover();
else {
    bezel_print();
    translate([xo + 16, 0, 0]) back();
    translate([-(xo + 20), 0, 0]) frame();
    // the TPU trio, laid out under the frame (preview only — TPU prints on
    // its own plate, from the external spool)
    translate([-(xo + 20), -(yo/2 + 36), 0]) {
        translate([-65, 0, 0]) usb_grommet();
        button_plug();
        translate([65, 0, 0]) sd_cover();
    }
    if (opt_stand) translate([0, -(yo/2 + std_d/2 + 16), 0]) stand();
}
