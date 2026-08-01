// ============================================================================
//  Canary — 7" TOUCH DASHBOARD CASE  ⚠️ IN DEVELOPMENT (v0.4-dev)
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
//    gauge — TWO SMALL CORNER BLOCKS (a bezel corner + its tray corner). Print
//            these FIRST: ~16.5 g of filament against the case pair's ~158 g
//            (10 %), assembled around the panel's corner with one M3. They prove
//            the four things that make or break the big print — the glass
//            seats in the cavity, the lip lands on the border and clears the
//            active area, the screw threads, and the closed stack height is
//            what the echo says. See bambu_p2s_bringup.md §7.
//    stand — free-standing desk cradle, reclined, no hardware (optional).
//    frame — ONE-PIECE drop-in case, the layout a fitting reference print
//            validated: the slab enters face-first through the front opening,
//            the board hangs on the panel's OWN white M3 standoffs, and
//            4x M3x8-10 from the back thread into those standoffs — the
//            screws, not a ledge, pull the glass flush with the front face.
//            BOOT/RESET window in the top wall with debossed labels; gill
//            vents down each side wall; top-wall exhaust; back grille;
//            a microSD access opening through the BACK PLATE covering the
//            socket, the card's slide travel and a fingertip ("SD" deboss);
//            keyhole wall mounts (hang on two screws, slide down); an
//            adhesive LEDGE behind the glass matched to the panel's own
//            adhesive strips (10 mm sides, 6 mm button edge, 2 mm over the
//            FPC), each with a 45° back-slope wedge to its wall; a USB
//            PASS-THROUGH centred on the bottom wall sized to pass the power
//            cable's overmold head, with the brand lettering cut THROUGH the
//            wall as slat-stencil intake vents flanking it — the bottom edge
//            breathes through its own name. Branding stays debossed on the
//            back plate. PRINTS BACK-PLATE-DOWN — the orientation the ledge
//            wedges self-support in.
//    TPU fitments (print in TPU 90–95 A from an EXTERNAL spool — never the
//            AMS; see bambu_p2s_bringup.md §0):
//      grommet_usb  — slit wire grommet for the USB pass-through: feed the
//            head through the port, plug it, wrap the grommet around the
//            wire and press it in. Inner flange carries tug loads so cable
//            yanks fatigue the frame, not the board's connector.
//      plug_buttons — captive plug for the BOOT/RESET window: cap nests the
//            window's bevel, a 45° lip snaps in behind the wall (can't fall
//            out, can't be pushed inside), and two press pips under the
//            face dimples actuate BOOT/RESET THROUGH the plug — it never
//            needs to come out.
//      plug_sd      — peel-open cover for the SD opening: flush cap in a
//            recess, battery-door hinge tongue at the top end (stays
//            anchored — can't get lost), fingernail peel tab at the card
//            end.
//    frame_gauge — one corner of the frame including one boss and a wall
//            keyhole. Print FIRST (~10 % of the frame's filament): it proves
//            the glass corner radius, the boss offset signs and screw reach.
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
//
//  ⚠️ CONNECTOR POSITIONS ARE NOMINAL — Waveshare's drawing dimensions the
//  glass + mount holes precisely but not every connector centre. MEASURE the
//  USB-C / UART / CAN / RS485 / battery positions on YOUR Rev before printing.
//  The old pcb_h dispute (97.60 vs 126.20) is RESOLVED: 126.20 is the M3
//  mount-hole X SPAN, measured off a reference case print that fits the real
//  panel — a published summary had recorded the hole span as an outline
//  height. pcb_h stays 97.60 nominal (MEASURE); the tray cavity still sizes
//  itself to the larger of glass and board, so an oversized board grows the
//  case rather than jamming it.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
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

/* [What to render] */
part = "all";        // ["bezel","back","frame","frame_gauge","gauge","gauge_bezel","gauge_tray","stand","grommet_usb","plug_buttons","plug_sd","all"]

/* [Glass slab] — bonded touch panel, from the Waveshare drawing (mm) */
glass_w = 192.96;    // touch-glass width  (X)
glass_h = 110.76;    // touch-glass height (Y)
glass_t = 4.0;       // glass + LCD module thickness at the edge — MEASURE
glass_r = 2.0;       // corner radius of the glass slab — MEASURE. A reference
                     // case whose cavity corners measure r≈2.7 still shows a
                     // sliver of daylight at each corner, so the slab is
                     // sharper than that — and sharper than the r3.0 v0.2
                     // assumed. The cavity is never rounded more than this: a
                     // pocket with a bigger radius than the glass binds (or
                     // gaps) on all four corners; a near-square panel needs a
                     // near-square pocket.
aa_w = 154.88;       // active area width
aa_h = 86.72;        // active area height
aa_dy = -1.02;       // AA centre offset from glass centre — native mounting:
                     // borders 13.04 top / 11.00 bottom. (A buttons-down
                     // build — panel rotated 180°, image flipped in firmware —
                     // negates this.)

/* [PCB stack behind the glass] */
pcb_standoff = 5.0;  // glass back → PCB front (the white M3 standoffs) — MEASURE.
                     // This one decides whether the bezel lip actually reaches
                     // the glass: too small and the case won't close, too big
                     // and the lip never touches it.
