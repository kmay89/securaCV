// ============================================================================
//  Canary SNAP library — the catalog's snap-fit engineering in one place
//
//  NOT A PRINTABLE PART. `use <canary_snap_lib.scad>` from a case file.
//
//  The catalog grew five snap systems (WAP cantilever board clips, the
//  C6/1.69 skirt nubs, the C3's derived-window nub, the stick's strain-
//  budgeted wall beams, the cradle's in-plane tongues) and only the newest
//  two did the arithmetic. The WAP's numbers were right — its comment
//  "1.0/0.5 keeps insertion strain ~4 % — vertical-print PETG cracks near
//  1.5/0.8" IS the beam formula, evaluated once by hand — but a comment
//  does not stop a copy from shipping the cracking numbers, and one did:
//  the bench fixture carried 1.5/0.8 for years. Math that lives in a
//  comment protects one file. Math that lives in an assert protects every
//  adopter. This file is the assert.
//
//  THE BEAM FORMULA. A cantilever of thickness t deflected d at its tip
//  over a working length L sees a peak surface strain
//        eps = 1.5 * t * d / L^2
//  Two budgets, because two duty cycles:
//    * snap_budget_once()  = 0.045 — a fit made a handful of times
//      (a board pressed into its clips). The WAP clip evaluates to ~4.3 %
//      against this; 1.5/0.8 evaluates to ~10 %, which is the crack.
//    * snap_budget_cycle() = 0.020 — a fit worked repeatedly or held
//      deflected (service lids, jigs, dock tongues). The stick's wall beams
//      budget 1.8 % and the cradle tongues 2 % — same doctrine, stated
//      there before this file existed.
//  Both budgets assume the flex crosses layer lines (the worst case, and
//  what a floor-rooted clip on a flat-printed part does). Flex kept
//  IN-PLANE with the layers (the cradle's tongues, the belt clip printed on
//  its side) is roughly twice as strong — the budget still binds, as margin.
//
//  THE WINDOW RULE (the C3's lid-slide lesson, print 3): one number cannot
//  size two features that are not the same size. A snap ridge and the
//  window it parks in are DIFFERENT sizes — the window follows the ridge by
//  derivation (ridge + play per side), never by sharing its knob. The
//  snap_window_* functions are that derivation; adopters that draw their
//  own nubs still size their windows through them.
//
//  `use<>` does not run top-level statements: call snap_selfcheck() from an
//  adopter (the fit coupon does).
// ============================================================================

// ---------------------------------------------------------------------------
//  The arithmetic
// ---------------------------------------------------------------------------
function snap_strain(t, d, len)  = 1.5 * t * d / (len * len);
function snap_budget_once()      = 0.045;
function snap_budget_cycle()     = 0.020;
// window derived from the ridge it parks: play per side, 0.15 the catalog
// default (a printed window comes out a hair small and the part still has
// to enter — the C3 tuned this on hardware)
function snap_window(ridge, play = 0.15) = ridge + 2*play;

// ---------------------------------------------------------------------------
//  Cantilever board clip — the WAP family's press-fit PCB retention, drawn
//  exactly as the seven local copies drew it (adopters render mesh-identical
//  parts). A beam rises from the floor beside the board edge; its lip hooks
//  over the board top; the board cams the lip out on the 45° lead-in and the
//  beam snaps back. The retention seat over the board stays FLAT (a sloped
//  seat loses grip); the underside is 45°-chamfered across the clearance gap
//  for cleaner FDM bridging.
//
//    cx        — position along the board edge (X)
//    ey        — the board edge line (Y of the edge itself)
//    sy        — +1 / -1: which side of the edge the clip stands on
//    z_floor   — cavity floor z (beam root)
//    z_board   — board TOP surface z (the lip seats here)
//    w/t/hook  — tab width along the edge / beam thickness / lip overhang
//    hook_h    — lip + lead-in height above the board top
//    clear     — beam face to board edge gap (a FIT — tune on the coupon)
//
//  The strain gate runs on every render: the beam's working length is the
//  root-to-seat rise, the tip deflection is the full lip (hook + clear cam
//  travel is bounded by hook), and the budget defaults to the once class.
// ---------------------------------------------------------------------------
module snap_boardclip(cx, ey, sy, z_floor, z_board,
                      w = 6.0, t = 1.0, hook = 0.5, hook_h = 1.2,
                      clear = 0.25, budget = snap_budget_once()) {
    len = z_board - z_floor;
    eps = snap_strain(t, hook, len);
    assert(len > 0, "snap_boardclip: board top must sit above the floor");
    assert(eps <= budget,
           str("snap_boardclip: insertion strain ", round(eps*1000)/10,
               " % exceeds the ", round(budget*1000)/10,
               " % budget — thin the beam (t), shorten the hook, or raise ",
               "the board (a longer beam flexes easier); 1.5/0.8-class ",
               "numbers crack vertical-print PETG"));
    bt = z_board;                        // board top surface (lip sits here)
    tp = bt + hook_h;                    // top of the clip
    pts = [ [clear, z_floor], [clear + t, z_floor],
            [clear + t, tp], [clear, tp],
            [-hook, bt], [0, bt], [clear, bt - clear] ];
    translate([cx, ey, 0]) scale([1, sy, 1]) translate([-w/2, 0, 0])
        rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(height = w) polygon(pts);
}

// ---------------------------------------------------------------------------
//  Self-check — the doctrine's own arithmetic. Call once from an adopter
//  (the fit coupon does).
// ---------------------------------------------------------------------------
module snap_selfcheck() {
    // the WAP's proven numbers pass the once budget...
    assert(snap_strain(1.0, 0.5, 4.2) <= snap_budget_once(),
           "snap: the WAP clip's 1.0/0.5 at L=4.2 must pass the once budget");
    // ...and the numbers that cracked do not — the gate must gate
    assert(snap_strain(1.5, 0.8, 4.2) > snap_budget_once(),
           "snap: 1.5/0.8 at L=4.2 must FAIL the once budget, or the gate is inert");
    assert(snap_budget_cycle() < snap_budget_once(),
           "snap: repeated flexing budgets tighter than one-time flexing");
    assert(snap_window(1.3) > 1.3,
           "snap: a window must be larger than its ridge");
    echo("canary_snap_lib: self-check OK");
}
