// ============================================================================
//  Canary Sense — BEDSIDE STAND  ⚠️ IN DEVELOPMENT (v0.1-dev)
//  The wellbeing/breathing channel wants the Sense unit BEDSIDE at <= 1.5 m
//  (design doc §2), but the radome case only wall/ceiling-mounts. This is a
//  weighted freestanding base with a tilted stalk ending in the standard
//  three-prong hinge head — the Sense case (or any Canary with the GoPro
//  two-prong hinge) clips on and locks with the usual M5 thumbscrew.
//
//  POSE: the case stands UP on the head — radome level and facing the bed is
//  the bedside pose — and tips forward from there down to 15° below
//  horizontal. It does NOT hang plumb below the head: a GoPro joint's center
//  fin points down the stalk, and the case's hinge root collides with it
//  (plumb measures 1,843 mm³ of case inside the head and stalk, the joint
//  itself aside). Re-measured 2026-10-22 against the shell whose fin crowns
//  print on the bed (canary_sense_enclosure.scad's front(), hung on the
//  head's bolt axis by sense_hinge_axis_stl(), the joint — fin_r + 0.4 round
//  the axis — excluded): zero overlap from 10° above horizontal toward the
//  back (5° above hits 3.7 mm³, horizontal 13.4 — the case's root web on the
//  center fin's rear flank),
//  through upright, to 30° below horizontal toward the front (32° touches,
//  35° hits 8 mm³, 40° 66 — the case's top wall on the stalk). pose_low = 15
//  keeps 15° in hand. DETENTS: the teeth hold the case at radome level and
//  every 30° from it — tipped back 30° and 60°, forward 30°, 60° and to
//  horizontal (radome straight down). The next one forward, 30° below
//  horizontal, is free by only the 2° before the top wall's touch, outside
//  pose_low's margin. (The 2026-09-26 numbers — free 45° past horizontal
//  toward the back, 20° below toward the front — did not reproduce: at
//  head_off 10 the fin points dug 0.8 mm into the stalk top from 10° above
//  to 30° below horizontal, 4.2 mm³ radome-down; plumb measured 2,061.)
//
//  Ballast: the base's TOP has pockets for 4 x M10 washers / US quarters
//  (~25 mm discs) under a ring-shaped recess round the stalk; drop them in
//  and cover with the ring ballast lid (glue or tape). The base prints flat on
//  its whole bottom face. Stick-on rubber feet recommended.
//
//  2026-08-23: the teardrop bore now comes from canary_core_lib — same
//  geometry (the local copy was verbatim), one home.
//  2026-09-26: case_reach / case_face are read off canary_sense_enclosure.scad
//  (sense_hinge_reach()/_face(), through `use <>`) — the knobs here had the
//  reach 1.6 short of the ported shell (71.2 for 72.8); the pose sweep above
//  re-run against that shell.
//  2026-10-22: three fixes. (1) The head rises: the case's hinge fins now
//  print with their crown ON the bed — the case's face plane, case_face (14)
//  from the axis — and pointing the way the radome does, so tipped radome-
//  down they aim down the stalk, whose top sat head_off - 0.2 = 9.8 out (the
//  crown-on-bed shell hit it from 30° above horizontal forward on, 105 mm³
//  radome-down). head_off
//  10 -> 15 and stalk_h 70 -> 65: the axis does not move, and an assert holds
//  the stalk top past the crown. (2) The ballast moves to the TOP, under a
//  ring lid: cut from below, the lid recess was a Ø83.6 ceiling 1.2 over the
//  bed — the base touched the bed on a 4.2 mm rim, 1158 of 6648 mm² — and each
//  pocket a 25.6 mm bridge. (3) The head fins' feet are trimmed to the stalk
//  top: the outer fins' 15 mm feet hung up to 6 mm over air, 12° off flat.
//  2026-10-23: the head's detents, against the case they hold:
//    * THE BEDSIDE POSE WAS NOT A DETENT. The pockets were cut in the head's
//      own frame, and the head leans stalk_tilt with the stalk, so its 30°
//      detents fell stalk_tilt off the world's: radome level sat 12° from one
//      and 18° from the next (the released front hung upright on the head:
//      teeth in the outer fins' solid, 34.97 mm³ in 24 pieces; EMPTY only
//      12° radome-down or 18° radome-up), and the clamped teeth could not
//      hold the pose this header calls the bedside one. The pockets are
//      turned back by stalk_tilt about the bolt: radome level and every 30°
//      from it are detents (EMPTY), 15° off one is not (the teeth meet the
//      fins), and the case clears the stand at every detent in the free range;
//    * the outer fins stood at the bare GoPro pitch, 0.175 off the case fins
//      — the head is the third copy of the bracket's three prongs, and the
//      bracket's own lesson (a printed fin's +0.1 left 0.075 of that gap)
//      reached the bracket in AUD-12 but not here. hinge_clear 0.15, the
//      bracket's: 0.325, and a free tooth rides 0.275 into the fin it cams
//      over instead of 0.425.
//
//  ⚠️ DEV STATUS: render/mesh-verified only — NOT print-validated.
// ============================================================================

