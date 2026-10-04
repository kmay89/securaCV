// canary-local/tests/probe_server.test.js — every browser probe serves the
// repository from an index built before its server starts, on loopback only.
//
// A probe's server must never turn a request URL into a filesystem path: the
// URL is untrusted input, and decode, resolve(join(ROOT, rel)), then a
// startsWith(ROOT + sep) test is the shape CodeQL's path-injection query
// flagged twice on #1737. probe_server.mjs indexes the tree once and answers
// a request with a Map lookup, so the path that reaches readFile is the
// index's own string; twelve probes still did the dance until sweep A46. The
// allowlist probes (bench, boardroom, workshop, boot, onboard) build their own
// map of the files they serve before their server starts, which holds to the
// same rule: the path a read gets is the map's, never the request's.
//
// What can rot, and which test catches it:
//   · lookup() starts reading a request as a path      → "lookup answers from the index"
//   · a probe resolves a request path itself again     → "no probe resolves a request path"
//   · a probe reads a file its index did not hand it   → same test
//   · a probe server listens beyond loopback           → same test
//   · a probe throws on a malformed escape (/%E0)      → same test
//   · the scan stops seeing any of those               → "the request-path scan refuses"
//   · a real probe's guard is put back unguarded       → "the scan refuses each allowlist probe with its decode unguarded"
//   · the scan stops finding a server file             → same test
//   · the gate is dropped from CI                      → "CI runs this gate"
//
// Runs under "page logic tests" (.github/workflows/canary-local.yml); reads
// source text and a scratch tree in the OS temp directory, starts no browser.

"use strict";

const { test } = require("node:test");
const assert = require("node:assert");
const fs = require("node:fs");
const os = require("node:os");
const { join } = require("node:path");
const { readJs, codeOnly, IDENT, esc, lineOf, trivia, argsAt, argSpansAt, functionsIn } = require("./js_scan.js");

const REPO = join(__dirname, "..", "..");
const read = (p) => fs.readFileSync(p, "utf8");

// ── probe_server.mjs itself ──────────────────────────────────────────────────

