// canary-local/tests/bench_probe.mjs — CI probe for the physical bench:
// serve the repo, open the watch's device sheet, and work the power plane
// like a hand would — pull USB and ride the battery, flip the switch and
// watch the rail die, restore power and watch the ROM banner + real boot,
// park the ROM in download mode with BOOT+RESET and recover.
//
//   node canary-local/tests/bench_probe.mjs [--shots DIR]
//
// The serial console lives on the Wire tab (the pane is only in the DOM
// while its tab is open), so the probe hops Bench ↔ Wire exactly like a
// person cross-checking the log — which also exercises tab switching.
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
  ".svg": "image/svg+xml", ".png": "image/png", ".glb": "model/gltf-binary",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const shotsIdx = process.argv.indexOf("--shots");
const SHOTS = shotsIdx > 0 ? process.argv[shotsIdx + 1] : null;

// Allowlist, not sanitization (same stance as the sibling probes).
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
await allow("canary-local/enclosures/preview");
await allow("canary-local/emulator/web");
await allow("canary-local/emulator/dist");
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
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));

const fail = async (msg) => {
  if (SHOTS) await page.screenshot({ path: `${SHOTS}/bench_fail.png` }).catch(() => {});
  console.error("BENCH_PROBE_FAIL:", msg);
  process.exit(1);
};
const openTab = async (name) => {
  await page.locator(".tabs .tab", { hasText: name }).first().click();
};
const serial = () =>
  page.evaluate(() => document.querySelector("pre.log")?.textContent || "");
const waitSerial = async (needle, timeout = 90000) => {
  await page
    .waitForFunction(
      (n) => (document.querySelector("pre.log")?.textContent || "").includes(n),
      needle,
      { timeout }
    )
    .catch(async () => fail(`serial never said ${JSON.stringify(needle)}`));
};
const sings = async () =>
  (await serial()).split("The canary is singing").length - 1;
const waitSings = async (above) => {
  await page
    .waitForFunction(
      (n) =>
        (document.querySelector("pre.log")?.textContent || "").split(
          "The canary is singing"
        ).length - 1 > n,
      above,
      { timeout: 90000 }
    )
    .catch(async () => fail(`firmware never booted again (past ${above} boots)`));
};
const chip = (label) => page.locator(".bench-chip", { hasText: label }).first();
const shade = () =>
  page.evaluate(() => {
    const s = document.querySelector(".glass-shade");
    return s?.classList.contains("on") ? s.textContent : null;
  });

// ── Open the watch sheet; let the firmware boot ─────────────────────────
await page.goto(`http://localhost:${port}/canary-local/index.html#canary-display-watch`);
await page.waitForSelector(".tabs .tab", { timeout: 30000 });
await openTab("Wire");
await waitSerial("The canary is singing");
if (!(await serial()).includes("ESP-ROM:esp32s3-20210327"))
  await fail("power-on ROM banner missing before the app's own boot log");

// ── The Bench tab: lights on, app running ───────────────────────────────
await openTab("Bench");
await page.waitForSelector(".bench-led-dot", { timeout: 10000 });
if ((await page.locator(".bench-led-dot.on, .bench-led-dot.flicker").count()) < 1)
  await fail("no LED lit on a powered, charging board");
await page
  .waitForFunction(
    () =>
      [...document.querySelectorAll(".bench-diag dd")].some(
        (d) => d.textContent === "app running"
      ),
    null,
    { timeout: 10000 }
  )
  .catch(() => fail("diagnostics never reported the app running"));

// ── Pull USB: the battery rides through, the firmware never notices ─────
await chip("USB-C cable").click();
await openTab("Wire");
await waitSerial("riding the battery");
if (await shade()) await fail("battery ride-through darkened the glass");

// ── Switch OFF on battery: the rail dies for real ───────────────────────
await openTab("Bench");
await chip("ON/OFF").click();
await page
  .waitForFunction(
    () => document.querySelector(".glass-shade")?.classList.contains("on"),
    null,
    { timeout: 5000 }
  )
  .catch(() => fail("rail down but the glass never went honest-dark"));
await openTab("Wire");
if (!(await serial()).includes("rail down"))
  await fail("rail-down never hit the serial log");

// ── Power restored: ROM banner, then the REAL firmware boots again ──────
const boots1 = await sings();
await openTab("Bench");
await chip("USB-C cable").click();
await openTab("Wire");
await waitSerial("power restored");
await waitSings(boots1);
if (await shade()) await fail("glass still shaded after power restore");

// ── A committed setting survives a bench power-cycle ────────────────────
// (Pins the NVS-preseeds-before-power-on ordering: the restored flash
// must be in place before setup() reads it, or the Character resets.)
await openTab("Try it");
await page.waitForSelector(".style-chip.on", { timeout: 15000 });
const target = page.locator(".style-chip:not(.on)").first();
const targetId = await target.getAttribute("data-id");
await target.click();
await page.waitForTimeout(3500); // COMMIT_DEBOUNCE_MS (2000) + margin
await openTab("Wire");
const boots3 = await sings();
await openTab("Bench");
await chip("USB-C cable").click(); // switch is OFF from earlier: rail dies
await page
  .waitForFunction(
    () => document.querySelector(".glass-shade")?.classList.contains("on"),
    null,
    { timeout: 5000 }
  )
  .catch(() => fail("USB pull with switch OFF should kill the rail"));
await chip("USB-C cable").click(); // power restored → true re-boot
await openTab("Wire");
await waitSings(boots3);
await openTab("Try it");
await page
  .waitForFunction(
    (id) =>
      document
        .querySelector(`.style-chip[data-id="${id}"]`)
        ?.classList.contains("on"),
    targetId,
    { timeout: 15000 }
  )
  .catch(() => fail("Character choice did not survive the bench power-cycle"));

// ── BOOT held + RESET: the ROM parks in download mode ───────────────────
await openTab("Bench");
await chip("BOOT").click();
await chip("RESET").click();
if ((await shade()) !== "waiting for download")
  await fail("download mode not shown on the glass");
await openTab("Wire");
await waitSerial("waiting for download");

// ── Release BOOT + RESET: the app comes back ────────────────────────────
const boots2 = await sings();
await openTab("Bench");
await chip("BOOT").click();
await chip("RESET").click();
await openTab("Wire");
await waitSings(boots2);
await openTab("Bench");
if (await shade()) await fail("glass still shaded after recovery from download mode");

if (SHOTS) await page.screenshot({ path: `${SHOTS}/bench_ok.png` });
await browser.close();
server.close();

if (errors.length) {
  console.error("BENCH_PROBE_FAIL: page errors:\n" + errors.slice(0, 8).join("\n"));
  process.exit(1);
}
console.log("BENCH_PROBE_OK power-pull, ride-through, brown-down, download-mode all behaved");
process.exit(0);
