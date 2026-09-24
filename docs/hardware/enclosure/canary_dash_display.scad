// ============================================================================
//  Canary — DASHBOARD DISPLAY CASE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Housing for the Waveshare ESP32-S3-Touch-LCD-4.3 — the "step-up dashboard"
//  option from docs/hardware/display_research.md (Option B): a 4.3" 800x480
//  IPS touch panel for an event-timeline view, vs the Watch Station's glance
//  puck. Three parts:
//
//    frame — front bezel shell; the panel drops in face-first, the bezel lip
//            overlaps the glass 2.5 mm all round (prints face-down: the
//            A-surface is your textured build plate)
//    back  — vented rear cover, M2 self-tap corner bosses; carries TWO
//            through-keyholes AND a 75 mm horizontal M4 pair (a VESA-75
//            square can't fit this panel height)
//    stand — free-standing desk cradle, 25 deg recline, no hardware
//
//  ⚠️ DIMENSIONS ARE NOMINAL — Waveshare doesn't publish a full mechanical
//  drawing. MEASURE your board (glass outline, rear stack depth, USB
//  position) before printing. USB-C exits the bottom wall; the CAN/RS485
//  terminal zone gets an optional opening.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

use <canary_core_lib.scad>     // rrect2d + the catalog tolerance trio the knobs cite
use <canary_port_lib.scad>     // the Type-C overmold envelope the bottom-wall channel passes
use <canary_cradle_lib.scad>   // the click-on wall dock — this case is so far
                               // its only adopter (the 7" hangs on two screws
                               // through plain keyholes, no dock)
// Downloaded this file alone? The back's dock pads and their pockets come
// from that library. A missing one would render a back plate with four blind
// bumps and no way to hang it, with only a console warning. Hard stop:
assert(is_num(cr_pad_h()),
       "canary_cradle_lib.scad is MISSING — this case docks on it. Download canary_cradle_lib.scad (and the canary_mount_lib / canary_core_lib files it and this case lean on) from the same folder and keep them side by side.");

/* [What to render] */
part = "all";        // ["frame","back","stand","cradle","all"]

/* [Panel] — Waveshare ESP32-S3-Touch-LCD-4.3. MEASURE YOURS */
panel_l = 106.3;     // glass outline X — MEASURE
panel_w = 66.2;      // glass outline Y — MEASURE
glass_t = 3.2;       // glass + panel module thickness — MEASURE
stack_t = 8.0;       // rear stack: PCB + connectors behind the glass — MEASURE
bez_lip = 2.5;       // bezel overlap onto the glass edge

/* [USB / terminals] — positions along the BOTTOM wall, from panel center.
   The USB opening is the plug OVERMOLD channel, not a shell-sized hole: the
   receptacle sits at the panel edge, a full frame_w + tol_slide behind the
   outer face, and a 12.0 x 6.5 hole stopped a Type-C spec-max overmold
   (12.35 wide, canary_port_lib) at the outer face. The channel is the
   overmold + tol_hole a side, centered on the shell's axis, and runs OPEN to
   the frame's rear rim — the screwed-on back plate closes it, and the
   face-down frame prints it with no bridge at all (the terminal notch's
   move). */
usb_dx   = 0.0;      // USB-C channel center offset along the bottom wall — MEASURE
usb_axis_z = 9.35;   // USB-C shell axis above the frame's face (frame z) — MEASURE (the old opening's center)
usb_w    = 12.95;    // wall opening the plug and its overmold boot pass through: spec-max 12.35 + 2 x tol_hole
usb_h    = 7.1;      // wall opening height the plug and its boot pass through: spec-max 6.5 + 2 x tol_hole
term_open = false;   // also open the CAN/RS485 terminal zone (a parting-line notch to the rear rim; the back plate closes it)
// term_h is the connector zone's height: the notch must clear it (asserted
// below), and the back plate covers everything above it
term_dx  = -30.0;    // terminal zone center offset along the bottom wall, from panel center
term_w   = 24.0;     // terminal notch width
term_h   = 8.0;      // connector zone height — the notch must clear it; the back plate covers the rest

/* [Print tolerances] — tune with canary_fit_coupon.scad */
tol_slide = 0.20;  // catalog default — canary_core_lib core_tol_slide()
tol_press = 0.10;  // catalog default — canary_core_lib core_tol_press()
tol_hole  = 0.30;  // catalog default — canary_core_lib core_tol_hole()

