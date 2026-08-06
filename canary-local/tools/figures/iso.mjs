// iso.mjs — the fleet's one isometric camera.
//
// Every physical thing SecuraCV talks about gets a figure: the same object,
// drawn from the same viewpoint, under the same light, in the same palette,
// at every size and on every surface (glass, wrist, phone, web, emulator).
// That sameness is the whole point — a user should recognize "the radome" or
// "the wall plate" instantly, without reading a word. See
// docs/design/FLEET_FIGURES.md.
//
// This module is the projector and nothing else: massing (mm) in, SVG out.
// It is PURE and DETERMINISTIC — no clock, no randomness, no filesystem, no
// float formatting that drifts between platforms — because the output is
// committed and CI regenerates it and diffs the bytes.
//
// ── Frame (documented once, obeyed everywhere) ───────────────────────────
//   +X = right   +Y = the thing's FRONT (toward the viewer's left)   +Z = up
//   Units are millimeters, matching the SCAD sources and the STLs.
//   The camera sits at (+X, +Y, +Z) looking at the origin: a true 30°
//   isometric, so the three visible faces are +X (right), +Y (front) and
//   +Z (top). A device is authored front-toward-+Y so its face reads.
//
// ── Why SVG and not a 3D scene ───────────────────────────────────────────
//   A watch complication, a 20 px list row and a 900 px hero all need the
//   same picture. Vector costs a few KB, needs no GPU, no model loader and
//   no decoder, scales without mud, and renders identically in SwiftUI,
//   a WKWebView, the emulator and a static page. The .glb models in the
//   website's models/ stay what they are — the AR "view in your room" path.

/* ------------------------------------------------------------------ camera */

const COS30 = Math.sqrt(3) / 2; // 0.8660254037844386
const SIN30 = 0.5;

// World (mm, Z up) -> screen (SVG, y down). Uniform scale is applied later.
export function project(x, y, z) {
  return [(x - y) * COS30, (x + y) * SIN30 - z];
}

// Depth along the view axis. The camera is at (+X,+Y,+Z), so a larger sum is
// nearer and must be painted later.
export function depthKey(x, y, z) {
  return x + y + z;
}

// Light rig. One direction for the whole fleet, chosen so the three visible
// faces land on three clearly separated tones: top brightest, right face
// mid, front face darkest. Flat facets, no gradients — this is technical
// illustration, not a render.
const LIGHT = (() => {
  const v = [0.5, -0.35, 0.79];
  const n = Math.hypot(...v);
  return v.map((c) => c / n);
})();

const AMBIENT = 0.42;
const DIFFUSE = 0.58;

// The rig as data, for the generator to carry into other surfaces' copies
// (the Swift massing file) — so a port shades with these exact numbers
// instead of re-deriving them and drifting by an ulp.
export const LIGHT_RIG = { light: LIGHT, ambient: AMBIENT, diffuse: DIFFUSE };

export function shade(nx, ny, nz) {
  const d = nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2];
  return AMBIENT + DIFFUSE * Math.max(0, Math.min(1, d));
}

/* ---------------------------------------------------------------- palette
 * Named materials, not per-figure colors: a printed shell is the same gray
 * in every figure in the fleet, so "the same part" looks like the same part
 * across the doorbell, the combo and the exploded view. Colors are the
 * physical object's, NOT the UI theme's — a case is off-white whether the
 * app is in light or dark mode. Only the outline adapts (see `stroke`). */

export const MATERIALS = {
  shell:  { base: [0xe8, 0xe6, 0xe0], name: 'printed shell' },
  shell2: { base: [0x9a, 0x9d, 0xa4], name: 'secondary printed part' },
  dark:   { base: [0x3a, 0x3d, 0x44], name: 'dark printed part' },
  gasket: { base: [0x4a, 0x40, 0x52], name: 'TPU gasket' },
  glass:  { base: [0x1b, 0x1f, 0x2a], name: 'glass / screen' },
  lit:    { base: [0x3d, 0x6f, 0xa8], name: 'lit screen' },
  board:  { base: [0x1f, 0x4a, 0x38], name: 'PCB' },
  lens:   { base: [0x14, 0x18, 0x22], name: 'camera lens' },
  metal:  { base: [0xb8, 0xbc, 0xc4], name: 'hardware' },
  accent: { base: [0xf0, 0xb4, 0x00], name: 'canary accent' },
  radome: { base: [0xd8, 0xd4, 0xc8], name: 'radar-transparent window' },
};

