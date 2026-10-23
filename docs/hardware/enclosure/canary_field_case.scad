// ============================================================================
//  Canary — FIELD CASE  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  A GoPro-class rugged witness: XIAO ESP32-S3 Sense + LiPo in a throw-in-
//  your-bag brick. Design-intent rating: CER-4 / Field — IP67 (submersion,
//  1 m / 30 min) + MIL-STD-810H 516.8 Procedure IV transit drop (1.22 m,
//  boot fitted). A rating is EARNED per printed unit by running the test
//  protocol in field_ratings.md — never assumed from the CAD.
//  @env cer=4 ip="IP67" mil="MIL-STD-810H" note="design intent — untested; do not claim before the field_ratings.md protocol"
//
//  Why this can honestly aim at IP67 where the other cases can't:
//    - ZERO external penetrations except the bonded lens disc and a Ø3 mm
//      vent hole backed by an adhesive ePTFE membrane (equalizes pressure
//      so thermal cycling can't pump moisture past the seal). No external
//      USB: open the case to charge/flash — the biggest leak path deleted.
//    - A real Ø1.5 mm O-ring CORD in a machined-style groove on the body
//      rim (27 % squeeze), not a printed TPU gasket.
//    - Eight M3 heat-set screw lobes (Pelican-style), worst unsupported seal
//      span ~38 mm (between the two lobes on a long side, measured along the
//      seal line and asserted under the ~40 mm clamp-spacing rule; the six-lobe
//      layout ran 47 mm end lobe -> mid-side lobe); verify compression in W-3.
//    - 4 mm walls everywhere; the electronics never touch the shell —
//      board on snap-clips, battery on a foam bed.
//    - A TPU impact BOOT takes the drops; the shell takes the water.
//
//  Parts: body / lid / boot (TPU) / gasket (printed-TPU fallback ring) /
//         bezel (contrast lens trim ring — hides the disc bond line) / all
//  Print: body + lid in ASA (outdoor) or PETG, 5-6 perimeters, 40 % gyroid,
//  0.2 mm layers, flat as modeled — no supports. Boot: TPU 95A, open up.
//  Hardware: 8x M3 heat-set insert (Ø4.6) + M3x8 pan head; Ø1.5 O-ring cord
//  (~200 mm, nitrile/silicone); Ø12x2 polycarbonate lens disc, silicone-
//  bonded; 1x adhesive ePTFE vent patch (Ø10+); closed-cell foam pad.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print- or dunk-validated.
//
//  v0.1-dev (2026-08-23): canary_*_lib adoption — and one deliberate geometry
//  change. The board clips shipped clip_t/clip_hook = 1.5/0.8 on a 4.2 mm
//  beam (standoff_h + board_h): ~10 % insertion strain, the exact pair the
//  WAP warns cracks vertical-print PETG. A case that is dropped BY DESIGN was
//  carrying the catalog's worst clips. Re-engineered through snap_boardclip's
//  strain gate: 1.0/0.5 (~4.3 % against the 4.5 % once-budget, the WAP's
//  print-proven pair) on this file's 8.0 mm tabs — the catalog's widest, and
//  the honest place to buy retention, since holding force scales with tab
//  width while strain does not. The lanyard bore roof is now the library
//  teardrop (flat 45° crown, not the pointed local cap). All other meshes
//  are unchanged.
//
//  2026-09-24 (v0.1-dev): the printed-TPU fallback ring filled 139 % of its
//  groove (it now prints 1.4 x 1.4, ~89 %, asserted by core_gasket_fill); the
//  lid seated turned 180° (six symmetric lobes, off-center lens) — it now
//  carries the house key (lid_key_rib / lid_key_slot); the two mid-side lobes
//  are four at x = ±inner_l/4, taking the worst seal span from 47 to ~38 mm;
//  inserts are 7.0 deep so the M3 x 8 does not bottom out.
//
//  2026-10-23 (v0.1-dev): FIT + PRINT REVIEW — every field mesh moves.
//  THE LID CLOSES OVER THE CAMERA: stack_h was 6.5 against the registry's
//  12.7 lens height (brd_xiao_sense_cam_h(), Seeed's model), so the lens top
//  stood at 21.9 over a 21.5 lid face — the module ∩ the seated lid probed
//  172 mm3 — and the window sat over the board center while the module sits
//  6.95 toward the USB (the WAP's error, corrected there 2026-09-26). stack_h
//  is the registry's, cam_dx +6.95, and the Ø8 barrel stands 1.3 into a Ø10
//  window, its top 0.5 behind the disc (the WAP's arrangement, asserted):
//  outer height 21.5 -> 24.6, the boot 25.4 -> 28.5, the plan view unchanged.
//  The vent left the lens's new end for x = 5, and the lid cuts its lens and
//  vent at -y (it assembles turned over about X, like its key slot).
//  BOARD END STOPS (the WAP's v0.9.1): nothing located the board along X —
//  it slid 2.2 to the divider and 12.7 to the wall, keeping 1.9 of each
//  8 mm clip hook. The cavity takes the house floor cove; the face-down
//  lid's bezel and disc counterbores take bridge steps; the trim ring prints
//  show face down. THE BOOT's retention lip sat 0.6 under the lid face, 0.5-
//  1.0 into the lid's side (162.7 mm3) — it now stands on the face and hooks
//  it — and its 8 x 29 lanyard window, a 29 mm TPU lintel printed on air, is
//  a bore on the case's lanyard axis. An echo sizes the battery's over-pad:
//  a second foam_t pad left the pack 4.0 mm of travel.
// ============================================================================

