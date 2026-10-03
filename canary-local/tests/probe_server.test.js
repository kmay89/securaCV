// canary-local/tests/probe_server.test.js — every browser probe serves the
// repository through probe_server.mjs's index, on loopback only.
//
// A probe's server must never turn a request URL into a filesystem path: the
// URL is untrusted input, and decode, resolve(join(ROOT, rel)), then a
// startsWith(ROOT + sep) test is the shape CodeQL's path-injection query
// flagged twice on #1737. probe_server.mjs indexes the tree once and answers
// a request with a Map lookup, so the path that reaches readFile is the
// index's own string; twelve probes still did the dance until sweep A46.
//
// What can rot, and which test catches it:
//   · lookup() starts reading a request as a path      → "lookup answers from the index"
//   · a probe resolves a request path itself again     → "no probe resolves a request path"
//   · a probe reads a file the index did not hand it   → same test
//   · a probe server listens beyond loopback           → same test
//   · the scan stops seeing any of those               → "the request-path scan refuses"
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
const { readJs, IDENT, esc, lineOf, trivia, argSpansAt, functionsIn } = require("./js_scan.js");

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
    // the index holds the paths it walked; a file the tree gains later is not served
    put("canary-local/late.html", "late");
    assert.strictEqual(lookup(files, "/canary-local/late.html"), null);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

// ── every probe's server ─────────────────────────────────────────────────────

// What the scan holds, for each createServer call in a probe (any file here
// but a test that starts a server), all within the one file:
//
//   1. It can find the handler: an inline function, or one the file defines
//      by that name, whose request parameter is a plain name.
//   2. Every filesystem read inside the handler reads a name whose every
//      write in the handler is the index's answer: lookup(INDEX, …) or a
//      prebuilt map's INDEX.get(…) (the allowlist probes), optionally `?? null`
//      or `|| null`. A read of anything else is refused, whatever it holds.
//   3. Nothing derived from the request reaches a path function (join,
//      resolve, normalize, relative) or a filesystem call: the request, every
//      name written from an expression that mentions it or another such name
//      (the index's answer excepted), and each parameter of a function the
//      file defines that a call hands one of those, followed into that
//      function. So a helper that joins what the handler hands it is refused
//      too.
//   4. It listens on loopback: listen(port, "127.0.0.1") (or "::1"), or an
//      options object whose host is one of those.
//
// What it cannot follow: a request value stored into an object or array by a
// call (push, set) and read back, a method of a class, a module's export.
// Rule 2 is the backstop for those inside the handler: whatever a value went
// through, the handler reads only what the index answered.

const PATH_FNS = ["join", "resolve", "normalize", "relative"];
const FS_FNS = ["readFile", "readFileSync", "createReadStream", "stat", "statSync", "lstat", "lstatSync", "access",
  "accessSync", "existsSync", "open", "openSync", "readdir", "readdirSync", "realpath", "realpathSync", "opendir"];
const LOOPBACK = new Set(["127.0.0.1", "::1"]);
// A bare call or a member's (path.join, fs.readFile, posix.normalize), never a
// method of something else with that name (FILES.get is not a read).
const callRe = (names, member = String.raw`(?:(?:path|posix|win32|fs|fsp|promises)\s*\.\s*)*`) =>
  new RegExp(String.raw`(?<![\w$.])${member}(${names.join("|")})\s*\(`, "g");
const mentions = (text, name) => new RegExp(String.raw`(?<![\w$.])${esc(name)}(?![\w$])`).test(text);
const SANITIZER = new RegExp(String.raw`^(?:lookup\s*\(\s*${IDENT}\s*,|${IDENT}\s*\.\s*get\s*\()`);

// The expression assigned at index from: up to the next comma or semicolon of
// the bracket around it, or that bracket's close.
function exprEnd(src, from) {
  const { pairs, commas, semis } = readJs(src);
  let o = -1;
  for (const [a, c] of pairs) if (a < from && from < c && a > o) o = a;
  const ends = [...(commas.get(o) || []), ...(semis.get(o) || [])].filter((x) => x >= from);
  return Math.min(o >= 0 ? pairs.get(o) : src.length, ...ends);
}

