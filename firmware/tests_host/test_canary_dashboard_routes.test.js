// Host test for firmware/canary's dashboard: it calls only routes the
// PlatformIO tree serves (canary/lib/securacv_webui/src/securacv_webui.cpp:
// the Bluetooth tab, the Wi-Fi card, the Settings storage card) — repo sweep
// F198, F176's shape.
//
// The page called GET /api/bluetooth and the routes under it (three of them
// at every page load, /api/bluetooth every 5 s on that tab), GET /api/wifi
// (at load and every 5 s), POST /api/wifi/forget and POST /api/logs/rotate,
// and nothing under firmware/canary or firmware/common registers any of
// them: each answered 404, so the Wi-Fi card read "Checking..." for good and
// its buttons and the Bluetooth tab's controls failed. What this pins:
//   - the Bluetooth nav button starts hidden; the page load's one
//     GET /api/bluetooth, answered with anything but a Bluetooth status,
//     leaves it hidden and asks for nothing else; a status shows it and
//     loads its settings and paired list; the 5 s poll asks for nothing
//     while no route has answered;
//   - the Wi-Fi card reads GET /api/wifi/status and shows what that body
//     carries (it names no network and has no `configured`: a saved network
//     is read from the state), Forget reaches POST /api/wifi/disconnect,
//     the route that clears the saved network, and a connect's success
//     names the network that was entered;
//   - every route the page names is one a source under firmware/canary or
//     firmware/common registers, method included where the call names one,
//     matched the way esp_http_server's httpd_uri_match_wildcard matches
//     (a `*` counts only at a template's end), except the gated Chirp and
//     Bluetooth routes and the one gap named in KNOWN_UNMATCHED;
//   - and, so the gates stay true, nothing registers an /api/bluetooth
//     route: when one does, that case fails and the gate can go.
// The functions are lifted out of the C++ raw string by literal markers and
// run against a stub DOM and a recording api(), the way
// test_canary_community_panel.test.js does it.
//
// Run: node --test firmware/tests_host/test_canary_dashboard_routes.test.js
// (the Makefile's `run` target does, after the C++ suites).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync, statSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const FW = join(__dirname, "..");
const UI = process.env.CANARY_WEBUI || join(FW, "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
const src = readFileSync(UI, "utf8");

function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}

// The page itself: the raw string the firmware serves.
function pageText() {
  const a = src.indexOf('R"rawliteral(');
  const b = src.indexOf(')rawliteral"', a);
  assert.ok(a >= 0 && b > a, "the page's raw string is in the file");
  return src.slice(a, b);
}

// What api() returns for esp_http_server's 404 page (text, not JSON).
const NOT_FOUND = { ok: false, success: false, error: "Nothing matches the given URI" };

// A stub DOM element, made on first use.
function dom() {
  const els = {};
  const el = (id) => els[id] || (els[id] = {
    id, style: {}, textContent: "", innerHTML: "", value: "", placeholder: "",
    checked: false, disabled: false, className: "",
    addEventListener() {},
  });
  return el;
}

// ── The Bluetooth tab ────────────────────────────────────────────────────

const bt = slice("    let btState = null;", "    function formatDuration(seconds) {");
const load = slice("    refreshLockBanner();\n    refreshStatus();", "    setInterval(refreshStatus, 2000);");
const poll = slice("    setInterval(() => {\n      if (currentPanel === 'logs') loadLogs();", "    }, 5000);");

function navButton(panel) {
  const m = src.match(new RegExp(`<button\\b[^>]*data-panel="${panel}"[^>]*>`));
  assert.ok(m, `the ${panel} nav button is in the page`);
  return m[0];
}

