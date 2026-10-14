// Host test for firmware/canary's dashboard: what a build without a feature
// shows, and what Export saves (canary/lib/securacv_webui/src/
// securacv_webui.cpp) — repo sweep F273, F274, F278, found by F238's
// GATED list in test_dashboard_route_match.test.js.
//
//   - Opera (F273): the mesh routes are registered only under
//     FEATURE_MESH_NETWORK, which only [env:full] sets, and the Opera tab
//     always showed, its controls posting to routes nothing registers. The
//     tab now takes F176's shape: it starts hidden and the page load's one
//     GET /api/mesh shows it on any JSON answer (the route's own refusals,
//     429/401/503, are JSON: the build serves it). The fallback's 404 page
//     or setup redirect keeps it hidden and nothing asks again; a probe that
//     got no verdict (no answer, another text page) is asked again on the
//     5 s poll, and unlocking with a token asks once more.
//   - Peek (F274): the camera routes are registered only under
//     FEATURE_CAMERA_PEEK ([env:minimal] and [env:esp32-wroom] have none).
//     refreshPeekStatus() ignored the refusal, so the tab kept "Ready" and
//     "▶ Start", and Start answered an alert. A status no route took (the
//     GET "/*" fallback's 404 page, or its redirect to the setup wizard),
//     before any status has answered, now shows the panel's no-camera state,
//     says the build has no preview, and hides the Camera Controls and Stream
//     Metrics cards, which hold every control that calls another camera
//     route; opening the panel reads the sensor state only once a status has
//     answered (the first answer pulls it), so such a build is asked only
//     for the status. No answer, the route's own refusal (it wants a token)
//     or a text error page from a route that exists (500, 408) says nothing
//     about the build and changes nothing; once a status has answered, a failed poll
//     changes nothing (a blip mid-stream is not a missing camera). The
//     answers fed to the panel come from the page's own api(), run against a
//     stub fetch.
//   - Export (F278): handle_export answers the bundle inline (ok and the
//     bundle's fields, no download_url), and exportWitness() looked for a
//     download_url, so every good export alerted "Export failed: Unknown
//     error". It now saves the answer as a JSON download. The answer this
//     test feeds the page carries exactly the keys the handler writes, read
//     from securacv_network.cpp.
// The functions are lifted out of the C++ raw string by literal markers that
// stand in the page before and after these changes (so the old page runs
// here and fails on what it does, not on a missing marker), and run against
// a stub DOM and a recording api(), the way test_canary_community_panel.test.js
// does it.
//
// Run: node --test firmware/tests_host/test_canary_dashboard_gates.test.js
// (the Makefile's `run` target does, after the C++ suites).

"use strict";

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const FW = join(__dirname, "..");
const UI = process.env.CANARY_WEBUI || join(FW, "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
const NET = join(FW, "canary", "lib", "securacv_network", "src", "securacv_network.cpp");
const src = readFileSync(UI, "utf8");

function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}

// What api() returns for the GET "/*" fallback's 404 page (text, not JSON),
// the answer a build gives a route it does not register (pinned to the
// page's own api() in the Peek section).
const NOT_FOUND = { ok: false, success: false, error: "not found", unrouted: true, notJson: true };

// What the page's own api() returns for a response (or for no answer at
// all): lifted from the page and run against a stub fetch, so the refusal a
// test feeds the panel is the one the page itself makes of it.
const apiSrc = slice("    async function api(endpoint, method = 'GET', body = null) {", "\n    }\n") + "\n    }\n";
async function answerTo(res, url = "/api/peek/status") {
  const ctx = {
    API_BASE: "", CV_TOKEN: "", console: { error() {} },
    fetch: async () => {
      if (res === "no answer") throw new TypeError("Failed to fetch");
      return { status: res.status, ok: res.status >= 200 && res.status < 300, redirected: !!res.redirected,
               text: async () => res.body };
    },
  };
  vm.createContext(ctx);
  vm.runInContext(apiSrc + "\nglobalThis.__api = api;\n", ctx);
  return ctx.__api(url);
}
// The GET "/*" fallback (handle_captive_catchall): a 404 page, or during
// setup a redirect to the wizard, which fetch follows.
const FALLBACK_404 = { status: 404, body: "not found" };
const FALLBACK_SETUP = { status: 200, redirected: true, body: "<!DOCTYPE html><html><title>Canary setup</title></html>" };
// The route's own refusals (securacv_auth, auth_gate): JSON.
const ROUTE_401 = { status: 401, body: '{"error":"unauthorized","message":"Bearer token required"}' };
const ROUTE_503 = { status: 503, body: '{"error":"not_provisioned"}' };
const ROUTE_429 = { status: 429, body: '{"error":"rate_limited"}' };
// Answers that are not JSON from a route that exists: esp_http_server's own
// error pages (httpd_resp_send_err) for a handler that failed or a request it
// timed out. Only the fallback's 404 or redirect says "no such route".
const IDF_500 = { status: 500, body: "Internal Server Error" };
const IDF_408 = { status: 408, body: "Server closed this connection" };
// A JSON 404: a route that answered, finding nothing.
const JSON_404 = { status: 404, body: '{"ok":false,"error":"not_found"}' };


