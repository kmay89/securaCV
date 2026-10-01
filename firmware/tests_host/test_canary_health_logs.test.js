// Host test for firmware/canary's health-log list
// (canary/lib/securacv_webui/src/securacv_webui.cpp, loadLogs) —
// repo sweep F49 part 1.
//
// GET /api/logs reports each entry's `timestamp_ms` as this device's uptime
// (millis()) when the line was logged — not a wall-clock time. The page used
// to pass it to new Date(ms).toLocaleTimeString(), the same made-up time of
// day F33 part 7 took out of the Opera alert list. What this pins:
//   - the response's `uptime_ms` (handle_logs) is what each timestamp is
//     measured against, and the row's meta says "<age> ago";
//   - the age is a u32 difference, like the firmware's, so it survives the
//     millis() wrap;
//   - a response without `uptime_ms` (or with a value that is not a u32)
//     shows no time at all rather than a guess — the meta still carries the
//     category and the #seq;
//   - nothing on this path constructs a Date: `Date` is poisoned in the
//     sandbox, so a regression to formatTimestamp() fails here.
// The functions are lifted out of the C++ raw string by literal markers and
// run against a stub DOM and a recording api(), the way
// test_canary_mesh_alerts.test.js does it.
//
// Run: node --test firmware/tests_host/test_canary_health_logs.test.js
// (the Makefile's `run` target does, after the C++ suites).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const UI = join(__dirname, "..", "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
const src = readFileSync(UI, "utf8");

function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}

// escapeHtml and formatTimestamp (the page's wall-clock formatter, lifted so
// a regression that calls it runs into the poisoned Date below), the page's
// log-filter state, formatLogAge (defined beside formatAlertAge, which
// shares it), then loadLogs through getLevelClass.
const code =
  slice("    function escapeHtml(str) {", "\n    }\n") + "\n    }\n" +
  slice("    function formatTimestamp(ms) {", "\n    }\n") + "\n    }\n" +
  slice("    let logFilter = 'all';", "\n") + "\n" +
  slice("    function formatLogAge(", "    async function loadOperaAlerts(") +
  slice("    async function loadLogs() {", "    // Witness records") +
  "\n;globalThis.__t = { formatLogAge, loadLogs };\n";

function harness(answer) {
  const els = {};
  const el = (id) => els[id] || (els[id] = { id, style: {}, textContent: "", innerHTML: "" });
  const calls = [];
  const dateCalls = [];
  const PoisonedDate = function (...args) {
    dateCalls.push(args);
    throw new Error("a health log's uptime timestamp must never become a Date");
  };
  const ctx = {
    document: {
      getElementById: el,
      createElement: () => ({
        set textContent(v) { this._t = String(v); },
        get innerHTML() {
          return this._t.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
        },
      }),
    },
    api: async (url) => { calls.push(url); return answer; },
    Date: PoisonedDate,
    console,
  };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  return { t: ctx.__t, el, calls, dateCalls };
}

const entry = (seq, timestamp_ms, extra) => Object.assign({
  seq, timestamp_ms, level: 2, level_name: "INFO", category: "SYSTEM",
  message: "m" + seq, ack_status: "acknowledged",
}, extra);

test("a log row shows its age against the response's uptime, never a date", async () => {
  const h = harness({
    ok: true, uptime_ms: 10000000, logs: [
      entry(3, 10000000 - 8000),               // 8 s ago
      entry(2, 10000000 - 90 * 60000),         // 1 h 30 min ago
      entry(1, 10000000 - 97000),              // 97 s -> 1 min ago
    ],
  });
  await h.t.loadLogs();
  assert.deepStrictEqual(h.calls, ["/api/logs"]);
  const html = h.el("logList").innerHTML;
  assert.match(html, /SYSTEM · 8 s ago · #3/);
  assert.match(html, /SYSTEM · 1 h 30 min ago · #2/);
  assert.match(html, /SYSTEM · 1 min ago · #1/);
  assert.strictEqual(h.dateCalls.length, 0);
});

test("the age is a u32 difference, so it survives a single millis() wrap", () => {
  const h = harness({ ok: true, logs: [] });
  // Logged 4 s before the wrap, read 4 s after it: 8 s, not -4 billion.
  assert.strictEqual(h.t.formatLogAge(0xFFFFFFFF - 3999, 4000), "8 s ago");
  assert.strictEqual(h.t.formatLogAge(0, 0), "0 s ago");
  assert.strictEqual(h.t.formatLogAge(0, 36 * 3600000), "1 d 12 h ago");
});

test("past one millis() period the age is omitted, not guessed", () => {
  const h = harness({ ok: true, logs: [] });
  const PERIOD = 0x100000000;  // 2^32 ms ~= 49.7 days
  // Uptime just over one period: a u32 entry timestamp can't be placed in
  // its wrap epoch, so no age is shown rather than a misleading small one.
  assert.strictEqual(h.t.formatLogAge(1000, PERIOD + 60000), "");
  assert.strictEqual(h.t.formatLogAge(0xFFFFFFFF, PERIOD + 1), "");
  // Right up to the boundary it is still exact.
  assert.strictEqual(h.t.formatLogAge(0, 0xFFFFFFFF), "49 d 17 h ago");
});

test("a log row past one period shows category and seq but no age", async () => {
  const h = harness({
    ok: true, uptime_ms: 0x100000000 + 5 * 60000,  // ~49.7 days + 5 min up
    logs: [entry(9, 1000)],
  });
  await h.t.loadLogs();
  const html = h.el("logList").innerHTML;
  assert.match(html, /SYSTEM · #9/);
  assert.doesNotMatch(html, /ago/);
  assert.strictEqual(h.dateCalls.length, 0);
});

test("no uptime_ms (or a non-u32 one) shows no time at all, not a guess", async () => {
  for (const uptime of [undefined, null, -1, 0x100000000, 1.5, "9000"]) {
    const h = harness({ ok: true, uptime_ms: uptime, logs: [entry(7, 1000)] });
    await h.t.loadLogs();
    const html = h.el("logList").innerHTML;
    assert.match(html, /SYSTEM · #7/, `uptime=${uptime}`);
    assert.doesNotMatch(html, /ago/, `uptime=${uptime}`);
    assert.strictEqual(h.dateCalls.length, 0, `uptime=${uptime}`);
  }
});

test("a message is escaped on its way into the row", async () => {
  const h = harness({
    ok: true, uptime_ms: 5000,
    logs: [entry(1, 4000, { message: "<img src=x>" })],
  });
  await h.t.loadLogs();
  const html = h.el("logList").innerHTML;
  assert.match(html, /&lt;img src=x&gt;/);
  assert.doesNotMatch(html, /<img/);
});