// The index's answer, and only it: lookup(INDEX, …) or INDEX.get(…), whole.
function isSanitizer(src, value, at) {
  const v = trivia(value).replace(/\s*(?:\?\?|\|\|)\s*null$/, "");
  if (!SANITIZER.test(v)) return false;
  const start = at + value.indexOf(v);
  const close = readJs(src).pairs.get(start + v.indexOf("("));
  return close !== undefined && close === start + v.length - 1;   // the call is the whole value
}

// Every write in [from, to): { names, value, at } for `const|let|var x = v`,
// destructuring, and `x = v` / `x += v`.
function writesIn(src, from, to) {
  const out = [];
  const body = src.slice(from, to);
  const decl = new RegExp(String.raw`(?<![\w$.])(?:const|let|var)\s+(${IDENT}|\{[^}]*\}|\[[^\]]*\])\s*=(?![=>])`, "g");
  const plain = new RegExp(String.raw`(?<![\w$.])(${IDENT})\s*(?:\+=|=(?![=>]))`, "g");
  const seen = new Set();
  for (const re of [decl, plain]) {
    for (const m of body.matchAll(re)) {
      const at = from + m.index + m[0].length;
      if (seen.has(at)) continue;
      seen.add(at);
      if (re === plain && /(?:const|let|var)\s+$/.test(body.slice(0, m.index))) continue;
      const names = m[1].match(new RegExp(IDENT, "g")).filter((n) => !["const", "let", "var"].includes(n));
      out.push({ names, value: src.slice(at, exprEnd(src, at)), at });
    }
  }
  return out;
}

// Problems with one function's body, given the names in it that carry the
// request; follows the file's own functions a call hands one of them.
function taintProblems(src, fn, carriers, visited, problems) {
  const key = `${fn.from}:${[...carriers].sort().join(",")}`;
  if (visited.has(key)) return;
  visited.add(key);
  const tainted = new Set(carriers);
  const writes = writesIn(src, fn.from, fn.to);
  for (let grew = true; grew;) {
    grew = false;
    for (const w of writes) {
      if (isSanitizer(src, w.value, w.at)) continue;
      if (![...tainted].some((t) => mentions(w.value, t))) continue;
      for (const n of w.names) if (!tainted.has(n)) { tainted.add(n); grew = true; }
    }
  }
  const body = src.slice(fn.from, fn.to);
  const carried = (text) => [...tainted].find((t) => mentions(text, t));
  for (const m of body.matchAll(callRe([...PATH_FNS, ...FS_FNS]))) {
    const open = fn.from + m.index + m[0].length - 1;
    const args = argSpansAt(src, open);
    const t = args && args.map((a) => carried(a.text)).find(Boolean);
    if (t) problems.push(`line ${lineOf(src, open)}: ${m[1]}(…) is handed ${t}, which comes from the request`);
  }
  // template literals and concatenations reach a sink only through a call
  // or a write, both of which the passes above and below see
  const defs = functionsIn(src).filter((f) => f.name && f.from !== fn.from);
  for (const def of defs) {
    for (const c of body.matchAll(new RegExp(String.raw`(?<![\w$.])${esc(def.name)}\s*\(`, "g"))) {
      const open = fn.from + c.index + c[0].length - 1;
      const args = argSpansAt(src, open) || [];
      const handed = new Set();
      args.forEach((a, k) => { if (carried(a.text) && def.params[k]) handed.add(def.params[k]); });
      if (handed.size) taintProblems(src, def, handed, visited, problems);
    }
  }
}

