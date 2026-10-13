// Every request the firmware's pages make reaches the route it means, as
// esp_http_server routes it, and a request to a route only some builds
// register is named (repo sweeps F214, F238, F239).
//
// Both firmware trees registered the per-entry log acknowledge as
// POST "/api/logs/*/ack" on servers whose uri_match_fn is
// httpd_uri_match_wildcard, which takes a `*` only as a template's last
// character (or before a final `?`) and as a literal anywhere else: no
// request matched it, and each dashboard's per-entry Acknowledge answered 404
// (405 on the PlatformIO canary, whose GET "/*" fallback matches the path)
// while ack-all worked. Both now register POST "/api/logs/*" after every
// other POST /api/logs/... route. Registration order is the point: httpd
// answers with the first registration that matches, and refuses a later
// registration whose template an earlier one already matches.
//
// So the question is asked of httpd's own code: idf_uri_route_oracle.c holds
// ESP-IDF's httpd_uri_match_wildcard() copied verbatim (release/v5.5,
// byte-identical in v4.4.7 and v5.3.2) and the first-match and
// refuse-a-duplicate loop around it. dashboard_route_tables.js reads each
// tree's primary route table in registration order (every #if branch, the
// way both route budget checks count; a registration it cannot place fails
// it), each registration with the #if conditions it sits under, and every
// /api/ URL written in each page the tree serves from a raw string: the
// PlatformIO canary's dashboard and setup page; canary-wap's admin dashboard
// (web_ui.h, /admin), Sensing dashboard (/, which /sense redirects to),
// companion PWA, Tuning Lab and MQTT page, and its captive page. A URL a page
// takes from a response is not read by that reader; Export's answer, the
// one the dashboards followed, is held on its own (F278).
// What this pins:
//   - the copy is the IDF function, byte for byte (a hash of its text), and
//     the oracle answers as IDF's matcher does on the cases that decided F214;
//   - the #if reader and the page reader read what they must (a synthetic
//     source each), and each page's count of URLs read with no method is
//     pinned;
//   - kProbePaths' comment holds (F239);
//   - no registration in either table is refused as a duplicate;
//   - every raw string in a tree is a page read here or one named as not a
//     page (and naming no /api/ route), and each page is what the handler
//     that serves it sends;
//   - every request a page names the method of reaches the registration it
//     means: an exact template equal to its path if one is registered, else
//     the one wildcard that matches it (the PlatformIO dashboard's Chirp and
//     Bluetooth calls excepted: gated, nothing serves them, F176/F198); a URL
//     a page holds for later reaches its route by some method;
//   - every request whose route sits under an #if the page's own
//     registration does not is listed in GATED, by page and condition, with
//     how the page meets a build without it (F238);
//   - every /api/ URL the POST /api/export handler (and any handler it
//     returns through) writes reaches a route that means it, and no page's
//     Export sends the browser or a request to a field of the answer the
//     handler never writes (F278);
//   - the log routes reach their handlers on both trees: POST
//     /api/logs/<seq>/ack the log-ack handler, ack-all and rotate theirs;
//   - each page's per-entry Acknowledge builds a URL its own tree's handler
//     reads as the entry it names: the requests a page makes that route to
//     the log-ack registration (each tree's dashboard must make one), each a
//     template carrying exactly one value, filled with 0, 7 and 4294967295
//     and handed to log_ack_seq_from_uri() as cut out of the firmware
//     (test_log_ack_route --parse). Reaching the route is not enough: the
//     wildcard takes any path under /api/logs/, and the handler answers 404
//     for every shape but <seq>/ack.
//
// Run: node --test firmware/tests_host/test_dashboard_route_match.test.js
// (the Makefile's `run` target does, with the oracle and test_log_ack_route
// it built; run alone, the test builds the oracle with $CC, default cc, and
// has make build test_log_ack_route).

"use strict";

const { test } = require("node:test");
const assert = require("node:assert");
const { createHash } = require("node:crypto");
const { mkdtempSync, readFileSync, rmSync, writeFileSync } = require("node:fs");
const { tmpdir } = require("node:os");
const { join } = require("node:path");
const R = require("./dashboard_route_tables.js");

// sha256 of the text from "bool httpd_uri_match_wildcard(" through its
// closing brace, as it stands in ESP-IDF release/v5.5
// components/esp_http_server/src/httpd_uri.c (and v4.4.7, v5.3.2).
const IDF_MATCHER_SHA256 = "3df494155c6770152cbc15743617a5e7473b09adb842221f7ea4731e1b30a396";

const TREES = [
  { name: "PlatformIO canary", key: "pio", table: R.pioTable, parser: "pio", ackPage: "dashboard",
    logs: { "GET /api/logs": "handle_logs", "POST /api/logs/1/ack": "handle_log_ack",
            "POST /api/logs/ack-all": "handle_ack_all" } },
  { name: "canary-wap", key: "wap", table: R.wapTable, parser: "wap", ackPage: "admin dashboard",
    logs: { "GET /api/logs": "handle_logs_auth", "POST /api/logs/1/ack": "handle_log_ack_auth",
            "POST /api/logs/ack-all": "handle_ack_all_auth", "POST /api/logs/rotate": "handle_logs_rotate_auth" } },
];

// A catch-all ("/*": the PlatformIO canary's GET fallback, which 404s or,
// during setup, redirects to the wizard) answers any path but means none: a
// request only it answers reaches no route.
const catchAll = (g) => g.uri === "/*";

// Per page (R.PAGES): the fewest requests its reader must find (a floor that
// proves it found the page's script); how many of them it reads no method
// for, exactly (a URL the page holds for later: an <img> src, a ternary
// assigned to a constant, a URL built before its call; each such request is
// held only to "some method reaches a route", so a reader that stops seeing
// a call's method, or a new request whose method it cannot read, must show
// here); and the route families nothing in the tree serves, which the page
// gates on a first answer (F176, F198).
const PAGE_EXPECT = {
  "pio:dashboard": { min: 50, noMethod: 14, unserved: ["/api/chirp", "/api/bluetooth"] },
  "pio:setup page": { min: 5, noMethod: 0, unserved: [] },
  "wap:admin dashboard": { min: 50, noMethod: 10, unserved: [] },
  "wap:Sensing dashboard": { min: 15, noMethod: 0, unserved: [] },
  "wap:companion PWA": { min: 10, noMethod: 0, unserved: [] },
  "wap:Tuning Lab": { min: 3, noMethod: 0, unserved: [] },
  "wap:MQTT page": { min: 3, noMethod: 0, unserved: [] },
  "wap:captive page": { min: 0, noMethod: 0, unserved: [] },
};

