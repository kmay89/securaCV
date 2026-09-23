// Host test for firmware/canary's dashboard timeline
// (canary/lib/securacv_webui/src/securacv_webui.cpp): the "Load More" paging
// into the card pages GET /api/witness serves from the SD card (repo sweep
// F35), driven the way a browser drives it.
//
// What it pins: a card row is badged "from card, chain-linked" (or says it is
// not linked, or that it is the card's oldest) — never the ring's "Verified",
// because nothing on the card path checks a signature; the card page's
// next_hint is echoed as ?hint= on the next click and dropped once the pages
// come from the ring again; a busy or timed-out card read keeps the button,
// and "no card" retires it. The timeline functions are lifted out of the C++
// raw string and run against a stub DOM and a recording api().
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
  "  get hint() { return timelineHint; }, renderTimeline, loadMoreTimeline };\n";

function harness() {
  const els = {};
  const el = (id) => els[id] || (els[id] = { id, style: {}, textContent: "", innerHTML: "" });
  const calls = [];
  const queue = [];
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
    setInterval, clearInterval, console,
    cameraReady: false, currentPanel: "timeline",
  };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  return { t: ctx.__t, el, calls, queue };
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

  const r = harness();
  ringOnScreen(r.t);
  r.queue.push({ ok: true, total: 32, more: true,
    records: [{ seq: 100, type_name: "EVNT", chain_hash: "44", verified: true }] });
  await r.t.loadMoreTimeline();
  assert.strictEqual(r.t.hint, null);
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
  queue.push({ ok: false, error: "no_card" });
  await t.loadMoreTimeline();
  assert.match(el("timelineNote").textContent, /no card is mounted/);
  assert.strictEqual(el("timelineLoadMore").style.display, "none");
});
