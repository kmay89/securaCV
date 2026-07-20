// canary-local/tests/sense_probe.mjs — drive the real Sense bench in a browser.
//
// Serves the repo and loads sense.html in headless Chromium, then walks the
// staged flow end to end: every section renders from sense.json, the serial
// console boots the firmware's own log, the MQTT retained snapshot and the
// flavor-gated HA entity set land, the placement lab's FSM reaches Present
// when the sandbox walks someone into the cone, the 3D radome mounts, and
// the tuning/placement surfaces carry their source badges — all with zero
// page errors.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set), same as the
// other probes. Prints SENSE_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
  ".glb": "model/gltf-binary", ".stl": "model/stl",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const fail = (m) => { console.error("SENSE_PROBE_FAIL:", m); process.exit(1); };

// tiny static server rooted at the repo (path-traversal guarded)
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
  await page.goto(`http://localhost:${port}/canary-local/sense.html`, { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForSelector("#sandbox", { timeout: 15000 });

  // all sections render from the JSON
  for (const id of ["device", "lab", "setup", "network", "placement", "tuning", "uses", "sandbox", "more"])
    if (!(await page.$("#" + id))) fail("missing section #" + id);

  // version strip built from sense.json
  const chips = await page.$$eval(".hub-chips .chip", (e) => e.length);
  if (chips < 5) fail("version strip thin (" + chips + " chips)");

  // 3D radome + lab canvas mounted
  if (!(await page.$(".wap-plug-3d"))) fail("device 3D canvas missing");
  if (!(await page.$(".sense-lab-canvas"))) fail("lab canvas missing");

  // the HA entity set is flavor-gated: default hides the vitals entities
  const entsDefault = await page.$$eval(".sense-ents .wap-ent", (e) => e.length);
  await page.locator(".sense-ents .tab", { hasText: "wellbeing" }).click();
  const entsWell = await page.$$eval(".sense-ents .wap-ent", (e) => e.length);
  if (entsWell - entsDefault !== 3) fail(`wellbeing must add exactly 3 entities (got ${entsDefault} → ${entsWell})`);

  // provenance discipline: placement/tuning claims carry source badges
  const badges = await page.$$eval(".sense-src", (e) => e.length);
  if (badges < 15) fail("source badges thin (" + badges + ")");

  // serial console: skip to ready → the firmware's own scenes stream
  await page.click(".wap-term .ghost");
  await page.waitForFunction(() => /The canary is singing/.test(
    document.querySelector(".wap-term-scroll")?.textContent || ""), null, { timeout: 10000 })
    .catch(() => fail("serial console never reached the ready banner"));
  const serialTxt = await page.$eval(".wap-term-scroll", (n) => n.textContent);
  for (const a of ["Who is in the room?", "MR60BHA2 60GHz FMCW radar", "[MQTT] Connected."])
    if (!serialTxt.includes(a)) fail("serial console missing: " + a);

  // MQTT retained snapshot lands after the boot stream's [MQTT] Connected
  await page.waitForFunction(() => document.querySelectorAll(".wap-mqtt-row").length >= 5, null, { timeout: 8000 })
    .catch(() => fail("MQTT retained topics never filled"));

  // sandbox: walk into the room → the lab FSM debounces to Present, the
  // console prints the firmware's line, a signed event streams on MQTT
  const cards = await page.$$(".wap-sand-card");
  if (cards.length < 6) fail("sandbox thin: " + cards.length + " cards");
  let clicked = false;
  for (const c of cards) { if (/Walk into the room/i.test(await c.textContent())) { await c.click(); clicked = true; break; } }
  if (!clicked) fail("no walk-in sandbox card");
  await page.waitForFunction(() => /Present/.test(
    document.querySelector(".sense-lab-side .wap-pill")?.textContent || ""), null, { timeout: 6000 })
    .catch(() => fail("lab FSM never reached Present after walk-in"));
  await page.waitForFunction(() => /presence_detected/.test(
    document.querySelector(".wap-mqtt-stream")?.textContent || ""), null, { timeout: 6000 })
    .catch(() => fail("presence_detected never streamed on MQTT"));
  await page.waitForFunction(() => /\[presence\] -> present/.test(
    document.querySelector(".wap-term-scroll")?.textContent || ""), null, { timeout: 6000 })
    .catch(() => fail("serial console missing [presence] -> present"));

  // and the stall card: pull the radar UART → Unknown + the problem sensor
  for (const c of cards) { if (/Unplug the radar/i.test(await c.textContent())) { await c.click(); break; } }
  await page.waitForFunction(() => /Unknown/.test(
    document.querySelector(".sense-lab-side .wap-pill")?.textContent || ""), null, { timeout: 10000 })
    .catch(() => fail("lab FSM never stalled to Unknown"));

  if (errors.length) fail(errors.length + " page/console errors: " + errors.join(" | "));
  console.log(`SENSE_PROBE_OK — ${chips} chips, ${entsDefault}→${entsWell} entities, ${cards.length} sandbox cards, ${badges} source badges`);
} finally {
  await browser.close();
  server.close();
}
process.exit(0);
