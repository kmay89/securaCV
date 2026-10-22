// canary-local/assets/revisions.js — the part-revision ledger, read.
//
// devices/revisions.json (docs/hardware/enclosure/gen_revisions.py) carries
// one entry per printable part: its rev ("r3"), when it was recorded, the
// digest of the sources that draw it, the mesh's fingerprint, and a history
// whose newest entry says WHAT moved — knobs, volume and envelope deltas,
// the commits, and `regions`: boxes in the STL's own coordinates where the
// new surface is not where the old one was. These helpers are pure (no DOM,
// no fetch) so the enclosure tab, the Assemble tab and the tests share one
// reading of the file: which entry a part file maps to, the one-line
// summary a legend chip shows, the lines a "what changed" list shows, and
// the split of a mesh into the triangles inside those boxes (tinted) and
// the rest (the part's own color).

// A tint that is no spool in finishes.js and no PART_COLORS entry: the
// changed surface has to read as a mark on the part, not as a second part.
export const CHANGED_COLOR = [1.0, 0.45, 0.1];

// The ledger keys a part by its render output: a committed catalog STL by
// name, a Lab preview by "preview/<name>". enclosures.json spells a preview
// part the same way; assembly.json carries `base: "preview"` beside a bare
// name. Both spellings resolve here.
export function partKey(file, base) {
  if (!file) return null;
  const name = file.replace(/^preview\//, "");
  return (base === "preview" || file.startsWith("preview/")) ? `preview/${name}` : name;
}

export function partRevision(ledger, file, base) {
  const key = partKey(file, base);
  return (key && ledger?.parts?.[key]) || null;
}

// The newest history entry's changes, or null for a part at r1 (nothing to
// compare against yet) or a ledger without history.
export function latestChanges(entry) {
  const h = entry?.history;
  if (!Array.isArray(h) || !h.length) return null;
  return h[h.length - 1].changes || null;
}

export function changedRegions(entry) {
  const ch = latestChanges(entry);
  return Array.isArray(ch?.regions) ? ch.regions : [];
}

const fmt = (v, unit) => `${v > 0 ? "+" : ""}${Math.round(v * 10) / 10} ${unit}`;

// "r3 · 2026-10-07" plus, when there is a previous rev, the headline deltas.
export function revisionSummary(entry) {
  if (!entry) return "";
  let s = `${entry.rev} · ${entry.recorded}`;
  const ch = latestChanges(entry);
  if (ch) {
    const bits = [];
    if (ch.volume_mm3) bits.push(fmt(ch.volume_mm3, "mm³"));
    if (ch.knobs?.length) bits.push(`${ch.knobs.length} knob${ch.knobs.length === 1 ? "" : "s"}`);
    if (Array.isArray(ch.regions)) bits.push(`${ch.regions.length} region${ch.regions.length === 1 ? "" : "s"}`);
    if (bits.length) s += ` · ${bits.join(" · ")}`;
  }
  return s;
}

// The lines a "what changed since the previous rev" list shows, in the
// order a reader wants them: the human note, the commits, the knobs, then
// the numbers. Empty for a part at r1.
export function changeLines(entry) {
  return changeLinesFor(latestChanges(entry));
}

// The same lines for one history entry's `changes` object (null at r1).
export function changeLinesFor(ch) {
  if (!ch) return [];
  const out = [];
  if (ch.note) out.push(ch.note);
  for (const c of ch.commits || []) out.push(`commit ${c}`);
  for (const k of ch.knobs || []) {
    if ("from" in k && "to" in k) out.push(`${k.knob}: ${k.from} → ${k.to}`);
    else if ("to" in k) out.push(`${k.knob}: new, ${k.to}`);
    else out.push(`${k.knob}: removed (was ${k.from})`);
  }
  const d = [];
  if (ch.volume_mm3) d.push(`volume ${fmt(ch.volume_mm3, "mm³")}`);
  if (ch.area_mm2) d.push(`surface ${fmt(ch.area_mm2, "mm²")}`);
  if (Array.isArray(ch.bbox_mm) && ch.bbox_mm.some((v) => v)) {
    d.push(`envelope ${ch.bbox_mm.map((v) => fmt(v, "")).join(" × ").replace(/\s+×/g, " ×")} mm`);
  }
  if (d.length) out.push(d.join(", "));
  if (Array.isArray(ch.regions)) {
    out.push(ch.regions.length
      ? `${ch.regions.length} changed region${ch.regions.length === 1 ? "" : "s"} on the part (tinted)`
      : (ch.regions_note || "no region above the voxel size"));
  } else if (ch.previous) {
    out.push(`regions: ${ch.previous}`);
  }
  return out;
}

function inBox(x, y, z, b) {
  return x >= b[0] && x <= b[3] && y >= b[1] && y <= b[4] && z >= b[2] && z <= b[5];
}

// Split a scene3d mesh ({pos, nrm, uv, idx}) into the triangles whose
// centroid lies in any region box and the rest. Both halves are unindexed
// soups (every vertex copied), which is what an STL already is; an indexed
// board mesh works too, at the cost of duplicated vertices. Normals and uvs
// follow their vertices. With no regions the whole mesh is `outside` and
// `inside` is empty, so a caller can add both halves unconditionally.
export function splitByRegions(mesh, regions) {
  const { pos, nrm, uv, idx } = mesh;
  const has = (arr, n) => arr && arr.length >= n;
  const out = { inside: mk(), outside: mk() };
  function mk() { return { pos: [], nrm: [], uv: [], idx: [] }; }
  const triCount = idx ? idx.length / 3 : pos.length / 9;
  for (let t = 0; t < triCount; t++) {
    const ia = idx ? idx[t * 3] : t * 3;
    const ib = idx ? idx[t * 3 + 1] : t * 3 + 1;
    const ic = idx ? idx[t * 3 + 2] : t * 3 + 2;
    const cx = (pos[ia * 3] + pos[ib * 3] + pos[ic * 3]) / 3;
    const cy = (pos[ia * 3 + 1] + pos[ib * 3 + 1] + pos[ic * 3 + 1]) / 3;
    const cz = (pos[ia * 3 + 2] + pos[ib * 3 + 2] + pos[ic * 3 + 2]) / 3;
    let hit = false;
    for (const r of regions) if (inBox(cx, cy, cz, r.box)) { hit = true; break; }
    const dst = hit ? out.inside : out.outside;
    for (const i of [ia, ib, ic]) {
      dst.idx.push(dst.pos.length / 3);
      dst.pos.push(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]);
      if (has(nrm, (i + 1) * 3)) dst.nrm.push(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]);
      if (has(uv, (i + 1) * 2)) dst.uv.push(uv[i * 2], uv[i * 2 + 1]);
    }
  }
  return out;
}

