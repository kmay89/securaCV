// Every request two dashboards make reaches the route it means, as
// esp_http_server routes it (repo sweep F214).
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
// it) and every request the two dashboards make
// (canary/lib/securacv_webui/src/securacv_webui.cpp, and canary-wap's
// web_ui.h). What this pins:
//   - the copy is the IDF function, byte for byte (a hash of its text), and
//     the oracle answers as IDF's matcher does on the cases that decided F214;
//   - no registration in either table is refused as a duplicate;
//   - every dashboard request whose method the page names reaches the
//     registration it means: an exact template equal to its path if one is
//     registered, else the one wildcard that matches it (the PlatformIO
//     page's Chirp and Bluetooth calls excepted: gated, nothing serves them,
//     F176/F198); a URL the page holds for later reaches its route by some
//     method;
//   - the log routes reach their handlers on both trees: POST
//     /api/logs/<seq>/ack the log-ack handler, ack-all and rotate theirs.
//
// Run: node --test firmware/tests_host/test_dashboard_route_match.test.js
// (the Makefile's `run` target does, with the oracle it built; run alone,
// the test builds it with $CC, default cc).

"use strict";

const { test } = require("node:test");
const assert = require("node:assert");
const { createHash } = require("node:crypto");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const R = require("./dashboard_route_tables.js");

// sha256 of the text from "bool httpd_uri_match_wildcard(" through its
// closing brace, as it stands in ESP-IDF release/v5.5
// components/esp_http_server/src/httpd_uri.c (and v4.4.7, v5.3.2).
const IDF_MATCHER_SHA256 = "3df494155c6770152cbc15743617a5e7473b09adb842221f7ea4731e1b30a396";

const TREES = [
  { name: "PlatformIO canary", table: R.pioTable, page: R.PIO_PAGE, gated: ["/api/chirp", "/api/bluetooth"],
    logs: { "GET /api/logs": "handle_logs", "POST /api/logs/1/ack": "handle_log_ack",
            "POST /api/logs/ack-all": "handle_ack_all" } },
  { name: "canary-wap", table: R.wapTable, page: R.WAP_PAGE, gated: [],
    logs: { "GET /api/logs": "handle_logs_auth", "POST /api/logs/1/ack": "handle_log_ack_auth",
            "POST /api/logs/ack-all": "handle_ack_all_auth", "POST /api/logs/rotate": "handle_logs_rotate_auth" } },
];

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

for (const tree of TREES) {
  test(`${tree.name}: no registration is refused as a duplicate`, () => {
    const table = tree.table();
    assert.ok(table.length > 50, "the table was read");
    const { regs } = R.route(table, []);
    const refused = regs.map((r, i) => r.exists === undefined ? null :
      `${table[i].method} ${table[i].uri} (${table[i].where}) — ${table[r.exists].uri} (${table[r.exists].where}) matches it first`)
      .filter(Boolean);
    assert.deepStrictEqual(refused, []);
  });

  test(`${tree.name}: every dashboard request reaches the route it means`, () => {
    const table = tree.table();
    const reqs = R.pageRequests(tree.page)
      .filter((r) => !tree.gated.some((g) => r.route === g || r.route.startsWith(g + "/")));
    assert.ok(reqs.length > 50, "the page's requests were read");
    // Each request, for each method it can be: the one the page names, or
    // (a URL held for later) any method a registration uses.
    const methods = [...new Set(table.map((r) => r.method))];
    const asks = [];
    for (const r of reqs) for (const m of r.method ? [r.method] : methods) asks.push({ r, method: m });
    const matched = R.match(asks.flatMap(({ r }) => table.map((g) => [g.uri, r.route])));
    const { answers } = R.route(table, asks.map((a) => ({ method: a.method, target: a.r.route })));
    const meant = (i) => {
      const { r, method } = asks[i];
      const row = matched.slice(i * table.length, (i + 1) * table.length);
      const exact = table.findIndex((g) => g.method === method && g.uri === r.route);
      if (exact >= 0) return exact;
      const wild = table.map((g, j) => (g.method === method && row[j] && g.uri !== r.route ? j : -1)).filter((j) => j >= 0);
      return wild.length === 1 ? wild[0] : wild.length ? `${wild.length} wildcards` : null;
    };
    const wrong = [];
    const reached = new Map();
    asks.forEach((a, i) => {
      const want = meant(i);
      const ok = typeof want === "number" && answers[i] === want;
      if (a.r.method) {
        if (!ok) wrong.push(`${a.method} ${a.r.route} (page line ${a.r.line}): answered ` +
          `${typeof answers[i] === "number" ? table[answers[i]].uri : answers[i]}, means ` +
          `${typeof want === "number" ? table[want].uri : want || "nothing registered"}`);
      } else {
        reached.set(a.r, (reached.get(a.r) || false) || ok);
      }
    });
    for (const [r, ok] of reached) if (!ok) wrong.push(`${r.route} (page line ${r.line}): no method reaches a route`);
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
}
