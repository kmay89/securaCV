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
// The PLUG side of the same standard. A shell-sized opening in a wall the
// receptacle sits behind is a cable blocker: the overmold stops at the outer
// face and the shell latches on whatever is left. Any wall standing between
// the outer face and the receptacle face must open to the OVERMOLD envelope,
// not the shell — the Type-C spec's maximum overmold is the envelope a
// compliant cable may fill, so it is the envelope the case must pass.
function port_usbc_overmold_w() = 12.35; // Type-C spec max plug overmold width
function port_usbc_overmold_h() = 6.5;   // ...and height
function port_usbc_insertion()  = 6.5;   // Type-C mated insertion depth: what the
                                         // shell must enter for the latch to take
function port_insertion_min()  = 11.0;   // series-A insertion-length floor (mm)
function port_bridge_cham()    = 2.5;    // the WAP top-corner chamfer size (minimum)
// The residual FLAT the chamfers may leave. The WAP's print-validated opening
// is 12.0 wide with 2.5 chamfers = a 7.0 mm bridge; that is the widest flat
// this catalog has printed clean, so it is the ceiling. The chamfer used to
// be a constant, which bounded nothing: the dash display's 24 mm terminal
// opening went through the same call and left a 19.0 mm bridge.
function port_flat_span_max()  = 7.0;
// The chamfer a given width needs to stay inside that ceiling — the WAP's own
// 2.5 for every opening up to 12.0, growing with the width past it.
function port_bridge_cham_for(w) = max(port_bridge_cham(), (w - port_flat_span_max())/2);

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
// The default chamfer SCALES with the width (port_bridge_cham_for), so a wide
// opening grows its chamfers instead of its bridge; the assert makes the
// ceiling hard for explicit chamfers too. An opening too short to host the
// chamfer it needs (cham >= h) fails here by design — that geometry has no
// bridge-safe form at 45°, and the honest fixes are a taller opening, a
// splitting mullion, or running the cut open to a parting line the mating
// part covers (the dash terminal does the last).
module port_bridge_profile2d(w, h, cham = undef) {
    c = is_undef(cham) ? port_bridge_cham_for(w) : cham;
    assert(c >= 0 && 2*c < w && c < h,
           str("port_bridge_profile2d: a ", w, " x ", h, " opening cannot host its ",
               c, " chamfers — too wide for its height to bridge safely at 45 "));
    assert(w - 2*c <= port_flat_span_max() + 1e-9,
           str("port_bridge_profile2d: ", w - 2*c, " mm of flat bridge survives the ",
               "chamfers — the print-validated ceiling is ", port_flat_span_max(),
               " mm. Let the chamfer default scale, or split the opening."));
    if (c > 0)
        polygon([[-w/2, -h/2], [w/2, -h/2],
                 [w/2, h/2 - c], [w/2 - c, h/2],
                 [-w/2 + c, h/2], [-w/2, h/2 - c]]);
    else
        square([w, h], center = true);
}

// ---------------------------------------------------------------------------
//  The insertion gate — every case that hosts a series-A PLUG end calls
//  this with what its wall arithmetic leaves of the shell's overhang.
// ---------------------------------------------------------------------------
module port_assert_insertion(remaining, what = "the USB-A plug") {
    // the floor is spelled out in the text (str(11.0) would print "11",
    // and a spec number reads like one with its decimal on)
    assert(remaining >= port_insertion_min(),
           str("port: ", remaining, " mm of insertion length survives for ",
               what, " — the floor is 11.0 mm or a recessed receptacle no ",
               "longer latches. Thin the end wall or relieve its outer ",
               "face; never shorten the shell."));
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
    assert(port_flat_span_max() == 7.0,
           "port: the flat ceiling is the WAP's own 12.0 - 2*2.5");
    // the scaled chamfer must reproduce the WAP's opening EXACTLY (that
    // geometry is print-validated; the scaling exists for wider openings)
    assert(port_bridge_cham_for(12.0) == port_bridge_cham(),
           "port: the scaled chamfer must leave the validated 12.0 opening alone");
    assert(port_bridge_cham_for(20.0) == 6.5,
           "port: a 20 mm opening must grow its chamfers to hold the 7.0 flat");
    assert(port_usbc_overmold_w() == 12.35 && port_usbc_overmold_h() == 6.5,
           "port: the plug overmold envelope is the Type-C spec maximum — not a knob");
    echo("canary_port_lib: self-check OK");
}
