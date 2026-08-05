#!/usr/bin/env node
// gen_figures.mjs — build the fleet's figure ledger and every surface's copy.
//
//   node canary-local/tools/figures/gen_figures.mjs [--check]
//
// Emits, all generated, all committed, none hand-edited:
//   canary-local/devices/figures.json      the ledger (identity + evidence)
//   canary-local/figures/<id>.svg          the full figure
//   canary-local/figures/<id>.glyph.svg    the small-size figure
//   firmware/common/core/fleet_figures.h   what a device says it is
//   ios/Shared/FleetFigures.swift          what the phone and wrist draw
//
// `--check` regenerates into memory and fails if anything on disk differs.
// That is the CI gate: edit massing.mjs or re-export an STL without running
// this, and the build stops. See docs/design/FLEET_FIGURES.md.

import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

import { renderFigure, renderFigureCompact, planFigure, envelopeOf, cornersOf } from './iso.mjs';
import { stlBounds } from './stlbox.mjs';
import { FIGURES, conceptFigure } from './massing.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '../../..');
const ENCLOSURE = join(ROOT, 'docs/hardware/enclosure');
const OUT_JSON = join(ROOT, 'canary-local/devices/figures.json');
const OUT_SVG = join(ROOT, 'canary-local/figures');
const OUT_H = join(ROOT, 'firmware/common/core/fleet_figures.h');
const OUT_SWIFT = join(ROOT, 'ios/Shared/FleetFigures.swift');
// The picker tier: one compact SVG per figure, small enough to be EMBEDDED
// in the flasher catalog — which the desktop app bakes into its binary at
// build time (desktop/src-tauri/build.rs), so a file on disk is not a
// channel that reaches it. gen_flash.py inlines the ones it needs.
const OUT_PICKER = join(ROOT, 'canary-local/devices/figures.picker.json');

const CHECK = process.argv.includes('--check');

// Bump when the projector, palette or light rig changes: every rev in the
// ledger moves, which is exactly the signal a surface needs to drop a cached
// figure it drew with the old camera.
const PROJECTOR_REV = 1;

const registry = JSON.parse(readFileSync(join(ROOT, 'canary-local/devices/registry.json'), 'utf8'));
const catalog = JSON.parse(readFileSync(join(ROOT, 'canary-local/devices/catalog.json'), 'utf8'));
const boards = JSON.parse(readFileSync(join(ROOT, 'canary-local/devices/boards.json'), 'utf8'));

/* ─────────────────────────────────────────────────────────── the ladder
 * "Which of these are real and which are still ideas?" is the question the
 * catalog has never been able to answer in one place, because status lived
 * in three vocabularies that didn't agree. One ladder, derived from evidence
 * that exists on disk — never hand-typed, so it cannot flatter itself:
 *
 *   shipping   you can print it and flash it today: committed STLs + a
 *              firmware config + a released catalog variant
 *   confirmed  the design is settled and released in the catalog, but one
 *              of those three proofs is still missing
 *   prototype  it exists, in development — CAD or firmware, not both, and
 *              nothing committed to print
 *   idea       a research note and a sketch. Nothing is built. Renders as a
 *              ghost so it can never be mistaken for a product.
 *
 * The evidence is written into the ledger next to the verdict so anyone can
 * check the ladder's arithmetic instead of trusting it. */

const LADDER = ['idea', 'prototype', 'confirmed', 'shipping'];

function firmwareConfigs(deviceId) {
  // firmware/configs/<family>/<flavor>/config.h — the flavors that exist.
  const family = deviceId.startsWith('canary-display') ? 'canary-display' : deviceId;
  const dir = join(ROOT, 'firmware/configs', family);
  if (!existsSync(dir)) return [];
  return readdirSync(dir, { withFileTypes: true })
    .filter((e) => e.isDirectory() && existsSync(join(dir, e.name, 'config.h')))
    .map((e) => e.name)
    .sort();
}

const catalogByDevice = new Map();
for (const p of catalog.products) {
  for (const v of p.variants || []) {
    const key = v.device || '_universal';
    if (!catalogByDevice.has(key)) catalogByDevice.set(key, []);
    catalogByDevice.get(key).push({ product: p.id, variant: v.id, status: v.status });
  }
}

function confidenceFor(fig, evidence) {
  if (fig.role === 'board') {
    // A board is somebody else's product, so "committed STLs + our firmware +
    // our catalog" cannot apply. What CAN be checked on disk is whether we
    // have pinned the exact orderable part and committed its geometry:
    //   shipping  — vendor CAD committed AND a manufacturer part number
    //   confirmed — the design names it, but one of those two is missing
    // Forcing every board to the top rung, as the first cut of this did,
    // asserted an availability the ledger could not substantiate.
    const b = evidence.vendor_board;
    return b && b.mesh_committed && b.mpn ? 'shipping' : 'confirmed';
  }
  if (evidence.registry_kind === 'concept') return 'idea';
  const printable = evidence.committed_stls.length > 0;
  const released = evidence.catalog_variants.some((v) => v.status === 'released');
  const flashable = evidence.firmware_configs.length > 0;
  if (printable && released && flashable) return 'shipping';
  if (released) return 'confirmed';
  return 'prototype';
}

/* ───────────────────────────────────────────────────────────── taxonomy
 * Two facets over ONE id space, not two competing hierarchies — the answer
 * to "organize by supplier, or by product family?" is both, because they
 * answer different questions. Family answers "what is this for?"; supply
 * answers "what did we buy, and what happens when that vendor changes the
 * part?". A supplier facet is what makes "every Seeed-hosted thing" a query
 * instead of a grep. */

