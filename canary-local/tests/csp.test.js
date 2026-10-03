// canary-local/tests/csp.test.js — every Lab page's Content-Security-Policy,
// pinned to the one table that writes it (tools/gen_csp.py).
//
// What can rot, and which test catches it:
//   · a page ships without a policy, or with two          → "exactly one CSP meta"
//   · a page or the table changed and nobody regenerated  → "--check passes"
//   · someone loosens script-src to make an inline work   → "never loosens"
//   · an onclick= or style= sneaks back into the markup   → "no inline handlers / styles"
//   · an inline <script> stays but its hash went stale    → "inline bodies are hashed"
//   · a page starts booting wasm without the source       → "wasm pages, and only those"
//   · a page reaches the release-fetching modules without → "signed-release hosts"
//     the hosts (fails behind a click, off the probe's path)
//   · a directive nobody needs quietly appears             → "trimmed, not granted"
//   · a firmware captive page (a srcdoc frame, which        → "each srcdoc page's style-src pins"
//     inherits the policy) changes and its hash goes stale
//   · a module creates a <style> element (inline style the  → "no module writes style="
//     load-time probe never sees — fleet.html's toggle did)
//   · a module writes a style= attribute the probe never  → "no module writes style="
//     happens to exercise
//   · flash.html's hand-written policy quietly widens     → "flash.html is no weaker"
//   · a page needs a source the desktop app would block   → "desktop Lab agrees"
//   · a probe waits on a string predicate, which          → "no probe waits on a string"
//     Playwright re-evaluates through eval in the page
//     every frame and the policy refuses (A44)
//   · that scan stops following a string through a        → "the string-predicate scan follows"
//     wrapper, a name or parentheses
//   · the gate is dropped from CI                         → "CI runs this gate"
//
// tests/csp_probe.mjs is the other half: real Chromium, every page, zero
// securitypolicyviolation events. Runs under "page logic tests"
// (.github/workflows/canary-local.yml); reads source text only.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync, existsSync } = require("node:fs");
const { join, dirname, resolve } = require("node:path");
const { spawnSync } = require("node:child_process");
const { createHash } = require("node:crypto");

const ROOT = join(__dirname, "..");        // canary-local/
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");

const HARNESS = "emulator/web/harness.html";
const PAGES = [...readdirSync(ROOT).filter((f) => f.endsWith(".html")).sort(), HARNESS];
const pageHtml = new Map(PAGES.map((p) => [p, read(join(ROOT, p))]));

const META_RE = /<meta http-equiv="Content-Security-Policy" content="([^"]*)"/g;
const policiesOf = (html) => [...html.matchAll(META_RE)].map((m) => m[1]);
const policyOf = (page) => policiesOf(pageHtml.get(page))[0];

// "directive → [sources]", in the order written.
function parse(csp) {
  const out = new Map();
  for (const part of csp.split(";")) {
    const [name, ...srcs] = part.trim().split(/\s+/);
    if (name) out.set(name, srcs);
  }
  return out;
}