// Raw strings in each tree that are not pages the walk reads, and why; none
// may name an /api/ route. canary-wap's companion service worker and
// manifest ride beside COMPANION_HTML; firmware/common's PORTAL_HTML is the
// shared setup portal the other devices serve (nothing under firmware/canary
// names it).
const NOT_PAGES = {
  pio: { PORTAL_HTML: "firmware/common's shared setup portal; the PlatformIO canary does not serve it" },
  wap: { COMPANION_SW_JS: "the companion PWA's service worker", COMPANION_MANIFEST: "the companion PWA's manifest" },
};

test("the oracle's matcher is ESP-IDF's httpd_uri_match_wildcard, verbatim", () => {
  const c = readFileSync(join(__dirname, "idf_uri_route_oracle.c"), "utf8");
  const a = c.indexOf("bool httpd_uri_match_wildcard(");
  const b = c.indexOf("\n}\n", a);
  assert.ok(a >= 0 && b > a, "the copied function is in the oracle");
  const text = c.slice(a, b + 2) + "\n";
  assert.strictEqual(createHash("sha256").update(text).digest("hex"), IDF_MATCHER_SHA256,
    "the copy differs from IDF's function (re-copy it; never edit it)");
});

test("the matcher answers the cases F214 turned on", () => {
  const cases = [
    ["/api/logs/*/ack", "/api/logs/42/ack", false],  // a `*` inside a template is a literal
    ["/api/logs/*/ack", "/api/logs/*/ack", true],
    ["/api/logs/*", "/api/logs/42/ack", true],
    ["/api/logs/*", "/api/logs/ack-all", true],      // why it is registered after ack-all
    ["/api/logs/*", "/api/logs", false],
    ["/api/logs/ack-all", "/api/logs/ack-all?x=1", true],  // the query is not the path
    ["/gen_204*", "/gen_204?cachebust=1", true],
    ["/api/a?", "/api/a", true],                     // `?`: the character before it is optional
    ["/api/a?", "/api/", true],
    ["/api/a?", "/api/ab", false],
    ["/*", "/api/logs/42/ack", true],
  ];
  const got = R.match(cases.map(([t, u]) => [t, u]));
  cases.forEach(([t, u, want], i) => assert.strictEqual(got[i], want, `${t} against ${u}`));
});

// canary-wap's wap_server.h lists the routes the sketch serves (sweep F237:
// it listed eight nothing registered). Held both ways against the table the
// walk reads: every entry is served (a METHOD /path entry reaches the
// registration it means, `<seq>` filled; a `*` family entry matches at least
// one registration, as httpd's matcher reads the template), every
// registration is an entry or falls under a family, and a registration under
// an #if has a covering entry that names each macro of it on its line or in
// its section's heading.
// The list as it is read: every line under a section heading that names an
// HTTP method before a path is an entry, whatever its indentation, or the
// read fails closed (an entry spelled so the reader skipped it would pass
// both directions unseen; F237's review found two such spellings).
const WAP_METHODS = "GET|POST|PUT|PATCH|DELETE|OPTIONS|HEAD";
function checkWapList(table, text) {
  const a = text.indexOf(" * HTTP routes, as canary_wap.ino registers them");
  const b = text.indexOf("*/", a);
  assert.ok(a >= 0 && b > a, "wap_server.h holds the route list");
  const entries = [];
  const unread = [];
  let heading = "";
  for (const line of text.slice(a, b).split("\n")) {
    if (/^\s*\*\s+── /.test(line)) { heading = line; continue; }
    const m = line.match(new RegExp(`^\\s*\\*\\s+(${WAP_METHODS}|\\*)\\s+(\\/\\S*)`));
    if (m) entries.push({ method: m[1], path: m[2], line, heading });
    else if (heading && new RegExp(`\\b(${WAP_METHODS})\\s+\\/`).test(line)) unread.push(line.trim());
  }
  const specific = entries.filter((e) => e.method !== "*");
  const family = entries.filter((e) => e.method === "*");
  // Every specific entry reaches the registration it means.
  const targets = specific.map((e) => ({ method: e.method, target: e.path.replace(/<[^>]+>/g, "7") }));
  const { answers } = R.route(table, targets);
  const means = (t) => {
    const exact = table.findIndex((g) => g.method === t.method && g.uri === t.target);
    if (exact >= 0) return exact;
    const hits = R.match(table.map((g) => [g.uri, t.target]));
    const wild = table.map((g, j) => (g.method === t.method && hits[j] && !catchAll(g) ? j : -1)).filter((j) => j >= 0);
    return wild.length === 1 ? wild[0] : null;
  };
  const wrong = [];
  specific.forEach((e, i) => {
    const want = means(targets[i]);
    if (typeof answers[i] !== "number" || answers[i] !== want) {
      wrong.push(`${e.method} ${e.path}: answered ${typeof answers[i] === "number" ? table[answers[i]].uri : answers[i]}`);
    }
  });
  // Every family entry covers something, and which registrations it covers.
  const famHits = family.map((e) => R.match(table.map((g) => [e.path, g.uri])));
  family.forEach((e, k) => { if (!famHits[k].some(Boolean)) wrong.push(`* ${e.path}: matches no registration`); });
  // Every registration is covered, and a gated one by an entry naming its gate.
  const uncovered = [];
  const unnamed = [];
  table.forEach((g, j) => {
    const covering = specific.filter((e, i) => answers[i] === j).concat(family.filter((e, k) => famHits[k][j]));
    if (!covering.length) { uncovered.push(`${g.method} ${g.uri} (${g.where})`); return; }
    const macros = [...new Set(g.conds.flatMap((c) => c.match(/\b[A-Z][A-Z0-9_]{2,}\b/g) || []))];
    if (macros.length && !covering.some((e) => macros.every((mac) => new RegExp(`\\b${mac}\\b`).test(e.line + e.heading)))) {
      unnamed.push(`${g.method} ${g.uri}: ${macros.join(", ")}`);
    }
  });
  return { entries, unread, wrong, uncovered, unnamed };
}

test("canary-wap: wap_server.h's route list is the route table (F237)", () => {
  const got = checkWapList(R.wapTable(), readFileSync(join(R.WAP_DIR, "wap_server.h"), "utf8"));
  assert.ok(got.entries.length > 60, `the list was read (${got.entries.length} entries)`);
  assert.deepStrictEqual(got.unread, [], "every line under a heading that names a method is read as an entry");
  assert.deepStrictEqual(got.wrong, [], "every entry in wap_server.h's list is served");
  assert.deepStrictEqual(got.uncovered, [], "every registration is in wap_server.h's list");
  assert.deepStrictEqual(got.unnamed, [], "a gated registration's list entry names its #if");
});

