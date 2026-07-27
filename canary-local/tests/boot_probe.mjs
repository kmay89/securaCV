// canary-local/tests/boot_probe.mjs — CI boot test: serve the repo, open
// the harness, and assert the wasm firmware actually boots — framebuffer
// flushes, MQTT round-trips (status out + fleet in), no page errors.
//
//   node canary-local/tests/boot_probe.mjs [--shots DIR]
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set).
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
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

// Allowlist, not sanitization: the probe serves exactly the files the
// harness needs, enumerated up front. Request paths are only ever used
// as lookup KEYS into this map — no user-influenced value reaches the
// filesystem (loopback-only harness, but taint-free beats taint-checked).
const SERVABLE = new Map();
for (const rel of [
  "canary-local/emulator/web/harness.html",
  "canary-local/emulator/web/emu-shell.js",
  "canary-local/emulator/dist/canary-display-watch.js",
  "canary-local/emulator/dist/canary-display-dash.js",
]) {
  SERVABLE.set("/" + rel, join(ROOT, rel));
}

const server = createServer(async (req, res) => {
  const key = decodeURIComponent(req.url.split("?")[0]);
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
const page = await browser.newPage({ viewport: { width: 1100, height: 620 } });
const errors = [];
page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
page.on("pageerror", (e) => errors.push(String(e)));

await page.goto(`http://localhost:${port}/canary-local/emulator/web/harness.html?hour=10`);
await page.waitForFunction("window.__ready === true", null, { timeout: 90000 });
await new Promise((res) => setTimeout(res, 6500)); // splash + face

const st = await page.evaluate(() => ({
  flushes: window.__state.flushes,
  mqtt: window.__state.mqtt,
  serial: window.__state.serialText,
}));

if (SHOTS) await page.screenshot({ path: `${SHOTS}/ci_face.png` });
await browser.close();
server.close();

const fail = (msg) => { console.error("BOOT_PROBE_FAIL:", msg); process.exit(1); };
if (errors.length) fail("page errors:\n" + errors.slice(0, 8).join("\n"));
if (st.flushes < 10) fail(`framebuffer barely flushed (${st.flushes})`);
if (!st.serial.includes("The canary is singing")) fail("boot banner missing from serial");
if (!st.mqtt.some((m) => m.dir === "out" && m.topic.endsWith("/status")))
  fail("display never published its status heartbeat");
if (!st.mqtt.some((m) => m.dir === "in" && m.topic.includes("canary_")))
  fail("fleet payloads never reached the dispatcher");
if (!st.serial.includes("Pinned new witness pubkey"))
  fail("TOFU pinning never happened — trust path broken");
console.log(`BOOT_PROBE_OK flushes=${st.flushes} mqtt=${st.mqtt.length}`);
process.exit(0);
