// canary-local/tests/kiri_slice_probe.mjs — drive the print estimate + the
// Kiri:Moto slice bridge in a real browser.
//
// This is the end-to-end verification the vendored engine wants: it mounts the
// enclosure print guide (WAP compact — committed STLs), proves the estimate
// card renders with real numbers, exercises "watch it print", then asserts the
// CORRECT outcome of the slice bridge for the current state of the tree:
//   · engine vendored (assets/vendor/kiri/engine.js present) → "⚡ slice for
//     exact time" is offered, and clicking it flips the time tile to a real
//     "sliced by Kiri:Moto" toolpath time;
//   · engine absent → the card offers no slice button and no sentence telling
//     the reader to press one (a button whose only answer was "not included"
//     was a dead end), and the estimate stands.
// Either way: zero page errors. So this both verifies the real slice once the
// engine lands AND pins the fail-closed contract in a real browser until then.
//
// Prints KIRI_PROBE_OK / exits 0 on success. Same harness style as the other
// canary-local probes (playwright or playwright-core + PW_EXECUTABLE).

import { createServer } from "node:http";
import { readFile, access } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { indexTree, lookup } from "./probe_server.mjs";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const FILES = indexTree(ROOT);
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
    const path = req.url.split("?")[0];
    if (path === "/favicon.ico") { res.writeHead(204); return res.end(); }
    // the URL never becomes a path: it is looked up in the tree's index
    // (probe_server.mjs), so the path that reaches readFile is the index's
    const file = lookup(FILES, req.url);
    if (!file) { res.writeHead(404); return res.end("not found"); }
    const body = await readFile(file);
    res.writeHead(200, { "content-type": TYPES[extname(file)] || "application/octet-stream" });
    res.end(body);
  } catch { res.writeHead(404); res.end("not found"); }
});
await new Promise((ok) => server.listen(0, "127.0.0.1", ok));
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
  await page.goto(`http://127.0.0.1:${port}/canary-local/tests/fixtures/kiri_harness.html`,
    { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForFunction(() => window.__harnessReady || window.__harnessError, null, { timeout: 20000 });
  if (await page.evaluate(() => window.__harnessError)) fail("harness: " + await page.evaluate(() => window.__harnessError));
  await page.waitForSelector(".enclab", { timeout: 15000 });

  // ── into the print guide ──
  const toPrint = await page.$$eval("button.tab", (bs) =>
    bs.some((b) => b.textContent.trim() === "print guide" && (b.click(), true)));
  if (!toPrint) fail("no 'print guide' subtab");
  await page.waitForSelector(".print-estimate", { timeout: 15000 });

  // ── the estimate card shows real, measured numbers ──
  await page.waitForSelector(".est-totals .est-big b", { timeout: 15000 });
  const totals = await page.$$eval(".est-totals .est-big b", (bs) => bs.map((b) => b.textContent.trim()));
  if (totals.length < 4) fail("estimate totals thin: " + JSON.stringify(totals));
  const grams = totals.find((t) => /\bg$/.test(t));
  if (!grams || !(parseFloat(grams) > 0)) fail("no positive filament mass in totals: " + JSON.stringify(totals));

  // ── cost-to-build panel renders with the real BOM (WAP is priced) ──
  const buildUnit = await page.$eval(".est-build-unit", (e) => e.textContent).catch(() => "");
  if (!/\$\d/.test(buildUnit)) fail("cost-to-build unit price missing: " + JSON.stringify(buildUnit));

  // ── "watch it print" runs without error ──
  const play = await page.$(".print-play");
  if (!play) fail("no watch-it-print button");
  await play.click();                 // start
  await new Promise((res) => setTimeout(res, 500));
  await page.$eval(".print-play", (b) => b.click());  // pause — must not throw

  // ── the slice bridge: assert the outcome for THIS tree ──
  // .est-slice-btn also styles the "⬇ slicer config (.ini)" download further
  // down the card, so the slice button is the one that says what it does.
  const SLICE = "⚡ slice for exact time";
  const timeBefore = await page.$eval(".est-totals .est-big:first-child b", (b) => b.textContent.trim());
  if (engineVendored) {
    await page.waitForFunction((t) => [...document.querySelectorAll(".est-slice-btn")]
      .some((b) => b.textContent.trim() === t), SLICE, { timeout: 15000 });
    await page.$$eval(".est-slice-btn", (bs, t) => bs.find((b) => b.textContent.trim() === t).click(), SLICE);
    await page.waitForFunction(
      () => {
        const n = document.querySelector(".est-slice-note");
        return n && n.textContent && n.textContent.trim() !== "" && n.textContent.trim() !== "slicing…";
      }, null, { timeout: 40000 });
    const note = (await page.$eval(".est-slice-note", (n) => n.textContent)).trim();
    const timeLabel = await page.$eval(".est-totals .est-big:first-child i", (i) => i.textContent);
    if (!/kiri:moto/i.test(note) && !/sliced/i.test(timeLabel))
      fail("engine vendored but no sliced result — note: " + note + " | label: " + timeLabel);
    console.log("  engine vendored → sliced:", note);
  } else {
    // The engine probe is one failed import; wait for the request that IS the
    // probe to settle, so "no button" is the answer, not a race with it.
    await page.waitForLoadState("networkidle", { timeout: 15000 });
    const offered = await page.$$eval(".est-slice-btn", (bs, t) => bs.some((b) => b.textContent.trim() === t), SLICE);
    if (offered) fail("engine absent but the card still offers " + SLICE);
    const prov = await page.$eval(".est-prov", (p) => p.textContent);
    if (/slice for exact time/i.test(prov)) fail("engine absent but the provenance still says to press the slice button: " + prov);
    const timeAfter = await page.$eval(".est-totals .est-big:first-child b", (b) => b.textContent.trim());
    if (timeAfter !== timeBefore) fail("estimate time changed with no engine present");
    console.log("  engine absent → no slice button; the estimate stands");
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