const hex = (n) => Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, '0');

function toneOf(material, k) {
  const m = MATERIALS[material];
  if (!m) throw new Error(`iso: unknown material "${material}"`);
  return `#${m.base.map((c) => hex(c * k)).join('')}`;
}

/* --------------------------------------------------------------- outlines
 * Every solid is a prism: a closed plan outline in XY, extruded along Z.
 * A box is a rounded rectangle; a cylinder is a circle. Unifying them means
 * one silhouette/extrusion path handles both, and a rounded box degrades to
 * a circle continuously (r = w/2 = d/2) with no special case. */

// Fixed vertex budgets keep the output byte-identical run to run.
const ARC_SEG = 6;   // per 90° corner of a rounded rect
const CIRCLE_SEG = 32;

function ringRect(x0, y0, w, d, r) {
  r = Math.min(r, w / 2, d / 2);
  if (r <= 1e-6) {
    return [[x0, y0], [x0 + w, y0], [x0 + w, y0 + d], [x0, y0 + d]];
  }
  const pts = [];
  // CCW seen from +Z, starting bottom-right corner.
  const corners = [
    [x0 + w - r, y0 + r, -Math.PI / 2, 0],
    [x0 + w - r, y0 + d - r, 0, Math.PI / 2],
    [x0 + r, y0 + d - r, Math.PI / 2, Math.PI],
    [x0 + r, y0 + r, Math.PI, 1.5 * Math.PI],
  ];
  for (const [cx, cy, a0, a1] of corners) {
    for (let i = 0; i <= ARC_SEG; i++) {
      const a = a0 + (a1 - a0) * (i / ARC_SEG);
      pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]);
    }
  }
  return pts;
}

function ringCircle(cx, cy, r) {
  const pts = [];
  for (let i = 0; i < CIRCLE_SEG; i++) {
    const a = (i / CIRCLE_SEG) * Math.PI * 2;
    pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]);
  }
  return pts;
}

/* ----------------------------------------------------------------- solids
 * The massing vocabulary. Placement is MIN-CORNER + size, not center: that
 * is how a bounding box reads off an STL, which is where these numbers come
 * from (see stlbox.mjs), so the spec can be diffed against the CAD directly.
 *
 *   box  { at:[x,y,z], size:[w,d,h], r?, face? }
 *        at = min corner. `r` rounds the outline; `face:'y'` rounds it in
 *        XZ instead of XY, which is what you want for a screen, a window or
 *        any thin plate ON a front face rather than lying flat.
 *   cyl  { at:[x,y,z], r, h, axis? }   at = center of the base cap
 *   disc { … }                          a cyl, spelled for lenses and knobs
 *        axis 'z' (default) stands it up; axis 'y' lays it along the view
 *        axis — a lens, a button, a status LED looking at the viewer.
 *
 * Every solid carries `m` (material) and may carry `detail:'full'` to be
 * dropped from the glyph render.
 *
 * Internally all four are ONE thing: a closed outline in two of the world
 * axes, extruded along the third. Keeping a single representation is why
 * the silhouette, the shading and the depth sort each exist once. */

// Extrusion axis -> the two outline axes, and how (u,v,w) maps to (x,y,z).
const AXES = {
  z: { u: 0, v: 1, w: 2, to: (u, v, w) => [u, v, w], capN: [0, 0, 1], sideN: (nu, nv) => [nu, nv, 0] },
  y: { u: 0, v: 2, w: 1, to: (u, v, w) => [u, w, v], capN: [0, 1, 0], sideN: (nu, nv) => [nu, 0, nv] },
};

