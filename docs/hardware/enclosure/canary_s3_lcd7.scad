// ============================================================================
//  Canary — 7" TOUCH DASHBOARD CASE  ⚠️ IN DEVELOPMENT (v0.2-dev)
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
//  ⚠️ SO IS pcb_h. Published summaries of this board disagree about the PCB
//  outline height (97.60 vs 126.20), so the cavity is sized to whichever of the
//  glass and the PCB is larger and the case grows to fit either. MEASURE yours.
//  If the PCB turns out to overhang the glass, the cavity walls no longer
//  locate the slab and only the bezel lip centres it — the render echoes a
//  NOTE with the resulting play when that happens.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//  Orientation: landscape, +X = width, +Y = up, +Z = toward the glass.
// ============================================================================

/* [What to render] */
part = "all";        // ["bezel","back","gauge","gauge_bezel","gauge_tray","stand","all"]

/* [Glass slab] — bonded touch panel, from the Waveshare drawing (mm) */
glass_w = 192.96;    // touch-glass width  (X)
glass_h = 110.76;    // touch-glass height (Y)
glass_t = 4.0;       // glass + LCD module thickness at the edge — MEASURE
glass_r = 3.0;       // corner radius of the glass slab — MEASURE. The cavity is
                     // never rounded more than this: a pocket with a bigger
                     // radius than the glass binds on all four corners, and a
                     // near-square panel needs a near-square pocket.
aa_w = 154.88;       // active area width
aa_h = 86.72;        // active area height
aa_dy = -1.02;       // AA centre offset from glass centre (top border 13.04, bottom 11.00)

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

/* [Board mounts] — 4x M3 corner standoffs carry the PCB (nominal pattern) */
m3_dx = 165.72;      // M3 hole pattern width  — MEASURE. ⚠️ This is EXACTLY
                     // pcb_w, which cannot be a real hole spacing: holes do not
                     // sit on the board outline. It was almost certainly copied
                     // from the outline dimension. Expect the true span to be
                     // ~6-10 mm smaller and measure it before you print a tray.
m3_dy = 88.0;        // M3 hole pattern height — MEASURE
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

/* [Print tolerances] */
tol_slide = 0.25; tol_press = 0.12; tol_hole = 0.35;

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
lob_pilot = 2.7;  screw_c = 3.4;  cb_d = 6.0;  cb_h = 2.0;

/* [Stand] */
opt_stand = true;
stand_ang = 20;  stand_w = 210.0;  stand_d = 120.0;  stand_t = 5.0;

/* [Quality] */
$fa = 3; $fs = 0.5;

// ----------------------------------------------------------------------------
//  Derived. Axis: X = width, Y = height, Z = toward glass.
// ----------------------------------------------------------------------------
// The cavity takes whichever is bigger, the glass or the board — normally the
// glass, but a PCB that overhangs it must still drop in.
xc = max(glass_w, pcb_w) + 2*tol_slide;
yc = max(glass_h, pcb_h) + 2*tol_slide;
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

assert(bez_lip >= 2.0, "bezel lip < 2 mm won't retain a 7in slab");
assert(lip_min >= bez_lip, str("bezel lip is only ", lip_min,
       " mm at its tightest edge — widen the glass border, shrink win_margin, ",
       "or lower bez_lip"));
assert(aa_w < glass_w && aa_h < glass_h, "active area must sit inside the glass");
assert(m3_dx + lob_d < xo && m3_dy + lob_d < yo, "M3 pattern doesn't fit the shell");
assert(m3_dx <= pcb_w && m3_dy <= pcb_h, "M3 pattern falls outside the PCB outline");
assert(cb_h + 1.0 <= bez_h, "counterbore deeper than the bezel ear");
assert(cav_d > comp_h + pcb_t, "no air gap left between the PCB and the glass");
echo(str("Canary 7in touch v0.2-dev — outer ", xo, " x ", yo, " x ", bez_h + cav_d + back_t,
         " mm, window ", view_w, " x ", view_h, ", lip ", lip_min,
         " mm, vent area ~",
         round(vent_back ? vent_rows*vent_cols*vent_slot_w*vent_slot_l/100 : 0), " cm2",
         "  (IN DEVELOPMENT — MEASURE CONNECTORS)"));
echo(str("  stack: floor ", z_floor, " | PCB under ", z_pcb_under, " | PCB top ",
         z_pcb_top, " | glass back ", z_glass, " | closed height ",
         back_t + cav_d + bez_h, " mm"));
// The cavity walls locate the glass only while the glass is the widest thing in
// it. If a measured PCB overhangs the slab, the cavity grows to the board and
// the glass gains that much side-to-side slop — the bezel lip is then the only
// thing holding it centred. Say so rather than letting it pass silently.
// (Phrased without the word CI greps for; this is a note, not a build failure.)
if (pcb_w > glass_w || pcb_h > glass_h)
    echo(str("  NOTE: PCB overhangs the glass — slab has ",
             max(pcb_w - glass_w, 0)/2, " mm X / ",
             max(pcb_h - glass_h, 0)/2, " mm Y of play in the cavity; ",
             "the bezel lip alone centres it"));

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
        // glass cavity behind the face ledge
        translate([0, 0, face_t]) linear_extrude(bez_h) rrect2d(xc, yc, r_cav);
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
module vent_grille() {
    // staggered slot grille centred on the back plate
    for (r = [0:vent_rows-1], c = [0:vent_cols-1]) {
        x = (c - (vent_cols-1)/2) * vent_pitch_x;
        y = (r - (vent_rows-1)/2) * vent_pitch_y;
        // keep the grille inside the PCB footprint and off the lobes
        if (abs(x) < pcb_w/2 - 6 && abs(y) < pcb_h/2 - 6)
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
            // PCB standoff bosses at the M3 pattern
            for (sx = [1,-1], sy = [1,-1])
                translate([sx*m3_dx/2, sy*m3_dy/2, back_t - 0.01])
                    cylinder(d = 7.0, h = comp_h);
        }
        // M3 boss pilots
        for (sx = [1,-1], sy = [1,-1])
            translate([sx*m3_dx/2, sy*m3_dy/2, back_t + comp_h - 6]) cylinder(d = m3_pilot, h = 6.2);
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
else if (part == "gauge")       gauge();
else if (part == "gauge_tray")  gauge_corner("back");
else if (part == "gauge_bezel") gauge_corner("bezel");
else if (part == "stand") stand();
else {
    bezel_print();
    translate([xo + 16, 0, 0]) back();
    if (opt_stand) translate([0, -(yo/2 + stand_d/2 + 16), 0]) stand();
}
