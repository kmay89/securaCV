// canary-local/tests/onboard_probe.mjs — the display's REAL first-boot portal,
// walked in a browser. CI: canary-local.yml, "firmware → wasm → boots".
//
//   node canary-local/tests/onboard_probe.mjs [--shots DIR] [--flavor NAME]
//
// The emulator compiles net/provision.cpp verbatim, so a first meeting (no
// Wi-Fi stored) must land in the firmware's own SoftAP + captive portal. Two
// passes:
//
//  1. The harness, per committed display flavor (?meet=1): the firmware says
//     its first-boot line on serial and raises "SecuraCV-XXXX" with the key it
//     printed; the glass's Join scene shows its QR card with nothing painted
//     over it (read off the framebuffer — F43, see joinCard; --shots saves
//     onboard_join_<flavor>.png); the phone's wrong key is refused and the
//     right one joins; the captive DNS answers A with 192.168.4.1 and AAAA
//     with no data; the OS
//     probe gets the 302; GET / serves PORTAL_HTML byte-for-byte as
//     devices/display_portal.json pins it; /scan lists the staged LAN
//     strongest-first with the display's zone; a wrong key and an absent SSID
//     come back with the firmware's own reasons while GET / keeps answering;
//     the right key succeeds, credentials land in NVS only then, the AP goes
//     away, and the boot carries on to "The canary is singing" and MQTT.
//  2. fleet.html, the watch sheet, "Try it": the same walk through the phone
//     the Lab shows, with a securitypolicyviolation listener installed and the
//     captive <iframe srcdoc> checked for the firmware's own styling (its
//     <style> block is allowed by the hash gen_csp.py pins, not by a loosened
//     policy).
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set).
import { createServer } from "node:http";
import { readFile, readdir, mkdir, writeFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".json": "application/json",
  ".css": "text/css", ".wasm": "application/wasm", ".stl": "application/octet-stream",
  ".svg": "image/svg+xml", ".png": "image/png", ".glb": "model/gltf-binary",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();
const { stripPortal } = await import("../assets/onboard-phone.js");

const shotsIdx = process.argv.indexOf("--shots");
const SHOTS = shotsIdx > 0 ? process.argv[shotsIdx + 1] : null;
const flavorIdx = process.argv.indexOf("--flavor");
const ONLY = flavorIdx > 0 ? process.argv[flavorIdx + 1] : null;

const PORTAL = JSON.parse(await readFile(join(ROOT, "canary-local/devices/display_portal.json"), "utf8"));
const DIST = join(ROOT, "canary-local/emulator/dist");
const FLAVORS = (await readdir(DIST))
  .map((f) => /^canary-display-([a-z0-9]+)\.js$/.exec(f)?.[1])
  .filter(Boolean)
  .sort();
const RUN = ONLY ? FLAVORS.filter((f) => f === ONLY) : FLAVORS;
if (!RUN.length) { console.error(`ONBOARD_PROBE_FAIL: no dist bundle for ${ONLY || "any flavor"}`); process.exit(1); }
if (SHOTS) await mkdir(SHOTS, { recursive: true });

// Allowlist, not sanitization (same stance as the sibling probes).
const SERVABLE = new Map();
async function allow(dirRel) {
  for (const f of await readdir(join(ROOT, dirRel))) {
    if (extname(f) in MIME) SERVABLE.set(`/${dirRel}/${f}`, join(ROOT, dirRel, f));
  }
}
for (const d of ["canary-local", "canary-local/assets", "canary-local/devices", "canary-local/boards",
  "canary-local/enclosures/preview", "canary-local/emulator/web", "canary-local/emulator/dist",
  "canary-local/models", "docs/hardware/enclosure"]) await allow(d);

const server = createServer(async (req, res) => {
  const key = decodeURIComponent(req.url.split("?")[0].split("#")[0]);
  if (key === "/favicon.ico") { res.writeHead(204); res.end(); return; }
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
const failures = [];
const sha256 = (s) => createHash("sha256").update(s, "utf8").digest("hex");
const until = async (fn, what, timeout = 60000, every = 250) => {
  const t0 = Date.now();
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() - t0 > timeout) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, every));
  }
};

