// canary-local/tests/device_models.test.js — the device 3D-model gates.
//
// The committed device GLBs (canary-local/models/ + the desktop Flasher's
// byte-identical desktop/src/models/ copies) are generated from the fleet
// massing by tools/figures/gen_device_glbs.mjs. This test holds the three
// promises the website's model pipeline taught us to hold (its
// tests/models.test.mjs is the ancestor), using the flasher page's OWN
// loader (assets/glb.js) as the oracle:
//
//   1. bytes match the generator (both trees, no strays, no gaps),
//   2. every triangle's geometric normal agrees with its stored normals —
//      a face can never be silently inverted,
//   3. no two different-material front caps share a plane with overlapping
//      footprints (the z-fighting shimmer), and the model is real-world
//      scale (centimeters, meters-as-mm never).
//
// The confidence ladder is enforced upstream (only prototype-or-better
// figures get a model at all); here we just assert no model exists for a
// figure the ledger calls an idea.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const MODELS = join(ROOT, "models");
const DESKTOP_MODELS = join(REPO, "desktop/src/models");
const ledger = JSON.parse(readFileSync(join(ROOT, "devices/figures.json"), "utf8"));

async function generator() {
  return import("../tools/figures/gen_device_glbs.mjs");
}

async function parse(bytes) {
  const { parseGLB } = await import("../assets/glb.js");
  return parseGLB(bytes);
}

test("committed device models match their generator, in both flasher trees", async () => {
  const { builtDeviceFigures, buildOne } = await generator();
  const want = new Map();
  for (const fig of builtDeviceFigures()) want.set(`${fig.id}.glb`, buildOne(fig));
  for (const dir of [MODELS, DESKTOP_MODELS]) {
    assert.ok(existsSync(dir), `${dir} is missing — run gen_device_glbs.mjs`);
    const have = readdirSync(dir).filter((f) => f.endsWith(".glb")).sort();
    assert.deepStrictEqual(have, [...want.keys()].sort(),
      `${dir}: committed model set differs from the generator's`);
    for (const [name, bytes] of want) {
      const disk = readFileSync(join(dir, name));
      assert.strictEqual(disk.length, bytes.length, `${dir}/${name}: size drift`);
      assert.ok(disk.equals(Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength)),
        `${dir}/${name}: bytes drift — regenerate with gen_device_glbs.mjs`);
    }
  }
});

test("no model exists for a figure the ladder calls an idea", async () => {
  const ideas = new Set(ledger.figures.filter((f) => f.confidence === "idea").map((f) => `${f.id}.glb`));
  for (const f of readdirSync(MODELS)) {
    assert.ok(!ideas.has(f), `${f}: an idea renders as a dashed ghost, never a solid 3D body`);
  }
});

test("normals: every triangle agrees with its vertex normals; scale is real-world", async () => {
  for (const file of readdirSync(MODELS).filter((f) => f.endsWith(".glb"))) {
    const scene = await parse(readFileSync(join(MODELS, file)));
    assert.ok(scene.triangles > 0, `${file}: empty model`);
    // parseGLB scales meters → mm; a device is centimeters tall, so its mm
    // bbox must land in [10, 400] on its largest axis. A meters-as-mm slip
    // shows up as 0.0x here; an mm-as-meters slip as 10000+.
    const largest = Math.max(...scene.bbox.size);
    assert.ok(largest >= 10 && largest <= 400,
      `${file}: largest axis ${largest} mm — not real-device scale`);
    for (const part of scene.parts) {
      const { pos, nrm, idx } = part;
      for (let t = 0; t < idx.length; t += 3) {
        const [a, b, c] = [idx[t], idx[t + 1], idx[t + 2]];
        const A = [pos[a * 3], pos[a * 3 + 1], pos[a * 3 + 2]];
        const B = [pos[b * 3], pos[b * 3 + 1], pos[b * 3 + 2]];
        const C = [pos[c * 3], pos[c * 3 + 1], pos[c * 3 + 2]];
        const u = [B[0] - A[0], B[1] - A[1], B[2] - A[2]];
        const v = [C[0] - A[0], C[1] - A[1], C[2] - A[2]];
        const g = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
        const glen = Math.hypot(...g);
        if (glen < 1e-9) continue; // degenerate sliver — ignore
        let dot = 0;
        for (const i of [a, b, c]) {
          const n = [nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]];
          const nlen = Math.hypot(...n);
          assert.ok(Math.abs(nlen - 1) < 1e-3, `${file}: non-unit normal on ${part.name || "part"}`);
          dot += (g[0] * n[0] + g[1] * n[1] + g[2] * n[2]) / (glen * nlen);
        }
        assert.ok(dot > 0, `${file}: inverted triangle (geometric vs stored normal)`);
      }
    }
  }
});

test("coplanar guard: different-material front caps never share a plane", async () => {
  // Mirror of the website's guard: constant-Z (toward-viewer) caps of two
  // different-material parts at the SAME plane with real XY overlap is the
  // per-pixel shimmer defect. Massing's own guard should make this
  // unreachable; this asserts the export preserved it. Tolerance is 0.05 mm
  // (float jitter), NOT the website's 0.2 mm: massing legitimately stacks
  // materials 0.1 mm proud (its EPS convention), and only an exact-plane
  // tie is the defect.
  for (const file of readdirSync(MODELS).filter((f) => f.endsWith(".glb"))) {
    const scene = await parse(readFileSync(join(MODELS, file)));
    const caps = []; // { z, material, box:{x0,x1,y0,y1} }
    for (const [pi, part] of scene.parts.entries()) {
      const { pos, nrm, idx } = part;
      for (let t = 0; t < idx.length; t += 3) {
        const a = idx[t];
        const n = [nrm[a * 3], nrm[a * 3 + 1], nrm[a * 3 + 2]];
        if (n[2] < 0.999) continue; // only +Z (toward viewer) faces
        const xs = [];
        const ys = [];
        const zs = [];
        for (const i of [idx[t], idx[t + 1], idx[t + 2]]) {
          xs.push(pos[i * 3]);
          ys.push(pos[i * 3 + 1]);
          zs.push(pos[i * 3 + 2]);
        }
        caps.push({
          part: pi, color: part.color.join(","),
          z: (zs[0] + zs[1] + zs[2]) / 3,
          box: { x0: Math.min(...xs), x1: Math.max(...xs), y0: Math.min(...ys), y1: Math.max(...ys) },
        });
      }
    }
    for (let i = 0; i < caps.length; i++) {
      for (let j = i + 1; j < caps.length; j++) {
        const a = caps[i];
        const b = caps[j];
        if (a.color === b.color) continue;
        if (Math.abs(a.z - b.z) > 0.05) continue;
        const ox = Math.min(a.box.x1, b.box.x1) - Math.max(a.box.x0, b.box.x0);
        const oy = Math.min(a.box.y1, b.box.y1) - Math.max(a.box.y0, b.box.y0);
        assert.ok(!(ox > 0.5 && oy > 0.5),
          `${file}: two materials share a front plane (z≈${a.z.toFixed(3)} mm) with `
          + "overlapping footprints — hold one proud of the other (massing.mjs)");
      }
    }
  }
});

test("the two flasher renderers are the same bytes (two frontends, one behavior)", () => {
  const a = readFileSync(join(ROOT, "assets/fig3d.js"), "utf8");
  const b = readFileSync(join(REPO, "desktop/src/fig3d.js"), "utf8");
  assert.strictEqual(a, b,
    "assets/fig3d.js and desktop/src/fig3d.js have drifted — copy one over the other");
});
