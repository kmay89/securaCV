// ============================================================================
//  Canary — OUTLET PLUG-IN CRADLE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  Wyze-plug-style zero-hardware indoor mounting: a collar that grips a USB
//  wall adapter ("wart") plugged into an outlet, carrying two printed T-studs
//  on its face — any Canary case with blind keyhole pockets (WAP weather,
//  Vision, Sense) hangs directly on the studs, powered by a short USB-C cable
//  looped behind it. Measure YOUR adapter body (they vary wildly) and print
//  the collar snug; a TPU collar grips best.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

/* [What to render] */
part = "cradle";     // ["cradle"]

/* [USB wall adapter body] — MEASURE yours (typical 1-port cube shown) */
wart_w = 33.0;       // width (across the outlet)
wart_h = 36.0;       // height
wart_d = 26.0;       // protrusion from the wall plate (collar grips this)
collar_t = 2.4;      // collar wall
collar_d = 14.0;     // collar depth along the wart (leave the plug face clear)
grip_lip = 1.2;      // inward LOCATING lip at the collar front (clearance-sized: it
                     // locates the wart, retention is friction — print the collar in
                     // TPU, or shrink the lip opening ~1 mm for a true snap grip)

/* [T-studs] — match the standard Canary keyhole pockets */
stud_gap   = 30.0;   // stud spacing (vertical) — must fit the target case's kh_ys spread
kh_face    = 1.0;

/* [Print tolerances] */
tol_slide = 0.20;

/* [Quality] */
$fa = 3; $fs = 0.4;

iw = wart_w + 2*tol_slide;
ih = wart_h + 2*tol_slide;
assert(collar_d > grip_lip + 4, "collar too shallow");
echo(str("Canary outlet cradle v0.1-dev — wart ", wart_w, "x", wart_h, "x", wart_d,
         " mm, studs at ", stud_gap, " mm  (IN DEVELOPMENT)"));

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }

module tstud(yc, zbase) {
    translate([0, yc, zbase]) {
        cylinder(d = 4.0, h = kh_face + 0.4);
        translate([0, 0, kh_face + 0.4]) cylinder(d1 = 4.0, d2 = 6.6, h = 1.2);
        translate([0, 0, kh_face + 1.6]) cylinder(d = 6.6, h = 0.8);
    }
}

module cradle() {
    face_t = 3.0;                                     // stud-carrying face plate
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
            // cable window through the face (USB-C plug + boot pass sideways)
            translate([0, -ih/2 + 8, -0.1]) linear_extrude(face_t + grip_lip + 0.2)
                rrect2d(16, 9, 3);
            // side split so the collar can flex over the wart (print PETG/TPU)
            translate([iw/2 - 2, -1.25, face_t + 2]) cube([collar_t + 4, 2.5, collar_d]);
        }
        // hanging studs on the FRONT face (-Z, opposite the wart) — mirrored so
        // they protrude instead of burying in the plate (review catch).
        // Print: collar-down (the plate bridges the collar) or support the studs.
        mirror([0, 0, 1]) { tstud( stud_gap/2, -0.01); tstud(-stud_gap/2, -0.01); }
    }
}

cradle();