const SUPPLY = [
  [/xiao/i, { vendor: 'seeed', line: 'XIAO' }],
  [/grove vision/i, { vendor: 'seeed', line: 'Grove Vision AI' }],
  [/mr60|recamera|round display|wio-sx/i, { vendor: 'seeed', line: 'Seeed module' }],
  [/waveshare/i, { vendor: 'waveshare', line: 'ESP32 display board' }],
  [/hammond/i, { vendor: 'hammond', line: 'diecast/ABS box' }],
  [/raspberry pi|linux sbc/i, { vendor: 'raspberry-pi', line: 'SBC' }],
];

function supplyOf(deviceId) {
  const dev = registry.devices.find((d) => d.id === deviceId);
  const board = dev?.board || '';
  const hits = [];
  for (const [re, tag] of SUPPLY) if (re.test(board)) hits.push(tag);
  // Dedupe by vendor, keep first-listed line — deterministic.
  const seen = new Set();
  return hits.filter((h) => !seen.has(h.vendor) && seen.add(h.vendor));
}

/* ──────────────────────────────────────────────────────── the envelope */

// SCAD parts are authored face-up for printing (+Z out through the face,
// +Y up the wall); the figure frame is +X right, +Y front, +Z up.
function toFigureFrame(size, frame) {
  if (frame === 'scad-wall') return { w: size[0], d: size[2], h: size[1] };
  return { w: size[0], d: size[1], h: size[2] };
}

function envelopeFor(fig) {
  if (fig.board) {
    // The committed board mesh (boards.json), whose geometry facts are
    // recomputed from the mesh itself. Same contract as an STL: the
    // dimensions are read, never retyped, so a re-tessellation moves the
    // figure with it. dims_mm is already [w, d, h] in the figure frame.
    const b = boards.boards[fig.board];
    if (!b) throw new Error(`figures: ${fig.id} names board "${fig.board}", which is not in boards.json`);
    if (!Array.isArray(b.dims_mm) || b.dims_mm.length !== 3) {
      throw new Error(`figures: board "${fig.board}" has no dims_mm for ${fig.id} to be built from`);
    }
    const [w, d, h] = b.dims_mm;
    return { E: { w, d, h }, parts: {}, source: 'board-cad', stls: [] };
  }
  if (fig.sketch) {
    return { E: { ...fig.sketch }, parts: {}, source: 'sketch', stls: [] };
  }
  const files = fig.parts || [fig.stl];
  const parts = {};
  const stls = [];
  for (const file of files) {
    const path = join(ENCLOSURE, file);
    if (!existsSync(path)) {
      throw new Error(`figures: ${fig.id} references ${file}, which is not committed. `
        + 'Either commit the STL (render.sh) or give the figure a `sketch` envelope.');
    }
    const b = stlBounds(path);
    parts[file] = toFigureFrame(b.size, fig.frame);
    stls.push({ file, triangles: b.triangles, mm: b.size.map(r3) });
  }
  // A multi-part figure's envelope is the deepest stack it can make: parts
  // sit front-to-back, so depth adds while width/height take the largest.
  const vals = Object.values(parts);
  const E = {
    w: Math.max(...vals.map((p) => p.w)),
    d: vals.reduce((a, p) => a + p.d, 0),
    h: Math.max(...vals.map((p) => p.h)),
  };
  return { E, parts, source: 'stl', stls };
}

const r3 = (v) => Math.round(v * 1000) / 1000;
const r2 = (v) => Math.round(v * 100) / 100;

/* ─────────────────────────────────────────────────── the drift guard
 * A single-part figure IS its STL: its massing must fill the same box, or
 * the picture and the print have parted company. Solids may sit proud of
 * the face by the EPS the coplanar rule requires, plus whatever a feature
 * legitimately stands out by, so the guard is one-sided and generous in
 * depth but tight in the plan — where a wrong number would actually mislead. */
const PLAN_TOL = 0.75;   // mm, width & height
const DEPTH_TOL = 4.0;   // mm, allows proud lenses/buttons/windows

function guardDrift(fig, E, solids, source) {
  // Applies to any figure with ONE dimensional source of truth behind it: a
  // committed STL, or a committed board mesh. Exempting the board path let
  // `board.xiao` publish 22.64 x 3.66 x 19.38 while claiming to come from CAD
  // measuring 22.64 x 4.42 x 17.78 — a figure that both fell short of the part
  // and overflowed it, under a `dims_source` that said otherwise.
  if (fig.parts) return null;
  if (source !== 'stl' && source !== 'board-cad') return null;
  const env = envelopeOf(solids);
  const got = { w: env.size[0], d: env.size[1], h: env.size[2] };
  const bad = [];
  if (Math.abs(got.w - E.w) > PLAN_TOL) bad.push(`width ${r3(got.w)} vs STL ${r3(E.w)}`);
  if (Math.abs(got.h - E.h) > PLAN_TOL) bad.push(`height ${r3(got.h)} vs STL ${r3(E.h)}`);
  if (got.d - E.d > DEPTH_TOL || got.d < E.d - PLAN_TOL) bad.push(`depth ${r3(got.d)} vs STL ${r3(E.d)}`);
  if (bad.length) {
    throw new Error(`figures: ${fig.id} has drifted from ${fig.stl} — ${bad.join('; ')}. `
      + 'The CAD moved; update massing.mjs so the figure matches the part again.');
  }
  return { plan_tol_mm: PLAN_TOL, depth_tol_mm: DEPTH_TOL };
}

