// canary-local/tests/csp_probe.mjs — every Lab page, in a real browser, under
// its own Content-Security-Policy, with zero violations.
//
// tests/csp.test.js proves each page's policy is the one tools/gen_csp.py
// writes. This proves the pages still WORK under it: serve the repo over
// loopback, open every page tools/gen_csp.py writes a policy into (every
// canary-local/*.html, the Canary Voice sound board voice/index.html, and the
// emulator harness once per committed display flavor), listen for `securitypolicyviolation` in every frame from before the
// first byte of page script runs, wait for load plus a settle, and fail on any
// violation, page error or console error, or on a document in quirks mode or
// not decoded as UTF-8 (this server sends a bare text/html, as
// `python3 -m http.server` does, so only the page's own <meta charset> sets it).
//
// The sound board is also PLAYED: every card is clicked, and the probe fails
// unless the Web Audio graph reaches the speakers (a connect() to the
// context's destination), the scope's analyzer node carries the signal, and
// the scope draws a trace. A one-word slip in a Web Audio method name once
// left the board throwing on the first click and silent after it, with every
// load-only gate green.
//
// A synthetic page with a deliberate violation runs FIRST: a listener that has
// silently stopped hearing violations would otherwise pass every real page.
//
//   node canary-local/tests/csp_probe.mjs [--only <page.html>] [--list]
//
// --list names every page it would open, one per line, without a browser.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set). Prints
// CSP_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile, readdir } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { indexTree, lookup } from "./probe_server.mjs";

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

const onlyIdx = process.argv.indexOf("--only");
const ONLY = onlyIdx > 0 ? process.argv[onlyIdx + 1] : null;

// A page that MUST violate: the same floor every Lab page carries, plus one
// inline script. If the probe hears nothing here, the probe is broken.
const SELFTEST_PATH = "/__csp_selftest.html";
const SELFTEST_HTML = `<!DOCTYPE html><html><head><meta charset="utf-8" />
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'self'; style-src 'self'; base-uri 'none'" />
<title>csp probe self-test</title></head><body><p>self-test</p><script>window.__ran = true;</script></body></html>`;

// Every page tools/gen_csp.py writes a policy into, read from its --explain
// (each page on an unindented line), so a page registered there is a page
// opened here; tests/csp.test.js holds that list to every .html under the Lab
// bar the one it names as out of scope, and holds this probe's --list to it.
// The emulator harness is the one page loaded once per committed display
// flavor instead of once.
const HARNESS = "emulator/web/harness.html";
const VOICE = "voice/index.html";
const explain = spawnSync("python3", [join(ROOT, LAB, "tools/gen_csp.py"), "--explain"], { encoding: "utf8" });
if (explain.status !== 0) {
  console.error(`CSP_PROBE_FAIL: tools/gen_csp.py --explain exited ${explain.status}: ${explain.stderr || explain.error}`);
  process.exit(1);
}
const listed = explain.stdout.split("\n").filter((l) => l && !/^\s/.test(l));
for (const must of ["index.html", VOICE, HARNESS]) {
  if (!listed.includes(must)) { console.error(`CSP_PROBE_FAIL: gen_csp.py --explain no longer names ${must}`); process.exit(1); }
}
const pages = listed.filter((p) => p !== HARNESS);
const flavors = (await readdir(join(ROOT, LAB, "emulator/dist")))
  .map((f) => /^canary-display-([a-z0-9]+)\.js$/.exec(f)?.[1]).filter(Boolean).sort();
if (!flavors.length) { console.error("CSP_PROBE_FAIL: no canary-display-*.js in emulator/dist/"); process.exit(1); }

// Pages that boot the firmware wasm get longer to do it; the harness reports
// readiness itself, and its wait is a function, never a string: Playwright
// re-evaluates a string predicate through eval on every animation frame,
// which the harness's policy refuses (and this probe would then count its
// own eval as the page's violation) (sweep A44). index.html redirects by script — proof the external
// redirect module ran under the policy is where it lands: a device hash goes
// to fleet.html, which only the script does (the meta refresh goes to lab.html).
// Installed before the sound board's own script: counts every AudioNode
// connect(), the ones that reach the context's destination, and keeps the
// AnalyserNode (the Web Audio interface's own name) the scope reads, so the
// probe can ask it whether sound is flowing.
function countAudioConnects() {
  window.__audio = { connects: 0, toDestination: 0, analyzer: null };
  const connect = AudioNode.prototype.connect;
  AudioNode.prototype.connect = function (dest, ...rest) {
    window.__audio.connects++;
    if (dest instanceof AudioDestinationNode) window.__audio.toDestination++;
    if (this instanceof AnalyserNode) window.__audio.analyzer = this;
    return connect.call(this, dest, ...rest);
  };
}