// The reader above, on the spellings F237's review slipped past an earlier
// version of it: a phantom route indented by two spaces, a PATCH or HEAD
// entry, and a method word the entry pattern cannot read. Each must fail.
test("canary-wap: the wap_server.h list reader refuses an entry it would once have skipped (F237)", () => {
  const table = R.wapTable();
  const text = readFileSync(join(R.WAP_DIR, "wap_server.h"), "utf8");
  const anchor = text.indexOf("\n", text.indexOf(" * GET    /api/chain"));
  assert.ok(anchor > 0, "the list still names GET /api/chain");
  const withLine = (l) => text.slice(0, anchor + 1) + l + "\n" + text.slice(anchor + 1);
  for (const [line, why] of [
    [" *  GET   /api/health                     - a phantom, indented two spaces", "two-space indent"],
    [" * PATCH  /api/identity                   - a phantom PATCH", "PATCH"],
    [" * HEAD   /api/witness/stats              - a phantom HEAD", "HEAD"],
    [" * -      GET /api/gps                    - a method the entry pattern cannot read", "unread"],
  ]) {
    const got = checkWapList(table, withLine(line));
    assert.ok(got.wrong.length + got.unread.length > 0, `${why}: refused (wrong ${got.wrong}, unread ${got.unread})`);
  }
});

// firmware/FEATURES.md's REST API table (sweep F240): one canary-wap column
// (the PlatformIO lane builds the Arduino sketch, so the old Arduino IDE and
// PlatformIO (canary-wap/) columns were one tree), and each canary-wap and
// canary (PIO) cell says what that tree registers: a cell starts with ❌
// exactly when the row's request reaches no registration that means it (a
// catch-all answer is not one), and a served route under an #if every
// registration of the tree does not share names each of its macros. Rows that
// name no single request are listed here, so a new one is read or named.
test("firmware/FEATURES.md's REST API table says what each tree serves (F240)", () => {
  const md = readFileSync(join(R.FW, "FEATURES.md"), "utf8");
  const a = md.indexOf("\n## REST API Endpoints\n");
  const b = md.indexOf("\n## ", a + 5);
  assert.ok(a >= 0 && b > a, "FEATURES.md has the REST API table");
  const lines = md.slice(a, b).split("\n").filter((l) => l.startsWith("|"));
  const split = (l) => l.trim().replace(/^\|/, "").replace(/\|$/, "").split("|").map((c) => c.trim());
  assert.deepStrictEqual(split(lines[0]), ["Endpoint", "WAP Snapshot", "canary-wap", "canary (PIO)"],
    "one canary-wap column beside the snapshot and the PlatformIO canary");
  const trees = { 2: R.wapTable(), 3: R.pioTable() };
  const shared = {};
  for (const [col, table] of Object.entries(trees)) {
    shared[col] = table[0].conds.filter((c) => table.every((g) => g.conds.includes(c)));
  }
  const skipped = [];
  const wrong = [];
  let read = 0;
  for (const line of lines.slice(2)) {
    const cells = split(line);
    const m = cells[0].match(/^`(GET|POST|DELETE|PUT|OPTIONS) (\/[^`\s]*)`/);
    if (!m) { skipped.push(cells[0]); continue; }
    read++;
    const target = m[2].replace(/<[^>]+>/g, "7");
    for (const [col, table] of Object.entries(trees)) {
      const { answers } = R.route(table, [{ method: m[1], target }]);
      const hit = typeof answers[0] === "number" && !catchAll(table[answers[0]]) ? table[answers[0]] : null;
      const cell = cells[+col];
      const says = !cell.startsWith("❌");
      if (says !== !!hit) {
        wrong.push(`${m[1]} ${m[2]} (${col === "2" ? "canary-wap" : "canary (PIO)"}): the cell says ` +
          `${says ? "served" : "not served"}, the table ${hit ? `registers ${hit.uri}` : `answers ${answers[0] === undefined ? "" : typeof answers[0] === "number" ? table[answers[0]].uri : answers[0]}`}`);
        continue;
      }
      if (!hit) continue;
      const macros = [...new Set(hit.conds.filter((c) => !shared[col].includes(c))
        .flatMap((c) => c.match(/\b[A-Z][A-Z0-9_]{2,}\b/g) || []))];
      const unnamed = macros.filter((mac) => !new RegExp(`\\b${mac}\\b`).test(cell));
      if (unnamed.length) wrong.push(`${m[1]} ${m[2]} (${col === "2" ? "canary-wap" : "canary (PIO)"}): served under ${unnamed.join(", ")}, which the cell does not name`);
    }
  }
  assert.ok(read > 30, `the table's rows were read (${read})`);
  assert.deepStrictEqual(skipped, ["Mesh pair/leave/remove endpoints"], "every row but these names one request");
  assert.deepStrictEqual(wrong, []);
});

// The #if reader the gated-route test stands on (sweep F238), on a source
// with every shape it must read: an include guard (no condition), #ifdef,
// #if / #elif / #else, nesting, a continued line, a comment on a directive,
// and a raw string whose CSS line looks like a directive.
test("the condition reader reads #if, #elif, #else and nesting, and skips guards and strings", () => {
  const code = [
    "#ifndef X_H",                                   // 1  include guard
    "#define X_H",                                   // 2
    "int a;",                                        // 3  []
    "#ifdef FEAT_A",                                 // 4
    "int b;",                                        // 5  [defined(FEAT_A)]
    "#if FEAT_B && \\",                              // 6
    "    FEAT_C  // trailing words",                 // 7
    "int c;",                                        // 8  [defined(FEAT_A), FEAT_B && FEAT_C]
    "#elif FEAT_D",                                  // 9
    "int d;",                                        // 10 [defined(FEAT_A), !(FEAT_B && FEAT_C), FEAT_D]
    "#else",                                         // 11
    "int e;",                                        // 12 [defined(FEAT_A), !(FEAT_B && FEAT_C), !(FEAT_D)]
    "#endif",                                        // 13
    "#endif",                                        // 14
    "#ifndef FEAT_E",                                // 15 (no #define after: a condition)
    "const char P[] = R\"H(",                        // 16
    "#if-banner { color: red; }",                    // 17 CSS, not a directive
    ")H\";",                                         // 18
    "int f;",                                        // 19 [!defined(FEAT_E)]
    "#endif",                                        // 20
    "int g;",                                        // 21 []
    "#endif",                                        // 22
  ].join("\n");
  const src = { path: "synthetic.h", text: R.stripComments(code) };
  const at = (line, word) => {
    const lines = src.text.split("\n");
    let off = 0;
    for (let i = 0; i < line - 1; i++) off += lines[i].length + 1;
    return off + lines[line - 1].indexOf(word);
  };
  assert.deepStrictEqual(R.condsAt(src, at(3, "int")), []);
  assert.deepStrictEqual(R.condsAt(src, at(5, "int")), ["defined(FEAT_A)"]);
  assert.deepStrictEqual(R.condsAt(src, at(8, "int")), ["defined(FEAT_A)", "FEAT_B && FEAT_C"]);
  assert.deepStrictEqual(R.condsAt(src, at(10, "int")), ["defined(FEAT_A)", "!(FEAT_B && FEAT_C)", "FEAT_D"]);
  assert.deepStrictEqual(R.condsAt(src, at(12, "int")), ["defined(FEAT_A)", "!(FEAT_B && FEAT_C)", "!(FEAT_D)"]);
  assert.deepStrictEqual(R.condsAt(src, at(19, "int")), ["!defined(FEAT_E)"]);
  assert.deepStrictEqual(R.condsAt(src, at(21, "int")), []);
  assert.throws(() => R.condsAt({ path: "open.h", text: "#if A\nint x;\n" }, 7), /never closed/);
});