/* [Shell] */
frame_w = 3.5;       // side wall thickness
face_t  = 2.4;       // bezel face thickness
back_t  = 3.0;       // 3.0 (was 2.4): the back carries the four dock pads the cradle assumes COPLANAR,
                     // and a 114 x 74 sheet at 2.4 does not stay a plane. Ribs cannot go on it — the
                     // inner face is the bed and the pads own the outer face — so the stiffness is t^3:
                     // 1.95x for 0.6 mm and 4.7 g  // [2.4:0.2:4]
r_out   = 5.0;   // deviates: display-frame radius class — scaled to the 4.3-inch face (palm shells run core_corner_r())

/* [Fasteners] — M2 x 8 self-tappers into corner lobes OUTSIDE the cavity
   (bosses inside the frame would collide with the panel's sharp glass corners) */
lob_d = 7.0;   // screw lobe Ø
lob_o = 2.2;   // lobe diagonal offset outboard of the cavity corner
pilot_d = 1.7;  screw_c = 2.4;  cb_d = 4.4;  cb_h = 1.4;
screw_l = 8.0;   // M2 self-tapper length the pilots are cut for (the BOM's M2 x 8)  // [6:1:12]

/* [Rear mounts] */
// v0.2: the back DOCKS on a cradle (canary_cradle_lib.scad) — a wall plate
// takes the screws and the case clicks onto it.
//
// What it replaces, and why. v0.1 put a 75 mm horizontal M4 pair on one
// centerline and two keyholes on the other, so the four mount points formed
// a DIAMOND across the back: two different fastener systems, crossed, each
// covering for what the other could not do. It was never a pattern anyone
// chose — it was the residue of "a true VESA-75 square does not fit a 74 mm
// shell" plus "but it should also hang on two screws." The cradle makes the
// question go away: the wall gets one plate with two screws in it, and the
// case gets four low pads and no fasteners at all.
mount = "cradle";    // ["cradle","none"] — "none" leaves the back bare for
                     // a desk-only build (the stand needs no mount features)
cradle_dx = 38.0;    // half-span of the four dock features, X…
cradle_dy = 16.0;    // …and Y. Asserted below: wide enough that the case
                     // cannot pivot on them and the clip arms clear each
                     // other, small enough that the plate hides behind the
                     // case and the pads miss the vent rows.

/* [Vents] */
vent_n = 8;  vent_w = 1.2;  vent_l = 16.0;

/* [Stand] */
stand_ang = 25;      // recline
stand_w = 120.0;  stand_d = 78.0;  stand_t = 4.0;

/* [Quality] */
$fa = 3; $fs = 0.4;

// ---- derived ----------------------------------------------------------------
inner_l = panel_l + 2*tol_slide;
inner_w = panel_w + 2*tol_slide;
out_l = inner_l + 2*frame_w;
out_w = inner_w + 2*frame_w;
cav_t = glass_t + stack_t + 3.0;        // frame interior depth: +3 mm rear clearance so the
                                        // back plate, keyhole screw heads and VESA-arm screw
                                        // tips land in AIR, not on the electronics stack
frame_h = face_t + cav_t;               // frame total height
total_t = frame_h + back_t;
view_l = panel_l - 2*bez_lip;
view_w = panel_w - 2*bez_lip;

assert(bez_lip >= 1.5, "bezel lip < 1.5 mm won't retain the glass");
// lobe must clear the cavity corner (cavity corner radius = 2)
assert(sqrt(2)*(lob_o + 2) >= lob_d/2 + 2 + 0.2,
       "screw lobe intrudes into the panel cavity — raise lob_o or shrink lob_d");
// The dock interface checks itself (strain budget, engagement, pad depth) —
// `use <>` does not run a library's top-level asserts, so the adopter has to
// ask. See the note in canary_cradle_lib.scad's header.
assert(mount != "cradle" || cradle_selfcheck(), "cradle interface self-check failed");
assert(mount != "cradle" || cradle_span_ok(cradle_dx, cradle_dy),
       "dash: cradle span too small — the case would pivot on its dock, or the clip arms would meet in the middle");
// the plate has to HIDE behind the case, or the mount is back on the outside
assert(mount != "cradle"
       || (cradle_plate_w(cradle_dx) <= out_l - 6 && cradle_plate_h(cradle_dy) <= out_w - 6),
       "dash: the cradle plate is bigger than the case it should disappear behind — shrink the span");
// ...and the pads must miss the vent rows, which own y = ±(out_w/2 - 20 .. -4)
assert(mount != "cradle"
       || cradle_dx - (cr_barb_w() + 9)/2 > (vent_n - 1)*8/2 + vent_w,
       "dash: a cradle pad lands on the back vent row — widen cradle_dx or narrow the vent field");
