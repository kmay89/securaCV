// ============================================================================
//  canary_panel_lib.scad — THE PANEL REGISTRY
//
//  One record per real display module. Every consumer reads from here: the
//  case that encloses it, the fit gates that prove the case fits it, and the
//  assembly/exploded views that show someone how to put it together.
//
//  WHY THIS EXISTS — the rot it is built to prevent
//  ------------------------------------------------
//  Before this file, a panel was described in three places: the case's
//  variable block owned the numbers, the fit-check file built its own stepped
//  probe out of them, and the section view built a third. Three descriptions
//  of one object, and the only thing keeping them equal was that one person
//  edited all three on the same afternoon. That is not a property, it is a
//  habit, and habits rot.
//
//  So: the numbers live HERE, once. canary_s3_lcd7.scad reads them into its
//  own names (so nothing downstream of it changes), the gates probe the mock
//  built from the same record, and the assembly view draws that same mock.
//  A wrong number is now wrong everywhere at once — which sounds worse and is
//  much better, because "wrong everywhere" is what a gate can catch and
//  "wrong in two of three places" is what ships.
//
//  ADDING A BOARD
//  --------------
//  Copy a record, change the fields, give it an id, and add the id to
//  panel_variant's enum in the case file. Nothing else. The case, the gates
//  and the views all follow. Fields are documented at PANEL FIELDS below.
//
//  ⚠️  DO NOT INVENT DIMENSIONS. A record is a claim that someone measured a
//  real board. `status` says how strong that claim is:
//      "measured"   — calipers on hardware, or a drawing confirmed against
//                     hardware. Only these may drive a PRINTABLE part.
//      "drawing"    — off a vendor drawing, not yet confirmed on metal.
//                     Usable, but every consumer should say so.
//      "unmeasured" — a placeholder. panel_assert_printable() REFUSES it.
//  A board you have not measured is not a smaller version of a board you
//  have; it is an unknown, and the registry's job is to keep that visible
//  rather than let a plausible-looking number become a case that does not fit.
//
//  RESOLUTION vs MECHANICS. `res` (pixels) and `aa` (active area, mm) are
//  separate fields on purpose. Two boards can share an outline and differ in
//  panel — that is exactly the "same case, two resolutions" situation — and
//  they can also share a resolution across different outlines. Neither
//  implies the other. The case geometry reads `aa`; only firmware and the
//  docs care about `res`.
//
//  ORIENTATION. Every field is stated in the MOUNTING pose: +X = width,
//  +Y = up, +Z = toward the glass, viewed from the FRONT. A board that mounts
//  half a turn from another is its own record with its own signs — NOT a
//  multiplier applied at the point of use. That multiplier is how the -7B
//  used to be handled, and it meant every consumer had to remember to apply
//  it. `flip` below records the relationship for documentation; the numbers
//  in the record are already in the pose.
// ============================================================================

// ── PANEL FIELDS ────────────────────────────────────────────────────────────
// Records are flat lists; these are the indices. Named accessors are below —
// prefer those, so a field can be inserted without touching every caller.
P_ID     = 0;   // string, matches panel_variant
P_LABEL  = 1;   // human name, appears in echoes and on generated docs
P_STATUS = 2;   // "measured" | "drawing" | "unmeasured"
P_GLASS  = 3;   // [w, h, t, r, edge_t]  — slab, corner radius, bare-glass border
P_AA     = 4;   // [w, h, dy]            — active area and its offset from glass center
P_RES    = 5;   // [px_w, px_h]          — pixels. NOT a mechanical field.
P_CORE   = 6;   // [w, h, dy]            — the LCD module can behind the glass
P_PCB    = 7;   // [w, h, t]             — carrier board outline
P_STACK  = 8;   // [standoff, comp_h, standoff_len] — glass back → PCB, tallest
                //                          part behind it, the panel's own studs
P_M3     = 9;   // [dx, dy, ox, oy]      — mount hole SPAN and pattern offset
P_PORTS  = 10;  // list of port records, see PORT FIELDS
P_FLIP   = 11;  // id of the board this is a half-turn from, or "" — doc only
P_SOC    = 12;  // [u, v, w, h] — the radio module's SHIELD CAN, in back-plate
                // coords (+u = back-view right, +v = back-view up), or [] if
                // the board's module has not been located. This is the ONLY
                // field that describes something nobody plugs into: it exists
                // because the module's shield is where the FCC/IC grant
                // numbers are printed, and a case that seals them in is a case
                // whose compliance marking has to be reprinted on a label
                // somewhere else. A window over the can lets the module's own
                // marking BE the marking. Size the can, not the module: the
                // PCB-antenna end of a WROOM carries no text.