/* ─────────────────────────────────────────────── the coplanar guard
 * The .glb generators learned this the hard way (see the website's working
 * notes): two faces of different materials on the exact same plane z-fight
 * on a GPU. Here the failure is quieter but the same defect — a tie in the
 * paint order, so which material wins is an accident of sort stability. The
 * fix is identical: hold every stacked element a hair proud of the one under
 * it. This refuses to emit a figure that breaks the rule. */
function guardCoplanar(fig, solids) {
  // Only FRONT faces are painted, so only front faces can tie in the paint
  // order — which is the actual defect: which material wins becomes an
  // accident of sort stability. An earlier cut registered each solid's back
  // plane too, which made ordinary stacking (the back of one part sitting
  // flush on the front of the part behind it) trip the guard, and pushed the
  // specs into EPS gaps that were papering over a false positive.
  const planes = new Map(); // "<front y>" -> { m, box } of the first claimant
  for (const s of solids) {
    const pts = cornersOf(s);
    const span = (i) => [Math.min(...pts.map((p) => p[i])), Math.max(...pts.map((p) => p[i]))];
    const box = { x: span(0), z: span(2) };
    const key = span(1)[1].toFixed(4);
    const prev = planes.get(key);
    if (prev && prev.m !== s.m && overlaps(prev.box, box)) {
      throw new Error(`figures: ${fig.id} shows ${prev.m} and ${s.m} at the same depth `
        + `(y=${key}) with overlapping footprints, so which one paints last is an `
        + 'accident. Hold the nearer one proud by EPS (see massing.mjs).');
    }
    if (!prev) planes.set(key, { m: s.m, box });
  }
}

// Overlapping footprints in the plane we're guarding — two parts that share
// a y-plane but sit side by side are fine; two that share it AND overlap on
// screen are the defect.
function overlaps(a, b) {
  const hit = (p, q) => p[1] > q[0] + 1e-6 && q[1] > p[0] + 1e-6;
  return hit(a.x, b.x) && hit(a.z, b.z);
}

/* ──────────────────────────────────────────────────────────── the build */

const files = new Map(); // path -> contents (string), written or checked at the end

function emit(path, contents) {
  files.set(path, contents);
}

function buildOne(fig) {
  const { E, parts, source, stls } = envelopeFor(fig);
  const solids = fig.build(E, parts);
  guardCoplanar(fig, solids);

  const dev = registry.devices.find((d) => d.id === fig.of);
  const evidence = {
    registry_kind: dev?.kind ?? (fig.of === '_universal' ? 'universal' : null),
    committed_stls: stls.map((s) => s.file),
    catalog_variants: catalogByDevice.get(fig.of) || [],
    firmware_configs: fig.role === 'board' ? [] : firmwareConfigs(fig.of),
  };
  if (fig.board || fig.role === 'board') {
    const b = fig.board ? boards.boards[fig.board] : null;
    evidence.vendor_board = {
      key: fig.board || null,
      vendor: b?.vendor || null,
      mpn: b?.mpn || null,
      // The mesh is what the dimensions are read from; the vendor STEP is
      // where that mesh came from. Most boards have both, but at least one
      // committed mesh was built without a vendor STEP to tessellate — so
      // these are two facts, not one. Conflating them (an earlier cut
      // required both to call the geometry committed) would have thrown away
      // real measurements we do hold.
      mesh_committed: !!(b && b.glb),
      vendor_step: !!(b && b.source_step),
    };
  }
  const confidence = confidenceFor(fig, evidence);
  const ghost = confidence === 'idea';

  const guard = guardDrift(fig, E, solids, source);
  const env = envelopeOf(solids);

  const svg = renderFigure({ ...fig, solids }, { size: 256, ghost });
  const glyph = renderFigure({ ...fig, solids }, { size: 64, pad: 3, detail: 'glyph', ghost });

  emit(join(OUT_SVG, `${fig.id}.svg`), svg);
  emit(join(OUT_SVG, `${fig.id}.glyph.svg`), glyph);

  // rev = the content hash of everything that decides the picture. A surface
  // that cached a figure compares this and knows whether to redraw.
  const rev = createHash('sha256').update(JSON.stringify({
    v: PROJECTOR_REV, solids, ghost,
  })).digest('hex').slice(0, 8);

  return {
    id: fig.id,
    picker: renderFigureCompact({ ...fig, solids }, { size: 64, pad: 3, ghost }),
    title: fig.title,
    role: fig.role,
    device: fig.of,
    family: dev?.family ?? null,
    supply: fig.supplier ? [{ vendor: fig.supplier, line: fig.title }] : supplyOf(fig.of),
    confidence,
    evidence,
    dims_source: source,
    sketch_note: fig.sketchNote ?? null,
    envelope_mm: { w: r3(env.size[0]), d: r3(env.size[1]), h: r3(env.size[2]) },
    traced_to: stls,
    drift_guard: guard,
    solids: solids.length,
    rev,
    svg: `canary-local/figures/${fig.id}.svg`,
    glyph: `canary-local/figures/${fig.id}.glyph.svg`,
    plan: planFigure({ ...fig, solids }, { size: 256, ghost }),
  };
}

const built = [];
for (const fig of FIGURES) built.push(buildOne(fig));
for (const dev of registry.devices) {
  const fig = conceptFigure(dev.id, dev);
  if (fig) built.push(buildOne(fig));
}
built.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
const byIdBuilt = new Map(built.map((f) => [f.id, f]));

/* ── firmware: what a device says it is ─────────────────────────────── */

