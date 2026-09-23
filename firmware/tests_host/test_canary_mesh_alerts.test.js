// Host test for firmware/canary's Opera alert list
// (canary/lib/securacv_webui/src/securacv_webui.cpp, loadOperaAlerts) —
// repo sweep F33 part 7.
//
// GET /api/mesh/alerts reports each alert's `timestamp_ms` as the RECEIVER's
// uptime (millis()) at receipt — not a wall-clock time. The page used to pass
// it to new Date(ms).toLocaleTimeString(), which shows an uptime of 90 s as
// the clock time 90 s after the Unix epoch in the browser's zone (7:01:30 PM
// in New York): a made-up time of day. What this pins:
//   - the response's `uptime_ms` (mesh_api::build_mesh_alerts_json, pinned in
//     test_mesh_session) is what each timestamp is measured against, and the
//     row says "received <age> ago";
//   - the age is a u32 difference, like the firmware's, so it survives the
//     millis() wrap;
//   - a response without `uptime_ms` (or with a value that is not a u32) shows
//     no time at all rather than a guess;
//   - nothing on this path constructs a Date: `Date` is poisoned in the
//     sandbox, so a regression to formatTimestamp() fails here.
// The functions are lifted out of the C++ raw string by literal markers and
// run against a stub DOM and a recording api(), the way
// test_canary_timeline.test.js does it.
//
// Run: node --test firmware/tests_host/test_canary_mesh_alerts.test.js
// (CI: firmware.yml's "Build + run all mesh + ble_scan host tests" step, with
// the mesh C++ suites whose JSON it reads).

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

// escapeHtml and formatTimestamp (the page's wall-clock formatter, lifted so a
// regression that calls it runs into the poisoned Date below), then
// formatAlertAge and loadOperaAlerts up to the next function.
const code =
  slice("    function escapeHtml(str) {", "\n    }\n") + "\n    }\n" +
  slice("    function formatTimestamp(ms) {", "\n    }\n") + "\n    }\n" +
  slice("    function formatAlertAge(", "    async function startPairing(") +
  "\n;globalThis.__t = { formatAlertAge, loadOperaAlerts };\n";

function harness(answer) {
  const els = {};
  const el = (id) => els[id] || (els[id] = { id, style: {}, textContent: "", innerHTML: "" });
  const calls = [];
  const dateCalls = [];
  const PoisonedDate = function (...args) {
    dateCalls.push(args);
    throw new Error("an Opera alert's uptime timestamp must never become a Date");
  };
  PoisonedDate.now = () => { dateCalls.push(["now"]); throw new Error("Date.now on the alert path"); };
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

test("the age reads in seconds, minutes, hours and days", () => {
  const { t } = harness();
  const age = (ts, up) => t.formatAlertAge(ts, up);
  assert.strictEqual(age(1000, 1000), "received 0 s ago");
  assert.strictEqual(age(1000, 60999), "received 59 s ago");
  assert.strictEqual(age(90000, 150000), "received 1 min ago");
  assert.strictEqual(age(0, 3599999), "received 59 min ago");
  assert.strictEqual(age(0, 3600000), "received 1 h 0 min ago");
  assert.strictEqual(age(0, (2 * 60 + 5) * 60000), "received 2 h 5 min ago");
  assert.strictEqual(age(0, 25 * 3600000), "received 1 d 1 h ago");
});

test("the age is a u32 difference, so it survives the millis() wrap", () => {
  const { t } = harness();
  // Received 4 s before the wrap, asked 4 s after it: 8 s, not ~49.7 days
  // and not a negative number.
  assert.strictEqual(t.formatAlertAge(0xFFFFFFFF - 3999, 4000), "received 8 s ago");
});

test("without a usable uptime_ms there is no time at all", () => {
  const { t } = harness();
  for (const [ts, up] of [
    [90000, undefined], [90000, null], [90000, "150000"], [90000, -1],
    [90000, 1.5], [90000, 0x100000000], [undefined, 150000], [-5, 150000],
    [90000, NaN], [Infinity, 150000],
  ]) {
    assert.strictEqual(t.formatAlertAge(ts, up), "", `ts=${ts} up=${up}`);
  }
});

test("each row says how long ago it was received, and never builds a Date", async () => {
  const answer = {
    ok: true, count: 2, uptime_ms: 150000,
    alerts: [
      { timestamp_ms: 90000, type: "TAMPER", severity: 6, sender_fp: "1011121314151617",
        sender_name: "", detail: "camera_tamper", witness_seq: 4242 },
      { timestamp_ms: 1, type: "TAMPER", severity: 3, sender_fp: "ffffffffffffffff",
        sender_name: "", detail: "enclosure_tamper", witness_seq: 7 },
    ],
  };
  const { t, el, calls, dateCalls } = harness(answer);
  await t.loadOperaAlerts();
  assert.deepStrictEqual(calls, ["/api/mesh/alerts"]);
  const html = el("operaAlertsList").innerHTML;
  assert.match(html, /<div class="log-meta">received 1 min ago<\/div>/);
  assert.match(html, /<div class="log-meta">received 2 min ago<\/div>/);
  assert.match(html, /camera_tamper/);
  // No clock time, no epoch date: nothing that reads like 12:01:30 or 1970.
  assert.doesNotMatch(html, /\d{1,2}:\d{2}/);
  assert.doesNotMatch(html, /1970|1969/);
  assert.strictEqual(dateCalls.length, 0);
});

test("a response without uptime_ms renders the rows with no time line", async () => {
  const { t, el, dateCalls } = harness({
    ok: true, count: 1,
    alerts: [{ timestamp_ms: 90000, type: "TAMPER", severity: 6, sender_name: "", detail: "camera_tamper" }],
  });
  await t.loadOperaAlerts();
  const html = el("operaAlertsList").innerHTML;
  assert.match(html, /camera_tamper/);
  assert.doesNotMatch(html, /log-meta/);
  assert.strictEqual(dateCalls.length, 0);
});

test("an empty history shows the empty state", async () => {
  const { t, el } = harness({ ok: true, count: 0, uptime_ms: 5, alerts: [] });
  await t.loadOperaAlerts();
  assert.match(el("operaAlertsList").innerHTML, /No alerts from opera/);
});
