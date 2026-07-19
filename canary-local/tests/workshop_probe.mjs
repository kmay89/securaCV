// canary-local/tests/workshop_probe.mjs — CI probe for the Workshop:
// serve the repo, open workshop.html, and drive the configurator like a
// person would — pick packages, tick GPS, watch the honesty ribbon flip,
// check the checklist names the real part and flag, walk every stage.
//
//   node canary-local/tests/workshop_probe.mjs [--shots DIR]
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set).
import { createServer } from "node:http";
import { readFile, readdir } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".json": "application/json",
  ".css": "text/css", ".stl": "application/octet-stream",
  ".svg": "image/svg+xml", ".png": "image/png",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const shotsIdx = process.argv.indexOf("--shots");
const SHOTS = shotsIdx > 0 ? process.argv[shotsIdx + 1] : null;

// Allowlist, not sanitization (same stance as boot_probe): enumerate the
// exact directories the page may draw from, once, up front. Request paths
// are only ever KEYS into this map.
const SERVABLE = new Map();
async function allow(dirRel) {
  for (const f of await readdir(join(ROOT, dirRel))) {
    if (extname(f) in MIME) {
      SERVABLE.set(`/${dirRel}/${f}`, join(ROOT, dirRel, f));
    }
  }
}
await allow("canary-local");
await allow("canary-local/assets");
await allow("canary-local/devices");
await allow("canary-local/enclosures/preview");
await allow("docs/hardware/enclosure");

const server = createServer(async (req, res) => {
  const key = decodeURIComponent(req.url.split("?")[0].split("#")[0]);
  const path = SERVABLE.get(key);
  if (!path) { res.writeHead(404); res.end(); return; }
  try {
    const data = await readFile(path);
    res.writeHead(200, { "content-type": MIME[extname(path)] || "application/octet-stream" });
    res.end(data);
  } catch { res.writeHead(404); res.end(); }
}).listen(0);
const port = server.address().port;

const browser = await pw.chromium.launch(
  process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {}
);
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
const errors = [];
page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
page.on("pageerror", (e) => errors.push(String(e)));

const fail = (msg) => { console.error("WORKSHOP_PROBE_FAIL:", msg); process.exit(1); };
const step = (m) => console.log("· " + m);

step("goto");
await page.goto(`http://127.0.0.1:${port}/canary-local/workshop.html#canary-wap`);
await page.waitForSelector(".ws-pkg", { timeout: 15000 });

// ── configure: packages present, default preset exact ──
const pkgs = await page.locator(".ws-pkg").count();
if (pkgs < 3) fail(`expected ≥3 wap packages, saw ${pkgs}`);
const ribbon = page.locator(".ws-ribbon");
if (!/print-validated/.test(await ribbon.textContent())) {
  fail("default config should match a print-validated preset");
}

step("packages ok");
// ── the 3D viewport actually holds meshes ──
await page.waitForFunction(() => {
  const cv = document.querySelector(".ws-canvas");
  return cv && cv.__scene && cv.__scene.parts.length >= 2;
}, { timeout: 15000 });

step("meshes in scene");
// ── honesty ribbon: deviate one option → custom; restore → exact ──
const gps = page.locator(".ws-opt", { hasText: "GPS" }).locator("input");
await gps.uncheck();
if (!/custom combo/.test(await ribbon.textContent())) {
  fail("unticking GPS must flip the ribbon to custom");
}
await gps.check();
if (!/print-validated/.test(await ribbon.textContent())) {
  fail("restoring GPS must return to the exact preset");
}

step("ribbon flips");
// ── checklist speaks the real BOM + firmware language ──
const checkText = await page.locator(".ws-check").textContent();
for (const needle of ["M1", "FEATURE_GNSS", "BT1"]) {
  if (!checkText.includes(needle)) fail(`checklist missing "${needle}"`);
}
// …and the options column tells the geometric truth from the scad
const optText = await page.locator(".ws-optgroup").allTextContents();
if (!/battery bay/.test(optText.join(" "))) {
  fail("options missing the scad's own consequence text (battery bay)");
}
if (SHOTS) await page.screenshot({ path: `${SHOTS}/workshop_configure.png`, fullPage: true });

step("checklist ok");
// ── weather package unlocks the solar/thermal addon ──
await page.locator(".ws-pkg", { hasText: "battery weather" }).click();
const addon = page.locator(".ws-addon input");
if (await addon.isDisabled()) fail("weather package should unlock the solar kit addon");
await addon.check();
await page.waitForFunction(() => {
  const cv = document.querySelector(".ws-canvas");
  return cv && cv.__scene && cv.__scene.parts.length >= 4;
}, { timeout: 15000 });

step("addon meshes ok");
// ── dreaming mode hides the deep sheet ──
await page.locator(".ws-mode .pill", { hasText: "dreaming" }).click();
if (await page.locator(".ws-check .ws-ref").first().isVisible().catch(() => false)) {
  fail("dreaming mode must hide RefDes chips");
}
await page.locator(".ws-mode .pill", { hasText: "building" }).click();

step("modes ok");
// ── walk the journey ──
const stage = async (label) => {
  await page.locator(".ws-stagebtn", { hasText: label }).click();
  return page.locator(".ws-stagebody").textContent();
};
const printT = await stage("Print plan");
if (!/Print this first/.test(printT)) fail("print plan missing the coupon-first call");
if (!/TPU/.test(printT)) fail("print plan missing the gasket TPU note");

const gatherT = await stage("Gather parts");
if (!/\$\d/.test(gatherT)) fail("gather stage missing a priced total");
if (!/weather kit/i.test(gatherT)) fail("gather stage missing the BOM's own recipes");

const asmT = await stage("Assemble");
if (!/100% scale|calibration/i.test(asmT)) fail("assemble missing the template calibration note");
const tplImgs = await page.locator(".ws-template img").count();
if (tplImgs < 1) fail("assemble missing drill template previews");

const flashT = await stage("Flash & setup");
if (!/FEATURE_GNSS/.test(flashT)) fail("flash stage missing the implied GNSS flag");

const cardT = await stage("Your build");
if (!/your spec/i.test(cardT)) fail("build card missing");
if (!/\$\d/.test(cardT)) fail("build card missing the parts budget");
if (SHOTS) await page.screenshot({ path: `${SHOTS}/workshop_card.png`, fullPage: true });

step("journey ok");
// ── a second device renders too ──
await page.locator(".ws-device", { hasText: "Sense" }).click();
await page.waitForSelector(".ws-pkg");
const senseCheck = await page.locator(".ws-check").textContent();
if (!/FEATURE_STATUS_LED|radome|part/i.test(senseCheck)) {
  fail("sense configurator did not render");
}

const benign = errors.filter((e) => !/favicon/.test(e));
if (benign.length) fail(`page errors: ${benign.join(" | ")}`);

await browser.close();
server.close();
console.log("WORKSHOP_PROBE_OK packages=" + pkgs);
process.exit(0);
