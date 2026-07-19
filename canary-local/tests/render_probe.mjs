// canary-local/tests/render_probe.mjs — CI probe for the studio renderer:
// serve the repo, stand every device from BUILDERS in front of the real
// WebGL engine, and prove three things a unit test can't:
//   1. the shaders COMPILE on an actual GPU/driver (DeviceScene throws
//      loud on compile/link failure — a bad GLSL edit fails here, not in
//      a visitor's browser),
//   2. every device draws a NON-EMPTY frame (pixels actually covered),
//   3. the frame isn't a flat blob (color variance — lighting is alive).
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

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".json": "application/json",
  ".css": "text/css",
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
    const file = join(ROOT, path);
    if (!resolve(file).startsWith(ROOT)) throw new Error("outside root");
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
    build(scene);
    scene.start();
    await frames(24);                          // settle: sway, shadow, glass
    scene.stop();
    scene.draw();                              // final deterministic frame
    const gl = scene.gl;
    const px = new Uint8Array(gl.drawingBufferWidth * gl.drawingBufferHeight * 4);
    gl.readPixels(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight,
                  gl.RGBA, gl.UNSIGNED_BYTE, px);
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

const EXPECT = ["canary-display-watch", "canary-display-dash", "canary-vision",
                "canary-wap", "canary-sense", "canary-fence-guard"];
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

await browser.close();
server.close();
if (failed) { console.error(failed + " render probe failure(s)"); process.exit(1); }
console.log("render probe: all devices compile, cover, and shade.");
