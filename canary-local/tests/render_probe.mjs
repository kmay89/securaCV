// canary-local/tests/render_probe.mjs — CI probe for the studio renderer:
// serve the repo, stand every device from BUILDERS in front of the real
// WebGL engine, and prove three things a unit test can't:
//   1. the shaders COMPILE on an actual GPU/driver (DeviceScene throws
//      loud on compile/link failure — a bad GLSL edit fails here, not in
//      a visitor's browser),
//   2. every device draws a NON-EMPTY frame (pixels actually covered),
//   3. the frame isn't a flat blob (color variance — lighting is alive).
//   4. a turned glass reads upright on its turned model (sweep A47), on the
//      dash the Lab's sheet shows — its real printed shape, the case
//      reclined in its desk stand (app.js runs upgradeRealShape after the
//      figure): fed a 480x800 canvas (a portrait dash's glass) with three
//      marks — red and green on one row, blue under red — and the panel's
//      own 800x480 shape, the case leaves its stand (painted a probe color
//      here, so its pixels can be counted) and stands alone, the marks
//      upright at the sheet's presentation pose and, face-on, at the
//      canvas's portrait proportions, the body's foot on the shadow drawn
//      for it; with no panel named the stand is drawn and the same canvas is
//      squeezed into the landscape screen. The marks sit in the glass's
//      lower half: the studio light's highlight across the top of a face-on
//      screen washes a mark there to near white.
//   5. the case turns the way the firmware turned the glass (sweep A56): a
//      cyan tab on the case's bottom wall (the USB-C's side) lands on the
//      left for turn 1 (glass_settings.h's ROT_PORTRAIT, clockwise), on the
//      right for turn 3 (ROT_PORTRAIT_INV) and on top for turn 2
//      (ROT_LANDSCAPE_INV, an 800x480 canvas), the marks upright each way.
//
//   node canary-local/tests/render_probe.mjs [--shots DIR]
//
// With --shots it saves a PNG per device — the human check: do they look
// like product photos?
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set).
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { indexTree, lookup } from "./probe_server.mjs";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const FILES = indexTree(ROOT);
const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".json": "application/json",
  ".css": "text/css", ".glb": "model/gltf-binary",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const shotsIdx = process.argv.indexOf("--shots");
const SHOTS = shotsIdx > 0 ? process.argv[shotsIdx + 1] : null;
if (SHOTS) await (await import("node:fs/promises")).mkdir(SHOTS, { recursive: true });

