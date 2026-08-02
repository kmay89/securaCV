// ============================================================================
//  Canary — UNIVERSAL FIT-CHECK COUPON  ⚠️ IN DEVELOPMENT (v0.3-dev)
//  ONE small print that calibrates your printer for the ENTIRE Canary
//  catalog before you commit to any case. Every fit the enclosures use is
//  exercised on a labelled station, laid out on a branded 90 × 68 plate:
//
//    BASE (part="base", rigid) — stations left→right, top→bottom:
//      EMBOSS  — raised "CANARY" wordmark (emboss_h): crisp or blobby?
//      GLYPH   — the bird mark, solid, raised at the same emboss_h: do the
//                wing and tail tapers hold their shape?          -> glyph_h
//      DEBOSS  — sunk "SecuraCV" wordmark (label_depth): every label's fate
//      SLIDE   — lip channel: the MATE's edge tongue slides in snugly -> tol_slide
//      PORT    — the cases' USB-C opening through a case wall: your
//                cable's head must pass                            -> usb_w/usb_h
//      PRESS   — 6 mm magnet pocket + 3 mm light-pipe hole         -> tol_press
//      GROOVE  — straight gasket groove: seat the TPU STRIP        -> gasket fit
//      SCREW   — M2 post (drive a screw ~0.3 N·m) + countersink,
//                plus a −/0/+ pilot ladder: pick the hole that
//                bites without splitting                           -> screw_d
//      CLIP    — the WAP's clip coupon, embedded: snap clips on BOTH long
//                edges of a board channel — press a 1.2 mm PCB edge (or
//                scrap) in and feel the click        -> clip_t/clip_hook/clip_clear
//      POCKET  — two blind keyhole pockets (gap 30): hang the MATE's studs,
//                slide to the CLICK — a detent parks them so the mate
//                doesn't slide back off (the doorbell-plate retention test)
//      INSERT  — heat-set boss (only if you'll use screw_insert)   -> insert_d
//    MATE (part="mate"): two T-studs + a bottom-flush slide tongue on the
//                edge (in-plane, so the stud face stays flat for the hang test)
//    STRIP (part="strip"): TPU gasket bar for the groove
//
//  If a station is tight/loose, adjust the matching tol_* / clip_* / screw
//  parameter in the case you print next. Labels are debossed beside each.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

/* [What to render] */
part = "all";        // ["base","mate","strip","all"]

/* [Print tolerances under test — same trio as every case] */
tol_slide = 0.20;
tol_press = 0.10;
tol_hole  = 0.30;

/* [Interface dims — mirror the case defaults] */
stud_gap = 30.0;
kh_head_d = 7.0;  kh_shank_d = 4.2;  kh_slot_l = 8.0;  kh_head_h = 3.5;  kh_face = 1.0;
kh_click = 0.25;  // detent bump proud of the head-channel ceiling (0 = no click)
clip_w = 6.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;
clip_bw = 17.5;   // the WAP's board width — the CLIP station is its coupon verbatim
pcb_t = 1.2;  standoff_h = 3.0;  standoff_d = 4.0;
screw_d = 1.6;  screw_head_d = 4.0;
screw_step = 0.15;   // pilot ladder: three holes at screw_d − step / screw_d / + step
insert_d = 3.5;  insert_h = 4.0;
mag_d = 6.0;  lp_d = 3.0;
lip_t = 1.2;  lip_h = 4.0;
gasket_w = 1.6;  gasket_groove = 1.2;  gasket_proud = 0.3;   // matches the catalog gasket recipe (20 % squeeze, ~86 % fill)
usb_w = 12.0;  usb_h = 6.5;   // the WAP case's USB-C opening (clears rugged cable boots)
port_wall = 2.0;              // case-typical wall the PORT opening is cut through

/* [Coupon] */
base_t = 5.0;        // base thickness (pockets live in it)
label_depth = 0.5;
emboss_h = 0.6;      // raised-wordmark height (EMBOSS station)
label_font = "Liberation Sans:style=Bold";
brand_raised = "CANARY";     // the EMBOSS station's wordmark
brand_sunk   = "SecuraCV";   // the DEBOSS station's wordmark
glyph_h = 9.0;       // GLYPH station: bird height (0 removes the station)

/* [Quality] */
$fa = 3; $fs = 0.4;

