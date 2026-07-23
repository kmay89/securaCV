// ============================================================================
//  Canary — FIELD CASE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  A GoPro-class rugged witness: XIAO ESP32-S3 Sense + LiPo in a throw-in-
//  your-bag brick. Design-intent rating: CER-4 / Field — IP67 (submersion,
//  1 m / 30 min) + MIL-STD-810H 516.8 Procedure IV transit drop (1.22 m,
//  boot fitted). A rating is EARNED per printed unit by running the test
//  protocol in field_ratings.md — never assumed from the CAD.
//
//  Why this can honestly aim at IP67 where the other cases can't:
//    - ZERO external penetrations except the bonded lens disc and a Ø3 mm
//      vent hole backed by an adhesive ePTFE membrane (equalises pressure
//      so thermal cycling can't pump moisture past the seal). No external
//      USB: open the case to charge/flash — the biggest leak path deleted.
//    - A real Ø1.5 mm O-ring CORD in a machined-style groove on the body
//      rim (27 % squeeze), not a printed TPU gasket.
//    - Six M3 heat-set screw lobes (Pelican-style), worst unsupported seal
//      span ~41 mm (end lobe -> mid-side lobe along the seal line) — at the
//      top of the ~40 mm clamp-spacing rule; verify compression in W-3.
//    - 4 mm walls everywhere; the electronics never touch the shell —
//      board on snap-clips, battery on a foam bed.
//    - A TPU impact BOOT takes the drops; the shell takes the water.
//
//  Parts: body / lid / boot (TPU) / gasket (printed-TPU fallback ring) /
//         bezel (contrast lens trim ring — hides the disc bond line) / all
//  Print: body + lid in ASA (outdoor) or PETG, 5-6 perimeters, 40 % gyroid,
//  0.2 mm layers, flat as modelled — no supports. Boot: TPU 95A, open up.
//  Hardware: 6x M3 heat-set insert (Ø4.6) + M3x8 pan head; Ø1.5 O-ring cord
//  (~200 mm, nitrile/silicone); Ø12x2 polycarbonate lens disc, silicone-
//  bonded; 1x adhesive ePTFE vent patch (Ø10+); closed-cell foam pad.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print- or dunk-validated.
// ============================================================================

/* [What to render] */
part = "all";        // ["body","lid","boot","gasket","bezel","all"]

/* [Board] — XIAO ESP32-S3 Sense, camera stack up, USB toward +X wall */
board_l = 21.0;  board_w = 17.5;  board_h = 1.2;
stack_h = 6.5;       // Sense expansion + camera height above the base PCB
cam_dx = 0.0;        // lens centre offset from board centre
cam_dy = 0.0;

/* [Battery] — MEASURE yours; 503035 (500 mAh) default. LiPo: never charge below 0 C */
batt_l = 35.0;  batt_w = 30.0;  batt_h = 5.0;
foam_t = 2.0;        // closed-cell foam bed under + over the pack
batt_pad = 1.0;

/* [Print tolerances] — tune with canary_fit_coupon.scad */
tol_slide = 0.20;  tol_press = 0.10;  tol_hole = 0.30;

/* [Shell] — 4 mm everywhere is the field-grade floor, not a suggestion.
   floor_t is 4.5 so the keyhole pockets reach the ecosystem-standard 3.5 mm
   depth (the shared T-stud is 3.4 mm tall) while keeping a 1.0 mm ceiling. */
wall_t  = 4.0;
floor_t = 4.5;
lid_t   = 4.0;
r_in    = 3.0;       // cavity corner radius
head_clear = 1.8;    // clearance above the camera stack

/* [O-ring seal] — Ø1.5 cord in a body-rim groove, 27 % squeeze */
oring_d = 1.5;
grv_w   = 2.0;       // groove width
grv_d   = 1.1;       // groove depth (squeeze = 1 - grv_d/oring_d)
g_c     = 2.0;       // groove centreline offset outboard of the cavity edge

/* [Screw lobes] — 6x M3 heat-set inserts, outboard of the seal line */
lob_d    = 9.0;
lob_off  = 8.0;      // lobe centre offset outboard of the cavity edge (keeps the lanyard bore >=1.5 mm off the pressure wall)
end_lob_y = 8.0;     // Y of the two lobes on each short end
insert_d = 4.6;  insert_h = 6.0;   // M3 heat-set pocket
screw_c_d = 3.4;                    // lid clearance hole
cb_d = 6.4;  cb_h = 2.0;            // pan-head counterbore