const settle = () => new Promise((r) => setImmediate(r));

// A stub DOM element, made on first use; `seed` gives an element the text
// the markup writes into it.
function dom(seed = {}) {
  const els = {};
  const el = (id) => els[id] || (els[id] = {
    id, style: {}, textContent: seed[id] || "", innerHTML: "", value: "", className: "",
    checked: false, disabled: false, classList: { add() {}, remove() {} },
  });
  return el;
}

// The opening tag of the element with this id, as the markup writes it.
function tagOf(id) {
  const m = src.match(new RegExp(`<[a-z]+\\b[^>]*\\bid="${id}"[^>]*>`));
  assert.ok(m, `#${id} is in the markup`);
  return m[0];
}
const hiddenInMarkup = (id) => /style="[^"]*display:\s*none/.test(tagOf(id));

// ── Opera (F273) ─────────────────────────────────────────────────────────

const opera = slice("    let pairingConfirmed = false;", "    async function loadPeers() {");
const load = slice("    refreshLockBanner();\n    refreshStatus();", "    setInterval(refreshStatus, 2000);");
const poll = slice("    setInterval(() => {\n      if (currentPanel === 'logs') loadLogs();", "    }, 5000);");
const unlock = slice("    function unlockWithToken() {", "    // GET /api/provisioning-receipt");

function navButton(panel) {
  const m = src.match(new RegExp(`<button\\b[^>]*data-panel="${panel}"[^>]*>`));
  assert.ok(m, `the ${panel} nav button is in the page`);
  return m[0];
}