// What is wrong with each probe server in src: an array of phrases.
function requestPathProblems(src) {
  const problems = [];
  const fns = functionsIn(src);
  let servers = 0;
  for (const m of src.matchAll(/(?<![\w$])createServer\s*\(/g)) {
    servers++;
    const open = m.index + m[0].length - 1;
    const args = argSpansAt(src, open);
    const where = `line ${lineOf(src, m.index)}`;
    if (!args || !args.length) { problems.push(`${where}: a createServer(…) the scan cannot read`); continue; }
    const h = args[args.length - 1];   // createServer([options,] handler)
    let fn = fns.filter((f) => f.from >= h.at && f.from <= h.at + 16 && f.to <= h.at + h.text.length + 1)
      .sort((a, b) => a.from - b.from)[0];
    if (!fn && new RegExp(`^${IDENT}$`).test(h.text)) fn = fns.find((f) => f.name === h.text);
    if (!fn) { problems.push(`${where}: a handler (${h.text.slice(0, 30)}) the scan cannot find`); continue; }
    const req = fn.params[0];
    if (!req) { problems.push(`${where}: a handler whose request is not a plain name, which the scan cannot follow`); continue; }
    // rule 2: every read in the handler is of the index's answer
    const writes = writesIn(src, fn.from, fn.to);
    for (const r of src.slice(fn.from, fn.to).matchAll(callRe(FS_FNS))) {
      const ropen = fn.from + r.index + r[0].length - 1;
      const first = (argSpansAt(src, ropen) || [])[0];
      const name = first && new RegExp(`^${IDENT}$`).test(first.text) ? first.text : null;
      const given = name ? writes.filter((w) => w.names.includes(name)) : [];
      const ok = name && given.length && given.every((w) => isSanitizer(src, w.value, w.at));
      if (!ok) problems.push(`line ${lineOf(src, ropen)}: ${r[1]}(${first ? first.text.slice(0, 30) : ""}) reads what the index did not answer`);
    }
    // rule 3: nothing from the request reaches a path or a read
    taintProblems(src, fn, new Set([req]), new Set(), problems);
  }
  // rule 4: loopback only
  for (const l of src.matchAll(/\.\s*listen\s*\(/g)) {
    const open = l.index + l[0].length - 1;
    const args = argSpansAt(src, open) || [];
    const host = (() => {
      const o = args[0] && /^\{/.test(args[0].text) ? /(?<![\w$])host\s*:\s*(["'])([^"']*)\1/.exec(args[0].text) : null;
      if (o) return o[2];
      const s = args[1] && /^(["'])([^"']*)\1$/.exec(args[1].text);
      return s ? s[2] : null;
    })();
    if (!LOOPBACK.has(host)) {
      problems.push(`line ${lineOf(src, open)}: listen(${args.map((a) => a.text).join(", ").slice(0, 40)}) binds ` +
        `${host === null ? "every interface (no loopback host the scan can read)" : host}, not loopback`);
    }
  }
  return { servers, problems };
}

// Probes another change owns this wave, which still listen on every
// interface: rules 1 to 3 hold for them, rule 4 waits for their owner.
const LOOPBACK_PENDING = new Set(["boot_probe.mjs", "onboard_probe.mjs"]);
const excused = (rel, why) => LOOPBACK_PENDING.has(rel) && / not loopback$/.test(why);

test("no probe resolves a request path itself: each serves the index's answer, on loopback", () => {
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
      if (!/(?<![\w$])createServer\s*\(/.test(src)) continue;
      probes.push(rel);
      const found = requestPathProblems(src);
      servers += found.servers;
      for (const why of found.problems) {
        if (excused(rel, why)) continue;
        bad.push(`${rel}: ${why}`);
      }
    }
  };
  walk(__dirname);
  assert.ok(servers >= 17, `only ${servers} probe servers found: the scan is looking in the wrong place`);
  assert.ok(scanned.includes(join("native", "probe_cores.js")), "the scan reads the directories below this one too");
  for (const f of LOOPBACK_PENDING) assert.ok(probes.includes(f), `${f} is excused from loopback but serves nothing here`);
  // the excuse covers the listen rule and nothing else, for those two only
  assert.ok(excused("boot_probe.mjs", "line 82: listen(0) binds every interface (no loopback host the scan can read), not loopback"));
  assert.ok(!excused("boot_probe.mjs", "line 78: readFile(path) reads what the index did not answer"));
  assert.ok(!excused("onboard_probe.mjs", "line 197: join(…) is handed key, which comes from the request"));
  assert.ok(!excused("vision_probe.mjs", "line 60: listen(0) binds every interface (no loopback host the scan can read), not loopback"));
  assert.deepStrictEqual(bad, [], "a probe's server turns a request into a path, reads what the index did not answer, or listens beyond loopback");
});

// The scan itself, on the shapes it must refuse and the ones the probes use.
test("the request-path scan refuses a path built from the request, however it gets there", () => {
  const ok = `\nawait new Promise((done) => server.listen(0, "127.0.0.1", done));`;
  const refused = {
    "vision_probe.mjs before A46 (resolve, then check)":
      `const server = createServer(async (req, res) => {\n  try {\n    if (cores && await cores.handle(req, res)) return;\n` +
      `    const rel = decodeURIComponent(new URL(req.url, "http://x").pathname);\n` +
      `    if (rel === "/favicon.ico") { res.writeHead(204); return res.end(); }\n    const p = resolve(join(ROOT, rel));\n` +
      `    if (p !== ROOT && !p.startsWith(ROOT + sep)) { res.writeHead(403); return res.end(); }\n` +
      `    const file = rel.endsWith("/") ? join(p, "index.html") : p;\n    const body = await readFile(file);\n    res.end(body);\n` +
      `  } catch { res.writeHead(404); res.end("not found"); }\n});` + ok,
    "a join straight from the URL": `const server = createServer(async (req, res) => res.end(await readFile(join(ROOT, req.url))));` + ok,
    "a resolved path looked up in a map (still a path from the request)":
      `const server = createServer(async (req, res) => {\n  const p = resolve(ROOT, "." + req.url);\n  const file = FILES.get(p);\n` +
      `  res.end(await readFile(file));\n});` + ok,
    "a decoded name, resolved, then looked up": `const server = createServer(async (req, res) => {\n` +
      `  const rel = decodeURIComponent(req.url);\n  const p = resolve(ROOT, "." + rel);\n  const file = FILES.get(p);\n` +
      `  res.end(await readFile(file));\n});` + ok,
    "a loop that feeds the URL back to an earlier line": `const server = createServer(async (req, res) => {\n` +
      `  let next = "/index.html";\n  for (let i = 0; i < 2; i++) {\n    const rel = next.slice(1);\n` +
      `    const file = FILES.get(resolve(ROOT, rel));\n    if (file) return res.end(await readFile(file));\n` +
      `    next = req.url;\n  }\n});` + ok,
    "a helper that joins what the handler hands it":
      `const serve = async (res, rel) => res.end(await readFile(join(ROOT, rel)));\n` +
      `const server = createServer((req, res) => serve(res, req.url));` + ok,
    "a helper handed the whole request": `function serve(rq, res) {\n  return stat(join(ROOT, rq.url));\n}\n` +
      `const server = createServer((req, res) => serve(req, res));` + ok,
    "a request path smuggled through an array (the read rule's catch)":
      `const server = createServer(async (req, res) => {\n  const parts = [];\n  parts.push(ROOT, req.url);\n` +
      `  const file = parts.join("");\n  res.end(await readFile(file));\n});` + ok,
    "a handler defined by name": `async function handler(req, res) {\n  res.end(await readFile(ROOT + req.url));\n}\n` +
      `const server = createServer(handler);` + ok,
    "a handler the scan cannot find": `const server = createServer(handlers.main);` + ok,
    "a destructured request": `const server = createServer(async ({ url }, res) => res.end(await readFile(lookup(FILES, url))));` + ok,
    "a lookup with a fallback": `const server = createServer(async (req, res) => {\n` +
      `  const file = lookup(FILES, req.url) ?? join(ROOT, req.url);\n  res.end(await readFile(file));\n});` + ok,
    "a lookup that falls back to the request's own path": `const server = createServer(async (req, res) => {\n` +
      `  const rel = req.url.slice(1);\n  const file = lookup(FILES, req.url) || rel;\n  res.end(await readFile(file));\n});` + ok,
    "a lookup, then rewritten": `const server = createServer(async (req, res) => {\n  let file = lookup(FILES, req.url);\n` +
      `  if (!file) file = ROOT + req.url;\n  res.end(await readFile(file));\n});` + ok,
    "a request aliased, then joined": `const server = createServer(async (req, res) => {\n  const r = req;\n  const { url } = r;\n` +
      `  res.end(await fs.promises.readFile(path.join(ROOT, url)));\n});` + ok,
    "a template literal path": "const server = createServer(async (req, res) => res.end(await readFile(`${ROOT}${req.url}`)));" + ok,
    "a read stream of the URL": `const server = createServer((req, res) => {\n  const u = decodeURIComponent(req.url);\n` +
      `  fs.createReadStream(normalize(ROOT + u)).pipe(res);\n});` + ok,
    "listening on every interface": `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
      `  res.end(await readFile(file));\n}).listen(0);`,
    "listening on all addresses by name": `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
      `  res.end(await readFile(file));\n});\nserver.listen(0, "0.0.0.0");`,
    "listening on a host the scan cannot read": `const server = createServer(async (req, res) => {\n  const file = SERVABLE.get(req.url);\n` +
      `  res.end(await readFile(file));\n});\nserver.listen(0, HOST);`,
  };
  for (const [what, src] of Object.entries(refused)) {
    const { servers, problems } = requestPathProblems(src);
    assert.ok(servers > 0, `${what}: the scan saw no server`);
    assert.ok(problems.length > 0, `${what}: the scan must refuse it`);
  }
  const passed = {
    "render_probe.mjs's server (the index)": `const FILES = indexTree(ROOT);\nconst server = createServer(async (req, res) => {\n` +
      `  const path = req.url.split("?")[0];\n  if (path === "/probe.html") { res.end(HARNESS); return; }\n  try {\n` +
      `    const file = lookup(FILES, req.url);\n    if (!file) throw new Error("not in the tree");\n` +
      `    const body = await readFile(file);\n    res.writeHead(200, { "content-type": MIME[extname(path)] || "x" });\n` +
      `    res.end(body);\n  } catch { res.writeHead(404); res.end("not found"); }\n});` + ok,
    "bench_probe.mjs's server (an allowlist built at start)": `const SERVABLE = new Map();\nasync function allow(dirRel) {\n` +
      `  for (const f of await readdir(join(ROOT, dirRel))) SERVABLE.set(\`/\${dirRel}/\${f}\`, join(ROOT, dirRel, f));\n}\n` +
      `await allow("canary-local");\nconst server = createServer(async (req, res) => {\n` +
      `  const key = decodeURIComponent(req.url.split("?")[0].split("#")[0]);\n  const path = SERVABLE.get(key);\n` +
      `  if (!path) { res.writeHead(404); res.end(); return; }\n  const data = await readFile(path);\n  res.end(data);\n});` + ok,
    "vision_probe.mjs now (the bridge first)": `const server = createServer(async (req, res) => {\n  try {\n` +
      `    if (cores && await cores.handle(req, res)) return;\n    const path = req.url.split("?")[0];\n` +
      `    if (path === "/favicon.ico") { res.writeHead(204); return res.end(); }\n    const file = lookup(FILES, req.url);\n` +
      `    if (!file) { res.writeHead(404); return res.end("not found"); }\n    const body = await readFile(file);\n` +
      `    res.writeHead(200, { "content-type": TYPES[extname(file)] || "application/octet-stream" });\n    res.end(body);\n` +
      `  } catch { res.writeHead(404); res.end("not found"); }\n});` + ok,
    "a lookup that falls back to null": `const server = createServer(async (req, res) => {\n` +
      `  const file = lookup(FILES, req.url) ?? null;\n  if (file) res.end(await readFile(file));\n});` + ok,
    "a page read once at start, answered by route": `const PAGE = await readFile(join(ROOT, "canary-local/x.html"));\n` +
      `const server = createServer((req, res) => { if (req.url === "/x") res.end(PAGE); });` + ok,
    "a handler defined by name": `const serve = async (req, res) => {\n  const file = lookup(FILES, req.url);\n` +
      `  res.end(await readFile(file));\n};\nconst server = createServer(serve);` + ok,
    "a server with options first": `const server = createServer({ keepAlive: true }, async (req, res) => {\n` +
      `  const file = lookup(FILES, req.url);\n  res.end(await readFile(file));\n});` + ok,
    "loopback through an options object": `const server = createServer(async (req, res) => {\n` +
      `  const file = lookup(FILES, req.url);\n  res.end(await readFile(file));\n});\nserver.listen({ port: 0, host: "127.0.0.1" });`,
    "a helper handed only what the index answered": `const send = async (res, file) => res.end(await readFile(file));\n` +
      `const server = createServer(async (req, res) => {\n  const file = lookup(FILES, req.url);\n  await send(res, file);\n});` + ok,
  };
  for (const [what, src] of Object.entries(passed)) {
    const { servers, problems } = requestPathProblems(src);
    assert.ok(servers > 0, `${what}: the scan saw no server`);
    assert.deepStrictEqual(problems, [], `${what}: a probe that serves the index's answer, refused`);
  }
});

// ── CI wiring ───────────────────────────────────────────────────────────────

test("CI runs this gate", () => {
  const workflow = read(join(REPO, ".github/workflows/canary-local.yml"));
  assert.ok(workflow.includes("node --test canary-local/tests/probe_server.test.js"),
    "node --test canary-local/tests/probe_server.test.js is not wired into canary-local.yml");
});