const deviceTypes = [];
for (const cfg of walkConfigs()) deviceTypes.push(cfg);

/* Every build env, and the physical board it compiles against. The pins
 * include is the primary signal; `board =` is the coarse fallback for envs
 * that carry no pins header of their own. Both are inherited through
 * `extends`, so a feature env that only adds a flag still resolves. */
function walkEnvs() {
  const out = [];
  const files = [
    ...globIni(join(ROOT, 'firmware/envs/platformio')),
    ...globIni(join(ROOT, 'firmware/projects'), true),
  ];
  const raw = new Map();
  for (const file of files) {
    const txt = readFileSync(file, 'utf8');
    for (const m of txt.matchAll(/^\[env:([^\]]+)\]\n([\s\S]*?)(?=^\[|$(?![\s\S]))/gm)) {
      const [, name, body] = m;
      raw.set(name, {
        name,
        pins: (body.match(/boards\/([a-z0-9._-]+)\/pins/) || [])[1] || null,
        config: (body.match(/configs\/([a-z0-9-]+)\/([a-z0-9_]+)/) || []).slice(1, 3).join('/') || null,
        board: (body.match(/^board\s*=\s*(\S+)/m) || [])[1] || null,
        extends: (body.match(/^extends\s*=\s*env:(\S+)/m) || [])[1] || null,
        file: file.replace(`${ROOT}/`, ''),
      });
    }
  }
  const resolve = (name, key, seen = new Set()) => {
    if (seen.has(name) || !raw.has(name)) return null;
    const e = raw.get(name);
    return e[key] || (e.extends ? resolve(e.extends, key, new Set([...seen, name])) : null);
  };
  for (const name of [...raw.keys()].sort()) {
    const pins = resolve(name, 'pins');
    const board = resolve(name, 'board');
    const config = resolve(name, 'config');
    const hardware = pins || (board ? `board:${board}` : null);
    out.push({ env: name, pins, board, config, hardware, file: raw.get(name).file });
  }
  return out;
}

function globIni(dir, nested = false) {
  if (!existsSync(dir)) return [];
  const out = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory() && nested) out.push(...globIni(p));
    else if (e.isFile() && e.name.endsWith('.ini')) out.push(p);
  }
  return out.sort();
}

