// Host test for the names three on-device pages show (device-details §1.4,
// "values that are wrong today"):
//
//   - The display's mirror page (canary-display include/canary/net/
//     mirror_html.h, served at / by glass_web.cpp) called every display that
//     is not a watch a "wall panel" (a nightstand, a nightlight, the 7"
//     nightstand) and labeled the Wi-Fi signal, a dBm reading, "dB". The kind
//     now comes from the device type GET /api/status names (`node`, the mDNS
//     `dt`), a type with no word shows as itself, and no answer shows "--".
//     The signal is a word, in the glass's own bands (theme.cpp
//     signal_word(), read from the source here): the page is one of the glass
//     sources microcopy_lint.sh's plain-words audit holds, which keeps "dBm"
//     out of its strings, so the unit it was missing is not the fix.
//     The receipt's script is lifted out of the C++ raw string between two
//     comments that stand in the page before and after the change (so the old
//     page runs here and fails on what it shows, not on a missing marker), and
//     run against a stub DOM and a fetch that answers per URL.
//   - The flagship's dashboard (canary/lib/securacv_webui/src/
//     securacv_webui.cpp) showed "2.0.1" as its Firmware row until GET
//     /api/status answered, and fell back to it when the answer had no
//     `firmware`. The WAP's /admin page (canary-wap web_ui.h) did the same
//     with "3.1.0" and "3.0.0", printed "v3.1.0" in its footer, and wrote
//     "ESP32" when /api/system named no chip. None of them is a version or a
//     chip any build has: the rows now read "--" until there is a value.
//
// Run: node --test firmware/tests_host/test_honest_names.test.js
// (the Makefile's `run` target does, after the C++ suites).