pcb_t   = 1.6;
comp_h  = 11.0;      // tallest thing behind the PCB (USB-C / terminals / batt JST) — MEASURE.
                     // Also sets the port band: cutouts open floor → PCB underside.
pcb_w   = 165.72;    // PCB outline width  (X) — from the drawing
pcb_h   = 97.60;     // PCB outline height (Y) — MEASURE (see the header warning)

/* [Board mounts] — 4x M3 corner standoffs carry the PCB (measured pattern) */
m3_dx = 126.20;      // M3 hole pattern width — MEASURED from a reference case
                     // print that fits the real panel (v0.2 shipped 165.72
                     // here — the outline width, which put every boss under
                     // the board's edge; see the header for how 126.20 also
                     // resolves the old pcb_h dispute). Verify on your board.
m3_dy = 65.65;       // M3 hole pattern height — measured with m3_dx; verify
m3_ox = -1.5;        // pattern centre offset from the GLASS centre, stated in
m3_oy = -0.9;        // FRONT view, panel mounted NATIVE (buttons at the top):
                     // +x = right, +y = up. The pattern is NOT symmetric about
                     // the glass centre — this offset is exactly why a panel
                     // can't be flipped 180° inside a case drawn for the other
                     // orientation. VERIFY the signs on your board with
                     // calipers before printing a tray or frame.
m3_pilot = 2.7;      // self-tap pilot for M3 into printed bosses

/* [Screen] */
bez_lip = 3.0;       // MINIMUM acceptable overlap of the lip onto the glass border.
                     // The real overlap is derived from the glass/AA borders and
                     // win_margin below; the assert fires if it drops under this.
win_margin = 0.6;    // window opened this much beyond the active area on each
                     // side, so print tolerance + panel placement can't eat
                     // visible pixels (the lip pays for it out of the border)

/* [Connectors] — openings, offsets from the board centre along their wall.
   NOMINAL — MEASURE. The bottom (−Y) wall carries the USB/UART cluster; the
   two short (±X) walls carry CAN/RS485/battery. Set opt_* to open them. */
opt_bottom_ports = true;
bottom_open_w = 96.0;   bottom_open_dx = 0.0;   // wide channel over the bottom cluster
opt_side_ports = true;
side_open_h = 40.0;     side_open_dy = 0.0;     // tall slot on each short wall

/* [Ventilation] — MUST let the backlight/SoC heat convect out.
   A grille alone only radiates; convection needs a low inlet and a high
   outlet, so the bottom wall takes air in and the top wall lets it out. */
vent_back = true;        // large grille in the back plate
vent_rows = 11;          // grille rows
vent_cols = 22;          // grille columns
vent_slot_w = 2.4;       // slot width
vent_slot_l = 9.0;       // slot length
vent_pitch_x = 7.0;      // column pitch
vent_pitch_y = 8.0;      // row pitch
vent_top = true;         // EXHAUST — slots through the top (+Y) wall
vent_top_n = 14;
vent_bottom = true;      // INTAKE — slots through the bottom (−Y) wall, placed
                         // outboard of the connector channel so they stay open
vent_bottom_n = 5;       // per side
vent_sides = true;       // side (±X) chimneys, kept clear of the port slots
vent_side_n = 3;         // per side, stacked toward the top corners

/* [Print tolerances] — only the two this case actually has fits for. There is
   no press fit anywhere in this design (no magnet pocket, no light pipe), so
   tol_press is deliberately absent rather than declared and ignored: a knob
   that tunes nothing sends you through reprints that cannot change the part. */
tol_slide = 0.25;    // the glass/board pockets — the SLIDE station on the coupon
tol_hole  = 0.35;    // M3 clearance through the bezel ears — the SCREW station

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
m3_nom = 3.0;                // M3 shank
lob_pilot = 2.7;  cb_d = 6.0;  cb_h = 2.0;
// Clearance is DERIVED from the coupon's SCREW tolerance, so dialling tol_hole
// after a coupon print actually moves this hole. (Self-tap pilots are not:
// lob_pilot/m3_pilot are undersize on purpose, so the thread forms.)
screw_c = m3_nom + tol_hole;

/* [Frame — one-piece drop-in case (part="frame")] */
frame_wall   = 2.0;  // sleeve wall (also the visible front rim around the glass)
frame_reveal = 0.15; // per-side glass↔opening clearance. The opening AND its
                     // corner radius both track the slab by this much, so the
                     // reference case's corner gap cannot come back.