use <canary_core_lib.scad>   // rrect2d, tearbore_x, flat pan-head counterbore
use <canary_snap_lib.scad>   // the cantilever board clip + its strain budget
use <canary_mount_lib.scad>  // the stud/keyhole hanging standard — the blind pocket's one home
use <canary_board_lib.scad>  // board registry — the XIAO numbers the knobs cite
use <canary_port_lib.scad>   // only the Type-C overmold envelope the board's end stops stand clear of — no wall
                             // opening: ZERO external connectors is still this case's whole seal story

/* [What to render] */
part = "all";        // ["body","lid","boot","gasket","bezel","all"]

/* [Board] — XIAO ESP32-S3 Sense, camera stack up, USB toward +X wall */
board_l = 21.0;      // board length — the xiao registry record, brd_l("xiao"), canary_board_lib
board_w = 17.5;      // board width — the xiao registry record, brd_w("xiao"); the board rides in
                     // CLIPS, so clip_clear absorbs the measured 17.8 width
                     // (brd_xiao_w_measured) and the spec 17.5 stays
board_h = 1.2;       // PCB thickness — the xiao registry record, brd_t("xiao")
stack_h = 12.7;      // Sense camera lens top above the base PCB's top face — brd_xiao_sense_cam_h() (Seeed's model), canary_board_lib
cam_dx = 6.95;       // lens center offset from board center, + = toward the USB (+X) — brd_xiao_sense_cam_dx() is -6.95 with + = away from the USB: the module sits over the USB end
cam_dy = 0.0;        // lens center offset from board center, along Y (cam_dx is X) — the disc pocket, aperture and bezel recess follow it

/* [Battery] — MEASURE yours; 503035 (500 mAh) default. LiPo: never charge below 0 C */
batt_l = 35.0;  batt_w = 30.0;  batt_h = 5.0;
foam_t = 2.0;        // closed-cell foam bed under + over the pack
batt_pad = 1.5;      // end clearance, at the -X WALL end: the pack rides against the divider on its foam
                     // and its corners clear the cavity's corner radius (asserted; at 1.0 against the
                     // wall they stood 0.2 into it)

/* [Print tolerances] — tune with canary_fit_coupon.scad */
tol_slide = 0.20;   // catalog default — core_tol_slide() (canary_core_lib)
tol_press = 0.10;   // catalog default — core_tol_press() (canary_core_lib)
tol_hole  = 0.30;   // catalog default — core_tol_hole() (canary_core_lib)

/* [Shell] — 4 mm everywhere is the field-grade floor, not a suggestion.
   floor_t is 4.5 so the keyhole pockets reach the ecosystem-standard 3.5 mm
   depth (the shared T-stud is 3.4 mm tall) while keeping a 1.0 mm ceiling. */
wall_t  = 4.0;   // deviates: drop-case wall — impact duty is this case's whole intent
floor_t = 4.5;   // floor thickness — 4.5 so the keyhole pockets reach the standard 3.5 mm depth with a 1.0 mm ceiling
lid_t   = 4.0;   // lid plate thickness, 4.0 like the walls; the lens web left under the disc pocket (lid_t - disc_t - 0.2) is asserted >= 1.5
r_in    = 3.0;       // cavity corner radius
head_clear = -1.3;   // camera-top clearance under the lid's inner face. NEGATIVE: the Ø8 lens barrel stands 1.3 into the lid's window, its top 0.5 behind the disc (the WAP's arrangement; asserted)

/* [O-ring seal] — Ø1.5 cord in a body-rim groove, 27 % squeeze */
oring_d = 1.5;       // O-ring cord diameter — the squeeze (1 - grv_d/oring_d) is asserted at 15-40 % and the groove fill at <= 90 %
grv_w   = 2.0;       // groove width
grv_d   = 1.1;       // groove depth (squeeze = 1 - grv_d/oring_d)
g_c     = 2.0;       // groove centerline offset outboard of the cavity edge