const server = createServer(async (req, res) => {
  const path = req.url.split("?")[0];
  // served beside the Lab's pages: real-shapes.js fetches its STLs relative
  // to the page (enclosures/preview/), as it does on the sheet
  if (path === "/canary-local/probe.html") {
    res.writeHead(200, { "content-type": "text/html" });
    res.end(HARNESS);
    return;
  }
  try {
    // the URL never becomes a path: it is looked up in the tree's index
    // (probe_server.mjs), so the path that reaches readFile is the index's
    const file = lookup(FILES, req.url);
    if (!file) throw new Error("not in the tree");
    const body = await readFile(file);
    res.writeHead(200, { "content-type": MIME[extname(path)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404); res.end("not found");
  }
});
await new Promise((ok) => server.listen(0, "127.0.0.1", ok));
const BASE = `http://127.0.0.1:${server.address().port}`;

// The harness: one 420×420 canvas per device, real engine, real builders.
const HARNESS = `<!doctype html><meta charset="utf-8">
<body style="background:#f4f4f5;margin:0;display:flex;flex-wrap:wrap">
<script type="module">
import { DeviceScene, BUILDERS, M4, turnedShadow, roundedBox } from "/canary-local/assets/scene3d.js";
import { upgradeRealShape } from "/canary-local/assets/real-shapes.js";
import { parseSTL } from "/canary-local/assets/stl.js";
window.__probe = { status: "running", results: {} };
const frames = (n) => new Promise((ok) => {
  const step = () => (n-- <= 0 ? ok() : requestAnimationFrame(step));
  step();
});
try {
  for (const [id, build] of Object.entries(BUILDERS)) {
    const cv = document.createElement("canvas");
    cv.id = id;
    cv.style.cssText = "width:420px;height:420px";
    document.body.append(cv);
    const scene = new DeviceScene(cv, null);   // throws on shader failure
    // the display line draws its committed fleet-figure model, which loads
    // async — wait for it, or the frame is judged before the device lands
    await build(scene);
    scene.start();
    await frames(24);                          // settle: sway, shadow, glass
    scene.stop();
    scene.draw();                              // final deterministic frame
    // the scene draws into the page's ONE shared context, bottom-left, at
    // its card's size (scene3d.js "GPU lifecycle") — read exactly that rect
    const gl = scene.gl, W = cv.width, H = cv.height;
    const px = new Uint8Array(W * H * 4);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, px);
    let covered = 0, sum = 0, sumSq = 0, n = 0;
    for (let i = 0; i < px.length; i += 16) {   // sample every 4th pixel
      if (px[i + 3] > 8) {
        covered++;
        const l = (px[i] + px[i + 1] + px[i + 2]) / 3;
        sum += l; sumSq += l * l; n++;
      }
    }
    const mean = n ? sum / n : 0;
    const stddev = n ? Math.sqrt(Math.max(0, sumSq / n - mean * mean)) : 0;
    window.__probe.results[id] = {
      coveredFrac: covered / (px.length / 16),
      mean, stddev,
    };
  }
  // 4. A47: the sheet's dash — its real shape in the desk stand — fed a
  // turned glass, at the sheet's presentation pose and face-on, against the
  // same glass with no panel named.
  const glass = document.createElement("canvas");
  glass.width = 480; glass.height = 800;
  const g = glass.getContext("2d");
  g.fillStyle = "#101010"; g.fillRect(0, 0, 480, 800);
  // red and green on one row, blue under red: centers 360 px across and
  // 240 px down, in the lower half (the highlight washes the top)
  g.fillStyle = "#ff0000"; g.fillRect(0, 440, 120, 120);
  g.fillStyle = "#00ff00"; g.fillRect(360, 440, 120, 120);
  g.fillStyle = "#0000ff"; g.fillRect(0, 680, 120, 120);
  // A56: the same marks on the glass as an upside-down dash shows it, the
  // panel's own 800x480 shape: red and green on one row, blue under red
  const flat = document.createElement("canvas");
  flat.width = 800; flat.height = 480;
  const fg = flat.getContext("2d");
  fg.fillStyle = "#101010"; fg.fillRect(0, 0, 800, 480);
  fg.fillStyle = "#ff0000"; fg.fillRect(120, 300, 120, 120);
  fg.fillStyle = "#00ff00"; fg.fillRect(480, 300, 120, 120);
  fg.fillStyle = "#0000ff"; fg.fillRect(120, 180 + 240, 120, 60);
  // the stand's own mesh, to know its part by (not by the flag under test)
  const standMesh = parseSTL(await (await fetch("enclosures/preview/canary_dash_display_stand.stl")).arrayBuffer()).mesh;
  const marks = async (panel, pose, firmwareTurn = null, src = glass) => {
    const cv = document.createElement("canvas");
    cv.style.cssText = "width:420px;height:420px";
    document.body.append(cv);
    const scene = new DeviceScene(cv, src);
    await BUILDERS["canary-display-dash"](scene);
    const real = await upgradeRealShape(scene, "canary-display-dash");
    // the stand in a color nothing else here has, so its pixels can be told
    const stands = scene.parts.filter((p) => p.count === standMesh.idx.length && p.src.pos.length === standMesh.pos.length);
    for (const p of stands) { p.role = null; p.color = [1, 0, 1]; p.gloss = 0; }
    // A56: the case's bottom wall — the side its USB-C leaves by
    // (canary_dash_display.scad) — marked by a cyan tab on the bezel there,
    // seated with the case (its model the seat undone), so it turns with it
    if (scene.turnPose) {
      const [, ch, cd] = scene.turnPose.size;
      scene.addMesh(roundedBox(14, 5, 1.2, 0.3), { color: [0, 1, 1], gloss: 0,
        model: M4.mul(M4.rigidInverse(scene.turnPose.seat), M4.translate(0, -ch / 2 + 4, cd / 2 + 2)) });
    }
    scene.firmwareTurn = firmwareTurn;
    scene.glass = panel;
    scene.autoSway = false;
    if (pose === "front") { scene.rot = { x: 0, y: 0 }; scene.home = { x: 0, y: 0 }; }
    scene.draw();
    const gl = scene.gl, W = cv.width, H = cv.height;
    const px = new Uint8Array(W * H * 4);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, px);
    const c = { r: [0, 0, 0], g: [0, 0, 0], b: [0, 0, 0], cable: [0, 0, 0] };   // sum x, sum y (up), count
    let stand = 0, foot = H, top = -1, left = W, right = -1;  // rows counted up from the bottom
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        const i = (y * W + x) * 4, R = px[i], G = px[i + 1], B = px[i + 2];
        if (px[i + 3] === 255) {
          foot = Math.min(foot, y); top = Math.max(top, y);
          left = Math.min(left, x); right = Math.max(right, x);
        }
        if (R - G > 60 && B - G > 60 && Math.abs(R - B) < 40) { stand++; continue; }
        if (G - R > 60 && B - R > 60 && Math.abs(G - B) < 40) { c.cable[0] += x; c.cable[1] += y; c.cable[2]++; continue; }
        // a mark is its channel standing 40 clear of the other two (the
        // studio light adds white to every channel; the margin survives it)
        const k = R - Math.max(G, B) > 40 ? "r" : G - Math.max(R, B) > 40 ? "g"
          : B - Math.max(R, G) > 40 ? "b" : null;
        if (k) { c[k][0] += x; c[k][1] += y; c[k][2]++; }
      }
    }
    // the shadow the scene drew (view space, so no orbit): its center's row
    const sh = turnedShadow(scene.shadow, scene.turn, scene.turnPose);
    let shadowRow = null;
    if (sh) {
      const P = M4.persp(0.62, W / H, 5, 2000), vy = sh.y - scene.viewY, vz = -scene.dist;
      shadowRow = ((P[5] * vy + P[9] * vz + P[13]) / (P[7] * vy + P[11] * vz + P[15]) + 1) / 2 * H;
    }
    scene.dispose?.();
    cv.remove();
    const at = (k) => (c[k][2] ? [c[k][0] / c[k][2], c[k][1] / c[k][2]] : null);
    return { real: real && stands.length === 1, turn: scene.turn, r: at("r"), g: at("g"), b: at("b"),
             cable: at("cable"), stand, foot, top, left, right, W, shadowRow };
  };
  window.__probe.turned = await marks({ w: 800, h: 480 }, "front");
  window.__probe.unturned = await marks(null, "front");
  window.__probe.turnedHome = await marks({ w: 800, h: 480 }, "home");
  window.__probe.unturnedHome = await marks(null, "home");
  // A56: the turn the firmware announced, each way
  window.__probe.turnedCw = await marks({ w: 800, h: 480 }, "front", 1);
  window.__probe.turnedCcw = await marks({ w: 800, h: 480 }, "front", 3);
  window.__probe.upsideDown = await marks({ w: 800, h: 480 }, "front", 2, flat);
  window.__probe.status = "done";
} catch (e) {
  window.__probe.status = "error: " + (e && e.message || e);
}
</script>`;

const exe = process.env.PW_EXECUTABLE;
const browser = await pw.chromium.launch(exe ? { executablePath: exe } : {});
const page = await browser.newPage({ viewport: { width: 1300, height: 900 } });
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));
await page.goto(BASE + "/canary-local/probe.html");
await page.waitForFunction(() => window.__probe && window.__probe.status !== "running",
                           null, { timeout: 30000 });