// The Join scene's QR card, read off the framebuffer the firmware drew (runs
// in the page). The card is the only pure-white paint on a Quiet Glass first
// boot (text inks top out below it), so its bounding box is the card; inside
// that box, past the rounded corners, every pixel must be the QR's own black
// or the card's white. A caption, hint or title laid across the card shows up
// as anti-aliased gray there — the F43 defect, where the dash's "or join …
// password" line crossed the card's lower edge (the quiet zone a phone's
// scanner needs empty).
function joinCard() {
  const cv = document.getElementById("glass");
  const w = cv.width, h = cv.height;
  const px = cv.getContext("2d").getImageData(0, 0, w, h).data;
  const white = (i) => px[i] === 255 && px[i + 1] === 255 && px[i + 2] === 255;
  const black = (i) => px[i] === 0 && px[i + 1] === 0 && px[i + 2] === 0;
  let x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      if (!white((y * w + x) * 4)) continue;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
  if (x1 - x0 < 64 || y1 - y0 < 64) return null;  // no card (yet)
  const R = 12;  // onboard_ui's 10 px card radius + its anti-aliased edge
  let stray = 0, first = null;
  for (let y = y0; y <= y1; y++) {
    for (let x = x0; x <= x1; x++) {
      if ((x < x0 + R || x > x1 - R) && (y < y0 + R || y > y1 - R)) continue;
      const i = (y * w + x) * 4;
      if (white(i) || black(i)) continue;
      stray++;
      if (!first) first = [x, y];
    }
  }
  return { panel: [w, h], box: [x0, y0, x1, y1], stray, first };
}

