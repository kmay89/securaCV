// ============================================================================
//  Canary — OUTLET PLUG-IN CRADLE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Wyze-plug-style zero-hardware indoor mounting: a collar that grips a USB
//  wall adapter ("wart") plugged into an outlet, carrying two printed T-studs
//  on its face — a Canary case with a PAIR of blind keyhole pockets hangs
//  directly on the studs, powered by a short USB-C cable looped behind it.
//  Measure YOUR adapter body (they vary wildly) and print the collar snug; a
//  TPU collar grips best.
//
//  WHICH CASES. The studs must match the case's pocket spacing (`target`
//  picks it) AND fit on the collar's face, which is only as tall as the
//  adapter it grips. The earlier header promised the WAP, Vision and Sense on
//  one fixed 30 mm pair; only the Sense's pockets are near 30. At the default
//  36 mm adapter only the Sense fits; the others need a taller adapter body
//  (wart_h at least the spacing + 3.8 — asserted): Vision 50.4 (XIAO host) /
//  51.8 (DevKit host), WAP 66.0 (custom defaults) / 84.3 (battery presets).
//  The WAP's compact preset has ONE centered pocket — it cannot hang on a
//  stud pair at all.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

use <canary_core_lib.scad>   // shared 2D primitives — rrect2d has one home now
use <canary_mount_lib.scad>  // the stud/keyhole standard this file carries around

/* [What to render] */
part = "cradle";     // ["cradle"]

/* [USB wall adapter body] — MEASURE yours (typical 1-port cube shown) */
wart_w = 33.0;       // width (across the outlet)
wart_h = 36.0;       // height
wart_d = 26.0;       // protrusion from the wall plate (collar grips this)
collar_t = 2.4;      // collar wall
collar_d = 14.0;     // collar depth along the wart (leave the plug face clear)
plug_room = 8.0;     // pocket behind the stud face for a RIGHT-ANGLE USB-A plug's body: the
                     // case hangs flush on that face, so a plug standing proud of it had
                     // nowhere to go (0 = plain 3 mm face, cable through the window)
grip_lip = 1.2;      // inward LOCATING lip at the collar front (clearance-sized: it
                     // locates the wart, retention is friction — print the collar in
                     // TPU, or shrink the lip opening ~1 mm for a true snap grip)

/* [Stud/keyhole interface] — T-studs matching the standard Canary keyhole pockets */
// the case this cradle hangs — sets the stud spacing from the table below
target     = "sense";   // ["sense","vision_xiao","vision_devkit","wap_custom","wap_battery","custom"]
stud_gap   = 30.0;   // stud spacing (vertical) for target = "custom" — must match that case's keyhole pocket spread
kh_face    = 1.0;    // target pocket's face web — canary_mount_lib mount_kh_face()

/* [Print tolerances] */
tol_slide = 0.20;

/* [Quality] */
$fa = 3; $fs = 0.4;

iw = wart_w + 2*tol_slide;
ih = wart_h + 2*tol_slide;
// Each case's pocket spread — the distance between its two keyhole pocket
// centers (2 × kh_ys, or 2 × kh_xs for the WAP, whose pockets run along its
// X), read off each case's own derivation at its defaults with OpenSCAD
// 2021.01 (2026-09). `use<>` cannot carry a case's variables across, so these
// are documented numbers: re-read them if a case's kh_inset or cavity moves.
//   Sense, every preset:        2 × 15.1   (inner_y/2 − kh_inset)
//   Vision, host "xiao":        2 × 23.3
//   Vision, host "devkit":      2 × 24.0
//   WAP, preset "custom":       2 × 31.1   (inner_l/2 − kh_inset)
//   WAP, battery presets:       2 × 40.25
function target_gap() =
      target == "sense"         ? 30.2
    : target == "vision_xiao"   ? 46.6
    : target == "vision_devkit" ? 48.0
    : target == "wap_custom"    ? 62.2
    : target == "wap_battery"   ? 80.5
    : stud_gap;
gap = target_gap();
assert(collar_d > grip_lip + 4, "collar too shallow");
// the studs stand on the collar's face plate, so the pair's heads and a
// min-wall rim around them must fit inside it
assert(gap/2 + mount_stud_head()/2 + core_min_wall() <= (ih + 2*collar_t)/2 + 1e-9,
       str("the ", gap, " mm stud pair for target \"", target, "\" does not fit on a ",
           ih + 2*collar_t, " mm collar face — it needs an adapter body at least ",
           gap + mount_stud_head() + 2*core_min_wall() - 2*collar_t - 2*tol_slide,
           " mm tall (wart_h)"));
echo(str("Canary outlet cradle v0.1-dev — wart ", wart_w, "x", wart_h, "x", wart_d,
         " mm, studs at ", gap, " mm for ", target, "  (IN DEVELOPMENT)"));

// T-stud from the mount library; this file's placement signature survives as
// a wrapper. The stem tracks the kh_face knob (stem = face web + 0.4 slide
// room) so a nonstandard pocket still gets a matching stud.
module tstud(yc, zbase) {
    translate([0, yc, zbase]) mount_tstud(stem = kh_face + 0.4);
}

module cradle() {
    face_t = 3.0 + plug_room;                         // stud-carrying face plate (+ the plug chamber)
    union() {
        difference() {
            // collar + face, printed face-down (studs up)
            union() {
                linear_extrude(face_t) rrect2d(iw + 2*collar_t, ih + 2*collar_t, 4);
                translate([0, 0, face_t - 0.01]) linear_extrude(collar_d)
                    difference() {
                        rrect2d(iw + 2*collar_t, ih + 2*collar_t, 4);
                        rrect2d(iw, ih, 2.5);
                    }
            }
            // wart face sits against the plate; grip lip hooks its front corners
            translate([0, 0, face_t + grip_lip])
                linear_extrude(collar_d) rrect2d(iw + 1.2, ih + 1.2, 3);
            // plug chamber: a blind pocket from the wart side, plug_room deep, for a
            // right-angle plug's body (22 x 14), leaving 3 mm of stud face; the
            // cable leaves through a slot in the chamber's bottom wall
            if (plug_room > 0) {
                translate([0, -ih/2 + 8, 3.0]) linear_extrude(plug_room + grip_lip + 0.2) rrect2d(22, 14, 3);
                translate([0, -ih/2 - collar_t - 1, 3.0]) linear_extrude(plug_room - 1.0) rrect2d(9, 2*(collar_t + 1 + 8 - 7), 2);
            } else
                translate([0, -ih/2 + 8, -0.1]) linear_extrude(face_t + grip_lip + 0.2) rrect2d(18, 10, 3);
            // side split so the collar can flex over the wart (print PETG/TPU)
            translate([iw/2 - 2, -1.25, face_t + 2]) cube([collar_t + 4, 2.5, collar_d]);
        }
        // hanging studs on the FRONT face (-Z, opposite the wart) — mirrored so
        // they protrude instead of burying in the plate (review catch).
        // Print: collar-down (the plate bridges the collar) or support the studs.
        mirror([0, 0, 1]) { tstud( gap/2, -0.01); tstud(-gap/2, -0.01); }
    }
}

cradle();
