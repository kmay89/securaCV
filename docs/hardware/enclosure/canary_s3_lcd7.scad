// ============================================================================
//  Canary — 7" TOUCH DASHBOARD CASE  ⚠️ IN DEVELOPMENT (v0.3-dev)
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
//    stand — free-standing desk DOCK for the FRAME case (optional, no
//            hardware). Reclined drop-in slot sized to the frame's derived
//            outer depth; the case rests on two tilted seat pads (the cheek
//            tops) with an open well between them, so the bottom-wall intake
//            vents keep their convection path AND the USB power lead that
//            leaves through the frame's bottom cable port has plug headroom,
//            then routes out the back through a desk-level cable channel.
//            Two chamfered CENTRING KEYS on the keyed ribs rise into the
//            case's ±dock_key_dx openings — intake slots in landscape, the
//            side-wall keying slots in portrait — so either way up, the
//            case finds its own centre and cannot slide out sideways.
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
//            4x M3x8-10 from the back thread into those standoffs — the
//            screws, not a ledge, pull the glass flush with the front face.
//            BOOT/RESET window in the top wall with debossed labels; gill
//            vents down each side wall; chimney intake/exhaust; back grille;
//            a microSD access opening through the BACK PLATE covering the
//            socket, the card's slide travel and a fingertip ("SD" deboss);
//            keyhole wall mounts (hang on two screws, slide down); an
//            adhesive LEDGE behind the glass matched to the panel's own
//            adhesive strips (10 mm sides, 6 mm button edge, 2 mm over the
//            FPC), each with a 45° back-slope wedge to its wall; branding on
//            the back plate and the visible bottom edge. PRINTS BACK-PLATE-
//            DOWN — the orientation the ledge wedges self-support in.
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
part = "all";        // ["bezel","back","frame","frame_gauge","gauge","gauge_bezel","gauge_tray","stand","stand_gauge","all"]

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
frame_vent_row_n   = 10; // intake slots in a row along the bottom wall
fv_x0    = 34.0;     // first intake slot's ± position — the stand's well and
fv_pitch = 7.0;      // seat ribs are laid out against these, so they are
                     // named, not literals
frame_vent_flank_n = 4;  // exhaust slots per side, flanking the button window
// Bottom cable port — the power lead leaves through the BOTTOM wall, straight
// down into the desk dock's well and out its cable channel (a reference case
// print has a solid 3 mm bottom wall and the cable has nowhere to go — this
// is the opening the dock is designed around). Sized to pass a USB-C plug so
// the lead can be threaded after assembly. Wall-mount builds can turn it off
// and get the TV-style edge brand back — port and brand share the wall's
// centre, so only one of them can exist at a time.
opt_bottom_cable = true;
cable_port_w = 14.0;  cable_port_h = 9.0;  cable_port_dx = 0.0;
// Dock keying — the dock's two chamfered centring keys rise into openings the
// case already has (or gets here), so the docked case self-centres and cannot
// slide sideways — the same doctrine as hanging the board on the panel's own
// standoffs: let the part's own features locate it.
//   landscape: the keys engage the ±(fv_x0 + 2*fv_pitch) INTAKE slots — no
//              extra bottom-wall openings at all.
//   portrait:  the ±x walls get one gill-style KEYING SLOT each at
//              dy = ±dock_key_dx, past the end of the gill row (asserted),
//              so whichever side wall faces down, the keys engage it too.
dock_keys   = true;
dock_key_dx = fv_x0 + 2*fv_pitch;   // 48 — shared by both engagements above
label_depth = 0.5;
label_font  = "Liberation Sans:style=Bold";

/* [Stand] — desk dock for the FRAME case (see the header). The slot is sized
   to the frame's DERIVED outer depth, so editing the frame's stack re-sizes
   the dock with it. A reference frame STL measures 23.5 mm against the
   derived 24.0 — stand_clear covers both without rattle. */
opt_stand = true;
stand_ang     = 20;    // recline from vertical
stand_w       = 174.0; // dock width — DELIBERATELY narrower than the case, so
                       // the side-wall gills stay in clear air and the case
                       // lifts straight out by its overhanging ends
