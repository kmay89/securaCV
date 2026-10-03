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

/**
 * Every frame the firmware drew landed on one glass (F184): the shape it
 * announced before its first frame, and no other shape after it. A saved
 * rotation is worn before the splash (main.cpp turns the dash glass right
 * after lvgl_port_init), so a turned boot's FIRST frame is already turned; a
 * firmware that turned the glass after the splash would draw the splash on
 * the panel's native landscape and only then turn, and every scene the walk
 * reads afterwards would still be the turned size. shapes: the harness's
 * __state.shapes, each {w, h, frames} with the frames drawn when it was
 * announced; frames: __state.flushes; glass: {w, h} the first frame must be
 * on (the turned glass), or null for "whatever it first was" (a native
 * walk). Returns a failure message, or null.
 */
export function framesOnGlass(shapes, frames, glass = null) {
  if (!(frames > 0)) return "the firmware has drawn no frame on the glass yet (F184)";
  const before = (shapes || []).filter((s) => s.frames === 0);
  const first = before.length ? before[before.length - 1] : null;
  if (!first) return "the firmware drew a frame before it announced its glass (F184)";
  const want = glass || first;
  if (first.w !== want.w || first.h !== want.h) {
    return `the firmware drew its first frame on a ${first.w}x${first.h} glass, not the ` +
      `${want.w}x${want.h} it wears from its saved rotation (F184)`;
  }
  const changed = shapes.find((s) => s.frames > 0 && (s.w !== want.w || s.h !== want.h));
  if (changed) {
    return `the glass changed to ${changed.w}x${changed.h} after ${changed.frames} frame(s) on ` +
      `${want.w}x${want.h} (F184)`;
  }
  return null;
}

/**
 * The QR card where the layout seats it (F184): card, the probe's joinCard()
 * answer (its box [x0, y0, x1, y1] the card's white pixels, inclusive);
 * layout, __emu.onboardJoin() (onboard_layout.h's stack, evaluated by the
 * firmware: card {x, y, side}). Each edge within 1 px (the anti-aliased
 * edge). Returns a failure message, or null.
 */
export function cardAtLayout(card, layout) {
  if (!layout) return "the firmware names no Join layout: no onboarding screen is up (F184)";
  if (!card) return "the Join scene shows no QR card (F184)";
  const c = layout.card;
  const want = [c.x, c.y, c.x + c.side - 1, c.y + c.side - 1];
  const off = card.box.map((v, i) => v - want[i]);
  if (off.some((d) => Math.abs(d) > 1)) {
    return `the QR card is drawn at ${JSON.stringify(card.box)}; onboard_layout.h's stack seats it at ` +
      `${JSON.stringify(want)} (${c.side} px), edges off by ${JSON.stringify(off)} (F184)`;
  }
  return null;
}

/**
 * The halo where the layout seats it (F184): halo, from haloOf() (the
 * circle lv_arc draws, emu_screen_arcs); layout, __emu.onboardJoin() (ring
 * {x, y, d, stroke}: the arc object's box and stroke as onboard_layout.h
 * names them). lv_arc draws about the box's center at half its side, the
 * stroke inward. Exact. Returns a failure message, or null.
 */
export function haloAtLayout(halo, layout) {
  if (!layout) return "the firmware names no Join layout: no onboarding screen is up (F184)";
  if (!halo) return "the Join scene shows no halo (F184)";
  const g = layout.ring;
  const r = Math.floor(g.d / 2);
  const want = { cx: g.x + r, cy: g.y + r, r, stroke: g.stroke };
  if (halo.cx !== want.cx || halo.cy !== want.cy || halo.r !== want.r || halo.stroke !== want.stroke) {
    return `the halo is drawn about ${halo.cx},${halo.cy} (r ${halo.r}, stroke ${halo.stroke}); ` +
      `onboard_layout.h's stack seats it about ${want.cx},${want.cy} (r ${want.r}, stroke ${want.stroke}) (F184)`;
  }
  return null;
}

// ── Reads off the framebuffer ────────────────────────────────────────────
// Each takes fr = {w, h, data} (the canvas's RGBA, as getImageData returns
// it) and is self-contained, so the probe evaluates its source in the page
// on the live canvas and the unit tests call it on a synthetic one.

