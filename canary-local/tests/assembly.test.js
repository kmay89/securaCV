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

// ── physical sanity, pinned after the field reports (the four bugs) ──────
// Ground truth: canary_wap_enclosure.scad (battery_weather) — board parks
// at the −X USB wall (board_cx −38.75), battery bay is central (batt_cx
// −0.15), the lid STL is the design flipped 180° for printing (assembled
// seat = z 17.2 with rot [180,0,0]), lid features cluster over the board
// (mag −44.75, lp −33.75), and the screws drive LAST into posts at
// ±47.65/±14.8 with heads flush at the lid top.
test("canary-wap: the assembly is physically true to the scad", () => {
  const a = asm.devices["canary-wap"];
  const by = Object.fromEntries(a.parts.map((p) => [p.id, p]));

  // board: USB end at the −X wall, camera stack up, USB yawed to the wall
  assert.ok(by.board.seated.pos[0] < -30, "board parks at the USB (−X) wall");
  assert.strictEqual(by.board.seated.rot[0], 90, "camera stack points up");
  assert.strictEqual(by.board.seated.rot[1], 180, "USB yawed toward the wall");

  // battery: central bay, flat on the floor (floor_t = 2)
  assert.ok(Math.abs(by.batt.seated.pos[0]) < 5, "battery bay is central");
  assert.strictEqual(by.batt.seated.pos[2], 2, "battery sits on the bay floor");
  assert.ok(by.batt.seated.pos[0] > by.board.seated.pos[0],
    "battery bay sits inboard of the board");

  // lid: flipped from print orientation, seated at base_h + lid_t
  assert.deepStrictEqual(by.lid.seated.rot, [180, 0, 0],
    "the printable lid is face-down; assembly flips it");
  assert.ok(Math.abs(by.lid.seated.pos[2] - 17.2) < 0.05, "lid top at 15.2 + 2");

  // lid features live over the BOARD end, not the GPS bay
  assert.ok(by.magnet.seated.pos[0] < -30, "magnet pocket is over the board end");
  assert.ok(by.lp.seated.pos[0] < -30, "light pipe is over the board end");

  // screws: their own beat AFTER the lid, heads flush at the lid top
  assert.ok(by.screws.step > by.lid.step, "screws drive after the lid closes");
  assert.ok(Math.abs(by.screws.iz - 17.2) < 0.05, "screw heads land at the lid top");
  for (const [x, y] of by.screws.instances) {
    assert.ok(Math.abs(Math.abs(x) - 47.65) < 0.05 && Math.abs(Math.abs(y) - 14.8) < 0.05,
      "screws at the scad's corner posts");
  }

  // the step rail reads in build order
  const titles = a.steps.map((s) => s.title.toLowerCase());
  const idx = (t) => titles.findIndex((x) => x.includes(t));
  assert.ok(idx("battery") < idx("board"), "battery before board (per the catalog)");
  assert.ok(idx("close the lid") < idx("screws"), "lid closes before screws drive");
});