stand_d       = 126.0; // base plate depth — sized so the base reaches well
                       // behind the reclined case's centre line in PORTRAIT
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
stand_rib_x   = 48.25; // ± KEYED RIBS: blades across the well that carry the
stand_rib_w   = 9.5;   // case in PORTRAIT (its 115 mm width misses the cheeks
                       // entirely) and carry the centring keys on top. Each
                       // blade straddles the key's slot — bearing on solid
                       // wall on BOTH sides of it, in both orientations —
                       // and clears the neighbouring intake slots and the
                       // gill row (all asserted).
stand_floor_h = 26.0;  // seat height: case bottom edge -> desk. This is the
                       // headroom for the USB power plug leaving the hole in
                       // the case's bottom wall — a straight plug + strain
                       // relief needs ~20 mm before the cable can bend away
stand_slot_y  = 32.0;  // seat centreline, measured from the plate's front edge
stand_clear   = 0.5;   // per-face case<->slot clearance (drop-in, not press)
stand_cable_w = 16.0;  // desk-level cable channel width (through plate + fin foot)
stand_feet    = true;  // 4x shallow recesses for adhesive rubber feet

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

// Stand derived. The dock is drawn in ITS print orientation (base on the
// plate, +z up, +y toward the back fin); the "seat frame" is the tilted
// coordinate system of the docked case — origin on the seat pads' plane at
// the seat centreline, local +z running up the reclined case.
std_cd   = fr_depth + 2*stand_clear;      // slot gap: the frame's outer depth + slack
std_ys   = -stand_d/2 + stand_slot_y;     // seat centreline in plate coords
std_open = stand_w - 2*stand_cheek_t;     // clear span between the seat pads
// outermost bottom-wall intake slot on the frame — the well must clear it
std_intake_x = fv_x0 + (frame_vent_row_n/2 - 1)*fv_pitch + gill_w/2;
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
assert(!mount_keyholes || (khm_dx + khm_head_d/2 + 2 < fr_xi/2
       && khm_y + khm_len + khm_slide_w/2 + 2 < fr_yi/2
       && khm_y - khm_head_d/2 > 4),
       "frame: keyhole runs off the back plate");
assert(khm_slide_w < khm_head_d, "frame: keyhole slide wider than its head hole");
assert(sd_dx + sd_w/2 + 2 < fr_xi/2 && abs(sd_dy) + sd_l/2 + 2 < fr_yi/2,
       "frame: SD opening runs off the back plate");
assert(min([for (p = fr_bosses) max(abs(sd_dx - p[0]) - sd_w/2 - frame_boss_d/2,
                                    abs(sd_dy - p[1]) - sd_l/2 - frame_boss_d/2)]) > 1,
       "frame: SD opening collides with a boss");
// the whole panel assembly (glass + standoffs + board) enters through the
// ledge opening, so the opening must clear the BOARD, not just the glass
assert(fr_xi - 2*ledge_side > pcb_w + 2 && fr_yi - ledge_top - ledge_bot > pcb_h + 2,
       "frame: adhesive ledge blocks the board's insertion path");
assert(ledge_z + ledge_t < glass_t + pcb_standoff,
       "frame: ledge intrudes into the board plane");

// Exported for canary_s3_lcd7_fitcheck.scad — the frame's own derived stack,
// so the frame fit gates read the real values instead of copies.
function lcd7_frame_stack() = [ledge_z, ledge_t, fr_xi, fr_yi, fr_ri, fr_depth];
assert(cb_h + 1.0 <= bez_h, "counterbore deeper than the bezel ear");
assert(cav_d > comp_h + pcb_t, "no air gap left between the PCB and the glass");
// the lip climbs the case's front border — it must never reach the window,
// in landscape OR portrait (portrait's border is the short-edge border)
assert(stand_lip_h + stand_clear <= fr_yo/2 - view_h/2 + aa_dy - 1.0,
       "stand: the front lip would cover the bottom of the screen window");
assert(stand_lip_h + stand_clear <= fr_xo/2 - view_w/2 - 1.0,
       "stand: the front lip would cover the window in portrait");
// the well between the pads must span every bottom-wall intake slot, or the
// dock smothers the convection path the case depends on
assert(std_open/2 >= std_intake_x + 2,
       "stand: the seat pads sit on the case's intake vents — widen stand_w or thin the cheeks");
assert(std_front_foot > -stand_d/2 + 2,
       "stand: the front lip runs off the base plate — deepen stand_d or raise stand_slot_y");
// anti-tip, BOTH orientations: the base must extend well behind the reclined
// case's centre line — portrait stands a 197 mm slab on its side, so it gets
// the bigger margin (a fingertip pressing the top of the touchscreen pries
// against exactly this lever)
assert(stand_d/2 - (std_ys + sin(stand_ang)*fr_yo/2) >= 12,
       "stand: base too short behind the reclined case — it will tip backward");
