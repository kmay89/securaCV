// canary-local/tests/real_shapes.test.js — the cards' real printed geometry
// is on disk, and the Watch sits where the CAD ledger says.
//
// real-shapes.js swaps a card's massing for the actual STLs: a hand-typed
// device → file table (REAL) whose loads are fetch()es with the failure
// swallowed (`.catch(() => false)` — offline copies keep the figure). So a
// renamed or deleted STL fell back to the procedural body on every card,
// with every gate green. And the Watch's seat in its stand was typed from
// the scad's echo (3.03, 27.36, 65°, 10.5, 20.1, 20.5, Ø37.5) while the
// Dash's was held to docs/hardware/enclosure/assembled_dims.json
// (scene_figures.test.js). These tests hold both:
//   · every file real-shapes.js names resolves under the base it names
//     (enclosures/preview/ for the display line, docs/hardware/enclosure/
//     for the print-validated witnesses), and every card that has an entry
//     in REAL upgrades — no fetch on any card returns a 4xx;
//   · the Watch card's numbers equal the ledger the way the Dash's do: the
//     stand seat is the row's seat_scad (gen_assembled_dims.py reads the
//     scad's own P0 / slope echo), the drum and bezel centers and the glass
//     plane come from its seam, envelope and face, and the one knob
//     (`flush`) from enclosures.json.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join, resolve } = require("node:path");
const { fileURLToPath } = require("node:url");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const registry = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
const SRC = readFileSync(join(ROOT, "assets/real-shapes.js"), "utf8");

// real-shapes.js fetches page-relative paths ("enclosures/preview/x.stl",
// "../docs/hardware/enclosure/x.stl"): the Lab's pages live in canary-local/,
// so resolve against it — exactly the files a served page would fetch.
function toPath(url) {
  const s = String(url);
  return s.startsWith("file:") ? fileURLToPath(s) : resolve(ROOT, s);
}
function shimFetch(log) {
  return async (url) => {
    const path = toPath(url);
    const ok = existsSync(path);
    log.push({ url: String(url), path, ok });
    if (!ok) return { ok: false, status: 404 };
    const bytes = readFileSync(path);
    return {
      ok: true,
      status: 200,
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    };
  };
}
function fakeScene() {
  return {
    buildGen: 0, parts: [], dist: 0, turnPose: null,
    clearParts() { this.buildGen++; this.parts = []; this.turnPose = null; },
    addMesh(builder, opts = {}) { const p = { builder, ...opts }; this.parts.push(p); return p; },
  };
}

