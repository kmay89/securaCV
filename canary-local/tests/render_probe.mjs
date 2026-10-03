// canary-local/tests/render_probe.mjs — CI probe for the studio renderer:
// serve the repo, stand every device from BUILDERS in front of the real
// WebGL engine, and prove three things a unit test can't:
//   1. the shaders COMPILE on an actual GPU/driver (DeviceScene throws
//      loud on compile/link failure — a bad GLSL edit fails here, not in
//      a visitor's browser),
//   2. every device draws a NON-EMPTY frame (pixels actually covered),
//   3. the frame isn't a flat blob (color variance — lighting is alive).
//   4. a turned glass reads upright on its turned model (sweep A47): the
//      dash scene fed a 480x800 canvas (a portrait dash's glass) with three
//      marks — red and green on one row, blue under red — and the panel's
//      own 800x480 shape draws them face-on where a person in front of the
//      glass sees them, at the canvas's portrait proportions, while the
//      same canvas left unturned is squeezed into the landscape screen. The
//      marks sit in the glass's lower half: the studio light's highlight
//      across the top of a face-on screen washes a mark there to near white.
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
  if (path === "/probe.html") {
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
import { DeviceScene, BUILDERS } from "/canary-local/assets/scene3d.js";
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
  // 4. A47: the turned glass, face-on, against the same glass left unturned.
  const glass = document.createElement("canvas");
  glass.width = 480; glass.height = 800;
  const g = glass.getContext("2d");
  g.fillStyle = "#101010"; g.fillRect(0, 0, 480, 800);
  // red and green on one row, blue under red: centers 360 px across and
  // 240 px down, in the lower half (the highlight washes the top)
  g.fillStyle = "#ff0000"; g.fillRect(0, 440, 120, 120);
  g.fillStyle = "#00ff00"; g.fillRect(360, 440, 120, 120);
  g.fillStyle = "#0000ff"; g.fillRect(0, 680, 120, 120);
  const marks = async (panel) => {
    const cv = document.createElement("canvas");
    cv.style.cssText = "width:420px;height:420px";
    document.body.append(cv);
    const scene = new DeviceScene(cv, glass);
    await BUILDERS["canary-display-dash"](scene);
    scene.glass = panel;
    scene.autoSway = false;
    scene.rot = { x: 0, y: 0 };
    scene.home = { x: 0, y: 0 };
    scene.draw();
    const gl = scene.gl, W = cv.width, H = cv.height;
    const px = new Uint8Array(W * H * 4);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, px);
    const c = { r: [0, 0, 0], g: [0, 0, 0], b: [0, 0, 0] };   // sum x, sum y (up), count
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        const i = (y * W + x) * 4, R = px[i], G = px[i + 1], B = px[i + 2];
        // a mark is its channel standing 40 clear of the other two (the
        // studio light adds white to every channel; the margin survives it)
        const k = R - Math.max(G, B) > 40 ? "r" : G - Math.max(R, B) > 40 ? "g"
          : B - Math.max(R, G) > 40 ? "b" : null;
        if (k) { c[k][0] += x; c[k][1] += y; c[k][2]++; }
      }
    }
    scene.dispose?.();
    cv.remove();
    const at = (k) => (c[k][2] ? [c[k][0] / c[k][2], c[k][1] / c[k][2]] : null);
    return { turn: scene.turn, r: at("r"), g: at("g"), b: at("b") };
  };
  window.__probe.turned = await marks({ w: 800, h: 480 });
  window.__probe.unturned = await marks(null);
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
await page.goto(BASE + "/probe.html");
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

// 4. A47: on the turned model the marks stand as on the glass — red left of
// green on one row, blue under red — at the canvas's proportions (240 down
// to 360 across); the unturned control squeezes them into the landscape
// screen, 480/800 down and 800/480 across.
const GLASS_RATIO = 240 / 360;
const SQUEEZED = GLASS_RATIO * (480 / 800) / (800 / 480);
const shape = (m) => {
  if (!m || !m.r || !m.g || !m.b) return null;
  const across = m.g[0] - m.r[0], down = m.r[1] - m.b[1];   // readPixels y runs up
  return { across, down, ratio: down / across, row: Math.abs(m.g[1] - m.r[1]), col: Math.abs(m.b[0] - m.r[0]) };
};
const near = (v, want) => Math.abs(v / want - 1) < 0.2;
const t = shape(probe.turned), u = shape(probe.unturned);
if (!t) fail(`A47: the turned glass's marks are not all on the frame (${JSON.stringify(probe.turned)})`);
else if (probe.turned.turn !== 1) fail(`A47: the scene did not turn the model for a 480x800 glass on an 800x480 panel`);
else if (!(t.across > 0 && t.down > 0 && t.row < 0.1 * t.across && t.col < 0.1 * t.down)) {
  fail(`A47: the turned glass does not read upright (red ${probe.turned.r}, green ${probe.turned.g}, blue ${probe.turned.b})`);
} else if (!near(t.ratio, GLASS_RATIO)) {
  fail(`A47: the turned glass is not at its proportions (down/across ${t.ratio.toFixed(2)}, the canvas's ${GLASS_RATIO.toFixed(2)})`);
} else console.log(`✓ A47: a 480x800 glass reads upright on the turned dash (down/across ${t.ratio.toFixed(2)}; canvas ${GLASS_RATIO.toFixed(2)})`);
if (!u || probe.unturned.turn !== 0 || !(u.across > 0 && u.down > 0) || !near(u.ratio, SQUEEZED)) {
  fail(`A47: the unturned control is not the squeezed glass (${JSON.stringify(u)}; want down/across near ${SQUEEZED.toFixed(2)})`);
} else console.log(`✓ A47: the same glass unturned is squeezed (down/across ${u.ratio.toFixed(2)}, want ${SQUEEZED.toFixed(2)}): what the Lab showed before`);

await browser.close();
server.close();
if (failed) { console.error(failed + " render probe failure(s)"); process.exit(1); }
console.log("render probe: all devices compile, cover, and shade.");
