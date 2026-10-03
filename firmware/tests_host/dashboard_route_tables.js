// The route tables two firmware trees register, in the order httpd sees
// them, and the requests two dashboards make, for the host tests that hold
// each request to the route it means (repo sweep F214):
// test_dashboard_route_match.test.js and test_canary_dashboard_routes.test.js.
//
// Routing is not modeled here. Every question about which registration
// answers a request goes to idf_uri_route_oracle.c, a host build of
// esp_http_server's own httpd_uri_match_wildcard() (copied verbatim) and of
// the loop around it (first match in registration order wins; a template an
// earlier registration already matches is refused). What this file does is
// read the sources:
//
//   - the PlatformIO canary's primary table: registerHttpHandlers() in
//     canary/lib/securacv_network/src/securacv_network.cpp, the
//     `httpd_uri_t x = {...}; register_route(server, &x)` pairs in textual
//     order, the probe loop expanded over kProbePaths;
//   - canary-wap's primary table: register_api_routes() (with
//     csi_integration::init() inlined where it is called), then
//     start_http_server() from its `register_extra_routes:` label on (each
//     X_api::register_routes() inlined where it is called), in textual order.
//     The HTTPS branch registers the same two parts, in the same order, on
//     the TLS server; the port-80 redirect server's own table (probes,
//     /api/fleet, the "/*" redirects) answers no dashboard call.
//
// Every #if branch is read (the worst case, as both route budget checks
// count): a build has a subsequence of the table, in the same order, so a
// shadowing in any build is a shadowing here. And the walk fails closed:
// every registration call in the tree is either one it visited or one in
// the short list of places that register on another server, so a route
// added somewhere the walk does not reach fails the test instead of
// vanishing from it.

"use strict";

const assert = require("node:assert");
const { execFileSync, spawnSync } = require("node:child_process");
const { existsSync, mkdtempSync, readFileSync, readdirSync } = require("node:fs");
const { tmpdir } = require("node:os");
const { join } = require("node:path");

const FW = join(__dirname, "..");
const PIO_NET = join(FW, "canary", "lib", "securacv_network", "src", "securacv_network.cpp");
const PIO_PAGE = join(FW, "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
const WAP_DIR = join(FW, "projects", "canary-wap", "arduino", "canary_wap");
const WAP_INO = join(WAP_DIR, "canary_wap.ino");
const WAP_PAGE = join(WAP_DIR, "web_ui.h");

// ── Source reading ───────────────────────────────────────────────────────

// Comments become spaces (newlines kept, so offsets and line numbers hold);
// string and character literals are kept as they are.
function stripComments(text) {
  let out = "";
  let i = 0;
  const n = text.length;
  while (i < n) {
    const c = text[i];
    const d = text[i + 1];
    if (c === "/" && d === "/") {
      while (i < n && text[i] !== "\n") { out += " "; i++; }
    } else if (c === "/" && d === "*") {
      out += "  "; i += 2;
      while (i < n && !(text[i] === "*" && text[i + 1] === "/")) { out += text[i] === "\n" ? "\n" : " "; i++; }
      if (i < n) { out += "  "; i += 2; }
    } else if (c === '"' || c === "'") {
      // A raw string literal (R"delim( ... )delim") is copied whole.
      if (c === '"' && text[i - 1] === "R") {
        const open = text.indexOf("(", i);
        const delim = text.slice(i + 1, open);
        const close = text.indexOf(")" + delim + '"', open);
        assert.ok(open > i && close > open, "unterminated raw string literal");
        out += text.slice(i, close + delim.length + 2);
        i = close + delim.length + 2;
        continue;
      }
      out += c; i++;
      while (i < n && text[i] !== c) {
        if (text[i] === "\\") { out += text[i] + text[i + 1]; i += 2; continue; }
        out += text[i]; i++;
      }
      out += c; i++;
    } else {
      out += c; i++;
    }
  }
  return out;
}

// The offset just past the brace that closes the one at `open`, skipping
// braces inside string and character literals (comments are already gone).
function closeBrace(text, open) {
  assert.strictEqual(text[open], "{", "closeBrace starts at a brace");
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (c === '"' || c === "'") {
      for (i++; i < text.length && text[i] !== c; i++) if (text[i] === "\\") i++;
    } else if (c === "{") {
      depth++;
    } else if (c === "}" && --depth === 0) {
      return i + 1;
    }
  }
  assert.fail("unbalanced braces");
}

