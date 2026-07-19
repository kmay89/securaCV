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
import { extname, join, dirname, resolve } from "node:path";
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
  await page.goto(`http://localhost:${port}/canary-local/wap.html`, { waitUntil: "networkidle", timeout: 45000 });
  await page.waitForSelector("#sandbox", { timeout: 15000 });

  // all five sections render from the JSON
  for (const id of ["board", "setup", "network", "sandbox", "more"])
    if (!(await page.$("#" + id))) fail("missing section #" + id);

  // version strip built from wap.json
  const chips = await page.$$eval(".hub-chips .chip", (e) => e.length);
  if (chips < 4) fail("version strip thin (" + chips + " chips)");

  // power on the serial console → the phone must catch the SoftAP live
  await page.click(".wap-term .primary");
  const own = await page.waitForSelector(".wap-wifi-own", { timeout: 8000 }).catch(() => null);
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

  // the real vendor board mesh mounted
  if (!(await page.$(".boardlab-3d"))) fail("board 3D canvas missing");

  if (errors.length) fail(errors.length + " page/console errors: " + errors.join(" | "));
  console.log(`WAP_PROBE_OK — ${chips} chips, ${ents} entities, ${cards.length} sandbox cards`);
} finally {
  await browser.close();
  server.close();
}
process.exit(0);