/* [Screw lobes] — 8x M3 heat-set inserts, outboard of the seal line */
lob_d    = 9.0;      // screw lobe diameter — each lobe is hulled back into the wall as a gusset neck, and the end lobes must clear the corner radius (asserted)
lob_off  = 8.0;      // lobe center offset outboard of the cavity edge (keeps the lanyard bore >=1.5 mm off the pressure wall)
end_lob_y = 8.0;     // Y of the two lobes on each short end
insert_d = 4.0;     // M3 heat-set HOLE Ø (the short-series insert's knurl is Ø4.6:
                    // bore it 4.0 so the brass bites — at 4.6 it pulled out under preload)
insert_h = 7.0;     // M3 heat-set hole depth — the M3 x 8 reaches 6.0 below the rim; 1 mm to spare
screw_c_d = 3.4;                    // lid clearance hole
cb_d = 6.4;                         // pan-head counterbore Ø
cb_h = 2.0;                         // pan-head counterbore depth

/* [Lens window] — Ø12x2 polycarbonate disc, bonded from outside */
disc_d = 12.0;  disc_t = 2.0;
ap_d   = 10.0;       // window through the web behind the disc. The Ø8 lens barrel stands in it, 1.0 a side (asserted); the disc keeps a 1.2 bond ring
bridge_layer = 0.2;  // your slicer's layer height: the face-down lid's bezel-recess and disc-seat counterbores get bridge steps this thick (core_bridge_steps)  // [0.08:0.04:0.32]

/* [Vent] — Ø3 hole + adhesive ePTFE patch on the INSIDE face */
vent_d = 3.0;    // vent hole diameter through the lid; the ePTFE patch's spot-face on the inner face is vent_d + 2.4 wide and 0.9 deep
vent_x = 5.0;    // vent position X — over the board's antenna end, clear of the lens and the battery
                 // (the lens moved to the USB end and its bezel reaches x = 29.7, so the old
                 // x = 30 over the plug room cut the bezel recess; at (-15, 6) the membrane sat
                 // under the battery's foam-over pad); the lens keep-out and the wall are asserted
vent_y = 0.0;    // vent position Y, assembled (the lid cuts it at -vent_y, see lid()) — see vent_x

/* [Stud/keyhole interface] — blind keyhole pockets, seal-safe (never reach the cavity) */
kh_x = 18.0;         // +/- X of the two keyholes
kh_head_d = 8.0;     // deviates: head Ø8.0 is a STATED deviation from
                     // mount_kh_head_d() = 7.0: a field case gets hung on found hardware, and Ø8
                     // passes a #8 pan head too, not just the T-stud. Shank/slot are the
                     // canary_mount_lib standard.
kh_shank_d = 4.2;    // shank slot width — the canary_mount_lib standard, mount_kh_shank_d()
kh_slot_l = 8.0;     // slot travel — the canary_mount_lib standard, mount_kh_slot_l()
kh_head_h = 3.5;     // total pocket depth — catalog standard, mount_kh_head_h();
                     // fits the 3.4 mm T-stud (mount_stud_h) with 0.1 ceiling clearance
kh_face = 1.0;       // face web the screw head grips behind — catalog standard, mount_kh_face()

/* [Lanyard] — paracord bore through the two -X lobes */
lan_d = 4.5;  lan_z = 5.0;

/* [Lid alignment lip] — shear key inboard of the seal */
lip_w = 1.5;  lip_h = 2.0;
lid_key = true;      // poka-yoke: a rib on the +Y cavity wall and a slot in the lid's lip — the lobes fit a lid two ways and the lens lines up one way

/* [Bezel accent] — contrast-color trim ring hiding the lens bond line */
bez_on = true;
bez_o = 19.0;        // bezel outer Ø (= recess Ø in the lid face)
bez_i = 10.5;        // bezel opening (overlaps the disc edge by ~0.75/side)
bez_recess = 1.0;    // recess depth in the lid face
bez_proud = 0.8;     // stand above the face = the inner lip thickness (>= 0.6);
                     // doubles as a sacrificial lens hood on face drops

/* [Boot] — TPU 95A impact boot; earns the drop rating */
boot_w    = 2.5;     // boot wall
boot_floor = 2.5;    // boot floor thickness under the case; the keyhole windows are cut through it
boot_fit  = 0.2;     // stretch interference
grip_grooves = 3;    // grip grooves round the boot: 1.4 tall at a 5 mm pitch, the first 4 mm above the boot floor