standoff_len = 6.9;  // the panel's own white M3 standoffs, PCB back → tip — MEASURE
frame_boss_h = 3.0;  // boss standing proud of the back plate's inner face
frame_boss_d = 8.0;
btn_w  = 25.0;       // BOOT/RESET access window through the TOP wall (measured)
btn_h  = 13.5;       // window height across the wall's depth
btn_dx = 0.0;        // window centre offset along the top wall
btn_lbl_dx = 9.0;    // BOOT/RESET label centres, ± of the window centre
mount_keyholes = true;  // wall-mount keyholes through the back plate, up near
khm_dx = 78.5;          // the top corners. Head hole LOW, slide runs UP so the
khm_y  = 34.0;          // catch points at the button edge: hang the case over
khm_head_d  = 9.5;      // two screws and slide it DOWN to seat. The head hole
khm_slide_w = 4.5;      // passes a #8 / M4 pan head; the slide, its shank.
khm_len = 13.0;         // head-hole centre → catch centre
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
// Every ledge carries a 45° back-slope wedge down to its wall, so the frame
// prints BACK-PLATE-DOWN with the ledges fully self-supporting — solid
// material under the adhesive landing, no overhang, no sacrificial geometry.
// (Face-down would hang the ledges over the glass pocket; don't.)
brand_back = "SecuraCV Canary 7\" Display";   // debossed across the back plate
brand_edge = "SecuraCV Canary";               // debossed on the visible bottom edge
// microSD access — the card slides DOWNWARD out of its push-push socket (the
// purple-rectangle zone on the Rev1.2 board photo: right side, below centre,
// in-use back view). The opening in the BACK PLATE covers the socket, the
// card's slide travel, and room for a fingertip to keep hold of the card so
// it never drops inside the case. Photo-derived — MEASURE your board.
sd_dx = 42.0;   // opening centre, + = back-view right
sd_dy = -26.0;
sd_w  = 18.0;   // width — fingertip-sized, not card-sized
sd_l  = 40.0;   // length along the slide direction
gill_n  = 8;         // straight "gill" vents per side (±x) wall — the sides
gill_y0 = -35.0;     // carry vents only; SD access is through the back plate
gill_w = 2.4;  gill_l = 9.0;  gill_rake = 0;    // rake 0: vertical slots print
                     // cleanest; the raked look read as slashes and bought
                     // nothing thermally
frame_vent_flank_n = 4;  // exhaust slots per side, flanking the button window
label_depth = 0.5;
label_font  = "Liberation Sans:style=Bold";

/* [Frame — bottom USB port, stencil vents, TPU fitments] */
usb_port   = true;   // pass-through for the power cable, centred on the bottom wall.
                     // Feed the head through BEFORE the panel goes in, plug it,
                     // leave a service loop, then wrap the grommet on the wire.
usb_dx     = 0.0;    // port centre along the bottom wall — 0 = centred
usb_zc     = 11.0;   // port axis across the wall band (front face → back). The
                     // stencil vent words centre on this same line.
usb_head_w = 13.0;   // widest overmold head that must PASS — MEASURE your cable
usb_head_h = 7.0;    // overmold head thickness — MEASURE
usb_pass_c = 0.3;    // per-side clearance around the head through the opening
usb_wire_d = 3.8;    // cable jacket Ø the grommet grips — MEASURE
grom_lip   = 2.0;    // grommet flange width per side (both flanges)
grom_bell  = 3.2;    // strain-relief bell beyond the outer flange
// TPU fit system — these two do for the TPU parts what tol_slide/tol_hole do
// for the rigid ones. TPU seats by squeeze, so its knobs are interferences,
// not clearances.
tpu_squeeze = 0.2;   // per-side interference each TPU waist carries in its opening
tpu_grip    = 0.4;   // how much smaller the grommet bore is than the cable jacket
// BOOT/RESET plug — the buttons are pressed THROUGH the installed plug.
btn_reach  = 2.0;    // press-pip stand-off, wall inner face → just shy of the
                     // button cap — MEASURE the gap and subtract ~0.5
btn_pip_d  = 5.0;    // press-pip Ø (face dimples mark them outside)
btn_pip_dz = 0.0;    // pip centre offset across the wall band, from the window centre
// SD cover — peel tab at the card (fingertip) end, hinge tongue at the other.
sd_lip    = 2.0;     // cover cap border beyond the opening, per side (flush recess)
sd_tab    = 4.0;     // peel-tab length beyond the cap
sd_hinge  = 3.0;     // hinge-tongue hook depth beyond the opening (stays anchored)
// Bottom-edge stencil vents — the intake air path IS the lettering. Horizontal
// tie bands interrupt every glyph, so no counter (the island inside an A or R)
// is ever left floating: nothing to fall out, nothing for the slicer to bridge.
vent_text_l = "SECURACV";  // front-view left of the port
vent_text_r = "CANARY";    // front-view right
vent_text_size = 7.0;
vent_slat = 1.6;     // open slat height cut through each glyph
vent_tie  = 0.8;     // uncut tie band between slats (keeps counters attached)
vent_gap  = 8.0;     // clear space between the port flange and each word

/* [Stand] */
opt_stand = true;
stand_ang = 20;  stand_w = 210.0;  stand_d = 120.0;  stand_t = 5.0;

/* [Quality] */
$fa = 3; $fs = 0.5;

// ----------------------------------------------------------------------------
//  Derived. Axis: X = width, Y = height, Z = toward glass.
// ----------------------------------------------------------------------------
// TWO pockets, not one. The tray cavity takes whichever is bigger, the glass or
// the board, so an overhanging PCB still drops in. The bezel's pocket is always
// sized to the GLASS — a shared cavity sized to a taller PCB would leave the
// slab centimetres of lateral play, and the lip is flat: it retains the glass
// axially but cannot centre it, so the panel could slide until the window
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

