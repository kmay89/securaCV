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
    ["heltec_v3",  51.0,  26.0, 1.2, "spec",
     "Heltec WiFi LoRa 32 V3 — the solar relay pod's radio board"],
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

// XIAO ESP32-S3 SENSE camera — the OV2640 module on the Sense expansion
// board, read off Seeed's own XIAO ESP32-S3 Sense model (GLB), "drawing"
// rung: PCB solid x -8.67..12.28 (the length axis), z -15.00..2.78 (the
// width — 17.78, the measured 17.8 again), top face y = 1.0; the camera
// module's top face at y = 13.71 over x 4.75..12.75, z -10.75..-2.75. So the
// lens top stands 12.7 above the PCB top face (the WAP's knob said 6.0 —
// half the real stack), the module is 8 x 8, and its center sits 6.95 along
// the LENGTH from the board center toward the antenna end, away from the
// USB (the WAP's window sat over the board center, 7 mm off the lens); it
// overhangs that end by ~0.5. Across the width the model reads ~0.6 toward
// one long edge; which edge needs a bench unit (the model's handedness is
// not stated), so it is carried as 0 and the WAP's window keeps 0.5 a side
// round the barrel — MEASURE before a print that depends on it. The WAP's
// window, disc seat and camera stack read these through its manifest.
function brd_xiao_sense_cam_h()  = 12.7;   // lens top above the PCB top face
function brd_xiao_sense_cam_dx() = -6.95;  // module center along the length, from the board center, + = away from the USB (it sits at the USB end)
function brd_xiao_sense_cam_dy() = -0.64;  // module center across the width, from the board center, + = the long edge on the LEFT with the USB toward you and the parts up (see above)
function brd_xiao_sense_cam_fp() = 8.0;    // module footprint, square side (the lens barrel's envelope)

// XIAO ESP32-S3 — the two tactile switches (B and R) that flank the USB-C
// on the component face, read off the vendor GLB (canary-local/boards/
// seeed_xiao_esp32s3.glb: the board 21.14 x 17.78 x 1.2, the switch bodies
// 2.6 x 1.6 x 0.63 on 0.1 of pad, the USB-C shell 7.3 long standing 1.5
// past the board's end). The switch centers sit ±5.93 across the width from
// the board's centerline and 1.61 along the length inboard of the USB end;
// the caps stand 0.75 over the PCB face. WHICH of the two is R is not in
// the model (no silkscreen) — a case that pokes them carries that side as
// its own MEASURE knob. The C3 shares the family outline; unmeasured there.
function brd_xiao_btn_dx()   = 5.93;   // switch center across the width, ± from the board's centerline
function brd_xiao_btn_dy()   = 1.61;   // switch center along the length, inboard of the USB end
function brd_xiao_btn_h()    = 0.75;   // the cap's top over the PCB face
function brd_xiao_btn_cap()  = [2.6, 1.6];   // the cap, [along the length, across the width]
function brd_xiao_usb_overhang() = 1.5;      // the USB-C shell past the board's end

// OV5647-62 CAMERA — the Grove Vision AI V2 kit's camera (Pi-cam v1.3 form,
// the "ov5647" row above), read off the vendor CAD: the "RPi cam Rev 1.3"
// posed inside boards/vendor/seeed_grove_vision_ai_v2.step.gz, "drawing"
// rung. In the carrier's frame (its center the origin, +Y toward the edge
// AWAY from the ribbon connector): the four Ø2.1 holes sit on a 21 x 12.3
// grid (12.5 nominal, the Pi-cam pattern) centered 4.05 ABOVE the carrier's
// center; the lens is centered on the carrier across X and 1.7 BELOW its
// center, beside the lower hole row; its Ø7.0 round barrel tops out 5.0
// over the PCB face (Seeed's datasheet: a 25 x 24 x 7±0.2 module), on an
// 8.8 square holder 3.65 tall. Every case that hung this camera on posts at
// the carrier's center with the lens 2.5 above it put the real lens ~8 mm
// off its hole — the doorbell's and the Vision's v0.6 found it. Their
// manifests own these knobs, so the two cases cannot drift apart again.
function brd_ov5647_hole_x()   = 21.0;   // mounting-hole grid across the carrier (X)
function brd_ov5647_hole_y()   = 12.5;   // mounting-hole grid along it (Y) — 12.3 in the model, the 12.5 nominal carried
function brd_ov5647_grid_dy()  = 4.05;   // hole-grid center ABOVE the carrier's center (away from the ribbon edge)
function brd_ov5647_lens_dx()  = 0.0;    // lens center across the carrier, from its center
function brd_ov5647_lens_dy()  = -1.7;   // lens center along it, from its center: BELOW, toward the ribbon edge
function brd_ov5647_lens_h()   = 5.0;    // lens barrel top above the PCB face
function brd_ov5647_holder_h() = 3.65;   // the square holder's top above the PCB face
function brd_ov5647_holder_sq() = 8.8;   // the holder's square side
function brd_ov5647_barrel_d() = 7.0;    // the round lens barrel's diameter

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
    // the OV5647's lens and hole grid sit ON its carrier — and the lens sits
    // below the carrier's center while the grid sits above it (the v0.6 bug)
    assert(abs(brd_ov5647_lens_dx()) + brd_ov5647_holder_sq()/2 < brd_w("ov5647")/2
           && abs(brd_ov5647_lens_dy()) + brd_ov5647_holder_sq()/2 < brd_l("ov5647")/2
           && brd_ov5647_hole_x()/2 < brd_w("ov5647")/2
           && brd_ov5647_grid_dy() + brd_ov5647_hole_y()/2 < brd_l("ov5647")/2,
           "board: the OV5647's lens holder or hole grid runs off its carrier");
    assert(brd_ov5647_lens_dy() < 0 && brd_ov5647_grid_dy() > 0,
           "board: the OV5647's lens is BELOW the carrier's center and its hole grid ABOVE — the case bug v0.6 fixed");
    echo("canary_board_lib: self-check OK");
}