assert(stand_d/2 - (std_ys + sin(stand_ang)*fr_xo/2) >= 20,
       "stand: base too short for PORTRAIT — deepen stand_d");
assert(stand_floor_h >= 20,
       "stand: no headroom under the case for the USB power plug");
assert(stand_cable_w + 8 < std_open, "stand: cable channel wider than the well");
assert(stand_gusset_h < stand_fin_h, "stand: cheek gusset overruns the fin");
// keyed ribs: each blade must straddle its key's opening — clear of the
// NEIGHBOURING intake slots in landscape and clear of the gill row in
// portrait — and the key must sit inside the blade with bearing both sides
assert(stand_rib_x - stand_rib_w/2 > fv_x0 + fv_pitch + gill_w/2 + 0.4
    && stand_rib_x + stand_rib_w/2 < fv_x0 + 3*fv_pitch - gill_w/2 - 0.4,
       "stand: keyed rib lands on a neighbouring intake slot — re-centre stand_rib_x");
assert(!dock_keys || abs(dock_key_dx - stand_rib_x) < stand_rib_w/2 - 2.5,
       "stand: the centring key overhangs its rib — re-centre stand_rib_x on dock_key_dx");
assert(stand_rib_x - stand_rib_w/2 > gill_y0 + (gill_n - 1)*11 + gill_w/2 + 0.2
    && -(stand_rib_x - stand_rib_w/2) < gill_y0 - gill_w/2,
       "stand: keyed rib lands on the gill row in portrait");
assert(stand_rib_x + stand_rib_w/2 < fr_yo/2 - fr_ro && stand_rib_x > fr_yo/4,
       "stand: keyed ribs miss the portrait case's flat or stand too narrow a stance");
// the keys engage an EXISTING intake slot pair in landscape; the portrait
// keying slots on the ±x walls must sit past the gill row and off the corner
assert(!dock_keys
    || (dock_key_dx - gill_w/2 > gill_y0 + (gill_n - 1)*11 + gill_w/2 + 1
     && -(dock_key_dx + gill_w/2) < gill_y0 - gill_w/2 - 1
     && dock_key_dx + gill_l/2 + 0.5 < fr_yi/2 - fr_ri),
       "frame: portrait keying slot collides with the gill row or the wall corner");
assert(!opt_bottom_cable || !dock_keys
    || abs(cable_port_dx) + cable_port_w/2 + 2 < dock_key_dx - gill_w/2,
       "frame: cable port runs into the keys' intake slots");
