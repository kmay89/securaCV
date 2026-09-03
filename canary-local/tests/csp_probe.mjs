// canary-local/tests/csp_probe.mjs — every Lab page, in a real browser, under
// its own Content-Security-Policy, with zero violations.
//
// tests/csp.test.js proves each page's policy is the one tools/gen_csp.py
// writes. This proves the pages still WORK under it: serve the repo over
// loopback, open every canary-local/*.html and the emulator harness (once per
// committed display flavor), listen for `securitypolicyviolation` in every
// frame from before the first byte of page script runs, wait for load plus a
// settle, and fail on any violation, page error or console error.
//
// A synthetic page with a deliberate violation runs FIRST: a listener that has
// silently stopped hearing violations would otherwise pass every real page.
//
//   node canary-local/tests/csp_probe.mjs [--only <page.html>]
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set). Prints
// CSP_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile, readdir } from "node:fs/promises";
import { extname, join, dirname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const LAB = "canary-local";
const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
  ".png": "image/png", ".jpg": "image/jpeg", ".webp": "image/webp", ".ico": "image/x-icon",
  ".wasm": "application/wasm", ".glb": "model/gltf-binary", ".stl": "application/octet-stream",
  ".3mf": "application/octet-stream", ".scad": "text/plain", ".txt": "text/plain",
  ".md": "text/markdown", ".woff2": "font/woff2", ".woff": "font/woff",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const onlyIdx = process.argv.indexOf("--only");
const ONLY = onlyIdx > 0 ? process.argv[onlyIdx + 1] : null;

// A page that MUST violate: the same floor every Lab page carries, plus one
// inline script. If the probe hears nothing here, the probe is broken.
const SELFTEST_PATH = "/__csp_selftest.html";
const SELFTEST_HTML = `<!DOCTYPE html><html><head><meta charset="utf-8" />
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'self'; style-src 'self'; base-uri 'none'" />
<title>csp probe self-test</title></head><body><p>self-test</p><script>window.__ran = true;</script></body></html>`;

const server = createServer(async (req, res) => {
  try {
    const rel = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (rel === "/favicon.ico") { res.writeHead(204); return res.end(); }
    if (rel === SELFTEST_PATH) {
      res.writeHead(200, { "content-type": "text/html" });
      return res.end(SELFTEST_HTML);
    }
    const p = resolve(join(ROOT, rel));
    if (p !== ROOT && !p.startsWith(ROOT + sep)) { res.writeHead(403); return res.end(); }
    const file = rel.endsWith("/") ? join(p, "index.html") : p;
    const body = await readFile(file);
    res.writeHead(200, { "content-type": TYPES[extname(file)] || "application/octet-stream" });
    res.end(body);
  } catch { res.writeHead(404); res.end("not found"); }
}).listen(0);
const port = server.address().port;
const url = (rel) => `http://localhost:${port}/${rel}`;

// What every page loads, plus the harness once per committed display flavor.
const pages = (await readdir(join(ROOT, LAB))).filter((f) => f.endsWith(".html")).sort();
const flavors = (await readdir(join(ROOT, LAB, "emulator/dist")))
  .map((f) => /^canary-display-([a-z0-9]+)\.js$/.exec(f)?.[1]).filter(Boolean).sort();
if (!flavors.length) { console.error("CSP_PROBE_FAIL: no canary-display-*.js in emulator/dist/"); process.exit(1); }

// Pages that boot the firmware wasm get longer to do it; the harness reports
// readiness itself. index.html redirects by script — proof the external
// redirect module ran under the policy is where it lands: a device hash goes
// to fleet.html, which only the script does (the meta refresh goes to lab.html).
const WASM_PAGES = new Set(["eyes.html", "fleet.html", "senselab.html", "smoke.html", "vision.html"]);
const targets = [];
for (const p of pages) {
  if (p === "index.html") targets.push({ name: p, path: `${LAB}/index.html#securacv-canary`, redirect: /\/fleet\.html#securacv-canary$/ });
  else targets.push({ name: p, path: `${LAB}/${p}`, settle: WASM_PAGES.has(p) ? 5000 : 1500 });
}
for (const f of flavors) {
  targets.push({ name: `emulator/web/harness.html?flavor=${f}`, path: `${LAB}/emulator/web/harness.html?hour=10&flavor=${f}`, ready: "window.__ready === true", settle: 1000 });
}
const run = ONLY ? targets.filter((t) => t.name === ONLY || t.name.startsWith(ONLY)) : targets;
if (!run.length) { console.error(`CSP_PROBE_FAIL: --only ${ONLY} matches no page`); process.exit(1); }

const browser = await pw.chromium.launch(
  process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {},
);
const context = await browser.newContext({ viewport: { width: 1280, height: 900 } });
// Installed in every frame before any page script: the record of violations.
await context.addInitScript(() => {
  window.__cspViolations = [];
  document.addEventListener("securitypolicyviolation", (e) => {
    window.__cspViolations.push({
      directive: e.effectiveDirective || e.violatedDirective,
      blocked: e.blockedURI,
      source: e.sourceFile ? `${e.sourceFile}:${e.lineNumber}:${e.columnNumber}` : "",
      sample: e.sample || "",
      disposition: e.disposition,
    });
  });
});

async function violationsIn(page) {
  const all = [];
  for (const f of page.frames()) {
    try {
      const v = await f.evaluate(() => (window.__cspViolations || []).slice());
      for (const x of v) all.push({ frame: f.url().replace(/^http:\/\/localhost:\d+\//, ""), ...x });
    } catch { /* a frame that navigated away or detached mid-read */ }
  }
  return all;
}
const fmt = (v) => `${v.frame}: ${v.directive} blocked ${v.blocked || "(inline)"}${v.source ? " at " + v.source : ""}${v.sample ? ` — "${v.sample.slice(0, 60)}"` : ""}`;

const failures = [];
const fail = (name, msg) => { console.error(`CSP_PROBE_FAIL[${name}]: ${msg}`); failures.push(name); };

// ── 0. the detector detects ──
{
  const page = await context.newPage();
  await page.goto(url(SELFTEST_PATH.slice(1)), { waitUntil: "load" });
  const ran = await page.evaluate(() => window.__ran === true);
  const v = await violationsIn(page);
  await page.close();
  if (ran) { console.error("CSP_PROBE_FAIL: the self-test's inline script RAN — the policy is not being enforced by this browser"); process.exit(1); }
  if (!v.some((x) => /script-src/.test(x.directive))) {
    console.error("CSP_PROBE_FAIL: the self-test page violated its policy but no securitypolicyviolation was recorded — the detector is broken");
    process.exit(1);
  }
  console.log(`csp_probe: self-test heard ${v.length} violation(s) — detector live`);
}

// ── 1. every page ──
for (const t of run) {
  const page = await context.newPage();
  const errors = [];
  // "Failed to load resource" console lines carry no URL; the response /
  // requestfailed events below re-report those misses WITH the URL.
  page.on("console", (m) => {
    if (m.type() === "error" && !/^Failed to load resource/.test(m.text())) errors.push("console: " + m.text());
  });
  page.on("pageerror", (e) => errors.push("pageerror: " + String(e)));
  page.on("response", (r) => { if (r.status() >= 400) errors.push(`http ${r.status()} ${r.url()}`); });
  page.on("requestfailed", (r) => {
    // The vendored Witness Wall emulator (its own document, in an iframe)
    // looks for a live kernel on the LAN (canary.local); off-LAN that is a
    // DNS miss, not a defect. A request the POLICY refused is reported by
    // the violation listener, never here — so this allowance cannot hide one.
    let host = "";
    try { host = new URL(r.url()).hostname; } catch { /* keep "" */ }
    if (/\.local$/.test(host)) return;
    errors.push(`request failed: ${r.failure()?.errorText || "?"} ${r.url()}`);
  });
  let violations = [];
  let hasMeta = false;
  try {
    await page.goto(url(t.path), { waitUntil: "load", timeout: 60000 });
    if (t.redirect) {
      await page.waitForURL(t.redirect, { timeout: 15000 });
      await page.waitForLoadState("load");
    }
    if (t.ready) await page.waitForFunction(t.ready, null, { timeout: 90000 });
    await page.waitForLoadState("networkidle", { timeout: 20000 }).catch(() => {});
    await new Promise((r) => setTimeout(r, t.settle || 1500));
    hasMeta = await page.evaluate(() => !!document.querySelector('meta[http-equiv="Content-Security-Policy"]'));
    violations = await violationsIn(page);
  } catch (e) {
    errors.push(`did not load: ${e}`);
  }
  await page.close();

  if (errors.length) { fail(t.name, "page errors:\n  " + errors.slice(0, 8).join("\n  ")); continue; }
  if (!hasMeta) { fail(t.name, "the loaded document carries no CSP <meta> (gen_csp.py did not run on it, or the redirect landed off the Lab)"); continue; }
  if (violations.length) { fail(t.name, `${violations.length} CSP violation(s):\n  ` + violations.slice(0, 12).map(fmt).join("\n  ")); continue; }
  console.log(`CSP_PROBE_OK[${t.name}]`);
}

await browser.close();
server.close();
if (failures.length) {
  console.error(`CSP_PROBE_FAIL: ${failures.length} of ${run.length} pages failed (${failures.join(", ")})`);
  process.exit(1);
}
console.log(`CSP_PROBE_OK all ${run.length} pages loaded under their policies with zero violations`);
process.exit(0);