assert(usb_h <= stack_t, "usb_h exceeds the rear stack depth");
assert(usb_w >= port_usbc_overmold_w() && usb_h >= port_usbc_overmold_h(),
       "the USB opening is smaller than a spec-max Type-C overmold — the plug would stop at the outer wall");
assert(usb_axis_z - usb_h/2 >= face_t + glass_t,
       "the USB channel's floor cuts into the panel's glass zone — check usb_axis_z");
assert(!term_open || face_t + glass_t + 0.5 + term_h <= frame_h + 1e-9,
       "the terminal zone stands taller than the frame — its notch cannot clear it");
// the USB slot and the terminal notch must not merge into one ragged opening
assert(!term_open || abs(term_dx - usb_dx) >= term_w/2 + usb_w/2 + 3.0,
       "terminal notch overlaps the USB slot — separate term_dx/usb_dx");
assert(cb_h + 1.0 <= back_t, "counterbore through the back plate");
pilot_l = screw_l - (back_t - cb_h) + 0.5;
assert(pilot_l <= frame_h - face_t, "the screw pilot runs into the face — shorten screw_l");

echo(str("Canary dash display v0.1-dev — ", out_l, " x ", out_w, " x ", total_t,
         " (view ", view_l, " x ", view_w, ")  (IN DEVELOPMENT — MEASURE YOUR PANEL)"));

// (rrect2d comes from canary_core_lib — this file's private copy is gone)
function bosses() = [for (sx = [1, -1], sy = [1, -1])
    [sx*(inner_l/2 + lob_o), sy*(inner_w/2 + lob_o)]];
module outline2d() {                    // shell + corner screw lobes
    rrect2d(out_l, out_w, r_out);
    for (p = bosses()) translate(p) circle(d = lob_d);
}

// ---- frame (z0 = face; print face-down) --------------------------------------
module frame() {
    difference() {
        linear_extrude(frame_h) outline2d();
        // view window through the face
        translate([0, 0, -0.1]) linear_extrude(face_t + 0.2) rrect2d(view_l, view_w, 2);
        // panel cavity behind the face
        translate([0, 0, face_t]) linear_extrude(cav_t + 0.1) rrect2d(inner_l, inner_w, 2);
        // corner reliefs: the panel has SHARP glass corners (see header) and a
        // rounded pocket corner (r2) interferes 0.55 mm — drill the corners out
        for (sx = [1, -1], sy = [1, -1])
            translate([sx*inner_l/2, sy*inner_w/2, face_t])
                cylinder(d = 3.4, h = cav_t + 0.1);
        // USB-C overmold channel through the bottom (-Y) wall, centered on
        // the shell's axis and open to the rear rim (see the [USB / terminals]
        // note): a round-ended floor the overmold rests in, straight sides up
        // to the rim — no bridge, and the back plate is its roof
        translate([usb_dx, -out_w/2 + frame_w + 0.1, usb_axis_z])
            rotate([90, 0, 0]) linear_extrude(frame_w + 0.2) {
                pill2d(usb_w, usb_h);
                translate([-usb_w/2, 0]) square([usb_w, frame_h - usb_axis_z + 0.1]);
            }
        // optional CAN/RS485 terminal opening. 24 mm is too wide for a
        // bridge-safe hole — port_bridge_profile2d refuses it (the 45°
        // chamfers it would need are taller than the opening; the old
        // constant-chamfer call shipped a 19.0 mm flat bridge here). So the
        // cut runs OPEN to the frame's rear rim instead: a parting-line
        // notch the screwed-on back plate covers, printing with zero
        // unsupported span in the face-down frame.
        if (term_open)
            translate([term_dx - term_w/2, -out_w/2 - 0.1, face_t + glass_t + 0.5])
                cube([term_w, frame_w + 0.2, frame_h - (face_t + glass_t + 0.5) + 0.1]);
        // top-wall chimney vents (pitch 8 keeps all of them inside the shell)
        for (i = [0:vent_n - 1])
            translate([-((vent_n - 1)*8)/2 + i*8 - vent_w/2,
                       out_w/2 - frame_w - 0.1, face_t + glass_t + 1])
                cube([vent_w, frame_w + 0.2, stack_t - 2]);
        // M2 pilots down into the lobes, as deep as the screw reaches past the
        // back plate plus 0.5 (a fixed 6.0 stopped an M2 x 8 0.4 mm short of
        // clamping: 8 - (back_t - cb_h) = 6.4 of thread into a 6.0 hole)
        for (p = bosses())
            translate([p[0], p[1], frame_h - pilot_l]) cylinder(d = pilot_d, h = pilot_l + 0.1);
    }
}