/* [Lens window] — Ø12x2 polycarbonate disc, bonded from outside */
disc_d = 12.0;  disc_t = 2.0;
ap_d   = 8.0;        // aperture through the web behind the disc

/* [Vent] — Ø3 hole + adhesive ePTFE patch on the INSIDE face */
vent_d = 3.0;
vent_x = -15.0;  vent_y = 6.0;

/* [Keyhole mounts] — blind, seal-safe (never reach the cavity) */
kh_x = 18.0;         // +/- X of the two keyholes
kh_head_d = 8.0;  kh_shank_d = 4.2;  kh_slot_l = 8.0;   // Ø8 passes a #8 pan head too, not just the T-stud
kh_head_h = 3.5;  kh_face = 1.0;    // 3.5 = catalog standard; fits the 3.4 mm T-stud

/* [Lanyard] — paracord bore through the two -X lobes */
lan_d = 4.5;  lan_z = 5.0;

/* [Lid alignment lip] — shear key inboard of the seal */
lip_w = 1.5;  lip_h = 2.0;

/* [Bezel accent] — contrast-color trim ring hiding the lens bond line */
bez_on = true;
bez_o = 19.0;        // bezel outer Ø (= recess Ø in the lid face)
bez_i = 10.5;        // bezel opening (overlaps the disc edge by ~0.75/side)
bez_recess = 1.0;    // recess depth in the lid face
bez_proud = 0.8;     // stand above the face = the inner lip thickness (>= 0.6);
                     // doubles as a sacrificial lens hood on face drops

/* [Boot] — TPU 95A impact boot; earns the drop rating */
boot_w    = 2.5;     // boot wall
boot_floor = 2.5;
boot_fit  = 0.2;     // stretch interference
grip_grooves = 3;

/* [Board cradle] */
standoff_h = 3.0;
clip_w = 8.0;  clip_t = 1.5;  clip_hook = 0.8;  clip_hook_h = 1.2;  clip_clear = 0.25;

/* [Divider] */
rib_t = 2.0;  rib_h = 8.0;  notch_w = 6.0;  notch_d = 4.0;

/* [Aesthetics] */
label_text = "CANARY";
label_size = 5.0;  label_depth = 0.5;
label_dx = -12.0;  label_dy = -7.0;
label_font = "Liberation Sans:style=Bold";
lid_edge = 0.64;     // stepped edge chamfer on the lid face

/* [Quality] */
$fa = 3; $fs = 0.4;

// ---- derived --------------------------------------------------------------
board_zone = board_l + 4.5;                        // board + clip room
inner_l = batt_l + batt_pad + rib_t + board_zone;
inner_w = max(batt_w + 1.0, board_w + 9.0);
cav_h   = standoff_h + board_h + stack_h + head_clear;
base_h  = floor_t + cav_h;
out_l   = inner_l + 2*wall_t;
out_w   = inner_w + 2*wall_t;
r_out   = r_in + wall_t;
total_h = base_h + lid_t;
bcx     = inner_l/2 - board_zone/2;                // board centre X
x_div   = -inner_l/2 + batt_l + batt_pad + rib_t/2;
lens_x  = bcx + cam_dx;  lens_y = cam_dy;

function lobes() = [
    [ (inner_l/2 + lob_off),  end_lob_y], [ (inner_l/2 + lob_off), -end_lob_y],
    [-(inner_l/2 + lob_off),  end_lob_y], [-(inner_l/2 + lob_off), -end_lob_y],
    [0,  (inner_w/2 + lob_off)], [0, -(inner_w/2 + lob_off)] ];

// O-ring engineering checks
squeeze = 1 - grv_d/oring_d;
grv_fill = (PI/4*oring_d*oring_d) / (grv_w*grv_d);
grv_R = r_in + g_c;
cord_len = 2*(inner_l + 2*g_c) + 2*(inner_w + 2*g_c) - 8*grv_R + 2*PI*grv_R;

