// ============================================================================
//  Canary — BENCH BRING-UP FIXTURE  ⚠️ IN DEVELOPMENT (v0.4-dev)
//  Companion to docs/hardware/bench_bringup.md ("start here"): instead of a
//  desk of loose alligator clips, a labeled plate holds every bring-up part
//  in its station while you wire and test:
//
//    XIAO    — rail/clip cradle for the XIAO ESP32-S3 (Sense), USB free
//    BZ1     — piezo buzzer ring pocket
//    DLED1   — WS2812 breakout pocket
//    SW1     — 6 mm tactile button pocket
//    SW2+MAG — reed-switch channel with a SLIDING magnet carriage on a rail:
//              slide the magnet away to fire tamper events repeatably
//
//  Wire channels route between stations (build-plan §5 pin map); zip-tie
//  slots dress the leads. Print flat, no supports.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//
//  v0.2-dev (2026-08-23): canary_*_lib adoption, and the clips re-engineered
//  by it. This file carried clip_t 1.5 / clip_hook 0.8 — the very numbers the
//  WAP's comment warns crack vertical-print PETG (~10 % insertion strain) —
//  on a jig whose whole point is swapping boards over and over, which is the
//  REPEATED-flex duty class (snap_budget_cycle, 2 %). Now: the WAP's proven
//  1.0/0.5 beam on a 6 mm standoff — the taller root lengthens the beam to
//  7.2 mm, landing insertion strain at 1.45 % under the cycle budget, with
//  the full 0.5 mm hook engagement kept. The lift also clears lead-routing
//  room under the XIAO, which a wiring jig wanted anyway. snap_boardclip's
//  assert holds the numbers honest on every render.
//
//  v0.3-dev (2026-10-23): the magnet could not reach the reed. When the reed
//  channel was opened into a slot its axis rose to 2.9, so the 2.8 glass
//  stands 2.0 above the groove floor (its top at z 4.2 resting, the bore's
//  crown at 4.4) — but the slider's relief stayed 1.2 tall (top at z 3.4),
//  sized for the 1.1 the glass stood when it was buried. Parked at the reed
//  end the slider met 12 mm³ of glass (14 with the glass up at the crown),
//  and the magnet pocket's floor over the relief was 0.1 thick. The relief is now 2.4 (the groove's 1.8 + the
//  crown's 0.3 + tol_press + 0.2 clear) and the pocket floor a 0.8 web over
//  it, 4.5 deep for the 3 mm magnet. And the XIAO label moved above the
//  board: under it, the wire channel to the reed ran through its "O".
//
//  v0.4-dev (2026-10-23, the cross-family review): NOTHING LOCATED THE XIAO
//  ALONG ITS RAILS. The clips hold X and Z only, so every USB plug-in shoved
//  the board along the rails — the space past each end at the PCB's height
//  was empty for 15 mm, and at 13.5 the board leaves its clips: the WAP's
//  AUD-2 and the J-box's AUD-20 defect, on the jig whose USB is plugged most.
//  End stops now: a core_min_wall() column off each end of each long edge,
//  0.3 off the drawn end, up to the PCB's top face, outside a spec-max Type-C
//  overmold across the axis (either end may be the USB end) and so clear of
//  a Sense's microSD card and camera, which stand over the PCB's top; each
//  column's root runs back into its rail's end at rail height, so a plug's
//  push bends the rail, not a 1.2 mm column. And the clip gate was run for
//  the 17.5 spec board (no `over`): the measured 17.8 bends the beam 0.65,
//  not 0.5 — 1.88 % against the 2 % cycle budget, not the 1.45 % above.
// ============================================================================

use <canary_core_lib.scad>   // rrect2d — the catalog's shared helpers
use <canary_snap_lib.scad>   // the cantilever board clip + the CYCLE strain budget
use <canary_board_lib.scad>  // board registry — the XIAO numbers the knobs cite
use <canary_port_lib.scad>   // port_usbc_overmold_w — the board's end stops stand outside a plug's envelope

/* [What to render] */
part = "all";        // ["plate","slider","all"]

/* [Board] — XIAO ESP32-S3 */
board_l = 21.0;      // board length — the 21 spec, brd_l("xiao")
board_w = 17.5;      // board width — the 17.5 spec, brd_w("xiao"); the clips' clip_clear
                     // absorbs the measured 17.8 (brd_xiao_w_measured())
board_h = 1.2;       // PCB thickness — the 1.2 spec, brd_t("xiao")
standoff_h = 6.0;    // clip-beam root height: 6.0 puts the beam at 7.2 mm and the
                     // repeated-insertion strain at 1.88 % with the measured 17.8
                     // board (< snap_budget_cycle; 1.45 % for the 17.5 spec);
                     // it also leaves lead room under the board (was 3.0 — that
                     // root made even the WAP's 1.0/0.5 clip fail the cycle budget)