// Frame (one-piece) derived. NOTE the frame is modelled PRINT-SIDE: z0 = the
// front face on the build plate, +z toward the back — so viewed from the BACK
// +x is right (from the front, +x is left). Board-relative x features
// therefore use -m3_ox where the two-part tray uses +m3_ox.
fr_xi = glass_w + 2*frame_reveal;  fr_yi = glass_h + 2*frame_reveal;
fr_ri = glass_r + frame_reveal;    // opening corners track the slab
fr_xo = fr_xi + 2*frame_wall;      fr_yo = fr_yi + 2*frame_wall;
fr_ro = fr_ri + frame_wall;
// The rear stack chains from the GLASS BACK: the PCB hangs from the glass on
// its own standoffs, so adhesive thickness moves the LEDGE, not the board —
// adh_t only sets where the ledge face sits, and the adhesive fills the gap
// between glass back and ledge. (Chaining the bosses from the ledge instead
// would open a screw gap equal to adh_t, or peel the adhesive closing it.)
ledge_z  = glass_t + adh_t;                    // ledge front face
fr_depth = glass_t + pcb_standoff + pcb_t + standoff_len + frame_boss_h + back_t;
fz_boss  = fr_depth - back_t - frame_boss_h;   // boss face the standoffs land on
fz_plate = fr_depth - back_t;                  // inner face of the back plate
fr_bosses = [for (sx = [1,-1], sy = [1,-1]) [-m3_ox + sx*m3_dx/2, m3_oy + sy*m3_dy/2]];
btn_z0 = glass_t + 1;  btn_z1 = fz_boss + 1;   // the band the buttons live in
btn_zc = (btn_z0 + btn_z1)/2;                  // window centre across the wall
// USB pass-through: the opening passes the HEAD; the grommet then fills it
// around just the wire, so the port needs no relation to where the connector
// actually is — the cable routes inside, and the case never has to know.
usb_open_w = usb_head_w + 2*usb_pass_c;
usb_open_h = usb_head_h + 2*usb_pass_c;
// Stencil vent words: each centred between the port flange and the wall's flat
// span end, on the port's own axis line. Width is estimated (no textmetrics in
// the render pipeline's OpenSCAD): bold caps advance ≈ 0.70 em × 1.12 tracking.
vword_x    = ((usb_open_w/2 + grom_lip + vent_gap) + (fr_xi/2 - fr_ri - 3))/2;
vword_half = 0.392 * vent_text_size * max(len(vent_text_l), len(vent_text_r));

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
assert(!mount_keyholes || (khm_dx + khm_head_d/2 + 2 < fr_xi/2
       && khm_y + khm_len + khm_slide_w/2 + 2 < fr_yi/2
       && khm_y - khm_head_d/2 > 4),
       "frame: keyhole runs off the back plate");
assert(khm_slide_w < khm_head_d, "frame: keyhole slide wider than its head hole");
assert(sd_dx + sd_w/2 + sd_lip + 2 < fr_xi/2
       && sd_dy + sd_l/2 + sd_lip + sd_hinge + 2 < fr_yi/2
       && sd_dy - sd_l/2 - sd_lip - sd_tab - 4 > -fr_yo/2 + frame_rim,
       "frame: SD cover (cap, hinge tongue or peel tab) runs off the back plate");
assert(min([for (p = fr_bosses) max(abs(sd_dx - p[0]) - sd_w/2 - sd_lip - frame_boss_d/2,
                                    abs(sd_dy - p[1]) - sd_l/2 - sd_lip - sd_tab - frame_boss_d/2)]) > 1,
       "frame: SD cover recess collides with a boss");
assert(!usb_port || (usb_zc - usb_open_h/2 - grom_lip > glass_t + 0.4
       && usb_zc + usb_open_h/2 + grom_lip < fz_boss - 0.2),
       "frame: USB port (plus its grommet flange) runs out of the bottom wall band");
assert(!usb_port || usb_wire_d - tpu_grip >= 1.0,
       "frame: grommet bore closes up — usb_wire_d vs tpu_grip");
assert(!usb_port || usb_wire_d + 2 < usb_open_h,
       "frame: cable jacket barely fits the opening the grommet must fill");
assert(vent_slat >= 1.2 && vent_tie >= 0.6,
       "frame: stencil vent slats/ties below printable feature size");
assert(vword_x - vword_half > usb_open_w/2 + grom_lip + 2
       && vword_x + vword_half + 2 < fr_xi/2 - fr_ri,
       "frame: stencil vent words don't fit between the USB port and the wall end");
assert(btn_reach >= 0.5 && btn_reach < 12,
       "frame: btn_reach out of sane range — measure wall inner face to button cap");
// the whole panel assembly (glass + standoffs + board) enters through the
// ledge opening, so the opening must clear the BOARD, not just the glass
assert(fr_xi - 2*ledge_side > pcb_w + 2 && fr_yi - ledge_top - ledge_bot > pcb_h + 2,
       "frame: adhesive ledge blocks the board's insertion path");
assert(ledge_z + ledge_t < glass_t + pcb_standoff,
       "frame: ledge intrudes into the board plane");

