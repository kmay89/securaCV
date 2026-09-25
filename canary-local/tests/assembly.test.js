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
      if (p.color && !Array.isArray(p.color)) assert.ok(asm.palette[p.color], `unknown palette color: ${p.color}`);
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

  // Where a part SITS is the CAD's, not a typed number: every printed shell
  // and every fastener pattern carries `pose: "cad"`, which only
  // docs/hardware/enclosure/gen_assembly_poses.py writes (and --checks in the
  // enclosure CI). Typed poses drifted until every released lid sat sunk
  // 1-1.4 mm into its base and the screws stood off their posts.
  test(`${dev}: shells and fasteners are posed from the CAD`, () => {
    for (const p of d.parts) {
      if (p.source === "stl" || p.part === "screw") {
        assert.strictEqual(p.pose, "cad", `${p.id}: seated pose is typed — run gen_assembly_poses.py`);
      }
    }
  });

  // A board placed from its PCB datum is only as right as the datum: the
  // generator's numbers were measured off the committed GLB, so re-measure
  // the GLB here with the page's own loader and hold them to it.
  test(`${dev}: board datums still describe the committed GLBs`, async () => {
    const { parseGLB } = await import("../assets/glb.js");
    for (const p of d.parts.filter((q) => q.glb_datum)) {
      const { bbox } = parseGLB(readFileSync(join(ROOT, boards.boards[p.board].glb)));
      p.glb_datum.center.forEach((v, i) => assert.ok(Math.abs(v - bbox.center[i]) < 0.01,
        `${p.board}: GLB center moved (${bbox.center.map((c) => c.toFixed(3))}) — re-measure BOARDS in gen_assembly_poses.py`));
      p.glb_datum.datum.forEach((v, i) => assert.ok(v >= bbox.min[i] - 1e-6 && v <= bbox.max[i] + 1e-6,
        `${p.board}: datum outside the GLB`));
    }
  });

  test(`${dev}: part quantities match the BOM`, () => {
    const rows = build.devices[dev]?.bom?.rows || [];
    const byRef = Object.fromEntries(rows.map((r) => [r.ref, r]));
    // parts sharing a RefDes (the WAP's lid screws and its shield's screws are
    // one BOM row) sum to the row's quantity
    const byRefQty = {};
    for (const p of d.parts) if (p.ref && p.qty != null) byRefQty[p.ref] = (byRefQty[p.ref] || 0) + p.qty;
    for (const [ref, qty] of Object.entries(byRefQty)) {
      if (byRef[ref]) assert.strictEqual(String(qty), String(byRef[ref].qty),
        `${ref}: assembly says ×${qty}, BOM says ×${byRef[ref].qty}`);
    }
    for (const p of d.parts) {
      if (p.instances) assert.strictEqual(p.instances.length, p.qty, `${p.id}: ${p.instances.length} placements ≠ qty ${p.qty}`);
    }
  });
}

