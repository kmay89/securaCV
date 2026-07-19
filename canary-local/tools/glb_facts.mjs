// canary-local/tools/glb_facts.mjs — geometry facts for a committed board GLB,
// computed by the SAME loader the browser uses (assets/glb.js). gen_boards.py
// shells out to this so boards.json's dims/triangles/materials are exactly what
// the page will render — and tests/boards.test.js re-runs it to gate drift.
//
//   node canary-local/tools/glb_facts.mjs <path-to.glb>  →  JSON on stdout
import { parseGLB } from "../assets/glb.js";
import { readFileSync } from "node:fs";

const path = process.argv[2];
if (!path) { console.error("usage: glb_facts.mjs <file.glb>"); process.exit(2); }
const buf = readFileSync(path); // Node Buffer is an ArrayBufferView → zero-copy
const { parts, bbox, triangles } = parseGLB(buf);

const hex = (c) => "#" + c.map((x) => Math.round(x * 255).toString(16).padStart(2, "0")).join("");
const mats = new Map();
for (const p of parts) {
  const h = hex(p.color);
  mats.set(h, (mats.get(h) || 0) + p.pos.length / 9);
}
const materials = [...mats.entries()]
  .sort((a, b) => b[1] - a[1])
  .map(([color, tris]) => ({ color, triangles: tris }));

process.stdout.write(JSON.stringify({
  triangles,
  parts: parts.length,
  dims_mm: bbox.size.map((v) => +v.toFixed(2)),
  center_mm: bbox.center.map((v) => +v.toFixed(2)),
  materials,
}, null, 2));
