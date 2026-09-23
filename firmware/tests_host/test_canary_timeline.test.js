// Host test for firmware/canary's dashboard timeline
// (canary/lib/securacv_webui/src/securacv_webui.cpp): the "Load More" paging
// into the card pages GET /api/witness serves from the SD card (repo sweep
// F35), driven the way a browser drives it.
//
// What it pins: a card row is badged "from card, chain-linked" (or says it is
// not linked, or that it is the card's oldest) — never the ring's "Verified",
// because nothing on the card path checks a signature; the card page's
// next_hint is echoed as ?hint= on the next click and dropped once the pages
// come from the ring again or the list is reloaded; the first load shows Load
// More exactly when the device says `more`; a busy, timed-out or failed card
// read keeps the button, and "no card" or an empty card page retires it with
// a note; one Load More is out at a time, the refresh waits for it, and a
// page that lands after a reload is dropped. The timeline functions are
// lifted out of the C++ raw string and run against a stub DOM, a recording
// api() and a hand-driven refresh timer.
//
// Run: node --test firmware/tests_host/test_canary_timeline.test.js
// (the Makefile's `run` target does, after the C++ suites).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const UI = join(__dirname, "..", "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
const src = readFileSync(UI, "utf8");

// The timeline section, from its state to the next section's marker, plus the
// page's own escapeHtml — lifted by literal markers, not by an HTML regex.
function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}
const code =
  slice("    function escapeHtml(str) {", "\n    }\n") + "\n    }\n" +
  slice("    let timelinePage = 0;", "    // Acknowledgment") +
  "\n;globalThis.__t = {\n" +
  "  get records() { return timelineRecords; }, set records(v) { timelineRecords = v; },\n" +
  "  get hint() { return timelineHint; }, renderTimeline, loadMoreTimeline, loadTimeline };\n";

function harness() {
  const els = {};
  const el = (id) => els[id] || (els[id] = { id, style: {}, textContent: "", innerHTML: "" });
  const calls = [];
  const queue = [];  // api() answers in order; a Promise answers when resolved
  const timers = [];  // the refresh timer's callbacks, fired by hand
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
    api: async (url) => { calls.push(url); return queue.shift(); },
    setInterval: (fn) => timers.push(fn), clearInterval: () => {}, console,
    cameraReady: false, currentPanel: "timeline",
  };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  return { t: ctx.__t, el, calls, queue, timers };
}

// The three answers a (re)load asks for, in the order it asks: twenty ring
// rows #120..#101 (oldest first, as the endpoint sends them), the chain, the
// status.
function queueReload(queue, more) {
  queue.push(
    { ok: true, total: 32, more,
      records: Array.from({ length: 20 }, (_, i) => ({
        seq: 101 + i, type_name: "EVNT", chain_hash: "ab".repeat(32), verified: true })) },
    { ok: true, sequence: 120, chain_head: "ab".repeat(32) },
    { ok: true, crypto_healthy: true, witness_count: 120 });
}

function deferred() {
  let resolve;
  const promise = new Promise((r) => { resolve = r; });
  return { promise, resolve };
}

// Twenty ring rows on screen, #120 down to #101.
function ringOnScreen(t) {
  t.records = Array.from({ length: 20 }, (_, i) => ({
    seq: 120 - i, type_name: "EVNT", chain_hash: "ab".repeat(32), verified: true,
  }));
}

test("card rows are chain-linked, never Verified, and the join is reported", async () => {
  const { t, el, calls, queue } = harness();
  ringOnScreen(t);
  queue.push({
    ok: true, source: "sd", total: 32, next_hint: 5000, more: true, joins: false,
    records: [
      { seq: 81, type_name: "EVNT", chain_hash: "11", source: "sd", linked: true },
      { seq: 100, type_name: "BOOT", chain_hash: "22", source: "sd", linked: false },
    ],
  });
  await t.loadMoreTimeline();
  assert.strictEqual(calls[0], "/api/witness?last=20&before=101");
  const html = el("timelineList").innerHTML;
  assert.strictEqual((html.match(/Older records, read from the SD card/g) || []).length, 1);
  assert.match(html, /from card, chain-linked/);
  assert.match(html, /from card, not chain-linked \(a gap or break below\)/);
  assert.match(html, /not chain-linked to the record above/);  // joins:false on #100
  // No card row claims what only a checked signature can.
  for (const badge of html.match(/<div class="tl-chain-badge[^"]*">[^<]*<\/div>/g)) {
    if (/from card/.test(badge)) assert.doesNotMatch(badge, /Verified|✓/);
  }
  assert.strictEqual(el("timelineLoadMore").style.display, "block");
});