const probe = await page.evaluate(() => window.__probe);

let failed = 0;
const fail = (msg) => { console.error("✗ " + msg); failed++; };
if (probe.status !== "done") fail("harness: " + probe.status);
for (const e of errors) fail("pageerror: " + e);

// Every device with a body of its own. The ideas are not here: they have no
// builder (scene3d.js draws each as its figure's ghost — edges only, which
// this probe's coverage floor is not written for); tests/scene_figures.test.js
// holds them to the ghost.
const EXPECT = ["canary-display-watch", "canary-display-dash", "canary-display-nightstand-s3",
                "canary-display-nightstand-c6", "canary-display-touch169", "canary-display-dash7",
                "canary-display-nightstand7", "canary-display-amoled241", "canary-nightlight",
                "canary-vision", "canary-wap", "canary-sense"];
for (const id of EXPECT) {
  const r = probe.results[id];
  if (!r) { fail(`${id}: no result`); continue; }
  if (r.coveredFrac < 0.04) fail(`${id}: frame nearly empty (covered ${(r.coveredFrac * 100).toFixed(1)}%)`);
  else if (r.stddev < 8) fail(`${id}: flat frame (stddev ${r.stddev.toFixed(1)} — lighting dead?)`);
  else console.log(`✓ ${id}: covered ${(r.coveredFrac * 100).toFixed(1)}%, tonal stddev ${r.stddev.toFixed(1)}`);
  if (SHOTS) {
    await page.locator("#" + id).screenshot({ path: join(SHOTS, id + ".png") });
  }
}