// One fetch per page, shared by the tabs; null when the ledger is missing.
let ledgerPromise = null;
export function loadLedger(url = "devices/revisions.json") {
  if (!ledgerPromise) {
    ledgerPromise = fetch(url).then((r) => (r.ok ? r.json() : null)).catch(() => null);
  }
  return ledgerPromise;
}

// ── "which revision is my STL?" ─────────────────────────────────────────
// The same fingerprint gen_revisions.py records (fingerprint() there):
// triangle count, volume, surface area, bounding box and its low corner,
// centroid and second moments, all read off the triangle soup. STL vertices
// are float32 on both sides, so a file the generator measured measures the
// same here to well inside the ledger's tolerance.
export function meshFingerprint(mesh) {
  const { pos } = mesh;
  const idx = mesh.idx;
  const n = idx ? idx.length / 3 : pos.length / 9;
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  let vol6 = 0, area = 0;
  const c1 = [0, 0, 0], c3 = [0, 0, 0];
  const c2 = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  const v = (i, k) => pos[i * 3 + k];
  for (let t = 0; t < n; t++) {
    const ia = idx ? idx[t * 3] : t * 3, ib = idx ? idx[t * 3 + 1] : t * 3 + 1, ic = idx ? idx[t * 3 + 2] : t * 3 + 2;
    const a = [v(ia, 0), v(ia, 1), v(ia, 2)], b = [v(ib, 0), v(ib, 1), v(ib, 2)], c = [v(ic, 0), v(ic, 1), v(ic, 2)];
    for (const p of [a, b, c]) for (let k = 0; k < 3; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
    const v6 = a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    vol6 += v6;
    const s = [a[0] + b[0] + c[0], a[1] + b[1] + c[1], a[2] + b[2] + c[2]];
    for (let i = 0; i < 3; i++) {
      c1[i] += v6 * s[i];
      for (let j = i; j < 3; j++) c2[i][j] += v6 * (a[i] * a[j] + b[i] * b[j] + c[i] * c[j] + s[i] * s[j]);
      const A = a[i], B = b[i], C = c[i];
      c3[i] += v6 * (A ** 3 + B ** 3 + C ** 3 + A * A * (B + C) + B * B * (A + C) + C * C * (A + B) + A * B * C);
    }
    const ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const wx = c[0] - a[0], wy = c[1] - a[1], wz = c[2] - a[2];
    const nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
    area += Math.sqrt(nx * nx + ny * ny + nz * nz) / 2;
  }
  const vol = vol6 / 6;
  const flat = Math.abs(vol) < 1e-9;
  const cen = flat ? [0, 0, 0] : c1.map((x) => x / (4 * vol6));
  const e2 = (i, j) => (flat ? 0 : c2[i][j] / (20 * vol6));
  const e3 = (i) => (flat ? 0 : c3[i] / (20 * vol6));
  return {
    triangles: n, volume_mm3: Math.abs(vol), area_mm2: area,
    bbox_mm: hi.map((h, k) => h - lo[k]), lo_mm: lo, centroid_mm: cen,
    moments_mm2: [0, 1, 2].map((k) => Math.abs(e2(k, k) - cen[k] ** 2)),
    products_mm2: [[0, 1], [0, 2], [1, 2]].map(([i, j]) => e2(i, j) - cen[i] * cen[j]),
    third_mm3: [0, 1, 2].map((k) => e3(k) - 3 * cen[k] * e2(k, k) + 2 * cen[k] ** 3),
  };
}

// gen_revisions.py geom_equal(), read: null when two fingerprints are the
// same shape within the ledger's tolerance, else the first difference.
export function sameShape(a, b, tol) {
  if (a.triangles !== b.triangles) return `triangles ${a.triangles} vs ${b.triangles}`;
  for (const k of ["bbox_mm", "lo_mm"]) for (let i = 0; i < 3; i++)
    if (Math.abs(a[k][i] - b[k][i]) > tol.bbox_mm + 1e-6) return `${k}[${"xyz"[i]}]`;
  for (const k of ["volume_mm3", "area_mm2"])
    if (Math.abs(a[k] - b[k]) > tol.abs_mm3 + tol.rel * Math.abs(a[k]) + 1e-6) return k;
  for (let i = 0; i < 3; i++) {
    if (Math.abs(a.centroid_mm[i] - b.centroid_mm[i]) > tol.centroid_mm + 1e-6) return "centroid";
    if (Math.abs(a.moments_mm2[i] - b.moments_mm2[i]) > tol.moment_abs + tol.rel * Math.abs(a.moments_mm2[i]) + 1e-6) return "moments";
  }
  // the products of inertia and the third moments, when both sides carry them
  // (the same scale-relative tolerances as the generator's geom_equal)
  if (a.products_mm2 && b.products_mm2) {
    const pairs = [[0, 1], [0, 2], [1, 2]];
    for (let i = 0; i < 3; i++) {
      const scale = Math.sqrt(a.moments_mm2[pairs[i][0]] * a.moments_mm2[pairs[i][1]]);
      if (Math.abs(a.products_mm2[i] - b.products_mm2[i]) > tol.moment_abs + tol.rel * scale + 1e-6) return "products";
    }
  }
  if (a.third_mm3 && b.third_mm3 && tol.third_abs !== undefined) {
    for (let i = 0; i < 3; i++) {
      if (Math.abs(a.third_mm3[i] - b.third_mm3[i]) > tol.third_abs + tol.rel * a.moments_mm2[i] ** 1.5 + 1e-6) return "third";
    }
  }
  return null;
}

// Which part and revision a mesh is, searched across every part's history:
// { key, entry, rev, recorded, current, behind, since: [{rev, recorded, changes}],
//   regions: the union of every later rev's regions } — or null for a shape
// the ledger never recorded (a custom Customizer build, a remix, a scaled file).
export function identifyMesh(ledger, mesh, preferKey = null) {
  if (!ledger?.parts) return null;
  const fp = meshFingerprint(mesh);
  const tol = ledger.tolerance;
  const keys = Object.keys(ledger.parts);
  if (preferKey && ledger.parts[preferKey]) keys.unshift(preferKey);
  for (const key of keys) {
    const entry = ledger.parts[key];
    const hist = entry.history || [];
    for (let i = hist.length - 1; i >= 0; i--) {
      if (sameShape(fp, hist[i].geom, tol) !== null) continue;
      const since = hist.slice(i + 1).map((h) => ({ rev: h.rev, recorded: h.recorded, changes: h.changes }));
      return {
        key, entry, rev: hist[i].rev, recorded: hist[i].recorded,
        current: entry.rev, behind: since.length, since,
        regions: since.flatMap((h) => (Array.isArray(h.changes?.regions) ? h.changes.regions : [])),
      };
    }
  }
  return null;
}

// The region boxes as line segments (scene3d `lines: true` parts take index
// pairs), so a changed region reads even where the part has no surface left
// to tint — a feature a later revision REMOVED shows as an empty box.
export function regionBoxLines(regions) {
  const pos = [], idx = [];
  for (const r of regions) {
    const [x0, y0, z0, x1, y1, z1] = r.box;
    const base = pos.length / 3;
    for (const x of [x0, x1]) for (const y of [y0, y1]) for (const z of [z0, z1]) pos.push(x, y, z);
    for (const [a, b] of [[0, 1], [2, 3], [4, 5], [6, 7], [0, 2], [1, 3], [4, 6], [5, 7],
                          [0, 4], [1, 5], [2, 6], [3, 7]]) idx.push(base + a, base + b);
  }
  return { pos, nrm: new Array(pos.length).fill(0), uv: new Array((pos.length / 3) * 2).fill(0), idx };
}