assert(squeeze >= 0.15 && squeeze <= 0.40, "O-ring squeeze must be 15-40% — adjust grv_d/oring_d");
assert(grv_fill <= 0.90, "O-ring groove over-filled (>90%) — widen grv_w");
assert(g_c - grv_w/2 >= 0.8, "inner groove cheek < 0.8 mm");
assert(wall_t - g_c - grv_w/2 >= 0.8, "outer groove cheek < 0.8 mm — thicken wall_t");
assert(lid_t - disc_t - 0.2 >= 1.5, "lens web < 1.5 mm — thicken lid_t or thin disc_t");
assert(!bez_on || bez_recess < disc_t, "bezel recess deeper than the disc pocket makes the disc unseatable");
assert(!bez_on || (bez_o >= disc_d + 4 && bez_i < disc_d), "bezel must overlap the disc edge and clear the pocket");
assert(!bez_on || lid_t - bez_recess >= 1.8, "bezel recess leaves < 1.8 mm lid web");
assert(!bez_on || bez_proud >= 0.6, "bezel lip (= bez_proud) too thin to print");
assert(lid_t - cb_h >= 1.8, "counterbore leaves < 1.8 mm lid web");
assert(floor_t - kh_head_h >= 1.0, "keyhole pocket breaches the floor — seal-unsafe");
assert(lob_off - wall_t - lan_d/2 >= 1.5, "lanyard bore too close to the pressure wall (< 1.5 mm surround) — raise lob_off");
assert(insert_h + 1.5 <= base_h, "insert pocket too deep");
assert(end_lob_y + lob_d/2 <= inner_w/2 - r_in, "end lobes ride onto the corner radius");
assert(wall_t >= 2.4 && floor_t >= 2.4 && lid_t >= 2.4, "field case needs >= 2.4 mm shell minimum");

echo(str("Canary FIELD case v0.1-dev — outer ", out_l, " x ", out_w, " x ", total_h,
         " (+boot ", out_l + 2*(boot_w + lob_off + lob_d/2 - wall_t), " wide at lobes)"));
echo(str("O-ring: Ø", oring_d, " cord, cut ~", round(cord_len), " mm, squeeze ",
         round(squeeze*100), "%, groove fill ", round(grv_fill*100), "%"));
echo("CER-4 design intent: IP67 + MIL-STD-810H 516.8 IV (boot on) — VERIFY per field_ratings.md");
if (wall_t < 4.0 || floor_t < 4.0 || lid_t < 4.0)
    echo("NOTE: shell thinner than 4.0 mm — CER-4 submersion intent assumes 4.0 mm");

// ---- shared 2D ------------------------------------------------------------
module rrect2d(l, w, r) { offset(r = r) offset(r = -r) square([l, w], center = true); }
module cav2d() { rrect2d(inner_l, inner_w, r_in); }
module outline2d() {
    rrect2d(out_l, out_w, r_out);
    // each lobe is hulled toward a copy pulled 18 % back into the wall — a
    // gusset neck; a bare tangent circle hangs on a ~0.5 mm-deep sliver, the
    // classic Z-weak FDM crack line under clamp preload / drop loads
    for (p = lobes()) hull() {
        translate(p) circle(d = lob_d);
        translate([p[0]*0.82, p[1]*0.82]) circle(d = lob_d);
    }
}
module grv_ring2d() {
    difference() {
        offset(r = g_c + grv_w/2) cav2d();
        offset(r = g_c - grv_w/2) cav2d();
    }
}

module teardrop2d(r) {
    circle(r = r);
    polygon([[-r*cos(45), r*sin(45)], [0, min(r + 0.75, r*1.38)], [r*cos(45), r*sin(45)]]);
}
module tearbore_y(px, pz, d, len) {   // horizontal bore along Y, teardrop up
    translate([px, len/2, pz]) rotate([90, 0, 0]) linear_extrude(len) teardrop2d(d/2);
}

// ---- interior furniture ----------------------------------------------------
module edgeclip(px, py, ang) {
    bt = floor_t + standoff_h + board_h;  tp = bt + clip_hook_h;
    pts = [ [clip_clear, floor_t], [clip_clear + clip_t, floor_t],
            [clip_clear + clip_t, tp], [clip_clear, tp],
            [-clip_hook, bt], [0, bt], [clip_clear, bt - clip_clear] ];
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        translate([-clip_w/2, 0, 0]) rotate([0, 0, 90]) rotate([90, 0, 0])
            linear_extrude(clip_w) polygon(pts);
}

