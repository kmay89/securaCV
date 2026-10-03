// canary-local/tests/onboard_glass.mjs — what the onboarding probe holds on
// the glass beyond the bird (F184): every line of text on the glass and none
// cut, on every walk, and on a turned glass the Join scene's QR card inside
// the halo where the layout puts it.
//
// The emulator boots the dash flavor with a saved portrait rotation (the
// harness's ?rotation=1), so main.cpp turns the glass before the splash and
// the onboarding, and the scenes run on the 480x800 canvas a portrait dash
// shows (F156). These read what the firmware drew: the labels as it holds
// them (__emu.screenLabels()), the halo as the circle lv_arc draws
// (__emu.screenArcs()), and the card off the framebuffer (the probe's
// joinCard). Pure functions, held by onboard.test.js.

/**
 * Every line that draws (shown, visible, with text) inside the glass.
 * labels: from __emu.screenLabels(); glass: {w, h}, the canvas. Returns a
 * failure message naming each line whose box leaves the glass, or null.
 */
export function linesOnGlass(labels, glass) {
  const off = labels.filter((l) => l.shown && l.opa > 0 && l.text.trim() !== "" &&
    (l.x < 0 || l.y < 0 || l.x + l.w > glass.w || l.y + l.h > glass.h));
  if (!off.length) return null;
  return `${off.length} line(s) leave the ${glass.w}x${glass.h} glass: ` +
    `${JSON.stringify(off.map((l) => ({ text: l.text, box: [l.x, l.y, l.w, l.h] })))} (F184)`;
}

/**
 * No line that draws is cut to an ellipsis. LVGL 8 (the emulator's) cuts a
 * LONG_DOT label by rewriting its own text to end in "...", so the text says
 * so even where the framebuffer read cannot: the probe's joinEllipses takes
 * periods of at most 4 px, and the 36 px title face the 800 px glass's Join
 * title wears draws 5 px ones — the portrait dash's cut title before F156,
 * "Scan with your phone...", passed it. A network name shortened on purpose
 * (onboard_layout.h's name_line) keeps its tail after the "..."; the probe's
 * names are short. labels: from __emu.screenLabels(). Returns a failure
 * message naming each cut line, or null.
 */
export function linesCut(labels) {
  const cut = labels.filter((l) => l.shown && l.opa > 0 && l.text.trimEnd().endsWith("..."));
  if (!cut.length) return null;
  return `${cut.length} line(s) cut to an ellipsis: ${JSON.stringify(cut.map((l) => l.text))} (F184)`;
}

/**
 * The onboarding halo: the largest shown arc on the glass (the scene's one
 * ring), or null. arcs: from __emu.screenArcs().
 */
export function haloOf(arcs) {
  const shown = (arcs || []).filter((a) => a.shown && a.r > 0);
  if (!shown.length) return null;
  return shown.reduce((a, b) => (b.r > a.r ? b : a));
}

/**
 * The Join scene's QR card inside its halo. card: the probe's joinCard()
 * answer, its box [x0, y0, x1, y1] the card's white pixels (inclusive); halo:
 * from haloOf(), the circle about (cx, cy) whose stroke covers radius
 * r - stroke .. r (lv_arc draws it over [cx - r, cx + r) on each axis). The
 * card's corners are rounded by cornerR (onboard_layout.h's kCardRadius).
 *
 * Always held: the card standing over the halo's center (the center inside
 * its box; the card need not be centered: under Heirloom the 800 px glass's
 * Join stack sets it 5 px above, F155) and the midpoint of each of its sides
 * inside the stroke's inner edge. With corners, also held: the farthest point of its
 * rounded corners inside that edge — what the landscape nightlight's layout
 * promises (its corners 2 px inside the stroke, F157), and what the 800 px
 * dash glass does not in either orientation: there the corners reach past
 * the ring (F155, an open decision; F156). Returns {fail, offset, sides,
 * reach, inner}: the measures in pixels, and a failure message or null.
 */
export function cardInHalo(card, halo, { corners = false, cornerR = 10 } = {}) {
  if (!card) return { fail: "the Join scene shows no QR card (F184)" };
  if (!halo) return { fail: "the Join scene shows no halo (F184)" };
  const [x0, y0, x1, y1] = card.box;
  const left = x0, top = y0, right = x1 + 1, bottom = y1 + 1;
  const inner = halo.r - halo.stroke;
  const offset = [(left + right) / 2 - halo.cx, (top + bottom) / 2 - halo.cy];
  // Each side's midpoint, as far from the halo's center as it stands.
  const mx = (left + right) / 2, my = (top + bottom) / 2;
  const sides = Math.max(
    Math.hypot(mx - halo.cx, top - halo.cy), Math.hypot(mx - halo.cx, bottom - halo.cy),
    Math.hypot(left - halo.cx, my - halo.cy), Math.hypot(right - halo.cx, my - halo.cy));
  // The rounded corners' farthest reach: each corner arc's center plus its radius.
  let reach = 0;
  for (const ccx of [left + cornerR, right - cornerR]) {
    for (const ccy of [top + cornerR, bottom - cornerR]) {
      reach = Math.max(reach, Math.hypot(ccx - halo.cx, ccy - halo.cy) + cornerR);
    }
  }
  const at = `card ${JSON.stringify(card.box)} in the halo about ${halo.cx},${halo.cy} ` +
    `(r ${halo.r}, stroke ${halo.stroke}, inner edge ${inner})`;
  const r1 = (v) => Math.round(v * 10) / 10;
  let fail = null;
  if (!(left < halo.cx && halo.cx < right && top < halo.cy && halo.cy < bottom)) {
    fail = `the QR card does not stand over its halo's center: ${at} (F184)`;
  } else if (sides > inner) {
    fail = `the QR card's sides reach ${r1(sides)} px from the halo's center, past its stroke: ${at} (F184)`;
  } else if (corners && reach > inner) {
    fail = `the QR card's corners reach ${r1(reach)} px from the halo's center, past its stroke: ${at} (F184)`;
  }
  return { fail, offset, sides: r1(sides), reach: r1(reach), inner };
}