// The page reader the request tests stand on (sweep F238), on a raw-string
// page with every shape it must read: each call form (api, secureFetch,
// fetch, cvFetch), a method in single, double or back quotes, an options
// object over several lines, a method the page computes, a ternary URL, a
// template, a query and a fragment (neither is part of the path), a
// double-quoted URL, and URLs held for later (no method).
test("the Export readers read an answer's keys and URLs, and what a page follows (F278)", () => {
  const answer = exportAnswer([
    'doc["ok"] = true;',
    'if (doc["ok"] == true) {}',
    'doc["download_url"] = String("/api/download?path=") + export_path;',
    'last_rec["seq"] = 7; doc["note"] = "see /sd/EXPORT";',
  ].join("\n"));
  assert.deepStrictEqual(answer, { keys: ["download_url", "note", "ok", "seq"], urls: ["/api/download"] });
  const page = [
    "async function a() { const data = await api('/api/export', 'POST');",
    "  if (data.ok && data.download_url) { window.location.href = data.download_url; } }",
    "async function b() { let r = await fetch(\"/api/export\", { method: 'POST' });",
    "  if (r.ok) { location.assign(r['file']); x.src = r.thumb; window.open(r.page); } }",
    "async function c() { const d = await secureFetch(`/api/export`); const t = `${d.ok ? '}' : '{'}`;",
    "  if (t === d.href) alert('x'); }",
    "  const later = data.download_url; window.location.href = data.after_scope;",
    "async function e() { const data = await api('/api/export', 'POST');",
    "  const url = URL.createObjectURL(blob); a.href = url; a.download = data.device_id; }",
  ].join("\n");
  const got = exportFollows(page);
  assert.strictEqual(got.calls, 4);
  assert.deepStrictEqual(got.follows.map((f) => `${f.key}@${f.line}`),
    ["download_url@2", "file@4", "thumb@4", "page@4"]);
});