module keyhole_pocket(xc) {            // opens on the bottom face, blind
    x0 = -kh_slot_l/2;  x1 = kh_slot_l/2;
    translate([xc, 0, 0]) {
        translate([0, 0, -0.1]) linear_extrude(kh_face + 0.1) {
            translate([x0, 0]) circle(d = kh_head_d);
            hull() { translate([x0, 0]) circle(d = kh_shank_d);
                     translate([x1, 0]) circle(d = kh_shank_d); }
        }
        translate([0, 0, kh_face]) linear_extrude(kh_head_h - kh_face)
            hull() { translate([x0, 0]) circle(d = kh_head_d + 0.6);
                     translate([x1, 0]) circle(d = kh_head_d + 0.6); }
    }
}

// ---- parts ------------------------------------------------------------------
module body() {
    difference() {
        linear_extrude(base_h) outline2d();
        // cavity
        translate([0, 0, floor_t]) linear_extrude(cav_h + 0.1) cav2d();
        // O-ring groove in the rim top
        translate([0, 0, base_h - grv_d]) linear_extrude(grv_d + 0.2) grv_ring2d();
        // heat-set insert pockets in the lobe tops
        for (p = lobes())
            translate([p[0], p[1], base_h - insert_h]) cylinder(d = insert_d, h = insert_h + 0.1);
        // blind keyhole pockets (bottom face; slot slides toward +X)
        keyhole_pocket(kh_x);
        keyhole_pocket(-kh_x);
        // lanyard bore through both -X lobes (clears the pressure wall)
        tearbore_y(-(inner_l/2 + lob_off), lan_z, lan_d, 2*end_lob_y + lob_d + 5);
    }
    // interior furniture — union AFTER the cuts so nothing erases it
    for (s = [1, -1])
        translate([bcx - (board_l - 1)/2, s*(board_w/2 - 1.5) - 1.5, floor_t - 0.01])
            cube([board_l - 1, 3, standoff_h + 0.01]);
    edgeclip(bcx,  board_w/2,  90);
    edgeclip(bcx, -board_w/2, 270);
    difference() {                     // battery divider with a wire notch
        translate([x_div - rib_t/2, -inner_w/2 + 0.5, floor_t - 0.01])
            cube([rib_t, inner_w - 1.0, rib_h + 0.01]);
        translate([x_div - rib_t/2 - 0.1, inner_w/2 - 2 - notch_w, floor_t + rib_h - notch_d])
            cube([rib_t + 0.2, notch_w, notch_d + 0.1]);
    }
}

module lid() {                          // z=0 is the OUTER face; print face-down
    difference() {
        union() {
            // bullnose face edge (6-step quarter-round; prints face-down,
            // kills elephant foot and reads "moulded" instead of "printed")
            for (k = [0:5])
                translate([0, 0, k*lid_edge/6]) linear_extrude(lid_edge/6 + 0.01)
                    offset(r = -(lid_edge - lid_edge*sin(90*(k + 1)/6))) outline2d();
            translate([0, 0, lid_edge]) linear_extrude(lid_t - lid_edge) outline2d();
            // alignment/shear lip, inboard of the seal line
            translate([0, 0, lid_t - 0.01]) linear_extrude(lip_h + 0.01) difference() {
                offset(r = -tol_slide) cav2d();
                offset(r = -tol_slide - lip_w) cav2d();
            }
        }
        // screw holes + pan-head counterbores
        for (p = lobes()) translate([p[0], p[1], 0]) {
            translate([0, 0, -0.1]) cylinder(d = cb_d, h = cb_h + 0.1);
            translate([0, 0, -0.1]) cylinder(d = screw_c_d, h = lid_t + 0.2);
        }
        // lens: disc pocket (disc lands 0.2 sub-flush) + aperture
        translate([lens_x, lens_y, -0.1]) cylinder(d = disc_d + 2*tol_slide, h = disc_t + 0.3);
        translate([lens_x, lens_y, -0.1]) cylinder(d = ap_d, h = lid_t + 0.2);
        // bezel recess around the lens (press-fit trim ring hides the bond line)
        if (bez_on)
            translate([lens_x, lens_y, -0.1]) cylinder(d = bez_o, h = bez_recess + 0.1);
        // vent hole (ePTFE patch adheres to the flat INNER face around it —
        // spot-face the INNER side; an outer recess would expose the membrane
        // to rain, UV and pokes)
        translate([vent_x, vent_y, -0.1]) cylinder(d = vent_d, h = lid_t + 0.2);
        translate([vent_x, vent_y, lid_t - 0.9]) cylinder(d = vent_d + 2.4, h = 1.0);
        // debossed label on the outer face (mirrored: read from outside)
        if (label_text != "")
            translate([label_dx, label_dy, -0.1]) linear_extrude(label_depth + 0.1)
                mirror([1, 0, 0]) text(label_text, size = label_size, font = label_font,
                                       halign = "center", valign = "center");
    }
}

