// canary-local/tests/vision_probe.mjs — drive the real Vision bench in a browser.
//
// Serves the repo and loads vision.html in headless Chromium, then walks the
// staged flow end to end: every section renders from vision.json, the
// two-port picker answers right and wrong, the SenseCraft stage walks
// Connect → port → Person Detection → upload → live preview with bounding
// boxes on canvas, the host console boots to [DISC], the MQTT retained
// surfaces + all 20 HA discovery entities land, and staged SSCMA boxes flow
// through the compiled production detection/config/voxel/FSM core before a
// witness event and Aim payload appear — all with zero page errors.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set), same as the
// other probes. Prints VISION_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
  ".glb": "model/gltf-binary",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const fail = (m) => { console.error("VISION_PROBE_FAIL:", m); process.exit(1); };

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
const page = await browser.newPage({ viewport: { width: 1200, height: 900 } });
page.on("console", (m) => { if (m.type() === "error") errors.push("console: " + m.text()); });
page.on("pageerror", (e) => errors.push("pageerror: " + String(e)));

try {
  await page.goto(`http://localhost:${port}/canary-local/vision.html`, { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForSelector("#play", { timeout: 15000 });

  // all sections render from the JSON
  for (const id of ["board", "ports", "model", "flash", "serial", "network", "aim", "place", "play", "fix", "more"])
    if (!(await page.$("#" + id))) fail("missing section #" + id);

  // version strip built from vision.json
  const chips = await page.$$eval(".hub-chips .chip", (e) => e.length);
  if (chips < 4) fail("version strip thin (" + chips + " chips)");
  const chipText = await page.$eval(".hub-chips", (n) => n.textContent);
  if (!/runtime\s+real firmware wasm/i.test(chipText))
    fail("Vision runtime did not identify the production firmware wasm: " + chipText);
  if (!(await page.evaluate(() => typeof globalThis.createCanaryVisionCore === "function")))
    fail("committed Canary Vision firmware core factory did not load");

  // ── two-port picker: right and wrong answers both teach ──
  const tasks = await page.$$(".vis-task");
  if (tasks.length < 4) fail("expected ≥4 port tasks, got " + tasks.length);
  // "Load / change the AI model" wants the module port
  await tasks[0].click();
  await page.click(".vis-port-xiao");
  let verdict = await page.$eval(".vis-ports-verdict", (n) => n.textContent);
  if (!/can't do this job/.test(verdict)) fail("wrong-port click did not correct: " + verdict);
  await page.click(".vis-port-module");
  verdict = await page.$eval(".vis-ports-verdict", (n) => n.textContent);
  if (!/Right port/.test(verdict)) fail("right-port click did not confirm: " + verdict);

  // ── SenseCraft stage: Connect → port → model → upload → preview ──
  await page.locator(".vis-ws button.primary", { hasText: "Connect" }).click();
  await page.waitForSelector(".vis-ws-port", { timeout: 4000 });
  await page.click(".vis-ws-port");
  await page.waitForSelector(".vis-ws-model.on", { timeout: 4000 });
  // the other models exist but refuse politely
  const dim = await page.$(".vis-ws-model.dim");
  if (!dim) fail("grayed alternative models missing");
  await page.locator(".vis-ws-model.on").click();
  await page.waitForSelector(".vis-progress-fill", { timeout: 4000 });
  // upload completes into the live preview
  await page.waitForSelector(".vis-preview-cv", { timeout: 20000 })
    .catch(() => fail("preview pane never appeared after the staged upload"));
  const sliders = await page.$$eval(".vis-preview-controls input[type=range]", (e) => e.length);
  if (sliders !== 2) fail("expected Confidence + IoU sliders, got " + sliders);
  // the preview canvas actually paints (the staged scene is not blank)
  await page.waitForFunction(() => {
    const cv = document.querySelector(".vis-preview-cv");
    if (!cv || !cv.width) return false;
    const ctx = cv.getContext("2d");
    const d = ctx.getImageData(0, 0, cv.width, Math.min(50, cv.height)).data;
    for (let i = 0; i < d.length; i += 4) if (d[i] || d[i + 1] || d[i + 2]) return true;
    return false;
  }, null, { timeout: 8000 }).catch(() => fail("preview canvas stayed blank"));
  await page.locator("button.primary", { hasText: "disconnect" }).click();
  const wsDone = await page.$eval(".vis-ws-body", (n) => n.textContent);
  if (!/Model loaded/.test(wsDone)) fail("model-load stage did not complete");

  // ── serial console boots to discovery ──
  await page.locator("#serial .wap-term .primary", { hasText: "power on" }).click();
  await page.locator("#serial .wap-term .ghost", { hasText: "skip" }).click();
  await page.waitForFunction(() => {
    const t = document.querySelector("#serial .wap-term-scroll");
    return t && /Home Assistant discovery published/.test(t.textContent);
  }, null, { timeout: 15000 }).catch(() => fail("serial console never reached [DISC]"));
  const serialTxt = await page.$eval("#serial .wap-term-scroll", (n) => n.textContent);
  if (!/Grove Vision AI ID=/.test(serialTxt)) fail("serial console missing the I2C handshake line");

  // ── MQTT retained surfaces + discovery entity set ──
  await page.waitForFunction(() => document.querySelectorAll(".vis-mqtt-row").length >= 5, null, { timeout: 8000 })
    .catch(() => fail("MQTT retained topics never filled"));
  const ents = await page.$$eval(".vis-entity", (e) => e.length);
  if (ents !== 20) fail("expected 20 HA discovery entities, got " + ents);

  // ── sandbox: walk someone through → a witness event publishes ──
  const cards = await page.$$(".wap-sand-card");
  if (cards.length < 5) fail("expected ≥5 sandbox scenes, got " + cards.length);
  for (const c of cards) { if (/Walk into frame/i.test(await c.textContent())) { await c.click(); break; } }
  await page.waitForFunction(() => {
    const l = document.querySelector(".vis-evlog-body");
    return l && /presence_started/.test(l.textContent);
  }, null, { timeout: 15000 }).catch(() => fail("walk scene never produced presence_started"));
  await page.waitForFunction(() => {
    const rows = [...document.querySelectorAll(".vis-mqtt-row")];
    return rows.some((r) => /\/events/.test(r.textContent) && /presence_started/.test(r.textContent));
  }, null, { timeout: 8000 }).catch(() => fail("presence_started never landed on MQTT"));

  // ── aim card: switch on → the firmware's exact payload streams ──
  await page.click(".vis-aim-switch");
  for (const c of await page.$$(".wap-sand-card")) {
    if (/Stay a while/i.test(await c.textContent())) { await c.click(); break; }
  }
  await page.waitForFunction(() => {
    const t = document.querySelector(".vis-aim-ticker");
    return t && /"present":true/.test(t.textContent);
  }, null, { timeout: 15000 }).catch(() => fail("aim ticker never streamed a present frame"));
  const ticker = await page.$eval(".vis-aim-ticker", (n) => n.textContent);
  for (const k of ["present", "x", "y", "w", "h", "score", "vr", "vc", "rows", "cols", "fw", "fh"])
    if (!new RegExp('"' + k + '":').test(ticker)) fail("aim payload missing key " + k + ": " + ticker);

  // ── placement preset lands on the tuning sliders ──
  const before = await page.$eval(".vis-tune input[type=range]", (i) => i.value);
  await page.locator(".vis-uc", { hasText: "Home office" }).click();
  const after = await page.$eval(".vis-tune input[type=range]", (i) => i.value);
  if (before === after) fail("preset did not move the score slider (" + before + " → " + after + ")");
  if (after !== "85") fail("office preset should set score 85, got " + after);

  // ── 3D board canvas mounted ──
  if (!(await page.$("#board canvas"))) fail("board 3D canvas missing");

  if (errors.length) fail("page errors:\n  " + errors.join("\n  "));
  console.log("VISION_PROBE_OK");
  process.exit(0);
} catch (e) {
  fail(String(e && e.stack || e));
} finally {
  await browser.close().catch(() => {});
  server.close();
}