/* [Board cradle] — clips are strain-gated by canary_snap_lib on every render.
   The beam is standoff_h + board_h = 4.2 mm; the 1.5/0.8 this file used to
   carry strains ~10 % there — the pair that cracks vertical-print PETG, on
   the one case that is dropped by design. 1.0/0.5 is the WAP's print-proven
   pair (~4.3 % against the 4.5 % once-budget, engagement 0.5 >= the 0.4
   floor); retention comes from the catalog's widest tabs — holding force
   scales with clip_w, insertion strain does not. */
standoff_h = 3.5;    // 3.5: beam 4.7 keeps the MEASURED 17.8 board at 4.4 % (3.0 ran 5.5 %)
usb_room = 12.0;     // plug room beyond the board's USB edge (a USB-C boot needs ~10 to seat);
                     // 0 = pop the board out of its clips to charge
clip_w = 8.0;  clip_t = 1.0;  clip_hook = 0.5;  clip_hook_h = 1.2;  clip_clear = 0.25;

/* [Divider] */
rib_t = 2.0;  rib_h = 8.0;  notch_w = 6.0;  notch_d = 4.0;

/* [Aesthetics] */
label_text = "CANARY";   // text debossed on the lid's outer face, mirrored to read from outside; "" = no label
label_size = 5.0;  label_depth = 0.5;
label_dx = -12.0;  label_dy = -7.0;
label_font = "Liberation Sans:style=Bold";   // the lid label's font (an OpenSCAD font name)
lid_edge = 0.64;     // deviates: the TPU boot's retention lip grips this stepped edge (DR-5) — conform only with a boot re-probe

/* [Quality] */
$fa = 3; $fs = 0.4;

// ---- derived --------------------------------------------------------------
board_zone = board_l + 4.5 + usb_room;             // board + clip room + plug room at the +X wall
inner_l = batt_l + batt_pad + rib_t + board_zone;
inner_w = max(batt_w + 1.0, board_w + 9.0);
cav_h   = standoff_h + board_h + stack_h + head_clear;
base_h  = floor_t + cav_h;
out_l   = inner_l + 2*wall_t;
out_w   = inner_w + 2*wall_t;
r_out   = r_in + wall_t;
total_h = base_h + lid_t;
bcx     = inner_l/2 - board_zone/2 - usb_room/2;   // board center X (the plug room sits +X of it)
x_div   = -inner_l/2 + batt_l + batt_pad + rib_t/2;
lens_x  = bcx + cam_dx;  lens_y = cam_dy;
// lid key: the +Y wall, off center toward +X (the board side) — turned 180°
// the slot lands on -X of the -Y wall and the lip stands on the rib
key_x   = inner_l/4 + 6.0;

// two lobes on each long side at x = ±inner_l/4 (it was one, mid-side: the
// end lobe -> mid-side lobe seal span ran 47 mm against the ~40 mm rule)
lob_xm  = inner_l/4;
function lobes() = [
    [ (inner_l/2 + lob_off),  end_lob_y], [ (inner_l/2 + lob_off), -end_lob_y],
    [-(inner_l/2 + lob_off),  end_lob_y], [-(inner_l/2 + lob_off), -end_lob_y],
    [ lob_xm,  (inner_w/2 + lob_off)], [ lob_xm, -(inner_w/2 + lob_off)],
    [-lob_xm,  (inner_w/2 + lob_off)], [-lob_xm, -(inner_w/2 + lob_off)] ];
// the clamp spans along the O-ring's centerline between neighboring lobes
seal_span_long = 2*lob_xm;                                           // the two lobes of one long side
seal_span_end  = (inner_l/2 - r_in - lob_xm) + PI/2*(r_in + g_c)     // long-side lobe -> round the corner
                 + (inner_w/2 - r_in - end_lob_y);                   //   -> end lobe
seal_span_short = 2*end_lob_y;                                       // the two lobes of one short end
seal_span = max(seal_span_long, seal_span_end, seal_span_short);

// O-ring engineering checks
squeeze = 1 - grv_d/oring_d;
grv_fill = (PI/4*oring_d*oring_d) / (grv_w*grv_d);
grv_R = r_in + g_c;
cord_len = 2*(inner_l + 2*g_c) + 2*(inner_w + 2*g_c) - 8*grv_R + 2*PI*grv_R;

assert(squeeze >= 0.15 && squeeze <= 0.40, "O-ring squeeze must be 15-40% — adjust grv_d/oring_d");
assert(seal_span <= 40, str("worst seal clamp span ", seal_span, " mm is past the ~40 mm clamp-spacing rule — add lobes"));
// the printed-TPU fallback ring (gasket()): 0.6 narrower than the groove and
// 0.3 proud of it — core_gasket_fill's house ceiling. At grv_w - 0.2 by
// grv_d + 0.6 it filled 139 %: a solid ring that props the lid open
function gk_shrink() = 0.6;
function gk_proud()  = 0.3;
assert(core_gasket_fill(grv_w, grv_d, gk_proud(), gk_shrink()) <= core_gasket_fill_max(),
       str("the printed TPU fallback ring would fill ", round(100*core_gasket_fill(grv_w, grv_d, gk_proud(), gk_shrink())),
           " % of the O-ring groove — past ", round(100*core_gasket_fill_max()), " %"));