// Exported for canary_s3_lcd7_fitcheck.scad — the frame's own derived stack,
// so the frame fit gates read the real values instead of copies.
function lcd7_frame_stack() = [ledge_z, ledge_t, fr_xi, fr_yi, fr_ri, fr_depth];
// ...and the port/fitment geometry the TPU fit gates need, same doctrine.
// [usb_dx, usb_zc, usb_head_w, usb_head_h, fr_yi, fr_yo, ledge_bot,
//  btn_dx, btn_zc, sd_dx, sd_dy, fz_plate, usb_port]
function lcd7_ports() = [usb_dx, usb_zc, usb_head_w, usb_head_h, fr_yi, fr_yo,
                         ledge_bot, btn_dx, btn_zc, sd_dx, sd_dy, fz_plate,
                         usb_port ? 1 : 0];
assert(cb_h + 1.0 <= bez_h, "counterbore deeper than the bezel ear");
assert(cav_d > comp_h + pcb_t, "no air gap left between the PCB and the glass");
echo(str("Canary 7in touch v0.4-dev — outer ", xo, " x ", yo, " x ", bez_h + cav_d + back_t,
         " mm, window ", view_w, " x ", view_h, ", lip ", lip_min,
         " mm, vent area ~",
         round(vent_back ? vent_rows*vent_cols*vent_slot_w*vent_slot_l/100 : 0), " cm2",
         "  (IN DEVELOPMENT — MEASURE CONNECTORS)"));
echo(str("  stack: floor ", z_floor, " | PCB under ", z_pcb_under, " | PCB top ",
         z_pcb_top, " | glass back ", z_glass, " | closed height ",
         back_t + cav_d + bez_h, " mm"));
echo(str("  frame: ", fr_xo, " x ", fr_yo, " x ", fr_depth,
         " mm one-piece; glass opening ", fr_xi, " x ", fr_yi, " r", fr_ri,
         "; 4x M3x8-10 from the back into the panel standoffs (boss face at ",
         fz_boss, ", head seat at ", fz_plate, ")"));
if (usb_port)
    echo(str("  USB port: ", usb_open_w, " x ", usb_open_h,
             " stadium at (", usb_dx, ", z ", usb_zc, ") — passes a ",
             usb_head_w, " x ", usb_head_h, " head; grommet grips a Ø",
             usb_wire_d, " jacket. TPU fitments (grommet_usb / plug_buttons / ",
             "plug_sd): TPU 90-95A, EXTERNAL spool, never the AMS"));
// When the board is the bigger part, the tray cavity opens up to clear it and
// stops being what locates the slab — the bezel's glass pocket takes that job.
// Worth saying out loud, because it changes which part you check first if the
// panel ends up sitting crooked.
// (Phrased without the word CI greps for; this is a note, not a build failure.)
if (pcb_w > glass_w || pcb_h > glass_h)
    echo(str("  NOTE: PCB overhangs the glass by ",
             max(pcb_w - glass_w, 0)/2, " mm X / ",
             max(pcb_h - glass_h, 0)/2, " mm Y, so the tray cavity is board-sized; ",
             "the bezel's glass pocket is what centres the slab"));