use <canary_core_lib.scad>   // tearbore_x — the teardrop bore the hinge bolt rides
use <canary_sense_enclosure.scad>   // sense_hinge_reach()/_face()/_back() — the case's reach from its hinge axis, read live

/* [What to render] */
part = "all";        // ["base","ballast_lid","all"]

/* [Stand] */
base_d   = 92.0;     // base disc diameter
base_t   = 12.0;     // base thickness
stalk_h  = 65.0;     // stalk height to the hinge axis region — puts the standing radome ~130 mm over the nightstand
                     // (stalk_h + head_off sets the axis: 70 + 10 became 65 + 15 and the axis stayed put)
pose_low   = 15;     // the forward tip limit below horizontal the base clearance is asserted at (header: free to 30°)
stalk_d  = 16.0;     // stalk diameter
stalk_tilt = 12;     // stalk lean (degrees, toward the front)  // [0:2:20]

/* [Ballast pockets] (coin / washer discs, glued in) */
bal_d    = 25.6;     // disc diameter (US quarter 24.26 + fit; M10 washer ~ 20-30)
bal_t    = 4.0;      // pocket depth ABOVE the lid recess — fits two stacked quarters (2 x 1.75)
                     // per pocket (~45 g total); a cantilevered case + USB cable needs the mass
bal_n    = 4;        // pocket count on a ring
bal_r    = 28.0;     // pocket ring radius

/* [Hinge head] — GoPro-compatible three prongs (mates the Canary two-prong) */
prong_t     = 3.0;   // fin thickness (GoPro standard) — the head's three fins are each this thick, on prong_pitch centers
prong_pitch = 6.35;  // fin center spacing (GoPro standard) — the outer fins sit this far either side of the center one, and the case's two prongs fit the gaps between
fin_r       = 7.5;   // fin end radius round the bolt axis — each fin is hulled from a 2 x this wide root on the stalk top up to this radius at the axis, and the detent pockets reach out to this - 0.5
hinge_bolt_d = 5.0;  // hinge bolt Ø (M5 at the default); its teardrop bore is cut 0.4 over, through all three fins
hinge_clear = 0.15;  // outer fins stand this much wider than the GoPro pitch: the bracket's (a printed fin's +0.1 left 0.075 of the bare 0.175 gap)  // [0:0.05:0.4]
hinge_teeth = true;  // radial detent pockets in the head's outer fins for the case's teeth; false = smooth faces
teeth_n     = 24;    // castellation steps round the hinge, a pocket on every other one: the head detents every 720 / teeth_n degrees. Keep it even and equal to the case's teeth_n
teeth_h     = 0.6;   // the case's detent tooth height — the pockets in the outer fins' inner faces are cut this + 0.15 deep and 0.12 oversize

