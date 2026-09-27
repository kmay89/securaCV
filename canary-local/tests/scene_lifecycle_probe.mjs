// canary-local/tests/scene_lifecycle_probe.mjs — CI probe for the studio
// renderer's GPU lifecycle: mount MORE DeviceScenes than a browser will
// keep WebGL contexts for, and prove the page still draws every one.
//
// fleet.html mounts a scene per registry device (28 and counting, plus the
// open sheet). Each used to own a WebGL context; Chromium keeps ~16 alive and
// loses the oldest ("WARNING: Too many active WebGL contexts. Oldest context
// will be lost."), so the first cards went blank — and every new device made
// it worse. scene3d.js now renders every scene through ONE shared context
// and gates each card's loop on visibility. This probe holds that:
//   1. forty scenes on one page log NO context-loss warning and NO WebGL
//      INVALID_OPERATION (the shadow pass used to draw with a stale
//      attribute enabled after a real-shape swap),
//   2. the page holds exactly one live context, and only the cards in view
//      run a frame loop,
//   3. cards above the fold draw a non-empty frame, and cards below the fold
//      draw one once scrolled into view — the eviction that blanked the
//      oldest cards is gone, not just quieter.
//
//   node canary-local/tests/scene_lifecycle_probe.mjs
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set), like
// render_probe.mjs.
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".json": "application/json",
  ".css": "text/css", ".glb": "model/gltf-binary",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const N = 40;              // well past Chromium's live-context cap (~16)
const CARD = 200;          // px; 6 per row at 1300 wide → 7 rows, ~4 on screen