/* [Stations] — MEASURE your parts */
bz_d   = 12.5;       // buzzer body diameter (CPT-9019 ~ 9 x 9; ring fits to 12.5)
bz_dep = 2.0;        // buzzer pocket depth into the plate face
led_w  = 10.5;       // WS2812 breakout square
led_dep = 1.6;       // WS2812 breakout pocket depth into the plate face
btn_w  = 6.4;        // 6 mm tact switch body
btn_dep = 2.0;       // tact switch pocket depth into the plate face
reed_l = 15.0;       // reed switch glass length
reed_d = 2.8;        // reed glass diameter — the channel bores it + 2 x tol_press and its crown stands 0.3 + tol_press above the face, so the top is open; the slider's relief channel is this + 1.0 wide
mag_d  = 6.0;        // tamper magnet (rides the slider)
mag_h  = 3.0;        // tamper magnet height — not read by the geometry: the slider's pocket is open at the knob top, its floor 3.2 above the foot's underside and 4.5 below the knob top

/* [Plate] */
fx_w = 120.0;        // plate width (X); the corner holes sit 6 in from each edge
fx_h = 80.0;         // plate depth (Y); the corner holes sit 6 in from each edge
fx_t = 4.0;          // plate thickness — the pockets, channels and labels are cut down from its top face, and the XIAO rails and clips stand on it
chan_w = 3.0;        // wire channel width
label_depth = 0.5;   // station label deboss depth into the plate face
label_font = "Liberation Sans:style=Bold";   // the station labels' font (an OpenSCAD font name)

/* [Clips / tolerances] */
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;
                     // 1.0/0.5 — catalog standard, canary_snap_lib (this file shipped
                     // the 1.5/0.8 that cracks; the lib's assert now forbids it)
tol_slide = 0.20;  tol_press = 0.10;

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary bench fixture v0.4-dev — ", fx_w, "x", fx_h, "  (IN DEVELOPMENT)"));

// (rrect2d comes from canary_core_lib — the local copy is gone)
module lbl(x, y, s) { translate([x, y, fx_t - label_depth]) linear_extrude(label_depth + 0.1)
    text(s, size = 3.6, font = label_font, halign = "center", valign = "center"); }
// Cantilever snap clip at edge point (px,py); `ang` = outward normal. The
// drawing is canary_snap_lib's beam, and the budget is the CYCLE one: a jig's
// clips flex on every board swap, not a handful of times over a case's life,
// so they get the 2 % allowance — the assert that would have refused this
// file's original 1.5/0.8 numbers before they ever met PETG.
module edgeclip(px, py, ang) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, fx_t, fx_t + standoff_h + board_h,
                       clip_w, clip_t, clip_hook, clip_hook_h, clip_clear,
                       snap_budget_cycle(),
                       over = (brd_xiao_w_measured() - board_w)/2);   // v0.4: the gate reads the real board
}

// the board's end stops: a column's inboard edge across the board, outside a
// spec-max Type-C overmold on the connector's axis (v0.4)
stop_x0 = port_usbc_overmold_w()/2 + 0.2;
assert(board_w/2 - stop_x0 >= core_min_wall(), "the board's end stops would be thinner than a wall beside the overmold");
assert(stop_x0 >= board_w/2 - 3, "the end stops' roots no longer sit on the rails");

// station centers
x_xiao = -38;  y_xiao =  16;
x_bz   =  10;  y_bz   =  22;
x_led  =  34;  y_led  =  22;
x_btn  =  10;  y_btn  =  -2;
x_reed = -38;  y_reed = -24;