// the pack's wall-end corners inside the cavity's corner radius (pack against the divider)
batt_dy = batt_w/2 - (inner_w/2 - r_in);
assert(batt_dy <= 0 || batt_pad >= r_in - sqrt(max(0, r_in*r_in - batt_dy*batt_dy)) + 0.1,
       "the battery's wall-end corners run into the cavity's corner radius — raise batt_pad");
assert(abs(key_x) + core_key_w()/2 + 1.0 <= inner_l/2 - r_in, "the lid key rides onto the cavity's corner radius");
assert(grv_fill <= 0.90, "O-ring groove over-filled (>90%) — widen grv_w");
assert(g_c - grv_w/2 >= 0.8, "inner groove cheek < 0.8 mm");
assert(wall_t - g_c - grv_w/2 >= 0.8, "outer groove cheek < 0.8 mm — thicken wall_t");
assert(lid_t - disc_t - 0.2 >= 1.5, "lens web < 1.5 mm — thicken lid_t or thin disc_t");
// the Sense camera (2026-10-23): the stack is at least the registry's lens
// height, the lens top stays >= 0.2 behind the bonded disc, and where the
// barrel stands into the lid it stands in a window 1.0 a side wider than it —
// which also covers the registry's 0.64 cross-width offset, whose side wants
// a bench unit (cam_dy stays 0)
assert(stack_h >= brd_xiao_sense_cam_h() - 1e-6,
       str("stack_h ", stack_h, " is under the Sense camera's lens height brd_xiao_sense_cam_h() ", brd_xiao_sense_cam_h(),
           " — the lid lands on the lens"));
assert(standoff_h + board_h + stack_h <= cav_h + (lid_t - disc_t - 0.2) - 0.2,
       "the camera's lens top reaches the lens disc — raise head_clear");
assert(standoff_h + board_h + stack_h <= cav_h || ap_d >= brd_xiao_sense_cam_fp() + 2.0,
       str("the Ø", brd_xiao_sense_cam_fp(), " lens barrel stands ", standoff_h + board_h + stack_h - cav_h,
           " mm into the lid — its window (ap_d) needs Ø", brd_xiao_sense_cam_fp() + 2.0, " (1.0 a side)"));
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
assert(batt_h + 2*foam_t + 1.0 <= cav_h, "battery + foam bed + foam over exceed the cavity — thin foam_t or raise stack_h");
assert(sqrt(pow(vent_x - lens_x, 2) + pow(vent_y - lens_y, 2)) >= (bez_on ? bez_o : disc_d)/2 + vent_d/2 + 2.4,
       "the vent's inner spot-face runs into the lens seat — move vent_x/vent_y");
assert(abs(vent_x) + vent_d/2 + 1.2 + 1.0 <= inner_l/2 && abs(vent_y) + vent_d/2 + 1.2 + 1.0 <= inner_w/2,
       "the vent's spot-face reaches the wall — move vent_x/vent_y inboard");

echo(str("Canary FIELD case v0.1-dev — outer ", out_l, " x ", out_w, " x ", total_h,
         " (+boot ", out_l + 2*(boot_w + lob_off + lob_d/2 - wall_t), " wide at lobes)"));
echo(str("O-ring: Ø", oring_d, " cord, cut ~", round(cord_len), " mm, squeeze ",
         round(squeeze*100), "%, groove fill ", round(grv_fill*100), "%; worst clamp span ",
         round(seal_span*10)/10, " mm (", len(lobes()), " lobes)"));
echo("CER-4 design intent: IP67 + MIL-STD-810H 516.8 IV (boot on) — VERIFY per field_ratings.md");
// the battery's foam: foam_t is the bed UNDER the pack; the pad over it fills
// the rest of the cavity up to the lid plate (0.5 squeeze), cut inside the
// lid's lip ring so it never props the lid. A second foam_t pad left the pack
// cav_h - batt_h - 2*foam_t of free travel (4.0 mm at 2026-09's numbers) in
// the case built to be dropped — the number nobody was told to cut.
echo(str("Battery foam: a ", foam_t, " mm closed-cell bed under the pack; the pad over it is ",
         cav_h - foam_t - batt_h + 0.5, " thick x ", inner_w - 2*(tol_slide + lip_w), " wide x ",
         min(batt_l, batt_l + batt_pad - tol_slide - lip_w), " long (inside the lid's lip ring, 0.5 squeeze) — a ",
         foam_t, " mm pad would leave the pack ", cav_h - batt_h - 2*foam_t, " mm to rattle"));