// ── 1. the harness, per flavor ──────────────────────────────────────────────
async function walkHarness(flavor) {
  const page = await browser.newPage({ viewport: { width: 1000, height: 620 } });
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  const E = (fn, arg) => page.evaluate(fn, arg);
  const serial = () => E(() => window.__state.serialText);
  const check = (ok, msg) => { if (!ok) throw new Error(msg); };
  // Poll /status until the firmware leaves "connecting".
  const verdict = () => until(async () => {
    const r = await E(() => window.__emu.http("GET", "/status"));
    const j = JSON.parse(r.body || "{}");
    return j.state === "connecting" || j.state === "idle" ? null : j;
  }, "a /status verdict", 30000, 900);

  try {
    await page.goto(`http://localhost:${port}/canary-local/emulator/web/harness.html?hour=10&meet=1&flavor=${flavor}`);
    await page.waitForFunction(() => window.__ready === true, null, { timeout: 90000 });

    // First boot: the firmware's own line, and the radio it configured.
    await until(async () => (await serial()).includes('First boot - onboarding AP "SecuraCV-'), "the first-boot serial line");
    const ap = await until(() => E(() => window.__emu.softAp()), "the SoftAP to come up");
    const s0 = await serial();
    check(/^SecuraCV-[2-9A-HJ-NP-Za-km-z]{4}$/.test(ap.ssid), `SoftAP SSID ${ap.ssid} is not SecuraCV-XXXX`);
    check(ap.pass.length === PORTAL.ap.password_length, `SoftAP key length ${ap.pass.length}`);
    check(s0.includes(`First boot - onboarding AP "${ap.ssid}"  password ${ap.pass}`),
      "the serial line and the radio disagree on the setup network");
    check(ap.channel === PORTAL.ap.channel && ap.maxStations === PORTAL.ap.max_stations,
      `SoftAP channel/max ${ap.channel}/${ap.maxStations}`);
    check(!s0.includes("The canary is singing"), "boot finished while the portal should hold it");

    // The Join scene as the glass draws it: the QR card is up and nothing
    // else paints over it. Read after the scene's 260 ms text fade settles.
    await until(() => E(joinCard), "the join QR card on the glass", 20000);
    await new Promise((r) => setTimeout(r, 800));
    const card = await E(joinCard);
    if (SHOTS) {
      const png = await E(() => document.getElementById("glass").toDataURL("image/png"));
      await writeFile(`${SHOTS}/onboard_join_${flavor}.png`, Buffer.from(png.split(",")[1], "base64"));
    }
    check(card && card.stray === 0,
      `the join scene paints over its QR card: ${card ? `${card.stray} px not the QR's black/white inside ` +
        `card ${JSON.stringify(card.box)} on ${card.panel.join("x")}, first at ${JSON.stringify(card.first)}` : "card gone"}`);

    // The phone joins: the radio checks the key the firmware chose.
    check(await E((a) => window.__emu.phoneJoin(a.ssid, "wrongkey"), ap) === -1, "a wrong AP key was not refused");
    check(await E((a) => window.__emu.phoneJoin(a.ssid, a.pass), ap) === 1, "the QR's key did not join");

    // Captive DNS: the firmware's dns_build_response.
    const dns = await E(async () => {
      const rd = (b) => (b ? [...b] : null);
      return {
        a: rd(await window.__emu.dnsQuery("captive.apple.com", 1)),
        aaaa: rd(await window.__emu.dnsQuery("captive.apple.com", 28)),
      };
    });
    const { parseDnsReply } = await import("../emulator/web/emu-shell.js");
    const a = parseDnsReply(Uint8Array.from(dns.a || []));
    const aaaa = parseDnsReply(Uint8Array.from(dns.aaaa || []));
    check(a && a.response && a.authoritative && a.ancount === 1 && a.a === PORTAL.captive.ip,
      `DNS A answer ${JSON.stringify(a)}`);
    check(aaaa && aaaa.response && aaaa.ancount === 0 && aaaa.rcode === 0, `DNS AAAA answer ${JSON.stringify(aaaa)}`);

    // The OS probe and the page.
    const probe = await E(() => window.__emu.http("GET", "/hotspot-detect.html"));
    check(probe.status === 302 && probe.headers.location === PORTAL.captive.not_found_redirect,
      `captive probe → ${probe.status} ${probe.headers.location}`);
    const home = await E(() => window.__emu.http("GET", "/"));
    check(home.status === 200 && home.contentType === "text/html", `GET / → ${home.status} ${home.contentType}`);
    check(home.headers["cache-control"] === "no-store", "GET / lost Cache-Control: no-store");
    check(sha256(home.body) === PORTAL.captive.html_sha256,
      "GET / does not serve the PORTAL_HTML devices/display_portal.json pins — dist and source disagree");
    check(stripPortal(home.body) === PORTAL.captive.html, "stripPortal(served page) != display_portal.json captive.html");

    // /scan: the firmware's sorted, escaped list + the zone it is on.
    const scan = await until(async () => {
      const r = await E(() => window.__emu.http("GET", "/scan"));
      const j = JSON.parse(r.body || "{}");
      return j.scanning ? null : j;
    }, "a finished /scan", 20000, 900);
    const names = (scan.networks || []).map((n) => n.ssid);
    check(names[0] === "HomeNet" && scan.networks[0].secure === true && scan.networks[0].rssi === -52,
      `/scan list ${JSON.stringify(scan.networks)}`);
    check(names.includes("Corner Cafe Guest") && scan.networks.find((n) => n.ssid === "Corner Cafe Guest").secure === false,
      "/scan lost the open network");
    const rssis = scan.networks.map((n) => n.rssi);
    check(rssis.every((r, i) => i === 0 || r <= rssis[i - 1]), "/scan is not strongest-first");
    check(typeof scan.tz === "string" && scan.tz.length > 0, "/scan carries no zone");

    const join = (ssid, pass, tz) => E((b) => window.__emu.http("POST", "/join",
      { body: b, contentType: "application/x-www-form-urlencoded" }),
    "ssid=" + encodeURIComponent(ssid) + "&pass=" + encodeURIComponent(pass) + (tz ? "&tz=" + encodeURIComponent(tz) : ""));

    // Wrong key: the firmware's reason, nothing persisted, the portal stays.
    await E(() => { window.__nvs = []; window.__emu.opts.onNvsWrite = (ns, k) => window.__nvs.push(`${ns}/${k}`); });
    let r = await join("HomeNet", "wrong-horse");
    check(r.status === 200 && JSON.parse(r.body).ok === true, `POST /join → ${r.status} ${r.body}`);
    let v = await verdict();
    check(v.state === "fail" && v.reason === PORTAL.join_failures.BadPassword, `wrong key verdict ${JSON.stringify(v)}`);
    check(!(await E(() => window.__nvs)).some((k) => k.startsWith("securacv/wifi_")), "a failed join wrote credentials");
    check((await E(() => window.__emu.http("GET", "/"))).status === 200, "the portal stopped answering after a failure");

    // An SSID nobody broadcasts.
    r = await join("Nobody Here", "whatever");
    v = await verdict();
    check(v.state === "fail" && v.reason === PORTAL.join_failures.NotFound, `absent SSID verdict ${JSON.stringify(v)}`);

    // A body the firmware refuses outright.
    r = await join("x".repeat(33), "k");
    check(r.status === 400 && JSON.parse(r.body).reason === "bad request", `33-byte SSID → ${r.status} ${r.body}`);

    // The right key, with a zone: success, and only now the credentials.
    r = await join("HomeNet", "correct-horse", "UTC0");
    v = await verdict();
    check(v.state === "success", `right key verdict ${JSON.stringify(v)}`);
    const nvs = await E(() => window.__nvs);
    check(nvs.includes("securacv/wifi_ssid") && nvs.includes("securacv/wifi_pass"), `credentials not persisted: ${nvs}`);

    // The phone saw the verdict: the AP lingers a beat, then goes; boot resumes.
    await until(async () => !(await E(() => window.__emu.softAp())), "the SoftAP to come down", 40000);
    await until(async () => (await serial()).includes("The canary is singing"), "the boot to finish after onboarding", 60000);
    const s1 = await serial();
    check(s1.includes('Joined "HomeNet"'), "serial never said the join");
    check(s1.includes("Onboarding complete."), "serial never said onboarding completed");
    check(s1.includes("Timezone chosen during setup."), "the zone the portal sent was not applied");
    check((await E(() => window.__emu.http("GET", "/"))).status === 0, "the portal still answers after teardown");
    await until(() => E(() => window.__state.mqtt.some((m) => m.dir === "out" && m.topic.endsWith("/status"))),
      "the display's MQTT status after onboarding", 30000);
    if (SHOTS) await page.screenshot({ path: `${SHOTS}/onboard_${flavor}.png` });
    if (errors.length) throw new Error("page errors:\n" + errors.slice(0, 8).join("\n"));
    console.log(`ONBOARD_PROBE_OK[${flavor}] ${ap.ssid}: join card clean, refused wrong key, captive DNS+302, served page pinned, ` +
      `3 verdicts from firmware, persisted on success, boot resumed`);
  } catch (e) {
    if (SHOTS) await page.screenshot({ path: `${SHOTS}/onboard_${flavor}_fail.png` }).catch(() => {});
    console.error(`ONBOARD_PROBE_FAIL[${flavor}]:`, e.message);
    failures.push(flavor);
  }
  await page.close();
}

for (const f of RUN) await walkHarness(f);

// ── 2. the Lab's phone, on fleet.html ───────────────────────────────────────
if (!ONLY || ONLY === "watch") {
  const context = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  const page = await context.newPage();
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));
  const phone = page.locator(".ob-wrap");
  try {
    await page.goto(`http://localhost:${port}/canary-local/fleet.html#canary-display-watch`);
    await page.waitForSelector(".tabs .tab", { timeout: 30000 });
    // The page's own violations, from here on. The captive frame is sandboxed
    // without scripts, so nothing can listen INSIDE it — a style it is refused
    // surfaces as a console error ("Refused to apply inline style …"), which
    // fails this pass, and the frame's computed style is checked directly.
    await page.evaluate(() => {
      window.__cspViolations = [];
      document.addEventListener("securitypolicyviolation", (e) => {
        window.__cspViolations.push(`${e.effectiveDirective || e.violatedDirective} ${e.blockedURI || "(inline)"}`);
      });
    });
    await page.locator(".tabs .tab", { hasText: "Try it" }).first().click();
    await phone.locator("text=No setup network on the air").waitFor({ timeout: 30000 });
    await phone.locator("button", { hasText: "factory-fresh first boot" }).click();
    await phone.locator(".wap-wifi-own").waitFor({ timeout: 60000 });
    const ssid = await phone.locator(".wap-wifi-own .wap-wifi-name").textContent();
    if (!/^SecuraCV-/.test(ssid)) throw new Error(`phone lists ${ssid} as the setup network`);
    await phone.locator("button", { hasText: "Scan the join QR" }).click();
    const frame = phone.locator("iframe.ob-frame");
    await frame.waitFor({ timeout: 30000 });
    const srcdoc = await frame.getAttribute("srcdoc");
    if (srcdoc !== PORTAL.captive.html) throw new Error("the phone's srcdoc is not the pinned, stripped portal page");
    // The firmware's own stylesheet applied inside the frame (body background
    // #000 from its :root palette) — allowed by the CSP hash, not blocked. A
    // refused <style> leaves the body transparent.
    const inner = await (await frame.elementHandle()).contentFrame();
    const bg = await inner.evaluate(() => getComputedStyle(document.body).backgroundColor);
    if (bg !== "rgb(0, 0, 0)") throw new Error(`portal styling not applied in the frame (body ${bg}) — CSP hash stale?`);
    await phone.locator(".wap-wiz-net", { hasText: "HomeNet" }).click({ timeout: 30000 });
    await phone.locator("input.pw-masked").fill("correct-horse");
    await phone.locator("button", { hasText: /^Join$/ }).click();
    await phone.locator("text=The setup network is gone").waitFor({ timeout: 60000 });
    if (SHOTS) await page.screenshot({ path: `${SHOTS}/onboard_fleet_phone.png` });
    const air = await phone.locator(".ob-air").textContent();
    for (const needle of ["DNS A captive.apple.com → 192.168.4.1", "302", "GET / → 200", '{"state":"success"}']) {
      if (!air.includes(needle)) throw new Error(`exchange log lacks ${JSON.stringify(needle)}`);
    }
    const violations = await page.evaluate(() => window.__cspViolations);
    if (violations.length) throw new Error("CSP violations: " + violations.join("; "));
    if (errors.length) throw new Error("page errors:\n" + errors.slice(0, 8).join("\n"));
    console.log(`ONBOARD_PROBE_OK[fleet.html] phone walked ${ssid} → HomeNet, frame styled by its hashed <style> (${bg}), zero violations`);
  } catch (e) {
    if (SHOTS) await page.screenshot({ path: `${SHOTS}/onboard_fleet_fail.png` }).catch(() => {});
    console.error("ONBOARD_PROBE_FAIL[fleet.html]:", e.message);
    failures.push("fleet.html");
  }
  await context.close();
}

await browser.close();
server.close();
if (failures.length) {
  console.error(`ONBOARD_PROBE_FAIL: ${failures.join(", ")}`);
  process.exit(1);
}
console.log(`ONBOARD_PROBE_OK all: ${RUN.join(", ")}${!ONLY || ONLY === "watch" ? " + fleet.html phone" : ""}`);
process.exit(0);