function walkConfigs() {
  const out = [];
  const base = join(ROOT, 'firmware/configs');
  if (!existsSync(base)) return out;
  for (const fam of readdirSync(base, { withFileTypes: true }).filter((e) => e.isDirectory())) {
    for (const flav of readdirSync(join(base, fam.name), { withFileTypes: true }).filter((e) => e.isDirectory())) {
      const h = join(base, fam.name, flav.name, 'config.h');
      if (!existsSync(h)) continue;
      const m = readFileSync(h, 'utf8').match(/#define\s+\w*DEVICE_TYPE\s+"([^"]+)"/);
      if (m) out.push({ family: fam.name, flavor: flav.name, device_type: m[1] });
    }
  }
  return out.sort((a, b) => (a.device_type + a.flavor < b.device_type + b.flavor ? -1 : 1));
}

/* Which figure does a running device get?
 *
 * An early cut guessed with a regex on the published DEVICE_TYPE, and matched
 * both rectangular nightstand boards onto the round Watch Station drum. There
 * is no guessing here any more — two lookups, at two honest precisions.
 *
 * ── HARDWARE (exact) ────────────────────────────────────────────────────
 * Every build env compiles against exactly one `boards/<id>/pins` header, and
 * that include is LOAD-BEARING: get it wrong and the device does not work. So
 * it is already a true, machine-readable statement of which physical board a
 * build is for — the declaration an earlier pass concluded was missing. It is
 * not: it was sitting in the build flags all along, one level below where we
 * were looking.
 *
 * This is why the config directory was the wrong key. `canary-display-dash`
 * compiles `waveshare-esp32s3-lcd43`, `canary-display-dash-b` compiles
 * `-lcd43b` and `canary-display-dash-mic` compiles `-lcd43c` — three different
 * panels, one `configs/canary-display/dash` directory between them.
 *
 * Reading a declaration the build already depends on beats adding a parallel
 * one that can drift. (SECURACV_OTA_PRODUCT looked like a candidate and is
 * not: it is an UPDATE CHANNEL, and it deliberately groups dash-b with dash.)
 *
 * ── DEVICE TYPE (coarse) ────────────────────────────────────────────────
 * What a peer witness publishes on the wire. It resolves only when every
 * config publishing it agrees on one figure; `canary-dash` and
 * `canary-nightstand` are each published by two different panels, so they are
 * absent and figure_for() returns nullptr. The caller draws its generic
 * marker. A wrong picture is worse than no picture.
 */

// boards/<id>/pins  ->  the figure of the thing that board is. Envs with no
// pins header of their own fall back to `board:<platformio id>`.
const HARDWARE_FIGURE = {
  'xiao-esp32s3-round': 'device.canary-display-watch',
  'waveshare-esp32s3-lcd43': 'device.canary-display-dash',
  'waveshare-esp32s3-lcd7': 'device.canary-display-dash7',
  'waveshare-esp32s3-touch-lcd169': 'device.canary-display-touch169',
  'xiao-esp32c6-mr60': 'device.canary-sense',
  'board:seeed_xiao_esp32s3': 'device.canary-wap',
  // The two XIAO hosts share the stacked-XIAO enclosure the Vision figure
  // traces; the DevKitM is a wider, Grove-cabled housing with its own STLs,
  // so it gets its own figure rather than borrowing one that fits neither.
  'xiao-esp32c3': 'device.canary-vision',
  'xiao-esp32s3': 'device.canary-vision',
  'esp32-c3': 'device.canary-vision-devkit',
};

/* Hardware we can name but cannot yet draw. Listed so the gap is data, with
 * the reason beside it, rather than a silent nullptr:
 *   waveshare-esp32s3-lcd43b / -lcd43c   the 4.3B and 4.3C panels; the Dash
 *       figure traces the plain 4.3, and these are different housings
 *   waveshare-esp32s3-lcd147             the S3 1.47" USB-A stick — no panel
 *       record on disk to size it from, and inventing one is the one place a
 *       sketch would mislead
 *   waveshare-esp32c6-lcd147             the C6 1.47" board is not a device in
 *       registry.json, and the registry is the one id space (CATALOG §4)
 *   xiao-esp32c3-sentinel-lite / board:seeed_xiao_esp32c6   the Sentinel line
 *       has no enclosure CAD at all yet
 */

// firmware/configs/<family>/<flavor> -> figure. Used ONLY to work out which
// published device types are unambiguous; never as an identity itself, since
// one config directory serves several panels.
const CONFIG_FIGURE = {
  'canary-vision/default': 'device.canary-vision',
  'canary-sense/default': 'device.canary-sense',
  'canary-sense/wellbeing': 'device.canary-sense',
  'canary-wap/default': 'device.canary-wap',
  'canary-wap/mobile': 'device.canary-wap',
  'canary-display/watch': 'device.canary-display-watch',
  'canary-display/dash': 'device.canary-display-dash',
  'canary-display/dash7': 'device.canary-display-dash7',
  'canary-display/touch169': 'device.canary-display-touch169',
};

const configRows = deviceTypes
  .map((c) => ({ ...c, figureId: CONFIG_FIGURE[`${c.family}/${c.flavor}`] || null }))
  .map((c) => ({ ...c, fig: c.figureId ? byIdBuilt.get(c.figureId) : null }))
  .sort((a, b) => (`${a.family}/${a.flavor}` < `${b.family}/${b.flavor}` ? -1 : 1));

for (const r of configRows) {
  if (r.figureId && !r.fig) {
    throw new Error(`figures: CONFIG_FIGURE maps ${r.family}/${r.flavor} to "${r.figureId}", `
      + 'which is not a figure. Fix the map or add the figure.');
  }
}

// A device type resolves ONLY when every flavor that publishes it agrees on
// one figure. Anything else is an ambiguity in the firmware's own vocabulary,
// not something this generator gets to invent an answer for.
const byType = new Map();
for (const r of configRows) {
  if (!byType.has(r.device_type)) byType.set(r.device_type, new Set());
  byType.get(r.device_type).add(r.figureId);
}
const uniqueRows = [];
const unmapped = [];
for (const [device_type, ids] of [...byType.entries()].sort()) {
  const known = [...ids].filter(Boolean);
  if (known.length === 1 && ids.size === 1) {
    uniqueRows.push({ device_type, fig: byIdBuilt.get(known[0]) });
  } else {
    unmapped.push({
      device_type,
      why: known.length === 0
        ? 'no figure for this hardware yet'
        : ids.has(null)
          ? `published by more than one board, and at least one has no figure yet `
            + `(the figured one is ${known.sort().join(', ')})`
          : `published by boards with different figures (${known.sort().join(', ')})`,
    });
  }
}
const mappedConfigRows = configRows.filter((r) => r.fig);

/* ── hardware: the exact lookup, from the pins each build compiles ────── */

const envs = walkEnvs();
for (const id of Object.keys(HARDWARE_FIGURE)) {
  if (!byIdBuilt.get(HARDWARE_FIGURE[id])) {
    throw new Error(`figures: HARDWARE_FIGURE maps "${id}" to "${HARDWARE_FIGURE[id]}", `
      + 'which is not a figure. Fix the map or add the figure.');
  }
}

// Group the envs by the board they compile against, so the table is one row
// per piece of hardware rather than one per build.
const byHardware = new Map();
for (const e of envs) {
  if (!e.hardware) continue;            // the OTA tool builds; not a product
  if (!byHardware.has(e.hardware)) byHardware.set(e.hardware, []);
  byHardware.get(e.hardware).push(e.env);
}
// What DEVICE TYPE each build publishes, so a board can say how many personas
// it carries. The 7" glass is the live example: canary-display-dash7 and
// canary-display-nightstand7 are one board and two products.
const typeOfConfig = new Map(deviceTypes.map((c) => [`${c.family}/${c.flavor}`, c.device_type]));
const servesOf = (builds) => [...new Set(builds
  .map((b) => typeOfConfig.get(envs.find((e) => e.env === b)?.config))
  .filter(Boolean))].sort();

const hardwareRows = [];
const hardwareGaps = [];
for (const [hardware, builtBy] of [...byHardware.entries()].sort()) {
  const builds = builtBy.sort();
  const serves = servesOf(builds);
  const figureId = HARDWARE_FIGURE[hardware];
  if (figureId) {
    hardwareRows.push({
      hardware, fig: byIdBuilt.get(figureId), builds, serves,
      // One board, several products. The SHAPE is still right — that is what a
      // figure is — but the figure's TITLE names only one of them, so a caller
      // that wants to name the product must ask the device type, not this.
      shared: serves.length > 1,
    });
  } else {
    hardwareGaps.push({ hardware, builds, serves });
  }
}

// Each mapped board must NAME itself in the very header the build compiles
// against, or my_figure() silently returns nullptr on a device that does have
// a figure. Checking it here keeps the id and the pins it travels with from
// ever disagreeing.
for (const r of hardwareRows) {
  if (r.hardware.startsWith('board:')) continue;  // no pins header to carry it
  const pins = join(ROOT, 'firmware/boards', r.hardware, 'pins/pins.h');
  if (!existsSync(pins)) {
    throw new Error(`figures: hardware "${r.hardware}" has a figure but no `
      + `firmware/boards/${r.hardware}/pins/pins.h to declare it in.`);
  }
  const want = `#define CANARY_FIGURE_HARDWARE "${r.hardware}"`;
  if (!readFileSync(pins, 'utf8').includes(want)) {
    throw new Error(`figures: firmware/boards/${r.hardware}/pins/pins.h must carry\n`
      + `  ${want}\n`
      + 'so a build can name itself. Without it my_figure() returns nullptr on '
      + 'a device that has a perfectly good figure.');
  }
}

// Envs with no hardware signal at all would silently vanish from the table,
// so name them. The canary-ota project is the OTA tool, not a product.
const OTA_TOOL_ENVS = new Set(['dev', 'production', 'test']);
const envsWithoutHardware = envs
  .filter((e) => !e.hardware && !OTA_TOOL_ENVS.has(e.env))
  .map((e) => e.env);
if (envsWithoutHardware.length) {
  throw new Error(`figures: these build envs declare neither a pins header nor a board, `
    + `so nothing can say what they run on: ${envsWithoutHardware.join(', ')}`);
}

/* ── ledger ─────────────────────────────────────────────────────────── */

const counts = Object.fromEntries(LADDER.map((c) => [c, built.filter((f) => f.confidence === c).length]));
const ledger = {
  generated_by: 'canary-local/tools/figures/gen_figures.mjs',
  spec: 'docs/design/FLEET_FIGURES.md',
  note: 'One isometric figure per physical thing in the fleet, generated from the '
    + 'committed CAD. Every surface — glass, wrist, phone, web, emulator — draws '
    + 'from this ledger, so they cannot show a user two different pictures of one '
    + 'object. Hand edits are overwritten; run the generator.',
  projector: {
    rev: PROJECTOR_REV,
    kind: 'true isometric, 30°',
    frame: '+X right, +Y front (toward the viewer), +Z up; millimeters',
    camera: 'fixed at (+X,+Y,+Z) for every figure in the fleet',
  },
  ladder: {
    order: LADDER,
    shipping: 'committed STLs + a firmware config + a released catalog variant',
    confirmed: 'released in the catalog; one of the three proofs still missing',
    prototype: 'in development — not committed to print',
    idea: 'a research note and a sketch; renders as a ghost, never as a product',
    derived: 'from evidence on disk, never hand-typed — see each figure\'s `evidence`',
  },
  counts,
  // What a running device can be drawn as. `flavors` is exact (a firmware
  // config directory is one piece of hardware); `unmapped_device_types` is
  // the honest gap — types the firmware's own vocabulary cannot pin to one
  // board, plus hardware we have no figure for yet. Those draw the generic
  // marker rather than a guess.
  device_types: {
    mapped: uniqueRows.map((r) => ({ device_type: r.device_type, figure: r.fig.id })),
    unmapped,
    // No exact per-build lookup: see the note above CONFIG_FIGURE. What a
    // config directory maps to is recorded for auditing, NOT published to the
    // firmware, because a config directory is not one piece of hardware.
    configs_audit: mappedConfigRows.map((r) => ({
      family: r.family, flavor: r.flavor, device_type: r.device_type, figure: r.fig.id,
    })),
  },
  // The EXACT lookup. Every build env compiles against exactly one
  // boards/<id>/pins header, and that include is load-bearing — get it wrong
  // and the device does not work — so it is a true statement of which
  // physical board a build is for. Reading it beats adding a parallel
  // declaration that could drift.
  hardware: {
    key: 'the boards/<id>/pins header the build compiles against, or '
      + 'board:<platformio id> for envs that carry no pins header of their own',
    mapped: hardwareRows.map((r) => ({
      hardware: r.hardware,
      figure: r.fig.id,
      builds: r.builds,
      serves: r.serves,
      // true when this board carries more than one product persona: the
      // drawing is right for all of them, the figure's title is right for
      // one. Ask the device type to name the product.
      shared: r.shared,
    })),
    unmapped: hardwareGaps,
  },
  figures: built.map(({ plan, picker, ...rest }) => rest),
};
emit(OUT_JSON, `${JSON.stringify(ledger, null, 1)}\n`);

// One compact SVG per figure, keyed by id — the only tier small enough to
// travel inside another catalog.
emit(OUT_PICKER, `${JSON.stringify({
  $generated_by: 'canary-local/tools/figures/gen_figures.mjs',
  $doc: 'Compact per-figure SVG for list-row rendering (46 px). Faces sharing a '
    + 'fill are merged into one path and the hairline stroke is dropped, which is '
    + 'where the bytes are. Embedded into flash.json by gen_flash.py.',
  projector_rev: PROJECTOR_REV,
  figures: Object.fromEntries(built.map((f) => [f.id, f.picker])),
}, null, 1)}\n`);


emit(OUT_H, `#pragma once
// GENERATED by canary-local/tools/figures/gen_figures.mjs — do not edit.
// Spec: docs/design/FLEET_FIGURES.md
//
// A device's own answer to "what am I?", in the fleet's shared vocabulary, so
// a display, a phone or a watch can draw the correct picture of a witness and
// can tell when the copy it cached was drawn from older CAD.
//
// Two lookups, at two HONEST precisions — the distinction matters, because
// getting it wrong once already showed a user the wrong product:
//
//   figure_for_hardware(id)     Exact about the BOARD, and therefore about
//     the SHAPE. Keyed on the boards/<id>/pins header the build compiles
//     against, which is load-bearing (wrong pins, dead device) and therefore a
//     true statement of which physical board this is. A device asking what it
//     LOOKS LIKE should use this; pass CANARY_FIGURE_HARDWARE, which each
//     board's pins header defines.
//
//     It is NOT a product name. One board can carry several products — the 7"
//     glass is both the Dash 7 and the Nightstand 7 — and a figure's title
//     names only one of them. Rows where that happens set shared_across_products;
//     to NAME the product, ask the device type, not the board.
//
//   figure_for(device_type)     COARSE, and deliberately INCOMPLETE. This is
//     what a PEER publishes on the wire, and several types are shared by more
//     than one board — canary-dash by the 4.3" and the 7" panel, canary-
//     nightstand by the 1.47" stick and the 1.69" touch. Those are ABSENT and
//     the lookup returns nullptr, so the caller draws its generic marker.
//     A wrong picture is worse than no picture.
//
// Unresolved entries, with reasons, are in canary-local/devices/figures.json
// under device_types.unmapped and hardware.unmapped.
//
// Pure C++, no Arduino/JSON dependencies, no allocation: the same rules
// fleet_model.h follows, so this is host-testable and safe in a hot path.

#include <stddef.h>

namespace canary::figures {

struct FigureRef {
  const char* device_type;  // what the firmware publishes
  const char* figure_id;    // the ledger id, e.g. "device.canary-sense"
  const char* rev;          // 8 hex chars; changes when the drawing changes
  const char* confidence;   // shipping | confirmed | prototype | idea
};

struct HardwareRef {
  const char* hardware;     // the boards/<id>/pins header this build compiles
  const char* figure_id;
  const char* rev;
  const char* confidence;
  // true when this board carries more than one product. The drawing is right
  // for all of them; the figure's TITLE is right for one. Do not print the
  // title as this device's product name when this is set.
  bool shared_across_products;
};

// Device types that resolve to exactly one figure. Types published by more
// than one board are absent ON PURPOSE — see the note above.
inline constexpr FigureRef kFigures[] = {
${uniqueRows.map((r) => `  { "${r.device_type}", "${r.fig.id}", "${r.fig.rev}", "${r.fig.confidence}" },`).join('\n')}
};
inline constexpr size_t kFigureCount = sizeof(kFigures) / sizeof(kFigures[0]);

// One row per piece of hardware we can draw. The board is exact; see
// shared_across_products for when the product name is not.
inline constexpr HardwareRef kHardware[] = {
${hardwareRows.map((r) => `  { "${r.hardware}", "${r.fig.id}", "${r.fig.rev}", "${r.fig.confidence}", ${r.shared} },`
  + `  // ${r.builds.length} build${r.builds.length === 1 ? '' : 's'}`
  + (r.shared ? ` — shared by ${r.serves.join(' + ')}` : '')).join('\n')}
};
inline constexpr size_t kHardwareCount = sizeof(kHardware) / sizeof(kHardware[0]);