// Exported for canary_s3_lcd7_fitcheck.scad, so the assembly check reads the
// real derived stack instead of a copy that can drift out of step with it.
// [back_t, cav_d, bez_h, glass_t, glass_w, glass_h, glass_r, z_glass]
function lcd7_stack() = [back_t, cav_d, bez_h, glass_t,
                         glass_w, glass_h, glass_r, z_glass];

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
module vent_grille(ox = m3_ox, oy = m3_oy, keepouts = []) {
    for (r = [0:vent_rows-1], c = [0:vent_cols-1]) {
        x = (c - (vent_cols-1)/2) * vent_pitch_x;
        y = (r - (vent_rows-1)/2) * vent_pitch_y;
        // keep the grille inside the PCB footprint, off the bosses, and out of
        // any caller-supplied keepout rectangles
        clear_boss = min([for (sx = [1,-1], sy = [1,-1])
            max(abs(x - (ox + sx*m3_dx/2)) - 6, abs(y - (oy + sy*m3_dy/2)) - 9)]) > 0;
        clear_keep = len(keepouts) == 0 ||
            min([for (k = keepouts) max(abs(x - k[0]) - k[2], abs(y - k[1]) - k[3])]) > 0;
        if (abs(x) < pcb_w/2 - 6 && abs(y) < pcb_h/2 - 6 && clear_boss && clear_keep)
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
                difference() { outline2d(); rrect2d(xc, yc, r_cav); }
            // PCB standoff bosses at the M3 pattern (offset — see m3_ox/m3_oy)
            for (sx = [1,-1], sy = [1,-1])
                translate([m3_ox + sx*m3_dx/2, m3_oy + sy*m3_dy/2, back_t - 0.01])
                    cylinder(d = 7.0, h = comp_h);
        }
        // M3 boss pilots
        for (sx = [1,-1], sy = [1,-1])
            translate([m3_ox + sx*m3_dx/2, m3_oy + sy*m3_dy/2, back_t + comp_h - 6]) cylinder(d = m3_pilot, h = 6.2);
        // case-screw self-tap pilots down the TOP of each full-height corner post
        // (the bezel's counterbored ear delivers the M3 into these)
        for (p = lobes())
            translate([p[0], p[1], back_t + cav_d - 12]) cylinder(d = lob_pilot, h = 12.1);
        // HEAT: back grille
        if (vent_back) vent_grille();
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
//  glass flush with the front rim. Get standoff_len right or the glass sits
//  proud/sunken by the same error.
// ----------------------------------------------------------------------------
module pill2d(l, w) { hull() for (d = [-1, 1]) translate([0, d*(l - w)/2]) circle(d = w); }
// A word as a slat stencil: the glyphs intersected with horizontal bands, so
// the cut-through letters double as vent slots. The uncut tie bands run the
// full width of every glyph, which is what keeps counters (the island inside
// an A or R) attached to the wall — no stencil font needed, no islands ever.
module slat_text(s) {
    n = max(2, floor((vent_text_size + vent_tie) / (vent_slat + vent_tie)));
    intersection() {
        text(s, size = vent_text_size, font = label_font, spacing = 1.12,
             halign = "center", valign = "center");
        for (i = [0 : n - 1])
            translate([0, (i - (n - 1)/2)*(vent_slat + vent_tie)])
                square([400, vent_slat], center = true);
    }
}
module frame_lbl(x, y, s) {
    translate([x, y, fr_depth - label_depth]) linear_extrude(label_depth + 0.1)
        text(s, size = 4.0, font = label_font, halign = "center", valign = "center");
}

// The shell, finished at both ends: a modelled foot chamfer at the plate (the
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

module frame() {
    gz = (glass_t + fz_boss)/2;        // centre of the clear air band in the walls
    gh = fz_boss - glass_t - 4;        // wall-vent height inside that band
    difference() {
        union() {
            difference() {
                frame_body();
                // one straight cavity, snug to the SLAB (the widest thing in
                // the stack — the board hangs inboard of it on its standoffs)
                translate([0, 0, -0.1]) linear_extrude(fz_plate + 0.1)
                    rrect2d(fr_xi, fr_yi, fr_ri);
            }
            // bosses hanging from the back plate's inner face
            for (p = fr_bosses) translate([p[0], p[1], fz_boss])
                cylinder(d = frame_boss_d, h = frame_boss_h + 0.01);
            // adhesive ledge behind the glass: a full ring at the glass-back
            // datum, widened where the panel's adhesive strips are (10 mm
            // sides, 6 mm button edge, 2 mm over the FPC)
            translate([0, 0, ledge_z]) linear_extrude(ledge_t) difference() {
                rrect2d(fr_xi, fr_yi, fr_ri);
                translate([0, (ledge_bot - ledge_top)/2])
                    rrect2d(fr_xi - 2*ledge_side, fr_yi - ledge_top - ledge_bot, 2);
            }
            // ...and each ledge's 45° back-slope wedge to its wall: solid
            // support under the adhesive landing, self-supporting when the
            // frame prints back-plate-down. Profile: flat landing at ledge_z,
            // inner face ledge_t tall, then 45° back up to the wall.
            for (sx = [1, -1])
                translate([sx*fr_xi/2, (fr_yi - 2*fr_ri)/2, ledge_z])
                    rotate([90, 0, 0]) linear_extrude(fr_yi - 2*fr_ri)
                        polygon([[0, 0], [-sx*ledge_side, 0],
                                 [-sx*ledge_side, ledge_t], [0, ledge_t + ledge_side]]);
            translate([-(fr_xi - 2*fr_ri)/2, fr_yi/2, ledge_z])
                rotate([90, 0, 90]) linear_extrude(fr_xi - 2*fr_ri)
                    polygon([[0, 0], [-ledge_top, 0],
                             [-ledge_top, ledge_t], [0, ledge_t + ledge_top]]);
            translate([(fr_xi - 2*fr_ri)/2, -fr_yi/2, ledge_z])
                rotate([90, 0, -90]) linear_extrude(fr_xi - 2*fr_ri)
                    polygon([[0, 0], [-ledge_bot, 0],
                             [-ledge_bot, ledge_t], [0, ledge_t + ledge_bot]]);
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
        // gill vents down each ±x wall — cut deep enough to pass through the
        // ledge wedge behind the wall, so they vent the cavity, not the wedge
        if (gill_n > 0) for (sx = [1, -1], i = [0 : gill_n - 1])
            translate([sx*(fr_xo/2 - frame_wall/2), gill_y0 + i*11, gz])
                rotate([sx*gill_rake, 0, 0]) rotate([0, 90, 0])
                    translate([0, 0, -(ledge_side + frame_wall)])
                        linear_extrude(2*(ledge_side + frame_wall))
                            pill2d(gill_l, gill_w);
        // exhaust through the top wall, flanking the button window
        for (sx = [1, -1], i = [0 : frame_vent_flank_n - 1])
            translate([btn_dx + sx*(btn_w/2 + 9 + i*6.5), fr_yi/2 - 0.1, gz])
                rotate([-90, 0, 0]) linear_extrude(frame_wall + 0.3) pill2d(gh, gill_w);
        // USB pass-through, centred on the bottom wall: a true stadium sized
        // to pass the power cable's overmold head, cut through the wall AND
        // the FPC-edge ledge/wedge behind it. The grommet fills it afterwards.
        if (usb_port) {
            translate([usb_dx, -fr_yi/2 + ledge_bot + 0.6, usb_zc])
                rotate([90, 0, 0]) linear_extrude(frame_wall + ledge_bot + 1.2)
                    rotate(90) pill2d(usb_open_w, usb_open_h);
            // 45° mouth bevel at the skin — the grommet's cone lands on it,
            // and the empty port reads finished when no cable is fitted
            hull() {
                translate([usb_dx, -fr_yo/2 + 0.01, usb_zc]) rotate([90, 0, 0])
                    linear_extrude(0.02) rotate(90)
                        pill2d(usb_open_w + 1.6, usb_open_h + 1.6);
                translate([usb_dx, -fr_yo/2 + 0.81, usb_zc]) rotate([90, 0, 0])
                    linear_extrude(0.02) rotate(90) pill2d(usb_open_w, usb_open_h);
            }
            // relieve the ledge ring + wedge behind the port so the grommet's
            // inner flange lands on a flat wall face. The panel carries no
            // adhesive on this stretch of the border — the FPC owns it — so
            // nothing structural is lost.
            translate([usb_dx, -fr_yi/2 + (ledge_bot + 1)/2 - 0.01,
                       ledge_z + (ledge_t + ledge_bot)/2])
                cube([usb_open_w + 2*grom_lip + 2, ledge_bot + 1,
                      ledge_t + ledge_bot + 0.6], center = true);
        }
        // the bottom edge breathes through its own name: the brand words are
        // cut THROUGH the wall as slat-stencil intake vents flanking the port,
        // centred on its axis. Readable standing below and in front, letter
        // tops toward the screen — where the debossed edge brand used to be.
        for (s = [1, -1])
            translate([0, -fr_yi/2 + ledge_bot + 0.6, usb_zc])
                rotate([90, 0, 0]) rotate([0, 0, 180]) translate([-s*vword_x, 0])
                    linear_extrude(frame_wall + ledge_bot + 1.2)
                        slat_text(s > 0 ? vent_text_l : vent_text_r);
        // back grille (dodging bosses and the keyholes — note -m3_ox: this
        // part is modelled print-side, x mirrored vs the two-part tray)
        translate([0, 0, fz_plate]) vent_grille(-m3_ox, m3_oy,
            keepouts = concat(
                mount_keyholes
                    ? [for (sx = [1,-1]) [sx*khm_dx, khm_y + khm_len/2, 8, 17]] : [],
                // the SD keepout reaches past the tab end to cover the cover's
                // flush recess and the nail scoop below it
                [[sd_dx, sd_dy - (sd_tab + 6)/2, sd_w/2 + 3.4,
                  sd_l/2 + 6.7 + (sd_tab + 6)/2]]));
        // microSD access through the back plate: socket + slide travel +
        // fingertip, so the card comes out without being dropped inside
        translate([sd_dx, sd_dy, fz_plate - 0.1])
            linear_extrude(back_t + 0.2) rrect2d(sd_w, sd_l, 5);
        // flush recess for the SD cover's cap + peel tab (hairline reveal),
        // and a fingernail scoop just past the tab tip to get under it
        translate([sd_dx, sd_dy, fr_depth - 0.8]) linear_extrude(0.9)
            sd_cap_2d(0.3);
        translate([sd_dx, sd_dy - sd_l/2 - sd_lip - sd_tab - 1.0, fr_depth + 7.2])
            sphere(r = 7.8);
        // wall-mount keyholes through the back plate: head hole LOW, slide
        // running UP so the catch points at the button edge — hang the case
        // over two screws and slide it DOWN to seat
        if (mount_keyholes) for (sx = [1, -1])
            translate([sx*khm_dx, khm_y, fz_plate - 0.1]) linear_extrude(back_t + 0.2) {
                circle(d = khm_head_d);
                hull() { circle(d = khm_slide_w);
                         translate([0, khm_len]) circle(d = khm_slide_w); }
            }
        // debossed labels on the back face — back view, buttons at the TOP:
        // BOOT on the left (-x here), RESET on the right, as on the board;
        // "SD" beside the card window so nobody hunts for the socket
        frame_lbl(btn_dx - btn_lbl_dx, fr_yo/2 - 6.5, "BOOT");
        frame_lbl(btn_dx + btn_lbl_dx, fr_yo/2 - 6.5, "RESET");
        frame_lbl(sd_dx, sd_dy + sd_l/2 + sd_lip + 4.7, "SD");
        // full product mark across the clear band under the grille — shifted
        // left of the SD cover's recess/tab zone, which owns the lower-right
        // of the plate (the default string renders ±36.8 wide at size 4, so
        // its centre sits where the right edge clears the recess by ≥1)
        frame_lbl(min(0, sd_dx - sd_w/2 - sd_lip - 1.5 - 37), -(fr_yi/2 - 6), brand_back);
    }
}

// One corner of the frame, cut from the real geometry by intersection — the
// (+x,+y) corner, chosen because it contains a boss AND a wall keyhole. Assemble
// it on the panel's corner with one M3x8-10: the glass corner proves glass_r,
// the screw only threads home if the m3 offsets have the right SIGNS, and the
// glass sits flush with the rim only if standoff_len is right.
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
module usb_grommet() {
    fw = usb_open_w + 2*grom_lip;  fh = usb_open_h + 2*grom_lip;
    ww = usb_open_w + 2*tpu_squeeze;  wh = usb_open_h + 2*tpu_squeeze;
    difference() {
        union() {
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
        // wrap the wire after the head is already plugged in
        cylinder(d = usb_wire_d - tpu_grip, h = 40, center = true);
        translate([-0.2, 0, -0.1]) cube([0.4, fh/2 + 1, 20]);
    }
}

// BOOT/RESET plug. Local z: 0 = cap face (on the bed). The cap nests the
// window's 45° bevel with a hairline reveal; the waist squeezes the straight
// land; the 45° lip snaps out behind the wall — push it in from outside and
// it is captive: the cap can't pass inward, the lip won't fall out, and it
// never needs to come out, because the buttons are pressed THROUGH it: two
// pips under the face dimples carry the press to BOOT and RESET.
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
            // press pips — stepped nose so the tip is narrow where it meets
            // the button cap. Reach is a MEASURE knob (btn_reach).
            for (sx = [1, -1]) translate([sx*btn_lbl_dx, btn_pip_dz, 3.6 - 0.01]) {
                cylinder(d = btn_pip_d, h = max(0.4, btn_reach - 0.5));
                cylinder(d = btn_pip_d - 1.2, h = btn_reach);
            }
        }
        // finger-findable dimples on the cap face, one over each pip
        for (sx = [1, -1])
            translate([sx*btn_lbl_dx, btn_pip_dz, -7.31]) sphere(r = 7.81);
    }
}

