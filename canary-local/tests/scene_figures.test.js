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

test("the display cards the manifests home cases on draw their own hardware", async () => {
  // registry.json carries a card for every display manifest that claims a
  // case (tests/chooser.test.js): the C6 draws its own pocket case, and the
  // Nightstand 7 — one board, one case, two products — the Dash 7's slab,
  // resolved through the figure's `manifests` (the manifest names the figure;
  // the figure's own id and device are the Dash 7's)
  const { FIGURE_BUILDERS } = await load();
  const { deviceFigure } = await import("../assets/body-dims.js");
  assert.deepStrictEqual(FIGURE_BUILDERS["canary-display-nightstand-c6"], ["device.canary-display-nightstand-c6", {}]);
  assert.deepStrictEqual(FIGURE_BUILDERS["canary-display-nightstand7"], ["device.canary-display-dash7", {}]);
  const dash7 = ledger.figures.find((f) => f.id === "device.canary-display-dash7");
  assert.deepStrictEqual(dash7.manifests, ["canary-display-dash7", "canary-display-nightstand7"]);
  assert.strictEqual(deviceFigure(ledger, "canary-display-nightstand7")?.id, "device.canary-display-dash7");
  const n7 = JSON.parse(readFileSync(join(ROOT, "../devices/canary-display-nightstand7/device.json"), "utf8"));
  assert.strictEqual(n7.figure, "device.canary-display-dash7", "the join is the manifest's own figure");
  // an id or device match still wins: the Nightlight card is the C3 manifest's figure by id
  assert.strictEqual(deviceFigure(ledger, "canary-nightlight")?.id, "device.canary-nightlight");
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

// real-shapes.js realDash seats the Dash's three STLs by hand along the case's
// face normal, from the module center outward. Its numbers were once a 16 mm
// body and an 8.4 mm back while the CAD had moved on; they are re-derived now,
// and held here to the ledger gen_assembled_dims.py measures off the same
// .scad (docs/hardware/enclosure/assembled_dims.json, device.canary-display-
// dash): depth fig.d from the dock pads to the face, seams_fig_d the pads'
// and the back's thickness, face_fig_mm the aperture. The module center sits
// total_t / 2 in front of the back plane (the fin face), so along the normal:
// the pads' tips at −total_t/2 − pads, the back centered over [tips, seam 2],
// the frame over [seam 2, face], the glass face_t behind the face. The
// center's own position on the stand is not pinned here (it is the stand's
// derivation, cited in the comment above realDash).
test("the Dash card seats its case where the CAD ledger measures it", () => {
  const REPO = join(ROOT, "..");
  const src = readFileSync(join(ROOT, "assets/real-shapes.js"), "utf8");
  const start = src.indexOf("async function realDash(");
  assert.ok(start >= 0, "real-shapes.js still has realDash");
  const body = src.slice(start, src.indexOf("\n}\n", start));
  const num = (re, what) => {
    const m = re.exec(body);
    assert.ok(m, `realDash: ${what} not found`);
    return m.slice(1).map(Number);
  };
  const [frameAt] = num(/seatPart\(scene, frame, \{[^}]*?D: off\((-?[\d.]+)\)/, "the frame's seat");
  const [backAt] = num(/seatPart\(scene, back, \{[^}]*?D: off\((-?[\d.]+)\)/, "the back's seat");
  const [glassAt] = num(/translate\(\.\.\.off\((-?[\d.]+)\)\)/, "the glass's seat");
  const [glassW, glassH] = num(/screenPlane\(([\d.]+), ([\d.]+),/, "the glass plane");

  const asm = JSON.parse(readFileSync(join(REPO, "docs/hardware/enclosure/assembled_dims.json"), "utf8"));
  const row = asm.devices["device.canary-display-dash"];
  assert.ok(row && row.scad === "canary_dash_display.scad", "the ledger measures the Dash off its case");
  const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
  const knob = (name) => Number(enc.scads[row.scad].groups.flatMap((g) => g.params)
    .find((p) => p.name === name).default);

  const [pads, backIn] = row.seams_fig_d;          // pads | back plate | frame
  const totalT = row.fig.d - pads;                 // frame_h + back_t
  const tips = -totalT / 2 - pads;
  const close = (got, want, what) =>
    assert.ok(Math.abs(got - want) < 0.01, `realDash ${what}: off(${got}), the ledger says ${want.toFixed(2)}`);
  close(backAt, tips + backIn / 2, "back");
  close(frameAt, tips + (backIn + row.fig.d) / 2, "frame");
  close(glassAt, totalT / 2 - knob("face_t"), "glass");
  close(glassW, row.face_fig_mm.w, "glass width");
  close(glassH, row.face_fig_mm.h, "glass height");
});

// ── the GPU lifecycle ───────────────────────────────────────────────────
// fleet.html mounts a DeviceScene per registry device (plus the open sheet).
// Each used to own a WebGL context; Chromium keeps ~16 alive and loses the
// oldest, so the first cards went blank — and every device added made it
// worse. scene3d.js renders every scene through one shared context and gates
// each card's loop on visibility; tests/scene_lifecycle_probe.mjs mounts
// forty in a real browser and proves it. This gate holds the probe in CI
// beside the render probe, and the module to the shape the probe relies on.
test("CI runs the render probe and the scene lifecycle probe", () => {
  const workflow = readFileSync(join(ROOT, "../.github/workflows/canary-local.yml"), "utf8");
  assert.ok(workflow.includes("node canary-local/tests/render_probe.mjs"),
    "render_probe.mjs is not wired into canary-local.yml");
  assert.ok(workflow.includes("node canary-local/tests/scene_lifecycle_probe.mjs"),
    "scene_lifecycle_probe.mjs is not wired into canary-local.yml");
  assert.ok(existsSync(join(ROOT, "tests/scene_lifecycle_probe.mjs")));
});

test("a scene retains its part data and shares the page's one context", async () => {
  const src = readFileSync(join(ROOT, "assets/scene3d.js"), "utf8");
  // one getContext("webgl") on the page, on a detached canvas — never on the card
  const ctxCalls = src.match(/getContext\("webgl"/g) || [];
  assert.strictEqual(ctxCalls.length, 1, "every DeviceScene must draw through the shared context");
  for (const hook of ["webglcontextlost", "webglcontextrestored", "IntersectionObserver",
                      "visibilitychange", "_acquireGL()", "_releaseGL()", "static stats()"])
    assert.ok(src.includes(hook), `scene3d.js lost its ${hook} lifecycle hook`);
  // the registry's card count is exactly why: past the browser's cap
  assert.ok(registry.devices.length > 16,
    `${registry.devices.length} registry devices — the fleet page is past a browser's ~16 live contexts`);
});

// ── A47: the Lab's 3D display turns with its glass ──────────────────────
// The emulator's canvas turns with the glass (F184: a portrait dash is
// 480x800; F204: a landscape nightlight 320x180), and the Lab textures its 3D
// screen from that canvas (app.js: ctx.scene.src = glass). Textured as is, a
// turned canvas stretched across the model's unturned screen. The choice
// held here: the device's body turns a quarter turn clockwise when the
// canvas is the panel's own shape (the registry card's glass, which app.js
// hands the scene) exactly turned on its side, and the plane samples the
// canvas turned the other way, so the glass reads upright on the turned
// body. A glass that never turns never turns its model, and neither does a
// canvas of any other shape (the 300x150 HTML default the glass canvas is
// before the firmware's first frame). What turns is the body alone, seated
// face-on (partModels; real_shapes.test.js holds the Dash's stand left out).
test("A47: a turned glass turns the model, and reads upright on it", async () => {
  const { glassTurn, turnModel, turnTexture, screenPlane } = await load();
  // The decision: only a canvas that is its panel exactly turned on its side
  // turns the model.
  assert.strictEqual(glassTurn({ width: 480, height: 800 }, { w: 800, h: 480 }), 1, "the dash worn portrait");
  assert.strictEqual(glassTurn({ width: 320, height: 180 }, { w: 180, h: 320 }), 1, "the nightlight stood on its edge");
  assert.strictEqual(glassTurn({ width: 800, height: 480 }, { w: 800, h: 480 }), 0, "the dash as it scans");
  assert.strictEqual(glassTurn({ width: 180, height: 320 }, { w: 180, h: 320 }), 0);
  assert.strictEqual(glassTurn({ width: 240, height: 240 }, { w: 240, h: 240 }), 0, "a round glass never turns");
  assert.strictEqual(glassTurn(null, { w: 800, h: 480 }), 0, "no live glass, no turn");
  assert.strictEqual(glassTurn({ width: 480, height: 800 }, null), 0, "no panel shape, no turn");
  assert.strictEqual(glassTurn({ width: 480, height: 640 }, { w: 800, h: 480 }), 0, "a canvas of neither shape turns nothing");
  // Before the firmware's first frame the glass canvas is the HTML default
  // 300x150 (emu-shell.js sizes it in _displayReady), or 0x0: no twin's
  // model turns on it — a portrait twin used to lie on its side until then.
  let twins = 0;
  for (const d of registry.devices) {
    if (d.kind !== "display" || !d.glass) continue;
    for (const c of [{ width: 300, height: 150 }, { width: 0, height: 0 }, { width: 150, height: 300 }]) {
      assert.strictEqual(glassTurn(c, d.glass), 0, `${d.id} (${d.glass.w}x${d.glass.h}): a ${c.width}x${c.height} canvas turns nothing`);
    }
    twins++;
  }
  assert.ok(twins >= 5, `every display card's glass was checked (${twins})`);
  // The look: where each corner of the canvas lands in the viewer's frame,
  // through the texture turnTexture() draws, the UVs screenPlane() gives the
  // plane and the model turn turnModel() applies (+x right, +y up).
  const landing = (turn, cw, ch, plane) => {
    const m = screenPlane(plane.w, plane.h, false);
    const at = (u, v) => {
      const i = [0, 1, 2, 3].find((k) => m.uv[2 * k] === u && m.uv[2 * k + 1] === v);
      return [m.pos[3 * i], m.pos[3 * i + 1]];
    };
    const [x00, y00] = at(0, 0), [x10] = at(1, 0), [, y01] = at(0, 1);
    const t = turnTexture(turn, cw, ch);
    const [a, b, c, d, e, f] = t.m;
    const M = turnModel(turn);
    return (px, py) => {
      const X = a * px + c * py + e, Y = b * px + d * py + f;      // canvas -> texture
      const u = X / t.w, v = Y / t.h;                               // texture -> uv
      const x = x00 + u * (x10 - x00), y = y00 + v * (y01 - y00);   // uv -> plane
      return [M[0] * x + M[4] * y, M[1] * x + M[5] * y];            // plane -> view
    };
  };
  const upright = (land, cw, ch, what) => {
    const [tl, tr, bl] = [land(0, 0), land(cw, 0), land(0, ch)];
    assert.ok(tl[0] < tr[0] && Math.abs(tl[1] - tr[1]) < 1e-6, `${what}: the canvas's top edge runs left to right`);
    assert.ok(bl[1] < tl[1] && Math.abs(tl[0] - bl[0]) < 1e-6, `${what}: the canvas's left edge runs top to bottom`);
    const vw = tr[0] - tl[0], vh = tl[1] - bl[1];
    assert.ok(Math.abs(vw / vh - cw / ch) < 1e-6, `${what}: the glass keeps its shape on the model (${vw}x${vh})`);
  };
  // The dash's own screen plane, 800:480 like its panel, and the
  // nightlight's, 180:320 like its panel.
  upright(landing(1, 480, 800, { w: 160, h: 96 }), 480, 800, "the portrait dash on its turned model");
  upright(landing(1, 320, 180, { w: 22.5, h: 40 }), 320, 180, "the landscape nightlight on its turned model");
  upright(landing(0, 800, 480, { w: 160, h: 96 }), 800, 480, "the unturned dash");
  // Without the turn, the portrait canvas lands stretched on the landscape
  // screen (what the Lab showed before): the shape check catches it.
  assert.throws(() => upright(landing(0, 480, 800, { w: 160, h: 96 }), 480, 800, "unturned"), /keeps its shape/);
  // And a texture turned the wrong way lands the glass upside down.
  const wrong = (cw, ch, plane) => {
    const good = landing(1, cw, ch, plane);
    return (px, py) => good(cw - px, ch - py);
  };
  assert.throws(() => upright(wrong(480, 800, { w: 160, h: 96 }), 480, 800, "flipped"), /left to right|top to bottom/);
});

test("A47: a figure turns where it stands, and its shadow follows it", async () => {
  const { buildFromFigure, partModels, turnModel, turnedShadow, M4 } = await load();
  const scene = fakeScene();
  await buildFromFigure("device.canary-display-dash")(scene);
  const pose = scene.turnPose;
  assert.ok(pose, "a figure names its body's pose");
  assert.deepStrictEqual(Array.from(pose.seat), Array.from(M4.ident()), "a figure is placed centered and face-on already");
  const [w, h] = pose.size;
  assert.ok(w > h, `the dash figure is landscape face-on (${pose.size.join(" x ")})`);
  // Unturned, every part draws as built; turned, every part (a figure has no
  // stand) turns about the viewer's axis.
  const flat = (m) => Array.from(m).map((v) => +v.toFixed(6) || 0);
  for (const { part, model } of partModels(scene.parts, 0, pose)) assert.strictEqual(model, part.model);
  const turned = partModels(scene.parts, 1, pose);
  assert.strictEqual(turned.length, scene.parts.length, "nothing of a figure is left out");
  for (const { part, model } of turned) {
    assert.deepStrictEqual(flat(model), flat(M4.mul(turnModel(1), part.model ?? M4.ident())));
  }
  // The shadow: unturned the figure's own; turned, under a body h wide and w
  // tall — the old shadow, left where it was, sat (w - h) / 2 up the turned
  // body.
  assert.strictEqual(turnedShadow(scene.shadow, 0, pose), scene.shadow);
  const sh = turnedShadow(scene.shadow, 1, pose);
  assert.ok(Math.abs(sh.y - (-w / 2 - 1.5)) < 1e-9, `the turned shadow stands under the body's foot (${sh.y})`);
  assert.ok(scene.shadow.y > -w / 2, "the figure's own shadow would cross the turned body");
  assert.strictEqual(sh.alpha, scene.shadow.alpha);
});

test("A47: the scene is handed the panel's own shape, which the firmware's panel is", async () => {
  const { glassTurn } = await load();
  const { flavorBoard, pinsPanel, turnedGlasses, readTurnedSources } = await import("./turned_glass.mjs");
  const fsp = require("node:fs/promises");
  const REPO = join(ROOT, "..");
  const buildSh = readFileSync(join(ROOT, "emulator/build.sh"), "utf8");
  const panel = (flavor) => {
    const pins = readFileSync(join(REPO, "firmware/boards", flavorBoard(buildSh, flavor), "pins/pins.h"), "utf8");
    const p = pinsPanel(pins);
    if (p) return p;
    return { w: Number(/^#define TFT_WIDTH\s+(\d+)/m.exec(pins)?.[1]), h: Number(/^#define TFT_HEIGHT\s+(\d+)/m.exec(pins)?.[1]) };
  };
  const turned = turnedGlasses(await readTurnedSources(REPO, fsp.readFile));
  let checked = 0;
  for (const d of registry.devices) {
    if (d.kind !== "display" || !d.emulator) continue;
    const flavor = /canary-display-([a-z0-9]+)\.js$/.exec(d.emulator.module)[1];
    // The card's glass, which app.js hands the scene, is the panel build.sh
    // compiles the twin against: the shape the canvas turns from.
    assert.deepStrictEqual({ w: d.glass.w, h: d.glass.h }, panel(flavor), `${d.id}: its card's glass is the ${flavor} panel`);
    assert.strictEqual(glassTurn({ width: d.glass.w, height: d.glass.h }, d.glass), 0, `${d.id}: its own glass turns nothing`);
    for (const t of turned.filter((x) => x.flavor === flavor)) {
      assert.strictEqual(glassTurn({ width: t.glass.w, height: t.glass.h }, d.glass), 1,
        `${d.id}: the ${t.name} ${t.glass.w}x${t.glass.h} glass turns the model`);
    }
    checked++;
  }
  assert.ok(checked >= 5, `the display twins were checked (${checked})`);
  // The render turns the texture and the body with the canvas, every frame,
  // and only when the build named its body's pose; the shadow follows the
  // turned body. project() is the orbit alone: its callers hand it world
  // points with any part transform applied (scene3d.js), and a turn is a
  // part transform now.
  const scene3d = readFileSync(join(ROOT, "assets/scene3d.js"), "utf8");
  assert.ok(scene3d.includes("this.turn = this.turnPose ? glassTurn(this.src, this.glass) : 0;") &&
    scene3d.includes("this.turn ? this._turnedSource() : this.src);") &&
    scene3d.includes("const shadow = turnedShadow(this.shadow, this.turn, this.turnPose);") &&
    scene3d.includes("for (const { part: p, model } of partModels(this.parts, this.turn, this.turnPose)) {") &&
    scene3d.includes("gl.uniformMatrix4fv(u.uModel, false, M4.mul(spin, model));"),
    "the draw turns the texture, the body and the shadow through glassTurn/partModels/turnedShadow");
  assert.strictEqual((scene3d.match(/const spin = M4\.mul\(M4\.rotX\(this\.rot\.x\), M4\.rotY\(this\.rot\.y\)\);/g) || []).length, 2,
    "the draw's and project()'s spin are the orbit alone");
  assert.ok(/clearParts\(\) \{[^}]*this\.turnPose = null;/.test(scene3d), "every build names its own body's pose");
  assert.ok(scene3d.includes("const t = turnTexture(this.turn, this.src.width, this.src.height);") &&
    scene3d.includes("g.setTransform(...t.m);"), "the turned plane samples the canvas through turnTexture()");
  // The Lab hands the scene the live canvas and the panel's own shape.
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  assert.ok(/ctx\.scene\.src = glass;\n\s*ctx\.scene\.glass = \{ w: dev\.glass\.w, h: dev\.glass\.h \};/.test(app),
    "app.js textures the 3D screen from the live glass and names its panel");
});
