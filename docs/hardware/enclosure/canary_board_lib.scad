// ============================================================================
//  Canary BOARD library — one registry for every board the catalog mounts
//
//  NOT A PRINTABLE PART. `use <canary_board_lib.scad>` from a case file.
//
//  canary_panel_lib.scad solved dimension rot for the 7" panel: measured
//  records with an evidence ladder, read through accessors, so a number
//  exists ONCE and its provenance travels with it. Every other board in the
//  catalog was retyped per file, and the retypes rotted exactly as the
//  panel registry predicts: the XIAO's real width (17.8 — pockets pinched
//  the 17.5 spec board) was discovered in canary_dock.scad and stayed
//  there; the Grove Vision AI V2's measured 40 x 20 replaced a wrong
//  25 x 25 in three files and missed the Hammond chassis; the seated-stack
//  height was measured at 6.5 in the doorbell while three siblings still
//  carry an unmeasured 11.5. This file is where those numbers live now.
//
//  EVIDENCE LADDER (same doctrine as the panel registry):
//    "measured"   — calipers on real hardware, the print history says whose;
//    "drawing"    — vendor mechanical drawing, no bench unit confirmed;
//    "spec"       — vendor datasheet prose (looser than a drawing);
//    "unmeasured" — a working guess that has SURVIVED PRINTS but never met
//                   calipers; safe the way a too-big cavity is safe.
//
//  Cases keep their Customizer knobs — a user measures THEIR board — but
//  knob defaults cite this registry, and a file whose fixed (non-knob)
//  arithmetic needs a board dimension reads it from here instead of
//  retyping it. Where a knob default deliberately keeps a validated value
//  that the registry has better evidence against (the Vision's 11.5 stack
//  height is print-validated as a roomier cavity), the file says so in a
//  comment beside the knob — the registry states the truth, the file
//  states the decision.
//
//  `use<>` does not run top-level statements: call board_selfcheck() from
//  an adopter (the fit coupon does).
// ============================================================================

// ---------------------------------------------------------------------------
//  The registry.  [id, along-USB length, width, pcb thickness, status, note]
//  "length" is the axis the USB/cable leaves on, matching how every case
//  file already orients its board knobs.
// ---------------------------------------------------------------------------
BRD_REGISTRY = [
    ["xiao",       21.0,  17.5, 1.2, "spec",
     "Seeed XIAO outline (ESP32-S3/C6 share it) — but see brd_xiao_w_measured(): a real board mics 17.8 and 17.5-spec pockets pinch it (canary_dock lesson)"],
    ["grove_v2",   40.0,  20.0, 1.0, "measured",
     "Grove Vision AI V2 — measured 1x2 form; the 25 x 25 it replaced was wrong in three files and is pinned dead by board_selfcheck()"],
    ["ov5647",     24.0,  25.0, 1.0, "spec",
     "OV5647 camera carrier, Pi-cam v1.3 form"],
    ["mr60",       44.0,  36.0, 1.0, "spec",
     "Seeed MR60 kit carrier (BHA2/FDA2 share the family) — XIAO C6 stacks on its back"],
    ["dk_c3",      39.0,  25.4, 1.0, "spec",
     "ESP32-C3-DevKitM-1 — the Vision's Grove-cabled host option"],
    ["ws147",      36.37, 20.32, 1.6, "drawing",
     "Waveshare *-LCD-1.47 family PCB (C3 / C6 / S3-stick share the outline); 1.6 thickness is the usual and the drawing does not call it out — MEASURE tags stay in the case files"],
    ["ws169",      29.83, 37.12, 1.6, "drawing",
     "Waveshare ESP32-S3-Touch-LCD-1.69 PCB (glass slab is larger — brd_ws169_glass_*)"],
    ["round_disp", 43.0,  43.0, 1.0, "measured",
     "Seeed Round Display disc, Ø43.0 measured (the watch station's bore)"],
];

// ---------------------------------------------------------------------------
//  Accessors
// ---------------------------------------------------------------------------
function _brd_find(id, i = 0) =
    i >= len(BRD_REGISTRY)
        ? assert(false, str("board registry: unknown board id \"", id, "\"")) undef
        : BRD_REGISTRY[i][0] == id ? BRD_REGISTRY[i] : _brd_find(id, i + 1);

function brd_l(id)      = _brd_find(id)[1];   // along the USB/cable axis
function brd_w(id)      = _brd_find(id)[2];
function brd_t(id)      = _brd_find(id)[3];
function brd_status(id) = _brd_find(id)[4];
function brd_note(id)   = _brd_find(id)[5];

// ---------------------------------------------------------------------------
//  Measured facts that refine a record without replacing it
// ---------------------------------------------------------------------------
// A real XIAO mics wider than its 17.5 spec — pockets sized to spec pinch
// the board (canary_dock: "was 17.5 — pockets pinched the real board").
// Cases whose board sits in CLIPS absorb the difference in clip_clear and
// keep the spec default; anything that pockets the board snugly uses this.
function brd_xiao_w_measured() = 17.8;

// XIAO seated in the Grove/MR60 socket: module underside -> XIAO underside.
// Measured 6.2 on the doorbell bench, carried as 6.5 with margin. The 11.5
// still in the Vision/Sense/Hammond knobs is UNMEASURED headroom — print-
// validated as a roomier cavity, never confirmed as a stack height.
function brd_stack_sock_measured()   = 6.5;
function brd_stack_sock_unmeasured() = 11.5;

// Waveshare 1.47 family brass corner pillars above the PCB back:
// the C3's are measured; the C6's 5.0 has never met calipers.
function brd_ws147_brass_c3() = 3.0;    // MEASURED (kmay89)
function brd_ws147_brass_c6() = 5.0;    // unmeasured — MEASURE tag lives in the C6 file

// The 1.69's bonded glass slab overhangs its PCB (~2 mm/side — the case's
// face lip captures the overhang; the board has no mounting holes)
function brd_ws169_glass_w() = 33.13;
function brd_ws169_glass_h() = 41.13;

// ---------------------------------------------------------------------------
//  Self-check — registry integrity + the pinned lessons. Call once from an
//  adopter (the fit coupon does).
// ---------------------------------------------------------------------------
module board_selfcheck() {
    for (i = [0 : len(BRD_REGISTRY) - 1]) {
        r = BRD_REGISTRY[i];
        assert(len(r) == 6, str("board registry: record ", i, " malformed"));
        assert(r[1] > 0 && r[2] > 0 && r[3] > 0,
               str("board registry: \"", r[0], "\" has a non-positive dimension"));
        s = r[4];
        assert(s == "measured" || s == "drawing" || s == "spec" || s == "unmeasured",
               str("board registry: \"", r[0], "\" has evidence \"", s,
                   "\" — not a rung of the ladder"));
        for (j = [0 : len(BRD_REGISTRY) - 1])
            assert(j == i || BRD_REGISTRY[j][0] != r[0],
                   str("board registry: duplicate id \"", r[0], "\""));
    }
    // the lessons stay dead: a regression here is a wrong case, not a style nit
    assert(brd_l("grove_v2") == 40.0 && brd_w("grove_v2") == 20.0,
           "board: the Grove Vision AI V2 is 40 x 20 measured — 25 x 25 was the bug");
    assert(brd_xiao_w_measured() > brd_w("xiao"),
           "board: the measured XIAO is WIDER than spec — that is the whole lesson");
    assert(brd_stack_sock_measured() < brd_stack_sock_unmeasured(),
           "board: the measured seated stack is shorter than the legacy guess");
    echo("canary_board_lib: self-check OK");
}