test("the page reader reads each call form, quote and URL shape", () => {
  const page = [
    "#pragma once",
    "static const char SYN_HTML[] PROGMEM = R\"SYN(",
    "<script>",
    "  cvFetch('/api/a', {method:'POST'});",
    "  fetch('/api/m', {",
    "    headers: { 'Content-Type': 'application/json' },",
    "    method: 'PUT',",
    "    body: JSON.stringify({ x: 1 })",
    "  });",
    "  fetch(\"/api/b\", {method: \"DELETE\"});",
    "  api('/api/c', 'POST');",
    "  api(\"/api/c2\", \"PATCH\");",
    "  secureFetch(on ? '/api/e/enable' : '/api/e/disable', { method: `POST` });",
    "  fetch(`/api/logs/${seq}/ack`, { method: 'POST' });",
    "  fetch('/api/d#x');",
    "  cvFetch('/api/q?x=1&y=2');",
    "  api('/api/g');",
    "  fetch('/api/h', { headers: {} });",
    "  fetch('/api/k', { method: m });",
    "  fetch('/api/i', opts);",
    "  const HELD = '/api/held';",
    "  img.src = \"/api/img?t=\" + Date.now();",
    "</script>",
    ")SYN\";",
  ].join("\n");
  const dir = mkdtempSync(join(tmpdir(), "route-match-"));
  try {
    const path = join(dir, "syn_page.h");
    writeFileSync(path, page);
    const got = R.pageRequests({ path, symbol: "SYN_HTML" }).map((r) => [r.route, r.method]);
    assert.deepStrictEqual(got, [
      ["/api/a", "POST"],
      ["/api/m", "PUT"],
      ["/api/b", "DELETE"],
      ["/api/c", "POST"],
      ["/api/c2", "PATCH"],
      ["/api/e/enable", "POST"],
      ["/api/e/disable", "POST"],
      ["/api/logs/1/ack", "POST"],
      ["/api/d", "GET"],
      ["/api/q", "GET"],
      ["/api/g", "GET"],
      ["/api/h", "GET"],
      ["/api/k", null],
      ["/api/i", null],
      ["/api/held", null],
      ["/api/img", null],
    ]);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

// kProbePaths' comment in securacv_network.cpp (sweep F239): the matcher is
// handed only the path, so an exact template matches a probe with a query and
// the trailing `*` is not what lets one through; what the `*` does is take a
// near miss (a longer path) to handle_captive_probe too, which tells the
// probes apart by the exact path component.
test("PlatformIO canary: the probe routes match as kProbePaths' comment says (F239)", () => {
  const table = R.pioTable();
  const probes = table.filter((g) => g.handler === "handle_captive_probe");
  assert.strictEqual(probes.length, 6, "the six OS connectivity probes are registered on the primary server");
  const paths = probes.map((g) => {
    assert.ok(g.uri.endsWith("*"), `${g.uri}: the comment explains a trailing '*'`);
    return g.uri.slice(0, -1);
  });
  // An exact template already takes the probe with a cache-busting query.
  const exact = R.match(paths.map((p) => [p, `${p}?cachebust=1`]));
  exact.forEach((m, i) => assert.ok(m, `"${paths[i]}" (no '*') matches ${paths[i]}?cachebust=1`));
  // On the registered table, the probe with a query and a near miss both
  // reach the probe handler: the near miss only because of the '*'.
  const asks = paths.flatMap((p) => [`${p}?cachebust=1`, `${p}x`]).map((target) => ({ method: "GET", target }));
  const { answers } = R.route(table, asks);
  answers.forEach((a, i) => assert.strictEqual(typeof a === "number" && table[a].handler, "handle_captive_probe",
    `GET ${asks[i].target} reaches handle_captive_probe`));
  const nearMiss = R.match(paths.map((p) => [p, `${p}x`]));
  nearMiss.forEach((m, i) => assert.ok(!m, `"${paths[i]}" (no '*') does not match ${paths[i]}x`));
  // And the handler tells them apart by the exact path, through
  // probe_path_is (req->uri keeps the query), for every probe it answers
  // other than through its Apple branch.
  const src = readFileSync(R.PIO_NET, "utf8");
  const a = src.indexOf("static esp_err_t handle_captive_probe(httpd_req_t* req) {");
  const b = src.indexOf("\n}\n", a);
  assert.ok(a >= 0 && b > a, "handle_captive_probe is defined");
  const body = src.slice(a, b);
  const checked = [...body.matchAll(/probe_path_is\(uri,\s*"([^"]+)"\)/g)].map((m) => m[1]).sort();
  assert.deepStrictEqual(checked, ["/connecttest.txt", "/gen_204", "/generate_204", "/ncsi.txt"]);
  assert.ok(!/\bstrcmp\s*\(\s*uri\b/.test(body), "no strcmp of the whole uri (it keeps the query)");
});

// Requests to a route only some builds register (sweep F238), by tree, page
// and the condition(s) the route sits under that the page's own
// registration does not: the routes, and how the page meets a build without
// them, read from the page. F238 marked five it did not meet; F273-F276
// gated them, and test_canary_dashboard_gates.test.js,
// test_canary_setup_page.test.js and canary-wap's dashboard_gates.test.js
// run each page's own code to hold what `meets` says. Which builds lack each
// condition is platformio.ini's (PlatformIO canary) and build_config.h's
// profiles (canary-wap) business; the test pins only what the sources say.
const GATED = {
  pio: {
    dashboard: {
      "FEATURE_CAMERA_PEEK": {
        routes: ["GET /api/peek/sensor", "GET /api/peek/snapshot", "GET /api/peek/status", "GET /api/peek/stream",
                 "POST /api/peek/resolution", "POST /api/peek/sensor", "POST /api/peek/stop"],
        meets: "refreshPeekStatus() shows the no-camera state and hides the Peek controls and camera info cards, the only callers of the rest, when no route takes the status read (the fallback's 404 or setup redirect) before any status has answered (F274)" },
      "FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER": {
        routes: ["GET /api/sensing"],
        meets: "refreshSensing() shows the offline pill on a refused answer" },
      "FEATURE_THERMAL_WATCHDOG": {
        routes: ["GET /api/thermal"],
        meets: "refreshThermal() hides the card on a refused answer" },
      "defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN": {
        routes: ["GET /api/scout", "GET /api/scout/pair/status", "POST /api/scout/pair/cancel",
                 "POST /api/scout/pair/start", "POST /api/scout/unpair"],
        meets: "refreshScout() hides the card, the only caller of the rest, on a refused answer" },
      "FEATURE_ACOUSTIC_EVENTS": {
        routes: ["GET /api/audio/level", "GET /api/audio/test/status", "POST /api/audio/mute", "POST /api/audio/test/start"],
        meets: "the acoustic card starts hidden and shows only when /api/sensing reports the microphone" },
      "FEATURE_VISION_DETECT": {
        routes: ["GET /api/vision/config", "GET /api/vision/thumbnail", "POST /api/vision/config", "POST /api/vision/config/save"],
        meets: "the vision cards show only when /api/sensing reports vision enabled" },
      "FEATURE_OTA_PULL": {
        routes: ["GET /api/ota/status", "POST /api/ota/check", "POST /api/ota/config", "POST /api/ota/install"],
        meets: "refreshOtaStatus() hides the card and stops asking (otaSupported) on a refused answer" },
      "defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK": {
        routes: ["DELETE /api/mesh/alerts", "GET /api/mesh", "GET /api/mesh/alerts", "GET /api/mesh/peers",
                 "POST /api/mesh/enable", "POST /api/mesh/leave", "POST /api/mesh/name", "POST /api/mesh/pair/cancel",
                 "POST /api/mesh/pair/confirm", "POST /api/mesh/pair/join", "POST /api/mesh/pair/start", "POST /api/mesh/remove"],
        meets: "the Opera tab, the only caller of the rest, starts hidden and shows when /api/mesh answers (F273)" },
    },
    "setup page": {
      "FEATURE_HA_MQTT": {
        routes: ["GET /api/mqtt/status", "POST /api/mqtt/ca", "POST /api/mqtt/config"],
        meets: "the hub box, the only caller of the rest, starts hidden and shows when /api/mqtt/status answers JSON (F275)" },
    },
  },
  wap: {
    "admin dashboard": {
      "FEATURE_BLE": {
        routes: ["GET /api/ble/status", "GET /api/nearby", "POST /api/ble/chirp/send"],
        meets: "refreshBleDiscovery() shows Unavailable and an empty nearby list for a refused answer" },
      "FEATURE_VAULT_SNAPSHOT": {
        routes: ["DELETE /api/vault/item", "DELETE /api/vault/key", "GET /api/vault/download", "GET /api/vault/list",
                 "GET /api/vault/status", "POST /api/vault/config", "POST /api/vault/key", "POST /api/vault/test"],
        meets: "refreshVault() hides the card, the only caller of the rest, on a refused answer" },
      "FEATURE_SYS_MONITOR": {
        routes: ["GET /api/system"],
        meets: "refreshSystemHealth() returns without a temperature; every profile that serves pages sets it" },
      "FEATURE_CAMERA_PEEK": {
        routes: ["GET /api/peek/sensor", "GET /api/peek/snapshot", "GET /api/peek/status", "GET /api/peek/stream",
                 "POST /api/peek/init", "POST /api/peek/resolution", "POST /api/peek/sensor", "POST /api/peek/stop"],
        meets: "refreshPeekStatus() shows the no-camera state and hides the Camera Controls and Camera Info cards, the only callers of the rest, when no route takes the status read (the server's 404) before any status has answered (F274)" },
      "FEATURE_MESH_NETWORK": {
        routes: ["DELETE /api/mesh/alerts", "GET /api/chirp", "GET /api/chirp/recent", "GET /api/mesh", "GET /api/mesh/alerts",
                 "GET /api/mesh/peers", "POST /api/chirp/confirm", "POST /api/chirp/disable", "POST /api/chirp/dismiss",
                 "POST /api/chirp/enable", "POST /api/chirp/mute", "POST /api/chirp/send", "POST /api/chirp/settings",
                 "POST /api/chirp/unmute", "POST /api/mesh/enable", "POST /api/mesh/leave", "POST /api/mesh/name",
                 "POST /api/mesh/pair/cancel", "POST /api/mesh/pair/confirm", "POST /api/mesh/pair/join",
                 "POST /api/mesh/pair/start", "POST /api/mesh/remove"],
        meets: "the Community tab's Opera and Chirp sub-tabs, the only callers of the rest, start hidden and show when /api/mesh and /api/chirp answer (F276)" },
      "FEATURE_ACOUSTIC_EVENTS": {
        routes: ["GET /api/audio/config", "GET /api/audio/selftest", "GET /api/audio/status", "POST /api/audio/config",
                 "POST /api/audio/mute", "POST /api/audio/selftest"],
        meets: "refreshMicStatus() hides the microphone card, the only caller of the rest, on a refused answer" },
      "FEATURE_BLUETOOTH": {
        routes: ["DELETE /api/bluetooth/paired", "DELETE /api/bluetooth/paired/all", "DELETE /api/bluetooth/scan/results",
                 "GET /api/bluetooth", "GET /api/bluetooth/ota", "GET /api/bluetooth/paired", "GET /api/bluetooth/scan/results",
                 "GET /api/bluetooth/settings", "POST /api/bluetooth/advertise/start", "POST /api/bluetooth/advertise/stop",
                 "POST /api/bluetooth/disable", "POST /api/bluetooth/disconnect", "POST /api/bluetooth/enable",
                 "POST /api/bluetooth/pair/cancel", "POST /api/bluetooth/pair/confirm", "POST /api/bluetooth/pair/reject",
                 "POST /api/bluetooth/pair/start", "POST /api/bluetooth/scan/start", "POST /api/bluetooth/settings"],
        meets: "refreshBtStatus() shows the backend-not-present state for an answer without a state" },
    },
    "Sensing dashboard": {
      "FEATURE_ACOUSTIC_EVENTS": {
        routes: ["GET /api/audio/status", "POST /api/audio/mute"],
        meets: "pollMicPill() hides the microphone pill, the only caller of the mute, on a refused answer" },
    },
    "companion PWA": {
      "FEATURE_QR_PROVISION": {
        routes: ["DELETE /api/wifi/qr-scan", "GET /api/wifi/qr-scan", "POST /api/wifi/qr-scan"],
        meets: "the QR offer shows only when GET /api/wifi carries qr_provision, which the same #if adds" },
    },
  },
};

// The requests a page makes, each with every method it can be sent with:
// the one the page names, or (a URL held for later) each method a
// registration uses, kept where that method reaches a route.
function pageAsks(table, page) {
  const exp = PAGE_EXPECT[page.key];
  const reqs = R.pageRequests(page)
    .filter((r) => !exp.unserved.some((g) => r.route === g || r.route.startsWith(g + "/")));
  const methods = [...new Set(table.map((r) => r.method))];
  const asks = [];
  for (const r of reqs) for (const m of r.method ? [r.method] : methods) asks.push({ r, method: m });
  const matched = R.match(asks.flatMap(({ r }) => table.map((g) => [g.uri, r.route])));
  const { answers } = R.route(table, asks.map((a) => ({ method: a.method, target: a.r.route })));
  return asks.map((a, i) => {
    const row = matched.slice(i * table.length, (i + 1) * table.length);
    const exact = table.findIndex((g) => g.method === a.method && g.uri === a.r.route);
    let want;
    if (exact >= 0) {
      want = exact;
    } else {
      const wild = table.map((g, j) => (g.method === a.method && row[j] && g.uri !== a.r.route && !catchAll(g) ? j : -1)).filter((j) => j >= 0);
      want = wild.length === 1 ? wild[0] : wild.length ? `${wild.length} wildcards` : null;
    }
    return { ...a, answer: answers[i], want, ok: typeof want === "number" && answers[i] === want };
  });
}

// The registrations that serve a page: those of each handler it names.
function servingRegs(table, page) {
  return page.servedBy.flatMap((h) => table.filter((g) => g.handler === h.handler));
}

// The registration a request means, as pageAsks() decides it (an exact
// template equal to its path, else the one wildcard other than a catch-all
// that matches it), if httpd answers it there; else null.
function meantRoute(table, method, path) {
  const matched = R.match(table.map((g) => [g.uri, path]));
  const { answers } = R.route(table, [{ method, target: path }]);
  let want = table.findIndex((g) => g.method === method && g.uri === path);
  if (want < 0) {
    const wild = table.map((g, j) => (g.method === method && matched[j] && !catchAll(g) ? j : -1)).filter((j) => j >= 0);
    want = wild.length === 1 ? wild[0] : -1;
  }
  return want >= 0 && answers[0] === want ? table[want] : null;
}

// ── Export's answer (sweep F278) ─────────────────────────────────────────
// The page reader reads only URLs a page writes, so a URL a page takes from
// a response is held here, for the one answer the dashboards followed:
// canary-wap's POST /api/export answered download_url
// "/api/download?path=...", which nothing registers, and the PlatformIO
// dashboard looked for a download_url its handler never sends. Both now
// answer the bundle inline and both pages save the answer. What is read:
//   - the answer: the keys a handler body assigns (doc["key"] = ...) and the
//     /api/ URLs it writes, through every handler it returns through
//     (`return handle_x(req);`, the canary-wap auth wrappers);
//   - what a page follows: in the scope of each `X = await api('/api/export'
//     ...)` (or fetch, secureFetch, cvFetch), a field of X handed to a
//     navigation, a link, an image or a request (`.href =`, `.src =`,
//     location.assign/replace, window.open, api/fetch/secureFetch/cvFetch).
//     A field copied to a variable first is not followed here.

function exportAnswer(text) {
  return {
    keys: [...new Set([...text.matchAll(/\[\s*"(\w+)"\s*\]\s*=(?!=)/g)].map((m) => m[1]))].sort(),
    urls: [...new Set([...text.matchAll(/"(\/api\/[^"?#\s]*)/g)].map((m) => m[1]))].sort(),
  };
}

function exportHandler(table) {
  const reg = table.find((g) => g.method === "POST" && g.uri === "/api/export");
  assert.ok(reg, "POST /api/export is registered");
  const file = reg.where.replace(/:\d+$/, "");
  const bodies = [];
  const queue = [reg.handler];
  while (queue.length) {
    const h = queue.shift();
    if (bodies.some((b) => b.handler === h)) continue;
    const body = R.handlerBody({ handler: h, file });
    bodies.push({ handler: h, body });
    for (const m of body.matchAll(/\breturn\s+(\w+)\s*\(\s*req\s*\)\s*;/g)) queue.push(m[1]);
  }
  return { handlers: bodies.map((b) => b.handler), ...exportAnswer(bodies.map((b) => b.body).join("\n")) };
}

// The end of the block that encloses `from` in page script: the offset of
// the first `}` that closes a brace opened before it (quotes and template
// literals skipped, a template's ${...} read as code).
function scopeEnd(text, from) {
  let depth = 0;
  const tpl = [];
  for (let i = from; i < text.length; i++) {
    const c = text[i];
    if (tpl.length && depth === tpl[tpl.length - 1]) {
      if (c === "\\") { i++; continue; }
      if (c === "`") { tpl.pop(); continue; }
      if (c === "$" && text[i + 1] === "{") { depth++; i++; }
      continue;
    }
    if (c === "'" || c === '"') {
      for (i++; i < text.length && text[i] !== c && text[i] !== "\n"; i++) if (text[i] === "\\") i++;
    } else if (c === "`") {
      tpl.push(depth);
    } else if (c === "{") {
      depth++;
    } else if (c === "}") {
      if (depth === 0) return i;
      depth--;
    }
  }
  return text.length;
}

function exportFollows(text) {
  const out = [];
  let calls = 0;
  const call = /\b(\w+)\s*=\s*await\s+(?:api|fetch|secureFetch|cvFetch)\(\s*(['"`])\/api\/export\2/g;
  for (const m of text.matchAll(call)) {
    calls++;
    const v = m[1];
    const scope = text.slice(m.index, scopeEnd(text, m.index));
    const sink = new RegExp(`(?:\\.(?:href|src)\\s*=(?!=)|\\blocation\\.(?:assign|replace)\\(|\\bwindow\\.open\\(|\\b(?:api|fetch|secureFetch|cvFetch)\\()` +
      `\\s*${v}\\s*(?:\\.\\s*(\\w+)|\\[\\s*['"](\\w+)['"]\\s*\\])`, "g");
    for (const f of scope.matchAll(sink)) {
      out.push({ key: f[1] || f[2], line: text.slice(0, m.index + f.index).split("\n").length });
    }
  }
  return { calls, follows: out };
}

for (const tree of TREES) {
  for (const page of R.PAGES[tree.key]) page.key = `${tree.key}:${page.name}`;

  test(`${tree.name}: no registration is refused as a duplicate`, () => {
    const table = tree.table();
    assert.ok(table.length > 50, "the table was read");
    const { regs } = R.route(table, []);
    const refused = regs.map((r, i) => r.exists === undefined ? null :
      `${table[i].method} ${table[i].uri} (${table[i].where}) — ${table[r.exists].uri} (${table[r.exists].where}) matches it first`)
      .filter(Boolean);
    assert.deepStrictEqual(refused, []);
  });

  // Fail closed on pages (sweep F238): every raw string in the tree is a page
  // the walk reads or one NOT_PAGES names, and each page is what the handler
  // the walk takes it from actually sends.
  test(`${tree.name}: every page the tree serves from a raw string is read`, () => {
    const table = tree.table();
    const files = tree.key === "pio"
      ? R.filesUnder(join(R.FW, "canary"), /\.(c|cc|cpp|h|hpp|ino)$/).concat(R.filesUnder(join(R.FW, "common"), /\.(c|cc|cpp|h|hpp|ino)$/))
      : R.filesUnder(R.WAP_DIR, /\.(c|cc|cpp|h|hpp|ino)$/);
    const pages = R.PAGES[tree.key];
    const found = [];
    for (const f of files) {
      for (const rs of R.rawStrings(f)) {
        const page = pages.find((p) => p.path === f && p.symbol === rs.name);
        if (page) { found.push(page.symbol); continue; }
        assert.ok(rs.name && NOT_PAGES[tree.key][rs.name],
          `${f}: raw string ${rs.name || "(unnamed)"} is neither a page dashboard_route_tables.js reads nor in NOT_PAGES`);
        const text = R.pageText({ path: f, symbol: rs.name });
        assert.ok(!/['"`]\/api\//.test(text), `${rs.name}: a raw string that names an /api/ route is a page to read`);
      }
    }
    assert.deepStrictEqual(found.sort(), pages.map((p) => p.symbol).sort(), "each listed page was found once");
    if (tree.key === "pio") {
      const canaryFiles = R.filesUnder(join(R.FW, "canary"), /\.(c|cc|cpp|h|hpp|ino)$/);
      for (const f of canaryFiles) assert.ok(!/\bPORTAL_HTML\b/.test(R.source(f).text), `${f} names PORTAL_HTML`);
    }
    for (const page of pages) {
      assert.ok(servingRegs(table, page).length >= 1, `${page.name}: a registration serves it`);
      for (const h of page.servedBy) {
        assert.ok(table.some((g) => g.handler === h.handler), `${page.name}: ${h.handler} is registered`);
        assert.match(R.handlerBody(h), new RegExp(`\\b${page.symbol}(?:_GZ)?\\b`),
          `${page.name}: ${h.handler} sends ${page.symbol}`);
      }
    }
    if (tree.key === "wap") {
      // /sense is the Sensing dashboard: handle_sense_page redirects to /.
      const sense = table.find((g) => g.uri === "/sense");
      assert.ok(sense, "/sense is registered");
      const body = R.handlerBody({ handler: sense.handler, file: join(R.WAP_DIR, "csi_integration.cpp") });
      assert.match(body, /"Location"\s*,\s*"\/"/, "/sense redirects to /");
    }
  });

  for (const page of R.PAGES[tree.key]) {
    test(`${tree.name} ${page.name}: every request reaches the route it means`, () => {
      const table = tree.table();
      const asks = pageAsks(table, page);
      const reqs = new Set(asks.map((a) => a.r));
      assert.ok(reqs.size >= PAGE_EXPECT[page.key].min, `the page's requests were read (${reqs.size})`);
      const held = R.pageRequests(page).filter((r) => !r.method);
      assert.strictEqual(held.length, PAGE_EXPECT[page.key].noMethod,
        `requests read with no method (${held.map((r) => `${r.raw} at page line ${r.line}`).join(", ")})`);
      const wrong = [];
      const reached = new Map();
      for (const a of asks) {
        if (a.r.method) {
          if (!a.ok) wrong.push(`${a.method} ${a.r.route} (page line ${a.r.line}): answered ` +
            `${typeof a.answer === "number" ? table[a.answer].uri : a.answer}, means ` +
            `${typeof a.want === "number" ? table[a.want].uri : a.want || "nothing registered"}`);
        } else {
          reached.set(a.r, (reached.get(a.r) || false) || a.ok);
        }
      }
      for (const [r, ok] of reached) if (!ok) wrong.push(`${r.route} (page line ${r.line}): no method reaches a route`);
      assert.deepStrictEqual(wrong, []);
    });
  }

  // Not every build registers every route (sweep F238): a request whose
  // route sits under an #if the page's own registration does not, is a call
  // a build without that feature answers 404 (or a catch-all answers). Each
  // is named in GATED below, by page and condition, with how the page meets
  // the build that lacks it; a new one fails until it is named there.
  test(`${tree.name}: every request to a route only some builds register is named`, () => {
    const table = tree.table();
    const got = {};
    for (const page of R.PAGES[tree.key]) {
      const serving = servingRegs(table, page);
      for (const a of pageAsks(table, page)) {
        if (!a.ok) continue;
        const reg = table[a.want];
        for (const s of serving) {
          const missing = reg.conds.filter((c) => !s.conds.includes(c));
          if (!missing.length) continue;
          const byCond = (got[page.name] = got[page.name] || {});
          const key = missing.join(" && ");
          (byCond[key] = byCond[key] || new Set()).add(`${a.method} ${a.r.route}`);
        }
      }
    }
    const actual = {};
    for (const [p, byCond] of Object.entries(got)) {
      actual[p] = {};
      for (const [c, set] of Object.entries(byCond)) actual[p][c] = [...set].sort();
    }
    const expected = {};
    for (const [p, byCond] of Object.entries(GATED[tree.key])) {
      expected[p] = {};
      for (const [c, g] of Object.entries(byCond)) expected[p][c] = [...g.routes].sort();
    }
    assert.deepStrictEqual(actual, expected);
  });

  test(`${tree.name}: the URL an Export answer names is served, and the dashboard's Export follows only what it answers (F278)`, () => {
    const table = tree.table();
    const answer = exportHandler(table);
    for (const k of ["ok", "device_id", "pubkey"]) {
      assert.ok(answer.keys.includes(k), `${answer.handlers.join(" → ")}: the answer's ${k} was read (${answer.keys})`);
    }
    const unserved = answer.urls.filter((u) => !meantRoute(table, "GET", u));
    assert.deepStrictEqual(unserved, [], `${answer.handlers.join(" → ")} answers a URL no route serves`);
    let calls = 0;
    const wrong = [];
    for (const page of R.PAGES[tree.key]) {
      const got = exportFollows(R.pageText(page));
      calls += got.calls;
      for (const f of got.follows) {
        if (!answer.keys.includes(f.key)) wrong.push(`${page.name} (page line ${f.line}) follows ${f.key}, which ${answer.handlers.join(" → ")} never answers`);
      }
    }
    assert.ok(calls >= 1, `a page of the ${tree.name} posts /api/export (${calls})`);
    assert.deepStrictEqual(wrong, []);
  });

  test(`${tree.name}: the log routes reach their handlers`, () => {
    const table = tree.table();
    const keys = Object.keys(tree.logs);
    const { answers } = R.route(table, keys.map((k) => {
      const [method, target] = k.split(" ");
      return { method, target };
    }));
    keys.forEach((k, i) => {
      assert.strictEqual(typeof answers[i], "number", `${k} answered ${answers[i]}`);
      assert.strictEqual(table[answers[i]].handler, tree.logs[k], `${k} reached ${table[answers[i]].uri}`);
    });
    // Every POST /api/logs/... registration other than the log-ack sits
    // before it, and a seq the page can send (any uint32) reaches it.
    const ack = table.findIndex((g) => g.handler === tree.logs["POST /api/logs/1/ack"]);
    assert.strictEqual(table[ack].uri, "/api/logs/*");
    for (const [j, g] of table.entries()) {
      if (g.method === "POST" && g.uri.startsWith("/api/logs/") && j !== ack) {
        assert.ok(j < ack, `${g.uri} is registered after "/api/logs/*", which would refuse it`);
      }
    }
    const seqs = ["0", "7", "4294967295"];
    const r = R.route(table, seqs.map((s) => ({ method: "POST", target: `/api/logs/${s}/ack` })));
    r.answers.forEach((a, i) => assert.strictEqual(a, ack, `POST /api/logs/${seqs[i]}/ack`));
  });

  test(`${tree.name}: each page's per-entry Acknowledge is a URL its handler reads`, () => {
    const table = tree.table();
    const ack = table.findIndex((g) => g.handler === tree.logs["POST /api/logs/1/ack"]);
    assert.ok(ack >= 0, "the log-ack handler is registered");
    const asks = [];
    const seqs = [0, 7, 4294967295];
    for (const page of R.PAGES[tree.key]) {
      // Every request the page can send as a POST: the ones it names POST
      // and the ones whose method it does not name (a URL held for later,
      // a method it computes).
      const reqs = R.pageRequests(page).filter((r) => r.method === "POST" || r.method === null);
      const { answers } = R.route(table, reqs.map((r) => ({ method: "POST", target: r.route })));
      const acks = reqs.filter((r, i) => answers[i] === ack);
      if (page.name === tree.ackPage) {
        assert.ok(acks.length >= 1, `the ${page.name} makes a per-entry Acknowledge (a POST the log-ack route answers)`);
      }
      for (const r of acks) {
        const holes = r.raw.match(/\$\{[^}]*\}/g) || [];
        assert.strictEqual(holes.length, 1,
          `${page.name}: ${r.raw} (page line ${r.line}): an Acknowledge carries one value, the entry's seq`);
        for (const seq of seqs) asks.push({ page, r, seq, target: r.raw.replace(/\$\{[^}]*\}/, String(seq)) });
      }
    }
    const read = R.logAckParse(asks.map((a) => a.target));
    const wrong = asks.map((a, i) => {
      const got = read[i][tree.parser];
      return got === a.seq ? null :
        `${a.page.name}: ${a.r.raw} (page line ${a.r.line}) as ${a.target}: the handler reads ${got === null ? "no entry (404)" : got}`;
    }).filter(Boolean);
    assert.deepStrictEqual(wrong, []);
  });
}
