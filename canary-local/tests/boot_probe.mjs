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
// Then boots each turned glass (F206) whose flavor it booted: the dash with a
// saved portrait rotation and the nightlight with a saved landscape one
// (?rotation=, staged before power-on; turned_glass.mjs reads the turns and
// panels from the sources). There the same checks hold, and the glass is the
// turned size, every frame the firmware drew landed on it from the first
// (framesOnGlass), and the face's bird is on stage (a turned face at 10:00
// always shows it; birdOnStage) and sits on that glass and clear of every
// line (birdPerch). On a native boot the bird is held where it is on stage
// (the landscape dash's face hides it).
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set).
import { createServer } from "node:http";
import { readFile, readdir } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { birdPerch, birdOnStage } from "./bird_perch.mjs";
import { framesOnGlass } from "./onboard_glass.mjs";
import { turnedGlasses, readTurnedSources } from "./turned_glass.mjs";

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
// F206: the turned glasses, for the flavors this run boots.
const TURNED = turnedGlasses(await readTurnedSources(ROOT, readFile)).filter((t) => RUN.includes(t.flavor));

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

// One boot: the harness for `flavor` (with a saved rotation when `turn` is
// given), the face read 6.5 s on, every check held. Returns the run's name.
async function bootOnce(flavor, turn = null) {
  const name = turn ? `${flavor}@${turn.name}` : flavor;
  const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));

  let st = null;
  try {
    const turnArg = turn ? `&rotation=${turn.rotation}` : "";
    await page.goto(`http://localhost:${port}/canary-local/emulator/web/harness.html?hour=10&flavor=${flavor}${turnArg}`);
    // A function, never a string: Playwright re-evaluates a string predicate
    // through eval on every animation frame, which harness.html's policy
    // (no 'unsafe-eval') refuses whenever the wasm is not ready at the first
    // poll. A function is compiled once, inside the DevTools call (sweep A44).
    await page.waitForFunction(() => window.__ready === true || window.__harnessError, null, { timeout: 90000 });
    const harnessError = await page.evaluate(() => window.__harnessError || null);
    if (harnessError) throw new Error(`the harness did not boot: ${harnessError}`);
    await new Promise((res) => setTimeout(res, 6500)); // splash + face
    st = await page.evaluate(async () => ({
      flushes: window.__state.flushes,
      shapes: window.__state.shapes,
      mqtt: window.__state.mqtt,
      serial: window.__state.serialText,
      bird: await window.__emu.markBox(),
      labels: await window.__emu.screenLabels(),
      glass: { w: document.getElementById("glass").width, h: document.getElementById("glass").height },
    }));
    if (SHOTS) await page.screenshot({ path: `${SHOTS}/ci_face_${name}.png` });
  } catch (e) {
    errors.push(`did not reach ready: ${e}`);
  }
  await page.close();

  if (errors.length) { fail(name, "page errors:\n" + errors.slice(0, 8).join("\n")); return; }
  if (st.flushes < 10) { fail(name, `framebuffer barely flushed (${st.flushes})`); return; }
  if (!st.serial.includes("The canary is singing")) { fail(name, "boot banner missing from serial"); return; }
  if (!st.mqtt.some((m) => m.dir === "out" && m.topic.endsWith("/status")))
    { fail(name, "display never published its status heartbeat"); return; }
  if (!st.mqtt.some((m) => m.dir === "in" && m.topic.includes("canary_")))
    { fail(name, "fleet payloads never reached the dispatcher"); return; }
  if (!st.serial.includes("Pinned new witness pubkey"))
    { fail(name, "TOFU pinning never happened — trust path broken"); return; }
  if (turn) {
    // F206: booted with its saved rotation, the glass is the turned one, and
    // the firmware drew every frame on it from the first (main.cpp turns the
    // glass before the splash).
    if (st.glass.w !== turn.glass.w || st.glass.h !== turn.glass.h) {
      fail(name, `booted with saved rotation ${turn.rotation}, the ${flavor} glass is ${st.glass.w}x${st.glass.h}, ` +
        `not the ${turn.name} ${turn.glass.w}x${turn.glass.h} main.cpp turns it to (F206)`);
      return;
    }
    const frames = framesOnGlass(st.shapes, st.flushes, turn.glass);
    if (frames) { fail(name, frames); return; }
    // and the face's bird is there to read: birdPerch passes a bird off stage
    const stage = birdOnStage(st, `the ${turn.name} ${flavor} face 6.5 s on`);
    if (stage) { fail(name, stage); return; }
  }
  const perch = birdPerch(st);
  if (perch) { fail(name, perch); return; }
  console.log(`BOOT_PROBE_OK[${name}] flushes=${st.flushes} mqtt=${st.mqtt.length}` +
    (turn ? ` glass=${st.glass.w}x${st.glass.h} shapes=${st.shapes.length} bird=${st.bird.w}x${st.bird.h}@${st.bird.x},${st.bird.y}` : ""));
}

const booted = [];
for (const flavor of RUN) { await bootOnce(flavor); booted.push(flavor); }
for (const t of TURNED) { await bootOnce(t.flavor, t); booted.push(`${t.flavor}@${t.name}`); }

await browser.close();
server.close();
if (failures.length) {
  console.error(`BOOT_PROBE_FAIL: ${failures.length} of ${booted.length} boots failed (${failures.join(", ")})`);
  process.exit(1);
}
console.log(`BOOT_PROBE_OK all ${booted.length} boots: ${booted.join(", ")}`);
process.exit(0);