// ---- back (z0 = outer face) ---------------------------------------------------
// The dock pads stand on the OUTER (wall-facing) face, which in this part's
// frame is z = 0 — hence the mirror. Getting that backwards puts four solid
// bumps inside the cavity, on top of the board, and the render still exports
// a perfectly watertight mesh that is simply wrong; the counterbores at z ≈ 0
// are the tell, since screws enter from outside.
//
// PRINT IT PADS-UP (inner face on the bed). v0.1 said outer-face-down for a
// clean A-surface, which was never the point here — the outer face is the one
// against the wall. Pads-up puts the pocket bridges where a bridge belongs
// and leaves nothing overhanging.
module back() {
    difference() {
        union() {
            linear_extrude(back_t) outline2d();
            if (mount == "cradle")
                mirror([0, 0, 1]) cradle_pads(cradle_dx, cradle_dy, 0);
        }
        if (mount == "cradle")
            mirror([0, 0, 1]) cradle_pad_cuts(cradle_dx, cradle_dy, 0);
        // screw counterbores + clearance (screws from the back into the bosses)
        for (p = bosses()) translate([p[0], p[1], 0]) {
            translate([0, 0, -0.1]) cylinder(d = cb_d, h = cb_h + 0.1);
            translate([0, 0, -0.1]) cylinder(d = screw_c, h = back_t + 0.2);
        }
        // (v0.1's M4 pair + centerline keyholes lived here — the diamond.
        //  They are gone, not disabled: the cradle covers both jobs, and a
        //  back plate carrying two retired mount systems as well as the live
        //  one is how a part ends up with four holes nobody can explain.)
        // vent slots (top third, mirrored bottom third)
        for (i = [0:vent_n - 1], sy = [1, -1])
            translate([-((vent_n - 1)*8)/2 + i*8 - vent_w/2, sy*(out_w/2 - 12) - vent_l/2, -0.1])
                cube([vent_w, vent_l, back_t + 0.2]);
    }
}