// A function's body, found by a regex for its head that ends at the `{`.
// Exactly one definition must match.
function functionBody(text, head, where) {
  const re = new RegExp(head.source, "g");
  const found = [...text.matchAll(re)];
  assert.strictEqual(found.length, 1, `${where}: ${head} defined ${found.length} time(s), expected once`);
  const open = found[0].index + found[0][0].length - 1;
  assert.strictEqual(text[open], "{", `${where}: ${head} does not end at the body's brace`);
  return { start: open, end: closeBrace(text, open) };
}

const sources = new Map();
function source(path) {
  if (!sources.has(path)) {
    const raw = readFileSync(path, "utf8");
    sources.set(path, { path, raw, text: stripComments(raw) });
  }
  return sources.get(path);
}
const lineAt = (text, at) => text.slice(0, at).split("\n").length;

// ── Registration walks ───────────────────────────────────────────────────

// The registration forms both trees use. A declaration
// `httpd_uri_t NAME = { .uri = ..., .method = HTTP_X, .handler = H }` is
// registered by `httpd_register_uri_handler(SERVER, &NAME)` or the
// PlatformIO tree's `register_route(SERVER, &NAME)`; canary-wap's modules
// also call `register_api_handler(SERVER, "uri", HTTP_X, handler)`.
const CALL_RE = /\b(httpd_register_uri_handler|register_route)\s*\(\s*(\w+)\s*,\s*&\s*(\w+)\s*\)|\bregister_api_handler\s*\(\s*(\w+)\s*,\s*"([^"]*)"\s*,\s*HTTP_(\w+)\s*,\s*([^;]*?)\)\s*;/g;
const DECL_RE = /\bhttpd_uri_t\s+(\w+)\s*=\s*\{([^}]*)\}/g;
const LOOP_RE = /\bfor\s*\(\s*const\s+char\s*\*\s*(\w+)\s*:\s*(\w+)\s*\)\s*\{/g;

function parseDecl(fields, bindings, where) {
  const uri = fields.match(/\.uri\s*=\s*(?:"([^"]*)"|(\w+))/);
  const method = fields.match(/\.method\s*=\s*HTTP_(\w+)/);
  const handler = fields.match(/\.handler\s*=\s*([^,}]+?)\s*(?:,|$)/);
  assert.ok(uri && method && handler, `${where}: a route declaration without .uri/.method/.handler`);
  let template = uri[1];
  if (template === undefined) {
    template = bindings[uri[2]];
    assert.ok(template !== undefined, `${where}: .uri = ${uri[2]} names no string the walk knows`);
  }
  return { uri: template, method: method[1], handler: handler[1].trim() };
}

// The strings of `static const char* NAME[] = { "...", ... };` in a file.
function stringArray(src, name) {
  const m = src.text.match(new RegExp(`\\bconst\\s+char\\s*\\*\\s*${name}\\s*\\[\\s*\\]\\s*=\\s*\\{([^}]*)\\}`));
  assert.ok(m, `${src.path}: array ${name} not found`);
  return [...m[1].matchAll(/"([^"]*)"/g)].map((s) => s[1]);
}

// Walk one span of a source in textual order. `servers` are the handle names
// that mean the primary server in this span; `inline(name, args)` returns
// the span a called registrar stands for (or null: not a registrar).
// Visited call sites are recorded in `visited` as "path:offset".
function walk(src, span, opts, out, bindings = {}) {
  const { servers, inline, visited } = opts;
  const text = src.text;
  const body = text.slice(span.start, span.end);
  // The declaration a call names: the last one of that name above the call
  // in this span, read with the span's loop bindings.
  const declFor = (name, before, where) => {
    let last = null;
    for (const d of body.slice(0, before).matchAll(DECL_RE)) if (d[1] === name) last = d;
    assert.ok(last, `${where}: &${name} names no httpd_uri_t declared above it in the same function`);
    return parseDecl(last[2], bindings, where);
  };
  // Every event in order: calls, loops, inlined registrar calls.
  const events = [];
  for (const m of body.matchAll(CALL_RE)) events.push({ at: m.index, kind: "call", m });
  for (const m of body.matchAll(LOOP_RE)) events.push({ at: m.index, kind: "loop", m });
  if (inline) {
    for (const m of body.matchAll(/\b(\w+)::(\w+)\s*\(([^;]*)\)\s*;/g)) {
      const target = inline(m[1], m[2], m[3]);
      if (target) events.push({ at: m.index, kind: "inline", target });
    }
  }
  events.sort((a, b) => a.at - b.at);
  let skipUntil = -1;
  for (const e of events) {
    if (e.at < skipUntil) continue;
    const abs = span.start + e.at;
    const where = `${src.path}:${lineAt(text, abs)}`;
    if (e.kind === "loop") {
      const open = abs + e.m[0].length - 1;
      const end = closeBrace(text, open);
      for (const value of stringArray(src, e.m[2])) {
        walk(src, { start: open, end }, opts, out, { ...bindings, [e.m[1]]: value });
      }
      skipUntil = end - span.start;
      continue;
    }
    if (e.kind === "inline") {
      walk(e.target.src, e.target.span, { ...opts, servers: e.target.servers }, out, {});
      continue;
    }
    const m = e.m;
    visited.add(`${src.path}:${abs}`);
    if (m[1]) {
      assert.ok(servers.includes(m[2]), `${where}: registers on ${m[2]}, not the primary server (${servers})`);
      out.push({ ...declFor(m[3], e.at, where), where });
    } else {
      assert.ok(servers.includes(m[4]), `${where}: registers on ${m[4]}, not the primary server (${servers})`);
      out.push({ uri: m[5], method: m[6], handler: m[7].trim(), where });
    }
  }
  return out;
}

