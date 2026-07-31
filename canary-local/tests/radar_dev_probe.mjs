// canary-local/tests/radar_dev_probe.mjs — drive the real Proving Ground in a browser.
//
// Serves the repo and loads radar-dev.html in headless Chromium, then walks
// the page the way a user without a board would: sections render from the
// drift-gated JSON, the twin boots and speaks the firmware's banner + [cfg],
// the knobs sync, the scene levers drive presence through the real pipeline,
// the auto drills all pass against the twin, and the placement scorer grades
// a spot — zero page errors.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set), same as the
// other probes. Prints RADAR_DEV_PROBE_OK / exits 0 on success.

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

const fail = (m) => { console.error("RADAR_DEV_PROBE_FAIL:", m); process.exit(1); };

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
  await page.goto(`http://localhost:${port}/canary-local/radar-dev.html`, { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForSelector("#placement", { timeout: 15000 });

  // all sections render from the JSON
  for (const id of ["journey", "bench", "drills", "calibrate", "placement", "more"])
    if (!(await page.$("#" + id))) fail("missing section #" + id);

  // version strip built from sense.json
  const chips = await page.$$eval("#radar-dev-versions .chip", (e) => e.length);
  if (chips < 4) fail("version strip thin (" + chips + " chips)");

  // journey: five steps, none done yet, connect controls offered
  const steps = await page.$$eval(".rdev-step", (e) => e.length);
  if (steps !== 5) fail("expected 5 journey steps, got " + steps);

  // boot the wellbeing twin
  const twinBtn = await page.$$(".rdev-connect button");
  let boot = null;
  for (const b of twinBtn) if (/wellbeing/.test(await b.textContent())) boot = b;
  if (!boot) fail("no wellbeing twin button");
  await boot.click();

  // the twin's boot banner (the firmware's own, drift-gated) hits the console
  await page.waitForFunction(
    () => /SecuraCV Canary Sense/.test(document.querySelector(".flash-sense-log")?.textContent || ""),
    null, { timeout: 10000 }).catch(() => fail("the firmware banner never played"));

  // the [cfg] handshake syncs the knobs — wellbeing carries all 11
  await page.waitForFunction(
    () => [...document.querySelectorAll(".flash-sense-knob input")].filter((i) => !i.disabled).length >= 11,
    null, { timeout: 10000 }).catch(() => fail("knobs never synced from [cfg]"));

  // the TWIN badge is honest about what's on the cable
  const badge = await page.$eval(".rdev-bench-head .flash-passport-chip", (n) => n.textContent);
  if (!/TWIN/.test(badge)) fail("source badge does not say TWIN: " + badge);

  // scene lever: walk in at 2.5 m → the real pipeline flips presence
  await page.evaluate(() => {
    const dist = document.querySelector(".rdev-scene input[type=range]");
    dist.value = "2.5";
    dist.dispatchEvent(new Event("input", { bubbles: true }));
  });
  const movingBtn = (await page.$$(".rdev-scene-toggles button"))[0];
  await movingBtn.click();
  await page.waitForFunction(
    () => /someone's here/.test(document.querySelector(".flash-sense-status")?.textContent || ""),
    null, { timeout: 10000 }).catch(() => fail("walking the twin in never flipped presence"));

  // the [radar] stream speaks the firmware dialect on the console
  await page.waitForFunction(
    () => /\[radar\] state=present count=1 range=/.test(document.querySelector(".flash-sense-log")?.textContent || ""),
    null, { timeout: 10000 }).catch(() => fail("no [radar] stream line on the console"));

  // run the auto drills — all four must pass against the twin
  const runAuto = await page.$$(".rdev-drill-sum button");
  if (!runAuto.length) fail("no run-auto button");
  await runAuto[0].click();
  await page.waitForFunction(
    () => [...document.querySelectorAll(".rdev-drill-state.rdev-pass")].length >= 4,
    null, { timeout: 40000 }).catch(() => fail("the auto drills did not all pass on the twin"));

  // the drills honestly report their count
  const sum = await page.$eval(".rdev-drill-sum", (n) => n.textContent);
  if (!/4 passed/.test(sum)) fail("drill summary wrong: " + sum);

  // placement scorer: grades render and react
  const grade = await page.$eval(".rdev-grade", (n) => n.textContent);
  if (!/^[ABCD]$/.test(grade)) fail("placement grade missing: " + grade);
  const mounts = await page.$$eval(".rdev-mount", (e) => e.length);
  if (mounts !== 3) fail("expected the 3 blessed mounts, got " + mounts);
  const avoid = await page.$$eval(".rdev-avoid .wap-ent", (e) => e.length);
  if (avoid < 5) fail("keep-out list thin (" + avoid + ")");

  // calibration: the wizard renders its steps and presets
  const calSteps = await page.$$eval("#calibrate .we2-guide-steps li", (e) => e.length);
  if (calSteps < 4) fail("calibration steps thin (" + calSteps + ")");

  if (errors.length) fail(errors.length + " page/console errors: " + errors.join(" | "));
  console.log(`RADAR_DEV_PROBE_OK — ${chips} chips, ${steps} steps, ${mounts} mounts, auto drills green`);
} finally {
  await browser.close();
  server.close();
}
process.exit(0);