test("lookup answers from the index: never a path the request spells", async () => {
  const { indexTree, lookup } = await import("./probe_server.mjs");
  const root = fs.mkdtempSync(join(os.tmpdir(), "probe-server-"));
  try {
    const put = (rel, body) => {
      fs.mkdirSync(join(root, rel, ".."), { recursive: true });
      fs.writeFileSync(join(root, rel), body);
    };
    put("canary-local/vision.html", "page");
    put("canary-local/assets/a b.js", "spaced");
    put("canary-local/emulator/dist/core.js", "core");
    for (const skipped of [".git/config", "node_modules/x/index.js", "canary-local/emulator/third_party/lvgl.c",
      "x/__pycache__/m.pyc", "desktop/target/app", "x/.build/o"]) put(skipped, "secret");
    const files = indexTree(root);
    assert.deepStrictEqual([...files.keys()].sort(), ["/canary-local/assets/a b.js", "/canary-local/emulator/dist/core.js",
      "/canary-local/vision.html"], "every regular file, keyed by its URL path, and nothing under a skipped directory");
    assert.strictEqual(lookup(files, "/canary-local/vision.html"), join(root, "canary-local/vision.html"));
    assert.strictEqual(lookup(files, "/canary-local/vision.html?flavor=dash&x=1"), join(root, "canary-local/vision.html"),
      "the query is not part of the key");
    assert.strictEqual(lookup(files, "/canary-local/assets/a%20b.js"), join(root, "canary-local/assets/a b.js"),
      "a percent-encoded name is decoded once, then looked up");
    for (const miss of [
      "/canary-local/emulator/dist/../../vision.html", "/canary-local/x/%2e%2e/vision.html", "/canary-local//vision.html",
      "/../canary-local/vision.html", "/canary-local/", "/canary-local", "canary-local/vision.html",
      "http://127.0.0.1/canary-local/vision.html", "/.git/config", "/node_modules/x/index.js",
      "/canary-local/emulator/third_party/lvgl.c", "/canary-local/vision.html#top", "/canary-local/vision.html%3Fx",
      "/%E0canary-local/vision.html",
    ]) assert.strictEqual(lookup(files, miss), null, `${miss}: not a key, so not a file`);
    // a malformed escape is a miss, never a throw: the allowlist probes hand
    // lookup() the raw request for exactly this (sweep A52)
    for (const bad of ["/%E0", "/%", "/%zz", "/canary-local/%C0%AF", "/%ED%A0%80", "/canary-local/vision.html%"]) {
      assert.doesNotThrow(() => lookup(files, bad), `${bad}: lookup() threw`);
      assert.strictEqual(lookup(files, bad), null, `${bad}: undecodable, so not a file`);
    }
    // the index holds the paths it walked; a file the tree gains later is not served
    put("canary-local/late.html", "late");
    assert.strictEqual(lookup(files, "/canary-local/late.html"), null);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});


// ── every probe's server ─────────────────────────────────────────────────────

// What the scan holds in a server file: any file here but a test that
// creates a server (createServer, createSecureServer, new …Server), says
// `listen`, or answers a page's requests (page.route, route.fulfill). All
// within the one file:
//
//   0. It names node:fs and node:path only in ways the scan sees called:
//      fs and path imported or required whole under a name it knows (fs, fsp,
//      path, posix, win32, promises), their functions under their own names,
//      or one function that is not a path function called on the module at
//      once ((await import("node:fs/promises")).mkdir(…), render_probe's).
//      Refused by name: a renamed import, require or destructuring (readFile
//      as rf, { promises: p }), any other require or import(…) of them, a
//      module it cannot read, an alias of a read, write or path function or
//      of fs or path (const crs = fs.createReadStream), such a function
//      handed itself to a call (Reflect.apply(readFile, …)), its .call,
//      .apply or .bind, eval, Function, createRequire, and a computed member
//      of fs or path.
//   1. It can find every handler: a server's (its last argument) or a page
//      route's (its second), inline or a function the file defines by that
//      name, whose request (or route) parameter is a plain name. A "request"
//      listener (server.on("request", …)) is refused: the scan follows only
//      the handler a server is created with.
//   2. Every filesystem read inside a handler (readFile and its family, bare
//      or a method of anything) reads a name declared in the handler whose
//      every write there is the index's answer: lookup(R, …) or R.get(…),
//      optionally `?? null` or `|| null`, where R is a map the file declares
//      once, at top level, before its server, as indexTree(…) or new Map(…),
//      and every other mention of R is a read (get, has, size, keys, values,
//      entries), a lookup(R, …), or sits before the server either at top
//      level or in a function only that top-level code calls (the allowlist
//      probes' allow()). A read of anything else is refused, whatever it
//      holds; so is route.fulfill() handed a path or anything but an object
//      literal.
//   3. Nothing derived from the request reaches a path function (join,
//      resolve, normalize, relative, bare or on path/posix/win32) or a
//      filesystem read or write (bare or a method of anything). Derived
//      means: the request; every name written (declared, assigned,
//      destructured, a loop's variable, a member of it) from an expression
//      that mentions it or another such name, the index's answer excepted;
//      the object a method call hands one of those to (parts.push(req.url)
//      taints parts) when the file declares it, unless the method only reads
//      (get, has) or is itself a read, write or path function; each
//      parameter of an inline function handed to a call that one of those
//      reaches; and each parameter of a function the file defines that a
//      call hands one of those (or that is handed by name to such a call),
//      followed into that function. A name the handler (or such a function)
//      does not declare that comes to hold the request carries it out: it is
//      then followed through the whole file, every function and the top
//      level alike, until no new name escapes.
//   4. Every `listen` in it is a .listen(…) the scan can read, on loopback:
//      listen(port, "127.0.0.1") (or "::1"), or an options object whose host
//      is one of those. "listen" spelled as a string is refused.
//   5. A malformed request is answered, never thrown out of a handler (an
//      async handler's throw is an unhandled rejection, and Node ends the
//      probe with exit 1 naming no request): every decodeURIComponent and
//      decodeURI in the file is a call inside a try whose catch swallows it
//      (no `throw` in the catch), with no function, method or arrow starting
//      between the try and the call; one handed itself on is refused. The
//      probes call lookup(), whose decode answers null. A `new URL(…)` handed
//      something derived from the request (rule 3's sense) is held the same
//      way, since a target like "//" throws ERR_INVALID_URL.
//
// What it cannot follow: a function of another module that a handler hands
// the request (the A41 bridge's handle() is one; native_cores.test.js tests
// it), a class's method or `this`, a method of a global (console, process,
// globalThis) handed the request, and a value that reaches a handler from
// outside any handler the scan knows (a Playwright event's URL, say). Rule 2
// is the backstop for those inside a handler: whatever a value went through,
// the handler reads only what an index built before the server answered.
// Rule 5 sees a decode by name, so not one reached through a computed member
// of a global (globalThis[k]), nor a throw from another module's function.

const PATH_FNS = ["join", "resolve", "normalize", "relative"];
const FS_FNS = ["readFile", "readFileSync", "createReadStream", "stat", "statSync", "lstat", "lstatSync", "access",
  "accessSync", "existsSync", "open", "openSync", "readdir", "readdirSync", "realpath", "realpathSync", "opendir"];
// what writes, which rule 3 holds as it holds a read
const FS_WRITES = ["writeFile", "writeFileSync", "appendFile", "appendFileSync", "createWriteStream", "mkdir", "mkdirSync",
  "mkdtemp", "mkdtempSync", "rm", "rmSync", "rmdir", "rmdirSync", "unlink", "unlinkSync", "rename", "renameSync", "copyFile",
  "copyFileSync", "cp", "cpSync", "symlink", "symlinkSync", "link", "linkSync", "chmod", "chmodSync", "truncate", "truncateSync",
  "utimes", "utimesSync"];
const FS_NAMES = new Set([...FS_FNS, ...FS_WRITES]);
// the names the scan knows node:fs and node:path by, so sees their functions called on
const NS_NAMES = new Set(["fs", "fsp", "path", "posix", "win32", "promises"]);
const FS_PATH_MODULE = /^(?:node:)?(?:fs|fs\/promises|path|path\/posix|path\/win32)$/;
const LOOPBACK = new Set(["127.0.0.1", "::1"]);
const IDENT_RE = new RegExp(String.raw`^${IDENT}$`);
const NOT_CALLS = /^(?:if|for|while|switch|catch|function|return|typeof|void|await|yield|in|of|else|do|case|delete|throw)$/;
// A read: the read family called bare or as a method of anything
// (fs.readFile, require("node:fs").readFileSync, io.stat). A path function:
// bare or on path/posix/win32, never a method of something else (parts.join
// and Promise.resolve are not paths).
const readRe = (names = FS_FNS) => new RegExp(String.raw`(?<![\w$])(${names.join("|")})\s*\(`, "g");
const pathRe = () => new RegExp(String.raw`(?<![\w$.])(?:(?:path|posix|win32)\s*\.\s*)*(${PATH_FNS.join("|")})\s*\(`, "g");
const mentions = (text, name) => new RegExp(String.raw`(?<![\w$.])${esc(name)}(?![\w$])`).test(text);
const SERVER_FILE = new RegExp(String.raw`(?<![\w$])(?:createServer|createSecureServer)\s*\(|(?<![\w$.])new\s+(?:${IDENT}\s*\.\s*)*Server\s*\(|` +
  String.raw`(?<![\w$])listen(?![\w$])|\.\s*route\s*\(|(?<![\w$])fulfill\s*\(`);
const isServerFile = (src) => SERVER_FILE.test(codeOnly(src)) || /(["'`])listen\1/.test(src);

// The names a binding pattern declares: x, { a, b: c, ...d }, [e, f = 1].
const bindings = (pattern) => (pattern.replace(/\.\.\./g, " ").replace(/=[^,}\]]*/g, "")
  .match(new RegExp(String.raw`(?<![\w$.])${IDENT}(?!\s*:)`, "g")) || []).filter((n) => !["const", "let", "var"].includes(n));
// The { from: to } renames in a destructuring pattern.
const renames = (pattern) => [...pattern.matchAll(new RegExp(String.raw`(?<![\w$.])(${IDENT})\s*:\s*(${IDENT})`, "g"))]
  .map((m) => ({ from: m[1], to: m[2] }));
// A function's or a module's name used as a value: a.b.c, as segments, or null.
const reference = (text) => {
  const segs = text.trim().split(/\s*\.\s*/);
  return segs.every((s) => IDENT_RE.test(s)) ? segs : null;
};
const fnRef = (segs) => {
  const last = segs[segs.length - 1];
  return FS_NAMES.has(last) || ["join", "normalize", "relative"].includes(last) ||
    (last === "resolve" && segs.length > 1 && ["path", "posix", "win32"].includes(segs[segs.length - 2]));
};
const nsRef = (segs) => NS_NAMES.has(segs[segs.length - 1]);

// The expression assigned at index from: up to the next comma or semicolon of
// the bracket around it, or that bracket's close.
function exprEnd(src, from) {
  const { pairs, commas, semis } = readJs(src);
  let o = -1;
  for (const [a, c] of pairs) if (a < from && from < c && a > o) o = a;
  const ends = [...(commas.get(o) || []), ...(semis.get(o) || [])].filter((x) => x >= from);
  return Math.min(o >= 0 ? pairs.get(o) : src.length, ...ends);
}

function scanContext(src) {
  const ctx = { src, code: codeOnly(src), fns: functionsIn(src), problems: [], escaped: new Set(), visited: new Set(),
    frozen: new Map(), serverAt: Infinity };
  ctx.within = (fn, at) => fn.file || (at >= fn.from && at < fn.to);
  ctx.topLevel = (at) => !ctx.fns.some((f) => ctx.within(f, at));
  ctx.line = (at) => `line ${lineOf(src, at)}`;
  ctx.codeOf = (span) => ctx.code.slice(span.at, span.at + span.text.length);
  ctx.imports = importsIn(ctx);
  ctx.decls = declarationsIn(ctx);
  ctx.isLocal = (name, fn) => fn.file || ctx.decls.some((d) => d.name === name && ctx.within(fn, d.at));
  ctx.declared = (name) => ctx.decls.some((d) => d.name === name);
  return ctx;
}

// Every static import: { at, mod, bindings: [{ imported, local } | { text }] };
// imported is "*", "default" or the export's name.
function importsIn(ctx) {
  const { src, code } = ctx;
  const out = [];
  for (const m of code.matchAll(/(?<![\w$.])import\s+([^;"'`()]*?)\s*from\s*(["'])/g)) {
    const q = m.index + m[0].length - 1;
    const mod = src.slice(q + 1, src.indexOf(src[q], q + 1));
    const clause = m[1].trim();
    const named = /\{([^}]*)\}/.exec(clause);
    const list = [];
    for (const b of clause.replace(/\{[^}]*\}/, "").split(",").map((s) => s.trim()).filter(Boolean)) {
      const n = new RegExp(String.raw`^(\*\s*as\s+)?(${IDENT})$`).exec(b);
      list.push(n ? { imported: n[1] ? "*" : "default", local: n[2] } : { text: b });
    }
    if (named) for (const s of named[1].split(",").map((x) => x.trim()).filter(Boolean)) {
      const n = new RegExp(String.raw`^(${IDENT})(?:\s+as\s+(${IDENT}))?$`).exec(s);
      list.push(n ? { imported: n[1], local: n[2] || n[1] } : { text: s });
    }
    out.push({ at: m.index, mod, bindings: list });
  }
  return out;
}

// Every declared name: { name, at, kind, value }. at is where it is declared
// (a parameter at its function's parameter list), value the initializer.
function declarationsIn(ctx) {
  const { src, code, fns } = ctx;
  const out = [];
  for (const m of code.matchAll(new RegExp(String.raw`(?<![\w$.])(const|let|var)\s+(${IDENT}|\{[^}]*\}|\[[^\]]*\])`, "g"))) {
    const end = m.index + m[0].length;
    const eq = /^\s*=(?![=>])/.exec(code.slice(end));
    const value = eq ? trivia(code.slice(end + eq[0].length, exprEnd(src, end + eq[0].length))) : null;
    for (const name of bindings(m[2])) out.push({ name, at: m.index, kind: m[1], value });
  }
  for (const f of fns) {
    const ps = src[f.from] === "(" ? argsAt(src, f.from) || [] : [f.params[0] || ""];
    for (const p of ps) for (const name of bindings(p)) out.push({ name, at: f.from, kind: "param", value: null });
  }
  for (const m of code.matchAll(new RegExp(String.raw`(?<![\w$.])(?:function\s*\*?|class)\s+(${IDENT})`, "g"))) {
    out.push({ name: m[1], at: m.index, kind: "function", value: null });
  }
  for (const m of code.matchAll(new RegExp(String.raw`(?<![\w$.])catch\s*\(\s*(${IDENT})`, "g"))) {
    out.push({ name: m[1], at: m.index + m[0].length - m[1].length, kind: "param", value: null });
  }
  for (const imp of ctx.imports) for (const b of imp.bindings) if (b.local) out.push({ name: b.local, at: imp.at, kind: "import", value: null });
  return out;
}

// Every write in [from, to): { names, value, at, kind, pattern } for a declaration,
// `x = v` (and +=, ||=, ??=, &&=), `x.p = v` / `x[k] = v` (names: [x]),
// `[a, b] = v` / `({ a } = v)`, and a loop's `for (const x of v)`.
function writesIn(ctx, from, to) {
  const { src, code } = ctx;
  const out = [];
  const body = code.slice(from, to);
  const ASSIGN = String.raw`(?:\+=|\|\|=|\?\?=|&&=|=(?![=>]))`;
  const forms = [
    ["decl", new RegExp(String.raw`(?<![\w$.])(?:const|let|var)\s+(${IDENT}|\{[^}]*\}|\[[^\]]*\])\s*=(?![=>])`, "g")],
    ["member", new RegExp(String.raw`(?<![\w$.])(${IDENT})(?:\s*\.\s*${IDENT}|\s*\[[^\]]*\])+\s*${ASSIGN}`, "g")],
    ["pattern", new RegExp(String.raw`(?<![\w$.\])])(\{[^}]*\}|\[[^\]]*\])\s*=(?![=>])`, "g")],
    ["plain", new RegExp(String.raw`(?<![\w$.])(${IDENT})\s*${ASSIGN}`, "g")],
    ["loop", new RegExp(String.raw`(?<![\w$.])for\s*(?:await\s*)?\(\s*(?:(?:const|let|var)\s+)?(${IDENT}|\{[^}]*\}|\[[^\]]*\])\s+(?:of|in)(?![\w$])`, "g")],
  ];
  const seen = new Set();
  for (const [kind, re] of forms) {
    for (const m of body.matchAll(re)) {
      const at = from + m.index + m[0].length;
      if (seen.has(at)) continue;
      seen.add(at);
      if (kind !== "decl" && kind !== "loop" && /(?:const|let|var)\s+$/.test(body.slice(0, m.index))) continue;
      const names = kind === "member" ? [m[1]] : bindings(m[1]);
      if (!names.length) continue;
      let end = exprEnd(src, at);
      if (kind === "loop") end = readJs(src).pairs.get(from + m.index + m[0].indexOf("(")) ?? end;
      out.push({ names, value: code.slice(at, end), at, kind, pattern: m[1] });
    }
  }
  return out;
}

// Every call in [from, to): { open, args, method, recv, root } where method
// is the name after a dot (or null), recv the code it is called on, and root
// that code's leading name when it is a plain member chain (a, a.b, a[k]).
function callsIn(ctx, from, to) {
  const { src, code, fns } = ctx;
  const { pairs } = readJs(src);
  const closes = ctx.closes || (ctx.closes = new Map([...pairs].map(([o, c]) => [c, o])));
  const params = ctx.paramOpens || (ctx.paramOpens = new Set(fns.map((f) => f.from)));
  const out = [];
  for (const [open, close] of pairs) {
    if (open < from || open >= to || src[open] !== "(" || params.has(open)) continue;
    const w = /([\w$]+|[)\]])\s*$/.exec(code.slice(Math.max(0, open - 64), open));
    if (!w || NOT_CALLS.test(w[1])) continue;
    const m = new RegExp(String.raw`\.\s*(${IDENT})\s*$`).exec(code.slice(Math.max(0, open - 96), open));
    let method = null, recv = "", root = null;
    if (m) {
      method = m[1];
      // walk back over the receiver: names, dots and bracketed groups
      let i = open - m[0].length;
      for (;;) {
        let j = i - 1;
        while (j >= 0 && /\s/.test(code[j])) j--;
        if (j >= 0 && (code[j] === ")" || code[j] === "]") && closes.has(j)) { i = closes.get(j); continue; }
        if (j >= 0 && /[\w$]/.test(code[j])) {
          let k = j;
          while (k > 0 && /[\w$]/.test(code[k - 1])) k--;
          if (NOT_CALLS.test(code.slice(k, j + 1)) || code.slice(k, j + 1) === "new") break;
          i = k;
          let p = k - 1;
          while (p >= 0 && /\s/.test(code[p])) p--;
          if (p >= 0 && code[p] === ".") { i = p; continue; }
        }
        break;
      }
      recv = code.slice(i, open - m[0].length);
      const r = new RegExp(String.raw`^(${IDENT})(?:\s*\.\s*${IDENT}|\s*\[[^\]]*\])*\s*$`).exec(recv.trim());
      root = r ? r[1] : null;
    }
    out.push({ open, close, args: argSpansAt(src, open) || [], method, recv, root });
  }
  return out;
}