// SD cover cap outline (cap + peel tab), shared with the frame's flush recess
// cut so the two cannot drift apart. Local +y = the hinge end.
module sd_cap_2d(o = 0) {
    offset(r = o) union() {
        rrect2d(sd_w + 2*sd_lip, sd_l + 2*sd_lip, 5 + sd_lip);
        hull() {
            translate([0, -(sd_l/2 + sd_lip - 0.5)]) square([11, 1], center = true);
            translate([0, -(sd_l/2 + sd_lip + sd_tab - 4)]) circle(d = 8);
        }
    }
}

// SD peel cover. Local z: 0 = cap face (on the bed). Battery-door motion:
// tilt the hinge tongue in under the plate's top edge first, then press the
// perimeter lip home — the cap sits flush in its recess. To open, get a nail
// into the scoop, peel the tab up: the shallow lip pops progressively and the
// cover hinges open on the tongue, still attached — nothing to lose, and the
// tongue stops it being pushed inside.
module sd_cover() {
    ww = sd_w + 2*tpu_squeeze;  wl = sd_l + 2*tpu_squeeze;
    union() {
        linear_extrude(0.8) sd_cap_2d();                       // flush cap + tab
        translate([0, 0, 0.8 - 0.01])
            linear_extrude(2.22) rrect2d(ww, wl, 5.2);         // waist
        hull() {   // shallow snap lip, 45° — pops out under a peel
            translate([0, 0, 3.0 - 0.01]) linear_extrude(0.01) rrect2d(ww, wl, 5.2);
            translate([0, 0, 4.2]) linear_extrude(0.4)
                rrect2d(ww + 1.8, wl + 1.8, 6.1);
        }
        hull() {   // hinge tongue — 45° underside, slides in under the plate
            translate([0, wl/2 - 0.6, 3.0]) cube([14, 1.2, 0.01], center = true);
            translate([0, wl/2 + sd_hinge/2 - 0.6, 3.0 + sd_hinge])
                cube([14, sd_hinge + 1.2, 0.01], center = true);
        }
    }
}

