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
        const dir = p.base === "preview" ? join(ROOT, "enclosures/preview") : join(REPO, "docs/hardware/enclosure");
        assert.ok(existsSync(join(dir, p.file)), `missing STL: ${p.file}`);
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

// ── the drafting gate: every device obeys the assembly-order rules ───────
// (assets/assembly-rules.js — supports open/close the build, internals
// before shells, seals before lids, fasteners last AND outermost, external
// accessories after the hardware). The Assemble tab shows this same check.
for (const [dev, d] of Object.entries(asm.devices)) {
  test(`${dev}: assembly order passes the drafting rules`, async () => {
    const { validateDevice } = await import("../assets/assembly-rules.js");
    const { ok, violations } = validateDevice(d);
    assert.ok(ok, `ordering violations:\n  ${violations.join("\n  ")}`);
  });
}

// ── dual-unit caliper formatting (the parts-list readout) ────────────────
test("caliper readout: mm · decimal inch · nearest-1/64 fraction", async () => {
  const { fmtLen, fracInch } = await import("../assets/assembly-rules.js");
  assert.strictEqual(fracInch(25.4), "1");
  assert.strictEqual(fracInch(12.7), "1/2");
  assert.strictEqual(fracInch(52.0), "2 3/64");     // watch drum Ø
  assert.strictEqual(fmtLen(25.4, "mm"), "25.4 mm");
  assert.strictEqual(fmtLen(25.4, "in"), "1.000″ · 1″");
  assert.strictEqual(fmtLen(52, "all"), "52.0 mm · 2.047″ · 2 3/64″");
});

// ── physical sanity for the two display builds (same spirit as the WAP
// pins): geometry from canary_watch_station.scad / canary_dash_display.scad
test("canary-display-watch: seated on the stand's 25° pocket axis", () => {
  const a = asm.devices["canary-display-watch"];
  const by = Object.fromEntries(a.parts.map((p) => [p.id, p]));
  // drum back cap at the scad's pocket start (0, 6, sh−4 = 39.98)
  assert.deepStrictEqual(by.drum.seated.pos, [0, 6, 39.98]);
  assert.strictEqual(by.drum.seated.rot[0], 65, "drum axis reclined 25° from vertical");
  // bezel flips from its face-down print (65 + 180)
  assert.strictEqual(by.bezel.seated.rot[0], 245);
  // screws: the very last step, outermost explode, at the scad's post radius
  const maxOther = Math.max(...a.parts.filter((p) => p.id !== "screws").map((p) => p.step));
  assert.ok(by.screws.step > maxOther, "screws drive last");
  for (const [x] of by.screws.instances) assert.ok(Math.abs(Math.abs(x) - 22.5) < 0.05, "screws on the post circle");
});

test("canary-display-dash: panel seats against the frame lip, screws from the back", () => {
  const a = asm.devices["canary-display-dash"];
  const by = Object.fromEntries(a.parts.map((p) => [p.id, p]));
  // module centre derived from the stand's channel + fin geometry
  assert.ok(Math.abs(by.panel.seated.pos[1] - (-1.04)) < 0.05, "panel glass 5.6 in front of module centre");
  assert.strictEqual(by.frame.seated.rot[0], 245, "frame flips from its face-down print");
  assert.strictEqual(by.back.seated.rot[0], 65, "back keeps its outer-face-out print orientation");
  assert.strictEqual(by.screws.instances.length, 4, "four corner-lobe screws");
  const maxOther = Math.max(...a.parts.filter((p) => p.id !== "screws").map((p) => p.step));
  assert.ok(by.screws.step > maxOther, "screws drive last");
});

test("canary-wap: the sun shield stands on its posts above the lid", () => {
  const a = asm.devices["canary-wap"];
  const shield = a.parts.find((p) => p.id === "shield");
  // printed installed-top-down (scad: "bed face = the installed top"),
  // so assembly flips it; posts are 7.6 (sh_t 1.6 + sh_gap 6) — installed
  // top at lid top 17.2 + 7.6 = 24.8, post tips resting on the lid
  assert.deepStrictEqual(shield.seated.rot, [180, 0, 0], "shield flips like the lid");
  assert.ok(Math.abs(shield.seated.pos[2] - 24.8) < 0.05, "posts stand on the lid top");
});
