// canary-local/tests/flash_probe.mjs — load the flasher page in a browser.
//
// The flasher's DOM-free core is heavily unit-tested (flash.test.js and
// friends), but nothing loaded flash.html itself — so a page-level regression
// (module load order, phase rendering, a catalog change boot() chokes on)
// only surfaced manually. This is the same cheap smoke every other bench gets:
// serve the repo, load flash.html in headless Chromium, and prove, with zero
// page errors, that the visitor gets a working first phase on BOTH branches:
//   - with Web Serial (Chromium exposes it headless): the committed catalog
//     loads and the Connect phase renders under the journey nav;
//   - with navigator.serial removed (Safari/Firefox visitors): the honest
//     "one hop to Chrome" card renders instead, hop endpoints labeled.
// Flashing itself can't run headless (no real serial device) — that stays on
// the bench. This probe pins "the page boots and shows the right first card."
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set). Prints
// FLASH_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const fail = (m) => { console.error("FLASH_PROBE_FAIL:", m); process.exit(1); };

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
  process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {},
);
const page = await browser.newPage({ viewport: { width: 1280, height: 960 } });
page.on("console", (m) => { if (m.type() === "error") errors.push("console: " + m.text()); });
page.on("pageerror", (e) => errors.push("pageerror: " + String(e)));

try {
  // ── branch 1: Web Serial present (headless Chromium exposes it) ──────────
  await page.goto(`http://127.0.0.1:${port}/canary-local/flash.html`, {
    waitUntil: "load", timeout: 30000,
  });
  const hasSerial = await page.evaluate(() => "serial" in navigator);
  if (!hasSerial) fail("headless Chromium unexpectedly lacks Web Serial — branch 1 unproven");

  await page.waitForSelector("#flash .flash-connect", { timeout: 15000 })
    .catch(() => fail("the Connect phase did not render (catalog fetch or boot() broke)"));
  // boot() only reaches the Connect phase after devices/flash.json parsed —
  // so this journey nav doubles as "the committed catalog still loads".
  const steps = await page.$$eval("#flash .flash-journey-step",
    (els) => els.map((e) => e.textContent.trim()).filter(Boolean));
  if (steps.length < 4) fail(`journey nav incomplete: ${JSON.stringify(steps)}`);
  const cards = await page.$$eval("#flash .flash-card", (els) => els.length);
  if (!(cards > 0)) fail("Connect phase rendered no cards");
  console.log(`serial path: connect phase up, ${cards} cards, journey ${steps.join(" · ")}`);

  // ── branch 2: no Web Serial (what Safari/Firefox visitors get) ───────────
  const noSerial = await browser.newPage({ viewport: { width: 1280, height: 960 } });
  noSerial.on("console", (m) => { if (m.type() === "error") errors.push("console: " + m.text()); });
  noSerial.on("pageerror", (e) => errors.push("pageerror: " + String(e)));
  await noSerial.addInitScript(() => { delete Object.getPrototypeOf(navigator).serial; });
  await noSerial.goto(`http://127.0.0.1:${port}/canary-local/flash.html`, {
    waitUntil: "load", timeout: 30000,
  });
  await noSerial.waitForSelector("#flash .flash-unsupported", { timeout: 15000 })
    .catch(() => fail("the unsupported-browser card did not render without Web Serial"));
  const labels = await noSerial.$$eval(".flash-hop-label",
    (els) => els.map((e) => e.textContent.trim()).filter(Boolean));
  if (labels.length < 2) fail("unsupported card rendered without the hop labels");
  console.log(`no-serial path: hop ${labels.join(" -> ")}`);

  if (errors.length) fail("page errors:\n  " + errors.join("\n  "));
  console.log("FLASH_PROBE_OK");
  process.exit(0);
} catch (e) {
  fail(String((e && e.stack) || e));
} finally {
  await browser.close().catch(() => {});
  server.close();
}