// Click every card on the board, then every control, and return what is wrong.
// The first card is the alarm (the longest signature): while it plays, read the
// analyzer's time-domain buffer and the scope canvas.
async function playVoiceBoard(page) {
  const problems = [];
  const cards = page.locator("button.card");
  const n = await cards.count();
  if (n < 12) problems.push(`expected the 12 signature cards, found ${n}`);
  if (!n) return problems;
  await cards.nth(0).click();
  await new Promise((r) => setTimeout(r, 300));
  const live = await page.evaluate(() => {
    const a = window.__audio;
    let peak = 0;
    if (a.analyzer) {
      const buf = new Uint8Array(a.analyzer.fftSize);
      a.analyzer.getByteTimeDomainData(buf);
      for (const v of buf) peak = Math.max(peak, Math.abs(v - 128));
    }
    const cv = document.getElementById("scope");
    const px = cv.getContext("2d").getImageData(0, 0, cv.width, cv.height).data;
    const colors = new Set();
    for (let i = 0; i < px.length; i += 4) colors.add((px[i] << 24) | (px[i + 1] << 16) | (px[i + 2] << 8) | px[i + 3]);
    return { peak, colors: colors.size, state: a.analyzer?.context.state || "none" };
  });
  for (let i = 1; i < n; i++) { await cards.nth(i).click(); await new Promise((r) => setTimeout(r, 60)); }
  for (const sel of ["#volseg button", "#nightBtn", "#themebtn"]) {
    const c = page.locator(sel);
    for (let i = 0; i < await c.count(); i++) await c.nth(i).click();
  }
  await new Promise((r) => setTimeout(r, 600));
  const audio = await page.evaluate(() => ({ connects: window.__audio.connects, toDestination: window.__audio.toDestination }));
  if (audio.toDestination < 1) problems.push(`no connect() reached the speakers (${audio.connects} connects, 0 to the destination): the board is silent`);
  if (live.state !== "running") problems.push(`the scope's analyzer node is missing or its context is not running (${live.state})`);
  if (live.peak < 1) problems.push("the analyzer node heard nothing while the alarm played");
  // The cleared canvas plus the grid line is 2 colors; a drawn trace is many more.
  if (live.colors <= 2) problems.push(`the scope drew no trace while the alarm played (${live.colors} colors on the canvas)`);
  if (!problems.length) console.log(`csp_probe: ${VOICE} played ${n} cards: ${audio.connects} connects, ${audio.toDestination} to the destination, analyzer peak ${live.peak}, scope ${live.colors} colors`);
  return problems;
}

const WASM_PAGES = new Set(["eyes.html", "fleet.html", "senselab.html", "smoke.html", "vision.html"]);
const targets = [];
for (const p of pages) {
  if (p === "index.html") targets.push({ name: p, path: `${LAB}/index.html#securacv-canary`, redirect: /\/fleet\.html#securacv-canary$/ });
  else if (p === VOICE) targets.push({ name: p, path: `${LAB}/${p}`, init: countAudioConnects, act: playVoiceBoard });
  else targets.push({ name: p, path: `${LAB}/${p}`, settle: WASM_PAGES.has(p) ? 5000 : 1500 });
}
for (const f of flavors) {
  targets.push({ name: `emulator/web/harness.html?flavor=${f}`, path: `${LAB}/emulator/web/harness.html?hour=10&flavor=${f}`, ready: () => window.__ready === true, settle: 1000 });
}
const run = ONLY ? targets.filter((t) => t.name === ONLY || t.name.startsWith(ONLY)) : targets;
if (!run.length) { console.error(`CSP_PROBE_FAIL: --only ${ONLY} matches no page`); process.exit(1); }
// --list: name what would be opened, one per line, and stop (no browser, no
// server, and no index of the tree: csp.test.js runs this under the
// path-filter recorder, which would hold every file the index stats).
if (process.argv.includes("--list")) {
  for (const t of run) console.log(t.name);
  process.exit(0);
}

const FILES = indexTree(ROOT);
const server = createServer(async (req, res) => {
  try {
    const path = req.url.split("?")[0];
    if (path === "/favicon.ico") { res.writeHead(204); return res.end(); }
    if (path === SELFTEST_PATH) {
      res.writeHead(200, { "content-type": "text/html" });
      return res.end(SELFTEST_HTML);
    }
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
const url = (rel) => `http://127.0.0.1:${port}/${rel}`;

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

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
      for (const x of v) all.push({ frame: f.url().replace(/^http:\/\/127\.0\.0\.1:\d+\//, ""), ...x });
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
  let doc = null;
  let acted = [];
  try {
    if (t.init) await page.addInitScript(t.init);
    await page.goto(url(t.path), { waitUntil: "load", timeout: 60000 });
    if (t.redirect) {
      await page.waitForURL(t.redirect, { timeout: 15000 });
      await page.waitForLoadState("load");
    }
    if (t.ready) await page.waitForFunction(t.ready, null, { timeout: 90000 });
    await page.waitForLoadState("networkidle", { timeout: 20000 }).catch(() => {});
    await new Promise((r) => setTimeout(r, t.settle || 1500));
    hasMeta = await page.evaluate(() => !!document.querySelector('meta[http-equiv="Content-Security-Policy"]'));
    doc = await page.evaluate(() => ({ mode: document.compatMode, charset: document.characterSet }));
    if (t.act) acted = await t.act(page);
    violations = await violationsIn(page);
  } catch (e) {
    errors.push(`did not load: ${e}`);
  }
  await page.close();

  if (errors.length) { fail(t.name, "page errors:\n  " + [...errors.slice(0, 8), ...acted.map((a) => "played wrong: " + a)].join("\n  ")); continue; }
  if (!hasMeta) { fail(t.name, "the loaded document carries no CSP <meta> (gen_csp.py did not run on it, or the redirect landed off the Lab)"); continue; }
  if (doc.mode !== "CSS1Compat" || doc.charset !== "UTF-8") { fail(t.name, `the document is ${doc.mode}, decoded as ${doc.charset} — it needs <!DOCTYPE html> and <meta charset="utf-8">`); continue; }
  if (acted.length) { fail(t.name, "played wrong:\n  " + acted.join("\n  ")); continue; }
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