// The page with the Bluetooth section live and every other loader a stub;
// api() answers from `answers` (a 404 for anything not listed) and records
// each request.
function btPage(answers) {
  const el = dom();
  el("navBluetooth").style.display = /style="display:\s*none;?"/.test(navButton("bluetooth")) ? "none" : "";
  const calls = [];
  const ctx = {
    document: { getElementById: el, querySelectorAll: () => [] },
    currentPanel: "status",
    chirpServed: false,
    alert() {}, confirm: () => true, setInterval: () => 0, clearInterval() {}, setTimeout: () => 0,
    formatBytes: (n) => String(n), formatDuration: (n) => String(n),
    api: async (url, method = "GET") => {
      calls.push(method === "GET" ? url : `${method} ${url}`);
      const a = answers[url];
      return a === undefined ? NOT_FOUND : typeof a === "function" ? a() : a;
    },
  };
  for (const stub of ["refreshLockBanner", "refreshStatus", "refreshLiveSensing", "loadChain",
                      "loadWifiStatus", "refreshOpera", "refreshChirpStatus", "updateResolutionUI",
                      "loadLogs", "loadWitness"]) {
    ctx[stub] = () => calls.push(stub);
  }
  vm.createContext(ctx);
  vm.runInContext(
    bt +
    "\n;globalThis.__load = async () => {\n" + load + "\n};\n" +
    "globalThis.__panelPoll = " + poll.replace(/^\s*setInterval\(/, "") + "};\n" +
    "globalThis.__t = { refreshBtStatus };\n",
    ctx);
  return { ctx, el, calls };
}

const settle = () => new Promise((r) => setImmediate(r));
const btCalls = (calls) => calls.filter((c) => c.includes("/api/bluetooth"));

const BT_STATUS = {
  state: "advertising", enabled: true, advertising: true, connected: false,
  device_name: "SecuraCV-Canary", local_address: "aa:bb:cc:dd:ee:ff", tx_power: 3,
  paired_count: 0, stats: { total_connections: 0 }, pairing: { state: "none" },
};

test("the Bluetooth nav button starts hidden", () => {
  assert.match(navButton("bluetooth"), /style="display:\s*none;?"/);
  assert.match(navButton("bluetooth"), /id="navBluetooth"/);
});

test("a 404 from GET /api/bluetooth at load keeps the tab hidden and asks for nothing more", async () => {
  const p = btPage({});
  await p.ctx.__load();
  await settle();
  assert.strictEqual(p.el("navBluetooth").style.display, "none");
  assert.deepStrictEqual(btCalls(p.calls), ["/api/bluetooth"]);
});

test("a Bluetooth status shows the tab and loads its settings and paired list", async () => {
  const p = btPage({
    "/api/bluetooth": BT_STATUS,
    "/api/bluetooth/settings": { enabled: true, auto_advertise: true, allow_pairing: true,
                                 require_pin: true, notify_on_connect: false, inactivity_timeout_sec: 300 },
    "/api/bluetooth/paired": { count: 0, devices: [] },
  });
  await p.ctx.__load();
  await settle();
  assert.strictEqual(p.el("navBluetooth").style.display, "");
  assert.deepStrictEqual(btCalls(p.calls).sort(),
    ["/api/bluetooth", "/api/bluetooth/paired", "/api/bluetooth/settings"]);
  assert.strictEqual(p.el("btStateVal").textContent, "advertising");
  // A second status read loads nothing again.
  p.calls.length = 0;
  await p.ctx.__t.refreshBtStatus();
  assert.deepStrictEqual(btCalls(p.calls), ["/api/bluetooth"]);
});

test("the panel poll asks for no Bluetooth status while no route has answered", async () => {
  const p = btPage({});
  await p.ctx.__load();
  await settle();
  p.calls.length = 0;
  p.ctx.currentPanel = "bluetooth";          // even were the panel current
  for (let i = 0; i < 3; ++i) p.ctx.__panelPoll();
  await settle();
  assert.deepStrictEqual(btCalls(p.calls), []);
  // Once a route answers, the poll refreshes it on that panel as before.
  const q = btPage({ "/api/bluetooth": BT_STATUS });
  await q.ctx.__t.refreshBtStatus();
  await settle();
  q.calls.length = 0;
  q.ctx.currentPanel = "bluetooth";
  q.ctx.__panelPoll();
  assert.deepStrictEqual(btCalls(q.calls), ["/api/bluetooth"]);
});

// ── The Wi-Fi card ───────────────────────────────────────────────────────

const wifi = slice("    let wifiState = null;", "    // SSID select -> input sync");

function wifiPage(statuses, extra = {}) {
  const el = dom();
  const calls = [];
  const alerts = [];
  let n = 0;
  const timers = [];
  const ctx = {
    document: { getElementById: el },
    alert: (m) => alerts.push(m),
    confirm: () => true,
    setInterval: (fn) => { timers.push(fn); return timers.length; },
    clearInterval: () => {},
    setTimeout: () => 0,
    api: async (url, method = "GET", body = null) => {
      calls.push(method === "GET" ? url : `${method} ${url}`);
      if (url === "/api/wifi/status") return statuses[Math.min(n++, statuses.length - 1)];
      if (url in extra) return extra[url];
      return NOT_FOUND;
    },
  };
  vm.createContext(ctx);
  vm.runInContext(wifi + "\n;globalThis.__t = { loadWifiStatus, forgetWifi, connectWifi };\n", ctx);
  return { t: ctx.__t, el, calls, alerts, timers };
}

// GET /api/wifi/status bodies as handle_wifi_status writes them.
const statusBody = (state, extra = {}) => Object.assign({
  ok: true, state, ap_active: true, sta_connected: false, ap_ip: "192.168.4.1",
  ap_clients: 0, ap_auth: "wpa2", ap_auth_reason: "", sta_pmf: false,
}, extra);
const CONNECTED = statusBody("connected", { sta_connected: true, sta_ip: "10.0.0.7", rssi: -55, ap_active: false });

test("the Wi-Fi card reads GET /api/wifi/status and shows what it carries", async () => {
  let p = wifiPage([CONNECTED]);
  await p.t.loadWifiStatus();
  assert.deepStrictEqual(p.calls, ["/api/wifi/status"]);
  assert.strictEqual(p.el("wifiState").textContent, "Connected");
  assert.strictEqual(p.el("wifiStaIp").textContent, "10.0.0.7");
  assert.strictEqual(p.el("wifiStaSsid").textContent, "Saved", "a network is saved; the body names none");
  assert.strictEqual(p.el("wifiApSsid").textContent, "Off", "the AP's state, not a name the body lacks");
  assert.strictEqual(p.el("wifiApIp").textContent, "192.168.4.1");
  assert.strictEqual(p.el("wifiDisconnectBtn").style.display, "inline-flex");
  assert.strictEqual(p.el("wifiForgetBtn").style.display, "none", "Disconnect already clears it");

  // Saved but failing: Failed, and Forget is offered.
  p = wifiPage([statusBody("failed")]);
  await p.t.loadWifiStatus();
  assert.strictEqual(p.el("wifiState").textContent, "Failed");
  assert.strictEqual(p.el("wifiStaSsid").textContent, "Saved");
  assert.strictEqual(p.el("wifiApSsid").textContent, "On");
  assert.strictEqual(p.el("wifiForgetBtn").style.display, "inline-flex");
  assert.strictEqual(p.el("wifiDisconnectBtn").style.display, "none");

  // The link just dropped, the state not yet moved on: saved, Disconnected.
  p = wifiPage([statusBody("connected", { ap_active: false })]);
  await p.t.loadWifiStatus();
  assert.strictEqual(p.el("wifiState").textContent, "Disconnected");
  assert.strictEqual(p.el("wifiForgetBtn").style.display, "inline-flex");

  // Nothing saved.
  p = wifiPage([statusBody("ap_only")]);
  await p.t.loadWifiStatus();
  assert.strictEqual(p.el("wifiState").textContent, "AP Only");
  assert.strictEqual(p.el("wifiStaSsid").textContent, "Not configured");
  assert.strictEqual(p.el("wifiForgetBtn").style.display, "none");

  // A body that does carry the names and `configured` is read as before.
  p = wifiPage([statusBody("idle", { ap_ssid: "Canary-1234", sta_ssid: "home", configured: true })]);
  await p.t.loadWifiStatus();
  assert.strictEqual(p.el("wifiApSsid").textContent, "Canary-1234");
  assert.strictEqual(p.el("wifiStaSsid").textContent, "home");
  assert.strictEqual(p.el("wifiState").textContent, "Disconnected");

  // An error body leaves the card alone.
  p = wifiPage([NOT_FOUND]);
  p.el("wifiState").textContent = "Checking...";
  await p.t.loadWifiStatus();
  assert.strictEqual(p.el("wifiState").textContent, "Checking...");
});

test("Forget reaches the route that clears the saved network", async () => {
  const p = wifiPage([statusBody("failed"), statusBody("ap_only")],
                     { "/api/wifi/disconnect": { ok: true, message: "Disconnected from home WiFi" } });
  await p.t.forgetWifi();
  assert.deepStrictEqual(p.calls, ["POST /api/wifi/disconnect", "/api/wifi/status"]);
  assert.deepStrictEqual(p.alerts, [], "no failure is reported");
});

test("a connect's success names the network that was entered", async () => {
  const p = wifiPage([CONNECTED], { "/api/wifi/connect": { ok: true, ssid: "home" } });
  p.el("wifiSsidInput").value = "home";
  p.el("wifiPassword").value = "pw";
  await p.t.connectWifi();
  assert.deepStrictEqual(p.calls, ["POST /api/wifi/connect"]);
  assert.strictEqual(p.timers.length, 1, "it polls the status");
  await p.timers[0]();
  assert.deepStrictEqual(p.calls.slice(1), ["/api/wifi/status"]);
  assert.strictEqual(p.alerts.length, 1);
  assert.match(p.alerts[0], /^Successfully connected to home!/);
  assert.match(p.alerts[0], /IP: 10\.0\.0\.7/);
});

// ── Every route the page names is served ─────────────────────────────────

// The routes a source registers: `{ .uri = "...", .method = HTTP_X, ... }`.
// The catch-all "/*" registrations (the port-80 server's HTTPS redirect and
// the captive fallback) serve no API, so they answer for no route here.
function registrations() {
  const out = [];
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      const path = join(dir, name);
      const st = statSync(path);
      if (st.isDirectory()) { if (name !== ".pio" && name !== "node_modules") walk(path); continue; }
      if (!/\.(c|cc|cpp|h|hpp|ino)$/.test(name) || path === UI) continue;
      const text = readFileSync(path, "utf8");
      for (const m of text.matchAll(/\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*HTTP_(\w+)/g)) {
        if (m[1] !== "/*") out.push({ uri: m[1], method: m[2], path });
      }
    }
  };
  walk(join(FW, "canary"));
  walk(join(FW, "common"));
  return out;
}