const server = createServer(async (req, res) => {
  const path = req.url.split("?")[0];
  if (path === "/probe.html") {
    res.writeHead(200, { "content-type": "text/html" });
    res.end(HARNESS);
    return;
  }
  try {
    const file = join(ROOT, path);
    const rp = resolve(file);
    if (rp !== ROOT && !rp.startsWith(ROOT + sep)) throw new Error("outside root");
    const body = await readFile(file);
    res.writeHead(200, { "content-type": MIME[extname(path)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404); res.end("not found");
  }
});
await new Promise((ok) => server.listen(0, "127.0.0.1", ok));
const BASE = `http://127.0.0.1:${server.address().port}`;

// The harness: N cards in a wrapping grid, the real engine, the procedural
// witness bodies (no fetch — the point is the count, not the model), each
// started the way fleet.html starts a card. Every card is rebuilt once after
// its first frames, the way a real-shape upgrade clears and refills a scene.
const HARNESS = `<!doctype html><meta charset="utf-8">
<body style="background:#f4f4f5;margin:0;display:flex;flex-wrap:wrap;gap:8px">
<script type="module">
import { DeviceScene, BUILDERS } from "/canary-local/assets/scene3d.js";
window.__probe = { status: "running" };
const frames = (n) => new Promise((ok) => {
  const step = () => (n-- <= 0 ? ok() : requestAnimationFrame(step));
  step();
});
const bodies = [BUILDERS["canary-wap"], BUILDERS["canary-vision"], BUILDERS["canary-sense"]];
const scenes = [];
try {
  for (let i = 0; i < ${N}; i++) {
    const cv = document.createElement("canvas");
    cv.id = "card" + i;
    cv.style.cssText = "width:${CARD}px;height:${CARD}px";
    document.body.append(cv);
    const scene = new DeviceScene(cv, null);   // throws on shader failure
    bodies[i % bodies.length](scene);
    scene.start();
    scenes.push(scene);
  }
  await frames(12);
  for (const [i, s] of scenes.entries()) bodies[(i + 1) % bodies.length](s); // the swap
  await frames(12);
  window.__probe.status = "mounted";
} catch (e) {
  window.__probe.status = "error: " + (e && e.message || e);
}
window.__stats = () => DeviceScene.stats();
window.__scenes = scenes;
// a card's frame: the fraction of its own pixels the device covers
window.__covered = (i) => {
  const cv = document.getElementById("card" + i);
  const px = cv.getContext("2d").getImageData(0, 0, cv.width, cv.height).data;
  let covered = 0;
  for (let a = 3; a < px.length; a += 4) if (px[a] > 8) covered++;
  return covered / (px.length / 4);
};
window.__settle = () => frames(20);
// wipe a card's surface, so anything on it afterwards was drawn afterwards
window.__blank = (i) => {
  const cv = document.getElementById("card" + i);
  cv.getContext("2d").clearRect(0, 0, cv.width, cv.height);
};
</script>`;

const exe = process.env.PW_EXECUTABLE;
const browser = await pw.chromium.launch(exe ? { executablePath: exe } : {});
const page = await browser.newPage({ viewport: { width: 1300, height: 900 } });
const errors = [];
const warnings = [];
page.on("pageerror", (e) => errors.push(String(e)));
page.on("console", (m) => {
  if (["warning", "error"].includes(m.type())) warnings.push(m.text());
});
await page.goto(BASE + "/probe.html");
await page.waitForFunction(() => window.__probe && window.__probe.status !== "running",
                           null, { timeout: 60000 });

let failed = 0;
const fail = (msg) => { console.error("✗ " + msg); failed++; };
const status = await page.evaluate(() => window.__probe.status);
if (status !== "mounted") fail("harness: " + status);
for (const e of errors) fail("pageerror: " + e);

const perRow = Math.floor(1300 / (CARD + 8));
const rows = Math.ceil(N / perRow);
const firstBelow = perRow * Math.ceil(900 / (CARD + 8)); // first card wholly under the fold
const lastCard = N - 1;

// 1. the page never tripped the context cap, and no draw was malformed
const evictions = warnings.filter((w) => /Too many active WebGL contexts/i.test(w));
const invalid = warnings.filter((w) => /WebGL: INVALID_/i.test(w));
if (evictions.length) fail(`${evictions.length}× "Too many active WebGL contexts" with ${N} scenes mounted`);
else console.log(`✓ ${N} scenes mounted, no context evicted`);
if (invalid.length) fail(`WebGL warning: ${invalid[0]}`);
else console.log("✓ no INVALID_OPERATION / INVALID_* WebGL warning");

// 2. one context, and only the cards in view are running
const stats = await page.evaluate(() => window.__stats());
if (stats.contexts !== 1) fail(`expected exactly 1 live WebGL context, got ${stats.contexts}`);
else console.log(`✓ one shared context for ${stats.scenes} scenes`);
if (stats.running >= N) fail(`every scene runs a frame loop (${stats.running}/${N}) — visibility gating is off`);
else if (stats.running === 0) fail("no scene runs a frame loop");
else console.log(`✓ ${stats.running} of ${N} loops running (the cards in view; ${rows} rows, ${perRow} per row)`);

// 3. cards above the fold drew; cards below the fold draw once scrolled to
const top = await page.evaluate((i) => window.__covered(i), 0);
if (top < 0.04) fail(`card 0 (above the fold) is blank (covered ${(top * 100).toFixed(1)}%)`);
else console.log(`✓ card 0 drew (covered ${(top * 100).toFixed(1)}%)`);
// (it drew its first frames before the observer paused it; wipe those, so
// the frame judged below is one drawn AFTER it came into view)
await page.evaluate((i) => window.__blank(i), lastCard);
const belowBefore = await page.evaluate((i) => window.__covered(i), lastCard);
if (belowBefore > 0) fail(`harness: card ${lastCard} did not blank (covered ${(belowBefore * 100).toFixed(1)}%)`);
await page.evaluate((i) => document.getElementById("card" + i).scrollIntoView({ block: "center" }), lastCard);
await page.evaluate(() => window.__settle());
const below = await page.evaluate((i) => window.__covered(i), lastCard);
if (below < 0.04) fail(`card ${lastCard} (below the fold, scrolled into view) is blank (covered ${(below * 100).toFixed(1)}%)`);
else console.log(`✓ card ${lastCard} drew after scrolling into view (covered ${(below * 100).toFixed(1)}%)`);
const stats2 = await page.evaluate(() => window.__stats());
if (stats2.contexts !== 1) fail(`after scrolling: expected 1 live context, got ${stats2.contexts}`);
const topRunning = await page.evaluate(() => !!window.__scenes[0]._raf);
if (topRunning) fail("card 0 scrolled out of view still runs its frame loop");
else console.log("✓ card 0, scrolled out of view, paused its loop");
// …and back again: the first card must come back, not stay blank as the
// evicted "oldest context" did
await page.evaluate((i) => window.__blank(i), 0);
await page.evaluate((i) => document.getElementById("card" + i).scrollIntoView({ block: "center" }), 0);
await page.evaluate(() => window.__settle());
const topAgain = await page.evaluate((i) => window.__covered(i), 0);
if (topAgain < 0.04) fail(`card 0 is blank after scrolling back (covered ${(topAgain * 100).toFixed(1)}%)`);
else console.log(`✓ card 0 draws again after scrolling back (covered ${(topAgain * 100).toFixed(1)}%)`);
if (firstBelow > lastCard) fail(`harness: ${N} cards of ${CARD}px all fit on screen — no card is below the fold`);

await browser.close();
server.close();
if (failed) { console.error(failed + " scene lifecycle probe failure(s)"); process.exit(1); }
console.log("scene lifecycle probe: one context, every card draws.");