if (wall_t < 4.0 || floor_t < 4.0 || lid_t < 4.0)
    echo("NOTE: shell thinner than 4.0 mm — CER-4 submersion intent assumes 4.0 mm");

// ---- shared 2D ------------------------------------------------------------
// rrect2d comes from canary_core_lib — the local copy is gone
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

// ---- interior furniture ----------------------------------------------------
// The WAP cantilever clip, routed through canary_snap_lib so the strain gate
// runs on every render — boards go in once, so the ONCE budget applies. The
// lib places clips across a Y edge line; this file's clips stand on the
// board's ±Y edges via the original rotate-into-place transform (identity
// ops around the lib's own drawing, so nothing but the clip numbers moved).
module edgeclip(px, py, ang) {
    translate([px, py, 0]) rotate([0, 0, ang - 90])
        snap_boardclip(0, 0, 1, floor_t, floor_t + standoff_h + board_h,
                       clip_w, clip_t, clip_hook, clip_hook_h, clip_clear,
                       snap_budget_once(), over = (brd_xiao_w_measured() - board_w)/2);
}

// The board's END STOPS (2026-10-23) — the WAP's v0.9.1 port. The clips hold
// Y and Z and the rails are flat rests, so nothing located the board ALONG X:
// the measured board slid 2.2 to the divider (a plug pushed in with the lid
// off) and 12.7 toward the +X wall (a drop), where each 8 mm clip kept 1.9 of
// its hook — and with the lens barrel standing 1.3 into a window 1.0 a side
// wider, a 1 mm slide lands the lid on the camera. A core_min_wall() column
// off each end of each long edge, up to the PCB's top face, 0.3 off the drawn
// ends (a real XIAO mics 21.14: 0.23 a side), outside the Type-C overmold's
// spec envelope across the connector's axis so a spec-max plug still seats
// with the lid off; the -X pair runs back into the divider rather than leave
// a 0.75 sliver beside it.
stop_y0 = port_usbc_overmold_w()/2 + 0.2;   // a stop's inboard edge across the board
module board_stops() {
    for (sx = [1, -1], sy = [1, -1]) {
        x1 = bcx + sx*(board_l/2 + 0.3);                  // the face a board end meets
        x0 = sx > 0 ? x1 : x_div + rib_t/2 - 0.01;
        t  = sx > 0 ? core_min_wall() : x1 - x0;
        translate([x0, sy > 0 ? stop_y0 : -board_w/2, floor_t - 0.01])
            cube([t, board_w/2 - stop_y0, standoff_h + board_h + 0.01]);
    }
}
assert(board_w/2 - stop_y0 >= core_min_wall(), "the board's end stops would be thinner than a wall across the board");
assert(bcx + board_l/2 + 0.3 + core_min_wall() <= inner_l/2 - 1.0, "the board's +X end stop runs into the +X wall");
assert(bcx - board_l/2 - 0.3 - (x_div + rib_t/2) >= core_min_wall(), "the board's -X end stop is thinner than a wall — the board sits too close to the divider");

// blind keyhole pocket, opening on the bottom face (z0 = 0); slot slides
// toward +X so the case drops on and gravity latches. The drawing is
// canary_mount_lib's, natively along X — this file passes its own Ø8.0 head
// (the stated deviation at the knob), the rest is the standard. Drawn at
// c = 0 and translated into place, matching how the local copy composed it
// (the pocket tessellates in the same local frame, so the mesh stays put).
module keyhole_pocket(xc) {
    translate([xc, 0, 0])
        mount_keyhole_pocket(0, 0, "x",
                             kh_head_d, kh_shank_d, kh_slot_l, kh_head_h, kh_face);
}