function prismOf(s) {
  const axis = s.kind === 'box' ? (s.face === 'y' ? 'y' : 'z') : (s.axis ?? 'z');
  const A = AXES[axis];
  if (!A) throw new Error(`iso: unknown axis "${axis}"`);
  let ring;
  let a0;
  let span;
  if (s.kind === 'box') {
    ring = ringRect(s.at[A.u], s.at[A.v], s.size[A.u], s.size[A.v], s.r ?? 0);
    a0 = s.at[A.w];
    span = s.size[A.w];
  } else if (s.kind === 'cyl' || s.kind === 'disc') {
    ring = ringCircle(s.at[A.u], s.at[A.v], s.r);
    a0 = s.at[A.w];
    span = s.h;
  } else {
    throw new Error(`iso: unknown solid kind "${s.kind}"`);
  }
  return { A, ring, a0, a1: a0 + span };
}

// The solid's outline as world-space points on its far (visible) cap.
function ringOf(s) {
  const { A, ring, a1 } = prismOf(s);
  return ring.map(([u, v]) => A.to(u, v, a1));
}

function heightOf(s) {
  return s.kind === 'box' ? s.size[2] : (s.axis === 'y' ? 2 * s.r : s.h);
}

function centerOf(s) {
  const { A, ring, a0, a1 } = prismOf(s);
  let su = 0;
  let sv = 0;
  for (const [u, v] of ring) { su += u; sv += v; }
  return A.to(su / ring.length, sv / ring.length, (a0 + a1) / 2);
}

/* ------------------------------------------------------------------ faces */

// The visible faces of one convex prism, already shaded. Within a convex
// solid the visible faces tile its silhouette exactly and never overlap, so
// they need no sorting among themselves — sides first, then the near cap.
function facesOf(s) {
  const { A, ring, a0, a1 } = prismOf(s);
  const out = [];
  const n = ring.length;
  for (let i = 0; i < n; i++) {
    const a = ring[i];
    const b = ring[(i + 1) % n];
    const eu = b[0] - a[0];
    const ev = b[1] - a[1];
    const len = Math.hypot(eu, ev);
    if (len < 1e-9) continue;
    // Outward normal of a CCW ring edge, lifted into world axes.
    const N = A.sideN(ev / len, -eu / len);
    if (N[0] + N[1] + N[2] <= 1e-9) continue; // faces away from the camera
    out.push({
      pts: [A.to(a[0], a[1], a0), A.to(b[0], b[1], a0), A.to(b[0], b[1], a1), A.to(a[0], a[1], a1)],
      k: shade(...N),
    });
  }
  // The cap at the far end of the extrusion axis is the one facing a camera
  // that sits on the +X/+Y/+Z side of everything.
  out.push({ pts: ring.map(([u, v]) => A.to(u, v, a1)), k: shade(...A.capN) });
  return out;
}

/* -------------------------------------------------------------- rendering */

// Fixed 3-decimal formatting: enough for sub-pixel accuracy at any sane
// size, and stable across platforms so the committed bytes never churn.
const f = (v) => {
  const s = (Math.round(v * 1000) / 1000).toFixed(3);
  return s.replace(/\.?0+$/, '') || '0';
};

/**
 * Plan one figure: the ordered list of flat polygons that make it up, with
 * their fills, in viewBox coordinates.
 *
 * This is the ONE place the geometry is decided. Both emitters — the .svg
 * files the web and the emulator load, and the SwiftUI path data the phone
 * and the wrist draw — serialize this same plan, so the two can't drift into
 * showing a user two different pictures of one object. That lockstep is the
 * point; see docs/design/FLEET_FIGURES.md §"One plan, many surfaces".
 *
 * @param {object} figure   { id, solids:[...], ground?:boolean }
 * @param {object} opts
 *   size     target square viewBox edge (default 256)
 *   pad      padding in viewBox units (default 8)
 *   detail   'full' | 'glyph'  — glyph drops solids marked detail:'full'
 *   ghost    true plans an unbuilt thing as a dashed wireframe: no fill,
 *            no shading, no shadow. An idea can never be mistaken for a
 *            product. This is an honesty invariant, not a style — see
 *            tests/figures.test.js.
 * @returns {{size:number, ops:Array<{kind:'shadow'|'face'|'ghost',
 *            pts:number[][], fill?:string, stroke?:string}>}}
 */