// ── PORT FIELDS ─────────────────────────────────────────────────────────────
// A port is where a human plugs something in, so the fields are the ones a
// person needs: which face, where along it, how far off the glass plane, and
// how big the CONNECTOR BODY is (not the opening — the case derives that).
PT_NAME = 0;   // "USB-C", "microSD", "BOOT", …
PT_FACE = 1;   // "bottom" | "top" | "left" | "right" | "back"
PT_U    = 2;   // position along that face, + = toward +x (or +y on side faces)
PT_V    = 3;   // distance from the GLASS PLANE toward the back
PT_W    = 4;   // connector body width  (along the face)
PT_H    = 5;   // connector body height (across the face)
PT_D    = 6;   // how far it projects out of the board edge (0 = flush/internal)
PT_NOTE = 7;   // what it is for — this is what an assembly guide prints

// ── THE REGISTRY ────────────────────────────────────────────────────────────
// Waveshare ESP32-S3-Touch-LCD-7. Numbers carried over from the case file,
// where they were established: glass and AA off the vendor drawing, the module
// can proved self-consistent by the drawing's own chain (19.76 + 126.20 +
// 19.76 = 165.72 exactly), glass_r and glass_edge_t off calipers and a screen
// comparator, aa_dy's SIGN off calipers after the drawing value was found to
// be stated in a different pose.
function panel_db() = [
  [ "lcd7", "Waveshare ESP32-S3-Touch-LCD-7", "measured",
    [192.96, 110.76, 4.0, 8.2, 1.2],      // glass: w h t r edge_t
    [154.88, 86.72, 1.02],                // active area + offset
    [800, 480],                           // resolution
    [165.72, 97.60, -1.435],              // module can
    [165.72, 97.60, 1.6],                 // pcb outline + thickness
    [5.0, 11.0, 6.9],                     // standoff, comp_h, stud length
    [126.20, 65.65, 1.5, 0.9],            // m3 span + pattern offset
    [ ["USB-C",   "bottom", 0.0,   11.0, 9.0, 3.2, 1.4, "power and flashing"],
      ["microSD", "back",   35.65, -26.0, 15.0, 11.0, 0.0, "card slides toward -y (down, out of the socket)"],
      ["BOOT",    "top",   -12.5,  8.0,  4.0, 3.0, 0.0, "hold at power-up to flash"],
      ["RESET",   "top",    12.5,  8.0,  4.0, 3.0, 0.0, "restart"] ],
    "",
    // ⚠️ SCALED OFF THE VENDOR DRAWING, NOT MEASURED — the one soft number in
    // an otherwise measured record, and the case says so out loud in its echo.
    // The drawing dimensions the outline (165.72 x 97.60) and the M3 chain
    // (19.76 + 126.20 + 19.76), so the frame it establishes is exact; the
    // MODULE is not dimensioned on it, so its center was scaled off the
    // artwork against that frame and then turned a half turn, because the
    // drawing is posed component-side-up with the microSD at top-left and this
    // project's back view is the other way round (microSD bottom-right, which
    // is what the microSD port record above says). Expect ±2 mm.
    //   TO MEASURE IT: with the board out of the case, caliper from the PCB's
    //   right and top edges to the shield can's near corners, then subtract
    //   half the outline (82.86, 48.80) to get u, v. Two numbers, five minutes,
    //   and they retire this comment.
    // 18.0 x 19.2 is the WROOM-1 can — the module is 18.0 x 25.5 overall, but
    // the extra 6.3 is the PCB antenna, which carries no marking and must not
    // be windowed (a hole over the antenna is a hole over the one part of the
    // module that wants dielectric, not air).
    [44.5, 36.0, 18.0, 19.2] ],

];
// ════════════════════════════════════════════════════════════════════════════
//  THE -7B IS DELIBERATELY ABSENT. Read this before adding it back.
// ════════════════════════════════════════════════════════════════════════════
// There WAS a second record here, id "lcd7b", claiming a 1024x600 panel with
// every component a half turn round. It was removed, and removing it is the
// honest state: this file's own rule is that a record is a CLAIM SOMEBODY
// MEASURED ONE, and nobody had. It was reverse-engineered from a drawing plus
// the sign flips an older `pv` multiplier used to apply at each point of use.
//
// It was not inert while it sat here. `panel_variant` is a Customizer dropdown
// and a user-facing catalog axis, so the row was SELECTABLE — and selecting it
// could not produce geometry. It hard-asserted every time, then poisoned every
// later render in that session until the knob was found and put back, which
// reads to the operator as "the model broke when I changed something else
// entirely". A choice that can only fail is worse than no choice.
//
// The two defects it carried, kept because they are what has to be resolved
// before any -7B record is trustworthy:
//
//   1. AN INCOMPLETE FLIP. m3 and microSD flipped sign; the can offset
//      (core_dy) and active-area offset (aa_dy) did not. That is internally
//      inconsistent — a module seated a half turn round has its can and its
//      active area on the other side of the glass center too. Completing the
//      flip is one line, and it immediately trips the ledge assert, because
//      ledge_top (6.0) exceeds the 5.145 mm of border a flipped can leaves at
//      the top. So EITHER the offsets genuinely do not flip, OR they do and
//      THE CASE DOES NOT FIT A -7B AT ALL: the asymmetric ledge was sized for
//      the native pose only. Nobody has held the two boards together.
//
//   2. THE PORT WALL. The record put USB-C on the TOP face while the case cuts
//      its only cable opening in the BOTTOM wall — a case with no hole where
//      its connector is. Both defects are the same shape: the -7B was once
//      handled by flipping a couple of signs at the point of use, and
//      everything the flip did not reach stayed wrong and invisible until the
//      port map became data.
//
// TO ADD IT BACK, measure a board — glass, can, hole span, active area, and
// every connector's FACE and offset — and add a record. The case, the gates
// and the assembly views pick it up with no other edit, and `panel_variant`'s
// enum in canary_s3_lcd7.scad gains the id. Do not re-derive one by flipping
// signs; that is exactly what produced the version that had to be deleted.
//
// Naming, separately and still open: the physical 800x480 board in hand may
// itself be the "-7B" in Waveshare's scheme — the owner distinguishes theirs
// by having no microphone. The MEASUREMENTS in the lcd7 record are validated
// by a print that fit; only the NAME is uncertain, so nothing about what gets
// printed depends on settling it.