inline bool figure_streq(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

// What a PEER is, from the device type it published. nullptr when that type
// cannot be pinned to one board, or we have no figure for it.
inline const FigureRef* figure_for(const char* device_type) {
  if (!device_type || !device_type[0]) return nullptr;
  for (size_t i = 0; i < kFigureCount; i++) {
    if (figure_streq(kFigures[i].device_type, device_type)) return &kFigures[i];
  }
  return nullptr;
}

// What THIS build LOOKS LIKE. The pins header is compile-time truth, so the
// board — and the shape — are exact. Check shared_across_products before
// using the figure's title as this device's product name.
inline const HardwareRef* figure_for_hardware(const char* hardware) {
  if (!hardware || !hardware[0]) return nullptr;
  for (size_t i = 0; i < kHardwareCount; i++) {
    if (figure_streq(kHardware[i].hardware, hardware)) return &kHardware[i];
  }
  return nullptr;
}

// Sugar for the common case: the figure of the board this firmware is being
// compiled for. Boards whose pins header does not define
// CANARY_FIGURE_HARDWARE get nullptr, the same honest fallback as everywhere
// else in this header.
inline const HardwareRef* my_figure() {
#ifdef CANARY_FIGURE_HARDWARE
  return figure_for_hardware(CANARY_FIGURE_HARDWARE);
#else
  return nullptr;
#endif
}

}  // namespace canary::figures
`);

/* ── Swift: what the phone and the wrist draw ───────────────────────── */

const swiftFigures = built.map((f) => {
  const ops = f.plan.ops.map((op) => {
    const pts = op.pts.map(([x, y]) => `${r2(x)},${r2(y)}`).join(' ');
    const fill = op.kind === 'face' ? op.fill : (op.kind === 'shadow' ? '#00000024' : '');
    return `    FleetFigure.Face(kind: .${op.kind}, hex: "${fill}", pts: "${pts}")`;
  });
  return `  "${f.id}": FleetFigure(
    id: "${f.id}", title: ${JSON.stringify(f.title)}, rev: "${f.rev}",
    confidence: .${f.confidence}, size: ${f.plan.size}, faces: [
${ops.join(',\n')}
  ]),`;
});

emit(OUT_SWIFT, `// GENERATED by canary-local/tools/figures/gen_figures.mjs — do not edit.
// Spec: docs/design/FLEET_FIGURES.md
//
// The same figures the web and the emulator draw, as flat polygon data the
// phone and the wrist can paint with Canvas at any size. Not an asset
// catalog and not an SVG parser: the polygons come out of the one projector
// in iso.mjs, so a Canary drawn on the watch and the same Canary drawn on
// the showroom page are the same picture, from the same camera, to the pixel.
//
// Vector, so a 20 pt list row and a 200 pt hero cost the same bytes; and
// shared by SecuraCV, SecuraCVWatch and both widget bundles, so none of them
// can drift.

