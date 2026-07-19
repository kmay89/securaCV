// canary-local/tests/vault_probe.mjs — drive the real Vault explainer in a browser.
//
// Loads vault.html in headless Chromium and exercises the two interactive
// surfaces end to end: the seal walkthrough (play + tamper toggle) and the
// break-glass quorum demo with REAL in-browser Ed25519 — collect enough
// distinct approvals to meet the threshold, break glass, then confirm the
// guardrails hold (single-use refusal + a forged grant with too few approvals
// is denied). Zero page errors.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set), like the other
// probes. Prints VAULT_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
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

const fail = (m) => { console.error("VAULT_PROBE_FAIL:", m); process.exit(1); };

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
const browser = await pw.chromium.launch(
  process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {}
);
const page = await browser.newPage({ viewport: { width: 1200, height: 900 } });
page.on("console", (m) => { if (m.type() === "error") errors.push("console: " + m.text()); });
page.on("pageerror", (e) => errors.push("pageerror: " + String(e)));

try {
  await page.goto(`http://localhost:${port}/canary-local/vault.html`, { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForSelector("#quorum", { timeout: 15000 });

  // every explainer section renders from the JSON
  for (const id of ["concepts", "vault", "sealed", "quorum", "operator", "invariants", "more"])
    if (!(await page.$("#" + id))) fail("missing section #" + id);
  if ((await page.$$eval(".vault-concept", (e) => e.length)) !== 3) fail("expected 3 concept cards");
  if ((await page.$$eval(".vault-invariant", (e) => e.length)) !== 2) fail("expected Invariants I and V");
  if ((await page.$$eval(".vault-hdr-cell", (e) => e.length)) !== 9) fail("expected a 9-field SVLT header");

  // seal walkthrough: play animates the steps; tamper flips the outcome
  await page.click(".vault-seal .primary");
  const stepped = await page.waitForFunction(() => document.querySelector(".vault-step.on"), null, { timeout: 4000 }).catch(() => null);
  if (!stepped) fail("seal walkthrough did not animate");
  await page.click(".vault-tamper button");
  const bad = await page.waitForSelector(".vault-tamper-out.bad", { timeout: 3000 }).catch(() => null);
  if (!bad) fail("tamper toggle did not show the failed-tag state");

  // quorum demo: wait for the (real) trustee keys, then collect approvals
  await page.waitForFunction(
    () => { const b = [...document.querySelectorAll(".vault-approve")]; return b.length === 3 && b.every((x) => !x.disabled); },
    null, { timeout: 15000 }).catch(() => fail("trustee approve buttons never enabled"));

  // a forged grab BEFORE any approvals must be denied
  await page.locator("button", { hasText: "force it open" }).click();
  const denied1 = await page.waitForFunction(
    () => document.querySelector(".vault-receipt.deny"), null, { timeout: 3000 }).catch(() => null);
  if (!denied1) fail("a forced open with zero approvals was not denied");

  // collect 2 of 3 approvals → quorum met
  const approves = await page.$$(".vault-approve");
  await approves[0].click();
  await approves[1].click();
  const ready = await page.waitForFunction(
    () => { const u = document.querySelector(".vault-unseal"); return u && !u.disabled; },
    null, { timeout: 5000 }).catch(() => null);
  if (!ready) fail("quorum was not reached after 2 of 3 approvals");

  // break glass → evidence opens + a GRANTED receipt lands
  await page.click(".vault-unseal");
  const opened = await page.waitForSelector(".vault-evidence.open", { timeout: 4000 }).catch(() => null);
  if (!opened) fail("break-glass did not unseal the evidence");
  const granted = await page.$$eval(".vault-receipt.ok", (e) => e.length);
  if (granted < 1) fail("no GRANTED receipt was logged");

  // single-use: forcing it again is refused
  await page.locator("button", { hasText: "force it open" }).click();
  const refused = await page.waitForFunction(
    () => /Refused/.test(document.querySelector(".vault-q-status")?.textContent || ""),
    null, { timeout: 3000 }).catch(() => null);
  if (!refused) fail("single-use token was not enforced on re-open");

  // distinct-key rule: two clicks on ONE trustee must not fill two slots.
  await page.locator("button", { hasText: "new request" }).click();
  await page.waitForSelector(".vault-evidence.sealed", { timeout: 3000 });
  const a2 = await page.$$(".vault-approve");
  await a2[0].click();
  await a2[0].click(); // same trustee twice
  const stillSealed = await page.evaluate(() => {
    const u = document.querySelector(".vault-unseal");
    return u && u.disabled; // 1 distinct < 2 → still not ready
  });
  if (!stillSealed) fail("a reused key wrongly filled two quorum slots");

  if (errors.length) fail(errors.length + " page/console errors: " + errors.join(" | "));
  console.log("VAULT_PROBE_OK — 3 concepts, 9-field header, real Ed25519 quorum + guardrails");
} finally {
  await browser.close();
  server.close();
}
process.exit(0);