// ---- parts ------------------------------------------------------------------
module body() {
    difference() {
        linear_extrude(base_h) outline2d();
        // cavity — with the house floor cove (cavity_cut, DESIGN_RULES §2):
        // the drop case's floor met its walls in a square corner along one
        // layer line, the hinge a corner drop cracks
        translate([0, 0, floor_t]) cavity_cut(inner_l, inner_w, r_in, cav_h + 0.1);
        // O-ring groove in the rim top
        translate([0, 0, base_h - grv_d]) linear_extrude(grv_d + 0.2) grv_ring2d();
        // heat-set insert pockets in the lobe tops
        for (p = lobes())
            translate([p[0], p[1], base_h - insert_h]) cylinder(d = insert_d, h = insert_h + 0.1);
        // blind keyhole pockets (bottom face; slot slides toward +X)
        keyhole_pocket(kh_x);
        keyhole_pocket(-kh_x);
        // lanyard bore through both -X lobes (clears the pressure wall) —
        // the library teardrop, swung onto Y (rotate about Z keeps the
        // teardrop crown up); its flat 45° roof replaces the pointed local cap
        translate([-(inner_l/2 + lob_off), 0, lan_z]) rotate([0, 0, 90])
            tearbore_x(-(2*end_lob_y + lob_d + 5)/2, 0, 0,
                       2*end_lob_y + lob_d + 5, lan_d);
    }
    // interior furniture — union AFTER the cuts so nothing erases it
    for (s = [1, -1])
        translate([bcx - (board_l - 1)/2, s*(board_w/2 - 1.5) - 1.5, floor_t - 0.01])
            cube([board_l - 1, 3, standoff_h + 0.01]);
    edgeclip(bcx,  board_w/2,  90);
    edgeclip(bcx, -board_w/2, 270);
    board_stops();
    difference() {                     // battery divider with a wire notch
        // the divider runs 0.3 INTO each side wall — it is the drop case's
        // only transverse member, and the old cut stopped 0.5 mm short of
        // both walls (probe-proven voids), so the one rib that should tie
        // the shell together tied nothing
        translate([x_div - rib_t/2, -inner_w/2 - 0.3, floor_t - 0.01])
            cube([rib_t, inner_w + 0.6, rib_h + 0.01]);
        translate([x_div - rib_t/2 - 0.1, inner_w/2 - 2 - notch_w, floor_t + rib_h - notch_d])
            cube([rib_t + 0.2, notch_w, notch_d + 0.1]);
    }
    // lid key (canary_core_lib): a rib on the +Y cavity wall under the lip zone
    if (lid_key) lid_key_rib(key_x, inner_w/2, 270, base_h, lip_h);
}

