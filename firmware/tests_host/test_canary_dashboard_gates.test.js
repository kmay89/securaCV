// Host test for firmware/canary's dashboard: what a build without a feature
// shows, and what Export saves (canary/lib/securacv_webui/src/
// securacv_webui.cpp) — repo sweep F273, F274, F278, found by F238's
// GATED list in test_dashboard_route_match.test.js.
//
//   - Opera (F273): the mesh routes are registered only under
//     FEATURE_MESH_NETWORK, which only [env:full] sets, and the Opera tab
//     always showed, its controls posting to routes nothing registers. The
//     tab now takes F176's shape: it starts hidden, the page load's one
//     GET /api/mesh shows it only when that answers ok, the 5 s poll asks
//     nothing until one has, and unlocking with a token asks once more.
//   - Peek (F274): the camera routes are registered only under
//     FEATURE_CAMERA_PEEK ([env:minimal] and [env:esp32-wroom] have none).
//     refreshPeekStatus() ignored the refusal, so the tab kept "Ready" and
//     "▶ Start", and Start answered an alert. A refused status before any
//     status has answered now shows the panel's no-camera state, says the
//     build has no preview, and hides the Camera Controls and Stream Metrics
//     cards, the only callers of the other camera routes; once a status has
//     answered, a failed poll changes nothing (a blip mid-stream is not a
//     missing camera).
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

// What api() returns for esp_http_server's 404 page (text, not JSON).
const NOT_FOUND = { ok: false, success: false, error: "Nothing matches the given URI" };

const settle = () => new Promise((r) => setImmediate(r));

// A stub DOM element, made on first use; `seed` gives an element the text
// the markup writes into it.
function dom(seed = {}) {
  const els = {};
  const el = (id) => els[id] || (els[id] = {
    id, style: {}, textContent: seed[id] || "", innerHTML: "", value: "", className: "",
    checked: false, disabled: false,
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

test("a refused GET /api/mesh at load keeps the Opera tab hidden and asks nothing more", async () => {
  const p = operaPage({});
  await p.ctx.__load();
  await settle();
  assert.strictEqual(p.el("navOpera").style.display, "none");
  assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh"]);
  // The 5 s poll asks nothing of the mesh while no status has answered,
  // even were the Opera panel current.
  p.ctx.currentPanel = "opera";
  for (let i = 0; i < 3; ++i) p.ctx.__panelPoll();
  await settle();
  assert.deepStrictEqual(meshCalls(p.calls), ["/api/mesh"]);
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

test("unlocking with a token asks a turned-away Opera probe once more", async () => {
  // [env:full] answers a tokenless page 401: refused, so hidden.
  const answers = { "/api/mesh": { ok: false, error: "unauthorized" } };
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
    document: { getElementById: el, querySelectorAll: () => [] },
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

test("a refused GET /api/peek/status shows the no-camera state and hides the camera cards", async () => {
  const p = peekPage({});
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
  answers["/api/peek/status"] = { ok: false, success: false, error: "Network error" };
  p.calls.length = 0;
  await p.ctx.__t.refreshPeekStatus();
  assert.strictEqual(p.el("peekToggle").textContent, "⏹ Stop", "still streaming");
  assert.strictEqual(p.el("peekToggle").disabled, false);
  assert.strictEqual(p.el("peekControlsCard").style.display, "");
  assert.strictEqual(p.el("camInfoCard").style.display, "");
  assert.ok(!p.calls.includes("stopCamInfoPolling"), "the metrics poll goes on");
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
  const record = { alerts: [], blobs: [], clicks: [], navigated: null, revoked: [] };
  class Blob {
    constructor(parts, opts) { this.text = parts.join(""); this.type = opts && opts.type; record.blobs.push(this); }
  }
  const ctx = {
    Blob,
    URL: { createObjectURL: (b) => `blob:${record.blobs.indexOf(b)}`, revokeObjectURL: (u) => record.revoked.push(u) },
    document: {
      createElement: () => ({ click() { record.clicks.push({ href: this.href, download: this.download }); }, remove() {} }),
      body: { appendChild() {} },
    },
    window: { location: { set href(v) { record.navigated = v; }, get href() { return ""; } } },
    alert: (m) => record.alerts.push(m),
    setTimeout: (fn) => fn(),
    api: async (url, method) => { record.asked = `${method} ${url}`; return answer; },
  };
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
  answer.device_id = "canary-s3-4dC2";
  answer.last_record = { seq: 7, hash: "ab".repeat(32), type: "BOOT", verified: true };
  const p = exportPage(answer);
  await p.ctx.__t.exportWitness();
  assert.strictEqual(p.record.asked, "POST /api/export");
  assert.deepStrictEqual(p.record.alerts, [], "no alert after a good export");
  assert.strictEqual(p.record.navigated, null, "the browser is not sent anywhere");
  assert.strictEqual(p.record.blobs.length, 1);
  assert.strictEqual(p.record.blobs[0].type, "application/json");
  assert.deepStrictEqual(JSON.parse(p.record.blobs[0].text), answer, "the file is the answer, every field");
  assert.deepStrictEqual(p.record.clicks, [{ href: "blob:0", download: "witness-export-canary-s3-4dC2.json" }]);
  assert.deepStrictEqual(p.record.revoked, ["blob:0"]);
});

test("a refused export alerts the device's reason and saves nothing", async () => {
  const p = exportPage({ ok: false, error: "forbidden" });
  await p.ctx.__t.exportWitness();
  assert.deepStrictEqual(p.record.alerts, ["Export failed: forbidden"]);
  assert.deepStrictEqual(p.record.blobs, []);
  assert.deepStrictEqual(p.record.clicks, []);
});