bw = 90; bh = 68;
echo(str("Canary fit coupon v0.3-dev — base ", bw, "x", bh, "  (IN DEVELOPMENT)"));

// ---------------------------------------------------------------------------
//  The bird mark as a SOLID silhouette. The brand artwork is line art — every
//  stroke is a ribbon ~0.08 mm wide even on the 60 mm badge — so the drawing
//  itself cannot be embossed at any size this plate can host. gen_bird_glyph.py
//  lifts the bird's outline out of that art and fills it; the points below are
//  its output, INLINE because this coupon is carried to the website as a single
//  standalone .scad and could not resolve an include or an SVG import.
//      python3 gen_bird_glyph.py art/securacv_bird_glyph.svg --into canary_fit_coupon.scad
// GENERATED-BEGIN bird_glyph
// 324 points, normalised to a 1 mm-tall box centred on
// the origin (width 0.8035 at that height).
// Thinnest solid neck is 0.32 mm at glyph_h = 9 —
// the wing and tail tapers. They round off at a 0.4 mm nozzle
// rather than vanishing; how much they round IS the test.
function bird_glyph_pts() = [
    [0.3332,-0.3699], [0.3249,-0.3647], [0.3208,-0.3606], [0.3166,-0.3585],
    [0.3125,-0.3544], [0.3073,-0.3512], [0.3062,-0.3492], [0.3083,-0.3471],
    [0.3104,-0.3471], [0.3125,-0.3450], [0.3218,-0.3398], [0.3301,-0.3315],
    [0.3322,-0.3274], [0.3353,-0.3242], [0.3426,-0.3242], [0.3436,-0.3232],
    [0.3550,-0.3211], [0.3613,-0.3180], [0.3685,-0.3118], [0.3737,-0.3024],
    [0.3737,-0.2900], [0.3675,-0.2744], [0.3613,-0.2661], [0.3623,-0.2651],
    [0.3644,-0.2651], [0.3654,-0.2661], [0.3727,-0.2672], [0.3737,-0.2682],
    [0.3851,-0.2682], [0.3862,-0.2672], [0.3914,-0.2661], [0.3976,-0.2609],
    [0.4007,-0.2557], [0.4007,-0.2526], [0.4017,-0.2516], [0.4007,-0.2412],
    [0.3997,-0.2402], [0.3986,-0.2350], [0.3966,-0.2319], [0.3966,-0.2298],
    [0.3934,-0.2246], [0.3934,-0.2225], [0.3696,-0.1748], [0.3696,-0.1727],
    [0.3446,-0.1239], [0.3446,-0.1218], [0.3415,-0.1156], [0.3395,-0.1135],
    [0.3249,-0.0844], [0.3187,-0.0751], [0.3166,-0.0699], [0.2896,-0.0284],
    [0.2772,-0.0118], [0.2689,-0.0024], [0.2689,-0.0014], [0.2533,0.0152],
    [0.2543,0.0162], [0.2709,0.0162], [0.2720,0.0173], [0.2761,0.0173],
    [0.2772,0.0194], [0.2574,0.0412], [0.2388,0.0671], [0.2242,0.0941],
    [0.2242,0.0962], [0.2211,0.1014], [0.2211,0.1034], [0.2170,0.1138],
    [0.2170,0.1180], [0.2180,0.1190], [0.2190,0.1273], [0.2201,0.1284],
    [0.2201,0.1336], [0.2211,0.1346], [0.2211,0.1450], [0.2222,0.1460],
    [0.2222,0.1699], [0.2211,0.1709], [0.2211,0.1803], [0.2201,0.1813],
    [0.2201,0.1875], [0.2190,0.1886], [0.2190,0.1927], [0.2170,0.1979],
    [0.2170,0.2021], [0.2128,0.2124], [0.2118,0.2187], [0.2097,0.2218],
    [0.2076,0.2291], [0.2045,0.2342], [0.2055,0.2363], [0.2149,0.2270],
    [0.2170,0.2270], [0.2170,0.2311], [0.2138,0.2394], [0.2138,0.2426],
    [0.2107,0.2488], [0.2076,0.2592], [0.2055,0.2623], [0.2055,0.2644],
    [0.2035,0.2675], [0.2035,0.2695], [0.2014,0.2727], [0.2014,0.2747],
    [0.1993,0.2778], [0.1993,0.2799], [0.1941,0.2913], [0.1941,0.2934],
    [0.1920,0.2965], [0.1920,0.2986], [0.1869,0.3100], [0.1869,0.3121],
    [0.1837,0.3173], [0.1837,0.3194], [0.1651,0.3557], [0.1401,0.3931],
    [0.1350,0.3983], [0.1308,0.4045], [0.1215,0.4138], [0.1215,0.4149],
    [0.0997,0.4356], [0.0986,0.4356], [0.0934,0.4408], [0.0924,0.4408],
    [0.0799,0.4512], [0.0644,0.4616], [0.0561,0.4657], [0.0540,0.4678],
    [0.0280,0.4803], [0.0145,0.4844], [0.0083,0.4875], [0.0052,0.4875],
    [0.0010,0.4896], [-0.0021,0.4896], [-0.0031,0.4907], [-0.0062,0.4907],
    [-0.0073,0.4917], [-0.0104,0.4917], [-0.0156,0.4938], [-0.0249,0.4948],
    [-0.0260,0.4958], [-0.0394,0.4969], [-0.0405,0.4979], [-0.0498,0.4979],
    [-0.0509,0.4990], [-0.0716,0.4990], [-0.0727,0.5000], [-0.0779,0.5000],
    [-0.0789,0.4990], [-0.1090,0.4979], [-0.1100,0.4969], [-0.1235,0.4958],
    [-0.1246,0.4948], [-0.1298,0.4948], [-0.1308,0.4938], [-0.1350,0.4938],
    [-0.1401,0.4917], [-0.1443,0.4917], [-0.1453,0.4907], [-0.1484,0.4907],
    [-0.1526,0.4886], [-0.1557,0.4886], [-0.1599,0.4865], [-0.1630,0.4865],
    [-0.1640,0.4855], [-0.1786,0.4813], [-0.2107,0.4668], [-0.2128,0.4647],
    [-0.2180,0.4626], [-0.2201,0.4606], [-0.2232,0.4595], [-0.2253,0.4574],
    [-0.2346,0.4522], [-0.2512,0.4398], [-0.2564,0.4346], [-0.2574,0.4346],
    [-0.2782,0.4138], [-0.3000,0.3848], [-0.3052,0.3744], [-0.3073,0.3723],
    [-0.3177,0.3505], [-0.3177,0.3484], [-0.3197,0.3453], [-0.3197,0.3432],
    [-0.3249,0.3298], [-0.3270,0.3194], [-0.3280,0.3183], [-0.3280,0.3142],
    [-0.3291,0.3131], [-0.3291,0.3090], [-0.3301,0.3080], [-0.3301,0.3038],
    [-0.3312,0.3028], [-0.3312,0.2965], [-0.3322,0.2955], [-0.3322,0.2851],
    [-0.3332,0.2841], [-0.3332,0.2633], [-0.3322,0.2623], [-0.3322,0.2529],
    [-0.3312,0.2519], [-0.3301,0.2405], [-0.3280,0.2353], [-0.3280,0.2291],
    [-0.3291,0.2280], [-0.3291,0.2239], [-0.3301,0.2228], [-0.3301,0.2187],
    [-0.3312,0.2176], [-0.3312,0.2135], [-0.3322,0.2124], [-0.3332,0.1990],
    [-0.3343,0.1979], [-0.3343,0.1906], [-0.3353,0.1896], [-0.3353,0.1522],
    [-0.3343,0.1512], [-0.3343,0.1450], [-0.3332,0.1439], [-0.3332,0.1398],
    [-0.3322,0.1387], [-0.3322,0.1346], [-0.3312,0.1336], [-0.3301,0.1263],
    [-0.3291,0.1252], [-0.3270,0.1169], [-0.3249,0.1138], [-0.3249,0.1118],
    [-0.3187,0.0993], [-0.3104,0.0879], [-0.3052,0.0837], [-0.3052,0.0816],
    [-0.3083,0.0754], [-0.3083,0.0733], [-0.3156,0.0598], [-0.3291,0.0432],
    [-0.3291,0.0422], [-0.3405,0.0297], [-0.3498,0.0162], [-0.3592,-0.0035],
    [-0.3592,-0.0066], [-0.3613,-0.0107], [-0.3613,-0.0159], [-0.3623,-0.0170],
    [-0.3623,-0.0190], [-0.3602,-0.0211], [-0.3571,-0.0211], [-0.3561,-0.0222],
    [-0.3654,-0.0325], [-0.3737,-0.0450], [-0.3820,-0.0616], [-0.3820,-0.0637],
    [-0.3862,-0.0720], [-0.3862,-0.0741], [-0.3924,-0.0907], [-0.3924,-0.0938],
    [-0.3934,-0.0948], [-0.3934,-0.0979], [-0.3945,-0.0990], [-0.3945,-0.1021],
    [-0.3966,-0.1073], [-0.3976,-0.1166], [-0.3986,-0.1177], [-0.3997,-0.1312],
    [-0.4007,-0.1322], [-0.4007,-0.1426], [-0.4017,-0.1436], [-0.4017,-0.1644],
    [-0.4007,-0.1654], [-0.4007,-0.1768], [-0.3997,-0.1779], [-0.3997,-0.1841],
    [-0.3986,-0.1851], [-0.3966,-0.1997], [-0.3955,-0.2007], [-0.3934,-0.2111],
    [-0.3924,-0.2121], [-0.3893,-0.2236], [-0.3851,-0.2319], [-0.3851,-0.2339],
    [-0.3748,-0.2547], [-0.3644,-0.2703], [-0.3571,-0.2786], [-0.3571,-0.2796],
    [-0.3436,-0.2931], [-0.3426,-0.2931], [-0.3343,-0.3004], [-0.3249,-0.3066],
    [-0.3177,-0.3097], [-0.3156,-0.3118], [-0.3083,-0.3263], [-0.3062,-0.3284],
    [-0.3052,-0.3315], [-0.2938,-0.3492], [-0.2907,-0.3523], [-0.2855,-0.3606],
    [-0.2772,-0.3699], [-0.2772,-0.3710], [-0.2606,-0.3886], [-0.2606,-0.3897],
    [-0.2325,-0.4156], [-0.2315,-0.4156], [-0.2180,-0.4270], [-0.2138,-0.4291],
    [-0.2107,-0.4322], [-0.1952,-0.4426], [-0.1900,-0.4447], [-0.1827,-0.4499],
    [-0.1734,-0.4540], [-0.1713,-0.4561], [-0.1609,-0.4613], [-0.1588,-0.4613],
    [-0.1516,-0.4654], [-0.1495,-0.4654], [-0.1443,-0.4685], [-0.1422,-0.4685],
    [-0.1339,-0.4727], [-0.1256,-0.4748], [-0.1235,-0.4769], [-0.1287,-0.4955],
    [-0.1308,-0.4976], [-0.1308,-0.4997], [-0.1311,-0.5000], [-0.0489,-0.5000],
];
// GENERATED-END bird_glyph
// ---------------------------------------------------------------------------

