// canary-local/tests/eyes_probe.mjs — drive the webcam bench in a browser.
//
// Serves the repo and loads eyes.html in headless Chromium, then proves three
// things end to end, with zero page errors:
//   1. The page renders: the standing "not a security device" disclaimer, the
//      version chip identifying the real firmware wasm, and the gated
//      "Start watching" button.
//   2. The COMMITTED WebAssembly presence engine actually decides — a strong
//      person-class box driven straight through the core's ABI in-browser
//      raises presence_started and, on loss, presence_ended; a box under the
//      score threshold raises nothing (the firmware filter holds).
//   3. The camera path wires up: with Chromium's fake video device, the Start
//      button opens the stream and the live bench appears; Stop releases it
//      and returns to idle.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set). Prints
// EYES_PROBE_OK / exits 0 on success.

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

const fail = (m) => { console.error("EYES_PROBE_FAIL:", m); process.exit(1); };

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
const browser = await pw.chromium.launch({
  ...(process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {}),
  args: ["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream"],
});
const page = await browser.newPage({ viewport: { width: 1280, height: 960 } });
page.on("console", (m) => { if (m.type() === "error") errors.push("console: " + m.text()); });
page.on("pageerror", (e) => errors.push("pageerror: " + String(e)));

try {
  await page.goto(`http://localhost:${port}/canary-local/eyes.html`, { waitUntil: "networkidle", timeout: 45000 });

  // ── 1. page renders the safeguards + real-firmware chip + gated button ──
  await page.waitForSelector(".eyes-disclaimer", { timeout: 15000 });
  const disc = await page.$eval(".eyes-disclaimer", (n) => n.textContent);
  if (!/not a security device/i.test(disc))
    fail("standing disclaimer missing: " + disc);

  const chipText = await page.$eval("#eyes-versions", (n) => n.textContent);
  if (!/real firmware wasm/i.test(chipText))
    fail("runtime chip did not identify the real firmware wasm: " + chipText);

  if (!(await page.evaluate(() => typeof globalThis.createCanaryVisionCore === "function")))
    fail("committed Canary Vision core factory did not load");
  if (!(await page.$(".eyes-start"))) fail("Start watching button missing");

  // ── 2. the committed wasm decides presence, and its filter holds ──
  const verdicts = await page.evaluate(async () => {
    const m = await globalThis.createCanaryVisionCore();
    const contract = () => JSON.parse(m.cwrap("vision_emu_contract_json", "string", [])());
    const reset = m.cwrap("vision_emu_reset", null, []);
    const setConfig = m.cwrap("vision_emu_set_config", null,
      ["number", "number", "number", "number"]);
    const begin = m.cwrap("vision_emu_begin_frame", null, []);
    const push = m.cwrap("vision_emu_push_box", "number",
      ["number", "number", "number", "number", "number", "number"]);
    const tick = (now) => JSON.parse(m.cwrap("vision_emu_tick_json", "string", ["number"])(now));

    setConfig(0, 70, 500, 1000);
    const c = contract();
    const events = [];
    const run = (now, boxes) => {
      begin();
      for (const b of boxes) push(b.x, b.y, b.w, b.h, b.score, b.target);
      const t = tick(now);
      if (t.event) events.push(t.event);
      return t;
    };

    reset();
    const person = [{ x: 100, y: 100, w: 40, h: 80, score: 90, target: c.person_target }];
    for (let i = 0; i <= 6; i++) run(i * 100, person);       // present 0.6 s
    for (let i = 7; i <= 14; i++) run(i * 100, []);          // gone past lost timeout

    reset();
    const weakEvents = [];
    for (let i = 0; i <= 6; i++) {
      const t = run(i * 100, [{ x: 100, y: 100, w: 40, h: 80, score: 60, target: c.person_target }]);
      if (t.event) weakEvents.push(t.event);
      if (t.sample.person_now) weakEvents.push("person_now");
    }
    return { events, weakEvents };
  });
  if (verdicts.events[0] !== "presence_started")
    fail("a strong box did NOT raise presence through the browser wasm: " + JSON.stringify(verdicts.events));
  if (!verdicts.events.includes("presence_ended"))
    fail("losing the box did NOT end presence: " + JSON.stringify(verdicts.events));
  if (verdicts.weakEvents.length)
    fail("a below-threshold box WRONGLY registered — the firmware filter failed: " + JSON.stringify(verdicts.weakEvents));

  // ── 3. the camera button opens the stream and the live bench appears ──
  await page.click(".eyes-start");
  await page.waitForSelector(".eyes-live", { timeout: 8000 })
    .catch(() => fail("Start watching did not bring up the live bench (camera path)"));
  if (!(await page.$(".eyes-aim"))) fail("boundary pane missing");
  if (!(await page.$("#eyes-chips"))) fail("verdict chips missing");
  const playing = await page.evaluate(() => {
    const v = document.querySelector(".eyes-stage video");
    return !!v && !!v.srcObject && v.readyState >= 2;
  });
  if (!playing) fail("the fake camera stream did not reach the sensor pane");
  await page.click(".eyes-stop");
  await page.waitForSelector(".eyes-start", { timeout: 5000 })
    .catch(() => fail("Stop did not return to the idle Start button"));
  const released = await page.evaluate(() => {
    const v = document.querySelector(".eyes-stage video");
    return !v || !v.srcObject || v.srcObject.getTracks().every((t) => t.readyState === "ended");
  });
  if (!released) fail("Stop did not release the camera tracks");

  if (errors.length) fail("page errors:\n  " + errors.join("\n  "));
  console.log("EYES_PROBE_OK");
  process.exit(0);
} catch (e) {
  fail(String((e && e.stack) || e));
} finally {
  await browser.close().catch(() => {});
  server.close();
}