// The receiver of the index's answer, lookup(R, …) or R.get(…), when that
// call is the whole value; else null.
function answerReceiver(ctx, value, at) {
  const v = trivia(value).replace(/\s*(?:\?\?|\|\|)\s*null$/, "");
  const m = new RegExp(String.raw`^(?:lookup\s*\(\s*(${IDENT})\s*,|(${IDENT})\s*\.\s*get\s*\()`).exec(v);
  if (!m) return null;
  const start = at + value.indexOf(v);
  const close = readJs(ctx.src).pairs.get(start + v.indexOf("("));
  return close !== undefined && close === start + v.length - 1 ? m[1] || m[2] : null;
}

// Whether R is an index the file built before its server and nothing changes
// while it serves (rule 2's conditions on R).
function frozenIndex(ctx, r) {
  if (ctx.frozen.has(r)) return ctx.frozen.get(r);
  const { code, fns } = ctx;
  const own = ctx.decls.filter((d) => d.name === r);
  let ok = own.length === 1 && ctx.topLevel(own[0].at) && own[0].at < ctx.serverAt &&
    /^(?:indexTree|new\s+Map)\s*\(/.test(own[0].value || "");
  const beforeServer = (f) => {   // f is called only by top-level code before the server
    for (const m of code.matchAll(new RegExp(String.raw`(?<![\w$.])${esc(f.name)}(?![\w$])`, "g"))) {
      if (/(?:const|let|var|function\s*\*?)\s+$/.test(code.slice(0, m.index))) continue;   // its definition
      if (!/^\s*\(/.test(code.slice(m.index + f.name.length)) || !ctx.topLevel(m.index) || m.index >= ctx.serverAt) return false;
    }
    return true;
  };
  if (ok) {
    for (const m of code.matchAll(new RegExp(String.raw`(?<![\w$.])${esc(r)}(?![\w$])`, "g"))) {
      const before = code.slice(0, m.index), after = code.slice(m.index + r.length);
      if (/(?:const|let|var)\s+$/.test(before)) continue;                                           // its declaration
      if (/^\s*\.\s*(?:(?:get|has|keys|values|entries)\s*\(|size(?![\w$]))/.test(after)) continue;   // a read
      if (/(?<![\w$.])lookup\s*\(\s*$/.test(before) && /^\s*,/.test(after)) continue;                // lookup(R, …)
      // anything else may change it: only before the server, at top level or in a function only that code calls
      const outer = fns.filter((f) => ctx.within(f, m.index)).sort((a, b) => a.from - b.from)[0];
      if (outer ? !outer.name || !beforeServer(outer) : m.index >= ctx.serverAt) { ok = false; break; }
    }
  }
  ctx.frozen.set(r, ok);
  return ok;
}

const isIndexAnswer = (ctx, value, at) => {
  const r = answerReceiver(ctx, value, at);
  return r !== null && frozenIndex(ctx, r);
};

// Rule 0: names for node:fs and node:path the scan sees called.
function bindingProblems(ctx) {
  const { src, code } = ctx;
  const out = [];
  const known = (n) => NS_NAMES.has(n);
  for (const imp of ctx.imports) {
    if (!FS_PATH_MODULE.test(imp.mod)) continue;
    for (const b of imp.bindings) {
      if (!b.local) { out.push(`${ctx.line(imp.at)}: an import from "${imp.mod}" the scan cannot read (${b.text})`); continue; }
      const whole = b.imported === "*" || b.imported === "default";
      if (whole ? !known(b.local) : b.imported !== b.local && !(known(b.imported) && known(b.local))) {
        out.push(`${ctx.line(imp.at)}: imports ${whole ? (b.imported === "*" ? "* as " : "") : `${b.imported} as `}${b.local} ` +
          `from "${imp.mod}", a name for fs or path the scan does not see called`);
      }
    }
  }
  for (const m of code.matchAll(/(?<![\w$.])(import|require)\s*\(/g)) {
    const open = m.index + m[0].length - 1;
    const args = argSpansAt(src, open) || [];
    const lit = args.length === 1 && /^(["'])([^"'`]*)\1$/.exec(args[0].text);
    if (!lit) { out.push(`${ctx.line(m.index)}: ${m[1]}(${args.map((a) => a.text).join(", ").slice(0, 30)}) names a module the scan cannot read`); continue; }
    if (!FS_PATH_MODULE.test(lit[2])) continue;
    const decl = new RegExp(String.raw`(?<![\w$.])(?:const|let|var)\s+(${IDENT}|\{[^}]*\})\s*=\s*(?:await\s+)?$`).exec(code.slice(0, m.index));
    const close = readJs(src).pairs.get(open);
    const whole = decl && trivia(code.slice(close + 1, exprEnd(src, close + 1))) === "";
    // or one function called on it at once, which the read and write rules see
    // by name wherever it is called: (await import("node:fs/promises")).mkdir(…)
    const once = new RegExp(String.raw`^(\s*\))?\s*\.\s*(${IDENT})\s*\(`).exec(code.slice(close + 1));
    const direct = once && (!once[1] || /\(\s*(?:await\s+)?$/.test(code.slice(0, m.index))) &&
      !PATH_FNS.includes(once[2]) && !known(once[2]);
    if (direct) continue;
    if (!whole || (IDENT_RE.test(decl[1]) && !known(decl[1]))) {
      out.push(`${ctx.line(m.index)}: ${m[1]}("${lit[2]}") is not a whole declaration under a name the scan knows`);
    }
  }
  for (const m of code.matchAll(new RegExp(String.raw`(?<![\w$.])(?:const|let|var)\s+(\{[^}]*\})\s*=(?![=>])|(?<![\w$.\])])(\{[^}]*\})\s*=(?![=>])`, "g"))) {
    for (const r of renames(m[1] || m[2])) {
      if ((FS_NAMES.has(r.from) || PATH_FNS.includes(r.from) || known(r.from)) && !(known(r.from) && known(r.to))) {
        out.push(`${ctx.line(m.index)}: destructures ${r.from} as ${r.to}, a name the scan does not see called`);
      }
    }
  }
  for (const w of writesIn(ctx, 0, src.length)) {
    const segs = reference(trivia(w.value));
    if (segs && IDENT_RE.test(w.pattern) && w.kind !== "member" && w.kind !== "loop" && (fnRef(segs) || nsRef(segs)) &&
      w.pattern !== segs[segs.length - 1] && !(nsRef(segs) && known(w.pattern))) {
      out.push(`${ctx.line(w.at)}: ${w.pattern} = ${segs.join(".")} aliases a read or path function, or fs or path, which the scan would not see called`);
    }
  }
  for (const c of callsIn(ctx, 0, src.length)) {
    for (const a of c.args) {
      const segs = reference(ctx.codeOf(a));
      if (segs && fnRef(segs)) out.push(`${ctx.line(a.at)}: hands ${segs.join(".")} itself to a call, which the scan would not see called`);
    }
  }
  const fnAlt = `(?:${[...FS_NAMES, "join", "normalize", "relative"].join("|")}|(?:path|posix|win32)\\s*\\.\\s*resolve)`;
  for (const m of code.matchAll(new RegExp(String.raw`(?<![\w$])${fnAlt}\s*\.\s*(?:call|apply|bind)(?![\w$])`, "g"))) {
    out.push(`${ctx.line(m.index)}: ${m[0].replace(/\s+/g, "")} calls a read or path function where the scan does not see it called`);
  }
  for (const m of code.matchAll(new RegExp(String.raw`(?<![\w$.])(?:eval|Function)\s*\(|(?<![\w$.])createRequire(?![\w$])|` +
    String.raw`(?<![\w$.])(?:${[...NS_NAMES].join("|")})(?:\s*\.\s*${IDENT})*\s*\[`, "g"))) {
    out.push(`${ctx.line(m.index)}: ${m[0].replace(/\s+/g, "")} is a spelling the scan cannot follow`);
  }
  return out;
}

// Rule 1: every handler, as { fn }; problems for the ones it cannot follow.
function handlersIn(ctx) {
  const { src, code, fns } = ctx;
  const found = [];
  let count = 0;
  const re = new RegExp(String.raw`(?<![\w$])(?:createServer|createSecureServer)\s*\(|(?<![\w$.])new\s+(?:${IDENT}\s*\.\s*)*Server\s*\(|\.\s*route\s*\(`, "g");
  for (const m of code.matchAll(re)) {
    count++;
    ctx.serverAt = Math.min(ctx.serverAt, m.index);
    const route = /route\s*\($/.test(m[0]);
    const open = m.index + m[0].length - 1;
    const args = argSpansAt(src, open);
    const where = ctx.line(m.index);
    const h = args && (route ? args[1] : args[args.length - 1]);   // route(url, handler[, options]); createServer([options,] handler)
    if (!h) { ctx.problems.push(`${where}: a ${route ? "route" : "server"} whose handler the scan cannot read`); continue; }
    let fn = fns.filter((f) => f.from >= h.at && f.from <= h.at + 16 && f.to <= h.at + h.text.length + 1)
      .sort((a, b) => a.from - b.from)[0];
    if (!fn && IDENT_RE.test(h.text)) fn = fns.find((f) => f.name === h.text);
    if (!fn) { ctx.problems.push(`${where}: a handler (${h.text.slice(0, 30)}) the scan cannot find`); continue; }
    if (!fn.params[0]) { ctx.problems.push(`${where}: a handler whose request is not a plain name, which the scan cannot follow`); continue; }
    found.push({ fn });
  }
  for (const m of code.matchAll(/\.\s*(?:on|once|addListener|prependListener|prependOnceListener)\s*\(\s*(["'`])/g)) {
    const q = m.index + m[0].length - 1;
    if (src.slice(q + 1, q + 8) === "request" && src[q + 8] === src[q]) {
      ctx.problems.push(`${ctx.line(m.index)}: a "request" listener the scan does not follow (hand the handler to createServer)`);
    }
  }
  return { found, count };
}

// Rule 3, within one function given the names in it that carry the request.
function taintProblems(ctx, fn, carriers) {
  const key = `${fn.file ? "file" : fn.from}:${[...carriers].sort().join(",")}`;
  if (ctx.visited.has(key)) return;
  ctx.visited.add(key);
  const { src, code } = ctx;
  const tainted = new Set(carriers);
  const writes = writesIn(ctx, fn.from, fn.to);
  const calls = callsIn(ctx, fn.from, fn.to);
  const carried = (text) => [...tainted].find((t) => mentions(text, t));
  const hold = (name) => {
    if (!ctx.isLocal(name, fn)) ctx.escaped.add(name);   // carried out of the function
    if (tainted.has(name)) return false;
    tainted.add(name);
    return true;
  };
  for (let grew = true; grew;) {
    grew = false;
    for (const w of writes) {
      if (isIndexAnswer(ctx, w.value, w.at) || !carried(w.value)) continue;
      for (const n of w.names) if (hold(n)) grew = true;
    }
    for (const c of calls) {
      const handed = c.args.some((a) => carried(ctx.codeOf(a)));
      if (handed && c.root && !["get", "has"].includes(c.method) && !FS_NAMES.has(c.method) && !PATH_FNS.includes(c.method) &&
        (ctx.isLocal(c.root, fn) || ctx.declared(c.root))) {
        if (hold(c.root)) grew = true;
      }
      if (handed || (c.method && carried(c.recv))) {
        for (const a of c.args) {
          const inline = ctx.fns.find((f) => f.from >= a.at && f.from <= a.at + 8 && f.to <= a.at + a.text.length + 1);
          for (const p of inline ? ctx.decls.filter((d) => d.kind === "param" && d.at === inline.from) : []) {
            if (hold(p.name)) grew = true;
          }
        }
      }
    }
  }
  const body = code.slice(fn.from, fn.to);
  for (const re of [readRe([...FS_NAMES]), pathRe()]) {
    for (const m of body.matchAll(re)) {
      const open = fn.from + m.index + m[0].length - 1;
      const args = argSpansAt(src, open);
      const t = args && args.map((a) => carried(ctx.codeOf(a))).find(Boolean);
      if (t) ctx.problems.push(`line ${lineOf(src, open)}: ${m[1]}(…) is handed ${t}, which comes from the request`);
    }
  }
  // rule 5's other thrower: new URL(…) of a request target such as "//" or
  // "//[" throws ERR_INVALID_URL, so one handed the request sits in a try too
  for (const m of body.matchAll(/(?<![\w$.])new\s+URL\s*\(/g)) {
    const open = fn.from + m.index + m[0].length - 1;
    const t = (argSpansAt(src, open) || []).map((a) => carried(ctx.codeOf(a))).find(Boolean);
    if (t && !guardedAt(ctx, fn.from + m.index)) {
      ctx.problems.push(`line ${lineOf(src, open)}: new URL(…) is handed ${t}, from the request, outside a try that catches it ` +
        `(a request target like // throws and ends the probe)`);
    }
  }
  // follow the file's own functions a call hands one of them (by name, or
  // the name itself handed to a call that one of them reaches)
  const defs = ctx.fns.filter((f) => f.name && f.from !== fn.from);
  for (const def of defs) {
    for (const c of body.matchAll(new RegExp(String.raw`(?<![\w$.])${esc(def.name)}(?![\w$])`, "g"))) {
      const at = fn.from + c.index;
      const after = /^\s*\(/.exec(code.slice(at + def.name.length));
      if (after) {
        const args = argSpansAt(src, at + def.name.length + after[0].length - 1) || [];
        const handed = new Set();
        args.forEach((a, k) => { if (carried(ctx.codeOf(a)) && def.params[k]) handed.add(def.params[k]); });
        if (handed.size) taintProblems(ctx, def, handed);
      } else {
        const call = calls.find((x) => x.args.some((a) => a.at === at && a.text === def.name));
        if (call && (call.args.some((a) => carried(ctx.codeOf(a))) || (call.method && carried(call.recv)))) {
          taintProblems(ctx, def, new Set(def.params.filter(Boolean)));
        }
      }
    }
  }
}

// Rule 2, within one handler.
function readProblems(ctx, fn) {
  const { src, code } = ctx;
  const writes = writesIn(ctx, fn.from, fn.to);
  for (const r of code.slice(fn.from, fn.to).matchAll(readRe())) {
    const open = fn.from + r.index + r[0].length - 1;
    const first = (argSpansAt(src, open) || [])[0];
    const name = first && IDENT_RE.test(first.text) ? first.text : null;
    const given = name ? writes.filter((w) => w.names.includes(name)) : [];
    const ok = name && ctx.isLocal(name, fn) && given.length && given.every((w) => isIndexAnswer(ctx, w.value, w.at));
    if (!ok) ctx.problems.push(`line ${lineOf(src, open)}: ${r[1]}(${first ? first.text.slice(0, 30) : ""}) reads what the index did not answer`);
  }
}

// Rule 2's other half: a page route answered from a file.
function fulfillProblems(ctx) {
  for (const m of ctx.code.matchAll(/(?<![\w$])fulfill\s*\(/g)) {
    const first = (argSpansAt(ctx.src, m.index + m[0].length - 1) || [])[0];
    const obj = first ? ctx.codeOf(first) : "";
    if (first && (!/^\{/.test(obj) || /(?<![\w$.])path(?![\w$])\s*[:,}]/.test(obj))) {
      ctx.problems.push(`${ctx.line(m.index)}: fulfill(${first.text.slice(0, 30)}) answers a page's request from a file the scan does not follow`);
    }
  }
}

// Rule 4: every listen readable, on loopback.
function listenProblems(ctx) {
  const { src, code } = ctx;
  for (const l of code.matchAll(/(?<![\w$])listen(?![\w$])/g)) {
    const call = /^\s*\(/.exec(code.slice(l.index + 6));
    if (!/\.\s*$/.test(code.slice(0, l.index)) || !call) {
      ctx.problems.push(`${ctx.line(l.index)}: a listen the scan cannot read (${code.slice(l.index, l.index + 24).trim()})`);
      continue;
    }
    const open = l.index + 6 + call[0].length - 1;
    const args = argSpansAt(src, open) || [];
    const host = (() => {
      const o = args[0] && /^\{/.test(args[0].text) ? /(?<![\w$])host\s*:\s*(["'])([^"']*)\1/.exec(args[0].text) : null;
      if (o) return o[2];
      const s = args[1] && /^(["'])([^"']*)\1$/.exec(args[1].text);
      return s ? s[2] : null;
    })();
    if (!LOOPBACK.has(host)) {
      ctx.problems.push(`${ctx.line(open)}: listen(${args.map((a) => a.text).join(", ").slice(0, 40)}) binds ` +
        `${host === null ? "every interface (no loopback host the scan can read)" : host}, not loopback`);
    }
  }
  for (const s of src.matchAll(/(["'`])listen\1/g)) {
    if (code[s.index] === s[1]) ctx.problems.push(`${ctx.line(s.index)}: "listen" spelled as a string, a listen the scan cannot read`);
  }
}

// Rule 5's guard: every try block whose catch can swallow what its block
// throws ({ o, c } the block's braces). A try with no catch, or whose catch
// says `throw` anywhere, guards nothing: the error still leaves the handler.
function guardingTries(ctx) {
  if (ctx.tries) return ctx.tries;
  const { src, code } = ctx;
  const { pairs } = readJs(src);
  ctx.tries = [];
  for (const m of code.matchAll(/(?<![\w$.])try\s*\{/g)) {
    const o = m.index + m[0].length - 1;
    const c = pairs.get(o);
    if (c === undefined) continue;
    const k = /^\s*catch\s*(?:\(\s*[^()]*\)\s*)?\{/.exec(code.slice(c + 1));
    if (!k) continue;
    const co = c + 1 + k[0].length - 1;
    const cc = pairs.get(co);
    if (cc === undefined || /(?<![\w$.])throw(?![\w$])/.test(code.slice(co, cc))) continue;
    ctx.tries.push({ o, c });
  }
  return ctx.tries;
}

// Whether the code at index at sits in a guarding try's own block, in the
// same function as the try: no function, method or arrow starts between the
// try's brace and it (a callback inside a try runs whenever its caller calls
// it, maybe after the try is gone), so a throw there is caught right there.
function guardedAt(ctx, at) {
  const { src, code } = ctx;
  const { pairs } = readJs(src);
  const closes = ctx.closes || (ctx.closes = new Map([...pairs].map(([o, c]) => [c, o])));
  const CONTROL = /^(?:if|for|while|switch|catch|with)$/;
  return guardingTries(ctx).some(({ o, c }) => {
    if (!(o < at && at < c)) return false;
    if (ctx.fns.some((f) => f.from > o && f.from < at && at < f.to)) return false;
    for (const [o2, c2] of pairs) {
      if (src[o2] !== "{" || !(o < o2 && o2 < at && at < c2)) continue;
      const before = code.slice(0, o2).trimEnd();
      if (!before.endsWith(")")) continue;                           // a plain block, an object literal
      const po = closes.get(before.length - 1);
      const word = po === undefined ? "" : (/([\w$]+)\s*$/.exec(code.slice(0, po)) || [])[1] || "";
      if (!CONTROL.test(word)) return false;                         // a method's or function's body
    }
    return true;
  });
}

// Rule 5: an undecodable request is answered, never thrown. Every
// decodeURIComponent or decodeURI in a server file is a call inside a try
// that catches it (lookup() is the one the probes use; it answers null);
// handed itself anywhere, the scan cannot see where it is called.
function decodeProblems(ctx) {
  const { code } = ctx;
  for (const m of code.matchAll(/(?<![\w$])(decodeURI(?:Component)?)(?![\w$])/g)) {
    if (!/^\s*\(/.test(code.slice(m.index + m[0].length))) {
      ctx.problems.push(`${ctx.line(m.index)}: ${m[1]} handed itself on, where the scan cannot see it called inside a try`);
    } else if (!guardedAt(ctx, m.index)) {
      ctx.problems.push(`${ctx.line(m.index)}: ${m[1]}(…) outside a try that catches it: a malformed escape (/%E0) ` +
        `throws URIError out of the handler and ends the probe (answer it, as lookup() does)`);
    }
  }
}

// What is wrong with the server file src: { servers, problems }, servers the
// count of servers and page routes it starts.
function requestPathProblems(src) {
  const ctx = scanContext(src);
  const { found, count } = handlersIn(ctx);
  ctx.problems.push(...bindingProblems(ctx));
  for (const h of found) taintProblems(ctx, h.fn, new Set([h.fn.params[0]]));
  // a name that carried the request out of a function carries it through the whole file
  for (let n = -1; n !== ctx.escaped.size;) {
    n = ctx.escaped.size;
    taintProblems(ctx, { from: 0, to: src.length, file: true }, new Set(ctx.escaped));
  }
  for (const h of found) readProblems(ctx, h.fn);
  fulfillProblems(ctx);
  listenProblems(ctx);
  decodeProblems(ctx);
  return { servers: count, problems: [...new Set(ctx.problems)] };
}

test("no probe resolves a request path itself: each serves its index's answer, on loopback, and answers a malformed request", () => {
  const bad = [];
  let servers = 0;
  const probes = [];
  const scanned = [];
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name);
      if (e.isDirectory()) { if (e.name !== "fixtures") walk(p); continue; }
      if (!/\.m?js$/.test(e.name) || /\.test\.m?js$/.test(e.name)) continue;
      const src = read(p);
      const rel = p.slice(__dirname.length + 1);
      scanned.push(rel);
      if (!isServerFile(src)) continue;
      probes.push(rel);
      const found = requestPathProblems(src);
      servers += found.servers;
      for (const why of found.problems) bad.push(`${rel}: ${why}`);
    }
  };
  walk(__dirname);
  assert.ok(servers >= 17, `only ${servers} probe servers found: the scan is looking in the wrong place`);
  assert.ok(scanned.includes(join("native", "probe_cores.js")), "the scan reads the directories below this one too");
  // every allowlist probe is in the scan, held to every rule: none is excused
  // from loopback any more (sweep A51 moved the boot and onboard probes)
  for (const f of ["bench_probe.mjs", "boardroom_probe.mjs", "workshop_probe.mjs", "boot_probe.mjs", "onboard_probe.mjs"]) {
    assert.ok(probes.includes(f), `${f} serves the Lab's pages but the scan did not find its server`);
  }
  assert.deepStrictEqual(bad, [], "a probe's server turns a request into a path, reads what its index did not answer, " +
    "listens beyond loopback, or throws on an undecodable request");
});

// The scan itself, on the shapes it must refuse and the ones the probes use.
const OK = `\nawait new Promise((done) => server.listen(0, "127.0.0.1", done));`;
const IDX = `const FILES = indexTree(ROOT);\n`;
const REFUSED = {
  // the historical shape, and plain ones
  "vision_probe.mjs before A46 (resolve, then check)":
    `const server = createServer(async (req, res) => {\n  try {\n    if (cores && await cores.handle(req, res)) return;\n` +
    `    const rel = decodeURIComponent(new URL(req.url, "http://x").pathname);\n` +
    `    if (rel === "/favicon.ico") { res.writeHead(204); return res.end(); }\n    const p = resolve(join(ROOT, rel));\n` +
    `    if (p !== ROOT && !p.startsWith(ROOT + sep)) { res.writeHead(403); return res.end(); }\n` +
    `    const file = rel.endsWith("/") ? join(p, "index.html") : p;\n    const body = await readFile(file);\n    res.end(body);\n` +
    `  } catch { res.writeHead(404); res.end("not found"); }\n});` + OK,
  "a join straight from the URL": `const server = createServer(async (req, res) => res.end(await readFile(join(ROOT, req.url))));` + OK,
  "a resolved path looked up in a map (still a path from the request)": IDX +
    `const server = createServer(async (req, res) => {\n  const p = resolve(ROOT, "." + req.url);\n  const file = FILES.get(p);\n` +
    `  res.end(await readFile(file));\n});` + OK,
  "a decoded name, resolved, then looked up": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const rel = decodeURIComponent(req.url);\n  const p = resolve(ROOT, "." + rel);\n  const file = FILES.get(p);\n` +
    `  res.end(await readFile(file));\n});` + OK,
  "a loop that feeds the URL back to an earlier line": IDX + `const server = createServer(async (req, res) => {\n` +
    `  let next = "/index.html";\n  for (let i = 0; i < 2; i++) {\n    const rel = next.slice(1);\n` +
    `    const file = FILES.get(resolve(ROOT, rel));\n    if (file) return res.end(await readFile(file));\n` +
    `    next = req.url;\n  }\n});` + OK,
  "a helper that joins what the handler hands it":
    `const serve = async (res, rel) => res.end(await readFile(join(ROOT, rel)));\n` +
    `const server = createServer((req, res) => serve(res, req.url));` + OK,
  "a helper handed the whole request": `function serve(rq, res) {\n  return stat(join(ROOT, rq.url));\n}\n` +
    `const server = createServer((req, res) => serve(req, res));` + OK,
  "a request path smuggled through an array (the read rule's catch)":
    `const server = createServer(async (req, res) => {\n  const parts = [];\n  parts.push(ROOT, req.url);\n` +
    `  const file = parts.join("");\n  res.end(await readFile(file));\n});` + OK,
  "a handler defined by name": `async function handler(req, res) {\n  res.end(await readFile(ROOT + req.url));\n}\n` +
    `const server = createServer(handler);` + OK,
  "a handler the scan cannot find": `const server = createServer(handlers.main);` + OK,
  "a destructured request": IDX + `const server = createServer(async ({ url }, res) => res.end(await readFile(lookup(FILES, url))));` + OK,
  "a lookup with a fallback": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const file = lookup(FILES, req.url) ?? join(ROOT, req.url);\n  res.end(await readFile(file));\n});` + OK,
  "a lookup that falls back to the request's own path": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const rel = req.url.slice(1);\n  const file = lookup(FILES, req.url) || rel;\n  res.end(await readFile(file));\n});` + OK,
  "a lookup, then rewritten": IDX + `const server = createServer(async (req, res) => {\n  let file = lookup(FILES, req.url);\n` +
    `  if (!file) file = ROOT + req.url;\n  res.end(await readFile(file));\n});` + OK,
  "a request aliased, then joined": `const server = createServer(async (req, res) => {\n  const r = req;\n  const { url } = r;\n` +
    `  res.end(await fs.promises.readFile(path.join(ROOT, url)));\n});` + OK,
  "a template literal path": "const server = createServer(async (req, res) => res.end(await readFile(`${ROOT}${req.url}`)));" + OK,
  "a read stream of the URL": `const server = createServer((req, res) => {\n  const u = decodeURIComponent(req.url);\n` +
    `  fs.createReadStream(normalize(ROOT + u)).pipe(res);\n});` + OK,
  "a read of an outer name the handler only sometimes sets": IDX + `let file = "/etc/passwd";\n` +
    `const server = createServer(async (req, res) => {\n  if (req.url !== "/") file = lookup(FILES, req.url);\n  res.end(await readFile(file));\n});` + OK,
  // rule 2's index: a map built before the server, which nothing changes while it serves
  "a value from the query string (URLSearchParams.get)": `const server = createServer(async (req, res) => {\n` +
    `  const params = new URL(req.url, "http://x").searchParams;\n  const file = params.get("f");\n  res.end(await readFile(file));\n});` + OK,
  "a value from a header (Headers.get)": `const server = createServer(async (req, res) => {\n  const h = new Headers(req.headers);\n` +
    `  const file = h.get("x-file");\n  res.end(await readFile(file));\n});` + OK,
  "a map the handler builds from the request": `const server = createServer(async (req, res) => {\n` +
    `  const M = new Map([[req.url, ROOT + req.url]]);\n  const file = M.get(req.url);\n  res.end(await readFile(file));\n});` + OK,
  "a lookup over an index the handler builds": `const server = createServer(async (req, res) => {\n` +
    `  const idx = new Map([["/x", ROOT + req.url]]);\n  const file = lookup(idx, "/x");\n  res.end(await readFile(file));\n});` + OK,
  "a map built inline (no name to check)": `const server = createServer(async (req, res) => {\n` +
    `  const file = new Map([[req.url, ROOT + req.url]]).get(req.url);\n  res.end(await readFile(file));\n});` + OK,
  "the index shadowed in the handler": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const FILES = new Map([[req.url, ROOT + req.url]]);\n  const file = FILES.get(req.url);\n  res.end(await readFile(file));\n});` + OK,
  "an index the scan cannot see built": `const FILES = makeIndex(ROOT);\nconst server = createServer(async (req, res) => {\n` +
    `  const file = lookup(FILES, req.url);\n  res.end(await readFile(file));\n});` + OK,
  "the handler fills the index": IDX + `const server = createServer(async (req, res) => {\n  FILES.set(req.url, ROOT + req.url);\n` +
    `  const file = lookup(FILES, req.url);\n  res.end(await readFile(file));\n});` + OK,
  "the index's has() taken for its answer": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const file = FILES.has(req.url);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "an index built inside a function, from what it is handed": `const make = (extra) => {\n` +
    `  const M = new Map([[extra, join(ROOT, extra)]]);\n  return createServer(async (req, res) => {\n` +
    `    const file = M.get(req.url);\n    if (file) res.end(await readFile(file));\n  });\n};\nconst server = make(process.argv[2]);` + OK,
  "an index declared after the server starts, from what the page asked for":
    `const server = createServer(async (req, res) => {\n  const file = SERVABLE.get(req.url);\n  if (file) res.end(await readFile(file));\n});` +
    OK + `\nlet seen = "/";\npage.on("requestfailed", (r) => { seen = new URL(r.url()).pathname; });\n` +
    `const SERVABLE = new Map([[seen, join(ROOT, seen)]]);`,
  "an allowlist filled from a function that runs later": `const SERVABLE = new Map();\nasync function allow(dirRel) {\n` +
    `  for (const f of await readdir(join(ROOT, dirRel))) SERVABLE.set(\`/\${dirRel}/\${f}\`, join(ROOT, dirRel, f));\n}\n` +
    `const later = () => allow("docs");\nconst server = createServer(async (req, res) => {\n` +
    `  const file = SERVABLE.get(req.url);\n  if (file) res.end(await readFile(file));\n});` + OK + `\nsetTimeout(later, 100);`,
  "an allowlist filled after the server starts": `const SERVABLE = new Map();\nasync function allow(dirRel) {\n` +
    `  for (const f of await readdir(join(ROOT, dirRel))) SERVABLE.set(\`/\${dirRel}/\${f}\`, join(ROOT, dirRel, f));\n}\n` +
    `const server = createServer(async (req, res) => {\n  const file = SERVABLE.get(req.url);\n  if (file) res.end(await readFile(file));\n});` +
    OK + `\nawait allow("docs");`,
  "a map changed while the server runs": `const SERVABLE = new Map();\nfunction refill() {\n  SERVABLE.clear();\n` +
    `  SERVABLE.set("/x", "/etc/passwd");\n}\nconst server = createServer(async (req, res) => {\n  refill();\n` +
    `  const file = SERVABLE.get(req.url);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "an allowlist filled when a request asks": `const SERVABLE = new Map();\nasync function allow(dirRel) {\n` +
    `  for (const f of await readdir(join(ROOT, dirRel))) SERVABLE.set(\`/\${dirRel}/\${f}\`, join(ROOT, dirRel, f));\n}\n` +
    `await allow("canary-local");\nconst server = createServer(async (req, res) => {\n  if (req.url === "/more") await allow("docs");\n` +
    `  const file = SERVABLE.get(req.url);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a map filled after the server starts, from what the page asked for": `const SERVABLE = new Map();\n` +
    `const server = createServer(async (req, res) => {\n  const file = SERVABLE.get(req.url);\n  if (file) res.end(await readFile(file));\n});` +
    OK + `\nlet seen = "/";\npage.on("requestfailed", (r) => { seen = new URL(r.url()).pathname; });\nSERVABLE.set(seen, join(ROOT, seen));`,
  // rule 3: values carried out of the handler, or through a store
  "a module-level name the handler writes and a helper reads":
    `let cur = "";\nconst send = (res) => res.end(readFileSync(ROOT + cur));\n` +
    `const server = createServer((req, res) => { cur = req.url; send(res); });` + OK,
  "a module-level map the handler fills and a helper reads": `const pending = new Map();\n` +
    `const send = (res, id) => res.end(readFileSync(pending.get(id)));\n` +
    `const server = createServer((req, res) => { pending.set(1, ROOT + req.url); send(res, 1); });` + OK,
  "an implicit global the top level reads back": `const server = createServer((req, res) => { last = req.url; res.end(); });` + OK +
    `\nawait page.goto(url);\nconsole.log(await readFile(join(ROOT, last), "utf8"));`,
  "a helper that stores the request in an array": `function serve(res, r) {\n  const a = [];\n  a.push(ROOT, r.url);\n` +
    `  return readFile(a.join("")).then((b) => res.end(b));\n}\nconst server = createServer((req, res) => serve(res, req));` + OK,
  "a helper that stores the request in an object": `function serve(res, r) {\n  const ctx = {};\n  ctx.path = ROOT + r.url;\n` +
    `  return readFile(ctx.path).then((b) => res.end(b));\n}\nconst server = createServer((req, res) => serve(res, req));` + OK,
  "a helper that destructures the request into a name": `function serve(res, r) {\n  let p;\n  ({ p } = { p: ROOT + r.url });\n` +
    `  return stat(p);\n}\nconst server = createServer((req, res) => serve(res, req));` + OK,
  "a helper that loops over the request's parts": `function serve(res, url) {\n  for (const part of url.split("/")) stat(join(ROOT, part));\n}\n` +
    `const server = createServer((req, res) => serve(res, req.url));` + OK,
  "a helper whose callback is handed the request's parts": `function serve(res, url) {\n` +
    `  url.split("/").forEach((part) => stat(join(ROOT, part)));\n}\nconst server = createServer((req, res) => serve(res, req.url));` + OK,
  "a request value relayed through a second module-level name": `let cur = "/", last = "/";\nfunction relay(v) {\n  last = v;\n}\n` +
    `const send = (res) => res.end(readFileSync(ROOT + last));\n` +
    `const server = createServer((req, res) => { cur = req.url; send(res); });` + OK + `\nsetInterval(() => relay(cur), 10);`,
  "a helper handed by name to a callback of the request's parts": `function probe(part) {\n  stat(join(ROOT, part));\n}\n` +
    `const server = createServer((req, res) => { req.url.split("/").forEach(probe); res.end(); });` + OK,
  "a path from the request, built and sent back": `const server = createServer((req, res) => res.end(String(path.join(ROOT, req.url))));` + OK,
  // rule 0: names for fs and path the scan would not see called
  "a renamed readFile import": `import { readFile as rf } from "node:fs/promises";\n` +
    `const server = createServer(async (req, res) => res.end(await rf(ROOT + req.url)));` + OK,
  "fs/promises under another namespace name": `import * as fsPromises from "node:fs/promises";\n` +
    `const server = createServer(async (req, res) => res.end(await fsPromises.readFile(ROOT + req.url)));` + OK,
  "path under another namespace name": `import * as p from "node:path";\n` +
    `const server = createServer((req, res) => res.end(String(p.join(ROOT, req.url))));` + OK,
  "a default import of fs/promises under another name": `import fsx from "node:fs/promises";\n` +
    `const server = createServer(async (req, res) => res.end(await fsx.readFile(ROOT + req.url)));` + OK,
  "require(…) chained into a read": `const server = createServer((req, res) => res.end(require("node:fs").readFileSync(ROOT + req.url)));` + OK,
  "path required and called at once": `const server = createServer((req, res) => res.end(String(require("node:path").join(ROOT, req.url))));` + OK,
  "a write to a path from the request": `const server = createServer(async (req, res) => {\n  await writeFile(ROOT + req.url, "x");\n  res.end();\n});` + OK,
  "path required under another name": `const p = require("node:path");\n` +
    `const server = createServer((req, res) => res.end(String(p.join(ROOT, req.url))));` + OK,
  "path imported at run time under another name": `const p = await import("node:path");\n` +
    `const server = createServer((req, res) => res.end(String(p.join(ROOT, req.url))));` + OK,
  "a module the scan cannot read": `const mod = "node:" + "path";\nconst q = require(mod);\n` +
    `const server = createServer((req, res) => res.end(String(q.join(ROOT, req.url))));` + OK,
  "fs.promises destructured under a name": `const { promises: p } = fs;\n` +
    `const server = createServer(async (req, res) => res.end(await p.readFile(ROOT + req.url)));` + OK,
  "join destructured under a name": `const { join: j } = path;\n` +
    `const server = createServer((req, res) => res.end(String(j(ROOT, req.url))));` + OK,
  "an alias of fs.createReadStream": `const crs = fs.createReadStream;\n` +
    `const server = createServer((req, res) => crs(ROOT + req.url).pipe(res));` + OK,
  "readFile handed itself to a call": `const server = createServer(async (req, res) => res.end(await Reflect.apply(readFile, null, [ROOT + req.url])));` + OK,
  "readFile through .call": `const server = createServer(async (req, res) => res.end(await readFile.call(null, ROOT + req.url)));` + OK,
  "a computed member of fs": `const server = createServer((req, res) => res.end(fs["readFileSync"](ROOT + req.url)));` + OK,
  "createRequire": `const server = createServer((req, res) => res.end(String(createRequire(import.meta.url)("node:path").join(ROOT, req.url))));` + OK,
  "eval": `const server = createServer((req, res) => res.end(eval("readFileSync")(ROOT + req.url)));` + OK,
  "a read of the read family on an object handed in": `const serve = (io, res, rel) => io.readFile(ROOT + rel).then((b) => res.end(b));\n` +
    `const server = createServer((req, res) => serve(fsp, res, req.url));` + OK,
  // rule 1: servers and routes the scan must find and follow
  "a server constructed with new": `import http from "node:http";\n` +
    `const server = new http.Server(async (req, res) => res.end(await readFile(join(ROOT, req.url))));` + OK,
  "a server with no handler, then a request listener": `const server = createServer();\n` +
    `server.on("request", async (req, res) => res.end(await readFile(join(ROOT, req.url))));` + OK,
  "a request listener beside a handler": IDX + `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
    `  if (file) res.end(await readFile(file));\n});\nserver.on("request", (req) => stat(join(ROOT, req.url)));` + OK,
  "a page route read from disk": `const server = createServer((req, res) => res.end(PAGE));` + OK +
    `\nawait page.route("**/*", async (route) => route.fulfill({ body: await readFile(join(ROOT, new URL(route.request().url()).pathname)) }));`,
  "a page route fulfilled from a path": `const server = createServer((req, res) => res.end(PAGE));` + OK +
    `\nawait page.route("**/*", (route) => route.fulfill({ path: FILE_FOR[route.request().url()] }));`,
  // rule 4: loopback, readably
  "listening on every interface": IDX + `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
    `  res.end(await readFile(file));\n}).listen(0);`,
  "listening on all addresses by name": IDX + `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
    `  res.end(await readFile(file));\n});\nserver.listen(0, "0.0.0.0");`,
  "listening on a host the scan cannot read": `const SERVABLE = new Map();\nconst server = createServer(async (req, res) => {\n` +
    `  const file = SERVABLE.get(req.url);\n  res.end(await readFile(file));\n});\nserver.listen(0, HOST);`,
  "listen spelled as a string": IDX + `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
    `  res.end(await readFile(file));\n});\nserver["listen"](0);`,
  "listen through .call": IDX + `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
    `  res.end(await readFile(file));\n});\nserver.listen.call(server, 0);`,
};
// Rule 5's own refusals: each must be refused for its decode or its URL,
// whatever else the scan says about it.
const UNGUARDED = {
  "bench_probe.mjs before A52 (an unguarded decode)": `const SERVABLE = new Map();\nasync function allow(dirRel) {\n` +
    `  for (const f of await readdir(join(ROOT, dirRel))) SERVABLE.set(\`/\${dirRel}/\${f}\`, join(ROOT, dirRel, f));\n}\n` +
    `await allow("canary-local");\nconst server = createServer(async (req, res) => {\n` +
    `  const key = decodeURIComponent(req.url.split("?")[0].split("#")[0]);\n  const path = SERVABLE.get(key);\n` +
    `  if (!path) { res.writeHead(404); res.end(); return; }\n  const data = await readFile(path);\n  res.end(data);\n});` + OK,
  "boot_probe.mjs before A51 and A52 (every interface, an unguarded decode)": `const SERVABLE = new Map();\n` +
    `for (const rel of ["a.html", "b.js"]) {\n  SERVABLE.set("/" + rel, join(ROOT, rel));\n}\n` +
    `const server = createServer(async (req, res) => {\n  const key = decodeURIComponent(req.url.split("?")[0]);\n` +
    `  if (key === "/favicon.ico") { res.writeHead(204); res.end(); return; }\n  const path = SERVABLE.get(key);\n` +
    `  if (!path) { console.error(\`404 for \${key}\`); res.writeHead(404); res.end(); return; }\n` +
    `  try {\n    res.end(await readFile(path));\n  } catch { res.writeHead(404); res.end(); }\n}).listen(0);\nconst port = server.address().port;`,
  "an unguarded decode in a handler that is not async": IDX + `const server = createServer((req, res) => {\n` +
    `  const file = FILES.get(decodeURIComponent(req.url));\n  res.end(file ? "yes" : "no");\n});` + OK,
  "an unguarded decodeURI": IDX + `const server = createServer(async (req, res) => {\n  const file = FILES.get(decodeURI(req.url));\n` +
    `  if (file) res.end(await readFile(file));\n});` + OK,
  "a decode through globalThis, unguarded": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const file = FILES.get(globalThis.decodeURIComponent(req.url));\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a decode in a try with no catch": IDX + `const server = createServer(async (req, res) => {\n  let key = "/";\n` +
    `  try { key = decodeURIComponent(req.url); } finally { res.setHeader("x-k", "1"); }\n  const file = FILES.get(key);\n` +
    `  if (file) res.end(await readFile(file));\n});` + OK,
  "a decode in a try whose catch throws it again": IDX + `const server = createServer(async (req, res) => {\n  let key;\n` +
    `  try { key = decodeURIComponent(req.url); } catch (e) { console.error(e); throw e; }\n  const file = FILES.get(key);\n` +
    `  if (file) res.end(await readFile(file));\n});` + OK,
  "a decode in the catch, not the try": IDX + `const server = createServer(async (req, res) => {\n  let key;\n` +
    `  try { key = req.url.slice(0); } catch { key = decodeURIComponent(req.url); }\n  const file = FILES.get(key);\n` +
    `  if (file) res.end(await readFile(file));\n});` + OK,
  "a decode in a try's finally": IDX + `const server = createServer(async (req, res) => {\n  let key = "/";\n` +
    `  try { res.setHeader("x", "1"); } catch { key = "/"; } finally { key = decodeURIComponent(req.url); }\n` +
    `  const file = FILES.get(key);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a decode in a callback the try only schedules": IDX + `const server = createServer((req, res) => {\n` +
    `  try { setTimeout(() => res.end(String(FILES.has(decodeURIComponent(req.url)))), 0); } catch { res.end(); }\n});` + OK,
  "a decode in a method defined inside a try": IDX + `const server = createServer(async (req, res) => {\n  let key;\n` +
    `  try {\n    const o = { k() { return decodeURIComponent(req.url); } };\n    key = "/";\n    later(o);\n  } catch { key = "/"; }\n` +
    `  const file = FILES.get(key);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a helper's decode, called from inside a try (not seen caught)": IDX + `const dec = (u) => decodeURIComponent(u);\n` +
    `const server = createServer(async (req, res) => {\n  let key;\n  try { key = dec(req.url); } catch { key = "/"; }\n` +
    `  const file = FILES.get(key);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "decodeURIComponent handed itself to map": IDX + `const server = createServer(async (req, res) => {\n  let parts = [];\n` +
    `  try { parts = req.url.split("/").map(decodeURIComponent); } catch { parts = []; }\n` +
    `  const file = FILES.get("/" + parts.join("/"));\n  if (file) res.end(await readFile(file));\n});` + OK,
  "decodeURIComponent aliased": IDX + `const dec = decodeURIComponent;\nconst server = createServer(async (req, res) => {\n` +
    `  const file = FILES.get(dec(req.url));\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a decode at top level, outside a try": IDX + `const START = decodeURIComponent(process.argv[2] || "/");\n` +
    `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a URL built from the request outside a try (// throws)": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const u = new URL(req.url, "http://127.0.0.1");\n  const file = lookup(FILES, u.pathname);\n` +
    `  if (file) res.end(await readFile(file));\n});` + OK,
  "a URL built from the request in a helper, outside a try": IDX + `function pathOf(r) {\n  return new URL(r.url, "http://x").pathname;\n}\n` +
    `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, pathOf(req));\n  if (file) res.end(await readFile(file));\n});` + OK,
};
const PASSED = {
  "render_probe.mjs's server (the index)": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const path = req.url.split("?")[0];\n  if (path === "/probe.html") { res.end(HARNESS); return; }\n  try {\n` +
    `    const file = lookup(FILES, req.url);\n    if (!file) throw new Error("not in the tree");\n` +
    `    const body = await readFile(file);\n    res.writeHead(200, { "content-type": MIME[extname(path)] || "x" });\n` +
    `    res.end(body);\n  } catch { res.writeHead(404); res.end("not found"); }\n});` + OK,
  "bench_probe.mjs's server (an allowlist built at start, looked up)": `const SERVABLE = new Map();\nasync function allow(dirRel) {\n` +
    `  for (const f of await readdir(join(ROOT, dirRel))) SERVABLE.set(\`/\${dirRel}/\${f}\`, join(ROOT, dirRel, f));\n}\n` +
    `await allow("canary-local");\nconst server = createServer(async (req, res) => {\n` +
    `  const path = lookup(SERVABLE, req.url);\n` +
    `  if (!path) { res.writeHead(404); res.end(); return; }\n  const data = await readFile(path);\n  res.end(data);\n});` + OK,
  "boot_probe.mjs's allowlist (filled by a top-level loop, looked up)": `const SERVABLE = new Map();\nfor (const rel of ["a.html", "b.js"]) {\n` +
    `  SERVABLE.set("/" + rel, join(ROOT, rel));\n}\nconst server = createServer(async (req, res) => {\n` +
    `  const asked = req.url.split("?")[0];\n  if (asked === "/favicon.ico") { res.writeHead(204); res.end(); return; }\n` +
    `  const path = lookup(SERVABLE, req.url);\n` +
    `  if (!path) { console.error(\`404 for \${asked}\`); res.writeHead(404); res.end(); return; }\n` +
    `  res.end(await readFile(path));\n});` + OK + `\nconsole.log(\`serving \${SERVABLE.size} files\`);`,
  "a decode answered 400 inside its try": `const SERVABLE = new Map([["/a.html", "/srv/a.html"]]);\n` +
    `const server = createServer(async (req, res) => {\n  let key;\n` +
    `  try { key = decodeURIComponent(req.url.split("?")[0]); } catch { res.writeHead(400); return res.end(); }\n` +
    `  const path = SERVABLE.get(key);\n  if (!path) { res.writeHead(404); return res.end(); }\n  res.end(await readFile(path));\n});` + OK,
  "a decode inside an if, inside a try": IDX + `const server = createServer(async (req, res) => {\n  let key = "/";\n` +
    `  try {\n    if (req.url.includes("%")) { key = decodeURIComponent(req.url); }\n  } catch (e) { console.error(e.message); }\n` +
    `  const file = FILES.get(key);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a URL built from the request inside a try": IDX + `const server = createServer(async (req, res) => {\n  try {\n` +
    `    const u = new URL(req.url, "http://127.0.0.1");\n    const file = lookup(FILES, u.pathname);\n` +
    `    if (file) res.end(await readFile(file));\n  } catch { res.writeHead(400); res.end(); }\n});` + OK,
  "a URL of the probe's own, outside a try (nothing from the request)": IDX +
    `const BASE = new URL("http://127.0.0.1/");\nconst server = createServer(async (req, res) => {\n` +
    `  const here = new URL("/canary-local/", BASE);\n  const file = lookup(FILES, req.url);\n` +
    `  if (file) res.end(await readFile(file)); else res.end(here.pathname);\n});` + OK,
  "vision_probe.mjs now (the bridge first)": IDX + `const cores = await probeCores(["canary-vision-core"]);\n` +
    `const server = createServer(async (req, res) => {\n  try {\n` +
    `    if (cores && await cores.handle(req, res)) return;\n    const path = req.url.split("?")[0];\n` +
    `    if (path === "/favicon.ico") { res.writeHead(204); return res.end(); }\n    const file = lookup(FILES, req.url);\n` +
    `    if (!file) { res.writeHead(404); return res.end("not found"); }\n    const body = await readFile(file);\n` +
    `    res.writeHead(200, { "content-type": TYPES[extname(file)] || "application/octet-stream" });\n    res.end(body);\n` +
    `  } catch { res.writeHead(404); res.end("not found"); }\n});` + OK + `\nif (cores) await cores.gotoIdle(page, url, { timeout: 45000 });`,
  "a lookup that falls back to null": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const file = lookup(FILES, req.url) ?? null;\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a page read once at start, answered by route": `const PAGE = await readFile(join(ROOT, "canary-local/x.html"));\n` +
    `const server = createServer((req, res) => { if (req.url === "/x") res.end(PAGE); });` + OK,
  "a handler defined by name": IDX + `const serve = async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
    `  res.end(await readFile(file));\n};\nconst server = createServer(serve);` + OK,
  "a server with options first": IDX + `const server = createServer({ keepAlive: true }, async (req, res) => {\n` +
    `  const file = lookup(FILES, req.url);\n  res.end(await readFile(file));\n});` + OK,
  "loopback through an options object": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const file = lookup(FILES, req.url);\n  res.end(await readFile(file));\n});\nserver.listen({ port: 0, host: "127.0.0.1" });`,
  "a helper handed only what the index answered": IDX + `const send = async (res, file) => res.end(await readFile(file));\n` +
    `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n  await send(res, file);\n});` + OK,
  "fs and path under the names the scan knows": `import { promises as fsp } from "node:fs";\nimport * as path from "node:path";\n` +
    `const { readFile } = fsp;\nconst ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");\n` + IDX +
    `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "render_probe.mjs's shots folder (fs/promises called at once)": IDX +
    `if (SHOTS) await (await import("node:fs/promises")).mkdir(SHOTS, { recursive: true });\n` +
    `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n  if (file) res.end(await readFile(file));\n});` + OK,
  "a request echoed back (parts.join and Promise.resolve are not paths)": IDX + `const server = createServer(async (req, res) => {\n` +
    `  const parts = [req.method, req.url];\n  const file = lookup(FILES, req.url);\n` +
    `  if (!file) return res.end(await Promise.resolve(parts.join(" ")));\n  res.end(await readFile(file));\n});` + OK,
  "a request logged and counted, which reaches no read": IDX + `const served = new Set();\n` +
    `const server = createServer(async (req, res) => {\n  console.log(req.method, req.url);\n  served.add(req.url.split("?")[0]);\n` +
    `  const file = lookup(FILES, req.url);\n  if (file) res.end(await readFile(file));\n});` + OK +
    `\nawait new Promise((resolve) => setTimeout(resolve, 10));\nconsole.log([...served].join("\\n"));`,
};

test("the request-path scan refuses a path built from the request, however it gets there", () => {
  for (const [what, src] of Object.entries(REFUSED)) {
    const { servers, problems } = requestPathProblems(src);
    assert.ok(isServerFile(src), `${what}: not seen as a server file`);
    assert.ok(servers > 0, `${what}: the scan saw no server`);
    assert.ok(problems.length > 0, `${what}: the scan must refuse it`);
  }
  for (const [what, src] of Object.entries(UNGUARDED)) {
    const { servers, problems } = requestPathProblems(src);
    assert.ok(isServerFile(src) && servers > 0, `${what}: the scan saw no server`);
    assert.ok(problems.some((p) => /decodeURI(?:Component)?(?:\(…\))? (?:outside|handed)|new URL\(…\) is handed/.test(p)),
      `${what}: not refused for throwing on a malformed request (${problems.join("; ") || "no problems"})`);
  }
  for (const [what, src] of Object.entries(PASSED)) {
    const { servers, problems } = requestPathProblems(src);
    assert.ok(isServerFile(src), `${what}: not seen as a server file`);
    assert.ok(servers > 0, `${what}: the scan saw no server`);
    assert.deepStrictEqual(problems, [], `${what}: a probe that serves its index's answer, refused`);
  }
  // a server file is found by what it says, not only by createServer
  for (const src of [`const s = new http.Server(h);`, `s["listen"](0);`, `const { listen } = s;`,
    `await page.route("**", h);`, `route.fulfill({ status: 204 });`, `https.createSecureServer(o, h);`]) {
    assert.ok(isServerFile(src), `${src}: not seen as a server file`);
  }
  for (const src of [`page.on("request", log);`, `// call listen() here\nconst x = "listening";`, `x.addEventListener("load", f);`]) {
    assert.ok(!isServerFile(src), `${src}: seen as a server file`);
  }
});

// The five allowlist probes as they are, each mutated back one way: its
// lookup() turned into the bare decode it replaced (sweep A52), or its
// listen turned back to every interface (A51). Each mutant must be refused
// for that, so the guard the real file carries is the one the scan holds.
test("the scan refuses each allowlist probe with its decode unguarded or its listen opened up", () => {
  const ALLOWLIST = ["bench_probe.mjs", "boardroom_probe.mjs", "workshop_probe.mjs", "boot_probe.mjs", "onboard_probe.mjs"];
  const LOOKUP = "lookup(SERVABLE, req.url)";
  const LISTEN = `server.listen(0, "127.0.0.1", ok)`;
  for (const f of ALLOWLIST) {
    const src = read(join(__dirname, f));
    assert.deepStrictEqual(requestPathProblems(src).problems, [], `${f}: refused as it stands`);
    assert.strictEqual(src.split(LOOKUP).length - 1, 1, `${f}: answers a request with ${LOOKUP} once`);
    assert.strictEqual(src.split(LISTEN).length - 1, 1, `${f}: listens with ${LISTEN} once`);
    for (const bare of [`SERVABLE.get(decodeURIComponent(req.url.split("?")[0]))`, `SERVABLE.get(decodeURI(req.url))`]) {
      const { problems } = requestPathProblems(src.replace(LOOKUP, bare));
      assert.ok(problems.some((p) => /decodeURI(?:Component)?\(…\) outside a try/.test(p)),
        `${f} with ${bare}: an unguarded decode the scan let through (${problems.join("; ") || "no problems"})`);
    }
    for (const open of ["server.listen(0, ok)", `server.listen(0, "0.0.0.0", ok)`, `server.listen({ port: 0 }, ok)`]) {
      const { problems } = requestPathProblems(src.replace(LISTEN, open));
      assert.ok(problems.some((p) => / not loopback$/.test(p)), `${f} with ${open}: listening beyond loopback, let through`);
    }
  }
});

// ── CI wiring ───────────────────────────────────────────────────────────────

test("CI runs this gate", () => {
  const workflow = read(join(REPO, ".github/workflows/canary-local.yml"));
  assert.ok(workflow.includes("node --test canary-local/tests/probe_server.test.js"),
    "node --test canary-local/tests/probe_server.test.js is not wired into canary-local.yml");
});
