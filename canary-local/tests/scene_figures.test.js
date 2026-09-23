// canary-local/tests/scene_figures.test.js — every Lab card draws its own device.
//
// The display line's cards used to be hand meshes: a Watch typed from the
// v0.1 CAD, a Dash typed with the wrong box, three nightstand boards
// borrowing the Dash mesh and two more — plus every concept — falling
// through to the WAP's witness body (app.js: `BUILDERS[id] || BUILDERS
// ["canary-wap"]`). scene3d.js now routes the display line through the
// committed fleet-figure GLBs (tools/figures/gen_device_glbs.mjs) and
// resolves anything else through the figure ledger. These tests hold that:
//   · every registry device resolves to its own builder or its own figure,
//     and no card borrows another device's body;
//   · every figure-routed display names the figure deviceFigure() resolves,
//     and its GLB is committed, with materials the Lab knows how to paint
//     (a printed shell that takes the finish, a lit face that becomes the
//     live glass);
//   · the builders run: a figure lands centered with a screen plane, an idea
//     stands in as a ghost (edges, no fill), a build superseded mid-flight
//     (a real-shape upgrade) adds nothing.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");
const { fileURLToPath } = require("node:url");

const ROOT = join(__dirname, "..");
const registry = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
const ledger = JSON.parse(readFileSync(join(ROOT, "devices/figures.json"), "utf8"));

// fetch over the working tree: the module resolves its URLs against itself
// (file:// under node), exactly the files a served page would fetch
globalThis.fetch = async (url) => {
  const path = fileURLToPath(url);
  if (!existsSync(path)) return { ok: false, status: 404 };
  const bytes = readFileSync(path);
  return {
    ok: true,
    status: 200,
    arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    json: async () => JSON.parse(bytes.toString("utf8")),
  };
};

function fakeScene() {
  return {
    buildGen: 0,
    parts: [],
    dist: 0,
    shadow: null,
    clearParts() { this.buildGen++; this.parts = []; },
    addMesh(builder, opts = {}) { const p = { builder, ...opts }; this.parts.push(p); return p; },
    setContactShadow(s) { this.shadow = s; },
  };
}

const load = () => import("../assets/scene3d.js");

test("every registry device resolves to its own builder or its own figure", async () => {
  const { BUILDERS, FIGURE_BUILDERS, buildWap, buildVision, buildSense } = await load();
  const { deviceFigure } = await import("../assets/body-dims.js");
  const witnessBodies = new Set([buildWap, buildVision, buildSense]);
  for (const d of registry.devices) {
    const own = BUILDERS[d.id];
    const fig = deviceFigure(ledger, d.id);
    assert.ok(own || fig, `${d.id}: no builder and no fleet figure — its card would be empty`);
    if (d.kind === "display") {
      assert.ok(FIGURE_BUILDERS[d.id], `${d.id} is a display: it reads its fleet figure`);
      assert.ok(!witnessBodies.has(own), `${d.id} is a display drawn as a witness body`);
    }
    if (own && witnessBodies.has(own)) {
      assert.strictEqual(own, BUILDERS[d.id], `${d.id} has its own witness body`);
      assert.ok(["canary-wap", "canary-vision", "canary-sense"].includes(d.id),
        `${d.id} borrows a witness body that is not its own`);
    }
  }
});

test("every figure-routed display is the figure the ledger resolves, with a committed model", async () => {
  const { FIGURE_BUILDERS, FIGURE_PAINT, SCREEN_MATERIAL } = await load();
  const { deviceFigure } = await import("../assets/body-dims.js");
  const { parseGLB } = await import("../assets/glb.js");
  const byId = new Map(registry.devices.map((d) => [d.id, d]));
  for (const [id, [figId, opts]] of Object.entries(FIGURE_BUILDERS)) {
    const dev = byId.get(id);
    assert.ok(dev, `${id} is a registry device`);
    assert.strictEqual(deviceFigure(ledger, id)?.id, figId, `${id} draws the figure its manifest names`);
    const fig = ledger.figures.find((f) => f.id === figId);
    assert.notStrictEqual(fig.confidence, "idea", `${figId} is built, so it has a solid model`);
    assert.strictEqual(!!opts.round, !!dev.glass?.round, `${id}: the live glass is round exactly when the panel is`);
    const path = join(ROOT, "models", `${figId}.glb`);
    assert.ok(existsSync(path), `${figId}.glb is committed (gen_device_glbs.mjs)`);
    const model = parseGLB(readFileSync(path));
    const names = new Set(model.parts.map((p) => p.name));
    for (const n of names) {
      assert.ok(n === SCREEN_MATERIAL || FIGURE_PAINT[n], `${figId}: the Lab has no paint for material "${n}"`);
    }
    assert.ok(names.has(SCREEN_MATERIAL), `${figId} has a lit face for the live glass`);
    assert.ok([...names].some((n) => FIGURE_PAINT[n]?.role), `${figId} has a printed part that takes the finish`);
  }
});