// ── LOOKUP ──────────────────────────────────────────────────────────────────
function panel_ids() = [for (p = panel_db()) p[P_ID]];
function panel(id) =
    let (hit = [for (p = panel_db()) if (p[P_ID] == id) p])
    assert(len(hit) == 1,
           str("panel: no record for \"", id, "\" — known: ", panel_ids()))
    hit[0];

// A PLACEHOLDER may never drive something you will hold. "drawing" passes —
// a vendor drawing is evidence, just weaker than calipers, and refusing it
// would block boards this project legitimately supports without owning one.
// "unmeasured" is refused outright, because that status means the numbers are
// a guess and a guess that reaches a printer becomes a case that does not fit.
// Consumers should still SAY which they got: pnl_status() is echoed by the
// case for exactly that reason.
function panel_assert_printable(p) =
    assert(p[P_STATUS] == "measured" || p[P_STATUS] == "drawing",
           str("panel: \"", p[P_ID], "\" is status \"", p[P_STATUS],
               "\" — a placeholder record may not drive a printable part. ",
               "Measure the board, or print for a board you have."))
    true;

// ── ACCESSORS ───────────────────────────────────────────────────────────────
// Everything downstream goes through these, so a field can move without a
// hunt through consumers.
function pnl_id(p)        = p[P_ID];
function pnl_label(p)     = p[P_LABEL];
function pnl_status(p)    = p[P_STATUS];
function pnl_glass_w(p)   = p[P_GLASS][0];
function pnl_glass_h(p)   = p[P_GLASS][1];
function pnl_glass_t(p)   = p[P_GLASS][2];
function pnl_glass_r(p)   = p[P_GLASS][3];
function pnl_edge_t(p)    = p[P_GLASS][4];
function pnl_aa_w(p)      = p[P_AA][0];
function pnl_aa_h(p)      = p[P_AA][1];
function pnl_aa_dy(p)     = p[P_AA][2];
function pnl_res_w(p)     = p[P_RES][0];
function pnl_res_h(p)     = p[P_RES][1];
function pnl_core_w(p)    = p[P_CORE][0];
function pnl_core_h(p)    = p[P_CORE][1];
function pnl_core_dy(p)   = p[P_CORE][2];
function pnl_pcb_w(p)     = p[P_PCB][0];
function pnl_pcb_h(p)     = p[P_PCB][1];
function pnl_pcb_t(p)     = p[P_PCB][2];
function pnl_standoff(p)  = p[P_STACK][0];
function pnl_comp_h(p)    = p[P_STACK][1];
function pnl_stud_len(p)  = p[P_STACK][2];
function pnl_m3_dx(p)     = p[P_M3][0];
function pnl_m3_dy(p)     = p[P_M3][1];
function pnl_m3_ox(p)     = p[P_M3][2];
function pnl_m3_oy(p)     = p[P_M3][3];
function pnl_ports(p)     = p[P_PORTS];
function pnl_flip(p)      = p[P_FLIP];
// The radio can. pnl_has_soc() is the guard every consumer must ask first:
// a record written before this field existed returns undef, and an undef that
// reaches a translate() becomes a NaN and then a part with a hole in the
// wrong place — the exact failure the port accessors assert against.
function pnl_has_soc(p)   = is_list(p[P_SOC]) && len(p[P_SOC]) == 4;
function pnl_soc_u(p)     = p[P_SOC][0];
function pnl_soc_v(p)     = p[P_SOC][1];
function pnl_soc_w(p)     = p[P_SOC][2];
function pnl_soc_h(p)     = p[P_SOC][3];