echo(str("Canary 7in touch v0.3-dev — outer ", xo, " x ", yo, " x ", bez_h + cav_d + back_t,
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
echo(str("  stand: ", stand_w, " x ", stand_d, " base, ", stand_ang,
         "° recline, slot ", std_cd, " mm for the ", fr_depth,
         " mm frame, seat ", stand_floor_h, " mm over the desk (plug room), ",
         "well ", std_open - 6, " mm across the intake vents; landscape on the",
         " cheek pads keyed at ±", dock_key_dx, ", portrait on ribs at ±",
         stand_rib_x, " (cable exits sideways in portrait)"));
if (opt_bottom_cable)
    echo("  frame: bottom cable port ON — the TV-style edge brand is off (they share the wall centre); wall-mount builds set opt_bottom_cable=false to get it back");
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
// Same idea for the dock, so the fitcheck can seat the real frame in the real
// stand instead of trusting a re-derived copy of either. fr_xo rides along so
// the portrait pose (case on its side) can be checked too.
// [stand_ang, std_ys, stand_floor_h, fr_depth, fr_yo, fr_xo]
function lcd7_stand_stack() = [stand_ang, std_ys, stand_floor_h, fr_depth, fr_yo, fr_xo];

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
    btn_zc = (btn_z0 + btn_z1)/2;
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
        // intake rows along the bottom wall, split to flank the centre band
        for (sx = [1, -1], i = [0 : frame_vent_row_n/2 - 1])
            translate([sx*(fv_x0 + i*fv_pitch), -fr_yi/2 + 0.1, gz])
                rotate([90, 0, 0]) linear_extrude(frame_wall + 0.3) pill2d(gh, gill_w);
        // dock keying, PORTRAIT: one gill-style slot in each ±x wall at
        // dy = ±dock_key_dx, past the end of the gill row. In landscape the
        // dock's centring keys engage the ±dock_key_dx INTAKE slots above —
        // the case's own vents locate it; these four slots do the same job
        // for whichever side wall faces down in portrait.
        if (dock_keys) for (sx = [1, -1], sy = [1, -1])
            translate([sx*(fr_xo/2 - frame_wall/2), sy*dock_key_dx, gz])
                rotate([0, 90, 0]) translate([0, 0, -(ledge_side + frame_wall)])
                    linear_extrude(2*(ledge_side + frame_wall))
                        pill2d(gill_l, gill_w);
        // power-cable port through the bottom wall's centre, bevelled like
        // the other windows: the USB lead drops straight down into the desk
        // dock's well and leaves through its cable channel. Sized to pass
        // the PLUG, so the lead threads through after assembly.
        if (opt_bottom_cable) {
            translate([cable_port_dx, -fr_yi/2 + 0.1, gz]) rotate([90, 0, 0])
                linear_extrude(frame_wall + 0.3) rrect2d(cable_port_w, cable_port_h, 3);
            hull() {
                translate([cable_port_dx, -fr_yo/2 + 0.01, gz]) rotate([90, 0, 0])
                    linear_extrude(0.02) rrect2d(cable_port_w + 2.4, cable_port_h + 2.4, 4);
                translate([cable_port_dx, -fr_yo/2 + 1.2, gz]) rotate([90, 0, 0])
                    linear_extrude(0.02) rrect2d(cable_port_w, cable_port_h, 3);
            }
        }
        // branding, TV-style on the visible bottom edge: readable standing
        // below and in front, letter tops toward the screen. The cable port
        // owns the wall's centre when it is on — one or the other.
        if (!opt_bottom_cable)
            translate([0, -fr_yo/2 + label_depth, gz])
                rotate([90, 0, 0]) rotate([0, 0, 180]) linear_extrude(label_depth + 0.2)
                    text(brand_edge, size = 5.5, font = label_font,
                         halign = "center", valign = "center");
        // back grille (dodging bosses and the keyholes — note -m3_ox: this
        // part is modelled print-side, x mirrored vs the two-part tray)
        translate([0, 0, fz_plate]) vent_grille(-m3_ox, m3_oy,
            keepouts = concat(
                mount_keyholes
                    ? [for (sx = [1,-1]) [sx*khm_dx, khm_y + khm_len/2, 8, 17]] : [],
                [[sd_dx, sd_dy, sd_w/2 + 3.4, sd_l/2 + 6.7]]));
        // microSD access through the back plate: socket + slide travel +
        // fingertip, so the card comes out without being dropped inside
        translate([sd_dx, sd_dy, fz_plate - 0.1])
            linear_extrude(back_t + 0.2) rrect2d(sd_w, sd_l, 5);
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
        frame_lbl(sd_dx, sd_dy + sd_l/2 + 4, "SD");
        // full product mark across the clear band under the grille
        frame_lbl(0, -(fr_yi/2 - 6), brand_back);
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
// A blade standing in the well, from the desk up to the tilted pad plane:
// the portrait seat ribs and the centring-key posts. It overruns the slot by
// 2 mm at each end so it welds into the lip's and fin's feet.
module stand_wellblade(x0, w) {
    difference() {
        stand_seatframe() translate([x0 - w/2, -std_cd/2 - 2, -45])
            cube([w, std_cd + 4, 45]);
        translate([0, 0, -50]) cube([stand_w + 40, stand_d + 300, 100], center = true);
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
            // base plate with a modelled foot chamfer (slicer compensation 0)
            hull() {
                linear_extrude(0.01) rrect2d(stand_w - 1.2, stand_d - 1.2, 9.4);
                translate([0, 0, 0.6]) linear_extrude(0.01) rrect2d(stand_w, stand_d, 10);
            }
            translate([0, 0, 0.6]) linear_extrude(stand_plate_t - 0.6)
                rrect2d(stand_w, stand_d, 10);
            // lip + fin: full-width raked blades, run long below the seat so
            // they fuse with the plate at any recline (trimmed at the desk)
            stand_seatframe() {
                translate([-stand_w/2, -std_cd/2 - stand_lip_t, -60])
                    cube([stand_w, stand_lip_t, 60 + stand_lip_h]);
                translate([-stand_w/2, std_cd/2, -60])
                    cube([stand_w, stand_fin_t, 60 + stand_fin_h]);
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
        translate([0, 0, -50]) cube([stand_w + 40, stand_d + 200, 100], center = true);
        // the seat pocket: everything above the tilted pad plane, between
        // the blades, the FULL width — the case overhangs both cheeks
        stand_seatframe() translate([-stand_w/2 - 1, -std_cd/2, 0])
            cube([stand_w + 2, std_cd, 200]);
        // sky cut: in front of the pocket, everything above the lip's capture
        stand_seatframe() translate([-stand_w/2 - 1, -300 - std_cd/2, stand_lip_h])
            cube([stand_w + 2, 300, 300]);
        // entry flares: lead-in chamfers on the lip top and the fin top, so
        // the case finds a ~30 mm mouth instead of a 25 mm slot
        stand_seatframe() {
            hull() {
                translate([-stand_w/2 - 1, -std_cd/2 - 0.04, stand_lip_h - 2.5])
                    cube([stand_w + 2, 0.04, 2.6]);
                translate([-stand_w/2 - 1, -std_cd/2 - 2.5, stand_lip_h - 0.04])
                    cube([stand_w + 2, 2.5, 0.04]);
            }
            hull() {
                translate([-stand_w/2 - 1, std_cd/2, stand_fin_h - 2.5])
                    cube([stand_w + 2, 0.04, 2.6]);
                translate([-stand_w/2 - 1, std_cd/2, stand_fin_h - 0.04])
                    cube([stand_w + 2, 2.5, 0.04]);
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
                        pill2d(stand_fin_h - 26, px[1]);
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
            cube([stand_cable_w, stand_d/2 - std_ys + 2, 10]);
        // rubber-foot recesses in the corners, clear of well and channel
        if (stand_feet) for (sx = [1, -1], sy = [1, -1])
            translate([sx*(stand_w/2 - 14), sy*(stand_d/2 - 12), -0.1])
                cylinder(d = 10.5, h = 0.9);
    }
    // well furniture, added AFTER the cuts (the well would otherwise carve
    // it away): the two KEYED RIBS — each blade seats the portrait case AND
    // carries a centring key. The key is a chamfered wedge rising into the
    // case's opening at ±dock_key_dx — an intake slot in landscape, the
    // side-wall keying slot in portrait — 1.5 proud of a 2 mm wall, so it
    // never bottoms out. The taper is what makes the case FIND centre:
    // drop it anywhere close and it slides home; the blade bears on solid
    // wall on both sides of the slot, so nothing sits tilted on a key.
    for (sx = [1, -1]) {
        stand_wellblade(sx*stand_rib_x, stand_rib_w);
        // the key is a compact stud, not a long fin, because it engages two
        // DIFFERENTLY-ORIENTED slots: the bottom intake slot is 2.4 wide
        // ACROSS the wall (landscape), the side keying slot is 2.4 wide
        // ACROSS the depth band (portrait). Both are centred on the same
        // depth (gz), so a stud at local y = -1 sits in either.
        if (dock_keys) stand_seatframe() hull() {
            translate([sx*dock_key_dx - 0.9, -1.95, -0.5]) cube([1.8, 1.9, 0.5]);
            translate([sx*dock_key_dx - 0.3, -1.35, 1.5])  cube([0.6, 0.7, 0.02]);
        }
    }
    }
    // one plan-silhouette trim for everything: the blades' and cheeks' feet
    // follow the base plate's rounded corners instead of overhanging them
    translate([0, 0, -1]) linear_extrude(300) rrect2d(stand_w, stand_d, 10);
    }
}

// One cheek's slice of the dock, cut from the real geometry by intersection —
// slot width, recline, seat height, lip capture and entry flares, for ~15 %
// of the dock's filament. Drop your printed FRAME's bottom corner in: it
// should seat on the pad with visible clearance front and back, and the lip
// should stop 2+ mm short of the screen window.
module stand_gauge() {
    intersection() {
        stand();
        translate([std_open/2 - 6, -stand_d/2 - 1, -1])
            cube([stand_cheek_t + 8, stand_d + 60, 120]);
    }
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
else if (part == "stand_gauge") stand_gauge();
else {
    bezel_print();
    translate([xo + 16, 0, 0]) back();
    translate([-(xo + 20), 0, 0]) frame();
    if (opt_stand) translate([0, -(yo/2 + stand_d/2 + 16), 0]) stand();
}
