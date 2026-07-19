// canary-local/tools/pin_anchors.mjs — pin-anchor authoring aid for the Board Room.
//
// The Board Room (boards.html) hangs its pin flags and wire endpoints on
// `anchor: [x,y,z]` coordinates carried by each pinout row in
// boards/boards.config.json (mirrored into devices/boards.json). Those numbers
// must come from the real mesh, not from eyeballing a render — so this tool
// reads a committed GLB with the page's OWN loader (assets/glb.js), buckets
// triangles by material colour, clusters each bucket into connected islands
// (union-find on a 0.5 mm vertex grid), and prints every island's centre and
// extent in raw GLB millimetres (the same frame the anchors are stored in;
// the viewer re-centres by bbox at load, exactly like the anchors' consumer).
//
//   node canary-local/tools/pin_anchors.mjs boards/<id>.glb [#colour] [minTris]
//
// Typical use: run with the pad colour (XIAO castellations are #e7c863) and
// read off the 14 pad centres; run with a connector-shell colour to anchor
// USB-C / Grove / JST rows. Authoring aid only — CI never runs it; the
// committed anchors are gated by tests/boards.test.js (inside the mesh bbox).
import { parseGLB } from "../assets/glb.js";
import { readFileSync } from "node:fs";

const [path, wantColor, minTrisArg] = process.argv.slice(2);
if (!path) {
  console.error("usage: pin_anchors.mjs <file.glb> [#rrggbb] [minTris]");
  process.exit(2);
}
const minTris = Number(minTrisArg || 4);
const { parts, bbox } = parseGLB(readFileSync(path));
const hex = (c) => "#" + c.map((x) => Math.round(x * 255).toString(16).padStart(2, "0")).join("");

const fmt = (v) => +v.toFixed(2);
console.log(`bbox min ${bbox.min.map(fmt)}  max ${bbox.max.map(fmt)}  center ${bbox.center.map(fmt)}`);

// gather triangles per colour
const byColor = new Map();
for (const p of parts) {
  const h = hex(p.color);
  if (wantColor && h !== wantColor.toLowerCase()) continue;
  if (!byColor.has(h)) byColor.set(h, []);
  byColor.get(h).push(p);
}

const GRID = 0.5; // mm — merges vertices across shared pad edges
const keyOf = (x, y, z) =>
  `${Math.round(x / GRID)},${Math.round(y / GRID)},${Math.round(z / GRID)}`;

for (const [color, plist] of byColor) {
  // union-find over quantized vertex cells
  const parent = new Map();
  const find = (a) => {
    let r = a;
    while (parent.get(r) !== r) r = parent.get(r);
    while (parent.get(a) !== a) { const n = parent.get(a); parent.set(a, r); a = n; }
    return r;
  };
  const union = (a, b) => {
    const ra = find(a), rb = find(b);
    if (ra !== rb) parent.set(ra, rb);
  };
  const seen = (k) => { if (!parent.has(k)) parent.set(k, k); return k; };

  const tris = []; // {keys:[k0,k1,k2], center:[x,y,z]}
  for (const p of plist) {
    for (let t = 0; t < p.pos.length; t += 9) {
      const ks = [];
      let cx = 0, cy = 0, cz = 0;
      for (let v = 0; v < 3; v++) {
        const x = p.pos[t + v * 3], y = p.pos[t + v * 3 + 1], z = p.pos[t + v * 3 + 2];
        cx += x / 3; cy += y / 3; cz += z / 3;
        ks.push(seen(keyOf(x, y, z)));
      }
      union(ks[0], ks[1]); union(ks[1], ks[2]);
      tris.push({ k: ks[0], c: [cx, cy, cz] });
    }
  }
  // collect islands
  const islands = new Map(); // root → {n, sum, min, max}
  for (const t of tris) {
    const r = find(t.k);
    let isl = islands.get(r);
    if (!isl) islands.set(r, (isl = {
      n: 0, sum: [0, 0, 0],
      min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity],
    }));
    isl.n++;
    for (let i = 0; i < 3; i++) {
      isl.sum[i] += t.c[i];
      if (t.c[i] < isl.min[i]) isl.min[i] = t.c[i];
      if (t.c[i] > isl.max[i]) isl.max[i] = t.c[i];
    }
  }
  const rows = [...islands.values()]
    .filter((i) => i.n >= minTris)
    .map((i) => ({
      center: i.sum.map((s) => fmt(s / i.n)),
      size: i.max.map((m, k) => fmt(m - i.min[k])),
      tris: i.n,
    }))
    .sort((a, b) => a.center[0] - b.center[0] || a.center[2] - b.center[2]);
  console.log(`\n${color}: ${rows.length} islands ≥${minTris} tris`);
  for (const r of rows) {
    console.log(`  center [${r.center.join(", ")}]  size [${r.size.join(", ")}]  tris ${r.tris}`);
  }
}
