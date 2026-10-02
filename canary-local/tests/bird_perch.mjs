// canary-local/tests/bird_perch.mjs — where the canary mark is drawn, judged
// the same way by every browser probe that reads the glass (F64).
//
// A bird on stage sits where its host placed it: on the glass, and clear of
// every line of text. The mark keeps its seat as a style offset from its
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
