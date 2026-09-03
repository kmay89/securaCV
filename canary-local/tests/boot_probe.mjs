// canary-local/tests/boot_probe.mjs — CI boot test: serve the repo, open
// the harness, and assert the wasm firmware actually boots — framebuffer
// flushes, MQTT round-trips (status out + fleet in), no page errors.
//
//   node canary-local/tests/boot_probe.mjs [--shots DIR] [--flavor NAME]
//
// Boots EVERY committed display flavor (dist/canary-display-*.js) in turn,
// or just --flavor NAME. Three of the five flavors had never been booted by
// CI before this loop existed — the probe only knew the watch.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set).
import { createServer } from "node:http";
import { readFile, readdir } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".json": "application/json",
  ".css": "text/css", ".wasm": "application/wasm",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const shotsIdx = process.argv.indexOf("--shots");
const SHOTS = shotsIdx > 0 ? process.argv[shotsIdx + 1] : null;
const flavorIdx = process.argv.indexOf("--flavor");
const ONLY = flavorIdx > 0 ? process.argv[flavorIdx + 1] : null;

// The flavors are whatever dist/ holds: the committed bundles are the
// truth about what a bare checkout can boot, so the probe reads the
// directory instead of keeping its own list to drift.
const DIST = join(ROOT, "canary-local/emulator/dist");
const FLAVORS = (await readdir(DIST))
  .map((f) => /^canary-display-([a-z0-9]+)\.js$/.exec(f)?.[1])
  .filter(Boolean)
  .sort();
if (!FLAVORS.length) { console.error("BOOT_PROBE_FAIL: no canary-display-*.js in dist/"); process.exit(1); }
if (ONLY && !FLAVORS.includes(ONLY)) {
  console.error(`BOOT_PROBE_FAIL: no dist bundle for --flavor ${ONLY} (have: ${FLAVORS.join(", ")})`);
  process.exit(1);
}
const RUN = ONLY ? [ONLY] : FLAVORS;

// Allowlist, not sanitization: the probe serves exactly the files the
// harness needs, enumerated up front. Request paths are only ever used
// as lookup KEYS into this map — no user-influenced value reaches the
// filesystem (loopback-only harness, but taint-free beats taint-checked).
const SERVABLE = new Map();
for (const rel of [
  "canary-local/emulator/web/harness.html",
  "canary-local/emulator/web/harness.js",
  "canary-local/emulator/web/harness.css",
  "canary-local/emulator/web/emu-shell.js",
  ...FLAVORS.map((f) => `canary-local/emulator/dist/canary-display-${f}.js`),
]) {
  SERVABLE.set("/" + rel, join(ROOT, rel));
}

const server = createServer(async (req, res) => {
  const key = decodeURIComponent(req.url.split("?")[0]);
  // Chromium asks for a favicon on its own; answering "no content" keeps
  // that off the page's error console (a 404 there fails the probe, and it
  // is not the firmware's doing).
  if (key === "/favicon.ico") { res.writeHead(204); res.end(); return; }
  const path = SERVABLE.get(key);
  if (!path) {
    // Name the miss: a flavor that asks for something outside the allowlist
    // fails the probe, and "404" alone does not say what it wanted.
    console.error(`boot_probe: 404 for ${key} (not in the allowlist)`);
    res.writeHead(404); res.end(); return;
  }
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
const failures = [];
const fail = (flavor, msg) => { console.error(`BOOT_PROBE_FAIL[${flavor}]:`, msg); failures.push(flavor); };

for (const flavor of RUN) {
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));

  let st = null;
  try {
    await page.goto(`http://localhost:${port}/canary-local/emulator/web/harness.html?hour=10&flavor=${flavor}`);
    await page.waitForFunction("window.__ready === true", null, { timeout: 90000 });
    await new Promise((res) => setTimeout(res, 6500)); // splash + face
    st = await page.evaluate(() => ({
      flushes: window.__state.flushes,
      mqtt: window.__state.mqtt,
      serial: window.__state.serialText,
    }));
    if (SHOTS) await page.screenshot({ path: `${SHOTS}/ci_face_${flavor}.png` });
  } catch (e) {
    errors.push(`did not reach ready: ${e}`);
  }
  await page.close();

  if (errors.length) { fail(flavor, "page errors:\n" + errors.slice(0, 8).join("\n")); continue; }
  if (st.flushes < 10) { fail(flavor, `framebuffer barely flushed (${st.flushes})`); continue; }
  if (!st.serial.includes("The canary is singing")) { fail(flavor, "boot banner missing from serial"); continue; }
  if (!st.mqtt.some((m) => m.dir === "out" && m.topic.endsWith("/status")))
    { fail(flavor, "display never published its status heartbeat"); continue; }
  if (!st.mqtt.some((m) => m.dir === "in" && m.topic.includes("canary_")))
    { fail(flavor, "fleet payloads never reached the dispatcher"); continue; }
  if (!st.serial.includes("Pinned new witness pubkey"))
    { fail(flavor, "TOFU pinning never happened — trust path broken"); continue; }
  console.log(`BOOT_PROBE_OK[${flavor}] flushes=${st.flushes} mqtt=${st.mqtt.length}`);
}

await browser.close();
server.close();
if (failures.length) {
  console.error(`BOOT_PROBE_FAIL: ${failures.length} of ${RUN.length} flavors failed (${failures.join(", ")})`);
  process.exit(1);
}
console.log(`BOOT_PROBE_OK all ${RUN.length} flavors booted: ${RUN.join(", ")}`);
process.exit(0);
