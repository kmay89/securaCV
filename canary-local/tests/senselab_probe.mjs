// canary-local/tests/senselab_probe.mjs — drive the real Sense Lab in a browser.
//
// Serves the repo and loads senselab.html in headless Chromium, then exercises
// the bench: sections render from sense.json, the placement stage draws both
// views, the live pipeline streams bytes → frames → a signed presence event,
// the Canary Cards grid builds one card per entity (with the P1 story
// honest), the privacy chokepoint drops what it promises, the knobs move the
// FSM, and unplugging the radar drives the stall path — zero page errors.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set), same as the
// other probes. Prints SENSELAB_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
  ".glb": "model/gltf-binary", ".stl": "model/stl",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const fail = (m) => { console.error("SENSELAB_PROBE_FAIL:", m); process.exit(1); };

const server = createServer(async (req, res) => {
  try {
    const rel = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (rel === "/favicon.ico") { res.writeHead(204); return res.end(); }
    const p = resolve(join(ROOT, rel));
    if (p !== ROOT && !p.startsWith(ROOT + sep)) { res.writeHead(403); return res.end(); }
    const file = rel.endsWith("/") ? join(p, "index.html") : p;
    const body = await readFile(file);
    res.writeHead(200, { "content-type": TYPES[extname(file)] || "application/octet-stream" });
    res.end(body);
  } catch { res.writeHead(404); res.end("not found"); }
}).listen(0);
const port = server.address().port;

const errors = [];
const browser = await pw.chromium.launch(
  process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {}
);
const page = await browser.newPage({ viewport: { width: 1280, height: 950 } });
page.on("console", (m) => { if (m.type() === "error") errors.push("console: " + m.text()); });
page.on("pageerror", (e) => errors.push("pageerror: " + String(e)));