// Every registration call site in a list of sources, as "path:offset".
function callSites(paths) {
  const sites = new Map();
  for (const path of paths) {
    const src = source(path);
    for (const m of src.text.matchAll(CALL_RE)) {
      sites.set(`${path}:${m.index}`, { src, at: m.index, server: m[2] || m[4] });
    }
  }
  return sites;
}

function filesUnder(dir, pattern) {
  const out = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) {
      if (entry.name !== ".pio" && entry.name !== "node_modules") out.push(...filesUnder(path, pattern));
    } else if (pattern.test(entry.name)) {
      out.push(path);
    }
  }
  return out;
}

// The sites the walk must not visit, each with the reason it is not the
// primary table; checkCoverage() fails on any other unvisited site.
function checkCoverage(paths, visited, others) {
  const unexplained = [];
  for (const [key, site] of callSites(paths)) {
    if (visited.has(key)) continue;
    if (others.some((o) => o(site))) continue;
    unexplained.push(`${site.src.path}:${lineAt(site.src.text, site.at)} (on ${site.server})`);
  }
  assert.deepStrictEqual(unexplained, [],
    "registration calls the route walk does not reach: teach dashboard_route_tables.js where they go");
}

// Is `at` inside the body of a function whose head matches `head`?
function insideFunction(src, at, head) {
  for (const m of src.text.matchAll(new RegExp(head.source, "g"))) {
    const open = m.index + m[0].length - 1;
    if (src.text[open] !== "{") continue;
    if (at > open && at < closeBrace(src.text, open)) return true;
  }
  return false;
}