import SwiftUI

public struct FleetFigure: Sendable {
  public enum Confidence: String, Sendable, CaseIterable {
    case shipping, confirmed, prototype, idea

    /// An idea is drawn as a dashed ghost and labeled — it must never read
    /// as something you can buy or print. Enforced by the generator, which
    /// only emits ghost faces for this case.
    public var isBuilt: Bool { self != .idea }
  }

  public struct Face: Sendable {
    public enum Kind: String, Sendable { case shadow, face, ghost }
    public let kind: Kind
    public let hex: String
    public let pts: String

    public var points: [CGPoint] {
      pts.split(separator: " ").compactMap { pair in
        let xy = pair.split(separator: ",")
        guard xy.count == 2, let x = Double(xy[0]), let y = Double(xy[1]) else { return nil }
        return CGPoint(x: x, y: y)
      }
    }

    public var color: Color {
      var v: UInt64 = 0
      Scanner(string: String(hex.dropFirst())).scanHexInt64(&v)
      let hasAlpha = hex.count == 9
      let a = hasAlpha ? Double(v & 0xFF) / 255 : 1
      let rgb = hasAlpha ? v >> 8 : v
      return Color(
        .sRGB,
        red: Double((rgb >> 16) & 0xFF) / 255,
        green: Double((rgb >> 8) & 0xFF) / 255,
        blue: Double(rgb & 0xFF) / 255,
        opacity: a)
    }
  }