try {
  await page.goto(`http://localhost:${port}/canary-local/senselab.html`, { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForSelector("#power", { timeout: 15000 });

  // all sections render from the JSON
  for (const id of ["deep", "place", "pipeline", "cards", "glass", "power", "more"])
    if (!(await page.$("#" + id))) fail("missing section #" + id);

  // version strip built from sense.json
  const chips = await page.$$eval("#senselab-versions .chip", (e) => e.length);
  if (chips < 5) fail("version strip thin (" + chips + " chips)");

  // §deep: the protocol table carries all 5 decoded types + the refused list
  const frameRows = await page.$$eval("#deep .slab-frames tbody tr", (e) => e.length);
  if (frameRows < 8) fail("protocol tables thin (" + frameRows + " rows across decoded+refused)");
  const benchItems = await page.$$eval(".slab-bench li", (e) => e.length);
  if (benchItems < 3) fail("[BENCH] list missing (" + benchItems + ")");
  if (!(await page.$(".slab-3d"))) fail("radome 3D canvas missing");

  // §place: both stage views drew, and the quality meter speaks
  const views = await page.$$(".slab-view");
  if (views.length !== 2) fail("expected 2 stage views, got " + views.length);
  const fovDrawn = await page.$$eval(".slab-view .slab-fov", (e) => e.length);
  if (fovDrawn < 1) fail("detection sector not drawn");
  const qlabel = await page.$eval(".slab-qlabel", (n) => n.textContent);
  if (!/quality \d+ %/.test(qlabel)) fail("quality meter silent: " + qlabel);

  // §pipeline: bytes stream, frames decode, and the default bedside scene
  // must sign a presence_detected within a few simulated seconds
  await page.waitForFunction(
    () => document.querySelectorAll(".slab-hex-scroll .wap-line").length > 3,
    null, { timeout: 10000 }).catch(() => fail("UART hex stream never flowed"));
  const ev = await page.waitForSelector(".slab-ev", { timeout: 15000 }).catch(() => null);
  if (!ev) fail("no witness event was recorded");
  const evText = await ev.textContent();
  if (!/presence_detected/.test(evText)) fail("first event is not presence_detected: " + evText);
  if (!/securacv-canary-sig\|v1\|sense\|/.test(evText)) fail("event canonical missing");

  // the chokepoint drops distance + shows the coarse vocabulary
  const chokeRaw = await page.$eval(".slab-choke", (n) => n.textContent);
  if (!/distance_cm/.test(chokeRaw) || !/dropped/.test(chokeRaw)) fail("chokepoint not dropping distance");
  if (!/presence/.test(chokeRaw) || !/occupants/.test(chokeRaw)) fail("chokepoint vocabulary missing");

  // the unknown counter ticks (phase-waveform frames arrive and are refused)
  await page.waitForFunction(
    () => /unknown ×[1-9]/.test(document.querySelector(".slab-choke")?.textContent || ""),
    null, { timeout: 12000 }).catch(() => fail("unknown-frame counter never ticked (0x0A13)"));

  // §cards: one card per entity, schema-valid, P1 honest
  const cardCount = await page.$$eval(".ccard", (e) => e.length);
  if (cardCount < 10) fail("card grid thin (" + cardCount + ")");
  const cardIds = await page.$$eval(".ccard", (es) => es.map((e) => e.dataset.cardId));
  for (const want of ["presence", "occupants", "range_band", "radar_link", "breathing", "heart_rate", "chain"])
    if (!cardIds.includes(want)) fail("missing card: " + want);
  if (await page.$(".ccard-invalid")) fail("an invalid card rendered");

  // the default bedside scene must reach a breathing LOCK (the bench-found
  // interleaved-traffic firmware fix — a still person at 1 m locks in ~4 s)
  await page.waitForFunction(() => {
    const br = [...document.querySelectorAll(".ccard")].find((c) => c.dataset.cardId === "breathing");
    return br && /locked/.test(br.textContent);
  }, null, { timeout: 25000 }).catch(() => fail("breathing never locked at the bedside preset"));

  // flip to the presence-only flavor: BPM cards must go provably-absent
  await page.click('button[data-flavor="canary-sense-default"]');
  await page.waitForFunction(() => {
    const hr = [...document.querySelectorAll(".ccard")].find((c) => c.dataset.cardId === "heart_rate");
    return hr && hr.classList.contains("ccard-absent");
  }, null, { timeout: 6000 }).catch(() => fail("presence-only build did not render BPM cards as absent"));
  await page.click('button[data-flavor="canary-sense-wellbeing"]');

  // scenario: unplug the radar → the stall deadline must drive radar_link to a problem
  const sand = await page.$$(".slab-sandbox .wap-sand-card");
  if (sand.length < 6) fail("sandbox thin (" + sand.length + ")");
  let unplug = null;
  for (const c of sand) if (/unplug/i.test(await c.textContent())) unplug = c;
  if (!unplug) fail("no unplug scenario");
  await unplug.click();
  await page.waitForFunction(() => {
    const rl = [...document.querySelectorAll(".ccard")].find((c) => c.dataset.cardId === "radar_link");
    return rl && /stalled/.test(rl.textContent);
  }, null, { timeout: 20000 }).catch(() => fail("stall deadline never surfaced on the radar_link card"));
  await unplug.click(); // plug it back

  // §power: rails render and the totals speak claims-per-joule
  const pbars = await page.$$eval(".slab-pbar", (e) => e.length);
  if (pbars < 5) fail("power rails thin (" + pbars + ")");
  const totals = await page.$eval(".slab-ptotals", (n) => n.textContent);
  if (!/claims per joule/.test(totals) || !/sensing share/.test(totals)) fail("power totals missing");

  // §glass: the bridge offers the real-firmware boot (artifact presence is
  // checked, not booted — the wasm boot has its own dedicated probes)
  const glassBtn = await page.$(".slab-glass button");
  if (!glassBtn) fail("glass bridge button missing");

  if (errors.length) fail(errors.length + " page/console errors: " + errors.join(" | "));
  console.log(`SENSELAB_PROBE_OK — ${chips} chips, ${cardCount} cards, ${sand.length} scenarios, ${pbars} rails`);
} finally {
  await browser.close();
  server.close();
}
process.exit(0);