// One port by name. Asserts rather than returning undef, because a silent
// undef becomes a NaN four expressions later and reports as a geometry bug.
function pnl_port(p, name) =
    let (hit = [for (q = pnl_ports(p)) if (q[PT_NAME] == name) q])
    assert(len(hit) == 1,
           str("panel ", pnl_id(p), ": no port named \"", name, "\" — has: ",
               [for (q = pnl_ports(p)) q[PT_NAME]]))
    hit[0];
function pnl_port_face(p, n) = pnl_port(p, n)[PT_FACE];
function pnl_port_u(p, n)    = pnl_port(p, n)[PT_U];
function pnl_port_v(p, n)    = pnl_port(p, n)[PT_V];
function pnl_port_w(p, n)    = pnl_port(p, n)[PT_W];
function pnl_port_h(p, n)    = pnl_port(p, n)[PT_H];
function pnl_port_d(p, n)    = pnl_port(p, n)[PT_D];
function pnl_port_note(p, n) = pnl_port(p, n)[PT_NOTE];
function pnl_has_port(p, n)  = len([for (q = pnl_ports(p))
                                    if (q[PT_NAME] == n) 1]) > 0;

// Pixel geometry, derived from res and aa so it cannot disagree with either.
//
// TWO NUMBERS, DELIBERATELY. The first version of this returned one ppi
// computed from the width, and that single number quietly concealed a 7.2%
// difference between the axes on the very first record: 0.1936 mm/px across,
// 0.1807 down. Reporting "131 ppi" for a panel whose vertical density is 141
// is not a rounding, it is an average of two things that are not the same.
//
// It is also a free cross-check on the ACTIVE AREA. Most panels are close to
// square-pixel, so a large skew is either a real property of a cheap TFT (the
// 7" 800x480 class is genuinely non-square) or a sign that one of aa_w/aa_h
// was measured across the wrong rectangle. This project has already made that
// exact error once, recording a caliper reading of the ACTIVE AREA as the
// module can — so a number that makes the mistake visible is worth having.
// Not an assert: non-square really happens, and a gate that cries wolf on a
// legitimate panel would be turned off within a week.
function pnl_pitch_x(p) = pnl_aa_w(p) / pnl_res_w(p);   // mm per pixel
function pnl_pitch_y(p) = pnl_aa_h(p) / pnl_res_h(p);
function pnl_ppi_x(p)   = 25.4 / pnl_pitch_x(p);
function pnl_ppi_y(p)   = 25.4 / pnl_pitch_y(p);
// How far from square the pixels are, as a fraction. 0 = square.
function pnl_pixel_skew(p) =
    abs(pnl_pitch_x(p) / pnl_pitch_y(p) - 1);

