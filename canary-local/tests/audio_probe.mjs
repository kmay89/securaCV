// canary-local/tests/audio_probe.mjs — drive the smoke-alarm bench in a browser.
//
// Serves the repo and loads smoke.html in headless Chromium, then proves three
// things end to end, with zero page errors:
//   1. The page renders: the standing "not a life-safety device" disclaimer,
//      the version chip identifying the real firmware wasm, and the gated
//      "Start listening" button.
//   2. The COMMITTED WebAssembly detector actually detects — a synthesized
//      NFPA-72 T3 cadence driven straight through the core's ABI in-browser
//      raises the smoke event, and a correctly-timed OFF-BAND rhythm does not
//      (the 3.4 kHz tone gate holds).
//   3. The microphone path wires up: with Chromium's fake audio device, the
//      Start button opens the stream and the live bench appears; Stop releases
//      it and returns to idle.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set). Prints
// AUDIO_PROBE_OK / exits 0 on success.

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

const fail = (m) => { console.error("AUDIO_PROBE_FAIL:", m); process.exit(1); };

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
const page = await browser.newPage({ viewport: { width: 1200, height: 900 } });
page.on("console", (m) => { if (m.type() === "error") errors.push("console: " + m.text()); });
page.on("pageerror", (e) => errors.push("pageerror: " + String(e)));

try {
  await page.goto(`http://localhost:${port}/canary-local/smoke.html`, { waitUntil: "networkidle", timeout: 45000 });

  // ── 1. page renders the safeguards + real-firmware chip + gated button ──
  await page.waitForSelector(".smoke-disclaimer", { timeout: 15000 });
  const disc = await page.$eval(".smoke-disclaimer", (n) => n.textContent);
  if (!/not (a smoke|an alarm)/i.test(disc) && !/not a smoke or CO/i.test(disc))
    fail("standing life-safety disclaimer missing: " + disc);

  const chipText = await page.$eval("#smoke-versions", (n) => n.textContent);
  if (!/real firmware wasm/i.test(chipText))
    fail("runtime chip did not identify the real firmware wasm: " + chipText);

  if (!(await page.evaluate(() => typeof globalThis.createCanaryAudioCore === "function")))
    fail("committed Canary WAP acoustic core factory did not load");
  if (!(await page.$(".smoke-start"))) fail("Start listening button missing");

  // ── 2. the committed wasm detects a synthesized T3, rejects off-band ──
  const detect = await page.evaluate(async () => {
    const m = await globalThis.createCanaryAudioCore();
    const reset = m.cwrap("audio_emu_reset", null, []);
    const N = m.cwrap("audio_emu_frame_samples", "number", [])();
    const framePtr = m.cwrap("audio_emu_frame_ptr", "number", []);
    const proc = m.cwrap("audio_emu_process_frame", "string", []);
    let ph = 0;
    const emit = (hz, nf, flags) => {
      for (let f = 0; f < nf; f++) {
        const view = new Int16Array(m.HEAP16.buffer, framePtr(), N);
        for (let i = 0; i < N; i++) {
          if (hz <= 0) { view[i] = 0; continue; }
          ph += 2 * Math.PI * hz / 16000; if (ph > 2 * Math.PI) ph -= 2 * Math.PI;
          view[i] = Math.round(8000 * Math.sin(ph));
        }
        const j = JSON.parse(proc());
        if (j.event === 1) flags.t3 = true;
        if (j.event === 2) flags.t4 = true;
      }
    };
    const inband = { t3: false, t4: false };
    reset(); ph = 0;
    emit(0, 30);
    for (let c = 0; c < 2; c++) { emit(3400, 25, inband); emit(0, 25, inband); emit(3400, 25, inband); emit(0, 25, inband); emit(3400, 25, inband); emit(0, 90, inband); }
    const offband = { t3: false, t4: false };
    reset(); ph = 0;
    emit(0, 30);
    for (let c = 0; c < 2; c++) { emit(300, 25, offband); emit(0, 25, offband); emit(300, 25, offband); emit(0, 25, offband); emit(300, 25, offband); emit(0, 90, offband); }
    return { inband, offband };
  });
  if (!detect.inband.t3) fail("synthesized T3 did NOT fire the smoke event through the browser wasm");
  if (detect.offband.t3) fail("off-band rhythm WRONGLY read as smoke — the tone gate failed in wasm");

  // ── 3. the mic button opens the stream and the live bench appears ──
  await page.click(".smoke-start");
  await page.waitForSelector(".smoke-live", { timeout: 8000 })
    .catch(() => fail("Start listening did not bring up the live bench (mic path)"));
  if (!(await page.$(".smoke-meter"))) fail("live meter missing");
  if (!(await page.$("#smoke-trace"))) fail("cadence trace missing");
  await page.click(".smoke-stop");
  await page.waitForSelector(".smoke-start", { timeout: 5000 })
    .catch(() => fail("Stop did not return to the idle Start button"));

  if (errors.length) fail("page errors:\n  " + errors.join("\n  "));
  console.log("AUDIO_PROBE_OK");
  process.exit(0);
} catch (e) {
  fail(String((e && e.stack) || e));
} finally {
  await browser.close().catch(() => {});
  server.close();
}