// ── the README → build.json → Assemble join ──────────────────────────────
// gen_enclosures.py lifts every '## Assembly' block of the catalog README into
// build.json (and refuses a block it cannot attribute, or two blocks for one
// device). This pins the other half: every block landed on a device the
// Assemble tab choreographs, and nothing choreographed lacks its block.
test("every README §Assembly block is carried into build.json, one per choreographed device", () => {
  const readme = readFileSync(join(REPO, "docs/hardware/enclosure/README.md"), "utf8");
  const blocks = readme.match(/^## Assembly\s*$/gm) || [];
  const carried = Object.keys(build.devices).filter((d) => build.devices[d].assembly).sort();
  assert.strictEqual(carried.length, blocks.length,
    `${blocks.length} README '## Assembly' blocks but ${carried.length} carried into build.json`);
  assert.deepStrictEqual(carried, Object.keys(asm.devices).sort(),
    "the devices with catalog steps are exactly the devices the Assemble tab choreographs");
});

// The two in-development display builds (Watch station, Dash) quote the
// catalog for every step. Their status caveat is README prose ABOVE the
// numbered list, so it travels as `caveat` — never as a step a later print
// validation would have to find and delete from the middle of the build.
// Each choreographed step names its part in its title, and the README step it
// quotes opens on that same part — so a reordered or inserted README step
// cannot leave the Assemble tab captioning the wrong step.
const DISPLAY_STEP_PARTS = {
  "canary-display-watch": ["stand", "drum", "XIAO", "bore", "bezel"],
  "canary-display-dash": ["desk cradle", "bezel frame", "panel", "vented back", "M2"],
};
for (const [dev, vocab] of [["canary-display-watch", /Round Display/], ["canary-display-dash", /4\.3″ panel/]]) {
  test(`${dev}: every step quotes the catalog, with the dev caveat beside the steps`, () => {
    const a = build.devices[dev].assembly;
    assert.ok(a, `${dev}: no README §Assembly carried into build.json`);
    assert.match(a.steps.join(" "), vocab, "the block landed on the right device");
    assert.match(a.caveat || "", /not print-validated/, "the dev caveat is carried");
    assert.doesNotMatch(a.caveat || "", /\bverified\b/i,
      "the caveat says checked, not verified (AGENTS.md rule 4 keeps 'verified' for a signature check)");
    for (const s of a.steps) {
      assert.doesNotMatch(s, /print-validated|in development/i, `caveat leaked into a step: ${s.slice(0, 60)}`);
    }
    const d = asm.devices[dev];
    assert.deepStrictEqual(d.steps.map((s) => s.readmeStep), a.steps.map((_, i) => i),
      "choreography step i quotes README step i");
    d.steps.forEach((s, i) => {
      const part = DISPLAY_STEP_PARTS[dev][i];
      const lead = a.steps[s.readmeStep].split(/\.\s/)[0];
      assert.ok(s.title.includes(part), `step ${i} ("${s.title}") is the ${part} step`);
      assert.ok(lead.includes(part), `README step ${s.readmeStep} opens on the ${part}: "${lead.slice(0, 60)}"`);
    });
    assert.match(d.assembly_source, /README\.md §Assembly/);
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
  // the GLB carries its USB at −X and components +Y: a quarter turn about X
  // stands the stack up and leaves the USB at the −X wall (a 180° yaw here
  // once pointed it at the battery, away from its opening)
  assert.deepStrictEqual(by.board.seated.rot, [90, 0, 0], "camera stack up, USB at the −X wall");

  // battery: central bay, flat on the floor (floor_t = 2)
  assert.ok(Math.abs(by.batt.seated.pos[0]) < 5, "battery bay is central");
  assert.strictEqual(by.batt.seated.pos[2], 2, "battery sits on the bay floor");
  assert.ok(by.batt.seated.pos[0] > by.board.seated.pos[0],
    "battery bay sits inboard of the board");

  // lid: flipped from print orientation, seated at base_h + lid_t
  assert.deepStrictEqual(by.lid.seated.rot, [180, 0, 0],
    "the printable lid is face-down; assembly flips it");
  assert.ok(by.lid.seated.pos[2] > by.gasket.seated.pos[2], "the lid closes over the gasket");

  // lid features live over the BOARD end, not the GPS bay
  assert.ok(by.magnet.seated.pos[0] < -30, "magnet pocket is over the board end");
  assert.ok(by.lp.seated.pos[0] < -30, "light pipe is over the board end");

  // screws: their own beat AFTER the lid, heads flush at the lid top
  assert.ok(by.screws.step > by.lid.step, "screws drive after the lid closes");
  // (their x/y are post_xy(), written by gen_assembly_poses.py and --checked
  // against the CAD in the enclosure CI — a number pinned here would be a
  // second, unchecked copy of it)
  // driven from the BACK, through the plate: every head sits inside the plate,
  // under the seal line (below the gasket's seat on the ledge) and turned
  // over (irot 180) — the lid face carries no screw
  for (const [, , z] of by.screws.instances)
    assert.ok(z < by.gasket.seated.pos[2], "the plate screws sit inside the plate, under the seal line, not on the lid");
  assert.deepStrictEqual(by.screws.irot, [180, 0, 0], "lid screws drive upward from the back");

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
  assert.strictEqual(fracInch(52.0), "2 3/64");     // formatter check (arbitrary length)
  assert.strictEqual(fmtLen(25.4, "mm"), "25.4 mm");
  assert.strictEqual(fmtLen(25.4, "in"), "1.000″ · 1″");
  assert.strictEqual(fmtLen(52, "all"), "52.0 mm · 2.047″ · 2 3/64″");
});

// ── physical sanity for the two display builds (same spirit as the WAP
// pins): geometry from canary_watch_station.scad / canary_dash_display.scad
test("canary-display-watch: seated in the stand's cradle divot (scad v0.2 echo)", () => {
  const a = asm.devices["canary-display-watch"];
  const by = Object.fromEntries(a.parts.map((p) => [p.id, p]));
  // drum back cap at the scad's own DRUM SEAT echo: [0, 3.03, 27.36] rot [65,0,0]
  by.drum.seated.pos.forEach((v, i) => assert.ok(Math.abs(v - [0, 3.03, 27.36][i]) < 0.01, "drum at the DRUM SEAT"));
  assert.deepStrictEqual(by.drum.seated.rot, [65, 0, 0],
    "drum axis reclined 25° from vertical, USB azimuth (270) into the chin slot — no 180 flip");
  // the XIAO pins into the display's BACK socket: component/USB side toward the
  // drum floor (the 180 in rx: 65+180 = 245), USB end spun to the slot azimuth
  assert.deepStrictEqual(by.xiao.seated.rot, [245, 0, 270], "XIAO upside-down in the socket, USB at 270");
  assert.ok(by.xiao.seated.pos[1] < by.drum.seated.pos[1],
    "XIAO offset toward the USB slot azimuth (the socket rows sit off-center on the disc)");
  // display: glass along the drum axis, socket-row axis spun onto the USB azimuth
  assert.deepStrictEqual(by.display.seated.rot, [155, 270, 0]);
  // bezel flips from its face-down print (65 + 180) and SNAPS — no fasteners in v0.2
  assert.strictEqual(by.bezel.seated.rot[0], 245);
  assert.ok(!a.parts.some((p) => p.part === "screw"),
    "v0.2 drum is fastener-free (snap bezel) — the Ø43.9 back-parts envelope leaves no room for posts");
  // bezel snaps last and outermost
  const maxOther = Math.max(...a.parts.filter((p) => p.id !== "bezel").map((p) => p.step));
  assert.ok(by.bezel.step >= maxOther, "the snap bezel closes the build");
});

test("canary-display-dash: panel seats against the frame lip, screws from the back", () => {
  const a = asm.devices["canary-display-dash"];
  const by = Object.fromEntries(a.parts.map((p) => [p.id, p]));
  // the frame turns face-out about Y (the CAD's placement), so its USB wall
  // stays at the bottom with the back's: Rx(65)·Ry(180) ≡ [245, 0, 180]
  assert.deepStrictEqual(by.frame.seated.rot, [245, 0, 180], "frame turned face-out about Y");
  assert.strictEqual(by.back.seated.rot[0], 65, "back keeps its outer-face-out print orientation");
  assert.strictEqual(by.screws.instances.length, 4, "four corner-lobe screws");
  const maxOther = Math.max(...a.parts.filter((p) => p.id !== "screws").map((p) => p.step));
  assert.ok(by.screws.step > maxOther, "screws drive last");
});

test("canary-wap: the sun shield stands on its posts above the lid", () => {
  const a = asm.devices["canary-wap"];
  const shield = a.parts.find((p) => p.id === "shield");
  // printed installed-top-down (scad: "bed face = the installed top"),
  // so assembly flips it; its tubes are 8.4 (sh_t 2.4 + sh_gap 6), tips on the lid
  const lid = a.parts.find((p) => p.id === "lid");
  assert.deepStrictEqual(shield.seated.rot, [180, 0, 0], "shield flips like the lid");
  assert.ok(Math.abs(shield.seated.pos[2] - lid.seated.pos[2] - 8.4) < 0.01, "posts stand on the lid top");
  // its own four screws sit flush in the shield's top and drive down into the lid
  const ss = a.parts.find((p) => p.id === "shield_screws");
  for (const [, , z] of ss.instances) assert.ok(Math.abs(z - shield.seated.pos[2]) < 0.01, "shield screws flush in its top");
  assert.ok(ss.step >= shield.step, "the shield's screws drive once it is on");
});