// 4. A47: on the turned case the marks stand as on the glass — red left of
// green on one row, blue under red — face-on at the canvas's proportions
// (240 down to 360 across), and at the sheet's presentation pose still
// upright (the row more across than down, the column more down than
// across); the case has left its stand, and its foot is on its shadow. The
// unturned control draws the stand and squeezes the marks into the
// landscape screen, 480/800 down and 800/480 across.
const GLASS_RATIO = 240 / 360;
const SQUEEZED = GLASS_RATIO * (480 / 800) / (800 / 480);
const shape = (m) => {
  if (!m || !m.r || !m.g || !m.b) return null;
  const across = m.g[0] - m.r[0], down = m.r[1] - m.b[1];   // readPixels y runs up
  return { across, down, ratio: down / across, row: Math.abs(m.g[1] - m.r[1]), col: Math.abs(m.b[0] - m.r[0]) };
};
const near = (v, want, tol = 0.2) => Math.abs(v / want - 1) < tol;
const held = (tag) => (what, m, check) => {
  if (!m || !m.real) return fail(`${tag} ${what}: the sheet's real dash did not load (${JSON.stringify(m)})`);
  const t = shape(m);
  const msg = check(t, m);
  if (msg) return fail(`${tag} ${what}: ${msg}`);
  const f = (v) => (typeof v === "number" ? v.toFixed(2) : v);
  console.log(`✓ ${tag} ${what} (down/across ${f(t?.ratio)}, stand pixels ${m.stand}, foot row ${m.foot}, `
    + `shadow center row ${f(m.shadowRow)}, body ${m.top - m.foot} rows, middle ${f((m.left + m.right) / 2)} of ${m.W}`
    + `${m.cable ? `, cable tab at ${m.cable.map((v) => v.toFixed(0)).join(", ")}` : ""})`);
};
const a47 = held("A47");
const turnedCase = (t, m) => {
  if (m.turn !== 1) return "the scene did not turn the case for a 480x800 glass on an 800x480 panel";
  if (m.stand !== 0) return `the stand turned with the case (${m.stand} stand pixels drawn)`;
  if (!t) return `the turned glass's marks are not all on the frame (${JSON.stringify(m)})`;
  // the shadow drawn for the turned case is under its foot (rows count up;
  // the shadow's center may sit a little below the lowest pixel, never up
  // the body)
  const h = m.top - m.foot;
  if (m.shadowRow === null || m.shadowRow > m.foot + 0.03 * h || m.shadowRow < m.foot - 0.06 * h) {
    return `the case does not stand on its shadow (foot row ${m.foot}, shadow center row ${m.shadowRow?.toFixed(1)}, body ${h} rows)`;
  }
  return null;
};
a47("face-on: a 480x800 glass reads upright on the turned case, out of its stand", probe.turned, (t, m) => {
  const bad = turnedCase(t, m);
  if (bad) return bad;
  if (!(t.across > 0 && t.down > 0 && t.row < 0.1 * t.across && t.col < 0.1 * t.down)) {
    return `the turned glass does not read upright (red ${m.r}, green ${m.g}, blue ${m.b})`;
  }
  // within 10%: the glass faces the camera square (a case left reclined in
  // the turn reads 15% off)
  if (!near(t.ratio, GLASS_RATIO, 0.1)) return `the turned glass is not at its proportions (down/across ${t.ratio.toFixed(2)}, the canvas's ${GLASS_RATIO.toFixed(2)})`;
  // seated face-on at the origin: the camera's axis runs through its middle
  const mid = (m.left + m.right) / 2;
  if (Math.abs(mid - m.W / 2) > 0.03 * m.W) return `the turned case is not standing where it was seated (its middle at ${mid}, the frame's at ${m.W / 2})`;
  return null;
});
a47("at the sheet's pose: the turned glass reads upright, the case out of its stand", probe.turnedHome, (t, m) => {
  const bad = turnedCase(t, m);
  if (bad) return bad;
  if (!(t.across > 0 && t.down > 0 && t.row < t.across && t.col < t.down)) {
    return `the turned glass does not read upright (red ${m.r}, green ${m.g}, blue ${m.b})`;
  }
  return null;
});
// (The stand's front lip hides the foot of the reclined glass, so the
// squeezed marks' centroids read flatter still than SQUEEZED; what is held
// is that they are nowhere near the canvas's own proportions.)
a47("face-on, no panel named: the stand is drawn and the glass squeezed (what the Lab showed before)", probe.unturned, (u, m) => {
  if (m.turn !== 0) return "a glass with no panel named turned the case";
  if (!(m.stand > 0)) return "the stand is not drawn";
  if (!u || !(u.across > 0 && u.down > 0) || !(u.ratio < (GLASS_RATIO + SQUEEZED) / 2)) {
    return `not the squeezed glass (${JSON.stringify(u)}; want down/across under ${((GLASS_RATIO + SQUEEZED) / 2).toFixed(2)}, the canvas's being ${GLASS_RATIO.toFixed(2)})`;
  }
  return null;
});
a47("at the sheet's pose, no panel named: the case stays in its stand", probe.unturnedHome, (u, m) => {
  if (m.turn !== 0) return "a glass with no panel named turned the case";
  if (!(m.stand > 0)) return "the stand is not drawn";
  return null;
});

