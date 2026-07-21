// canary-local/tests/kiri_slice_probe.mjs — drive the print estimate + the
// Kiri:Moto slice bridge in a real browser.
//
// This is the end-to-end verification the vendored engine wants: it mounts the
// enclosure print guide (WAP compact — committed STLs), proves the estimate
// card renders with real numbers, exercises "watch it print", then clicks
// "⚡ slice for exact time" and asserts the CORRECT outcome for the current
// state of the tree:
//   · engine vendored (assets/vendor/kiri/engine.js present) → the time tile
//     flips to a real "sliced by Kiri:Moto" toolpath time;
//   · engine absent → the button degrades to the honest "isn't vendored" note
//     and the estimate stands.
// Either way: zero page errors. So this both verifies the real slice once the
// engine lands AND pins the fail-closed contract in a real browser until then.
//
// Prints KIRI_PROBE_OK / exits 0 on success. Same harness style as the other
// canary-local probes (playwright or playwright-core + PW_EXECUTABLE).

import { createServer } from "node:http";
import { readFile, access } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
  ".stl": "application/octet-stream", ".wasm": "application/wasm",
  ".glb": "model/gltf-binary",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const fail = (m) => { console.error("KIRI_PROBE_FAIL:", m); process.exit(1); };

// Is the engine actually vendored? Decides which outcome we assert.
const engineVendored = await access(join(ROOT, "canary-local/assets/vendor/kiri/engine.js"))
  .then(() => true).catch(() => false);

const server = createServer(async (req, res) => {
  try {
    const rel = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (rel === "/favicon.ico") { res.writeHead(204); return res.end(); }
    const p = resolve(join(ROOT, rel));
    if (!p.startsWith(ROOT)) { res.writeHead(403); return res.end(); }
    const body = await readFile(p.endsWith("/") ? join(p, "index.html") : p);
    res.writeHead(200, { "content-type": TYPES[extname(p)] || "application/octet-stream" });
    res.end(body);
  } catch { res.writeHead(404); res.end("not found"); }
}).listen(0);
const port = server.address().port;

const errors = [];
const badResponses = [];
// Feature-detecting an ABSENT engine is done by trying to load
// assets/vendor/kiri/engine.js — a miss (404) is the expected mechanism, not a
// bug, so it's the one allowed failed request. Its shadow console line
// ("Failed to load resource…") is filtered too, since it's covered here.
const EXPECT_MISS = "/vendor/kiri/engine.js";
const browser = await pw.chromium.launch(
  process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {}
);
const page = await browser.newPage({ viewport: { width: 1200, height: 900 } });
page.on("console", (m) => {
  if (m.type() === "error" && !/Failed to load resource/i.test(m.text())) errors.push("console: " + m.text());
});
page.on("pageerror", (e) => errors.push("pageerror: " + String(e)));
page.on("response", (r) => {
  if (r.status() >= 400 && !r.url().includes(EXPECT_MISS)) badResponses.push(`${r.status()} ${r.url()}`);
});

try {
  await page.goto(`http://localhost:${port}/canary-local/tests/fixtures/kiri_harness.html`,
    { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForFunction(() => window.__harnessReady || window.__harnessError, { timeout: 20000 });
  if (await page.evaluate(() => window.__harnessError)) fail("harness: " + await page.evaluate(() => window.__harnessError));
  await page.waitForSelector(".enclab", { timeout: 15000 });

  // ── into the print guide ──
  const toPrint = await page.$$eval("button.tab", (bs) =>
    bs.some((b) => b.textContent.trim() === "print guide" && (b.click(), true)));
  if (!toPrint) fail("no 'print guide' subtab");
  await page.waitForSelector(".print-estimate", { timeout: 15000 });

  // ── the estimate card shows real, measured numbers ──
  await page.waitForSelector(".est-slice-btn", { timeout: 15000 });
  const totals = await page.$$eval(".est-totals .est-big b", (bs) => bs.map((b) => b.textContent.trim()));
  if (totals.length < 4) fail("estimate totals thin: " + JSON.stringify(totals));
  const grams = totals.find((t) => /\bg$/.test(t));
  if (!grams || !(parseFloat(grams) > 0)) fail("no positive filament mass in totals: " + JSON.stringify(totals));

  // ── "watch it print" runs without error ──
  const play = await page.$(".print-play");
  if (!play) fail("no watch-it-print button");
  await play.click();                 // start
  await page.waitForTimeout(500);
  await page.$eval(".print-play", (b) => b.click());  // pause — must not throw

  // ── the slice bridge: assert the outcome for THIS tree ──
  const timeBefore = await page.$eval(".est-totals .est-big:first-child b", (b) => b.textContent.trim());
  await page.$eval(".est-slice-btn", (b) => b.click());
  await page.waitForFunction(
    () => {
      const n = document.querySelector(".est-slice-note");
      return n && n.textContent && n.textContent.trim() !== "" && n.textContent.trim() !== "slicing…";
    }, { timeout: 40000 });
  const note = (await page.$eval(".est-slice-note", (n) => n.textContent)).trim();
  const timeLabel = await page.$eval(".est-totals .est-big:first-child i", (i) => i.textContent);

  if (engineVendored) {
    if (!/kiri:moto/i.test(note) && !/sliced/i.test(timeLabel))
      fail("engine vendored but no sliced result — note: " + note + " | label: " + timeLabel);
    console.log("  engine vendored → sliced:", note);
  } else {
    if (!/vendor|isn.t vendored|not vendored/i.test(note))
      fail("engine absent but note wasn't the graceful fallback — got: " + note);
    const timeAfter = await page.$eval(".est-totals .est-big:first-child b", (b) => b.textContent.trim());
    if (timeAfter !== timeBefore) fail("estimate time changed with no engine present");
    console.log("  engine absent → graceful fallback:", note);
  }

  if (errors.length) fail("page errors:\n" + errors.join("\n"));
  if (badResponses.length) fail("unexpected failed requests:\n" + badResponses.join("\n"));
  console.log("KIRI_PROBE_OK");
} catch (e) {
  fail(String(e && e.stack || e));
} finally {
  await browser.close();
  server.close();
}
