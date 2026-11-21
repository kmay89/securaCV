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
// The same file also holds the page's ledger of raw splices into markup
// (sweep F443, at the end), beside the WAP page's in web_ui_logic.test.js.
//
// Run: node --test firmware/tests_host/test_canary_mesh_alerts.test.js
// (CI: firmware.yml's "Build + run all mesh + ble_scan host tests" step, with
// the mesh C++ suites whose JSON it reads).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

// CANARY_WEB_UI points it at another copy of the page (a planted one, to
// watch the splice ledger below fail).
const UI = process.env.CANARY_WEB_UI ||
  join(__dirname, "..", "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
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

// ── Raw splices into markup: the flagship page's ledger (sweep F443) ────
//
// The flagship's securacv_webui.cpp splices device vocabulary, fingerprints
// and numbers into markup as they came, the WAP page's shapes (its ledger and
// what this one means are in web_ui_logic.test.js; what counts as a splice is
// html_splices.js's header). F443 changed none of them; whether to move them
// all to escapeHtml() and a row index is its decision. Until then the set is
// held where it stands: a new raw splice fails, and so does a recorded one
// that is gone, until its line is lowered. The script is read out of the C++
// raw string as it ships.
const splices = require("../projects/canary-wap/arduino/canary_wap/html_splices.js");
const FLAGSHIP_RAW_SPLICES = Object.freeze({
  // Named by F443 (the chirp row is unserved, F176).
  "refreshSensing | text | e.type": 1,
  "loadLogs | on:onclick | log.seq": 1,
  "loadPeers | text | peer.fingerprint": 1,
  "loadPeers | on:onclick | peer.fingerprint": 1,
  "loadChirps | text | chirp.emoji": 1,
  "loadChirps | text | chirp.urgency": 1,
  "loadChirps | text | chirp.category": 1,
  "loadChirps | on:onclick | chirp.nonce": 2,
  // Found by the scanner, not in F443's list.
  "loadChirps | text | chirp.hop_count": 1,
  "loadChirps | text | chirp.confirm_count": 2,
  "refreshSensing | text | e.zone": 1,
  "refreshSensing | text | e.confidence": 1,
  "loadChain | text | truncateHash(b.hash, 8)": 1,
  "loadChain | text | b.seq": 1,
  "loadLogs | attr:class | getLevelClass(log.level)": 1,
  "loadLogs | text | log.level_name": 1,
  "loadLogs | text | [log.category, formatLogAge(log.timestamp_ms, data.uptime_ms), '#' + log.seq].filter(Boolean).join(' · ')": 1,
  "loadWitness | text | r.seq": 1,
  "loadWitness | text | r.type_name": 1,
  "loadWitness | text | truncateHash(r.chain_hash, 16)": 1,
  "loadWitness | text | r.time_bucket": 1,
  "loadWitness | text | r.payload_len": 1,
  "refreshTimelineHero | attr:src | URL.createObjectURL(blob)": 1,
  "renderTimeline | attr:class | typeInfo.css": 1,
  "renderTimeline | text | typeInfo.icon": 1,
  "renderTimeline | text | typeInfo.name": 1,
  "renderTimeline | attr:class | badgeCss": 1,
  "loadPeers | text | peer.state": 1,
  "loadPeers | text | peer.rssi": 1,
  "loadPeers | text | peer.last_seen_sec": 1,
  "loadOperaAlerts | text | formatAlertAge(alert.timestamp_ms, data.uptime_ms)": 1,
  "loadOperaAlerts | text | alert.type": 1,
  "loadChirps | text | formatChirpAge(chirp.age_sec)": 1,
  "refreshBtStatus | text | data.tx_power": 1,
  "formatBytes | text | bytes": 1,
});

test("the page splices nothing raw into markup beyond its ledger (F443)", () => {
  const { added, gone } = splices.against(FLAGSHIP_RAW_SPLICES, splices.rawSplices(src));
  assert.deepStrictEqual(added, [], "a new raw splice into markup: pass it through escapeHtml(), or name its row " +
                         "by an index in an on*= handler, rather than adding it to FLAGSHIP_RAW_SPLICES");
  assert.deepStrictEqual(gone, [], "a recorded raw splice is gone: lower its line in FLAGSHIP_RAW_SPLICES");
});

test("a raw splice planted in the C++ raw string's script is found (F443)", () => {
  const planted = (js) => src.replace("\n  </script>", `\n${js}\n  </script>`);
  for (const [js, entry] of [
    ["function plantA(x) { el.innerHTML = '<b>' + x.name + '</b>'; }", "plantA | text | x.name"],
    ["function plantB(p) { return `<button onclick=\"drop('${p.fp}')\">x</button>`; }", "plantB | on:onclick | p.fp"],
    ["function plantC(rows) { list.innerHTML = rows.map(r => `<li>\n            ${r.label}\n          </li>`).join(''); }", "plantC | text | r.label"],
    ["function plantD(xs) { let h = ''; for (const x of xs) h += '<td>' + x.v + '</td>'; return h; }", "plantD | text | x.v"],
    // Built one `+=` statement at a time, the page's own idiom: the value on
    // a line of its own, and a handler opened by one statement and filled by
    // the next (escapeHtml() is decoded away before the handler runs).
    ["function plantE(xs) { let h = '<ul>'; xs.forEach(x => { h += '<li>'; h += x.n; h += '</li>'; }); list.innerHTML = h; }",
     "plantE | text | x.n"],
    ["function plantF(x) { let h = ''; h += '<a onclick=\"go(\\''; h += escapeHtml(x.id) + '\\')\">x</a>'; list.innerHTML = h; }",
     "plantF | on:onclick | escapeHtml(x.id)"],
    // A const that opens a handler, and an array literal joined.
    ["function plantG(x) { const open = '<a onclick=\"go(\\''; el.innerHTML = open + escapeHtml(x.id) + '\\')\">x</a>'; }",
     "plantG | on:onclick | escapeHtml(x.id)"],
    ["function plantH(x) { return ['<td>', x.v, '</td>'].join(''); }", "plantH | text | x.v"],
  ]) {
    const text = planted(js);
    assert.notStrictEqual(text, src, "the script's end is in the page");
    const { added, gone } = splices.against(FLAGSHIP_RAW_SPLICES, splices.rawSplices(text));
    assert.ok(added.some((a) => a.startsWith(entry + " (")), `${entry}: ${JSON.stringify(added)}`);
    assert.deepStrictEqual(gone, [], entry);
  }
  // An escape undone in the page's own rows by splitting the value onto a
  // `+=` line of its own is new.
  const row = "html += '<strong>' + escapeHtml(dev.name || 'Unknown') + '</strong> ' + trustBadge + blockBadge;";
  assert.strictEqual(src.split(row).length, 2, "the paired-device row is in the page once");
  const split = src.replace(row, "html += '<strong>'; html += dev.name || 'Unknown'; html += '</strong> ' + trustBadge + blockBadge;");
  assert.deepStrictEqual(splices.against(FLAGSHIP_RAW_SPLICES, splices.rawSplices(split)),
                         { added: ["loadBtPairedDevices | text | dev.name (1 now, 0 recorded)"], gone: [] });
  // And one that is gone fails until its line is lowered.
  const fixed = src.replace("FP: ${peer.fingerprint || '--'}", "FP: ${escapeHtml(peer.fingerprint || '--')}");
  assert.notStrictEqual(fixed, src, "the fingerprint line is in the page");
  assert.deepStrictEqual(splices.against(FLAGSHIP_RAW_SPLICES, splices.rawSplices(fixed)).gone,
                         ["loadPeers | text | peer.fingerprint (0 now, 1 recorded)"]);
});
