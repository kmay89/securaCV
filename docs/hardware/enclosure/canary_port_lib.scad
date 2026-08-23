// ============================================================================
//  Canary PORT library — connector openings, drawn once, printable every time
//
//  NOT A PRINTABLE PART. `use <canary_port_lib.scad>` from a case file.
//
//  Three lessons this catalog paid for, now in one place:
//
//  1. THE BRIDGE-SAFE TOP (the WAP's opening, print-validated). A USB
//     opening in an upright-printed wall is a bridge; a 12 mm flat top
//     droops into the plug envelope. 45°-chamfered top corners halve the
//     unsupported span and keep any droop out of the plug's way. The WAP
//     drew this and its siblings kept cutting plain rectangles — the
//     Vision, the Sense, the combo and the relay all carried ~10.5 mm flat
//     bridges over their connectors. port_bridge_profile2d() is the WAP's
//     polygon, verbatim.
//
//  2. SHAPE FOLLOWS SHELL (the C3/stick port doctrine). USB-C is a stadium
//     — full-round ends, hugging the shell. Series-A is a RECTANGLE — cut a
//     stadium and the shell's four square corners have nowhere to go, the
//     plug does not pass, and the instinct is to open the hole until it
//     does, which ends as a sloppy oval around a square connector. The
//     shell dimensions are the USB mechanical standard and are NOT to be
//     "adjusted to fit": if the plug will not pass, the opening is wrong.
//
//  3. INSERTION LENGTH IS A HARD STANDARD (the USB-A plug-end lesson,
//     asserted in two files with two retyped floors before this one).
//     ~12 mm of a series-A shell must enter a receptacle; every millimeter
//     a case adds past the PCB edge comes straight off it. 11.0 mm is the
//     catalog floor — below it a recessed wall-wart receptacle no longer
//     latches. An assert, not a comment: the C3's first collar cut failed
//     exactly this at 8.8 mm, and only the assert saw it before a print.
//
//  `use<>` does not run top-level statements: call port_selfcheck() from an
//  adopter (the fit coupon does).
// ============================================================================

// ---------------------------------------------------------------------------
//  The standards — functions so `use<>` carries them
// ---------------------------------------------------------------------------
function port_usba_shell_w()   = 12.00;  // series-A shell width — USB 2.0 standard
function port_usba_shell_h()   = 4.50;   // series-A shell height — the standard
function port_usbc_shell_w()   = 8.94;   // USB-C receptacle shell, nominal
function port_usbc_shell_h()   = 3.26;
function port_insertion_min()  = 11.0;   // series-A insertion-length floor (mm)
function port_bridge_cham()    = 2.5;    // the WAP top-corner chamfer size

// ---------------------------------------------------------------------------
//  2D opening profiles — in the wall's plane, opening center at the origin.
//  Extrude through the wall from the caller's own transform (each wall has
//  its own axis; keeping the transform local is what lets an adopting case
//  render mesh-identical where the shape already matched).
// ---------------------------------------------------------------------------

// USB-C stadium: full-round ends, w x h overall
module port_usbc_stadium2d(w, h) {
    assert(w > h, "port_usbc_stadium2d: a stadium is wider than tall");
    hull() {
        translate([-(w - h)/2, 0]) circle(d = h);
        translate([ (w - h)/2, 0]) circle(d = h);
    }
}

// Series-A rectangle with a small corner break — a few tenths keeps the
// printed opening off an elephant-foot allowance and is invisible against
// the shell's near-sharp corners
module port_usba_rect2d(w, h, r = 0.35) {
    offset(r = r) offset(r = -r) square([w, h], center = true);
}

// The WAP's bridge-safe opening: rectangle with 45°-chamfered TOP corners
// (+y is the wall's up). cham = 0 degenerates to the plain rectangle.
module port_bridge_profile2d(w, h, cham = port_bridge_cham()) {
    assert(cham >= 0 && 2*cham < w && cham < h,
           "port_bridge_profile2d: chamfer must fit the opening");
    if (cham > 0)
        polygon([[-w/2, -h/2], [w/2, -h/2],
                 [w/2, h/2 - cham], [w/2 - cham, h/2],
                 [-w/2 + cham, h/2], [-w/2, h/2 - cham]]);
    else
        square([w, h], center = true);
}

// ---------------------------------------------------------------------------
//  The insertion gate — every case that hosts a series-A PLUG end calls
//  this with what its wall arithmetic leaves of the shell's overhang.
// ---------------------------------------------------------------------------
module port_assert_insertion(remaining, what = "the USB-A plug") {
    assert(remaining >= port_insertion_min(),
           str("port: ", remaining, " mm of insertion length survives for ",
               what, " — the floor is ", port_insertion_min(),
               " mm or a recessed receptacle no longer latches. Thin the ",
               "end wall or relieve its outer face; never shorten the shell."));
}

// ---------------------------------------------------------------------------
//  Self-check — call once from an adopter (the fit coupon does).
// ---------------------------------------------------------------------------
module port_selfcheck() {
    assert(port_usba_shell_w() == 12.00 && port_usba_shell_h() == 4.50,
           "port: the series-A shell is the USB 2.0 standard — not a knob");
    assert(port_insertion_min() == 11.0,
           "port: the insertion floor is the two files' shared 11.0");
    assert(port_bridge_cham() == 2.5,
           "port: the bridge chamfer is the WAP's print-validated 2.5");
    echo("canary_port_lib: self-check OK");
}
