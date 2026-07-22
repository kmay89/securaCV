// canary-local/tests/boards.test.js — the boards.json honesty gate.
//
// boards.json claims dimensions, triangle counts and material colours for each
// committed board GLB. This test re-derives those facts from the committed mesh
// using the page's OWN loader (assets/glb.js) and asserts they match — so the
// JSON can never drift from what the browser will actually render. It also
// checks the device→board map and that every board carries its provenance and
// the pins the firmware uses. Runs in CI with node only (no cascadio).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const boards = JSON.parse(readFileSync(join(ROOT, "devices/boards.json"), "utf8"));

async function loadGLB(relToCanaryLocal) {
  const { parseGLB } = await import("../assets/glb.js");
  const buf = readFileSync(join(ROOT, relToCanaryLocal)); // Buffer → zero-copy view
  return parseGLB(buf);
}

const hex = (c) => "#" + c.map((x) => Math.round(x * 255).toString(16).padStart(2, "0")).join("");

test("device_board maps every device to real board(s)", () => {
  for (const [device, mapped] of Object.entries(boards.device_board)) {
    // a device may carry more than one board (primary first); tolerate the
    // legacy single-string form too
    const list = Array.isArray(mapped) ? mapped : [mapped];
    assert.ok(list.length, `${device} maps to no board`);
    for (const bid of list) {
      assert.ok(boards.boards[bid], `${device} → ${bid} is not a real board`);
    }
  }
});

for (const [bid, b] of Object.entries(JSON.parse(readFileSync(join(ROOT, "devices/boards.json"), "utf8")).boards)) {
  test(`${bid}: committed GLB matches boards.json (no drift)`, async () => {
    assert.ok(existsSync(join(ROOT, b.glb)), `missing committed GLB: ${b.glb}`);
    const parsed = await loadGLB(b.glb);

    // triangles + parts are exact (same loader authored the JSON)
    assert.strictEqual(parsed.triangles, b.triangles, "triangle count drifted");
    assert.strictEqual(parsed.parts.length, b.parts, "part count drifted");

    // dimensions match to 0.01 mm
    parsed.bbox.size.forEach((v, i) => {
      assert.ok(Math.abs(v - b.dims_mm[i]) < 0.02,
        `dim[${i}] ${v.toFixed(2)} ≠ ${b.dims_mm[i]}`);
    });

    // material buckets (colour → triangles) match
    const got = new Map();
    for (const p of parsed.parts) {
      const h = hex(p.color);
      got.set(h, (got.get(h) || 0) + p.pos.length / 9);
    }
    assert.strictEqual(got.size, b.materials.length, "material bucket count drifted");
    for (const m of b.materials) {
      assert.ok(got.has(m.color), `material ${m.color} missing from mesh`);
      assert.strictEqual(got.get(m.color), m.triangles, `material ${m.color} triangle count drifted`);
    }
  });

  test(`${bid}: carries provenance, source STEP and firmware pins`, () => {
    assert.ok(b.provenance && b.provenance.length > 20, "provenance missing");
    // vendor-CAD boards carry a real source STEP; procedural boards (built from
    // photos/spec, no vendor CAD) carry none — the provenance must say so
    if (b.source_step) {
      assert.ok(existsSync(join(REPO, b.source_step)), `vendor STEP missing: ${b.source_step}`);
    } else {
      assert.ok(/procedural|dimensional model|reverse-engineer/i.test(b.provenance),
        `${bid}: no source_step but provenance doesn't declare it procedural`);
    }
    assert.ok(Array.isArray(b.pinout) && b.pinout.length > 0, "pinout missing");
    assert.ok(b.pose && typeof b.pose.rx === "number", "default pose missing");
    for (const p of b.pinout) assert.ok(p.label && p.pin, "pin row incomplete");
  });
}