/* [Print tolerances] */
tol_slide = 0.20;    // catalog default — core_tol_slide(), canary_core_lib
tol_hole  = 0.30;    // catalog default — core_tol_hole(), canary_core_lib

/* [Quality] */
$fa = 3; $fs = 0.4;

hinge_hole = hinge_bolt_d + 0.4;
// the case's reach from its hinge axis — read off canary_sense_enclosure.scad's own
// derivation (out_y + hinge_off to the far wall; base_d + lid_t - fin_r to the radome
// face), never retyped here: a case that grows moves this stand's pose limits with it
case_reach = sense_hinge_reach();
case_face  = sense_hinge_face();
head_off = 15.0;                     // bolt axis above the head's base plate: the stalk top (head_off - 0.2 out)
                                     // must clear the case fin's crown, case_face from the axis (asserted below)
// the hinge axis as base() places it: the stalk top plus head_off, both along the tilted stalk
axis_z = base_t - 2 + (stalk_h - 0.2 + head_off)*cos(stalk_tilt);
// the same point, for a probe that hangs the case on it (the pose sweep in the header)
function sense_stand_axis() = [0, base_d*0.05 + (stalk_h - 0.2 + head_off)*sin(stalk_tilt), axis_z];
function sense_stand_head_x() = prong_pitch + hinge_clear + prong_t;   // the head's half-width along the bolt
// the lowest free pose (tipped pose_low below horizontal, radome down-forward) keeps the
// case's leading corner 2 mm over the base; the plumb pose is not free, so not asserted
assert(axis_z - case_reach*sin(pose_low) - case_face*cos(pose_low) >= base_t + 2,
       "the case tipped fully forward hits the base — raise stalk_h");
assert(bal_r + bal_d/2 < base_d/2 - 3, "ballast ring exceeds the base — shrink bal_r/bal_d");
// the case fin's crown prints on the bed, which is the case's face plane, so it stands
// case_face from the bolt axis and points the way the radome does: tipped radome-down it
// aims down the stalk, whose top disc (the nearest stalk point to the axis) is head_off - 0.2
// out. 0.5 in hand covers the crown's 1.2 foot (its corners reach 0.013 farther). At
// head_off 10 the stalk top was 9.8 out and even the old floating point (10.6) dug 0.8 into it
assert(head_off - 0.2 >= case_face + 0.5,
       str("the case fin's crown (", case_face, " mm from the axis) meets the stalk top facing down it — raise head_off (and lower stalk_h by as much)"));
// the ballast lid is a RING on the base's top, round the stalk: the stalk's footprint
// there (its axis leans tan(stalk_tilt) per mm, its section an ellipse) stays inside the ring
stalk_foot_r = base_d*0.05 + 2*tan(stalk_tilt) + (stalk_d/2)/cos(stalk_tilt);
assert(stalk_foot_r + 0.5 <= bal_r - bal_d/2 - 1,
       "the ballast lid ring runs into the stalk — raise bal_r or shrink bal_d");
assert(bal_r + bal_d/2 + 1 <= base_d/2 - 3 - 1.0,
       "the ballast lid recess leaves under 1 mm of the base's flat top outside it — shrink bal_r/bal_d");
echo(str("Canary Sense bedside stand v0.1-dev — base ", base_d, " mm, head axis at ",
         axis_z, " mm; case stands up, tips to ", pose_low, "° below level  (IN DEVELOPMENT)"));