test("the card page's next_hint rides the next click; a ring page drops it", async () => {
  const { t, el, calls, queue } = harness();
  ringOnScreen(t);
  queue.push({ ok: true, source: "sd", total: 32, next_hint: 5000, more: true,
    records: [{ seq: 100, type_name: "EVNT", chain_hash: "11", source: "sd", linked: true }] });
  await t.loadMoreTimeline();
  assert.strictEqual(t.hint, 5000);
  queue.push({ ok: true, source: "sd", total: 32, next_hint: 0, more: false,
    records: [{ seq: 99, type_name: "EVNT", chain_hash: "33", source: "sd", linked: null }] });
  await t.loadMoreTimeline();
  assert.strictEqual(calls[1], "/api/witness?last=20&before=100&hint=5000");
  assert.match(el("timelineList").innerHTML, /from card, oldest on the card/);
  assert.strictEqual(el("timelineLoadMore").style.display, "none");  // more:false

  // On one list: a card page sets the hint, then a ring page drops it, and
  // the click after that carries no stale hint.
  const r = harness();
  ringOnScreen(r.t);
  r.queue.push({ ok: true, source: "sd", total: 32, next_hint: 7000, more: true,
    records: [{ seq: 100, type_name: "EVNT", chain_hash: "11", source: "sd", linked: true }] });
  await r.t.loadMoreTimeline();
  assert.strictEqual(r.t.hint, 7000);
  r.queue.push({ ok: true, total: 32, more: true,
    records: [{ seq: 99, type_name: "EVNT", chain_hash: "44", verified: true }] });
  await r.t.loadMoreTimeline();
  assert.strictEqual(r.calls[1], "/api/witness?last=20&before=100&hint=7000");
  assert.strictEqual(r.t.hint, null);
  r.queue.push({ ok: true, total: 32, more: false,
    records: [{ seq: 98, type_name: "EVNT", chain_hash: "55", verified: true }] });
  await r.t.loadMoreTimeline();
  assert.strictEqual(r.calls[2], "/api/witness?last=20&before=99");
});

test("a reload starts over: no hint, and Load More follows the device's more", async () => {
  const { t, el, calls, queue } = harness();
  ringOnScreen(t);
  queue.push({ ok: true, source: "sd", total: 32, next_hint: 5000, more: true,
    records: [{ seq: 100, type_name: "EVNT", chain_hash: "11", source: "sd", linked: true }] });
  await t.loadMoreTimeline();
  assert.strictEqual(t.hint, 5000);

  // Twenty of the ring's 32 on screen, and the device says nothing is older.
  queueReload(queue, false);
  await t.loadTimeline();
  assert.strictEqual(calls[1], "/api/witness?last=20");
  assert.strictEqual(t.hint, null);
  assert.strictEqual(t.records.length, 20);
  assert.strictEqual(el("timelineLoadMore").style.display, "none");

  // The same twenty, and the device says the card holds more.
  queueReload(queue, true);
  await t.loadTimeline();
  assert.strictEqual(el("timelineLoadMore").style.display, "block");
});

test("one Load More at a time; the refresh waits; a page after a reload is dropped", async () => {
  const { t, calls, queue, timers } = harness();
  queueReload(queue, true);
  await t.loadTimeline();
  const refresh = timers[timers.length - 1];
  const before = calls.length;

  // Two clicks while the first card page is out, and the 5 s refresh fires.
  const slow = deferred();
  queue.push(slow.promise);
  const first = t.loadMoreTimeline();
  const second = t.loadMoreTimeline();
  refresh();
  slow.resolve({ ok: true, source: "sd", total: 32, next_hint: 5000, more: true,
    records: [{ seq: 100, type_name: "EVNT", chain_hash: "11", source: "sd", linked: true }] });
  await Promise.all([first, second]);
  assert.deepStrictEqual(calls.slice(before), ["/api/witness?last=20&before=101"]);
  assert.deepStrictEqual(t.records.map((r) => r.seq).slice(-2), [101, 100]);
  assert.strictEqual(t.records.length, 21);

  // A page still out when Refresh reloads the list belongs to the old list.
  const late = deferred();
  queue.push(late.promise);
  const third = t.loadMoreTimeline();
  queueReload(queue, true);
  await t.loadTimeline();
  late.resolve({ ok: true, source: "sd", total: 32, next_hint: 4000, more: true,
    records: [{ seq: 99, type_name: "EVNT", chain_hash: "33", source: "sd", linked: true }] });
  await third;
  assert.strictEqual(t.records.length, 20);
  assert.strictEqual(t.records[t.records.length - 1].seq, 101);
  assert.strictEqual(t.hint, null);
});

test("busy and timeout keep Load More; no card retires it", async () => {
  const { t, el, queue } = harness();
  ringOnScreen(t);
  for (const error of ["history_busy", "history_timeout"]) {
    el("timelineLoadMore").style.display = "block";
    queue.push({ ok: false, error });
    await t.loadMoreTimeline();
    assert.match(el("timelineNote").textContent, /busy reading its card/);
    assert.strictEqual(el("timelineLoadMore").style.display, "block");
    assert.strictEqual(t.records.length, 20);
  }
  // A card that failed to read (500): say so, and keep the button to retry.
  queue.push({ ok: false, error: "history_read_failed" });
  await t.loadMoreTimeline();
  assert.match(el("timelineNote").textContent, /Could not read older records \(history_read_failed\)/);
  assert.strictEqual(el("timelineLoadMore").style.display, "block");
  queue.push({ ok: false, error: "no_card" });
  await t.loadMoreTimeline();
  assert.match(el("timelineNote").textContent, /no card is mounted/);
  assert.strictEqual(el("timelineLoadMore").style.display, "none");
});

test("an empty card page retires Load More and says why", async () => {
  const { t, el, queue } = harness();
  ringOnScreen(t);
  el("timelineLoadMore").style.display = "block";
  queue.push({ ok: true, source: "sd", total: 32, next_hint: 0, more: false, records: [] });
  await t.loadMoreTimeline();
  assert.strictEqual(el("timelineLoadMore").style.display, "none");
  assert.match(el("timelineNote").textContent, /Nothing older is on the card/);
  assert.strictEqual(el("timelineNote").style.display, "block");
  assert.strictEqual(t.records.length, 20);
});
