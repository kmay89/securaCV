// The route tables two firmware trees register, in the order httpd sees
// them, and the requests every page they serve from a raw string makes (the
// two dashboards since repo sweep F214; the setup page, the companion PWA,
// the Tuning Lab and the MQTT and Sense pages since F238), for the host tests
// that hold each request to the route it means:
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
// shadowing in any build is a shadowing here. Each registration also carries
// the #if conditions it is registered under (`conds`: every enclosing
// condition in its file, and for a registrar inlined from another file, the
// conditions around its call too; #elif and #else as the negations of the
// branches before them; include guards dropped), so a test can tell a route
// every build serves from one only some builds do (sweep F238). And the walk
// fails closed:
// every registration call in the tree is either one it visited or one in
// the short list of places that register on another server, so a route
// added somewhere the walk does not reach fails the test instead of
// vanishing from it.
//
// Past the route, one handler is asked too: logAckParse() hands targets to
// test_log_ack_route --parse, which runs both trees' log_ack_seq_from_uri()
// as cut out of the firmware, so a dashboard URL that reaches the log-ack
// route can be held to the entry the handler reads from it.

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

// ── Preprocessor conditions ──────────────────────────────────────────────

// The comment-stripped text with every string, character and raw string
// literal blanked (newlines kept), so a directive scan cannot read a page's
// CSS (`#if-x {`) or a string as a directive.
function blankLiterals(text) {
  let out = "";
  let i = 0;
  const n = text.length;
  const blank = (s) => s.replace(/[^\n]/g, " ");
  while (i < n) {
    const c = text[i];
    if (c === '"' && text[i - 1] === "R" && !/\w/.test(text[i - 2] || "")) {
      const open = text.indexOf("(", i);
      const delim = text.slice(i + 1, open);
      const close = text.indexOf(")" + delim + '"', open);
      assert.ok(open > i && close > open, "unterminated raw string literal");
      const end = close + delim.length + 2;
      out += blank(text.slice(i, end));
      i = end;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && text[j] !== c && text[j] !== "\n") j += text[j] === "\\" ? 2 : 1;
      out += blank(text.slice(i, j + 1));
      i = j + 1;
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

const negate = (atom) => {
  const d = atom.match(/^(!?)defined\((\w+)\)$/);
  if (d) return `${d[1] ? "" : "!"}defined(${d[2]})`;
  return `!(${atom})`;
};

// The #if conditions in force from each directive on: [{ at, conds }], `at`
// the offset of the line after the directive, `conds` the conjunction (one
// string per condition, whitespace collapsed) of every open branch.
function ppChanges(src) {
  if (src.pp) return src.pp;
  const lines = blankLiterals(src.text).split("\n");
  const stack = [];
  const changes = [{ at: 0, conds: [] }];
  let off = 0;
  for (let i = 0; i < lines.length; i++) {
    let line = lines[i];
    off += lines[i].length + 1;
    while (/\\\s*$/.test(line) && i + 1 < lines.length) {
      i++;
      line = line.replace(/\\\s*$/, " ") + lines[i];
      off += lines[i].length + 1;
    }
    const m = line.match(/^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$/);
    if (!m) continue;
    const expr = m[2].replace(/\s+/g, " ").trim();
    const where = `${src.path}:${lineAt(src.text, off - 1)}`;
    if (m[1] === "if" || m[1] === "ifdef" || m[1] === "ifndef") {
      assert.ok(expr, `${where}: #${m[1]} without a condition`);
      const atom = m[1] === "if" ? expr : m[1] === "ifdef" ? `defined(${expr})` : `!defined(${expr})`;
      // An include guard (`#ifndef X` then `#define X`) is no condition.
      let next = i + 1;
      while (next < lines.length && lines[next].trim() === "") next++;
      const guard = m[1] === "ifndef" && next < lines.length &&
        new RegExp(`^\\s*#\\s*define\\s+${expr}\\b`).test(lines[next]);
      stack.push(guard ? { guard, seen: [], cur: [] } : { guard, seen: [atom], cur: [atom] });
    } else if (m[1] === "elif") {
      const f = stack[stack.length - 1];
      assert.ok(f && !f.guard, `${where}: #elif outside an #if`);
      f.cur = [...f.seen.map(negate), expr];
      f.seen.push(expr);
    } else if (m[1] === "else") {
      const f = stack[stack.length - 1];
      assert.ok(f, `${where}: #else outside an #if`);
      f.cur = f.seen.map(negate);
    } else {
      assert.ok(stack.length, `${where}: #endif outside an #if`);
      stack.pop();
    }
    changes.push({ at: off, conds: stack.flatMap((f) => f.cur) });
  }
  assert.strictEqual(stack.length, 0, `${src.path}: an #if is never closed`);
  src.pp = changes;
  return changes;
}

// The conditions in force at an offset of a source.
function condsAt(src, at) {
  let conds = [];
  for (const c of ppChanges(src)) {
    if (c.at > at) break;
    conds = c.conds;
  }
  return conds;
}

const union = (...lists) => [...new Set(lists.flat())];

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
  const { servers, inline, visited, base = [] } = opts;
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
      walk(e.target.src, e.target.span,
        { ...opts, servers: e.target.servers, base: union(base, condsAt(src, abs)) }, out, {});
      continue;
    }
    const m = e.m;
    visited.add(`${src.path}:${abs}`);
    const conds = union(base, condsAt(src, abs));
    if (m[1]) {
      assert.ok(servers.includes(m[2]), `${where}: registers on ${m[2]}, not the primary server (${servers})`);
      out.push({ ...declFor(m[3], e.at, where), where, conds });
    } else {
      assert.ok(servers.includes(m[4]), `${where}: registers on ${m[4]}, not the primary server (${servers})`);
      out.push({ uri: m[5], method: m[6], handler: m[7].trim(), where, conds });
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
  const out = ask(lines);
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

// ── The log-ack handler's own parser ─────────────────────────────────────

// What each tree's log_ack_seq_from_uri() (cut verbatim out of the firmware
// into test_log_ack_route, which the Makefile builds) reads from each target:
// [{ pio, wap }] per target, each the seq (a number) or null (refused).
let logAckBin = null;
function logAckParse(targets) {
  if (!logAckBin) {
    if (process.env.LOG_ACK_ROUTE_BIN && existsSync(process.env.LOG_ACK_ROUTE_BIN)) {
      logAckBin = process.env.LOG_ACK_ROUTE_BIN;
    } else {
      // Run outside the Makefile's `run`: have make cut the functions and
      // build it (its recipe, its flags).
      execFileSync("make", ["-C", __dirname, "--no-print-directory", "build/test_log_ack_route"],
        { stdio: ["ignore", "ignore", "inherit"] });
      logAckBin = join(__dirname, "build", "test_log_ack_route");
    }
  }
  if (targets.length === 0) return [];
  const res = spawnSync(logAckBin, ["--parse"], { input: targets.join("\n") + "\n", encoding: "utf8" });
  assert.strictEqual(res.status, 0, `test_log_ack_route --parse failed: ${res.stderr}`);
  const out = res.stdout.trim().split("\n");
  assert.strictEqual(out.length, targets.length, "one answer per target");
  return out.map((l) => {
    const m = l.match(/^(\d+|-) (\d+|-)$/);
    assert.ok(m, `test_log_ack_route --parse answered ${l}`);
    const read = (v) => (v === "-" ? null : +v);
    return { pio: read(m[1]), wap: read(m[2]) };
  });
}

// ── The pages' requests ──────────────────────────────────────────────────

// Every page each tree serves from a raw string, with the handler(s) whose
// registrations serve it (as the table names them) and the file defining
// each handler (sweep F238 added all but the two dashboards). canary-wap's
// "/" is the Sensing dashboard, and "/sense" is a redirect to it
// (handle_sense_page), so that page is csi_integration.cpp's Sense page too.
// A page sent from its gzip copy (web_assets_gz.h) is read from the source
// string gen_web_assets_gz.py compresses, which its --check holds.
const PIO_SETUP = join(FW, "canary", "lib", "securacv_webui", "src", "securacv_setup_page.cpp");
const WAP_CSI = join(WAP_DIR, "csi_integration.cpp");
const WAP_MQTT = join(WAP_DIR, "csi_mqtt.cpp");
const PAGES = {
  pio: [
    { name: "dashboard", path: PIO_PAGE, symbol: "CANARY_UI_HTML",
      servedBy: [{ handler: "handle_ui", file: PIO_NET }] },
    { name: "setup page", path: PIO_SETUP, symbol: "CANARY_SETUP_HTML",
      servedBy: [{ handler: "handle_setup_page", file: PIO_NET }, { handler: "handle_captive_probe", file: PIO_NET }] },
  ],
  wap: [
    { name: "admin dashboard", path: WAP_PAGE, symbol: "CANARY_UI_HTML",
      servedBy: [{ handler: "handle_legacy_ui", file: WAP_INO }] },
    { name: "Sensing dashboard", path: join(WAP_DIR, "csi_dashboard_html.h"), symbol: "CSI_DASHBOARD_HTML",
      servedBy: [{ handler: "handle_ui", file: WAP_INO }] },
    { name: "companion PWA", path: join(WAP_DIR, "companion_pwa.h"), symbol: "COMPANION_HTML",
      servedBy: [{ handler: "handle_companion_html", file: WAP_INO }] },
    { name: "Tuning Lab", path: join(WAP_DIR, "tune_ui.h"), symbol: "TUNE_UI_HTML",
      servedBy: [{ handler: "handle_tune_page", file: WAP_CSI }] },
    { name: "MQTT page", path: WAP_MQTT, symbol: "MQTT_UI_HTML",
      servedBy: [{ handler: "csi_mqtt::handle_ui", file: WAP_MQTT }] },
    { name: "captive page", path: join(WAP_DIR, "setup_page_html.h"), symbol: "CAPTIVE_PORTAL_HTML",
      servedBy: [{ handler: "handle_ui", file: WAP_INO }, { handler: "handle_captive_probe", file: WAP_INO }] },
  ],
};

// Every raw string literal in a source: [{ name, delim, at }], `name` the
// array it initializes (`NAME[] [PROGMEM] = R"delim(`), or null.
function rawStrings(path) {
  const text = source(path).text;
  const out = [];
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"' && text[i - 1] === "R" && !/\w/.test(text[i - 2] || "")) {
      const open = text.indexOf("(", i);
      const delim = text.slice(i + 1, open);
      const close = text.indexOf(")" + delim + '"', open);
      assert.ok(open > i && close > open, `${path}: unterminated raw string literal`);
      const head = text.slice(Math.max(0, i - 160), i - 1).match(/(\w+)\s*\[\s*\]\s*(?:PROGMEM\s*)?=\s*$/);
      out.push({ name: head ? head[1] : null, delim, at: i });
      i = close + delim.length + 1;
    } else if (c === '"' || c === "'") {
      for (i++; i < text.length && text[i] !== c && text[i] !== "\n"; i++) if (text[i] === "\\") i++;
    }
  }
  return out;
}

