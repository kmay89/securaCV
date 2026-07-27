// canary-local/tests/boardroom_probe.mjs — CI probe for the Board Room:
// serve the repo, open boards.html, and use it like a person would — spin
// through every board, check the vendor mesh actually lands in the scene and
// the pin flags hang where the catalog says, flip on "every pad", then walk
// the Wire-it harness step by step and make sure every wire is really there.
//
//   node canary-local/tests/boardroom_probe.mjs [--shots DIR]
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set).
import { createServer } from "node:http";
import { readFile, readdir } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".json": "application/json",
  ".css": "text/css", ".glb": "model/gltf-binary",
  ".svg": "image/svg+xml", ".png": "image/png",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const shotsIdx = process.argv.indexOf("--shots");
const SHOTS = shotsIdx > 0 ? process.argv[shotsIdx + 1] : null;

// Allowlist, not sanitization (same stance as the other probes).
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
await allow("canary-local/boards");

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
page.on("console", (m) => {
  // the favicon 404 is browser boilerplate, not the page (same stance as
  // workshop_probe) — the URL rides in location(), not always in text()
  if (m.type() !== "error") return;
  if (/favicon/.test(m.text()) || /favicon/.test(m.location()?.url || "")) return;
  errors.push(m.text());
});
page.on("pageerror", (e) => errors.push(String(e)));

const fail = (msg) => { console.error("BOARDROOM_PROBE_FAIL:", msg); process.exit(1); };
const step = (m) => console.log("· " + m);

const boards = JSON.parse(await readFile(join(ROOT, "canary-local/devices/boards.json"), "utf8"));
const wiring = JSON.parse(await readFile(join(ROOT, "canary-local/devices/wiring.json"), "utf8"));
const boardCount = Object.keys(boards.boards).length;

step("goto");
await page.goto(`http://127.0.0.1:${port}/canary-local/boards.html`);
await page.waitForSelector(".broom .pill", { timeout: 15000 });

// ── every board gets a pill; the first one auto-loads its real mesh ──
const pills = await page.locator(".broom .pill").count();
if (pills !== boardCount) fail(`expected ${boardCount} board pills, saw ${pills}`);
await page.waitForFunction(() => {
  const cv = document.querySelector(".broom-3d");
  return cv && cv.__scene && cv.__scene.parts.length >= 5;
}, { timeout: 15000 });

step("mesh in scene");
// ── pin flags hang off the pinout's own anchors ──
await page.waitForFunction(() => document.querySelectorAll(".pin-flag").length >= 5,
  { timeout: 15000 });
const flagText = await page.locator(".broom-overlay").textContent();
for (const needle of ["D1", "D6", "D7", "BAT+"]) {
  if (!flagText.includes(needle)) fail(`missing pin flag "${needle}"`);
}
// planned pins wear the tag in the table
const tableText = await page.locator(".pin-table").textContent();
for (const needle of ["Chirp / buzzer", "GPIO2", "planned"]) {
  if (!tableText.includes(needle)) fail(`pin table missing "${needle}"`);
}

step("flags + table speak");
// ── "every pad": the full castellation map appears ──
const flagsBefore = await page.locator(".pin-flag").count();
await page.locator(".broom-pads-toggle input").check();
await page.waitForFunction((n) => document.querySelectorAll(".pin-flag").length > n,
  flagsBefore, { timeout: 5000 });
const allPads = await page.locator(".broom-overlay").textContent();
for (const needle of ["D8", "D9", "D10"]) {
  if (!allPads.includes(needle)) fail(`"every pad" missing ${needle}`);
}
await page.locator(".broom-pads-toggle input").uncheck();
if (SHOTS) await page.screenshot({ path: `${SHOTS}/boardroom_board.png`, fullPage: true });

step("every-pad toggle");
// ── wire it: the harness lands, step player walks it ──
const build = wiring.builds.find((b) => b.board === Object.keys(boards.boards)[0])
  || wiring.builds[0];
await page.locator(".broom-modes .tab", { hasText: "wire it" }).click();
await page.waitForSelector(".wire-step-card", { timeout: 15000 });
const cards = await page.locator(".wire-step-card").count();
if (cards !== build.steps.length) fail(`expected ${build.steps.length} step cards, saw ${cards}`);
const wireInfo = await page.locator(".broom-info").textContent();
for (const conn of build.connections) {
  if (!wireInfo.includes(conn.to)) fail(`wire list missing pad ${conn.to}`);
}
// scene now holds board + peripherals + one line part per connection
await page.waitForFunction((min) => {
  const cv = document.querySelector(".broom-3d");
  return cv && cv.__scene && cv.__scene.parts.length >= min;
}, build.connections.length + 5, { timeout: 15000 });

step("harness in scene");
// step player: walk to step 1 and back to the whole harness
await page.locator(".wire-nav .primary", { hasText: "next" }).click();
await page.waitForFunction(() =>
  document.querySelector(".wire-counter")?.textContent.includes("step 1"),
  { timeout: 5000 });
if (!(await page.locator(".wire-step-card.on h5").textContent()).includes(build.steps[0].title)) {
  fail("step 1 card not active after next");
}
await page.locator(".wire-nav .ghost", { hasText: "whole harness" }).click();
if (SHOTS) await page.screenshot({ path: `${SHOTS}/boardroom_wire.png`, fullPage: true });

step("step player");
// ── the other boards load too; boards without a harness disable wire-it ──
for (const [bid, b] of Object.entries(boards.boards).slice(1)) {
  // exact-match the pill: "Seeed XIAO ESP32-S3" is a substring of the Sense's
  // "…S3 Sense", so a hasText substring match would resolve to two pills
  const exact = new RegExp("^" + b.name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "$");
  await page.locator(".broom .pill", { hasText: exact }).click();
  await page.waitForFunction(() => {
    const cv = document.querySelector(".broom-3d");
    return cv && cv.__scene && cv.__scene.parts.length >= 3;
  }, { timeout: 15000 });
  const hasBuild = wiring.builds.some((w) => w.board === bid);
  const disabled = await page.locator(".broom-modes .tab", { hasText: "wire it" }).isDisabled();
  if (disabled === hasBuild) fail(`${bid}: wire-it disabled=${disabled} but harness exists=${hasBuild}`);
}

step("all boards load");
if (errors.length) fail("console errors:\n" + errors.join("\n"));
console.log("BOARDROOM_PROBE_OK");
await browser.close();
server.close();