// The four mount-hole centers, in the mounting pose.
function pnl_m3_holes(p) =
    [for (sx = [1,-1], sy = [1,-1])
        [pnl_m3_ox(p) + sx*pnl_m3_dx(p)/2, pnl_m3_oy(p) + sy*pnl_m3_dy(p)/2]];

// Total depth from glass front to the back of whatever sticks out furthest.
function pnl_depth(p) = pnl_glass_t(p) + pnl_standoff(p) + pnl_pcb_t(p)
                        + max(pnl_comp_h(p), pnl_stud_len(p));

// ── SANITY, CHECKED AT LOAD ─────────────────────────────────────────────────
// Cheap invariants that catch a typo in a new record before it becomes a
// mystery. Deliberately about RELATIONSHIPS, not values: a value assert would
// just be the number written twice.
// NOTE FOR EDITORS: OpenSCAD has no adjacent-string-literal concatenation.
// "one " "two" is a syntax error here, not "one two" — join with str(a, b).
// It cost a parse error on this very function, and the message went to the
// OUTPUT file rather than stderr, so a grep for ERROR on stderr saw nothing.
function panel_check(p) =
    assert(pnl_core_w(p) <= pnl_glass_w(p) && pnl_core_h(p) <= pnl_glass_h(p),
           str("panel ", pnl_id(p), ": the module can is bigger than the glass ",
               "it sits behind — a can cannot exceed its own cover"))
    assert(pnl_aa_w(p) <= pnl_core_w(p) && pnl_aa_h(p) <= pnl_core_h(p),
           str("panel ", pnl_id(p), ": the active area is bigger than the ",
               "module can — a screen cannot be larger than the thing lighting ",
               "it. This is the reading that caught four caliper measurements ",
               "taken across the ACTIVE AREA and recorded as the can."))
    assert(pnl_edge_t(p) > 0 && pnl_edge_t(p) < pnl_glass_t(p),
           str("panel ", pnl_id(p), ": the bare-glass border must be thinner ",
               "than the full module stack and non-zero"))
    assert(abs(pnl_m3_ox(p)) + pnl_m3_dx(p)/2 <= pnl_pcb_w(p)/2
           && abs(pnl_m3_oy(p)) + pnl_m3_dy(p)/2 <= pnl_pcb_h(p)/2,
           str("panel ", pnl_id(p), ": a mount hole falls outside the board ",
               "outline — check the SPAN/offset signs, not the outline"))
    true;

// ── THE MOCK ────────────────────────────────────────────────────────────────
// NOT PRINTABLE, and never exported as a part. This is the object the case
// encloses, drawn so it can be seen and so the gates can probe it.
//
// Origin is the GLASS CENTER, front face at z = 0, +z toward the BACK. That
// is the panel's own frame; a consumer places it wherever its own datum is.
// `blow` explodes the stack along +z for an assembly figure: 0 is assembled.

// THE PHYSICAL SLAB — solid only, no color, no decoration, nothing that
// pokes outside the real envelope. This is the module the FIT GATES probe
// with, which is why it is separated from the pretty version below: a gate
// intersects this against the case and asks whether anything collides, so a
// decorative sliver sitting 0.05 mm proud of the glass would register as the
// case crushing the panel. The two used to be one module and the fit-check
// file built its own copy of this shape rather than risk that.
//
// STEPPED, and that is the whole point: a thin bare-glass border around a
// thicker module can. Modeling it as a uniform slab is what once floated an
// adhesive ledge 3.2 mm clear of anything it was supposed to bond to.
module pnl_slab(p, blow = 0) {
    linear_extrude(pnl_edge_t(p))
        rrect2d_p(pnl_glass_w(p), pnl_glass_h(p), pnl_glass_r(p));
    translate([0, pnl_core_dy(p), pnl_edge_t(p) + blow*0.5])
        linear_extrude(pnl_glass_t(p) - pnl_edge_t(p))
            rrect2d_p(pnl_core_w(p), pnl_core_h(p), 2);
}

