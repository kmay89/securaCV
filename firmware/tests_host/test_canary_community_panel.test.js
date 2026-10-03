// Host test for firmware/canary's dashboard Community (Chirp) tab
// (canary/lib/securacv_webui/src/securacv_webui.cpp: the Community nav
// button, refreshChirpStatus, the panel poll) — repo sweep F176.
//
// The page has a Community panel driven by GET /api/chirp,
// /api/chirp/recent, POST /api/chirp/send and the rest, but nothing under
// firmware/canary or firmware/common registers an /api/chirp route
// (firmware/common/chirp/ holds only a header). The tab was always shown:
// it read "Disabled", its toggle alerted a failure, and every 5 s on that
// tab the page asked for /api/chirp again and got a 404. What this pins:
//   - the Community nav button starts hidden in the markup;
//   - the page load's one GET /api/chirp, answered with anything but a
//     Chirp status (the server's 404 page comes back from api() as
//     {ok: false, error: ...}), leaves it hidden and asks for nothing else;
//   - a Chirp status shows the tab and loads the recent list, so a firmware
//     that serves the routes gets the panel back with no page change;
//   - the 5 s panel poll does not ask for /api/chirp while no route has
//     answered, even with the Community panel current;
//   - and, so the premise stays true, no source under firmware/canary or
//     firmware/common registers an /api/chirp route: when one does, this
//     case fails and the gate can go.
// The functions are lifted out of the C++ raw string by literal markers and
// run against a stub DOM and a recording api(), the way
// test_canary_mesh_alerts.test.js does it.
//
// Run: node --test firmware/tests_host/test_canary_community_panel.test.js
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

// The Chirp state and refreshChirpStatus through loadChirps, then the
// page's 5 s panel poll, lifted as a function.
const chirp = slice("    let chirpState = null;", "    function formatChirpAge(sec) {");
const poll = slice("    setInterval(() => {\n      if (currentPanel === 'logs') loadLogs();", "    }, 5000);");
const code =
  chirp +
  "\n    const __panelPoll = " + poll.replace(/^\s*setInterval\(/, "") + "};\n" +
  ";globalThis.__t = { refreshChirpStatus, panelPoll: __panelPoll };\n";

// The Community nav button as the page's markup writes it.
function navButton() {
  const m = src.match(/<button\b[^>]*data-panel="community"[^>]*>/);
  assert.ok(m, "the Community nav button is in the page");
  return m[0];
}

function page(answers) {
  const els = {};
  const el = (id) => els[id] || (els[id] = { id, style: {}, textContent: "", innerHTML: "", checked: false, disabled: false });
  // The nav button starts as the markup writes it.
  const hidden = /style="display:\s*none;?"/.test(navButton());
  el("navCommunity").style.display = hidden ? "none" : "";
  const calls = [];
  const ctx = {
    document: { getElementById: el, querySelectorAll: () => [] },
    escapeHtml: (s) => String(s),
    currentPanel: "status",
    loadLogs: () => calls.push("loadLogs"),
    loadWitness: () => calls.push("loadWitness"),
    refreshOpera: () => calls.push("refreshOpera"),
    refreshBtStatus: () => calls.push("refreshBtStatus"),
    api: async (url) => {
      calls.push(url);
      const a = answers[url];
      assert.ok(a !== undefined, `the page asked for ${url}`);
      return typeof a === "function" ? a() : a;
    },
  };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  return { t: ctx.__t, el, calls, ctx };
}

// What api() returns for esp_http_server's 404 page (text, not JSON).
const NOT_FOUND = { ok: false, success: false, error: "Nothing matches the given URI" };
const CHIRP_STATUS = {
  state: "ACTIVE", enabled: true, session_emoji: "🐦", nearby_count: 2, recent_chirps: 0,
  can_send: true, muted: false, relay_enabled: true,
};

test("the Community nav button starts hidden", () => {
  assert.match(navButton(), /style="display:\s*none;?"/);
  assert.match(navButton(), /id="navCommunity"/);
});

test("a 404 from GET /api/chirp keeps the tab hidden and asks for nothing more", async () => {
  const p = page({ "/api/chirp": NOT_FOUND });
  await p.t.refreshChirpStatus();
  assert.strictEqual(p.el("navCommunity").style.display, "none");
  assert.deepStrictEqual(p.calls, ["/api/chirp"]);
});

test("a Chirp status shows the tab and loads the recent list", async () => {
  const p = page({ "/api/chirp": CHIRP_STATUS, "/api/chirp/recent": { chirps: [] } });
  await p.t.refreshChirpStatus();
  assert.strictEqual(p.el("navCommunity").style.display, "");
  assert.ok(p.calls.includes("/api/chirp/recent"));
});

test("the panel poll asks for no Chirp status while no route has answered", async () => {
  const p = page({ "/api/chirp": NOT_FOUND });
  await p.t.refreshChirpStatus();                  // the page load's one call
  p.ctx.currentPanel = "community";                // even were the panel current
  for (let i = 0; i < 3; ++i) p.t.panelPoll();
  assert.deepStrictEqual(p.calls, ["/api/chirp"]);
  // Once a route answers, the poll refreshes it on that panel as before.
  const q = page({ "/api/chirp": CHIRP_STATUS, "/api/chirp/recent": { chirps: [] } });
  await q.t.refreshChirpStatus();
  q.ctx.currentPanel = "community";
  q.calls.length = 0;
  q.t.panelPoll();
  assert.deepStrictEqual(q.calls, ["/api/chirp"]);
  // Other panels poll as they did.
  q.ctx.currentPanel = "logs";
  q.calls.length = 0;
  q.t.panelPoll();
  assert.deepStrictEqual(q.calls, ["loadLogs"]);
});

test("nothing in the PlatformIO tree serves /api/chirp (the premise)", () => {
  const found = [];
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      const path = join(dir, name);
      const st = statSync(path);
      if (st.isDirectory()) { if (name !== ".pio" && name !== "node_modules") walk(path); continue; }
      if (!/\.(c|cc|cpp|h|hpp)$/.test(name) || path === UI) continue;
      const text = readFileSync(path, "utf8");
      if (/\.uri\s*=\s*"\/api\/chirp/.test(text) || /"\/api\/chirp[^"]*"\s*,\s*HTTP_/.test(text)) found.push(path);
    }
  };
  walk(join(FW, "canary"));
  walk(join(FW, "common"));
  assert.deepStrictEqual(found, [],
    "an /api/chirp route is registered now: drop the F176 gate and this case");
});