// 5. A56: the case turns the way the firmware turned the glass. The cable
// tab's middle, against the case's: left of it for 1, right for 3, above for
// 2 — by at least a third of the case's half-width (or half-height) — and
// the glass upright on the case every way.
const upright = (t) => t && t.across > 0 && t.down > 0 && t.row < 0.1 * t.across && t.col < 0.1 * t.down;
const a56 = (what, m, turn, side) => held("A56")(what, m, (t) => {
  if (m.turn !== turn) return `the scene turned the case ${m.turn} quarter turns, not the firmware's ${turn}`;
  if (m.stand !== 0) return `the stand turned with the case (${m.stand} stand pixels drawn)`;
  if (!upright(t)) return `the glass does not read upright on the case (red ${m.r}, green ${m.g}, blue ${m.b})`;
  if (!m.cable) return "the case's bottom wall (its cable side) drew no pixel";
  const mx = (m.left + m.right) / 2, my = (m.foot + m.top) / 2;
  const hw = (m.right - m.left) / 2, hh = (m.top - m.foot) / 2;
  const [cx, cy] = m.cable;
  const ok = side === "left" ? cx < mx - hw / 3 : side === "right" ? cx > mx + hw / 3 : cy > my + hh / 3;
  if (!ok) return `the cable side is not on the ${side} (its tab at ${cx.toFixed(0)}, ${cy.toFixed(0)}; the case's middle ${mx}, ${my})`;
  return null;
});
a56("turn 1 (clockwise): the cable side on the left, the glass upright", probe.turnedCw, 1, "left");
a56("turn 3 (counterclockwise): the cable side on the right, the glass upright", probe.turnedCcw, 3, "right");
a56("turn 2 (upside down): the cable side on top, the glass upright", probe.upsideDown, 2, "top");

await browser.close();
server.close();
if (failed) { console.error(failed + " render probe failure(s)"); process.exit(1); }
console.log("render probe: all devices compile, cover, and shade.");