// esp_http_server's httpd_uri_match_wildcard, the canary servers' matcher:
// a `*` at the end of a template matches anything after it, and anywhere
// else it is a literal character.
function matches(template, uri) {
  if (template.endsWith("*")) return uri.startsWith(template.slice(0, -1));
  return template === uri;
}

// Every /api/... string literal in the page's script: the path (a template
// literal's ${...} read as one segment, `1`), and the method when the
// literal is the first argument of an api() call that names one or of a
// plain api(...) (GET).
function pageRoutes() {
  const text = pageText();
  const out = [];
  for (const m of text.matchAll(/(['`])(\/api\/[^'`?]*)/g)) {
    const raw = m[2];
    const route = raw.replace(/\$\{[^}]*\}/g, "1");
    const before = text.slice(Math.max(0, m.index - 4), m.index);
    let method = null;
    if (before.endsWith("api(")) {
      const close = text.indexOf(m[1], m.index + 1);
      const after = text.slice(close + 1, close + 40);
      const named = after.match(/^\s*,\s*'([A-Z]+)'/);
      method = named ? named[1] : /^\s*\)/.test(after) ? "GET" : null;
    }
    out.push({ raw, route, method, at: text.slice(0, m.index).split("\n").length });
  }
  return out;
}

// Gated behind a status read that answers on a firmware serving them (F176,
// F198); premise cases below hold that nothing serves them today.
const GATED = ["/api/chirp", "/api/bluetooth"];
// A route the page names that the tree registers in a form the matcher does
// not take: POST /api/logs/<seq>/ack against "/api/logs/*/ack" (a `*` that is
// not the template's last character is a literal to httpd_uri_match_wildcard).
// Handed up from F198; not run on a device.
const KNOWN_UNMATCHED = ["POST /api/logs/1/ack"];

test("every route the page names is one the tree serves", () => {
  const regs = registrations();
  assert.ok(regs.length > 50, "the registrations were found");
  const missing = [];
  for (const r of pageRoutes()) {
    if (GATED.some((g) => r.route === g || r.route.startsWith(g + "/"))) continue;
    const served = regs.filter((g) => matches(g.uri, r.route) && (r.method === null || g.method === r.method));
    const key = `${r.method || "ANY"} ${r.route}`.replace(/^ANY /, "");
    if (served.length === 0 && !KNOWN_UNMATCHED.includes(key)) missing.push(`${key} (page line ${r.at}: ${r.raw})`);
  }
  assert.deepStrictEqual(missing, [], "the page names routes nothing serves");
  // The known gap is still a gap (drop it from the list the day it is not).
  for (const k of KNOWN_UNMATCHED) {
    const [method, route] = k.split(" ");
    assert.ok(pageRoutes().some((r) => r.route === route && r.method === method), `the page still names ${k}`);
    assert.ok(!regs.some((g) => matches(g.uri, route) && g.method === method), `${k} is served now`);
  }
});

test("nothing in the PlatformIO tree serves /api/bluetooth (the premise)", () => {
  const found = registrations().filter((g) => g.uri === "/api/bluetooth" || g.uri.startsWith("/api/bluetooth/"));
  assert.deepStrictEqual(found.map((g) => `${g.method} ${g.uri} (${g.path})`), [],
    "an /api/bluetooth route is registered now: drop the F198 gate and this case");
});