module lid() {                          // z=0 is the OUTER face; print face-down
    difference() {
        union() {
            // bullnose face edge (6-step quarter-round; prints face-down,
            // kills elephant foot and reads "molded" instead of "printed")
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
        // the key's slot through the lip. The lid is modeled face-down and
        // assembles turned over about X (y -> -y), so the slot sits at -y here
        // and the library's lid frame (plate above z = 0, lip below) is this
        // one mirrored about the plate's inner face
        if (lid_key) translate([0, 0, lid_t]) mirror([0, 0, 1])
            lid_key_slot(key_x, -inner_w/2, 90, lip_h, lip_w);
        // screw holes + FLAT pan-head counterbores — canary_core_lib's seat
        // (a shallow cone under a pan head bears only on its lip). The lib
        // cuts down toward z = t; this lid's show face is z = 0, so mirror —
        // the cut volumes land exactly where the local cylinders did.
        for (p = lobes()) translate([p[0], p[1], lid_t]) mirror([0, 0, 1])
            cb_flat_cut(0, 0, lid_t, screw_c_d, cb_d, cb_h);
        // lens: disc pocket (disc lands 0.2 sub-flush) + window. At -lens_y:
        // the lid assembles turned over about X, like the key slot above (cut
        // at +lens_y, cam_dy = -0.64 put the window 1.28 off the camera)
        translate([lens_x, -lens_y, -0.1]) cylinder(d = disc_d + 2*tol_slide, h = disc_t + 0.3);
        translate([lens_x, -lens_y, -0.1]) cylinder(d = ap_d, h = lid_t + 0.2);
        // bezel recess around the lens (press-fit trim ring hides the bond line)
        if (bez_on)
            translate([lens_x, -lens_y, -0.1]) cylinder(d = bez_o, h = bez_recess + 0.1);
        // both counterbores open on the BED of this face-down print, so the
        // first layer over each floor was a ring round the next hole with
        // nothing under it (3.3 wide round the disc pocket; 2.2 round the old
        // Ø8 aperture, 1.2 round the Ø10 window — and that floor is the disc's
        // seal face). Bridge steps: a slot, then a square, every layer a
        // straight bridge anchored on the bore's wall
        if (bez_on)
            core_bridge_steps(lens_x, -lens_y, bez_recess, disc_d + 2*tol_slide, bez_o, up = 1, layer = bridge_layer);
        core_bridge_steps(lens_x, -lens_y, disc_t + 0.2, ap_d, disc_d + 2*tol_slide, up = 1, layer = bridge_layer);
        // vent hole (ePTFE patch adheres to the flat INNER face around it —
        // spot-face the INNER side; an outer recess would expose the membrane
        // to rain, UV and pokes); at -vent_y, the same turn-over as the lens
        translate([vent_x, -vent_y, -0.1]) cylinder(d = vent_d, h = lid_t + 0.2);
        translate([vent_x, -vent_y, lid_t - 0.9]) cylinder(d = vent_d + 2.4, h = 1.0);
        // debossed label on the outer face (mirrored: read from outside)
        if (label_text != "")
            translate([label_dx, label_dy, -0.1]) linear_extrude(label_depth + 0.1)
                mirror([1, 0, 0]) text(label_text, size = label_size, font = label_font,
                                       halign = "center", valign = "center");
    }
}

module gasket() {                       // printed-TPU fallback if you skip the cord
    linear_extrude(grv_d + gk_proud()) difference() {
        offset(r = g_c + (grv_w - gk_shrink())/2) cav2d();
        offset(r = g_c - (grv_w - gk_shrink())/2) cav2d();
    }
}

module bezel() {                        // contrast-color lens trim ring
    // z0 = the SHOW FACE, and it prints face down as modeled: the lip's face
    // is on the bed, so every layer stands inside the one below. (Modeled
    // relief face down until 2026-10-23, the lip's underside was a 1.05 ring
    // overhanging the disc relief, 0.2 off the lens for any sag to close.)
    // Assembled it turns over: the relief face lands on the recess floor. The
    // inner lip floats 0.2 mm above the bonded disc and hides the silicone
    // bond line — the detail every consumer camera trim ring exists for.
    difference() {
        cylinder(d = bez_o - 2*tol_press, h = bez_recess + bez_proud);
        translate([0, 0, -0.1]) cylinder(d = bez_i, h = bez_recess + bez_proud + 0.2);
        // relief over the disc, cut from the relief face: the disc top sits
        // (bez_recess - 0.2) above the recess floor, so a relief bez_recess
        // deep leaves 0.2 clearance and the inner lip exactly bez_proud thick
        translate([0, 0, bez_proud]) cylinder(d = disc_d + 0.6, h = bez_recess + 0.1);
    }
}

module boot() {                         // TPU 95A; print open-side-up
    // The rim stands 0.6 proud of the TALLEST face feature — which with the
    // trim ring fitted is the bezel tip (bez_proud above the lid), not the
    // lid itself. The old +0.6 was measured from the lid face, so the bezel
    // stood 0.2 proud of the boot and a lid-face drop landed the whole case
    // on a press-fit trim ring over the lens.
    rim_over = (bez_on ? bez_proud : 0) + 0.6;
    bh = boot_floor + total_h + rim_over;
    difference() {
        linear_extrude(bh) offset(r = boot_w - boot_fit) outline2d();
        // case pocket (stretch fit) — cut past the rim so the taller rim
        // stays a rim, not a roof
        translate([0, 0, boot_floor]) linear_extrude(total_h + rim_over + 0.5) offset(r = -boot_fit) outline2d();
        // keyhole windows in the floor
        for (s = [1, -1])
            translate([s*kh_x, 0, -0.1]) linear_extrude(boot_floor + 0.2)
                rrect2d(kh_slot_l + kh_head_d + 8, kh_head_d + 8, 3);
        // lanyard bore on the case's own bore axis, through the wraps round
        // both -X lobes and the tongue between them — 0.5 over the case's
        // bore. It was an 8 x 29 window, and the TPU wall over it was a lintel
        // anchored only at its jambs, 29 mm apart, printed on air
        translate([-(inner_l/2 + lob_off), 0, boot_floor + lan_z]) rotate([0, 0, 90])
            tearbore_x(-(2*end_lob_y + lob_d + 2*boot_w + 8)/2, 0, 0,
                       2*end_lob_y + lob_d + 2*boot_w + 8, lan_d + 0.5);
        // grip grooves
        for (k = [0:grip_grooves - 1])
            translate([0, 0, boot_floor + 4 + k*5]) linear_extrude(1.4) difference() {
                offset(r = boot_w - boot_fit + 0.1) outline2d();
                offset(r = boot_w - boot_fit - 0.8) outline2d();
            }
    }
    // retention lip: snaps over the lid's chamfered edge — anchored to the
    // LID EDGE's height (it used to ride at bh - 1.2, which the raised rim
    // would have carried up past the edge it grips). It STANDS ON the face
    // plane (2026-10-23): centered on it, its lower 0.6 sat 0.5-1.0 inside the
    // lid's side all round (162.7 mm3 against a 0.2 boot_fit), so it pressed
    // the side instead of hooking the face; now it overhangs the face's flat
    // band ~0.5 past the bullnose, 0.3 clear of the counterbores, its top
    // still under the rim
    translate([0, 0, boot_floor + total_h]) linear_extrude(min(1.2, rim_over)) difference() {
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