test("every STL real-shapes.js names is on disk under the base it names", () => {
  // the literal scan: every "x.stl" in the module, resolved under the base
  // the surrounding call names (load(file) → PREVIEW; load(file, ENC) and
  // realTwoPart(...) → ENC) — so a file behind a branch no card exercises
  // is still checked
  const bases = {};
  for (const m of SRC.matchAll(/^const (PREVIEW|ENC) = "([^"]+)";/gm)) bases[m[1]] = m[2];
  assert.deepStrictEqual(Object.keys(bases).sort(), ["ENC", "PREVIEW"], "the two bases are still named");
  const files = [];
  for (const m of SRC.matchAll(/load\("([a-z0-9_]+\.stl)"(?:,\s*(ENC|PREVIEW))?\)/g)) {
    files.push({ file: m[1], base: m[2] || "PREVIEW" });
  }
  for (const m of SRC.matchAll(/realTwoPart\(scene,\s*"([a-z0-9_]+\.stl)",\s*"([a-z0-9_]+\.stl)"/g)) {
    files.push({ file: m[1], base: "ENC" }, { file: m[2], base: "ENC" });
  }
  assert.ok(files.length >= 6, `found ${files.length} STL references (the two displays alone name six)`);
  const missing = files
    .map(({ file, base }) => resolve(ROOT, bases[base] + file))
    .filter((p) => !existsSync(p));
  assert.deepStrictEqual(missing, [], "every named STL exists");
});

test("every card with real geometry upgrades, and no card's fetch comes back 4xx", async () => {
  const { upgradeRealShape } = await import("../assets/real-shapes.js");
  const real = globalThis.fetch;
  const log = [];
  globalThis.fetch = shimFetch(log);
  const upgraded = [];
  try {
    for (const d of registry.devices) {
      const before = log.length;
      const scene = fakeScene();
      const ok = await upgradeRealShape(scene, d.id);
      const fetched = log.slice(before);
      if (fetched.length) {
        // an entry in REAL: its loads must all land, and the card must be
        // rebuilt from them (the swallowed failure is what this catches)
        assert.ok(ok, `${d.id}: has real geometry but the upgrade failed — `
          + fetched.filter((f) => !f.ok).map((f) => f.url).join(", "));
        assert.ok(scene.parts.length >= 2, `${d.id}: the real card has its parts`);
        upgraded.push(d.id);
      } else {
        assert.strictEqual(ok, false, `${d.id}: no REAL entry, nothing fetched — kept as is`);
      }
    }
  } finally {
    globalThis.fetch = real;
  }
  assert.deepStrictEqual(log.filter((f) => !f.ok), [], "no fetch returned 404");
  assert.ok(upgraded.includes("canary-display-watch") && upgraded.includes("canary-display-dash"),
    `the two displays are real-shape cards (upgraded: ${upgraded.join(", ")})`);
});

// The Watch counterpart of scene_figures.test.js's Dash pin. Along the pocket
// axis a (from the drum's back cap): the drum is centered at drum_h / 2 (the
// ledger's seam), the bezel at fig.d − its own print height / 2 (its face caps
// the puck), and the glass — the disc's front ring, `flush` below the drum
// rim — at drum_h − flush, drawn at the bezel aperture (face_fig_mm). The
// seat itself is the ledger's seat_scad: pos in the stand's frame (typed here
// stand-centered, as `X − cS[1]`), and the recline rot[0] as the axis angle.
test("the Watch card seats its drum where the CAD ledger measures it", async () => {
  const start = SRC.indexOf("async function realWatch(");
  assert.ok(start >= 0, "real-shapes.js still has realWatch");
  const body = SRC.slice(start, SRC.indexOf("\n}\n", start));
  const num = (re, what) => {
    const m = re.exec(body);
    assert.ok(m, `realWatch: ${what} not found`);
    return m.slice(1).map(Number);
  };
  const [angle] = num(/const A = (-?[\d.]+) \* Math\.PI \/ 180/, "the pocket axis angle");
  const [seatY, seatZ] = num(/const p0 = \[0, (-?[\d.]+) - cS\[1\], (-?[\d.]+) - cS\[2\]\]/, "the seat");
  const [drumAt] = num(/seatPart\(scene, drum, \{[^}]*?D: along\((-?[\d.]+)\)/, "the drum's seat");
  const [bezelAt] = num(/seatPart\(scene, bezel, \{[^}]*?D: along\((-?[\d.]+)\)/, "the bezel's seat");
  const [glassAt] = num(/translate\(\.\.\.along\((-?[\d.]+)\)\)/, "the glass's seat");
  const [glassW, glassH] = num(/screenPlane\(([\d.]+), ([\d.]+),/, "the glass plane");

  const asm = JSON.parse(readFileSync(join(REPO, "docs/hardware/enclosure/assembled_dims.json"), "utf8"));
  const row = asm.devices["device.canary-display-watch"];
  assert.ok(row && row.scad === "canary_watch_station.scad", "the ledger measures the Watch off its case");
  assert.ok(row.seat_scad && row.seat_scad.part === "stand", "the ledger records the stand seat");
  const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
  const knob = (name) => Number(enc.scads[row.scad].groups.flatMap((g) => g.params)
    .find((p) => p.name === name).default);
  const { parseSTL } = await import("../assets/stl.js");
  const bezel = parseSTL(readFileSync(join(ROOT, "enclosures/preview/canary_watch_station_bezel.stl")).buffer);

  const [drumH] = row.seams_fig_d;                 // drum | bezel
  const close = (got, want, what) =>
    assert.ok(Math.abs(got - want) < 0.01, `realWatch ${what}: ${got}, the ledger says ${want.toFixed(3)}`);
  assert.deepStrictEqual(row.seat_scad.rot.slice(1), [0, 0], "the seat rotates about x only");
  close(angle, row.seat_scad.rot[0], "pocket axis angle");
  close(seatY, row.seat_scad.pos[1], "seat y");
  close(seatZ, row.seat_scad.pos[2], "seat z");
  close(row.seat_scad.pos[0], 0, "seat x (on the stand's centerline, which the card assumes)");
  close(drumAt, drumH / 2, "drum center");
  close(bezelAt, row.fig.d - bezel.bbox.size[2] / 2, "bezel center");
  close(glassAt, drumH - knob("flush"), "glass plane");
  close(glassW, row.face_fig_mm.w, "glass width");
  close(glassH, row.face_fig_mm.h, "glass height");
});

// A47: a glass worn portrait turns the Dash's case and leaves its stand. The
// sheet shows the real shape (app.js runs upgradeRealShape after the figure),
// and the desk stand is cut for the landscape case — the plug's channel under
// the USB-C in its bottom wall, the screw lobes on the pedestal
// (canary_dash_display.scad) — so the turned case stands alone, face-on,
// over a shadow sized for it. Turning the whole scene stood the stand on its
// side and leaned the case sideways (the review of #<W14>).
test("A47: a turned glass turns the Dash's case and leaves its stand", async () => {
  const { upgradeRealShape } = await import("../assets/real-shapes.js");
  const { partModels, turnModel, turnedShadow, M4 } = await import("../assets/scene3d.js");
  const real = globalThis.fetch;
  globalThis.fetch = shimFetch([]);
  const scene = fakeScene();
  try {
    assert.ok(await upgradeRealShape(scene, "canary-display-dash"), "the Dash card upgrades");
  } finally {
    globalThis.fetch = real;
  }
  const { parseSTL } = await import("../assets/stl.js");
  const stl = (f) => parseSTL(readFileSync(join(ROOT, "enclosures/preview", f)).buffer);
  const frame = stl("canary_dash_display_frame.stl"), back = stl("canary_dash_display_back.stl");
  const stands = scene.parts.filter((p) => p.stand);
  assert.strictEqual(stands.length, 1, "the stand is the one part that holds the case");
  assert.strictEqual(stands[0].builder.pos.length, stl("canary_dash_display_stand.stl").mesh.pos.length,
    "the part marked as the stand is the stand's mesh");
  const pose = scene.turnPose;
  assert.ok(pose, "the real Dash names its case's pose");
  assert.deepStrictEqual(pose.size.map((v) => +v.toFixed(3)),
    [frame.bbox.size[0], frame.bbox.size[1], frame.bbox.size[2] + back.bbox.size[2]].map((v) => +v.toFixed(3)),
    "the case's face-on size is the frame's outline and the case's depth");

  // Unturned: every part as built, the stand among them.
  const flat = (m) => Array.from(m).map((v) => +v.toFixed(4) || 0);
  const still = partModels(scene.parts, 0, pose);
  assert.strictEqual(still.length, scene.parts.length);
  for (const { part, model } of still) assert.strictEqual(model, part.model);

  // Turned: the stand is left out, and only the case turns.
  const turned = partModels(scene.parts, 1, pose);
  assert.strictEqual(turned.length, scene.parts.length - 1, "the stand does not turn with the case");
  assert.ok(turned.every(({ part }) => !part.stand));
  // The glass: face-on at the case's center, 7.4 (the ledger's glass plane)
  // in front of it, turned a quarter turn clockwise — exactly the figure's
  // turned screen, so scene_figures.test.js's corner arithmetic holds here.
  const glass = turned.find(({ part }) => part.screen);
  assert.deepStrictEqual(flat(glass.model), flat(M4.mul(turnModel(1), M4.translate(0, 0, 7.4))),
    "the turned glass faces the viewer, upright for the turned texture");
  // The case's body, every vertex through its turned model: h wide and w
  // tall, centered on the origin — standing, not leaning.
  const [w, h] = pose.size;
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (const { part, model } of turned) {
    if (part.screen) continue;
    const pos = part.builder.pos;
    for (let i = 0; i < pos.length; i += 3) {
      for (let k = 0; k < 3; k++) {
        const v = model[k] * pos[i] + model[4 + k] * pos[i + 1] + model[8 + k] * pos[i + 2] + model[12 + k];
        lo[k] = Math.min(lo[k], v); hi[k] = Math.max(hi[k], v);
      }
    }
  }
  const close = (got, want, what) => assert.ok(Math.abs(got - want) < 0.05, `${what}: ${got.toFixed(3)}, want ${want.toFixed(3)}`);
  close(hi[0] - lo[0], h, "turned width");
  close(hi[1] - lo[1], w, "turned height");
  close((hi[0] + lo[0]) / 2, 0, "centered across");
  close((hi[1] + lo[1]) / 2, 0, "centered up");
  // The shadow the turned case stands on: just under its foot.
  const sh = turnedShadow({ y: -40.5, rx: 73.2, rz: 33.7, alpha: 0.3 }, 1, pose);
  assert.ok(sh.y <= lo[1] && sh.y > lo[1] - 3, `the shadow (y ${sh.y.toFixed(2)}) is under the case's foot (${lo[1].toFixed(2)})`);
});

// A56: the case turns the way the visitor turned the glass. Its USB-C leaves
// the case's bottom wall (canary_dash_display.scad), and glass_settings.h's
// turns say where that wall goes: ROT_PORTRAIT is the glass turned clockwise
// (the cable side on the viewer's left), ROT_PORTRAIT_INV counterclockwise
// (on the right), ROT_LANDSCAPE_INV upside down (on top, the cable-up
// mount). A47 turned the case clockwise for both portrait turns, so the
// other way stood the cable side where the first turn puts it.
test("A56: the real Dash's cable side lands where the visitor's turn puts it", async () => {
  const { upgradeRealShape } = await import("../assets/real-shapes.js");
  const { partModels, M4 } = await import("../assets/scene3d.js");
  const real = globalThis.fetch;
  globalThis.fetch = shimFetch([]);
  const scene = fakeScene();
  try {
    assert.ok(await upgradeRealShape(scene, "canary-display-dash"), "the Dash card upgrades");
  } finally {
    globalThis.fetch = real;
  }
  const pose = scene.turnPose;
  const [w, h] = pose.size;
  const apply = (m, pos, i) => [0, 1, 2].map((k) => m[k] * pos[i] + m[4 + k] * pos[i + 1] + m[8 + k] * pos[i + 2] + m[12 + k]);
  // The bottom wall: every case vertex within 1 mm of the seated case's foot,
  // read face-on (the seat alone), so the wall is picked before any turn.
  const body = scene.parts.filter((p) => !p.stand && !p.screen);
  let foot = Infinity;
  for (const p of body) {
    const m = M4.mul(pose.seat, p.model);
    for (let i = 0; i < p.builder.pos.length; i += 3) foot = Math.min(foot, apply(m, p.builder.pos, i)[1]);
  }
  const wall = (turn) => {
    const at = new Map(partModels(scene.parts, turn, pose).map(({ part, model }) => [part, model]));
    const sum = [0, 0];
    let n = 0;
    for (const p of body) {
      const seated = M4.mul(pose.seat, p.model);
      for (let i = 0; i < p.builder.pos.length; i += 3) {
        if (apply(seated, p.builder.pos, i)[1] > foot + 1) continue;
        const v = apply(at.get(p), p.builder.pos, i);
        sum[0] += v[0]; sum[1] += v[1]; n++;
      }
    }
    assert.ok(n > 100, `the bottom wall has vertices (${n})`);
    return [sum[0] / n, sum[1] / n];
  };
  const [x1, y1] = wall(1), [x3, y3] = wall(3), [x2, y2] = wall(2);
  assert.ok(x1 < -0.4 * h && Math.abs(y1) < 0.2 * w, `turned clockwise, the cable side is on the left (${x1.toFixed(1)}, ${y1.toFixed(1)})`);
  assert.ok(x3 > 0.4 * h && Math.abs(y3) < 0.2 * w, `turned counterclockwise, it is on the right (${x3.toFixed(1)}, ${y3.toFixed(1)})`);
  assert.ok(y2 > 0.4 * h && Math.abs(x2) < 0.2 * w, `upside down, it is on top (${x2.toFixed(1)}, ${y2.toFixed(1)})`);
  // and the stand stays out of every turn, the case centered for each
  for (const t of [1, 2, 3]) {
    const turned = partModels(scene.parts, t, pose);
    assert.strictEqual(turned.length, scene.parts.length - 1, `turn ${t}: the stand is left out`);
  }
});