module plate() {
    union() {
        difference() {
            linear_extrude(fx_t) rrect2d(fx_w, fx_h, 5);
            // component pockets
            translate([x_bz, y_bz, fx_t - bz_dep]) cylinder(d = bz_d + 2*tol_slide, h = bz_dep + 0.1);
            translate([x_led, y_led, fx_t - led_dep]) linear_extrude(led_dep + 0.1)
                rrect2d(led_w + 2*tol_slide, led_w + 2*tol_slide, 1);
            translate([x_btn, y_btn, fx_t - btn_dep]) linear_extrude(btn_dep + 0.1)
                rrect2d(btn_w + 2*tol_slide, btn_w + 2*tol_slide, 0.5);
            // reed channel — an OPEN slot (its axis reed_d/2 - 0.3 below the face, so the
            // bore's crown stands 0.3 + tol_press above it, the top is open and the glass
            // reed presses in; buried 0.7 deep it was a tunnel)
            translate([x_reed, y_reed, fx_t - reed_d/2 + 0.3]) rotate([0, 90, 0])
                translate([0, 0, -reed_l/2 - 4]) cylinder(d = reed_d + 2*tol_press, h = reed_l + 8);
            // magnet slider rail: dovetail-ish groove running toward the reed
            translate([x_reed + 14, y_reed, fx_t - 1.8]) linear_extrude(1.9)
                rrect2d(44 + 2*tol_slide, 8 + 2*tol_slide, 1);
            // wire channels between stations + to the XIAO
            for (seg = [[x_xiao + 14, y_xiao, x_bz - 8, y_bz],
                        [x_bz + 7, y_bz, x_led - 7, y_led],
                        [x_xiao + 14, y_xiao - 6, x_btn - 5, y_btn],
                        [x_xiao, y_xiao - 14, x_reed, y_reed + 6]])
                hull() {
                    translate([seg[0], seg[1], fx_t - 1.4]) cylinder(d = chan_w, h = 1.5);
                    translate([seg[2], seg[3], fx_t - 1.4]) cylinder(d = chan_w, h = 1.5);
                }
            // zip-tie slots
            for (p = [[-10, 34], [30, -30], [-52, -4]])
                translate([p[0], p[1], -0.1]) linear_extrude(fx_t + 0.2) rrect2d(4, 8, 1.5);
            // labels
            lbl(x_xiao, y_xiao + 14, "XIAO S3");   // above the board (v0.3): under it, the reed wire channel ran through the "O"
            lbl(x_bz,  y_bz - 10, "BZ1");
            lbl(x_led, y_led - 10, "DLED1");
            lbl(x_btn, y_btn - 8, "SW1");
            lbl(x_reed, y_reed - 8, "SW2");
            lbl(x_reed + 34, y_reed - 8, "MAG1 >>>");
            // corner mounting/rubber-feet holes
            for (sx = [1, -1], sy = [1, -1])
                translate([sx*(fx_w/2 - 6), sy*(fx_h/2 - 6), -0.1]) cylinder(d = 3.4, h = fx_t + 0.2);
        }
        // XIAO cradle: rails + clips
        for (s = [1, -1]) {
            translate([x_xiao + s*(board_w/2 - 1.5) - 1.5, y_xiao - (board_l - 1)/2, fx_t - 0.01])
                cube([3, board_l - 1, standoff_h + 0.01]);
            edgeclip(x_xiao + s*board_w/2, y_xiao, s > 0 ? 0 : 180);
        }
        // END STOPS along the rails (v0.4): the clips hold X and Z only. A column
        // off each end of each long edge, 0.3 off the drawn end, up to the PCB's
        // top face and outside the overmold across the axis; its root runs back
        // into the rail's end at rail height (the rail is 3 wide under it)
        for (sx = [1, -1], sy = [1, -1]) {
            x0 = x_xiao + (sx > 0 ? stop_x0 : -board_w/2);
            translate([x0, y_xiao + (sy > 0 ? board_l/2 + 0.3 : -(board_l/2 + 0.3 + core_min_wall())), fx_t - 0.01])
                cube([board_w/2 - stop_x0, core_min_wall(), standoff_h + board_h + 0.01]);
            translate([x0, y_xiao + (sy > 0 ? (board_l - 1)/2 - 0.01 : -(board_l/2 + 0.3) - 0.01), fx_t - 0.01])
                cube([board_w/2 - stop_x0, board_l/2 + 0.3 - (board_l - 1)/2 + 0.02, standoff_h + 0.01]);
        }
    }
}

// magnet carriage: slides in the rail groove toward/away from the reed.
// One press-fit magnet pocket, OPEN from below (a sealed two-pocket version
// rendered as an enclosed cavity — mesh-check catch).
module slider() {
    // the relief's height over the groove floor: the groove's 1.8 + the reed bore
    // crown's 0.3 + tol_press above the face, + 0.2 clear (2.4) — the glass can
    // rise no higher than that crown
    rl_h = 1.8 + 0.3 + tol_press + 0.2;
    difference() {
        union() {
            linear_extrude(1.7) rrect2d(20, 8, 1);                  // rail foot
            translate([0, 0, 1.7 - 0.01]) cylinder(d = 8, h = 6);   // grip knob
        }
        // underside relief channel over the reed's glass, which stands 2.0 proud of
        // the groove floor since the channel became an open slot (v0.3: this was 1.2
        // tall, cut for the 1.1 the glass stood when buried, and the slider rode on it)
        translate([0, 0, rl_h/2 - 0.05]) cube([20.2, reed_d + 1.0, rl_h + 0.1], center = true);
        // magnet pocket, loaded from the KNOB TOP (a pocket open from below put the
        // magnet's face at z 0, in the glass's path) on a 0.8 web over the relief
        translate([0, 0, rl_h + 0.8]) cylinder(d = mag_d + 2*tol_press, h = 10);
    }
}

if      (part == "plate")  plate();
else if (part == "slider") slider();
else { plate(); translate([fx_w/2 + 18, 0, 0]) slider(); }