"use strict";

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const FW = join(__dirname, "..");
const MIRROR = join(FW, "projects", "canary-display", "include", "canary", "net", "mirror_html.h");
const THEME = join(FW, "projects", "canary-display", "src", "ui", "theme.cpp");
const FLAGSHIP_UI = join(FW, "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
const WAP_UI = join(FW, "projects", "canary-wap", "arduino", "canary_wap", "web_ui.h");

const VERSION = /\d+\.\d+\.\d+/;

// ── the display mirror page's receipt ──────────────────────────────────────

const mirror = readFileSync(MIRROR, "utf8");
function receiptScript() {
  const a = mirror.indexOf("// the receipt\n");
  const b = mirror.indexOf("// serial monitor\n");
  assert.ok(a >= 0 && b > a, "the receipt's script is between its two comments");
  return mirror.slice(a, b);
}

// One element per id, made on first use; createElement's spans are kept in
// order on their parent so the receipt reads back as label/value pairs.
function stubDom() {
  const els = {};
  const el = (id) => els[id] || (els[id] = {
    id, style: {}, textContent: "", className: "", checked: false, kids: [],
    set innerHTML(v) { if (v === "") this.kids = []; },
    appendChild(k) { this.kids.push(k); },
  });
  const document = { createElement: () => ({ style: {}, textContent: "" }) };
  return { el, document };
}

const DEVICE = {
  fw: "2.4.16", flavor: "nightstand", chip: "ESP32-S3 r2", cores: 2, mhz: 240,
  flash_mb: 16, psram_kb: 8192, heap_kb: 120, heap_min_kb: 96, up_s: 3660,
  id: "canary_nightstand_001", ssid: "Home", ip: "192.168.1.9", signal: -61, caps: ["live glass mirror"],
};

// Runs the receipt against a fetch that answers /api/device with `device`
// and /api/status with `status` (null: the status request fails), and
// returns the receipt's rows as {label: value}.
async function receipt(device, status) {
  const { el, document } = stubDom();
  const ctx = {
    $: el, document, Math, console,
    setInterval() {},
    fetch: async (url) => {
      if (url === "/api/device") return { json: async () => device };
      if (url === "/api/status") {
        if (status === null) throw new TypeError("Failed to fetch");
        return { json: async () => status };
      }
      throw new Error(`unexpected fetch ${url}`);
    },
  };
  vm.createContext(ctx);
  vm.runInContext(receiptScript(), ctx);
  for (let i = 0; i < 20; i++) await new Promise((r) => setImmediate(r));
  const kids = el("receipt").kids;
  const rows = {};
  for (let i = 0; i + 1 < kids.length; i += 2) rows[kids[i].textContent] = kids[i + 1].textContent;
  return rows;
}

test("the kind of glass comes from the device type, not from 'is it a watch'", async () => {
  const want = {
    "canary-watch": "round watch glass",
    "canary-dash": "wall panel",
    "canary-nightstand": "nightstand glass",
    "canary-nightstand7": "nightstand glass",
    "canary-nightlight": "nightlight",
  };
  for (const [node, kind] of Object.entries(want)) {
    // The flavor word /api/device carries cannot tell these apart (the 7"
    // nightstand builds the dash flavor), so it is set to the misleading one.
    const flavor = node === "canary-watch" ? "watch" : "dash";
    const rows = await receipt({ ...DEVICE, flavor }, { node, fw: "2.4.16", uptime_s: 1 });
    assert.strictEqual(rows.Kind, kind, `${node} reads "${kind}"`);
  }
});

test("a device type with no word shows as itself, and no answer shows a dash", async () => {
  let rows = await receipt(DEVICE, { node: "canary-future-glass", fw: "x", uptime_s: 1 });
  assert.strictEqual(rows.Kind, "canary-future-glass");
  rows = await receipt(DEVICE, {});
  assert.strictEqual(rows.Kind, "--", "a status with no node says nothing it does not know");
  rows = await receipt({ ...DEVICE, flavor: "watch" }, null);
  assert.strictEqual(rows.Kind, "--", "no status answer: no guess, even on a watch flavor");
  assert.strictEqual(rows.Firmware, "2.4.16", "the rest of the receipt still renders");
});

// The glass's bands and words, from theme.cpp's signal_word(): each
// `if (dbm > N) return "word";` in order, then the last return.
function glassSignalBands() {
  const src = readFileSync(THEME, "utf8");
  const a = src.indexOf("const char* signal_word(int dbm) {");
  const b = src.indexOf("\n}\n", a);
  assert.ok(a >= 0 && b > a, "theme.cpp defines signal_word()");
  const body = src.slice(a, b);
  const bands = [...body.matchAll(/if \(dbm > (-?\d+)\) return "([^"]+)";/g)]
    .map((m) => ({ above: Number(m[1]), word: m[2] }));
  const last = [...body.matchAll(/^\s*return "([^"]+)";/gm)].pop();
  assert.ok(bands.length >= 2 && last, "signal_word()'s bands and its last word");
  return { bands, below: last[1] };
}

test("the Wi-Fi signal is the glass's word, never a decibel number", async () => {
  const { bands, below } = glassSignalBands();
  // Each band's edge, on both sides, and the floor.
  const cases = [];
  bands.forEach((band, i) => {
    cases.push([band.above + 1, band.word]);
    const next = bands[i + 1];
    cases.push([band.above, next ? next.word : below]);
  });
  cases.push([-95, below]);
  for (const [dbm, word] of cases) {
    const rows = await receipt({ ...DEVICE, signal: dbm }, { node: "canary-nightstand" });
    assert.strictEqual(rows.Network, `Home • 192.168.1.9 • ${word}`, `${dbm} dBm reads "${word}"`);
    assert.doesNotMatch(rows.Network, /dB|-\d/, "no decibel number and no unit");
  }
  const off = await receipt({ ...DEVICE, ssid: "" }, { node: "canary-nightstand" });
  assert.strictEqual(off.Network, "not connected");
});

// ── the firmware rows of the two dashboards ────────────────────────────────

// The markup's text for the element with this id, and every fallback the
// script writes into it after `data.firmware ||`.
function firmwareRow(src) {
  const tag = src.match(/<span class="identity-value" id="firmware">([^<]*)<\/span>/);
  assert.ok(tag, "the Firmware row is in the markup");
  const fallbacks = [...src.matchAll(/getElementById\('firmware'\)\.textContent = data\.firmware \|\| '([^']*)'/g)]
    .map((m) => m[1]);
  assert.ok(fallbacks.length > 0, "the script fills the Firmware row from data.firmware");
  return { markup: tag[1], fallbacks };
}

test("the flagship dashboard names no firmware version it was not told", () => {
  const { markup, fallbacks } = firmwareRow(readFileSync(FLAGSHIP_UI, "utf8"));
  assert.strictEqual(markup, "--", "until /api/status answers, the row reads --");
  assert.deepStrictEqual(fallbacks, ["--"], "an answer with no firmware reads --");
});

test("the WAP dashboard names no firmware version or chip it was not told", () => {
  const src = readFileSync(WAP_UI, "utf8");
  const { markup, fallbacks } = firmwareRow(src);
  assert.strictEqual(markup, "--", "until /api/status answers, the row reads --");
  assert.deepStrictEqual(fallbacks, ["--"], "an answer with no firmware reads --");
  // The page itself, the raw string the gzip carries (gen_web_assets_gz.py
  // reads the same delimiters): no footer version, nothing version-shaped
  // left beside the product name.
  const a = src.indexOf('R"rawliteral(');
  const b = src.indexOf(')rawliteral"', a);
  assert.ok(a >= 0 && b > a, "the page's raw string");
  const page = src.slice(a, b);
  const footer = page.slice(page.indexOf("<footer"), page.indexOf("</footer>"));
  assert.ok(footer.length > 0, "the footer");
  assert.doesNotMatch(footer, VERSION, "the footer names no version");
  // /api/system's chip model is "" when the firmware cannot read one: the
  // row says so rather than "ESP32".
  const chip = page.match(/getElementById\('sysChip'\)\.textContent = \(dev\.model \|\| '([^']*)'\)/);
  assert.ok(chip, "the Chip row is filled from dev.model");
  assert.strictEqual(chip[1], "--", "no chip model reads --, never a chip's name");
});
