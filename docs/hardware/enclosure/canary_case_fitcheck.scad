// ============================================================================
//  Canary released cases — ASSEMBLY FIT CHECK (not a printable part)
//
//  WHY THIS FILE EXISTS. The 7" case had a fit check and the four RELEASED
//  cases did not, and the class of defect it guards against duly shipped in
//  all four of them.
//
//  The 2026-09-03 assembly pass fixed a real bug — a pan-head seat as deep as
//  the front is a through-hole the head falls through — by adding a pad under
//  each head on the inside of the front. The pad was drawn as a bare Ø7.8
//  cylinder while the corner post it stands on is Ø5.0 and is deliberately
//  pushed 0.2 INTO the cavity wall, so the pad overhung its post by 1.4 mm all
//  round and 1.6 mm of that landed in solid shell wall. The front then rested
//  on four pads standing on the wall rim, `head_pad` proud: the lip never
//  entered the cavity, the screws clamped nothing, and on the sealed builds
//  the gasket never compressed. On the sealed Vision the pads additionally sat
//  on top of the TPU gasket ring.
//
//  Every one of those parts is a watertight, single-part, warning-free mesh.
//  `render.sh` was green. `admesh` was green. The interference is fully buried
//  — the pad stops 0.4 mm short of the outer face — so nothing that looks at
//  ONE part could ever have seen it. Only putting the two together can, which
//  is exactly what canary_s3_lcd7_fitcheck.scad's header says and why that
//  file exists. This is that file, for the cases that ship STLs.
//
//  HOW TO RUN IT — the recipe is `canary_s3_lcd7_fitcheck.scad`'s, verbatim,
//  and every line of it is load-bearing:
//
//      rm -f /tmp/c.stl                       # a stale file fails you silently
//      out=$(openscad --export-format binstl -o /tmp/c.stl \
//            -D 'check="sense"' canary_case_fitcheck.scad 2>&1) || true
//      if   printf '%s' "$out" | grep -qE 'ERROR|WARNING'; then
//          echo "FAIL — dirty render, nothing was verified"
//      elif [ -f /tmp/c.stl ]; then
//          echo "FAIL — the case does not close"
//      elif printf '%s' "$out" | grep -q 'Current top level object is empty'; then
//          echo "PASS"
//      else
//          echo "FAIL — no file and no empty marker; did it evaluate at all?"
//      fi
//
//  ⚠️  DO NOT join the run and the test with &&, and do not judge by exit
//  status: a PASSING check exits NON-ZERO, because OpenSCAD returns non-zero
//  whenever it has nothing to export, which for an empty-expected gate is
//  precisely success. And absence of a file is not evidence — a run killed by
//  a timeout leaves the same nothing behind as a clean pass, which is why the
//  empty MARKER is demanded rather than inferred.
//
//  WHY THE LIFT. Where two intended contact faces land exactly coplanar the
//  raw intersection returns a zero-volume patch and CGAL emits "may not be a
//  valid 2-manifold" — a dirty render that proves nothing. So the front is
//  displaced `seat_lift` along the mating axis first, and only interference
//  DEEPER than that survives. That is the 7" file's own seat_lift doctrine.
//
//  WHERE THE GEOMETRY LIVES, AND WHY NOT HERE. Each case carries its own
//  `<case>_fitcheck()` module, and this file only selects between them. That
//  is not indirection for its own sake: every case in this catalog names its
//  halves front()/back(), so a single file that `use`d all four would resolve
//  those names to whichever file was parsed last and cheerfully check the same
//  case four times. A gate that silently checks the wrong part is worse than
//  no gate. Keeping the probe beside the geometry also lets it read that
//  file's own derived seam datum rather than duplicating the arithmetic it is
//  meant to be checking — the rule that makes the 7" check trustworthy.
// ============================================================================

use <canary_wap_enclosure.scad>
use <canary_vision_enclosure.scad>
use <canary_vision_doorbell.scad>
use <canary_sense_enclosure.scad>

/* [What to check] */
// Each of these renders the two halves in their ASSEMBLED positions and
// intersects them. EVERY ONE MUST COME OUT EMPTY.
check = "sense";   // ["sense","vision_indoor","vision_weather","vision_devkit","doorbell","wap_battery","wap_compact","wap_weather","wap_pan"]

/* [Tuning] */
seat_lift = 0.1;   // hover the front this far off its seat, so face-on-face contact is not read as interference  // [0.05:0.05:0.5]

/* [Hidden] */

// The variants (Vision's host/preset, the WAP's preset and screw_head) are
// selected on the command line exactly as render.sh selects them, so each
// family below is one call and the caller picks which build it is checking.
if      (check == "sense")               sense_fitcheck(seat_lift);
else if (check == "vision_indoor"
      || check == "vision_weather"
      || check == "vision_devkit")       vision_fitcheck(seat_lift);
else if (check == "doorbell")            doorbell_fitcheck(seat_lift);
else if (check == "wap_battery"
      || check == "wap_compact"
      || check == "wap_weather"
      || check == "wap_pan")             wap_fitcheck(seat_lift);
else assert(false, str("canary_case_fitcheck: unknown check \"", check, "\""));