export function planFigure(figure, opts = {}) {
  const size = opts.size ?? 256;
  const pad = opts.pad ?? 8;
  const detail = opts.detail ?? 'full';
  const ghost = !!opts.ghost;

  const solids = figure.solids.filter(
    (s) => detail !== 'glyph' || (s.detail ?? 'glyph') === 'glyph',
  );
  if (solids.length === 0) throw new Error(`iso: figure "${figure.id}" has no solids at detail=${detail}`);

  // The contact shadow: the footprint at z=0, flattened. Grounds the object
  // the way the fleet's product shots do. Computed BEFORE the fit, and folded
  // into it — a shadow laid out afterwards can fall outside the viewBox and be
  // clipped, which is what happened to the round display (a point at y=293.8
  // in a 256 box). Everything drawn has to be inside the frame it declares.
  const wantsShadow = !ghost && detail !== 'glyph' && figure.ground !== false;
  let shadowRing = null;
  if (wantsShadow) {
    let fx0 = Infinity; let fy0 = Infinity; let fx1 = -Infinity; let fy1 = -Infinity;
    for (const s of solids) {
      for (const [x, y] of cornersOf(s)) {
        if (x < fx0) fx0 = x;
        if (x > fx1) fx1 = x;
        if (y < fy0) fy0 = y;
        if (y > fy1) fy1 = y;
      }
    }
    const pad2 = Math.max(fx1 - fx0, fy1 - fy0) * 0.06;
    shadowRing = ringRect(fx0 - pad2, fy0 - pad2, (fx1 - fx0) + 2 * pad2,
      (fy1 - fy0) + 2 * pad2, Math.min(fx1 - fx0, fy1 - fy0) * 0.35 + pad2);
  }

  // Fit: project every solid's ring at both z levels, plus the shadow, and
  // take the screen bbox.
  let minX = Infinity; let minY = Infinity; let maxX = -Infinity; let maxY = -Infinity;
  const see = (x, y, z) => {
    const [px, py] = project(x, y, z);
    if (px < minX) minX = px;
    if (px > maxX) maxX = px;
    if (py < minY) minY = py;
    if (py > maxY) maxY = py;
  };
  for (const s of solids) for (const [x, y, z] of cornersOf(s)) see(x, y, z);
  if (shadowRing) for (const [x, y] of shadowRing) see(x, y, 0);

  const span = Math.max(maxX - minX, maxY - minY, 1e-6);
  const sc = (size - 2 * pad) / span;
  // Center the projected bbox in the square viewBox.
  const ox = pad + ((size - 2 * pad) - (maxX - minX) * sc) / 2 - minX * sc;
  const oy = pad + ((size - 2 * pad) - (maxY - minY) * sc) / 2 - minY * sc;

  const ops = [];
  const flat = (pts) => pts.map(([x, y, z]) => {
    const [px, py] = project(x, y, z);
    return [px * sc + ox, py * sc + oy];
  });

  // Skipped for ghosts (nothing is there to cast one) and for glyphs (it
  // muddies at 20 px) — see wantsShadow above.
  if (shadowRing) {
    ops.push({ kind: 'shadow', pts: flat(shadowRing.map(([x, y]) => [x, y, 0])) });
  }

  // Painter's algorithm across solids, nearest last. Ties break on id so the
  // order is total and the bytes are reproducible.
  const ordered = solids
    .map((s, i) => ({ s, i, key: depthKey(...centerOf(s)) }))
    .sort((a, b) => (a.key - b.key) || (a.i - b.i));

  for (const { s } of ordered) {
    if (ghost) {
      // Wireframe: the visible edges only, dashed. No fill, no tones.
      for (const face of facesOf(s)) ops.push({ kind: 'ghost', pts: flat(face.pts) });
      continue;
    }
    for (const face of facesOf(s)) {
      ops.push({
        kind: 'face',
        pts: flat(face.pts),
        fill: toneOf(s.m, face.k),
        stroke: toneOf(s.m, face.k * 0.82),
      });
    }
  }

  return { size, ops };
}

/** Serialize a plan as a COMPACT SVG, for drawing at list-row size.
 *
 * Same geometry, three savings that only make sense small:
 *   · faces sharing a fill collapse into one <path> with several subpaths,
 *     which is where nearly all the bytes are — a figure is ~44 faces but
 *     only a handful of distinct tones, and each <path> otherwise repeats
 *     its own fill/stroke/width attributes
 *   · the hairline stroke is dropped; at 46 px it is below a pixel and only
 *     muddies the silhouette
 *   · coordinates round to 1 decimal, which is ~0.02 px at that size
 *
 * This is what rides inside the flasher catalog, where every byte is
 * embedded in a desktop binary as well as fetched by a browser.
 */