test("a figure builder lands the model centered, with the live glass on its face", async () => {
  const { buildFromFigure } = await load();
  const scene = fakeScene();
  await buildFromFigure("device.canary-display-watch", { round: true })(scene);
  const screens = scene.parts.filter((p) => p.screen);
  assert.strictEqual(screens.length, 1, "one live-glass plane");
  assert.ok(scene.parts.some((p) => p.role === "shell"), "the bezel takes the finish");
  assert.ok(scene.parts.some((p) => p.role === "shell2"), "the drum takes the secondary finish");
  const watch = ledger.figures.find((f) => f.id === "device.canary-display-watch");
  // the screen plane sits on the front face: at +d/2 of the centered model or just proud of it
  const z = screens[0].model[14];
  assert.ok(z > watch.envelope_mm.d / 2 - 0.5 && z < watch.envelope_mm.d / 2 + 2, `glass at z ${z}`);
  assert.ok(scene.dist > 0 && scene.shadow, "the camera and the contact shadow are framed to the model");
});

test("a build superseded mid-flight adds nothing", async () => {
  const { buildFromFigure } = await load();
  const scene = fakeScene();
  const pending = buildFromFigure("device.canary-display-dash")(scene);
  scene.clearParts();          // what a real-shape upgrade does when it lands first
  scene.addMesh({}, { role: "real" });
  await pending;
  assert.deepStrictEqual(scene.parts.map((p) => p.role), ["real"]);
});

test("an idea stands in as a ghost, and a figure with no model as its envelope", async () => {
  const { builderFor, BUILDERS } = await load();
  const idea = registry.devices.find((d) => d.kind === "concept" && !BUILDERS[d.id]);
  assert.ok(idea, "some concept has no dedicated body");
  const scene = fakeScene();
  await builderFor(idea.id)(scene);
  assert.ok(scene.parts.length > 0, `${idea.id} draws something`);
  assert.ok(scene.parts.every((p) => p.lines && p.unlit), `${idea.id} is edges only — no fill for an idea`);

  const warn = console.warn;
  const warned = [];
  console.warn = (m) => warned.push(m);
  try {
    const noModel = registry.devices.find((d) => {
      const f = ledger.figures.find((x) => x.id === `device.${d.id}`);
      return f && f.confidence !== "idea" && !BUILDERS[d.id]
        && !existsSync(join(ROOT, "models", `${f.id}.glb`));
    });
    if (noModel) {
      const s2 = fakeScene();
      await builderFor(noModel.id)(s2);
      assert.strictEqual(s2.parts.length, 1, `${noModel.id} stands in with one envelope slab`);
      assert.ok(!s2.parts[0].lines, "a built figure's stand-in is solid, not a ghost");
      assert.ok(warned.some((m) => m.includes(noModel.id)), "and the console says why");
    }
  } finally {
    console.warn = warn;
  }
});

test("an idea never has a body of its own: every concept card is its figure's ghost", async () => {
  // The honesty invariant, on the Lab's 3D tier: an idea renders as a dashed
  // ghost everywhere (docs/design/FLEET_FIGURES.md). The Fence Guard kept a
  // solid hand-modeled slab — solar lid, antenna, clamp, LED — in BUILDERS
  // while its own figure was a ghost, so the one card on the page that looked
  // like a product you could buy was an idea. No builder for an idea, then:
  // builderFor() falls through to the ledger, and the ledger says ghost.
  const { BUILDERS, builderFor } = await load();
  const { deviceFigure } = await import("../assets/body-dims.js");
  const ideas = registry.devices.filter((d) => d.kind === "concept"
    || deviceFigure(ledger, d.id)?.confidence === "idea");
  assert.ok(ideas.some((d) => d.id === "canary-fence-guard"), "the Fence Guard is an idea");
  for (const d of ideas) {
    assert.strictEqual(BUILDERS[d.id], undefined,
      `${d.id} is an idea with a body of its own — draw it as its figure's ghost`);
    const scene = fakeScene();
    await builderFor(d.id)(scene);
    assert.ok(scene.parts.length > 0, `${d.id} draws its ghost`);
    assert.ok(scene.parts.every((p) => p.lines && p.unlit), `${d.id} is edges only — no fill for an idea`);
  }
});

test("no card asks for a file that is not there (the Lab's probes fail a page on any 4xx)", async () => {
  const { builderFor } = await load();
  const real = globalThis.fetch;
  const missing = [];
  globalThis.fetch = async (url) => {
    const r = await real(url);
    if (!r.ok) missing.push(String(url));
    return r;
  };
  const warn = console.warn;
  console.warn = () => {};
  try {
    for (const d of registry.devices) {
      const scene = fakeScene();
      await builderFor(d.id)(scene);
    }
  } finally {
    globalThis.fetch = real;
    console.warn = warn;
  }
  assert.deepStrictEqual(missing, []);
});