// ---- stand (free-standing cradle; prints flat) -------------------------------
// The case's lowest point is its corner screw lobes (lobe_low_y), and it rests
// there on the base at the channel's rear edge (chan_back, stand_t), leaning
// back on the fin. Everything that holds it is derived from that line.
//
// FRONT LIP: a wedge whose back face follows the case's front face, 0.2 clear
// (the 1.69 stand's yf()). The old lip was an 8 mm block at a fixed y: the
// case's front-bottom corner stood 0.3 above its top and 3.8 behind it, so the
// foot of a leaning case could slide forward with nothing to stop it. Inside
// the lip's span the frame's straight bottom edge rides ylob above the lobes,
// so the wedge climbs the face to frame_w above that edge — it captures the
// frame's border band and stops short of the view window.
//
// CABLE SLOT: the USB plug leaves the bottom wall pointing down into the
// base, so a slot runs through the base and the lip at the plug's x (the 1.69
// and Watch stands' move) — a spec-max overmold landed 313.9 mm³ in the base.
lobe_low_y = inner_w/2 + lob_o + lob_d/2;     // the lobes' reach below the center
ylob       = lobe_low_y - out_w/2;            // the straight bottom edge above that
cable_w    = port_usbc_overmold_w() + 2*1.0;  // 1.0 of play a side: the plug arrives tilted
module stand() {
    a = stand_ang;
    chan_w = total_t + 2.0;   // channel for the tilted display's bottom edge
    chan_back = -stand_d/2 + 16 + chan_w;      // back-rail front face
    // fin position DERIVED so its tilted front face contains the plane the
    // display's back face lies in when its bottom rear edge sits at the rail
    // (0.8 lands the face on that plane within 0.01 mm; the old 0.6 left the
    // fin 0.17 mm proud of it — a real, if tiny, interference fit)
    // The back grew four cradle pads (cr_pad_h() proud of its outer face), so
    // on this stand the case leans on its PAD TIPS, not its back face: the fin
    // plane steps back by the pad height along its normal (pad / cos(ang)
    // horizontally). Without it the pads buried 6.5 mm into the fin.
    back_off = (mount == "cradle") ? cr_pad_h() : 0;
    fin_y = chan_back + 4*cos(a) + 0.8 + back_off/cos(a);
    fw = stand_w - 30;
    // the case's front face: its lifted front-bottom corner (on the lobes'
    // line), then y at height z along the reclined face, 0.2 clear
    cy = chan_back - total_t*cos(a);  cz = stand_t + total_t*sin(a);
    function yf(z) = cy + (z - cz)*tan(a) - 0.2;
    lip_top = cz + (ylob + frame_w)*cos(a);
    // the plug's axis stands zu in front of the back's outer face; the rear
    // edge of its channel envelope runs parallel to the back plane, so it
    // crosses the base top (zu − usb_h/2)/cos(a) in front of the rest line.
    // World x mirrors the frame's x: the frame turns face-out about Y.
    zu      = total_t - usb_axis_z;
    slot_y1 = chan_back - (zu - usb_h/2)/cos(a) + 0.4;
    assert(slot_y1 <= chan_back - 0.5, "the cable slot reaches the rest line — the case's lobes would lose their land");
    assert(yf(stand_t) - 3 >= -stand_d/2 + 2, "the front lip runs off the stand's front edge — deepen stand_d");
    difference() {
        union() {
            linear_extrude(stand_t) rrect2d(stand_w, stand_d, 6);          // base
            // back rest fin, leaning stand_ang from vertical, HULLED down to its
            // footprint on the base: a bare tilted block stood on its back
            // corner, its front-bottom edge 4·sin(ang) = 1.7 over the base —
            // an overhang the print had to bridge. The front face's plane
            // meets the base 4/cos(ang) ahead of fin_y, so the footprint
            // keeps that plane exactly.
            hull() {
                translate([0, fin_y, stand_t - 0.01])
                    rotate([-a, 0, 0]) translate([-fw/2, -4, 0])
                        cube([fw, 8, 42]);
                translate([-fw/2, fin_y - 4/cos(a), stand_t - 0.01]) cube([fw, 8/cos(a), 0.01]);
            }
            // front lip: a wedge whose back face follows the case's front face
            hull() {
                translate([-fw/2, yf(stand_t) - 3, stand_t - 0.01]) cube([fw, 3, 0.02]);
                translate([-fw/2, yf(lip_top) - 3, lip_top - 0.02]) cube([fw, 3, 0.02]);
            }
            // back rail: a reclined panel's back plane retreats tan(ang) per mm of
            // height, so a rail flush at the panel's bottom edge digs its top corner
            // ~h·tan(ang) into the glass-back. Set the rail back by exactly that
            // (+0.4 clearance) so it backstops the edge without touching the panel.
            // ⚠️ regenerate enclosures/preview/canary_dash_display_stand.stl.
            translate([-fw/2, chan_back + 6*tan(a) + 0.4, stand_t - 0.01])
                cube([fw, 3, 6]);
        }
        // the cable slot: base and lip, open to the front edge
        translate([-usb_dx - cable_w/2, -stand_d/2 - 1, -0.1])
            cube([cable_w, slot_y1 + stand_d/2 + 1, lip_top + 1]);
    }
}

// ---- the wall cradle ----------------------------------------------------------
// Prints face-down exactly as modeled: studs and clip barbs point up, and
// every overhang on it is either a 45° ramp or the T-stud's 1.3 mm collar.
module cradle() {
    assert(mount == "cradle", "part=\"cradle\" needs mount=\"cradle\"");
    cradle_plate(cradle_dx, cradle_dy,
                 cradle_plate_w(cradle_dx), cradle_plate_h(cradle_dy));
}

// ---- selector -----------------------------------------------------------------
if      (part == "frame")  frame();
else if (part == "back")   back();
else if (part == "stand")  stand();
else if (part == "cradle") cradle();
// DOCK FIT GATE — the seated plate against the back it seats in. Must be
// EMPTY: studs sit in their keyholes, barbs in their pockets, and nothing
// touches anything. This is the check that a span, a tolerance or a moved
// feature cannot quietly break, because it tests the whole engagement at
// once rather than each dimension on its own. (The back is modeled with its
// outer face at z = 0, so that is the z0 the dock transform wants.)
else if (part == "dock_probe") {
    assert(mount == "cradle", "part=\"dock_probe\" needs mount=\"cradle\"");
    // The SAME mirror the pads are built under — this part models its outer
    // face at z = 0 with the case behind it, so the dock grows in -z. Without
    // it the plate lands on the far side and only its studs and barbs reach
    // back into the slab, which is exactly what this gate reported the first
    // time it ran: four small volumes, one per feature. A gate that can
    // localize its own failure to "the four things that stick out" is worth
    // more than one that just says no.
    intersection() { back(); mirror([0, 0, 1]) cradle_docked(0) cradle(); }
}
else {
    frame();
    translate([0, out_w + 14, 0]) back();
    translate([0, -(out_w/2 + stand_d/2 + 16), 0]) stand();
    if (mount == "cradle")
        translate([out_l + 20, out_w + 14, 0]) cradle();
}