module gasket() {                       // printed-TPU fallback if you skip the cord
    linear_extrude(grv_d + 0.6) difference() {
        offset(r = g_c + (grv_w - 0.2)/2) cav2d();
        offset(r = g_c - (grv_w - 0.2)/2) cav2d();
    }
}

module bezel() {                        // contrast-color lens trim ring
    // z0 = the face that lands on the recess floor; print as modelled.
    // The inner lip floats 0.2 mm above the bonded disc and hides the
    // silicone bond line — the detail every consumer camera trim ring exists for.
    difference() {
        cylinder(d = bez_o - 2*tol_press, h = bez_recess + bez_proud);
        translate([0, 0, -0.1]) cylinder(d = bez_i, h = bez_recess + bez_proud + 0.2);
        // relief over the disc: the disc top sits (bez_recess - 0.2) above z0,
        // so a relief of height bez_recess leaves 0.2 clearance and makes the
        // inner lip exactly bez_proud thick
        translate([0, 0, -0.1]) cylinder(d = disc_d + 0.6, h = bez_recess + 0.1);
    }
}

module boot() {                         // TPU 95A; print open-side-up
    bh = boot_floor + total_h + 0.6;
    difference() {
        linear_extrude(bh) offset(r = boot_w - boot_fit) outline2d();
        // case pocket (stretch fit)
        translate([0, 0, boot_floor]) linear_extrude(total_h + 1.0) offset(r = -boot_fit) outline2d();
        // keyhole windows in the floor
        for (s = [1, -1])
            translate([s*kh_x, 0, -0.1]) linear_extrude(boot_floor + 0.2)
                rrect2d(kh_slot_l + kh_head_d + 8, kh_head_d + 8, 3);
        // lanyard window on the -X face
        translate([-(out_l/2 + lob_off + boot_w + 4), -(end_lob_y + lob_d/2 + 2),
                   boot_floor + lan_z - 3.5])
            cube([lob_off + boot_w + 6, 2*(end_lob_y + lob_d/2 + 2), 8]);
        // grip grooves
        for (k = [0:grip_grooves - 1])
            translate([0, 0, boot_floor + 4 + k*5]) linear_extrude(1.4) difference() {
                offset(r = boot_w - boot_fit + 0.1) outline2d();
                offset(r = boot_w - boot_fit - 0.8) outline2d();
            }
    }
    // retention lip: snaps over the lid's chamfered edge
    translate([0, 0, bh - 1.2]) linear_extrude(1.2) difference() {
        offset(r = -boot_fit + 0.3) outline2d();
        offset(r = -boot_fit - 0.8) outline2d();
    }
}

// ---- part selector -----------------------------------------------------------
if      (part == "body")   body();
else if (part == "lid")    lid();
else if (part == "boot")   boot();
else if (part == "gasket") gasket();
else if (part == "bezel")  bezel();
else {
    body();
    translate([0, out_w + 2*lob_off + 16, 0]) lid();
    translate([0, -(out_w + 2*lob_off + 22), 0]) boot();
    translate([out_l + 2*lob_off + 20, 0, 0]) gasket();
    if (bez_on) translate([out_l + 2*lob_off + 20, inner_w + 10, 0]) bezel();
}