/**
 * The join QR's finder patterns (F184): inside the card's box, the QR's dark
 * modules' bounding box, and at each of its corners whether the 7x7 finder
 * (and its light separator) stands there, the module pitch read off the
 * corner's edge run. A phone finds a QR by these three; an upright code has
 * them at top-left, top-right and bottom-left and none at bottom-right, and
 * a glass drawn mirrored, upside down or flipped moves one to bottom-right.
 * card: the joinCard() box [x0, y0, x1, y1]. Returns {box, module, tl, tr,
 * bl, br}, or null when the card holds no dark module. cornerR: the card's
 * corner radius (onboard_layout.h's kCardRadius).
 */
export function qrFinders(fr, card, cornerR = 10) {
  const { w, data } = fr;
  const dark = (x, y) => {
    const i = (y * w + x) * 4;
    return data[i] < 128 && data[i + 1] < 128 && data[i + 2] < 128;
  };
  // The glass behind the card's rounded corners (past cornerR, less a px of
  // anti-aliasing, from each corner arc's center) is not the QR's.
  const left = card[0], top = card[1], right = card[2] + 1, bottom = card[3] + 1;
  const behind = (x, y) => {
    const px = x + 0.5, py = y + 0.5;
    const ax = px < left + cornerR ? left + cornerR : px > right - cornerR ? right - cornerR : null;
    const ay = py < top + cornerR ? top + cornerR : py > bottom - cornerR ? bottom - cornerR : null;
    return ax !== null && ay !== null && Math.hypot(px - ax, py - ay) > cornerR - 1;
  };
  let x0 = Infinity, y0 = Infinity, x1 = -1, y1 = -1;
  for (let y = card[1]; y <= card[3]; y++) {
    for (let x = card[0]; x <= card[2]; x++) {
      if (behind(x, y) || !dark(x, y)) continue;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
  if (x1 < 0) return null;
  // sx, sy: +1 inward from a left/top edge, -1 from a right/bottom one.
  const finder = (sx, sy) => {
    const ox = sx > 0 ? x0 : x1, oy = sy > 0 ? y0 : y1;
    let run = 0;
    while (run <= x1 - x0 && dark(ox + sx * run, oy)) run++;
    const m = run / 7;
    if (run < 7 || run % 7 !== 0) return { finder: false, module: m };
    for (let j = 0; j < 8; j++) {
      for (let i = 0; i < 8; i++) {
        const want = i < 7 && j < 7 &&
          (i === 0 || i === 6 || j === 0 || j === 6 || (i >= 2 && i <= 4 && j >= 2 && j <= 4));
        const px = ox + sx * Math.floor(i * m + m / 2), py = oy + sy * Math.floor(j * m + m / 2);
        if (px < x0 || px > x1 || py < y0 || py > y1 || dark(px, py) !== want) return { finder: false, module: m };
      }
    }
    return { finder: true, module: m };
  };
  const tl = finder(1, 1), tr = finder(-1, 1), bl = finder(1, -1), br = finder(-1, -1);
  const found = [tl, tr, bl, br].find((c) => c.finder);
  return { box: [x0, y0, x1, y1], module: found ? found.module : 0,
    tl: tl.finder, tr: tr.finder, bl: bl.finder, br: br.finder };
}

/**
 * The QR stands upright on the glass, as a phone reads it (F184): finders at
 * top-left, top-right and bottom-left, none at bottom-right. f: qrFinders().
 * A mirrored glass puts the finders at the top two corners and bottom-right,
 * an upside-down or flipped one at bottom-right too. Returns a failure
 * message, or null.
 */
export function qrUpright(f) {
  if (!f) return "the Join scene's card holds no QR code (F184)";
  if (f.tl && f.tr && f.bl && !f.br) return null;
  const at = ["tl", "tr", "bl", "br"].filter((k) => f[k]).join(", ") || "no corner";
  return `the join QR's finder patterns stand at ${at} of ${JSON.stringify(f.box)}, not top-left, ` +
    `top-right and bottom-left: the glass is drawn mirrored or turned (F184)`;
}

/**
 * The halo's stroke as the glass shows it (F184): halo from haloOf() (cx,
 * cy, r, stroke: the stroke covers radius r - stroke .. r). On each axis,
 * either side of the center: the brightest pixel across the stroke, and the
 * pixel 3 px outside it. Returns {stroke: [4], outside: [4]} (right, left,
 * down, up), each the brightest channel, -1 off the glass.
 */
export function haloInk(fr, halo) {
  const { w, h, data } = fr;
  const lum = (x, y) => {
    if (x < 0 || y < 0 || x >= w || y >= h) return -1;
    const i = (y * w + x) * 4;
    return Math.max(data[i], data[i + 1], data[i + 2]);
  };
  const dirs = [[1, 0], [-1, 0], [0, 1], [0, -1]];
  const stroke = dirs.map(([dx, dy]) => {
    let best = -1;
    for (let d = halo.r - halo.stroke; d <= halo.r - 1; d++) best = Math.max(best, lum(halo.cx + dx * d, halo.cy + dy * d));
    return best;
  });
  const outside = dirs.map(([dx, dy]) => lum(halo.cx + dx * (halo.r + 3), halo.cy + dy * (halo.r + 3)));
  return { stroke, outside };
}

/**
 * The halo the firmware reports is the one the glass shows (F184): on every
 * axis its stroke is inked (brighter than the glass 3 px outside it, and
 * than the background, by more than `margin`) and nothing is inked 3 px
 * outside it. Where that outside pixel is past the glass's edge (-1 from
 * haloInk) nothing can be inked there, and the stroke is held against the
 * background alone: the landscape nightlight's halo stands 2 px from the
 * panel's right edge (F157, F204). A circle reported wider or narrower than
 * lv_arc drew, or with no stroke, reads the glass's background (or nothing)
 * where the stroke should be. ink: haloInk(); bg: the glass's background
 * brightness. Returns a failure message, or null.
 */
export function haloInked(ink, bg = 0, margin = 6) {
  const names = ["right", "left", "bottom", "top"];
  const bad = names.filter((n, i) => {
    const out = ink.outside[i];
    return !(ink.stroke[i] > Math.max(out, bg) + margin && (out < 0 || out <= bg + margin));
  });
  if (!bad.length) return null;
  return `the halo the firmware reports is not the one on the glass at its ${bad.join(", ")} ` +
    `(stroke ${JSON.stringify(ink.stroke)}, 3 px outside ${JSON.stringify(ink.outside)}) (F184)`;
}

/**
 * Ink inside every line that draws (F184): for each label that draws whole
 * on the glass (shown, faded in, with text, its box inside the glass), the
 * number of pixels inside its box brighter than 40 (the probe's text ink:
 * the halo at its brightest stays under it). A blank glass, or one drawn
 * mirrored or flipped so its lines land elsewhere, leaves boxes empty.
 * labels: __emu.screenLabels(). Returns [{text, box, ink}].
 */
export function linesInk(fr, labels) {
  const { w, h, data } = fr;
  return labels.filter((l) => l.shown && l.opa >= 250 && l.text.trim() !== "" && l.x >= 0 && l.y >= 0 &&
    l.x + l.w <= w && l.y + l.h <= h).map((l) => {
    let ink = 0;
    for (let y = l.y; y < l.y + l.h; y++) {
      for (let x = l.x; x < l.x + l.w; x++) {
        const i = (y * w + x) * 4;
        if (Math.max(data[i], data[i + 1], data[i + 2]) > 40) ink++;
      }
    }
    return { text: l.text, box: [l.x, l.y, l.w, l.h], ink };
  });
}

/**
 * Every line that draws has ink on the glass where the firmware says it is
 * (F184). reads: linesInk(). Returns a failure message, or null.
 */
export function linesInked(reads) {
  const empty = reads.filter((r) => !(r.ink > 0));
  if (!empty.length) return null;
  return `${empty.length} line(s) the firmware draws show no ink where it says they are: ` +
    `${JSON.stringify(empty.map((r) => ({ text: r.text, box: r.box })))} (F184)`;
}