module pnl_glass(p, blow = 0) {
    color([0.10, 0.12, 0.16]) pnl_slab(p, blow);
    // the lit area, so an assembly figure shows which way up the image is.
    // VIEW ONLY — it stands proud of the glass on purpose so it reads, which
    // is exactly why no gate may probe with this module.
    color([0.15, 0.45, 0.75])
        translate([0, pnl_aa_dy(p), -0.05])
            linear_extrude(0.06)
                square([pnl_aa_w(p), pnl_aa_h(p)], center = true);
}

module pnl_board(p, blow = 0) {
    z = pnl_glass_t(p) + pnl_standoff(p) + blow;
    color([0.05, 0.35, 0.20])
        translate([0, 0, z]) linear_extrude(pnl_pcb_t(p))
            rrect2d_p(pnl_pcb_w(p), pnl_pcb_h(p), 3);
    // the panel's own white M3 studs — the case bolts into THESE, it does not
    // clamp the glass, and drawing them is how that reads at a glance
    color([0.92, 0.92, 0.90])
        for (h = pnl_m3_holes(p))
            translate([h[0], h[1], z + pnl_pcb_t(p)])
                cylinder(d = 6, h = pnl_stud_len(p), $fn = 24);
    // one block standing in for everything tall behind the board
    color([0.25, 0.25, 0.28])
        translate([0, 0, z + pnl_pcb_t(p)])
            linear_extrude(pnl_comp_h(p))
                square([pnl_pcb_w(p)*0.55, pnl_pcb_h(p)*0.45], center = true);
}

// Connector bodies where the record says they are. These are what an
// assembly guide points at, and what tells you at a glance which edge the
// cable leaves from — the question a flat drawing answers badly.
module pnl_ports_mock(p, blow = 0) {
    for (pt = pnl_ports(p)) {
        f = pt[PT_FACE];
        gw = pnl_glass_w(p)/2;  gh = pnl_glass_h(p)/2;
        pos = f == "bottom" ? [pt[PT_U], -gh, pt[PT_V]]
            : f == "top"    ? [pt[PT_U],  gh, pt[PT_V]]
            : f == "left"   ? [-gw, pt[PT_U], pt[PT_V]]
            : f == "right"  ? [ gw, pt[PT_U], pt[PT_V]]
            :                 [pt[PT_U], pt[PT_V], pnl_depth(p)];   // "back"
        color([0.75, 0.65, 0.20])
            translate(pos + [0, 0, blow])
                cube([pt[PT_W], pt[PT_H], pt[PT_H]], center = true);
    }
}

module panel_mock(p, blow = 0) {
    pnl_glass(p, blow);
    pnl_board(p, blow);
    pnl_ports_mock(p, blow);
}

// Local rounded rect, so this file does not depend on its consumers. Named
// apart from the case's rrect2d deliberately: two files owning one name is
// how you get a silent redefinition when both are included.
module rrect2d_p(w, h, r) {
    hull() for (sx = [1,-1], sy = [1,-1])
        translate([sx*(w/2 - r), sy*(h/2 - r)]) circle(r = r, $fn = 48);
}

// ── WHAT A DOC GENERATOR READS ──────────────────────────────────────────────
// One line per panel, so a guide, a table or a page can be generated instead
// of transcribed. Transcription is the rot this file exists to end.
function panel_summary(p) =
    str(pnl_label(p), " [", pnl_id(p), ", ", pnl_status(p), "] — glass ",
        pnl_glass_w(p), " x ", pnl_glass_h(p), " r", pnl_glass_r(p),
        ", active ", pnl_aa_w(p), " x ", pnl_aa_h(p),
        " (", pnl_res_w(p), " x ", pnl_res_h(p), " px, ",
        round(pnl_ppi_x(p)), " x ", round(pnl_ppi_y(p)), " ppi",
        pnl_pixel_skew(p) > 0.02
            ? str(" — NON-SQUARE pixels, ", round(pnl_pixel_skew(p)*1000)/10,
                  "%; expected on this panel class, but if it is a surprise, ",
                  "re-measure the active area")
            : "",
        "), board ", pnl_pcb_w(p), " x ", pnl_pcb_h(p),
        ", M3 span ", pnl_m3_dx(p), " x ", pnl_m3_dy(p),
        ", depth ", pnl_depth(p), " mm, ", len(pnl_ports(p)), " ports");