module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module lbl(x, y, s, size = 4.5) { translate([x, y, base_t - label_depth]) linear_extrude(label_depth + 0.1)
    text(s, size = size, font = label_font, halign = "center", valign = "center"); }
module emb(x, y, s, size = 7) { translate([x, y, base_t - 0.05]) linear_extrude(emboss_h + 0.05)
    text(s, size = size, font = label_font, halign = "center", valign = "center"); }
// GLYPH station: the bird raised at the SAME height the cases' brand marks use
// (emboss_h), so the wordmark beside it and the bird ask the same question two
// ways. Text has stroke width to spare; a silhouette with tapering wing and
// tail tips does not — which is exactly why it earns its own station.
module bird_emb(px, py, h) {
    translate([px, py, base_t - 0.05]) linear_extrude(emboss_h + 0.05)
        polygon(points = [for (p = bird_glyph_pts()) [p[0]*h, p[1]*h]]);
}
module keyhole_pocket(px, py) {         // blind, from the back (z=0 face)
    translate([px, py, 0]) union() {
        translate([0, -kh_slot_l/2, -0.1]) linear_extrude(kh_face + 0.1) {
            circle(d = kh_head_d);
            hull() { circle(d = kh_shank_d); translate([0, kh_slot_l]) circle(d = kh_shank_d); }
        }
        translate([0, -kh_slot_l/2, kh_face]) linear_extrude(kh_head_h - kh_face)
            hull() { circle(d = kh_head_d + 0.6); translate([0, kh_slot_l]) circle(d = kh_head_d + 0.6); }
    }
}
// The WAP's boardclip profile verbatim, rooted on the coupon face: beam +
// inward hook + 45° lead-in. sy = +1/-1 picks which long edge of the CLIP
// station's board channel the clip guards — one on EACH side, like the case.
module stationclip(cx, cy, sy) {
    bt = base_t + standoff_h + pcb_t;  tp = bt + clip_hook_h;
    pts = [ [clip_clear, base_t], [clip_clear + clip_t, base_t],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([cx, cy + sy*clip_bw/2, 0]) scale([1, sy, 1])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
}
// POCKET click detent: a shallow dome on the head-channel ceiling at slot
// mid-travel — cleared during stud insertion, cammed over on the slide, and
// the head parks BEHIND it (kh_click 0.25 against the head's 0.1 ceiling
// clearance = 0.15 squeeze). Slide on, CLICK, stays until a firm pull back.
module keyhole_click(px, py) {
    translate([px, py, kh_head_h]) scale([0.9, 0.9, kh_click]) sphere(1);
}

// Station anchors — one grid, every label gets clear air.
// Brand band y≈+27 · row 1 y≈+16 · row 2 y≈+1 · row 3 y≈−14 · foot y≈−30
pocket_cx = -1;                       // keyholes at pocket_cx ± stud_gap/2
ladder_x  = [31, 36, 41];             // SCREW pilot ladder columns
ladder_d  = [screw_d - screw_step, screw_d, screw_d + screw_step];
ladder_tag = ["-", "0", "+"];

module base() {
    union() {
        difference() {
            linear_extrude(base_t) rrect2d(bw, bh, 4);
            // DEBOSS station: the sunk wordmark IS the test (depth = label_depth,
            // the same cut every label on every case gets)
            lbl(22, 27, brand_sunk, 7);
            // SLIDE station: female channel for the mate's lip rib
            translate([-24, 16, base_t - lip_h + 1]) linear_extrude(lip_h)
                rrect2d(30 + 2*tol_slide, lip_t + 2*tol_slide, 0.3);
            // PORT station: the WAP's USB-C opening through a case-typical wall —
            // counterbored from the back so the web is port_wall thick, like a case.
            // The through profile is the WAP's port VERBATIM (square lower corners,
            // 45°-chamfered top corners) so a boot that passes here passes the case.
            translate([2, 16, -0.1]) linear_extrude(base_t - port_wall + 0.1)
                rrect2d(usb_w + 6, usb_h + 5, 2);
            translate([2, 16, -0.1]) linear_extrude(base_t + 0.2)
                polygon([[-usb_w/2, -usb_h/2], [usb_w/2, -usb_h/2],
                         [usb_w/2, usb_h/2 - 2.5], [usb_w/2 - 2.5, usb_h/2],
                         [-usb_w/2 + 2.5, usb_h/2], [-usb_w/2, usb_h/2 - 2.5]]);
            // PRESS station: magnet pocket + light-pipe hole
            translate([26, 16, base_t - 2.2]) cylinder(d = mag_d + 2*tol_press, h = 2.3);   // matches mag_h 2.2
            translate([35, 16, -0.1]) cylinder(d = lp_d + 2*tol_press, h = base_t + 0.2);
            // GROOVE station: straight gasket groove
            translate([-24, 1, base_t - gasket_groove]) linear_extrude(gasket_groove + 0.1)
                rrect2d(30, gasket_w, 0.3);
            // SCREW station: countersunk clearance hole (lid side of the joint)...
            translate([23, 1, -0.1]) cylinder(d = screw_d + 2*tol_hole, h = base_t + 0.2);
            translate([23, 1, base_t - 1.6]) cylinder(d1 = screw_d + 2*tol_hole, d2 = screw_head_d, h = 1.7);
            // ...plus the −/0/+ pilot ladder: blind holes, drive the screw into each
            for (i = [0:2]) translate([ladder_x[i], 1, base_t - 4]) cylinder(d = ladder_d[i], h = 4.1);
            // POCKET station: keyhole pocket pair for the mate's studs (gap = stud_gap)
            keyhole_pocket(pocket_cx - stud_gap/2, -14);
            keyhole_pocket(pocket_cx + stud_gap/2, -14);
            // labels — grid-aligned, none touching a neighbour
            lbl(-24, 21, "EMBOSS", 3);  lbl(-3, 21, "GLYPH", 3);  lbl(22, 21, "DEBOSS", 3);
            lbl(-24, 9.5, "SLIDE");     lbl(2, 9.5, "PORT");     lbl(30, 9.5, "PRESS");
            lbl(-24, -4.5, "GROOVE", 3);   lbl(19, -5.5, "SCREW");
            for (i = [0:2]) lbl(ladder_x[i], -4.5, ladder_tag[i], 3);   // on the SCREW line, under their holes
            lbl(-31, -16.5, "CLIP", 3);    // on the channel floor, between the rails
            lbl(pocket_cx, -25.5, "POCKET");  lbl(34, -21, "INSERT");
            // foot line: the small-text legibility test doubles as the colophon
            lbl(-2, -31, "securacv.com · fit coupon v0.3-dev", 3);
        }
        // EMBOSS station: raised wordmark at the height case branding uses
        emb(-24, 27, brand_raised, 7);
        // GLYPH station: the bird, same emboss_h, in the clear air between the
        // two wordmarks (CANARY ends ≈−11.5, SecuraCV starts ≈+5)
        if (glyph_h > 0) bird_emb(-3, 27, glyph_h);
        // CLIP station — the WAP clip coupon, both sides: board-rest rails flush
        // to the board's edge lines and a snap clip on EACH long edge. Press a
        // 1.2 mm scrap (or real board edge) past the lead-ins and feel the click.
        for (sy = [1, -1]) {
            translate([-37, -16.5 + sy*(clip_bw/2 - standoff_d/2) - standoff_d/2, base_t - 0.01])
                cube([12, standoff_d, standoff_h + 0.01]);
            stationclip(-31, -16.5, sy);
        }
        // POCKET station: the click detents, one per keyhole
        if (kh_click > 0) for (s = [1, -1]) keyhole_click(pocket_cx + s*stud_gap/2, -14);
        // SCREW station: M2 self-tap post
        translate([14, 1, base_t - 0.01]) difference() {
            cylinder(d = 5, h = 8);
            translate([0, 0, 1.5]) cylinder(d = screw_d, h = 8);
        }
        // INSERT station: heat-set boss
        translate([34, -14, base_t - 0.01]) difference() {
            cylinder(d = insert_d + 2.4, h = insert_h + 2);
            translate([0, 0, 2]) cylinder(d = insert_d - 0.1, h = insert_h + 2.1);
        }
    }
}

module mate() {
    union() {
        difference() {
            linear_extrude(3) rrect2d(46, 20, 3);
            translate([0, -6.5, 3 - label_depth]) linear_extrude(label_depth + 0.1)
                text("CANARY MATE", size = 3.2, font = label_font, halign = "center", valign = "center");
        }
        // T-studs at stud_gap (hang on the base's pockets, slide down)
        for (s = [1, -1]) translate([s*stud_gap/2, 0, 3 - 0.01]) {
            cylinder(d = 4.0, h = kh_face + 0.4);
            translate([0, 0, kh_face + 0.4]) cylinder(d1 = 4.0, d2 = 6.6, h = 1.2);
            translate([0, 0, kh_face + 1.6]) cylinder(d = 6.6, h = 0.8);
        }
        // slide tongue — bottom-flush on the +y EDGE, in the plate's plane, so
        // the stud face stays dead flat and the hang test seats fully (the v0.2
        // face rib propped the mate 3 mm off the base). Hold the mate on edge
        // and push the tongue into the SLIDE channel: same lip_t vs tol_slide
        // gauge, and at 3 mm proud it bottoms out flush with the coupon face.
        translate([-15, 10 - 0.01, 0]) cube([30, lip_h - 1 + 0.01, lip_t]);
    }
}

module strip() {   // TPU gasket bar for the GROOVE station
    linear_extrude(gasket_groove + gasket_proud) rrect2d(29.4, gasket_w - 0.5, 0.3);   // strip 0.5 narrower, like the case gaskets
}

if      (part == "base")  base();
else if (part == "mate")  mate();
else if (part == "strip") strip();
else { base(); translate([-12, -50, 0]) mate(); translate([28, -50, 0]) strip(); }