// The fitments in their INSTALLED positions in the frame's coordinates —
// used by canary_s3_lcd7_fitcheck.scad's TPU gates, so the gates test the
// exact transforms these comments claim.
module usb_grommet_installed() {
    translate([usb_dx, -fr_yi/2 + 1.6, usb_zc]) rotate([90, 0, 0]) usb_grommet();
}
module button_plug_installed() {
    translate([btn_dx, fr_yo/2, btn_zc]) rotate([90, 0, 0]) button_plug();
}
module sd_cover_installed() {
    translate([sd_dx, sd_dy, fr_depth]) rotate([0, 180, 0]) sd_cover();
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
// the frame and its gauge EXPORT back-plate-down: the print orientation the
// ledge wedges are self-supporting in (face-down would hang them over the
// glass pocket)
else if (part == "frame")       rotate([180, 0, 0]) translate([0, 0, -fr_depth]) frame();
else if (part == "frame_gauge") rotate([180, 0, 0]) translate([0, 0, -fr_depth]) frame_gauge();
else if (part == "gauge")       gauge();
else if (part == "gauge_tray")  gauge_corner("back");
else if (part == "gauge_bezel") gauge_corner("bezel");
else if (part == "stand") stand();
// TPU fitments export in their print orientation (A-face down), as modelled
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
    if (opt_stand) translate([0, -(yo/2 + stand_d/2 + 16), 0]) stand();
}
