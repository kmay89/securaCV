// ============================================================================
//  Canary — SHOP TOOLS  ⚠️ IN DEVELOPMENT (v0.2-dev)
//    part = "insert_guide" — heat-set insert press guide: a corner block
//        that drops into a bore corner of the face-down shell, sits on the
//        ledge with two faces on the bore walls, and holds its bore over a
//        corner post's insert bore (screw_insert mode) — keeping the M2
//        insert and iron tip SQUARE while it melts in (crooked inserts are
//        the #1 insert failure). Print in PETG minimum (it touches hot
//        zones briefly); remove as soon as the insert seats.
//    part = "button_ring" — decorative accent ring for the doorbell button
//        bezel seat: sits under the 12 mm button flange, prints in any
//        contrast color (the Ring-faceplate delight, in 0.4 g).
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
//
//  v0.2-dev (2026-10-23): the insert guide could not seat. It was a tube
//  meant to slip over a Ø6.7 post, but no catalog corner post is free to
//  slip over: each stands 0.2 INTO two walls, its corner filled to the
//  walls, gusseted to 0.5 short of its end — and none is Ø6.7 (the WAP's is
//  5.9, the Vision's, Sense's and doorbell's 6.5). Set over the WAP's post
//  the tube met 237 mm³ of shell. It is now a block that registers where
//  every piston-plate case is the same shape: the bore corner, on the
//  ledge, tol_slide off both bore walls, with its bore on the post's axis
//  (post_dx/post_dy, measured per case in their help). The button ring took
//  tol_slide off twice (Ø16.1 in the Ø16.9 seat, 0.4 a side): it is now
//  bez_d, tol_slide a side once.
// ============================================================================

/* [What to render] */
part = "insert_guide";   // ["insert_guide","button_ring"]

/* [Insert guide] — sits in a bore corner on the ledge, over a screw_insert post */
post_dx  = 4.55;     // post center from the bore wall along X (ledge_w + the post's offset from the cavity wall): WAP 4.55, Vision and Sense 4.85, sealed doorbell 8.85
post_dy  = 4.55;     // post center from the bore wall along Y: WAP 4.55, Vision and Sense 4.85, sealed doorbell 8.85
bore_corner_r = 2.6; // the plate bore's corner radius (the case's bore_r): 2.6 on the unsealed WAP, Vision and Sense, 10.8 on the sealed doorbell
insert_d = 3.5;      // insert knurl OD (M2 short series); the guide bore passes it with 0.2 a side
guide_h  = 12.0;     // guide height above the ledge — the length that holds the iron square

/* [Button ring] — matches the doorbell bezel seat */
bez_d = 16.5;        // bezel seat diameter
btn_d = 12.0;        // button body diameter
ring_t = 0.8;

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide(), canary_core_lib

/* [Quality] */
$fa = 3; $fs = 0.4;

echo(str("Canary shop tools v0.2-dev — ", part, "  (IN DEVELOPMENT)"));

// The bore corner at the origin, its walls on x = 0 and y = 0, the block in
// +x +y; z = 0 is the face that sits on the ledge (the post end is
// pl_relief's 0.2 under it, so the insert leaves the guide straight into its bore)
module insert_guide() {
    sx = post_dx + (insert_d + 0.4)/2 + 2.4;   // the block: the guide bore plus a 2.4 wall past it
    sy = post_dy + (insert_d + 0.4)/2 + 2.4;
    difference() {
        linear_extrude(guide_h) intersection() {
            translate([tol_slide, tol_slide]) square([sx - tol_slide, sy - tol_slide]);   // tol_slide off both bore walls
            translate([bore_corner_r, bore_corner_r])                                   // and inside the bore's corner radius
                offset(r = max(bore_corner_r - tol_slide, 0.4)) square([sx, sy]);
        }
        // the guide bore, on the post's axis: keeps the insert + iron tip square
        translate([post_dx, post_dy, -0.1]) cylinder(d = insert_d + 0.4, h = guide_h + 0.2);
        // window low in the free +X face, to watch the insert leave the guide into the post
        translate([post_dx, post_dy, 2.5]) rotate([0, 90, 0]) cylinder(d = 3, h = sx);
    }
}

module button_ring() {
    difference() {
        cylinder(d = bez_d, h = ring_t);   // the seat is cut bez_d + 2*tol_slide: tol_slide a side, once (v0.2)
        translate([0, 0, -0.1]) cylinder(d = btn_d + 2*tol_slide + 0.6, h = ring_t + 0.2);
    }
}

if (part == "insert_guide") insert_guide();
else button_ring();
