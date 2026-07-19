// canary-local/tests/assembly.test.js — the assembly honesty gate.
//
// The Assemble tab choreographs REAL parts with the catalog's REAL step text.
// This test pins that: every part resolves to a file that exists (enclosure STL,
// committed board GLB, or a known procedural builder), every step that quotes
// the catalog maps to a real README assembly step (carried in build.json), and
// every part that claims a quantity matches the drift-gated BOM. So the guided
// build can never quietly point at a missing part or misstate how many screws.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const asm = JSON.parse(readFileSync(join(ROOT, "devices/assembly.json"), "utf8"));
const build = JSON.parse(readFileSync(join(ROOT, "devices/build.json"), "utf8"));
const boards = JSON.parse(readFileSync(join(ROOT, "devices/boards.json"), "utf8"));

for (const [dev, d] of Object.entries(asm.devices)) {
  test(`${dev}: every part resolves to a real file/builder`, async () => {
    const { PARTS } = await import("../assets/assembly.js");
    for (const p of d.parts) {
      if (p.source === "stl") {
        assert.ok(existsSync(join(REPO, "docs/hardware/enclosure", p.file)), `missing STL: ${p.file}`);
      } else if (p.source === "board") {
        assert.ok(boards.boards[p.board], `unknown board: ${p.board}`);
        assert.ok(existsSync(join(ROOT, boards.boards[p.board].glb)), `missing board GLB: ${p.board}`);
      } else if (p.source === "proc") {
        assert.strictEqual(typeof PARTS[p.part], "function", `unknown procedural part: ${p.part}`);
      } else {
        assert.fail(`unknown part source: ${p.source}`);
      }
      if (p.color && !Array.isArray(p.color)) assert.ok(asm.palette[p.color], `unknown palette colour: ${p.color}`);
    }
  });

  test(`${dev}: steps quote the catalog's real §Assembly, and cover every part`, () => {
    const readme = build.devices[dev]?.assembly?.steps || [];
    for (const s of d.steps) {
      if (s.readmeStep != null) {
        assert.ok(readme[s.readmeStep], `readmeStep ${s.readmeStep} out of range (build.json has ${readme.length})`);
      } else {
        assert.ok(s.note && s.note.length > 10, "a non-catalog step needs its own note");
      }
    }
    const maxStep = Math.max(...d.parts.map((p) => p.step));
    assert.ok(d.steps.length > maxStep, `parts reference step ${maxStep} but only ${d.steps.length} steps are defined`);
  });

  test(`${dev}: part quantities match the BOM`, () => {
    const rows = build.devices[dev]?.bom?.rows || [];
    const byRef = Object.fromEntries(rows.map((r) => [r.ref, r]));
    for (const p of d.parts) {
      if (p.ref && byRef[p.ref] && p.qty != null) {
        assert.strictEqual(String(p.qty), String(byRef[p.ref].qty),
          `${p.ref}: assembly says ×${p.qty}, BOM says ×${byRef[p.ref].qty}`);
      }
      if (p.instances) assert.strictEqual(p.instances.length, p.qty, `${p.id}: ${p.instances.length} placements ≠ qty ${p.qty}`);
    }
  });
}