export function renderFigureCompact(figure, opts = {}) {
  const { size, ops } = planFigure(figure, { detail: 'glyph', ...opts });
  const f1 = (v) => {
    const r = Math.round(v * 10) / 10;
    return Number.isInteger(r) ? String(r) : r.toFixed(1);
  };
  const d = (pts) => pts.map(([x, y], i) => `${i ? 'L' : 'M'}${f1(x)} ${f1(y)}`).join('') + 'Z';
  // Preserve paint order: a fill's run is emitted at the position of its
  // FIRST face. Merging out of order would put a near face behind a far one.
  const runs = [];
  const byFill = new Map();
  for (const op of ops) {
    const fill = op.kind === 'face' ? op.fill
      : op.kind === 'shadow' ? 'var(--scv-figure-shadow, rgba(16,18,24,.14))' : null;
    if (fill === null) { runs.push({ ghost: true, d: d(op.pts) }); continue; }
    let run = byFill.get(fill);
    if (!run) { run = { fill, d: '' }; byFill.set(fill, run); runs.push(run); }
    run.d += d(op.pts);
  }
  const body = runs.map((r) => (r.ghost
    ? `<path d="${r.d}" fill="none" stroke="var(--scv-figure-ghost, #8b90a0)" stroke-width="1.25" stroke-dasharray="3 2.5"/>`
    : `<path d="${r.d}" fill="${r.fill}"/>`)).join('');
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${size} ${size}" `
    + `role="img" aria-label="${escapeXml(figure.title || figure.id)}">${body}</svg>`;
}

/** Serialize a plan as a standalone, self-contained SVG document. */
export function renderFigure(figure, opts = {}) {
  const { size, ops } = planFigure(figure, opts);
  const d = (pts) => pts.map(([x, y], i) => `${i ? 'L' : 'M'}${f(x)} ${f(y)}`).join('') + 'Z';
  const body = ops.map((op) => {
    if (op.kind === 'shadow') {
      return `<path d="${d(op.pts)}" fill="var(--scv-figure-shadow, rgba(16,18,24,.14))"/>`;
    }
    if (op.kind === 'ghost') {
      return `<path d="${d(op.pts)}" fill="none" stroke="var(--scv-figure-ghost, #8b90a0)" `
        + 'stroke-width="1.25" stroke-dasharray="3 2.5" stroke-linejoin="round"/>';
    }
    return `<path d="${d(op.pts)}" fill="${op.fill}" stroke="${op.stroke}" `
      + 'stroke-width=".5" stroke-linejoin="round"/>';
  });
  const title = figure.title ? `<title>${escapeXml(figure.title)}</title>` : '';
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${size} ${size}" `
    + `width="${size}" height="${size}" role="img" `
    + `aria-label="${escapeXml(figure.title || figure.id)}">${title}`
    + `<g shape-rendering="geometricPrecision">${body.join('')}</g></svg>\n`;
}

function escapeXml(s) {
  return String(s).replace(/[<>&"']/g, (c) => (
    { '<': '&lt;', '>': '&gt;', '&': '&amp;', '"': '&quot;', "'": '&apos;' }[c]
  ));
}

/* ------------------------------------------------------------ geometry aid
 * Exported for the generator's drift guard and the coplanar test. */

// Every world-space point of a solid's two caps — the full corner set, so a
// bounding box built from it is exact for any extrusion axis.
export function cornersOf(s) {
  const { A, ring, a0, a1 } = prismOf(s);
  const out = [];
  for (const [u, v] of ring) { out.push(A.to(u, v, a0)); out.push(A.to(u, v, a1)); }
  return out;
}

export function envelopeOf(solids) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (const s of solids) {
    for (const p of cornersOf(s)) {
      for (let i = 0; i < 3; i++) {
        if (p[i] < min[i]) min[i] = p[i];
        if (p[i] > max[i]) max[i] = p[i];
      }
    }
  }
  return { min, max, size: [max[0] - min[0], max[1] - min[1], max[2] - min[2]] };
}

export { ringOf, heightOf, centerOf, prismOf };
