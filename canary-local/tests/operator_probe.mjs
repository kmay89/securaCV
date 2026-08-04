// canary-local/tests/operator_probe.mjs — drive the Operator's Bench in a browser.
//
// Loads operator.html in headless Chromium and walks the setup ceremony end to
// end: step through init → enroll×3 → drill → doctor and confirm the live "vault
// state" tracks the code's real behavior — the committed policy stays a draft
// below the threshold, goes live as 2-of-2 the instant it's valid, strengthens
// to 2-of-3, and the drill/doctor output renders. Zero page errors.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set), like the other
// probes. Prints OPERATOR_PROBE_OK / exits 0 on success.

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

const fail = (m) => { console.error("OPERATOR_PROBE_FAIL:", m); process.exit(1); };

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

const badge = () => page.$eval(".op-badge", (e) => e.textContent).catch(() => "");
const lit = () => page.$$eval(".op-tr.on", (e) => e.length);
const term = () => page.$eval(".op-term", (e) => e.textContent);
const wait = (fn, msg) => page.waitForFunction(fn, null, { timeout: 3000 }).catch(() => fail(msg));

try {
  await page.goto(`http://localhost:${port}/canary-local/operator.html`, { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForSelector("#ceremony", { timeout: 15000 });

  // every section renders from the JSON
  for (const id of ["commands", "concepts", "ceremony", "doctor", "drill", "keep-going"])
    if (!(await page.$("#" + id))) fail("missing section #" + id);
  if ((await page.$$eval(".op-cmd", (e) => e.length)) !== 4) fail("expected 4 command cards");
  if ((await page.$$eval(".op-dot", (e) => e.length)) !== 6) fail("expected 6 ceremony step dots");

  // step 0 (init): a draft, no policy, no enrolled trustees
  if (!(await page.$(".op-badge-draft"))) fail("step 0 should show a draft (no-policy) badge");
  if ((await lit()) !== 0) fail("step 0 should have no enrolled trustees");

  const next = page.locator(".op-btn", { hasText: "Next" });

  // step 1 (alice): 1 enrolled, still below the threshold → still a draft
  await next.click();
  if ((await lit()) !== 1) fail("step 1 should show 1 enrolled trustee");
  if (!/draft/.test(await badge())) fail("step 1 (below threshold) should still be a draft");

  // step 2 (bob): threshold reached → policy goes live as 2-of-2
  await next.click();
  await wait(() => /live · 2-of-2/.test(document.querySelector(".op-badge")?.textContent || ""),
    "policy did not go live 2-of-2 at the threshold");
  if ((await lit()) !== 2) fail("step 2 should show 2 enrolled trustees");

  // step 3 (carol, minted): strengthens to 2-of-3, roster full
  await next.click();
  await wait(() => /live · 2-of-3/.test(document.querySelector(".op-badge")?.textContent || ""),
    "policy did not strengthen to 2-of-3");
  if ((await lit()) !== 3) fail("step 3 should show all 3 trustees enrolled");

  // step 4 (drill): the sandbox rehearsal output renders
  await next.click();
  await wait(() => /DRILL PASSED/.test(document.querySelector(".op-term")?.textContent || ""),
    "drill step did not show DRILL PASSED");

  // step 5 (doctor): HEALTHY renders; Next is now disabled (last step)
  await next.click();
  await wait(() => /Result: HEALTHY/.test(document.querySelector(".op-term")?.textContent || ""),
    "doctor step did not show HEALTHY");
  if (!(await page.$eval(".op-btn-primary", (e) => e.disabled))) fail("Next should be disabled at the last step");

  // Prev returns to the drill step
  await page.locator(".op-btn", { hasText: "Prev" }).click();
  if (!/DRILL PASSED/.test(await term())) fail("Prev did not return to the drill step");

  if (errors.length) fail(errors.length + " page/console errors: " + errors.join(" | "));
  console.log("OPERATOR_PROBE_OK — 4 commands, 6-step ceremony; policy draft → live 2-of-2 → 2-of-3, drill + doctor render");
} finally {
  await browser.close();
  server.close();
}
process.exit(0);