const sha256 = (text) => "'sha256-" + createHash("sha256").update(text, "utf8").digest("base64") + "'";
// Strip every match, and keep stripping until nothing changes: a single pass
// over "<!<!---->--" leaves "<!--" behind, and the same goes for a nested
// "<scr<script></script>ipt>". An end tag is "</script" followed by anything
// up to ">" ("</script >" and "</script foo>" both close it in every browser).
const stripAll = (text, re) => {
  let prev;
  do { prev = text; text = text.replace(re, ""); } while (text !== prev);
  return text;
};
const stripHtmlComments = (html) => stripAll(html, /<!--[\s\S]*?-->/g);
const stripJsComments = (src) =>
  src.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/(^|[^:"'`\w])\/\/.*$/gm, "$1");
// Markup only: no comments, no script or style bodies.
const markupOf = (html) =>
  stripAll(stripAll(stripHtmlComments(html), /<script\b[^>]*>[\s\S]*?<\/script\b[^>]*>/gi), /<style\b[^>]*>[\s\S]*?<\/style\b[^>]*>/gi);

// ── the policies are present, generated, and strict ─────────────────────────

test("every Lab page carries exactly one CSP meta, on the line after <meta charset>", () => {
  assert.ok(PAGES.length >= 27, `expected the Lab's pages, found ${PAGES.length}`);
  for (const [page, html] of pageHtml) {
    const found = policiesOf(html);
    assert.strictEqual(found.length, 1, `${page}: expected one CSP <meta>, found ${found.length}`);
    const lines = html.split("\n");
    const i = lines.findIndex((l) => /<meta charset=/.test(l));
    assert.ok(i >= 0, `${page}: no <meta charset> line`);
    assert.match(lines[i + 1] || "", /<meta http-equiv="Content-Security-Policy"/,
      `${page}: the CSP meta must directly follow <meta charset> — gen_csp.py puts it there`);
  }
});

test("gen_csp.py --check passes: every page matches the policy table", () => {
  const r = spawnSync("python3", [join(ROOT, "tools/gen_csp.py"), "--check"], { encoding: "utf8" });
  assert.strictEqual(r.status, 0, `gen_csp.py --check failed (status ${r.status}):\n${r.stdout}${r.stderr}`);
});

test("no page's policy ever loosens: deny-all floor, same-origin code, no unsafe-*", () => {
  const SCRIPT_OK = /^(?:'self'|'wasm-unsafe-eval'|'sha256-[A-Za-z0-9+/=]+')$/;
  const STYLE_OK = /^(?:'self'|'sha256-[A-Za-z0-9+/=]+')$/;
  for (const page of PAGES) {
    const csp = policyOf(page);
    const p = parse(csp);
    assert.deepStrictEqual(p.get("default-src"), ["'none'"], `${page}: default-src must be 'none'`);
    const script = p.get("script-src") || [];
    assert.strictEqual(script[0], "'self'", `${page}: script-src must start with 'self'`);
    for (const s of script) assert.match(s, SCRIPT_OK, `${page}: script-src source ${s} is not allowed`);
    const style = p.get("style-src") || [];
    assert.strictEqual(style[0], "'self'", `${page}: style-src must start with 'self'`);
    for (const s of style) assert.match(s, STYLE_OK, `${page}: style-src source ${s} is not allowed`);
    for (const d of ["object-src", "base-uri", "form-action"]) {
      assert.deepStrictEqual(p.get(d), ["'none'"], `${page}: ${d} must be 'none'`);
    }
    assert.doesNotMatch(csp, /'unsafe-inline'|'unsafe-eval'|'unsafe-hashes'/, `${page}: unsafe-* in the policy`);
    // Ignored in a <meta> CSP — a promise the browser would not keep (lab_shell.test.js).
    assert.doesNotMatch(csp, /frame-ancestors|report-uri|sandbox/, `${page}: directive meaningless in a meta CSP`);
    // A scheme source in connect-src would let the page reach anywhere on that
    // scheme; the Lab talks to Canaries through the native side, never from the page.
    assert.doesNotMatch(csp, /connect-src[^;]*\b(?:https?|wss?):(?!\/\/)/, `${page}: open scheme source in connect-src`);
  }
});

// ── nothing inline that the policy does not account for ─────────────────────

test("no page carries an on*= handler or a style= attribute", () => {
  for (const [page, html] of pageHtml) {
    for (const tag of markupOf(html).match(/<[a-zA-Z][^>]*>/g) || []) {
      assert.doesNotMatch(tag, /\son[a-z]+\s*=/i,
        `${page}: inline handler in ${tag.slice(0, 80)} — addEventListener in the page's module instead`);
      assert.doesNotMatch(tag, /\sstyle\s*=/i,
        `${page}: style= attribute in ${tag.slice(0, 80)} — a class in the page's stylesheet instead`);
    }
  }
});

test("every inline <script> and <style> body is hashed into the policy — and there is exactly one", () => {
  const inline = [];
  for (const [page, html] of pageHtml) {
    const p = parse(policyOf(page));
    for (const m of html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script\b[^>]*>/gi)) {
      if (/\ssrc\s*=/.test(m[1])) continue;
      inline.push(`${page} <script${m[1]}>`);
      const want = sha256(m[2]);
      assert.ok((p.get("script-src") || []).includes(want),
        `${page}: inline <script${m[1]}> is not pinned — ${want} is missing from script-src (rerun gen_csp.py, or move it to a file)`);
    }
    for (const m of html.matchAll(/<style\b[^>]*>([\s\S]*?)<\/style\b[^>]*>/gi)) {
      inline.push(`${page} <style>`);
      const want = sha256(m[1]);
      assert.ok((p.get("style-src") || []).includes(want),
        `${page}: inline <style> is not pinned — ${want} is missing from style-src`);
    }
  }
  // flash.html's import map is the one script that cannot move (browsers do
  // not load an import map from a file). If this list grows, gen_csp.py has
  // grown an exception — it must carry a reason there, and be worth one here.
  assert.deepStrictEqual(inline, ['flash.html <script type="importmap">'],
    `inline blocks across the Lab: ${inline.join(", ")}`);
});

test("no first-party module writes a style= attribute", () => {
  // The probe only sees the paths a page load takes; a style= in a markup
  // string or a setAttribute("style") on a click path would slip past it.
  const dirs = [join(ROOT, "assets"), join(ROOT, "emulator/web")];
  for (const dir of dirs) {
    for (const f of readdirSync(dir)) {
      if (!/\.m?js$/.test(f)) continue;
      const src = stripJsComments(read(join(dir, f)));
      assert.doesNotMatch(src, /setAttribute\(\s*["']style["']/,
        `${f}: setAttribute("style") — use el.style.cssText / setProperty (CSSOM is allowed, the attribute is not)`);
      assert.doesNotMatch(src, /createElement\(\s*["']style["']\s*\)/,
        `${f}: creates a <style> element — that is inline style under style-src 'self'; put the rules in a stylesheet`);
      assert.doesNotMatch(src, /\sstyle=["'`]/,
        `${f}: a style= attribute inside a markup string — a class, or set it through the CSSOM after insertion`);
    }
  }
});

// ── the per-page grants describe the pages ──────────────────────────────────

const IMPORT_PATTERNS = [
  /\bfrom\s*["']([^"']+)["']/g,
  /\bimport\s*\(\s*["']([^"']+)["']/g,
  /(?:^|[;{}\s])import\s+["']([^"']+)["']/g,
];
const BUNDLE_RE = /canary-(?:display-[a-z0-9]+|vision-core|wap-audio)\.js\b/;

// Every first-party module a page's <script src> tags pull in, statically.
function moduleGraph(page) {
  const html = pageHtml.get(page);
  const base = dirname(join(ROOT, page));
  const queue = [...html.matchAll(/<script\b[^>]*\ssrc="([^"]+)"/g)].map((m) => resolve(base, m[1]));
  const seen = new Set();
  while (queue.length) {
    const f = queue.pop();
    if (seen.has(f) || !existsSync(f)) continue;
    seen.add(f);
    if (f.includes("emulator/dist/")) continue;
    const src = read(f);
    for (const re of IMPORT_PATTERNS) {
      for (const m of src.matchAll(re)) if (m[1].startsWith(".")) queue.push(resolve(dirname(f), m[1]));
    }
  }
  return [...seen];
}
const bootsWasm = (page) =>
  BUNDLE_RE.test(stripHtmlComments(pageHtml.get(page))) ||
  moduleGraph(page).some((f) => !f.includes("emulator/dist/") && BUNDLE_RE.test(stripJsComments(read(f))));

test("pages that boot the WebAssembly firmware carry 'wasm-unsafe-eval' — and only those", () => {
  const granted = PAGES.filter((p) => parse(policyOf(p)).get("script-src").includes("'wasm-unsafe-eval'"));
  const boots = PAGES.filter(bootsWasm);
  assert.deepStrictEqual(granted, boots, "the grant and the pages that load emulator/dist/ bundles disagree");
  // Pinned, so a page gaining or losing the firmware is a visible diff.
  assert.deepStrictEqual(boots, ["eyes.html", "fleet.html", "senselab.html", "smoke.html", "vision.html", HARNESS]);
});

test("frame-src 'self' is granted to exactly the pages that frame another", () => {
  const IFRAME_JS = /\bh\(\s*["']iframe["']|createElement\(\s*["']iframe["']|<iframe\b/;
  const frames = (page) =>
    /<iframe/.test(stripHtmlComments(pageHtml.get(page))) ||
    moduleGraph(page).some((f) => !f.includes("emulator/dist/") && IFRAME_JS.test(stripJsComments(read(f))));
  const granted = PAGES.filter((p) => parse(policyOf(p)).has("frame-src"));
  assert.deepStrictEqual(granted, PAGES.filter(frames));
  assert.deepStrictEqual(granted, ["lab.html", "witness-wall.html"]);
  for (const p of granted) assert.deepStrictEqual(parse(policyOf(p)).get("frame-src"), ["'self'"]);
});

test("the signed-release hosts are granted to exactly the pages whose modules fetch a release", () => {
  // flash.js and we2-flash.js fetch manifest-flash.json / the factory images /
  // the camera module's model from the GitHub release host. A page whose
  // <script src> graph reaches either without the hosts would fail behind a
  // click — a path the browser probe never takes.
  const FETCHERS = /(?:^|[\\/])(?:flash|we2-flash)\.js$/;
  const HOSTS = ["https://github.com", "https://*.githubusercontent.com"];
  const fetches = (page) => moduleGraph(page).some((f) => FETCHERS.test(f));
  const granted = PAGES.filter((p) => HOSTS.every((h) => parse(policyOf(p)).get("connect-src").includes(h)));
  assert.deepStrictEqual(granted, PAGES.filter(fetches), "the grant and the pages that reach flash.js / we2-flash.js disagree");
  assert.deepStrictEqual(granted, ["flash.html"]);
});

test("directives no page needs are trimmed, not granted: no blob:, no worker-src, no media-src", () => {
  // The floor could carry these; nothing in the Lab needs them (see the
  // policy table's docstring), so no page carries them. When a page does, it
  // is a table row with a reason — and this pin moves in the same diff.
  const IGNORED = ["worker-src", "media-src", "child-src", "manifest-src", "prefetch-src"];
  for (const page of PAGES) {
    const p = parse(policyOf(page));
    for (const d of IGNORED) assert.ok(!p.has(d), `${page}: ${d} granted without a documented need`);
    assert.doesNotMatch(policyOf(page), /\bblob:/, `${page}: blob: granted without a documented need`);
  }
  // And the module scan behind the generator's Worker check: no first-party
  // module spawns one today.
  for (const dir of [join(ROOT, "assets"), join(ROOT, "emulator/web")]) {
    for (const f of readdirSync(dir)) {
      if (!/\.m?js$/.test(f)) continue;
      assert.doesNotMatch(stripJsComments(read(join(dir, f))), /\bnew\s+(?:Shared)?Worker\s*\(/,
        `${f}: spawns a Worker — the page that loads it needs a worker-src row in gen_csp.py`);
    }
  }
});

// The pages that frame a firmware document in an <iframe srcdoc>, and the
// generated JSON each document ships in (gen_csp.py SRCDOC_STYLES).
const SRCDOC = {
  "wap.html": "devices/wap.json",                // gen_wap.py: canary-wap captive page
  "fleet.html": "devices/display_portal.json",   // gen_display_portal.py: the display's portal
};

test("each srcdoc page's style-src pins its firmware captive page — and nothing else does", () => {
  // wap-ui.js (wap.html) and onboard-phone.js (fleet.html) show a device's own
  // captive-portal HTML in an <iframe srcdoc>; a srcdoc document inherits the
  // embedder's policy, so its one <style> block is hashed from the generated
  // JSON (its generator reads the firmware source — the pin follows the
  // firmware). No hash can cover a style= attribute, an on*= handler or an
  // inline <script> there, so that document must carry none.
  for (const [page, rel] of Object.entries(SRCDOC)) {
    const captive = JSON.parse(read(join(ROOT, rel))).captive.html;
    const blocks = [...captive.matchAll(/<style\b[^>]*>([\s\S]*?)<\/style\b[^>]*>/gi)].map((m) => m[1]);
    assert.strictEqual(blocks.length, 1, `${rel}: the captive page carries one <style> block`);
    assert.deepStrictEqual(parse(policyOf(page)).get("style-src"), ["'self'", sha256(blocks[0])],
      `${page} style-src must be 'self' plus the captive page's <style> hash (rerun gen_csp.py after the page's generator)`);
    for (const tag of markupOf(captive).match(/<[a-zA-Z][^>]*>/g) || []) {
      assert.doesNotMatch(tag, /\s(?:on[a-z]+|style)\s*=/i, `${rel}: ${tag.slice(0, 80)} — no hash can allow it`);
    }
    assert.doesNotMatch(captive, /<script\b(?![^>]*\ssrc=)/i, `${rel}: the captive page has no inline script`);
  }
  // Every other page's style-src is exactly 'self'.
  for (const page of PAGES) {
    if (Object.hasOwn(SRCDOC, page)) continue;
    assert.deepStrictEqual(parse(policyOf(page)).get("style-src"), ["'self'"], `${page}: style-src`);
  }
});

// ── the flasher's policy is the one place a hand-written line existed ───────

test("flash.html's policy is no weaker than the hand-written one it replaced", () => {
  // The line flash.html carried before gen_csp.py existed (drift-gated by
  // flash.test.js then, and still).
  const OLD = "default-src 'none'; script-src 'self' 'sha256-EktR3SFf0lLCbyovl7Zr3tGJNxikCFvtZ3djuXGsas0='; " +
    "style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'self' https://github.com " +
    "https://*.githubusercontent.com http://localhost:* http://127.0.0.1:* https://localhost:* " +
    "https://127.0.0.1:*; object-src 'none'; base-uri 'none'; form-action 'none'";
  // The only sources the generator adds on top, each with its reason in
  // gen_csp.py (SHARED): the desktop Lab's Tauri IPC origins.
  const DOCUMENTED = { "connect-src": ["ipc:", "http://ipc.localhost"] };
  const norm = (s) => s.replace(/^'sha256-.+'$/, "'sha256-*'"); // the map's hash may legitimately move
  const oldP = parse(OLD);
  const newP = parse(policyOf("flash.html"));
  assert.deepStrictEqual([...newP.keys()], [...oldP.keys()], "flash.html gained or lost a directive");
  for (const [d, oldSrcs] of oldP) {
    const allowed = new Set([...oldSrcs.map(norm), ...(DOCUMENTED[d] || [])]);
    for (const s of newP.get(d)) {
      assert.ok(allowed.has(norm(s)), `flash.html ${d}: ${s} is neither in the original policy nor documented in gen_csp.py`);
    }
    for (const s of oldSrcs) {
      assert.ok(newP.get(d).map(norm).includes(norm(s)), `flash.html ${d} lost ${s}`);
    }
  }
  assert.strictEqual(newP.get("script-src").filter((s) => s.startsWith("'sha256-")).length, 1,
    "flash.html pins exactly one inline script: the import map");
});

// ── the same files run inside the desktop Lab's own policy ──────────────────

test("the desktop Lab agrees: IPC origins on every page, no page source the app would block", () => {
  const tauri = JSON.parse(read(join(REPO, "desktop-lab/src-tauri/tauri.conf.json")));
  const app = parse(tauri.app.security.csp);
  const appConnect = app.get("connect-src");
  for (const s of ["ipc:", "http://ipc.localhost"]) {
    assert.ok(appConnect.includes(s), `desktop-lab/src-tauri/tauri.conf.json connect-src lost ${s}`);
  }
  for (const page of PAGES) {
    const connect = parse(policyOf(page)).get("connect-src");
    for (const s of ["ipc:", "http://ipc.localhost"]) {
      assert.ok(connect.includes(s), `${page}: connect-src lacks ${s} — lab-nav.js's opener invoke would be blocked in the app`);
    }
    // Both policies are enforced in the app; a source the app lacks is a source
    // the page cannot use there, however this table reads.
    for (const s of connect) {
      assert.ok(appConnect.includes(s), `${page}: connect-src ${s} is not in the app's csp (tauri.conf.json) — blocked inside the desktop Lab`);
    }
  }
  assert.ok(app.get("script-src").includes("'wasm-unsafe-eval'"), "the app's own policy must allow the firmware wasm");
});

// ── the probes' own waits: never a string the policy refuses ─────────────────

// page.waitForFunction("window.__ready === true") is re-evaluated through
// globalThis.eval on every animation frame in the page (Playwright's
// server/frames.js, waitForFunctionExpression), and no page here allows
// 'unsafe-eval': whenever the predicate is false at the first poll (a wasm
// boot slower than the load event), the next poll throws EvalError and the
// probe fails, not the page. A function predicate is evaluated once, inside
// the DevTools call, which the policy does not govern, and then only called.
// boot_probe.mjs and csp_probe.mjs both waited on that string (sweep A44).
//
// What the scan follows, all within the one file: the predicate as written at
// the call, through any parentheses; a name, through every `name =` and
// `name:` the file writes (a string there is refused, another name is
// followed in turn); and a parameter of the function the call sits in (a
// wrapper such as operator_probe.mjs's `wait = (fn, msg) =>
// page.waitForFunction(fn, ...)`), through every call of that function in the
// file, where the argument in that place must pass the same test. A name the
// file never writes (an import, a loop variable, a method's parameter), a
// parameter of an unnamed function, a wrapper the file never calls, and a
// call this reader cannot cut into its arguments are refused rather than
// trusted: the scan cannot see what they hold. What it
// cannot follow at all is a value a written name gets some other way (a
// ternary's arm, a call's return): `const p = ok ? f : "s"` passes it.

// A small reader of JavaScript: where each bracket closes and where each
// bracket's top-level commas sit, past strings, template literals, comments
// and regex literals. Enough to cut a call into its arguments; not a parser.
const readJsMemo = new Map();
function readJs(src) {
  if (readJsMemo.has(src)) return readJsMemo.get(src);
  const pairs = new Map();             // open index → close index
  const commas = new Map([[-1, []]]);  // open index (-1: top level) → its commas
  const semis = new Map([[-1, []]]);   // same, for semicolons
  const stack = [];                    // [kind, index]; kind "${" is a template substitution
  const REGEX_AFTER = "(,=:[!&|?{};+-*%<>~^";
  const REGEX_WORDS = new Set(["return", "typeof", "case", "void", "yield", "await", "in", "of", "delete", "throw", "else", "do"]);
  let prev = "", word = "";
  const template = (j) => {            // template text from j; the index code resumes at
    for (; j < src.length; j++) {
      if (src[j] === "\\") j++;
      else if (src[j] === "`") return j + 1;
      else if (src[j] === "$" && src[j + 1] === "{") { stack.push(["${", j + 1]); return j + 2; }
    }
    return src.length;
  };
  const quoted = (j, q) => {
    for (j++; j < src.length; j++) {
      if (src[j] === "\\") j++;
      else if (src[j] === q || src[j] === "\n") return j + 1;
    }
    return src.length;
  };
  const regex = (j) => {               // past the regex literal at j, or j when there is none
    let cls = false;
    for (let k = j + 1; k < src.length; k++) {
      const d = src[k];
      if (d === "\\") k++;
      else if (d === "\n") return j;
      else if (cls) { if (d === "]") cls = false; }
      else if (d === "[") cls = true;
      else if (d === "/") { k++; while (/[a-z]/i.test(src[k] || "")) k++; return k; }
    }
    return j;
  };
  const top = () => (stack.length ? stack[stack.length - 1][1] : -1);
  for (let i = 0; i < src.length;) {
    const c = src[i], n = src[i + 1];
    if (c === "/" && n === "/") { const e = src.indexOf("\n", i); i = e < 0 ? src.length : e; continue; }
    if (c === "/" && n === "*") { const e = src.indexOf("*/", i + 2); i = e < 0 ? src.length : e + 2; continue; }
    if (c === '"' || c === "'") { i = quoted(i, c); prev = c; word = ""; continue; }
    if (c === "`") { i = template(i + 1); prev = "`"; word = ""; continue; }
    if (c === "/" && (prev === "" || REGEX_AFTER.includes(prev) || REGEX_WORDS.has(word))) {
      const e = regex(i);
      if (e > i) { i = e; prev = "x"; word = ""; continue; }
    }
    if (c === "(" || c === "[" || c === "{") {
      stack.push([c, i]);
      commas.set(i, []);
      semis.set(i, []);
    } else if (c === ")" || c === "]" || c === "}") {
      const t = stack.pop();
      if (t && t[0] === "${") { i = template(i + 1); prev = "`"; word = ""; continue; }
      if (t) pairs.set(t[1], i);
    } else if (c === ",") commas.get(top()).push(i);
    else if (c === ";") semis.get(top()).push(i);
    if (/[\w$]/.test(c)) word = (/[\w$]/.test(src[i - 1] || "") ? word : "") + c;
    else if (!/\s/.test(c)) word = "";
    if (!/\s/.test(c)) prev = c;
    i++;
  }
  const read = { pairs, commas, semis };
  readJsMemo.set(src, read);
  return read;
}

const IDENT = String.raw`[A-Za-z_$][\w$]*`;
const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const lineOf = (src, at) => src.slice(0, at).split("\n").length;
// leading whitespace and comments off, trailing whitespace off
const trivia = (s) => s.replace(/^(?:\s|\/\/[^\n]*(?:\n|$)|\/\*[\s\S]*?\*\/)+/, "").trimEnd();

// The call's arguments, as source text, given the index of its "(".
function argsAt(src, open) {
  const { pairs, commas } = readJs(src);
  const close = pairs.get(open);
  if (close === undefined) return null;
  const cuts = [open, ...commas.get(open), close];
  const args = cuts.slice(1).map((to, k) => trivia(src.slice(cuts[k] + 1, to)));
  if (args.length && args[args.length - 1] === "") args.pop();   // f(a, b,) and f()
  return args;
}

// Every function in src: { name, params, from, to }. name is null for an
// unnamed one (a callback); params holds each plain parameter's name in its
// place (null for a destructured one); [from, to] spans parameters and body.
const functionsMemo = new Map();
function functionsIn(src) {
  if (functionsMemo.has(src)) return functionsMemo.get(src);
  const { pairs, commas, semis } = readJs(src);
  const out = [];
  const params = (open) => argsAt(src, open).map((p) => {
    const m = new RegExp(`^(?:\\.\\.\\.)?(${IDENT})\\s*(?:=|$)`).exec(p);
    return m ? m[1] : null;
  });
  const nameBefore = (at) => {
    const m = new RegExp(`(?:^|[^\\w$.])(${IDENT})\\s*[:=]\\s*(?:async\\s+)?$`).exec(src.slice(Math.max(0, at - 160), at));
    return m ? m[1] : null;
  };
  const enclosing = (at) => {
    let best = -1;
    for (const [o, c] of pairs) if (o < at && at < c && o > best) best = o;
    return best;
  };
  // an arrow's body: a block, or an expression to the next comma or
  // semicolon of the bracket around the arrow (or that bracket's close)
  const bodyEnd = (from, arrowAt) => {
    const b = /^\s*/.exec(src.slice(from))[0].length + from;
    if (src[b] === "{" && pairs.has(b)) return pairs.get(b);
    const o = enclosing(arrowAt);
    const ends = [...commas.get(o), ...semis.get(o)].filter((x) => x > from);
    return Math.min(o >= 0 ? pairs.get(o) : src.length, ...ends);
  };
  for (const [open] of pairs) {
    if (src[open] !== "(") continue;
    const close = pairs.get(open);
    const after = src.slice(close + 1);
    const arrow = /^\s*=>/.exec(after);
    const before = src.slice(Math.max(0, open - 160), open);
    const fnKw = new RegExp(`(?:^|[^\\w$.])function\\s*\\*?\\s*(${IDENT})?\\s*$`).exec(before);
    if (arrow) {
      const asyncAt = /async\s*$/.exec(before);
      const start = asyncAt ? open - asyncAt[0].length : open;
      out.push({ name: nameBefore(start), params: params(open), from: open, to: bodyEnd(close + 1 + arrow[0].length, open) });
    } else if (fnKw) {
      const brace = close + 1 + /^\s*/.exec(after)[0].length;
      if (src[brace] !== "{" || !pairs.has(brace)) continue;
      const start = open - (before.length - fnKw.index) + (fnKw[0].match(/^[^\w$]/) ? 1 : 0);
      const asyncAt = /async\s+$/.exec(src.slice(Math.max(0, start - 16), start));
      const name = fnKw[1] || nameBefore(asyncAt ? start - asyncAt[0].length : start);
      out.push({ name, params: params(open), from: open, to: pairs.get(brace) });
    }
  }
  // the one-parameter arrow without parentheses: x => ...
  for (const m of src.matchAll(new RegExp(`(?<![\\w$.])(${IDENT})\\s*=>`, "g"))) {
    const asyncAt = /async\s+$/.exec(src.slice(Math.max(0, m.index - 16), m.index));
    out.push({ name: nameBefore(asyncAt ? m.index - asyncAt[0].length : m.index), params: [m[1]], from: m.index,
      to: bodyEnd(m.index + m[0].length, m.index) });
  }
  functionsMemo.set(src, out);
  return out;
}

const NOT_FN = "something other than a function";

// What the predicate expr (written at index at) may be other than a function,
// as a phrase ("something other than a function", "fn, which ..."), or null.
function predicateProblem(src, expr, at, depth) {
  const e = trivia(expr);
  if (depth > 8) return `${e.slice(0, 30)}, followed further than the scan goes`;
  if (new RegExp(`^(?:async\\s+)?(?:function\\b|${IDENT}\\s*=>)`).test(e)) return null;
  const lead = /^(?:async\s*)?\(/.exec(e);
  if (lead) {
    const open = lead[0].length - 1;
    const close = readJs(e).pairs.get(open);
    if (close !== undefined && /^\s*=>/.test(e.slice(close + 1))) return null;
    // (expr): the parentheses change nothing; look inside
    if (open === 0 && close === e.length - 1) return predicateProblem(src, e.slice(1, close), at, depth + 1);
    return NOT_FN;
  }
  if (new RegExp(`^${IDENT}(?:\\.${IDENT})*$`).test(e)) return nameProblem(src, e, at, depth + 1);
  return NOT_FN;
}

// What the name (used at index at) may hold other than a function, as a
// phrase that starts with the name, or null when everything the file gives it
// is a function or another name that passes in turn.
function nameProblem(src, name, at, depth) {
  const dotted = name.includes(".");
  const last = name.split(".").pop();
  // `last = v` (not ==, ===, =>) and `last: v` (not ::); a dotted name's
  // field may be written through any object (t.ready = …, { ready: … })
  const give = new RegExp(String.raw`(?:^|[^\w$${dotted ? "" : "."}])${esc(last)}\s*(?::(?!:)|=(?![=>]))`, "gm");
  let given = 0;
  for (const m of src.matchAll(give)) {
    given++;
    const v = trivia(src.slice(m.index + m[0].length));
    if (/^\(*\s*["'`]/.test(v)) return `${name}, which line ${lineOf(src, m.index)} gives a string`;
    const other = new RegExp(`^\\(*\\s*(${IDENT}(?:\\.${IDENT})*)\\s*\\)*\\s*(?:[,;})\\n]|$)`).exec(v);
    if (other && other[1] !== name && !["null", "undefined", "true", "false"].includes(other[1])) {
      const p = nameProblem(src, other[1], m.index, depth + 1);
      if (p) return `${name}, which line ${lineOf(src, m.index)} gives ${p}`;
    }
  }
  if (dotted) return given ? null : `${name}, whose ${last} nothing in this file writes: the scan cannot see what it holds`;
  const fn = functionsIn(src).filter((f) => f.from < at && at <= f.to && f.params.includes(name))
    .sort((a, b) => b.from - a.from)[0];
  if (!fn) return given ? null : `${name}, which nothing in this file writes (an import, a loop variable, a method's parameter?): the scan cannot see what it holds`;
  if (!fn.name) return `${name}, a parameter of an unnamed function, whose arguments the scan cannot follow`;
  const k = fn.params.indexOf(name);
  let calls = 0;
  for (const c of src.matchAll(new RegExp(String.raw`(?<![\w$.])${esc(fn.name)}\s*\(`, "g"))) {
    if (/\bfunction\s*\*?\s*$/.test(src.slice(Math.max(0, c.index - 16), c.index))) continue;   // its own definition
    calls++;
    const open = c.index + c[0].length - 1;
    const args = argsAt(src, open);
    if (!args) return `${name}, which ${fn.name}(…) at line ${lineOf(src, c.index)} hands what the scan cannot read`;
    if (args.length <= k) continue;   // nothing in that place: undefined, not a string
    const p = predicateProblem(src, args[k], open, depth + 1);
    if (p) return `${name}, which ${fn.name}(…) at line ${lineOf(src, c.index)} hands ${p}`;
  }
  if (!calls) return `${name}, a parameter of ${fn.name}, which this file never calls: the scan cannot see what reaches it`;
  return null;
}

// Every waitForFunction call in src, and what is wrong with each one's predicate.
function stringPredicates(src, file) {
  const bad = [];
  let calls = 0;
  for (const m of src.matchAll(/\bwaitForFunction\s*\(/g)) {
    calls++;
    const open = m.index + m[0].length - 1;
    const args = argsAt(src, open);
    const first = args && args.length ? args[0] : "";
    const p = first ? predicateProblem(src, first, open, 0) : "no predicate the scan can read";
    if (p) bad.push(`${file}:${lineOf(src, m.index)}: waitForFunction(${first.slice(0, 40).split("\n")[0]}…) is handed ${p}`);
  }
  return { calls, bad };
}

test("no probe waits on a string: every waitForFunction predicate is a function", () => {
  const bad = [];
  let calls = 0;
  // every probe and test beside this file (which names the call in its prose)
  for (const f of readdirSync(__dirname).filter((x) => /\.m?js$/.test(x) && x !== "csp.test.js").sort()) {
    const found = stringPredicates(read(join(__dirname, f)), f);
    calls += found.calls;
    bad.push(...found.bad);
  }
  assert.ok(calls > 0, "no waitForFunction call found: the scan is looking in the wrong place");
  assert.deepStrictEqual(bad, [], "a string predicate is eval'd by the page on every poll, and the policy refuses it");
});

// The scan itself, on the ways a string has reached (or could reach) the wait,
// and on the function forms the probes use, which it must leave alone.
test("the string-predicate scan follows a string through parentheses, names and wrappers", () => {
  const refused = {
    "boot_probe.mjs before A44": `await page.waitForFunction("window.__ready === true", null, { timeout: 90000 });`,
    "a template literal": "await page.waitForFunction(`window.__ready === ${want}`);",
    "a parenthesized string": `await page.waitForFunction(("window.__ready === true"), null, { timeout: 90000 });`,
    "parentheses twice": `await page.waitForFunction(((  "window.__ready"  )));`,
    "a concatenation": `await page.waitForFunction("window." + field);`,
    "a named string": `const READY = "window.__ready === true";\nawait page.waitForFunction(READY);`,
    "a named, parenthesized string": `const READY = ("window.__ready === true");\nawait page.waitForFunction(READY, null, {});`,
    "a name given a name given a string": `const A = "window.__ready";\nconst B = A;\nawait page.waitForFunction(B);`,
    "csp_probe.mjs before A44 (a table's field)":
      `targets.push({ name: "h", ready: "window.__ready === true", settle: 1000 });\n` +
      `for (const t of targets) if (t.ready) await page.waitForFunction(t.ready, null, { timeout: 90000 });`,
    "a field written through an object": `targets.push({ ready: () => true });\nt.ready = "window.__ready";\n` +
      `await page.waitForFunction(t.ready);`,
    "a one-line wrapper (W1)": `const until = (pred) => page.waitForFunction(pred, null, { timeout: 90000 });\n` +
      `await until("window.__ready === true");`,
    "operator_probe.mjs's wait, handed a string (M-C)":
      `const wait = (fn, msg) => page.waitForFunction(fn, null, { timeout: 3000 }).catch(() => fail(msg));\n` +
      `await wait(() => /live · 2-of-3/.test(document.querySelector(".op-badge")?.textContent || ""), "no live badge");\n` +
      `await wait("/DRILL PASSED/.test(document.querySelector('.op-term')?.textContent || '')", "drill never passed");`,
    "a wrapper handed a parenthesized string": `const wait = (fn, msg) => page.waitForFunction(fn);\nawait wait(("x"), "m");`,
    "a function declaration, second place": `async function waitFor(page, pred) {\n  await page.waitForFunction(pred);\n}\n` +
      `await waitFor(page, () => true);\nawait waitFor(page, "window.__ready");`,
    "a block-bodied async wrapper": `const settle = async (pred, ms) => {\n  await page.waitForFunction(pred, null, { timeout: ms });\n};\n` +
      "await settle(`document.title === \"x\"`, 1000);",
    "a wrapper of a wrapper": `const wait = (fn) => page.waitForFunction(fn);\nconst until = (p, why) => wait(p);\n` +
      `await until(() => true, "a");\nawait until("window.__ready", "b");`,
    "a wrapper's parameter given a string default": `const wait = (fn = "window.__ready") => page.waitForFunction(fn);\nawait wait();`,
    "an unnamed callback's parameter": `preds.forEach((p) => page.waitForFunction(p));`,
    "a one-parameter arrow callback": `preds.forEach(p => page.waitForFunction(p));`,
    "an import": `import { READY } from "./ready.mjs";\nawait page.waitForFunction(READY);`,
    "a loop variable": `for (const pred of PREDS) await page.waitForFunction(pred);`,
    "a wrapper nobody here calls": `export const wait = (fn) => page.waitForFunction(fn);`,
    "a wrapper call the reader cannot cut": `const wait = (fn) => page.waitForFunction(fn);\nawait wait(() => true, "unclosed"`,
    "no predicate at all": `await page.waitForFunction();`,
  };
  for (const [what, src] of Object.entries(refused)) {
    const { calls, bad } = stringPredicates(src, "fixture.mjs");
    assert.ok(calls > 0, `${what}: the scan saw no call`);
    assert.strictEqual(bad.length, 1, `${what}: the scan must refuse it, once; it said ${JSON.stringify(bad)}`);
  }
  const passed = {
    "boot_probe.mjs now": `await page.waitForFunction(() => window.__ready === true, null, { timeout: 90000 });`,
    "an argument": `await page.waitForFunction((n) => document.querySelectorAll(".pin-flag").length > n, 3, { timeout: 8000 });`,
    "async, function, bare arrow": `await page.waitForFunction(async () => true);\n` +
      `await page.waitForFunction(function () { return true; });\nawait page.waitForFunction(x => !x);`,
    "a parenthesized function": `await page.waitForFunction((() => window.__ready === true));`,
    "a regex literal holding a quote": `await page.waitForFunction(() => /'/.test(document.title), null, { timeout: 1000 });`,
    "a template whose substitution holds a template":
      `const wait = (fn, msg) => page.waitForFunction(fn).catch(() => fail(msg));\n` +
      "await wait(() => true, `${late ? `(` : \"\"} late`);",
    "a named function": `const READY = () => window.__ready === true;\nawait page.waitForFunction(READY);`,
    "csp_probe.mjs now (a table's field)":
      `targets.push({ name: "h", ready: () => window.__ready === true, settle: 1000 });\n` +
      `for (const t of targets) if (t.ready) await page.waitForFunction(t.ready, null, { timeout: 90000 });`,
    "operator_probe.mjs now, with every awkward literal":
      `const wait = (fn, msg) => page.waitForFunction(fn, null, { timeout: 3000 }).catch(() => fail(msg));\n` +
      `await wait(() => /"\\(,'/.test(document.querySelector(".op-term")?.textContent || ""), "a message, with a comma");\n` +
      "await wait(() => /[)\"]/.test(document.title), `took ${elapsed(1, 2)} ms, (or \"more\")`);\n" +
      `await wait(function () { return 1 / 2 > 0; }, 'it\\'s fine');`,
    "a wrapper of a wrapper, functions all the way": `const wait = (fn) => page.waitForFunction(fn);\n` +
      `const until = (p, why) => wait(p);\nawait until(() => true, "a string message is not the predicate");`,
    "a function declaration": `async function waitFor(page, pred) {\n  await page.waitForFunction(pred);\n}\n` +
      `await waitFor(page, () => true, "x");`,
  };
  for (const [what, src] of Object.entries(passed)) {
    const { calls, bad } = stringPredicates(src, "fixture.mjs");
    assert.ok(calls > 0, `${what}: the scan saw no call`);
    assert.deepStrictEqual(bad, [], `${what}: a function predicate, refused`);
  }
});

// ── CI wiring: this gate cannot be silently dropped ─────────────────────────

test("CI runs this gate, the generator's --check, and the browser probe", () => {
  const workflow = read(join(REPO, ".github/workflows/canary-local.yml"));
  for (const needle of [
    "node --test canary-local/tests/csp.test.js",
    "python3 canary-local/tools/gen_csp.py --check",
    "node canary-local/tests/csp_probe.mjs",
  ]) {
    assert.ok(workflow.includes(needle), `${needle} is not wired into canary-local.yml`);
  }
});
