// canary-local/tests/bird_perch.mjs — where the canary mark is drawn, judged
// the same way by every browser probe that reads the glass (F64).
//
// A bird on stage sits where its host placed it: on the glass, and clear of
// every line of text. In the onboarding scenes it sits exactly where the
// layout seats it (F89): birdOnSeat holds one drawn box to the seat
// onboard_layout.h names for that glass and scene, and breathOnSeat holds
// a scene's run of drawn boxes to it exactly. The mark keeps its seat as a
// style offset from its alignment; a base read from the laid-out position instead (before F64)
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
  const off = birdOnGlass(st);
  if (off !== null) return off;
  const over = st.labels.filter((l) => l.shown && l.opa > 0 && l.text.trim() !== "" &&
    l.x < b.x + b.w && b.x < l.x + l.w && l.y < b.y + b.h && b.y < l.y + l.h);
  if (over.length) {
    return `the bird (${b.x},${b.y} ${b.w}x${b.h}) is drawn over ` +
      `${JSON.stringify(over.map((l) => l.text))} (F64)`;
  }
  return null;
}

/**
 * The bird, when on stage, drawn whole on the glass — the half of birdPerch
 * that holds while it moves: the Success scene's earned hop crosses the
 * scene's lines as they fade in (F184 reads it there). st: as birdPerch
 * takes it. Returns a failure message, or null when the bird is off stage
 * or on the glass.
 */
export function birdOnGlass(st) {
  const b = st.bird;
  if (!b || !b.shown) return null;
  const { w, h } = st.glass;
  if (b.x < 0 || b.y < 0 || b.x + b.w > w || b.y + b.h > h) {
    return `the bird is drawn at ${b.x},${b.y} (${b.w}x${b.h}), off the ${w}x${h} glass (F64)`;
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

/**
 * F89, over the breath: a run of reads of one scene, in time order (each
 * {bird, seat} as birdOnSeat takes it), held to the seat exactly.
 *
 * canary_mark breathes the bird `breath` px either way of its seat on an
 * eased swing (lv_anim_path_ease_in_out), and LVGL's anim rounds the eased
 * offset down. So the drawn top sits at seat.y - breath for about a third of
 * every swing, and at seat.y + breath - 1 for another third, while
 * seat.y + breath itself is drawn only on the tick a swing completes. One
 * read cannot place the bird: anywhere in the breath, a bird up to
 * 2 * breath px off its seat can read within it. Across reads that span a
 * whole swing the drawn top must reach seat.y - breath exactly, reach
 * seat.y + breath - 1 (or the top itself), and never pass either end (each
 * read through birdOnSeat): a bird 1 px off its seat moves the low end and
 * fails wherever the reads fall. Reads that do not span a swing fail too,
 * rather than pass on what they did not see. Returns a failure message, or
 * null.
 */
export function breathOnSeat(reads) {
  if (!reads || reads.length === 0) return "no reads of the bird in the scene (F89)";
  const seat = reads[0].seat;
  for (const r of reads) {
    if (JSON.stringify(r.seat) !== JSON.stringify(seat)) {
      return `the seat changed within the scene: ${JSON.stringify(seat)}, then ` +
        `${JSON.stringify(r.seat)} (F89)`;
    }
    const one = birdOnSeat(r);
    if (one !== null) return one;
  }
  const ys = reads.map((r) => r.bird.y);
  const lo = Math.min(...ys);
  const hi = Math.max(...ys);
  if (lo !== seat.y - seat.breath || hi < seat.y + seat.breath - 1) {
    return `over ${reads.length} reads the bird's top ran y ${lo}..${hi}; seated at y ${seat.y} ` +
      `it breathes from ${seat.y - seat.breath} to ${seat.y + seat.breath}, so a whole swing reaches ` +
      `${seat.y - seat.breath} and at least ${seat.y + seat.breath - 1} (F89)`;
  }
  return null;
}

/**
 * F89: did an idle flourish hop the bird during these reads? canary_mark's
 * idle bird hops now and then on its own (a flourish: every 25-60 s at most
 * x1.4, a hop about one time in six), and a hop lifts it at least 8 px
 * (12 px times the temperament, clamped to 8..18) for 560 ms. A run that
 * caught one cannot be held to the breath, so the probe reads the scene
 * again — once the hop is over, the next is 17 s or more away. A bird drawn
 * 8 px or more above its seat on every read also counts, and fails again on
 * the re-read. reads: as breathOnSeat takes them.
 */
export function flourishHop(reads) {
  return reads.some((r) => r.bird && r.bird.shown && r.seat && r.bird.y <= r.seat.y - 8);
}
