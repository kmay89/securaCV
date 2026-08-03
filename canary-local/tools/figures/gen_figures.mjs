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

import { renderFigure, planFigure, envelopeOf, cornersOf } from './iso.mjs';
import { stlBounds } from './stlbox.mjs';
import { FIGURES, conceptFigure } from './massing.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '../../..');
const ENCLOSURE = join(ROOT, 'docs/hardware/enclosure');
const OUT_JSON = join(ROOT, 'canary-local/devices/figures.json');
const OUT_SVG = join(ROOT, 'canary-local/figures');
const OUT_H = join(ROOT, 'firmware/common/core/fleet_figures.h');
const OUT_SWIFT = join(ROOT, 'ios/Shared/FleetFigures.swift');

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
 * is no guessing here any more.
 *
 * A device type resolves ONLY when every firmware config that publishes it
 * agrees on one figure. Types published by more than one board — `canary-dash`
 * by both the 4.3" and the 7" panel, `canary-nightstand` by both the 1.47"
 * stick and the 1.69" touch — are OMITTED rather than resolved by a coin flip,
 * and listed in the ledger's `unmapped_device_types` so the gap is visible
 * instead of silently wrong. figure_for() returns nullptr for them and the
 * caller draws its generic marker. A wrong picture is worse than no picture.
 *
 * ── The open gap, stated plainly ────────────────────────────────────────
 * There is deliberately NO exact per-build lookup, because the firmware does
 * not currently record which physical product a build is for, and nothing on
 * disk can be read to recover it:
 *
 *   · a config directory is not one board — 12 build envs resolve to
 *     `canary-display/dash`, and `canary-display-dash-b` among them is a
 *     different panel; `canary-vision/default` is shared by 4 envs spanning
 *     the DevKitM, the XIAO C3 and the XIAO S3
 *   · a build env is not a clean signal either — `canary-display-watch`,
 *     `-watch-debug` and `-watch-modes` are one board with feature flags,
 *     while `-dash` and `-dash-b` are two boards; telling those apart means
 *     reading the suffix and guessing, which is the bug above wearing a hat
 *   · `board =` cannot separate them: dash-b `extends` dash and inherits it
 *
 * The fix is for a build to DECLARE its product (a `CANARY_PRODUCT_ID` per
 * env), not for this generator to infer it. Until that exists, the coarse
 * table is the honest ceiling. See docs/design/FLEET_FIGURES.md §7.
 */
const CONFIG_FIGURE = {
  'canary-vision/default': 'device.canary-vision',
  'canary-sense/default': 'device.canary-sense',
  'canary-sense/wellbeing': 'device.canary-sense',
  'canary-wap/default': 'device.canary-wap',
  'canary-wap/mobile': 'device.canary-wap',
  'canary-display/watch': 'device.canary-display-watch',
  'canary-display/dash': 'device.canary-display-dash',
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
    exact_lookup: {
      available: false,
      why: 'the firmware does not record which physical product a build is for; '
        + 'a config directory and a build env are both shared across boards',
      fix: 'declare a product id per build env, then key the exact table on it',
    },
  },
  figures: built.map(({ plan, ...rest }) => rest),
};
emit(OUT_JSON, `${JSON.stringify(ledger, null, 1)}\n`);


emit(OUT_H, `#pragma once
// GENERATED by canary-local/tools/figures/gen_figures.mjs — do not edit.
// Spec: docs/design/FLEET_FIGURES.md
//
// A device's own answer to "what am I?", in the fleet's shared vocabulary, so
// a display, a phone or a watch can draw the correct picture of a witness and
// can tell when the copy it cached was drawn from older CAD.
//
// This table is COARSE and deliberately INCOMPLETE. A device type appears
// here only when every firmware config that publishes it agrees on one
// figure; types shared by more than one board are ABSENT and figure_for()
// returns nullptr, so the caller draws its generic marker. An earlier cut
// guessed with a string match and handed the rectangular nightstand boards
// the round Watch Station drum — a wrong picture is worse than no picture.
// The unresolved types, with reasons, are in
// canary-local/devices/figures.json under device_types.unmapped.
//
// There is no exact per-build lookup, on purpose: the firmware does not
// record which physical product a build is for, and a config directory and a
// build env are each shared across different boards, so nothing on disk can
// recover it. Adding one means a build DECLARING its product id. See
// docs/design/FLEET_FIGURES.md §7.
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

inline constexpr FigureRef kFigures[] = {
${uniqueRows.map((r) => `  { "${r.device_type}", "${r.fig.id}", "${r.fig.rev}", "${r.fig.confidence}" },`).join('\n')}
};
inline constexpr size_t kFigureCount = sizeof(kFigures) / sizeof(kFigures[0]);

// Exact match on the published device type; nullptr when the type cannot be
// pinned to one board, or we have no figure for it. Draw the generic marker
// then — never a guessed picture of the wrong product.
inline const FigureRef* figure_for(const char* device_type) {
  if (!device_type || !device_type[0]) return nullptr;
  for (size_t i = 0; i < kFigureCount; i++) {
    const char* a = kFigures[i].device_type;
    const char* b = device_type;
    while (*a && *a == *b) { a++; b++; }
    if (*a == *b) return &kFigures[i];
  }
  return nullptr;
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
