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
//   · the firmware's captive page (a srcdoc frame, which    → "wap.html's style-src pins"
//     inherits the policy) changes and its hash goes stale
//   · a module creates a <style> element (inline style the  → "no module writes style="
//     load-time probe never sees — fleet.html's toggle did)
//   · a module writes a style= attribute the probe never  → "no module writes style="
//     happens to exercise
//   · flash.html's hand-written policy quietly widens     → "flash.html is no weaker"
//   · a page needs a source the desktop app would block   → "desktop Lab agrees"
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

test("wap.html's style-src pins the firmware's captive page (a srcdoc frame) — and nothing else does", () => {
  // wap-ui.js shows the device's real captive-portal HTML in an <iframe
  // srcdoc>; a srcdoc document inherits the embedder's policy, so its one
  // <style> block is hashed from devices/wap.json (gen_wap.py writes it from
  // the firmware source — the pin follows the firmware). No hash can cover a
  // style= attribute, an on*= handler or an inline <script> there, so that
  // document must carry none.
  const captive = JSON.parse(read(join(ROOT, "devices/wap.json"))).captive.html;
  const blocks = [...captive.matchAll(/<style\b[^>]*>([\s\S]*?)<\/style\b[^>]*>/gi)].map((m) => m[1]);
  assert.strictEqual(blocks.length, 1, "the captive page carries one <style> block");
  assert.deepStrictEqual(parse(policyOf("wap.html")).get("style-src"), ["'self'", sha256(blocks[0])],
    "wap.html style-src must be 'self' plus the captive page's <style> hash (rerun gen_csp.py after gen_wap.py)");
  for (const tag of markupOf(captive).match(/<[a-zA-Z][^>]*>/g) || []) {
    assert.doesNotMatch(tag, /\s(?:on[a-z]+|style)\s*=/i, `captive page: ${tag.slice(0, 80)} — no hash can allow it`);
  }
  assert.doesNotMatch(captive, /<script\b(?![^>]*\ssrc=)/i, "the captive page has no inline script");
  // Every other page's style-src is exactly 'self'.
  for (const page of PAGES) {
    if (page === "wap.html") continue;
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