// The body of the handler a page names, from the file that defines it.
function handlerBody(h) {
  const name = h.handler.split("::").pop();
  const src = source(h.file);
  const span = functionBody(src.text,
    new RegExp(`\\b(?:static\\s+)?esp_err_t\\s+${name}\\s*\\(\\s*httpd_req_t\\s*\\*\\s*\\w+\\s*\\)\\s*\\{`), h.file);
  return src.text.slice(span.start, span.end);
}

// The page's text: the raw string `SYMBOL[] PROGMEM = R"delim( ... )delim"`,
// whatever its delimiter, defined once in its file.
function pageText(page) {
  const src = source(page.path).raw;
  const heads = [...src.matchAll(new RegExp(`\\b${page.symbol}\\s*\\[\\s*\\]\\s*PROGMEM\\s*=\\s*R"([^(\\s]*)\\(`, "g"))];
  assert.strictEqual(heads.length, 1, `${page.path}: ${page.symbol}'s raw string is defined once`);
  const a = heads[0].index + heads[0][0].length;
  const b = src.indexOf(`)${heads[0][1]}"`, a);
  assert.ok(b > a, `${page.path}: ${page.symbol}'s raw string is closed`);
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
// secureFetch(url[, { method: 'METHOD' }]) / fetch(url[, {...}]) /
// cvFetch(url[, {...}]) (the Sensing dashboard's, the Tuning Lab's and the
// MQTT page's fetch() with the session cookie); the method a string literal
// in any quotes ('POST', "POST", `POST`); none named is GET, and a method
// the call computes is null (unknown).
const METHOD_LITERAL = /^(['"`])([A-Z]+)\1$/;
function callMethod(fn, args) {
  if (args.length < 2) return "GET";
  const second = args[1].text;
  if (fn === "api") {
    const named = second.match(METHOD_LITERAL);
    return named ? named[2] : null;
  }
  if (!second.startsWith("{")) return null;
  const named = second.match(/\bmethod\s*:\s*([^,}]+)/);
  if (!named) return "GET";
  const literal = named[1].trim().match(METHOD_LITERAL);
  return literal ? literal[2] : null;
}

// Every /api/... string or template literal in a page (single, double or
// back quotes), with the request it makes where the page says so: the path
// (a template's ${...} read as one segment, `1`; a query is not part of it)
// and the method, when the literal sits in the first argument of an api(),
// secureFetch(), fetch() or cvFetch() call (a ternary's arms included) that
// names it or none (GET). Elsewhere (a constant, a URL held for later, an
// href) the method is null.
function pageRequests(page) {
  const text = pageText(page);
  const calls = [];
  for (const c of text.matchAll(/\b(api|secureFetch|fetch|cvFetch)\(/g)) {
    const open = c.index + c[0].length - 1;
    const args = callArgs(text, open);
    if (args.length) calls.push({ fn: c[1], first: args[0], method: callMethod(c[1], args) });
  }
  const out = [];
  for (const m of text.matchAll(/(['"`])(\/api\/[^'"`?#]*)/g)) {
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
  PIO_NET, PIO_PAGE, WAP_INO, WAP_PAGE, PAGES,
  pioTable, wapTable, route, match, logAckParse, pageRequests, pageText, handlerBody, rawStrings, filesUnder,
  stripComments, blankLiterals, ppChanges, condsAt, source, FW, WAP_DIR,
};