module teeth2d() {
    step = 360 / teeth_n;
    for (i = [0 : 2 : teeth_n - 1]) rotate([0, 0, i * step])
        intersection() {
            difference() { circle(r = fin_r - 0.5); circle(r = 3.5); }
            polygon([[0, 0], [2*fin_r, 0], [2*fin_r*cos(step), 2*fin_r*sin(step)]]);
        }
}
// three-prong head: fins ⊥ X rise from a base plate at local z=0 (which embeds
// into the stalk top); bolt axis along X at z = head_off (defined with the knobs' derived values)
module head_fin(xc) {
    hull() {
        // the foot stays on the stalk top: a full 2*fin_r foot on an outer fin (x 4.85..7.85,
        // where the Ø16 stalk is only ±6.4..±1.6 wide) hung up to 6 mm over air, 12° off flat
        intersection() {
            translate([xc - prong_t/2, -fin_r, -0.5]) cube([prong_t, 2*fin_r, 0.5]);
            translate([0, 0, -1]) cylinder(d = stalk_d, h = 1);
        }
        translate([xc - prong_t/2, 0, head_off]) rotate([0, 90, 0]) cylinder(r = fin_r, h = prong_t);
    }
}
module hinge_head() {
    difference() {
        union() {
            // the outer fins hinge_clear wider than the GoPro pitch, the bracket's
            head_fin(0); head_fin(-(prong_pitch + hinge_clear)); head_fin(prong_pitch + hinge_clear);
        }
        tearbore_x(-prong_pitch - hinge_clear - prong_t, 0, head_off, 2*(prong_pitch + hinge_clear + prong_t), hinge_hole);
        // detent tooth POCKETS in the outer fins' inner faces — the case fins
        // carry the male teeth (same female convention as the Vision bracket).
        // Turned back by stalk_tilt about the bolt: the head leans with the
        // stalk, and pockets cut square in its frame put every detent
        // stalk_tilt off the world's, the radome-level pose among them
        if (hinge_teeth) translate([0, 0, head_off]) rotate([stalk_tilt, 0, 0]) translate([0, 0, -head_off]) {
            xi = prong_pitch + hinge_clear - prong_t/2;
            translate([ xi - 0.05, 0, head_off]) rotate([0,  90, 0])
                linear_extrude(teeth_h + 0.15) offset(delta = 0.12) teeth2d();
            translate([-xi + 0.05, 0, head_off]) rotate([0, -90, 0])
                linear_extrude(teeth_h + 0.15) offset(delta = 0.12) teeth2d();
        }
    }
}

module base() {
    union() {
        difference() {
            // base disc with a soft top edge
            union() {
                cylinder(d = base_d, h = base_t - 2);
                translate([0, 0, base_t - 2 - 0.01]) cylinder(d1 = base_d, d2 = base_d - 6, h = 2);
            }
            // ballast pockets, from the TOP, under a 1.2 lid recess that is a RING round
            // the stalk (it covers the pockets fully). They were cut from below, and the
            // base prints on its bottom: the lid recess was a Ø83.6 ceiling 1.2 over the bed
            // (the base stood on a 4.2 mm rim) and every pocket a 25.6 mm bridge. Depth is
            // still bal_t below the recess floor — two quarters a pocket
            for (i = [0 : bal_n - 1]) rotate([0, 0, i * 360 / bal_n])
                translate([bal_r, 0, base_t - 1.2 - bal_t]) cylinder(d = bal_d, h = bal_t + 1.3);
            translate([0, 0, base_t - 1.2]) difference() {
                cylinder(d = 2*bal_r + bal_d + 2, h = 1.3);
                translate([0, 0, -0.1]) cylinder(d = 2*bal_r - bal_d - 2, h = 1.5);
            }
        }
        // tilted stalk + hinge head (fins rise off the stalk top; bolt axis along X)
        translate([0, base_d*0.05, base_t - 2]) rotate([-stalk_tilt, 0, 0]) {
            cylinder(d = stalk_d, h = stalk_h);
            translate([0, 0, stalk_h - 0.2]) hinge_head();
        }
    }
}

module ballast_lid() {   // a ring: it seats round the stalk, in the base's top
    difference() {
        cylinder(d = 2*bal_r + bal_d + 2 - 2*tol_slide, h = 1.2);
        translate([0, 0, -0.1]) cylinder(d = 2*bal_r - bal_d - 2 + 2*tol_slide, h = 1.4);
    }
}

if      (part == "base")        base();
else if (part == "ballast_lid") ballast_lid();
else { base(); translate([base_d/2 + bal_r + 12, 0, 0]) ballast_lid(); }