// The PlatformIO canary's primary table, in registration order.
function pioTable() {
  const src = source(PIO_NET);
  const visited = new Set();
  const span = functionBody(src.text, /void\s+ScvNetworkManager::registerHttpHandlers\s*\(\s*httpd_handle_t\s+server\s*\)\s*\{/, PIO_NET);
  const table = walk(src, span, { servers: ["server"], inline: null, visited }, []);
  const files = filesUnder(join(FW, "canary"), /\.(c|cc|cpp|h|hpp|ino)$/)
    .concat(filesUnder(join(FW, "common"), /\.(c|cc|cpp|h|hpp|ino)$/))
    .filter((p) => p !== PIO_PAGE);
  checkCoverage(files, visited, [
    // The FEATURE_HTTPS port-80 server: the six probes and the two "/*"
    // redirects to https://. It answers no dashboard call.
    (s) => s.src.path === PIO_NET && s.server === "m_http_server" &&
           insideFunction(s.src, s.at, /bool\s+ScvNetworkManager::startRedirectServer\s*\(\s*\)\s*\{/),
  ]);
  return table;
}

// canary-wap's primary table, in registration order.
function wapTable() {
  const ino = source(WAP_INO);
  const visited = new Set();
  const moduleFiles = readdirSync(WAP_DIR).filter((f) => /\.(h|cpp)$/.test(f)).map((f) => join(WAP_DIR, f));
  // csi_integration::init(server, ...) and X_api::register_routes(server, ...)
  // stand for the registrar's body in the file that defines it.
  const inline = (ns, fn, args) => {
    if (!(fn === "register_routes" || (ns === "csi_integration" && fn === "init"))) return null;
    const server = args.split(",")[0].trim();
    const head = fn === "init"
      ? /\bbool\s+init\s*\(\s*httpd_handle_t\s+server\b[^)]*\)\s*\{/
      : /\b(?:inline\s+)?void\s+register_routes\s*\(\s*httpd_handle_t\s+server\b[^)]*\)\s*\{/;
    const homes = moduleFiles.filter((p) => new RegExp(`\\bnamespace\\s+${ns}\\s*\\{`).test(source(p).text) &&
                                           head.test(source(p).text));
    assert.strictEqual(homes.length, 1, `${ns}::${fn}() is defined in ${homes.length} sketch files, expected one`);
    const src = source(homes[0]);
    return { src, span: functionBody(src.text, head, homes[0]), servers: ["server"], server };
  };
  const table = [];
  const api = functionBody(ino.text, /\bstatic\s+void\s+register_api_routes\s*\(\s*httpd_handle_t\s+server\s*\)\s*\{/, WAP_INO);
  walk(ino, api, { servers: ["server"], inline, visited }, table);
  const start = functionBody(ino.text, /\bstatic\s+void\s+start_http_server\s*\(\s*\)\s*\{/, WAP_INO);
  // Both branches call register_api_routes() on the primary server (TLS or
  // plain) and then reach the label: the order the walk reads.
  const calls = [...ino.text.slice(start.start, start.end).matchAll(/\bregister_api_routes\s*\(\s*(\w+)\s*\)/g)].map((m) => m[1]);
  assert.deepStrictEqual(calls.sort(), ["g_http_server", "g_https_server"],
    "start_http_server() calls register_api_routes() once per primary server");
  const label = ino.text.indexOf("register_extra_routes:", start.start);
  assert.ok(label > start.start && label < start.end, "start_http_server() has its register_extra_routes: label");
  assert.match(ino.text.slice(label, start.end),
    /httpd_handle_t\s+active_server\s*=\s*g_https_server\s*\?\s*g_https_server\s*:\s*g_http_server\s*;/,
    "the label's routes go on the primary server");
  walk(ino, { start: label, end: start.end }, { servers: ["active_server"], inline, visited }, table);
  checkCoverage([WAP_INO, ...moduleFiles], visited, [
    // The HTTPS build's port-80 server (probes, GET/OPTIONS /api/fleet and
    // the "/*" redirects): registered on g_http_server inside the HTTPS
    // branch, before the goto. It answers no dashboard call.
    (s) => s.src.path === WAP_INO && s.server === "g_http_server" && s.at > start.start && s.at < label,
    // The modules' register_api_handler() helpers: the call inside each is
    // the registration every register_api_handler(...) line above stands for.
    (s) => s.server === "server" &&
           insideFunction(s.src, s.at, /\bvoid\s+register_api_handler\s*\(\s*httpd_handle_t\s+server\s*,\s*const\s+char\s*\*\s*uri\s*,\s*httpd_method_t\s+method\s*,\s*esp_err_t\s*\(\s*\*\s*handler\s*\)\s*\(\s*httpd_req_t\s*\*\s*\)\s*\)\s*\{/),
  ]);
  return table;
}

// ── The oracle ───────────────────────────────────────────────────────────

let oracleBin = null;
function oracle() {
  if (oracleBin) return oracleBin;
  if (process.env.IDF_ROUTE_ORACLE && existsSync(process.env.IDF_ROUTE_ORACLE)) {
    oracleBin = process.env.IDF_ROUTE_ORACLE;
    return oracleBin;
  }
  // Run outside the Makefile: build it (the Makefile's recipe, same flags).
  const dir = mkdtempSync(join(tmpdir(), "idf-route-oracle-"));
  oracleBin = join(dir, "idf_uri_route_oracle");
  execFileSync(process.env.CC || "cc",
    ["-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-sign-compare", "-Werror",
     join(__dirname, "idf_uri_route_oracle.c"), "-o", oracleBin], { stdio: "inherit" });
  return oracleBin;
}

function ask(lines) {
  const res = spawnSync(oracle(), { input: lines.join("\n") + "\n", encoding: "utf8", maxBuffer: 1 << 26 });
  assert.strictEqual(res.status, 0, `idf_uri_route_oracle failed: ${res.stderr}`);
  const out = res.stdout.trim().split("\n");
  assert.strictEqual(out.length, lines.length, "one answer per command");
  return out;
}

// The matcher alone: does each [template, target] pair match (on the
// target's path, as httpd_uri() hands it over)?
function match(pairs) {
  if (pairs.length === 0) return [];
  return ask(pairs.map(([t, u]) => `M ${t} ${u}`)).map((l) => {
    assert.ok(l === "M 1" || l === "M 0", `oracle answered ${l}`);
    return l === "M 1";
  });
}

// One server: register `table` in order, then route each request. Returns
// { regs: [{ slot } | { exists: earlier slot }] per registration,
//   answers: [slot (a number) | "404" | "405"] per request }.
function route(table, requests) {
  const lines = ["S"];
  for (const r of table) lines.push(`R ${r.method} ${r.uri}`);
  for (const q of requests) lines.push(`Q ${q.method} ${q.target}`);
  const res = { stdout: ask(lines).join("\n"), status: 0 };
  const out = res.stdout.split("\n");
  const regs = out.slice(1, 1 + table.length).map((l) => {
    const m = l.match(/^R (?:(\d+)|EXISTS (\d+))$/);
    assert.ok(m, `oracle answered ${l}`);
    return m[1] !== undefined ? { slot: +m[1] } : { exists: +m[2] };
  });
  const answers = out.slice(1 + table.length).map((l) => {
    const m = l.match(/^Q (?:(\d+)|E(404|405))$/);
    assert.ok(m, `oracle answered ${l}`);
    return m[1] !== undefined ? +m[1] : m[2];
  });
  return { regs, answers };
}

// ── The dashboards' requests ─────────────────────────────────────────────

// The page served by `path`: the one raw string the file holds.
function pageText(path) {
  const src = source(path).raw;
  const a = src.indexOf('R"rawliteral(');
  const b = src.indexOf(')rawliteral"', a);
  assert.ok(a >= 0 && b > a, `${path}: the page's raw string is in the file`);
  return src.slice(a, b);
}

// A call's arguments from its opening parenthesis: [{ text, start, end }],
// split at top-level commas, string and template literals skipped whole.
function callArgs(text, open) {
  assert.strictEqual(text[open], "(", "callArgs starts at a parenthesis");
  const args = [];
  let depth = 0;
  let start = open + 1;
  const push = (end) => {
    const raw = text.slice(start, end);
    const lead = raw.length - raw.trimStart().length;
    args.push({ text: raw.trim(), start: start + lead, end: start + lead + raw.trim().length });
  };
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (c === "'" || c === '"' || c === "`") {
      for (i++; i < text.length && text[i] !== c; i++) if (text[i] === "\\") i++;
    } else if (c === "(" || c === "[" || c === "{") {
      depth++;
    } else if (c === ")" || c === "]" || c === "}") {
      if (--depth === 0) {
        if (text.slice(start, i).trim() !== "" || args.length) push(i);
        return args;
      }
    } else if (c === "," && depth === 1) {
      push(i);
      start = i + 1;
    }
  }
  assert.fail("a call that never closes");
}

// The method a request call names: api(url[, 'METHOD']) and
// secureFetch(url[, { method: 'METHOD' }]) / fetch(url[, {...}]); none named
// is GET, and a method the call computes is null (unknown).
function callMethod(fn, args) {
  if (args.length < 2) return "GET";
  const second = args[1].text;
  if (fn === "api") {
    const named = second.match(/^'([A-Z]+)'$/);
    return named ? named[1] : null;
  }
  if (!second.startsWith("{")) return null;
  const named = second.match(/\bmethod\s*:\s*([^,}]+)/);
  if (!named) return "GET";
  const literal = named[1].trim().match(/^'([A-Z]+)'$/);
  return literal ? literal[1] : null;
}

// Every /api/... string or template literal in a page's script, with the
// request it makes where the page says so: the path (a template's ${...}
// read as one segment, `1`; a query is not part of it) and the method, when
// the literal sits in the first argument of an api(), secureFetch() or
// fetch() call (a ternary's arms included) that names it or none (GET).
// Elsewhere (a constant, a URL held for later) the method is null.
function pageRequests(path) {
  const text = pageText(path);
  const calls = [];
  for (const c of text.matchAll(/\b(api|secureFetch|fetch)\(/g)) {
    const open = c.index + c[0].length - 1;
    const args = callArgs(text, open);
    if (args.length) calls.push({ fn: c[1], first: args[0], method: callMethod(c[1], args) });
  }
  const out = [];
  for (const m of text.matchAll(/(['`])(\/api\/[^'`?]*)/g)) {
    const raw = m[2];
    const route = raw.replace(/\$\{[^}]*\}/g, "1");
    let method = null;
    let best = null;
    for (const c of calls) {
      if (m.index >= c.first.start && m.index < c.first.end && (!best || c.first.start > best.first.start)) best = c;
    }
    if (best) method = best.method;
    out.push({ raw, route, method, line: text.slice(0, m.index).split("\n").length });
  }
  return out;
}

module.exports = {
  PIO_NET, PIO_PAGE, WAP_INO, WAP_PAGE,
  pioTable, wapTable, route, match, pageRequests, pageText, stripComments,
};