  public let id: String
  public let title: String
  public let rev: String
  public let confidence: Confidence
  public let size: Double
  public let faces: [Face]

  /// The figure for a witness's published device type, or nil when we have
  /// none — draw the generic marker then, never a guessed product.
  public static func forDeviceType(_ deviceType: String) -> FleetFigure? {
    guard let id = deviceTypeToFigure[deviceType] else { return nil }
    return all[id]
  }

  public static let all: [String: FleetFigure] = [
${swiftFigures.join('\n')}
  ]

  public static let deviceTypeToFigure: [String: String] = [
${uniqueRows.map((r) => `    "${r.device_type}": "${r.fig.id}",`).join('\n')}
  ]
}

/// Draws a figure at whatever size it is given. Scales the stored 256-unit
/// plan to the view, so one description serves a complication and a hero.
public struct FleetFigureView: View {
  public let figure: FleetFigure

  public init(_ figure: FleetFigure) { self.figure = figure }

  public var body: some View {
    Canvas { ctx, size in
      let s = min(size.width, size.height) / figure.size
      for face in figure.faces {
        let pts = face.points
        guard pts.count > 2 else { continue }
        var path = Path()
        path.move(to: CGPoint(x: pts[0].x * s, y: pts[0].y * s))
        for p in pts.dropFirst() { path.addLine(to: CGPoint(x: p.x * s, y: p.y * s)) }
        path.closeSubpath()
        switch face.kind {
        case .ghost:
          ctx.stroke(path, with: .color(.secondary),
                     style: StrokeStyle(lineWidth: 1.25 * s, dash: [3 * s, 2.5 * s]))
        case .shadow, .face:
          ctx.fill(path, with: .color(face.color))
        }
      }
    }
    .accessibilityLabel(figure.title)
  }
}
`);

/* ── write or check ─────────────────────────────────────────────────── */

if (CHECK) {
  const drift = [];
  for (const [path, contents] of files) {
    const have = existsSync(path) ? readFileSync(path, 'utf8') : null;
    if (have !== contents) drift.push(path.replace(`${ROOT}/`, ''));
  }
  // A figure deleted from massing.mjs must take its .svg with it.
  for (const f of existsSync(OUT_SVG) ? readdirSync(OUT_SVG) : []) {
    if (!files.has(join(OUT_SVG, f))) drift.push(`canary-local/figures/${f} (orphan)`);
  }
  if (drift.length) {
    console.error('figures: out of date —\n  ' + drift.join('\n  '));
    console.error('\nRun: node canary-local/tools/figures/gen_figures.mjs');
    process.exit(1);
  }
  console.log(`figures: up to date (${built.length} figures, ${files.size} files)`);
} else {
  if (existsSync(OUT_SVG)) {
    for (const f of readdirSync(OUT_SVG)) {
      if (!files.has(join(OUT_SVG, f))) rmSync(join(OUT_SVG, f));
    }
  }
  for (const [path, contents] of files) {
    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(path, contents);
  }
  const byConf = LADDER.map((c) => `${counts[c]} ${c}`).join(' · ');
  console.log(`figures: ${built.length} figures (${byConf})`);
  console.log(`  ledger  ${OUT_JSON.replace(`${ROOT}/`, '')}`);
  console.log(`  svg     ${built.length * 2} files in canary-local/figures/`);
  console.log(`  firmware ${OUT_H.replace(`${ROOT}/`, '')} (${uniqueRows.length} device types)`);
  console.log(`  swift   ${OUT_SWIFT.replace(`${ROOT}/`, '')}`);
}
