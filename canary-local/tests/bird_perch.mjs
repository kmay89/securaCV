// canary-local/tests/bird_perch.mjs — where the canary mark is drawn, judged
// the same way by every browser probe that reads the glass (F64).
//
// A bird on stage sits where its host placed it: on the glass, and clear of
// every line of text. In the onboarding scenes it sits exactly where the
// layout seats it (F89): birdOnSeat holds the drawn box to the seat
// onboard_layout.h names for that glass and scene. The mark keeps its seat as a style offset from its
// alignment; a base read from the laid-out position instead (before F64)
// lost the offset, so the round watch's bird perched on the ring and the
// onboarding's sat behind the titles, and once a host re-seated it the
// bird left the glass.

/**
 * st: {bird, labels, glass} as the page reads them — bird from
 * __emu.markBox(), labels from __emu.screenLabels(), glass {w, h} from the
 * canvas. Returns a failure message, or null when the bird is off stage
 * (hidden, or none alive) or sits where it should.
 */
export function birdPerch(st) {
  const b = st.bird;
  if (!b || !b.shown) return null;
  const { w, h } = st.glass;
  if (b.x < 0 || b.y < 0 || b.x + b.w > w || b.y + b.h > h) {
    return `the bird is drawn at ${b.x},${b.y} (${b.w}x${b.h}), off the ${w}x${h} glass (F64)`;
  }
  const over = st.labels.filter((l) => l.shown && l.opa > 0 && l.text.trim() !== "" &&
    l.x < b.x + b.w && b.x < l.x + l.w && l.y < b.y + b.h && b.y < l.y + l.h);
  if (over.length) {
    return `the bird (${b.x},${b.y} ${b.w}x${b.h}) is drawn over ` +
      `${JSON.stringify(over.map((l) => l.text))} (F64)`;
  }
  return null;
}

/**
 * F89: the onboarding bird drawn at the seat its scene's layout names.
 * st: {bird, seat} — bird from __emu.markBox(), seat from
 * __emu.onboardSeat(): {x, y, w, h, breath}, onboard_layout.h's
 * bird_seat() for this glass, these faces and this scene, evaluated by the
 * firmware itself (no second table here). The bird must be on stage, its
 * box the seat's size at the seat's x, and its top within the breath of
 * the seat's y. Returns a failure message, or null.
 */
export function birdOnSeat(st) {
  const b = st.bird;
  const s = st.seat;
  if (!s) return "no onboarding scene names a seat for the bird (F89)";
  if (!b || !b.shown) return `the scene seats the bird at ${s.x},${s.y} but none is on stage (F89)`;
  if (b.w !== s.w || b.h !== s.h || b.x !== s.x || Math.abs(b.y - s.y) > s.breath) {
    return `the bird is drawn at ${b.x},${b.y} (${b.w}x${b.h}); onboard_layout.h seats it at ` +
      `${s.x},${s.y} (${s.w}x${s.h}, breathing ${s.breath} px) (F89)`;
  }
  return null;
}
