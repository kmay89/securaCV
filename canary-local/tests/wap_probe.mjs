// canary-local/tests/wap_probe.mjs — drive the real WAP bench in a browser.
//
// Serves the repo and loads wap.html in headless Chromium, then walks the
// staged first-boot flow end to end: power on the serial console, watch the
// phone catch the SoftAP, render the firmware's captive HTML, walk the setup
// wizard to "online", confirm the MQTT retained snapshot + all 24 Home
// Assistant discovery entities land, fire a sandbox cadence, and confirm the
// board mesh + 3D canvas mounted — all with zero page errors.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set), same as the
// other probes. Prints WAP_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
  ".glb": "model/gltf-binary",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const fail = (m) => { console.error("WAP_PROBE_FAIL:", m); process.exit(1); };

// tiny static server rooted at the repo (path-traversal guarded)
const server = createServer(async (req, res) => {
  try {
    const rel = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (rel === "/favicon.ico") { res.writeHead(204); return res.end(); }
    const p = resolve(join(ROOT, rel));
    if (p !== ROOT && !p.startsWith(ROOT + sep)) { res.writeHead(403); return res.end(); }
    const body = await readFile(rel.endsWith("/") ? join(p, "index.html") : p);
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
  await page.goto(`http://localhost:${port}/canary-local/wap.html`, { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForSelector("#sandbox", { timeout: 15000 });

  // all sections render from the JSON
  for (const id of ["board", "setup", "flash", "network", "sandbox", "more"])
    if (!(await page.$("#" + id))) fail("missing section #" + id);

  // version strip built from wap.json
  const chips = await page.$$eval(".hub-chips .chip", (e) => e.length);
  if (chips < 4) fail("version strip thin (" + chips + " chips)");

  // the bench starts unplugged: the console must be gated until a power
  // source is chosen — that choice IS the lesson
  const plugCards = await page.$$(".wap-plug-card");
  if (plugCards.length !== 2) fail("expected 2 plug-in choices, got " + plugCards.length);
  if (!(await page.$eval(".wap-term .primary", (b) => b.disabled)))
    fail("power-on must be disabled before the device is plugged in");
  await plugCards[0].click(); // laptop: USB data — the console becomes yours
  await page.waitForFunction(() => !document.querySelector(".wap-term .primary").disabled, null, { timeout: 4000 })
    .catch(() => fail("choosing the laptop did not enable the serial console"));

  // power on the serial console → the phone must catch the SoftAP live
  // (the boot log streams ~40 lines before the AP line; give a loaded CI
  // runner headroom — this wait flaked at 8 s under CPU contention)
  await page.click(".wap-term .primary");
  const own = await page.waitForSelector(".wap-wifi-own", { timeout: 15000 }).catch(() => null);
  if (!own) fail("phone did not surface the SoftAP after power-on");
  const ssid = (await own.textContent()) || "";
  if (!/SecuraCV-/.test(ssid)) fail("SoftAP SSID not SecuraCV-*: " + ssid);

  // captive sheet renders the firmware's own HTML (verbatim, in an iframe)
  await own.click();
  const frame = await page.waitForSelector(".wap-captive-frame", { timeout: 6000 }).catch(() => null);
  if (!frame) fail("captive frame missing");
  // the iframe renders the firmware's own CAPTIVE_PORTAL_HTML via srcdoc
  const srcdoc = await frame.getAttribute("srcdoc");
  if (!srcdoc || !/Set up your Canary/.test(srcdoc)) fail("captive iframe is not the firmware page");

  // walk the wizard to online
  await page.click(".wap-captive-go");
  await page.waitForSelector(".wap-wiz-go", { timeout: 6000 });
  await page.click(".wap-wiz-go");                     // step 1 → 2
  await page.waitForSelector(".wap-wiz-net", { timeout: 6000 });
  await page.click(".wap-wiz-net");                    // step 2 → 3
  await page.waitForSelector(".wap-wiz-btns .primary", { timeout: 6000 });
  await page.click(".wap-wiz-btns .primary");          // connect
  // step 5 must actually render — pre-flight checks (/api/selftest) + recovery kit
  const pf = await page.waitForSelector(".wap-preflight", { timeout: 12000 }).catch(() => null);
  if (!pf) fail("wizard never reached step 5 (pre-flight checks)");
  const checkCount = await page.$$eval(".wap-checks .wap-check-row", (e) => e.length);
  if (checkCount < 6) fail("expected 6 pre-flight checks, got " + checkCount);
  const hasRecovery = await page.$$eval(".wap-ha-block h5", (hs) => hs.some((h) => /recovery kit/i.test(h.textContent)));
  if (!hasRecovery) fail("recovery-kit block missing from step 5");
  // Home Assistant is gated behind saving the recovery kit (mirrors the firmware,
  // where /api/mqtt/config 401s without the receipt's cv_session cookie).
  const haVisible = () => page.evaluate(() => {
    const ha = [...document.querySelectorAll(".wap-ha-block")]
      .find((b) => /home assistant/i.test(b.querySelector("h5")?.textContent || ""));
    return !!ha && getComputedStyle(ha).display !== "none";
  });
  if (await haVisible()) fail("Home Assistant block shown before the recovery kit was saved");
  await page.locator("button.primary", { hasText: "Save my recovery kit" }).click();
  const revealed = await page.waitForFunction(() => {
    const ha = [...document.querySelectorAll(".wap-ha-block")]
      .find((b) => /home assistant/i.test(b.querySelector("h5")?.textContent || ""));
    return !!ha && getComputedStyle(ha).display !== "none";
  }, null, { timeout: 4000 }).catch(() => null);
  if (!revealed) fail("Home Assistant block was not revealed after saving the recovery kit");
  // finish via the Home Assistant "Test & save" (brings up MQTT)
  await page.locator("button.primary", { hasText: "Test & save" }).click();

  // MQTT retained snapshot + full discovery set land
  await page.waitForFunction(() => document.querySelectorAll(".wap-mqtt-row").length >= 6, null, { timeout: 8000 })
    .catch(() => fail("MQTT retained topics never filled"));
  const ents = await page.$$eval(".wap-ent", (e) => e.length);
  if (ents !== 24) fail("expected 24 HA discovery entities, got " + ents);

  // the serial console shows the connect handshake
  const serialTxt = await page.$eval(".wap-term-scroll", (n) => n.textContent);
  if (!/\[MQTT\] connected/.test(serialTxt)) fail("serial console missing [MQTT] connected");
  if (!/The canary is singing/.test(serialTxt)) fail("serial console missing the ready banner");

  // sandbox: fire the T3 smoke cadence → acoustic card turns red + MQTT event
  const cards = await page.$$(".wap-sand-card");
  let clicked = false;
  for (const c of cards) { if (/smoke/i.test(await c.textContent())) { await c.click(); clicked = true; break; } }
  if (!clicked) fail("no smoke sandbox card");
  const red = await page.waitForFunction(() => !!document.querySelector(".wap-ac-card.alarm"), null, { timeout: 4000 }).catch(() => null);
  if (!red) fail("smoke cadence did not alarm the acoustic card");

  // the real vendor board mesh mounted, and so did the plug-in device scene
  if (!(await page.$(".boardlab-3d"))) fail("board 3D canvas missing");
  if (!(await page.$(".wap-plug-3d"))) fail("plug-in 3D canvas missing");

  // §flash: the strap trainer must reach download mode the way a human would —
  // cable in, hold BOOT, tap RESET → the ROM's own 'waiting for download'
  const bench = await page.$$("#flash .wap-bench .bench-chip");
  if (bench.length !== 3) fail("strap trainer should have 3 chips, got " + bench.length);
  await bench[0].click(); // USB in
  await bench[1].click(); // hold BOOT
  await bench[2].click(); // tap RESET
  const dl = await page.waitForFunction(
    () => /waiting for download/.test(document.querySelector("#flash .wap-bench-console")?.textContent || ""),
    null, { timeout: 4000 }).catch(() => null);
  if (!dl) fail("strap trainer never reached download mode");

  if (errors.length) fail(errors.length + " page/console errors: " + errors.join(" | "));
  console.log(`WAP_PROBE_OK — ${chips} chips, ${ents} entities, ${cards.length} sandbox cards`);
} finally {
  await browser.close();
  server.close();
}
process.exit(0);