// The page with the Opera section, the page load, the panel poll and the
// unlock live and every other loader a stub; api() answers from `answers`
// (a 404 for anything not listed) and records each request.
function operaPage(answers) {
  const el = dom();
  el("navOpera").style.display = /style="display:\s*none;?"/.test(navButton("opera")) ? "none" : "";
  const calls = [];
  const ctx = {
    document: { getElementById: el, querySelectorAll: () => [] },
    currentPanel: "status",
    chirpServed: false,
    btServed: false,
    CV_TOKEN: "",
    alert() {}, confirm: () => true, setInterval: () => 0, clearInterval() {}, setTimeout: () => 0,
    api: async (url, method = "GET") => {
      calls.push(method === "GET" ? url : `${method} ${url}`);
      const a = answers[url];
      return a === undefined ? NOT_FOUND : typeof a === "function" ? a() : a;
    },
  };
  for (const stub of ["refreshLockBanner", "refreshStatus", "refreshLiveSensing", "loadChain",
                      "loadWifiStatus", "refreshChirpStatus", "refreshBtStatus", "updateResolutionUI",
                      "loadLogs", "loadWitness", "loadPeers", "loadOperaAlerts"]) {
    ctx[stub] = () => calls.push(stub);
  }
  vm.createContext(ctx);
  vm.runInContext(
    opera +
    "\n;globalThis.__load = async () => {\n" + load + "\n};\n" +
    "globalThis.__panelPoll = " + poll.replace(/^\s*setInterval\(/, "") + "};\n" +
    unlock +
    "\nglobalThis.__t = { refreshOpera, unlockWithToken };\n",
    ctx);
  return { ctx, el, calls };
}

const meshCalls = (calls) => calls.filter((c) => c.includes("/api/mesh"));
const MESH_STATUS = { ok: true, state: "NO_OPERA", enabled: true, has_opera: false, peers_total: 0,
                      peers_online: 0, alerts_received: 0 };

test("the Opera nav button starts hidden", () => {
  assert.match(navButton("opera"), /style="display:\s*none;?"/);
  assert.match(navButton("opera"), /id="navOpera"/);
});

for (const [name, res] of [["the fallback's 404 page", FALLBACK_404], ["the fallback's redirect to setup", FALLBACK_SETUP]]) {
  test(`a GET /api/mesh no route took (${name}) keeps the Opera tab hidden and asks nothing more`, async () => {
    const p = operaPage({ "/api/mesh": await answerTo(res, "/api/mesh") });
    await p.ctx.__load();
    await settle();
    assert.strictEqual(p.el("navOpera").style.display, "none");
    assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh"]);
    // The 5 s poll asks nothing of the mesh while no status has answered,
    // even were the Opera panel current.
    for (const panel of ["status", "opera"]) {
      p.ctx.currentPanel = panel;
      for (let i = 0; i < 3; ++i) p.ctx.__panelPoll();
    }
    await settle();
    assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh"]);
  });
}

// [env:full]'s handle_mesh_status refuses in JSON (rate_limit_check, then
// auth_gate): the build serves the route, so the tab shows and its click
// asks again, as the page always did there.
for (const [name, res] of [["a token it wants (401)", ROUTE_401], ["the device-wide request limit (429)", ROUTE_429],
                           ["no token to check yet (503)", ROUTE_503]]) {
  test(`the mesh route's own refusal at load (${name}) shows the Opera tab`, async () => {
    const p = operaPage({ "/api/mesh": await answerTo(res, "/api/mesh") });
    await p.ctx.__load();
    await settle();
    assert.strictEqual(p.el("navOpera").style.display, "", "the tab shows");
    p.calls.length = 0;
    p.ctx.currentPanel = "status";
    p.ctx.__panelPoll();
    await settle();
    assert.deepStrictEqual(meshCalls(p.calls), [], "nothing more is asked off the Opera panel");
    p.ctx.currentPanel = "opera";
    p.ctx.__panelPoll();
    await settle();
    assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh"], "and the panel's poll asks again");
  });
}

for (const [name, res] of [["no answer", "no answer"], ["a text error page (500)", IDF_500]]) {
  test(`a probe that got ${name} at load is asked again on the 5 s poll until it gets a verdict`, async () => {
    const answers = { "/api/mesh": await answerTo(res, "/api/mesh") };
    const p = operaPage(answers);
    await p.ctx.__load();
    await settle();
    assert.strictEqual(p.el("navOpera").style.display, "none");
    p.ctx.currentPanel = "status";
    p.ctx.__panelPoll();
    await settle();
    assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh", "/api/mesh"], "the poll asks again");
    assert.strictEqual(p.el("navOpera").style.display, "none");
    answers["/api/mesh"] = MESH_STATUS;
    p.ctx.__panelPoll();
    await settle();
    assert.strictEqual(p.el("navOpera").style.display, "", "a status on a later tick shows the tab");
    p.calls.length = 0;
    for (let i = 0; i < 3; ++i) p.ctx.__panelPoll();
    await settle();
    assert.deepStrictEqual(meshCalls(p.calls), [], "and the probe stops off the Opera panel");
  });
}

test("a probe with no verdict stops once a later one finds no route", async () => {
  const answers = { "/api/mesh": await answerTo("no answer", "/api/mesh") };
  const p = operaPage(answers);
  await p.ctx.__load();
  await settle();
  answers["/api/mesh"] = await answerTo(FALLBACK_404, "/api/mesh");
  p.ctx.__panelPoll();
  await settle();
  for (let i = 0; i < 3; ++i) p.ctx.__panelPoll();
  await settle();
  assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh", "/api/mesh"]);
  assert.strictEqual(p.el("navOpera").style.display, "none");
});

test("a mesh status shows the Opera tab, and the poll refreshes it on that panel", async () => {
  const p = operaPage({ "/api/mesh": MESH_STATUS });
  await p.ctx.__load();
  await settle();
  assert.strictEqual(p.el("navOpera").style.display, "");
  assert.strictEqual(p.el("operaState").textContent, "No Opera");
  p.calls.length = 0;
  p.ctx.currentPanel = "opera";
  p.ctx.__panelPoll();
  await settle();
  assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh"]);
  // Other panels poll as they did.
  p.calls.length = 0;
  p.ctx.currentPanel = "logs";
  p.ctx.__panelPoll();
  assert.deepStrictEqual(p.calls, ["loadLogs"]);
});

test("unlocking with a token asks a probe that got no verdict once more", async () => {
  const answers = { "/api/mesh": await answerTo("no answer", "/api/mesh") };
  const p = operaPage(answers);
  await p.ctx.__load();
  await settle();
  assert.strictEqual(p.el("navOpera").style.display, "none");
  answers["/api/mesh"] = MESH_STATUS;
  p.calls.length = 0;
  p.el("lanTokenInput").value = "tok";
  p.ctx.__t.unlockWithToken();
  await settle();
  assert.strictEqual(p.el("navOpera").style.display, "", "the tab is back");
  assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh"]);
  // Once a status has answered, an unlock asks nothing more of the mesh.
  p.calls.length = 0;
  p.el("lanTokenInput").value = "tok2";
  p.ctx.__t.unlockWithToken();
  await settle();
  assert.deepStrictEqual(meshCalls(p.calls), []);
});

// ── Peek (F274) ──────────────────────────────────────────────────────────

const peek = slice("    // PEEK (Camera Preview)", "    function togglePeek() {");

// The panel's text as the markup writes it, so a refusal that changes
// nothing reads "Ready" and "▶ Start" as the page does.
function peekSeed() {
  const text = (id) => {
    const m = src.match(new RegExp(`\\bid="${id}"[^>]*>([^<]*)<`));
    assert.ok(m, `#${id} is in the markup`);
    return m[1];
  };
  return { peekStatus: text("peekStatus"), peekToggle: text("peekToggle"), peekOfflineText: text("peekOfflineText") };
}

function peekPage(answers) {
  const el = dom(peekSeed());
  // The cards as the markup writes them (shown; an old page names neither).
  for (const id of ["peekControlsCard", "camInfoCard"]) {
    el(id).style.display = src.includes(`id="${id}"`) && hiddenInMarkup(id) ? "none" : "";
  }
  const calls = [];
  const ctx = {
    document: { getElementById: el, querySelectorAll: () => [], querySelector: () => ({ classList: { add() {} } }) },
    cameraReady: false, peekActive: false, currentResolution: 8,
    updateResolutionUI() {}, applyCameraInfo() {},
    startCamInfoPolling: () => calls.push("startCamInfoPolling"),
    stopCamInfoPolling: () => calls.push("stopCamInfoPolling"),
    refreshSensorState: () => calls.push("refreshSensorState"),
    api: async (url, method = "GET") => {
      calls.push(method === "GET" ? url : `${method} ${url}`);
      const a = answers[url];
      return a === undefined ? NOT_FOUND : typeof a === "function" ? a() : a;
    },
  };
  vm.createContext(ctx);
  vm.runInContext(peek + "\nglobalThis.__t = { refreshPeekStatus };\n", ctx);
  return { ctx, el, calls };
}

const PEEK_STATUS = { ok: true, camera_initialized: true, peek_active: false, resolution: 8 };

test("api() reads the fallback's 404 page as no route of this build", async () => {
  assert.deepStrictEqual({ ...(await answerTo(FALLBACK_404)) }, NOT_FOUND);
});

for (const [name, res] of [["the fallback's 404 page", FALLBACK_404], ["the fallback's redirect to setup", FALLBACK_SETUP]]) {
  test(`a GET /api/peek/status no route took (${name}) shows the no-camera state and hides the camera cards`, async () => {
    const refused = await answerTo(res);
    const p = peekPage({ "/api/peek/status": refused });
    await p.ctx.__t.refreshPeekStatus();
    assert.strictEqual(p.el("peekToggle").disabled, true, "Start is off");
    assert.strictEqual(p.el("peekToggle").textContent, "⚠ No Camera");
    assert.strictEqual(p.el("peekStatus").textContent, "No camera in this build");
    assert.match(p.el("peekOfflineText").textContent, /built without the camera preview/);
    assert.strictEqual(p.el("peekStream").style.display, "none");
    assert.strictEqual(p.el("peekControlsCard").style.display, "none");
    assert.strictEqual(p.el("camInfoCard").style.display, "none");
    assert.deepStrictEqual(p.calls.filter((c) => c.startsWith("/api/") || c.includes(" /api/")),
      ["/api/peek/status"], "nothing else is asked of the camera");
  });
}

for (const [name, res] of [["no answer", "no answer"], ["the route wants a token (401)", ROUTE_401],
                           ["the route has no token to check (503)", ROUTE_503],
                           ["a text error page from a route that exists (500)", IDF_500],
                           ["a text timeout page (408)", IDF_408],
                           ["a JSON 404 from a route that answered", JSON_404]]) {
  test(`${name}, before any status, says nothing about the build`, async () => {
    const refused = await answerTo(res);
    assert.notStrictEqual(refused.ok, true);
    const p = peekPage({ "/api/peek/status": refused });
    const seed = peekSeed();
    await p.ctx.__t.refreshPeekStatus();
    assert.strictEqual(p.el("peekStatus").textContent, seed.peekStatus, "no claim about the build");
    assert.strictEqual(p.el("peekToggle").textContent, seed.peekToggle);
    assert.strictEqual(p.el("peekToggle").disabled, false);
    assert.strictEqual(p.el("peekControlsCard").style.display, "");
    assert.strictEqual(p.el("camInfoCard").style.display, "");
  });
}

test("a status no route took, after a no-answer, still shows the no-camera state", async () => {
  const answers = { "/api/peek/status": await answerTo("no answer") };
  const p = peekPage(answers);
  await p.ctx.__t.refreshPeekStatus();
  answers["/api/peek/status"] = await answerTo(FALLBACK_404);
  await p.ctx.__t.refreshPeekStatus();
  assert.strictEqual(p.el("peekStatus").textContent, "No camera in this build");
  assert.strictEqual(p.el("peekControlsCard").style.display, "none");
});

test("a camera status after a status no route took brings the cards back and Start with them", async () => {
  const answers = { "/api/peek/status": await answerTo(FALLBACK_404) };
  const p = peekPage(answers);
  await p.ctx.__t.refreshPeekStatus();
  assert.strictEqual(p.el("peekControlsCard").style.display, "none");
  answers["/api/peek/status"] = PEEK_STATUS;
  await p.ctx.__t.refreshPeekStatus();
  assert.strictEqual(p.el("peekControlsCard").style.display, "");
  assert.strictEqual(p.el("camInfoCard").style.display, "");
  assert.strictEqual(p.el("peekToggle").disabled, false);
  assert.strictEqual(p.el("peekToggle").textContent, "▶ Start");
  assert.strictEqual(p.el("peekStatus").textContent, "Ready");
});

test("a camera status shows the cards and the preview's own state", async () => {
  const p = peekPage({ "/api/peek/status": PEEK_STATUS });
  await p.ctx.__t.refreshPeekStatus();
  assert.strictEqual(p.el("peekToggle").disabled, false);
  assert.strictEqual(p.el("peekToggle").textContent, "▶ Start");
  assert.strictEqual(p.el("peekStatus").textContent, "Ready");
  assert.strictEqual(p.el("peekControlsCard").style.display, "");
  assert.strictEqual(p.el("camInfoCard").style.display, "");
  assert.ok(p.calls.includes("refreshSensorState"), "a camera that just came up pulls its sensor state");
});

test("once a status has answered, a failed poll changes nothing", async () => {
  const answers = { "/api/peek/status": { ...PEEK_STATUS, peek_active: true } };
  const p = peekPage(answers);
  await p.ctx.__t.refreshPeekStatus();
  assert.strictEqual(p.el("peekToggle").textContent, "⏹ Stop");
  for (const res of ["no answer", FALLBACK_404]) {
    answers["/api/peek/status"] = await answerTo(res);
    await p.ctx.__t.refreshPeekStatus();
  }
  p.calls.length = 0;
  await p.ctx.__t.refreshPeekStatus();
  assert.strictEqual(p.el("peekToggle").textContent, "⏹ Stop", "still streaming");
  assert.strictEqual(p.el("peekToggle").disabled, false);
  assert.strictEqual(p.el("peekControlsCard").style.display, "");
  assert.strictEqual(p.el("camInfoCard").style.display, "");
  assert.ok(!p.calls.includes("stopCamInfoPolling"), "the metrics poll goes on");
});

// Opening the panel: the page's switchPanel() and refreshSensorState() live
// beside the Peek section, so a request the panel's opening makes is recorded
// as the request it is.
const switchPanelSrc = slice("    function switchPanel(panel) {", "    // ── LAN unlock + recovery kit");
const sensorSrc = slice("    async function refreshSensorState() {", "    async function resetSensorDefaults() {");
function peekPanelPage(answers, init = {}) {
  const p = peekPage(answers);
  Object.assign(p.ctx, { currentPanel: "status", timelineRefreshTimer: 0, applySensorState() {},
                         stopOtaPolling() {}, stopPeek() {}, clearInterval() {} }, init);
  vm.runInContext(switchPanelSrc + sensorSrc + "\nglobalThis.__t.switchPanel = switchPanel;\n", p.ctx);
  p.open = async () => { p.ctx.__t.switchPanel("peek"); await settle(); await settle(); };
  p.asked = () => p.calls.filter((c) => c.startsWith("/api/") || c.includes(" /api/"));
  return p;
}

test("opening the Peek panel in a build without the camera asks only for the status, every time", async () => {
  const p = peekPanelPage({ "/api/peek/status": await answerTo(FALLBACK_404) });
  await p.open();
  assert.strictEqual(p.el("peekStatus").textContent, "No camera in this build");
  await p.open();
  assert.deepStrictEqual(p.asked(), ["/api/peek/status", "/api/peek/status"],
    "no GET /api/peek/sensor while no status has answered");
});

for (const [name, init] of [["a camera /api/status had not reported", { cameraReady: false }],
                            ["a camera /api/status already reported ready", { cameraReady: true }]]) {
  test(`opening the Peek panel in a camera build reads the sensor state once per open (${name})`, async () => {
    const p = peekPanelPage({ "/api/peek/status": PEEK_STATUS, "/api/peek/sensor": { ok: true } }, init);
    await p.open();
    assert.deepStrictEqual(p.asked(), ["/api/peek/status", "/api/peek/sensor"],
      "the first status pulls the sensor state the opening held back");
    p.calls.length = 0;
    await p.open();
    assert.deepStrictEqual([...p.asked()].sort(), ["/api/peek/sensor", "/api/peek/status"],
      "after a status has answered, the opening reads it again");
  });
}

test("a camera that is not up yet still has its sensor state read on the first open", async () => {
  const p = peekPanelPage({ "/api/peek/status": { ...PEEK_STATUS, camera_initialized: false }, "/api/peek/sensor": { ok: true } });
  await p.open();
  assert.deepStrictEqual(p.asked(), ["/api/peek/status", "/api/peek/sensor"]);
});

test("the cards a refusal hides hold every control that calls another camera route", () => {
  const panel = slice('<div class="panel" id="panel-peek">', '<div class="panel" id="panel-opera">');
  const a = panel.indexOf('id="peekControlsCard"');
  const b = panel.indexOf('id="camInfoCard"');
  assert.ok(a > 0 && b > a, "both cards are in the Peek panel, controls first");
  const controls = panel.slice(a, b);
  const handlers = ["takeSnapshot", "setResolution", "onSensorSlider", "onSensorToggle", "onSensorSelect",
                    "applyPreset", "resetSensorDefaults"];
  for (const h of handlers) {
    assert.ok(controls.includes(`${h}(`), `${h} is in the Camera Controls card`);
    assert.ok(!src.replace(controls, "").includes(`"${h}(`) && !src.replace(controls, "").includes(`=${h}(`),
      `${h} is called from nowhere else in the markup`);
  }
});

// ── Export (F278) ────────────────────────────────────────────────────────

const exporter = slice("    async function exportWitness() {", "\n    }\n") + "\n    }\n";

// The keys handle_export writes into its answer, from the firmware.
function exportKeys() {
  const net = readFileSync(NET, "utf8");
  const a = net.indexOf("static esp_err_t handle_export(httpd_req_t* req) {");
  const b = net.indexOf("\n}\n", a);
  assert.ok(a >= 0 && b > a, "handle_export is defined");
  return [...net.slice(a, b).matchAll(/\bdoc\["(\w+)"\]/g)].map((m) => m[1]);
}

function exportPage(answer) {
  const record = { alerts: [], blobs: [], clicks: [], navigated: null, revoked: [], later: [] };
  class Blob {
    constructor(parts, opts) { this.text = parts.join(""); this.type = opts && opts.type; record.blobs.push(this); }
  }
  // Every way a page sends the browser elsewhere is recorded: location.href,
  // location = ..., window.location = ..., assign/replace.
  const loc = {
    set href(v) { record.navigated = v; }, get href() { return ""; },
    assign: (v) => { record.navigated = v; }, replace: (v) => { record.navigated = v; },
  };
  const nav = { get() { return loc; }, set(v) { record.navigated = v; } };
  const win = Object.defineProperty({}, "location", nav);
  const ctx = {
    Blob,
    URL: { createObjectURL: (b) => `blob:${record.blobs.indexOf(b)}`, revokeObjectURL: (u) => record.revoked.push(u) },
    document: {
      createElement: () => ({ click() { record.clicks.push({ href: this.href, download: this.download }); }, remove() {} }),
      body: { appendChild() {} },
    },
    window: win,
    alert: (m) => record.alerts.push(m),
    // Timers wait until the test runs them, as a browser's would.
    setTimeout: (fn) => { record.later.push(fn); return record.later.length; },
    api: async (url, method) => { record.asked = `${method} ${url}`; return answer; },
  };
  Object.defineProperty(ctx, "location", nav);
  vm.createContext(ctx);
  vm.runInContext(exporter + "\nglobalThis.__t = { exportWitness };\n", ctx);
  return { ctx, record };
}

test("handle_export answers the bundle inline and names no URL", () => {
  const keys = exportKeys();
  assert.ok(keys.includes("ok") && keys.includes("device_id") && keys.includes("pubkey") && keys.includes("chain_head"),
    `the answer carries the bundle (${keys})`);
  assert.ok(!keys.some((k) => /url/i.test(k)), "and no URL to fetch it from");
});

test("Export saves the answer it got, as a JSON download, and alerts nothing", async () => {
  const answer = {};
  for (const k of exportKeys()) answer[k] = k === "ok" ? true : `${k}-value`;
  // An id with characters a file name should not carry, as a device id
  // set by hand could.
  answer.device_id = "canary s3/4dC2";
  answer.last_record = { seq: 7, hash: "ab".repeat(32), type: "BOOT", verified: true };
  const p = exportPage(answer);
  await p.ctx.__t.exportWitness();
  assert.strictEqual(p.record.asked, "POST /api/export");
  assert.deepStrictEqual(p.record.alerts, [], "no alert after a good export");
  assert.strictEqual(p.record.navigated, null, "the browser is not sent anywhere");
  assert.strictEqual(p.record.blobs.length, 1);
  assert.strictEqual(p.record.blobs[0].type, "application/json");
  assert.deepStrictEqual(JSON.parse(p.record.blobs[0].text), answer, "the file is the answer, every field");
  assert.deepStrictEqual(p.record.clicks, [{ href: "blob:0", download: "witness-export-canary_s3_4dC2.json" }],
    "saved under the id with every other character made _");
  assert.deepStrictEqual(p.record.revoked, [], "the link is not revoked before the save can start");
  for (const t of p.record.later) t();
  assert.deepStrictEqual(p.record.revoked, ["blob:0"], "and is revoked after it");
});

test("a refused export alerts the device's reason and saves nothing", async () => {
  const p = exportPage({ ok: false, error: "forbidden" });
  await p.ctx.__t.exportWitness();
  assert.deepStrictEqual(p.record.alerts, ["Export failed: forbidden"]);
  assert.deepStrictEqual(p.record.blobs, []);
  assert.deepStrictEqual(p.record.clicks, []);
});
