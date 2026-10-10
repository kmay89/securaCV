// Drift-gate: the Lab frontend may only reach OUTSIDE its own directory for
// things the native app actually bundles.
//
// The bug this exists to prevent, in full: `canary-local/` is one frontend with
// two very different roots under it. Served from a repo checkout (or the
// deployed site, which mirrors the checkout) it has a parent, so a URL like
// `../docs/hardware/enclosure/preview_dev_station.png` resolves to the real
// enclosure library. Inside the native Lab, Tauri embeds the staged web root
// and serves it as the WHOLE origin — the webview collapses `../` at the root,
// asks for `/docs/hardware/enclosure/…`, finds nothing, and 404s. The failure
// is silent in the worst way: a broken-image glyph where a package render
// belongs and an empty 3D viewport, with the few meshes committed under
// `canary-local/enclosures/` still working, so the page looks alive.
//
// The fix is packaging — desktop-lab/scripts/stage-frontend.mjs mirrors the
// sibling directories listed in desktop-lab/frontend-stage.json next to
// canary-local, so both roots have the same shape. This test is the other half:
// it fails the moment the frontend reaches for a sibling the manifest does not
// carry, instead of letting the app ship half a workshop.
//
// Runs under "page logic tests" (enumerated in .github/workflows/canary-local.yml,
// whose paths: filters include desktop-lab/** so an app-side edit trips it too).
// Reads source text only — no Rust, no Node toolchain beyond the test runner.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync, existsSync, statSync } = require("node:fs");
const { join, posix, relative, sep } = require("node:path");

const CANARY = join(__dirname, "..");          // canary-local/
const ROOT = join(CANARY, "..");               // repo root
const APP = join(ROOT, "desktop-lab");
const read = (p) => readFileSync(p, "utf8");

const manifest = JSON.parse(read(join(APP, "frontend-stage.json")));
const tauri = JSON.parse(read(join(APP, "src-tauri/tauri.conf.json")));

// Every file the app would ship that a page could fetch by URL. third_party,
// .build and tests are pruned from the bundle (frontend-stage.json, the same
// trees pages.yml deletes from the site). tools/ DOES ship — assets/hatchery.js
// imports tools/hatchery/derive.mjs at runtime — but its other .mjs files are
// Node generators whose "../" paths are filesystem reads in the repo, not URLs
// a page resolves, so they would only be noise here; the module the page does
// load is covered by the "nothing a page loads is pruned" test below.
const SKIP = new Set(["node_modules", "third_party", ".build", "tests", "tools"]);
function walk(dir, out = []) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (SKIP.has(entry.name)) continue;
    const p = join(dir, entry.name);
    if (entry.isDirectory()) walk(p, out);
    else if (/\.(js|mjs|html|css|json)$/.test(entry.name)) out.push(p);
  }
  return out;
}

// A relative URL in a *string literal* is resolved by the browser against the
// DOCUMENT, not the file it was written in — so a path in assets/*.js, or an
// href in a data file like build-line.json, is relative to the page that loads
// it, and every Lab page sits at canary-local/'s top level. Only markup
// resolves against its own location.
// The one script that is NOT loaded by a top-level page: emulator/web/harness.js
// belongs to emulator/web/harness.html alone (lifted out of it so the harness
// carries the same no-inline-script CSP as the product pages), so its bundle
// paths ("../dist/…") resolve against that page's directory, one level down.
const OWN_PAGE_DIR = { "emulator/web/harness.js": "canary-local/emulator/web" };
const docBase = (file) => {
  const rel = relative(CANARY, file).split(sep).join("/");
  const isDocument = /\.html?$/.test(rel);
  if (isDocument) return posix.dirname("canary-local/" + rel);
  return OWN_PAGE_DIR[rel] || "canary-local";
};

// Module specifiers are the exception: `import … from "../emulator/…"` resolves
// against the MODULE's own URL and stays inside canary-local. Drop those lines
// before looking for escaping URLs.
const isModuleSpecifier = (line) =>
  /^\s*(?:import|export)\b/.test(line) || /\bfrom\s*["'`]/.test(line) || /\bimport\s*\(/.test(line);

// Prose that *discusses* a path is not a path. Blank out comments (keeping line
// numbers intact) so a note like "…don't write ../docs/… here" can't fail the
// build. `//` is only a comment when it isn't the slashes in `https://`.
const stripComments = (src) =>
  src
    .replace(/\/\*[\s\S]*?\*\/|<!--[\s\S]*?-->/g, (m) => m.replace(/[^\n]/g, " "))
    .split("\n")
    .map((line) => line.replace(/(^|[^:"'`\w])\/\/.*$/, "$1"))
    .join("\n");

const resolveRef = (base, ref) =>
  posix.normalize(posix.join(base, ref)).replace(/\/+$/, "");

// A URL in a data file reaches the DOM exactly as written — build-line.json's
// hrefs become the site map's anchors — so it escapes the bundle just as surely
// as one typed into a .js file. Walk the parsed values rather than the text: a
// VALUE that is a path is a URL, while a path mentioned inside a sentence is
// prose (the enclosure catalog's `for` blurbs quote markdown links from the
// library README, and the Lab renders those as textContent, never as links).
function jsonRefs(file) {
  const base = docBase(file);
  const found = [];
  const walk = (node, path) => {
    if (Array.isArray(node)) node.forEach((v, i) => walk(v, `${path}[${i}]`));
    else if (node && typeof node === "object")
      for (const [k, v] of Object.entries(node)) walk(v, `${path}.${k}`);
    else if (typeof node === "string" && node.startsWith("../"))
      found.push({
        ref: node,
        resolved: resolveRef(base, node),
        where: `${relative(ROOT, file)} ${path}`,
      });
  };
  walk(JSON.parse(read(file)), "");
  return found;
}

function escapingRefs(file) {
  if (file.endsWith(".json")) return jsonRefs(file);
  const base = docBase(file);
  const found = [];
  stripComments(read(file))
    .split("\n")
    .forEach((line, i) => {
      if (isModuleSpecifier(line)) return;
      for (const m of line.matchAll(/["'`(](\.\.\/[^"'`)\s]*)["'`)]/g)) {
        found.push({
          ref: m[1],
          resolved: resolveRef(base, m[1]),
          where: `${relative(ROOT, file)}:${i + 1}`,
        });
      }
    });
  return found;
}

const mirrored = (p) =>
  manifest.mirror.some((m) => p === m || p.startsWith(m + "/"));

test("every path the frontend reaches outside canary-local is bundled with the app", () => {
  const files = walk(CANARY);
  // Data files carry URLs too — build-line.json's hrefs are the site map's
  // anchors. If the sweep stops covering them, the gate silently narrows.
  assert.ok(
    files.some((f) => f.endsWith("build-line.json")),
    "the sweep no longer visits JSON manifests — URLs defined as data would escape it",
  );
  const escapes = files.flatMap(escapingRefs);
  // Not "there are none" — the enclosure library legitimately lives next door.
  // The invariant is that each one is mirrored into the app's web root.
  for (const e of escapes) {
    assert.ok(
      mirrored(e.resolved),
      `${e.where} points at "${e.ref}" → ${e.resolved || "the repo root"}, which ` +
        `desktop-lab/frontend-stage.json does not mirror. In the native Lab that ` +
        `URL 404s (the bundle has no parent directory). Either link it at its ` +
        `source (const GH = "https://github.com/kmay89/securaCV/blob/main/"), or ` +
        `add its directory to the manifest's "mirror" list.`,
    );
  }
  // The workshop's enclosure base is the reference case — if the scan ever
  // stops seeing it, the scan broke, not the frontend.
  assert.ok(
    escapes.some((e) => e.resolved === "docs/hardware/enclosure"),
    "scan found no reference to docs/hardware/enclosure — the detector is broken",
  );
});

test("the staging manifest names things that exist", () => {
  for (const rel of [...manifest.mirror, ...manifest.sentinels, manifest.entry]) {
    assert.ok(existsSync(join(ROOT, rel)), `frontend-stage.json names missing ${rel}`);
  }
  for (const rel of manifest.sentinels) {
    assert.ok(statSync(join(ROOT, rel)).size > 0, `sentinel ${rel} is empty`);
  }
});

test("the app builds from the staged root, not from canary-local directly", () => {
  // Pointing frontendDist back at ../../canary-local is exactly the regression
  // that shipped a workshop with no renders: it makes canary-local the origin,
  // and every sibling path escapes it again.
  assert.equal(tauri.build.frontendDist, "../dist");
  for (const hook of ["beforeDevCommand", "beforeBuildCommand"]) {
    assert.match(
      tauri.build[hook] || "",
      /stage-frontend\.mjs/,
      `tauri.conf.json ${hook} must stage the web root, or dist/ is stale or absent`,
    );
  }
  assert.equal(tauri.app.windows[0].url, manifest.entry);
});

// ── what the bundle leaves out, and that it is the site's list ───────────────
// The site deploy (pages.yml, "Assemble site") deletes the page-logic tests and
// every .py/.sh under canary-local after the copy — "publishing them only put
// the hub host-provision script and two dozen generators on a public URL". The
// app's web root is the same tree by design (frontend-stage.json `_why`), but
// its prune list held only the two emulator build trees, so every macOS, Linux
// and iPad bundle carried ~100 test files and 34 scripts, the host-provision
// script among them. Parsing pages.yml keeps the two lists one list.
function pagesAssembleStep() {
  const yml = read(join(ROOT, ".github/workflows/pages.yml"));
  const start = yml.indexOf("- name: Assemble site");
  assert.ok(start >= 0, "pages.yml lost its \"Assemble site\" step — this gate reads it");
  const rest = yml.slice(start + 1);
  const end = rest.search(/\n\s*- (name|uses):/);
  return end < 0 ? rest : rest.slice(0, end);
}

test("the app prunes every tree the site deploy deletes from canary-local", () => {
  const step = pagesAssembleStep();
  const removed = [...step.matchAll(/^\s*rm -rf (.+)$/gm)]
    .flatMap((m) => m[1].trim().split(/\s+/))
    .filter((p) => p.startsWith("_site/canary-local/"))
    .map((p) => p.slice("_site/".length).replace(/\/+$/, ""));
  assert.ok(removed.includes("canary-local/tests"),
    `pages.yml's rm -rf lines changed shape (found ${JSON.stringify(removed)}) — this gate reads them`);
  for (const rel of removed) {
    assert.ok(manifest.prune.includes(rel),
      `pages.yml deletes ${rel} from the site, but desktop-lab/frontend-stage.json ` +
        `does not prune it, so the app would ship what the site stopped publishing`);
  }
});

test("the app drops the same file types the site deploy deletes", () => {
  const step = pagesAssembleStep();
  const finds = [...step.matchAll(/^\s*find _site\/(\S+) -type f (.+) -delete\s*$/gm)];
  assert.ok(finds.length > 0, "pages.yml lost its find … -delete line — this gate reads it");
  for (const [, dir, expr] of finds) {
    const exts = [...expr.matchAll(/-name '\*(\.[A-Za-z0-9]+)'/g)].map((m) => m[1]).sort();
    assert.ok(exts.length > 0, `could not read the -name patterns in: find _site/${dir} ${expr}`);
    const ours = ((manifest.pruneExt || {})[dir.replace(/\/+$/, "")] || []).slice().sort();
    assert.deepStrictEqual(ours, exts,
      `pages.yml deletes ${exts.join(", ")} under ${dir}; frontend-stage.json pruneExt ` +
        `must name the same types for it, or the app ships what the site does not`);
  }
  const staged = read(join(APP, "scripts/stage-frontend.mjs"));
  assert.match(staged, /manifest\.pruneExt/,
    "stage-frontend.mjs must act on pruneExt — a manifest key nothing reads prunes nothing");
});

test("nothing a page loads is pruned from the bundle", () => {
  // The reason tools/ is not pruned wholesale: a page imports a module from it.
  // Every module specifier and src/href in the shipped frontend must survive
  // the prunes, or the app breaks where the site would not.
  const exts = Object.entries(manifest.pruneExt || {});
  const pruned = (p) =>
    manifest.prune.some((d) => p === d || p.startsWith(d + "/")) ||
    exts.some(([d, xs]) => p.startsWith(d + "/") && xs.some((x) => p.endsWith(x)));
  const files = walk(CANARY);
  let imports = 0;
  for (const f of files) {
    if (f.endsWith(".json")) continue;
    const dir = posix.dirname("canary-local/" + relative(CANARY, f).split(sep).join("/"));
    for (const m of stripComments(read(f)).matchAll(/(?:\bfrom\s*|\bimport\s*\(\s*|\bsrc=)["'](\.{1,2}\/[^"']+)["']/g)) {
      const target = resolveRef(dir, m[1]);
      if (target.startsWith("canary-local/tools/")) imports += 1;
      assert.ok(!pruned(target),
        `${relative(ROOT, f)} loads ${m[1]} (${target}), which frontend-stage.json prunes — ` +
          `the app would fail to load it`);
    }
  }
  assert.ok(imports > 0, "found no page import from tools/ — hatchery.js's derive.mjs import moved, re-check this gate");
  assert.ok(manifest.sentinels.includes("canary-local/tools/hatchery/derive.mjs"),
    "the staged root must prove tools/hatchery/derive.mjs survived the prunes (a sentinel)");
});

// ── the webview's IPC surface is exactly what the Lab's pages use ────────────
// desktop-lab registered four commands no Lab page ever invoked — app_version
// (app_info's older twin), list_serial_ports (list_ports under another name),
// companion_snapshot (the tray is the companion's only display) and
// serial_monitor_send (the native monitor has no command box). Each one was
// reachable over IPC from the webview while serving no feature. Both
// directions are held here: a registered command has a caller in canary-local,
// and an invoke() in canary-local has a registered command, or the page's call
// rejects at runtime with nothing in CI to say so. (The Flasher's commands are
// held to the Lab's wrappers by desktop_parity.test.js; this is the Lab alone.)
test("every command the Lab app registers is invoked by a Lab page, and vice versa", () => {
  const libRs = read(join(APP, "src-tauri/src/lib.rs"));
  const handlers = [...libRs.matchAll(/invoke_handler\(tauri::generate_handler!\[([\s\S]*?)\]\)/g)].map((m) => m[1]);
  assert.strictEqual(handlers.length, 2, "expected a desktop and a non-desktop invoke_handler in desktop-lab lib.rs");
  const registered = new Set(handlers.flatMap((h) =>
    h.split(",").map((x) => x.trim().split("::").pop()).filter(Boolean)));
  assert.ok(registered.has("app_info") && registered.has("flash"), "the handler parse found the wrong list");

  const invoked = new Map(); // command -> first file that invokes it
  const scan = (dir) => {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      if (["node_modules", "third_party", ".build", "tests", "dist"].includes(entry.name)) continue;
      const p = join(dir, entry.name);
      if (entry.isDirectory()) scan(p);
      else if (/\.(m?js|html)$/.test(entry.name)) {
        for (const m of stripComments(read(p)).matchAll(/\binvoke\(\s*["']([a-z_]+)["']/g)) {
          if (!invoked.has(m[1])) invoked.set(m[1], relative(ROOT, p));
        }
      }
    }
  };
  scan(CANARY);
  assert.ok(invoked.has("native_capabilities"), "found no invoke() in canary-local — the scan broke, not the app");

  const uncalled = [...registered].filter((c) => !invoked.has(c)).sort();
  assert.deepStrictEqual(uncalled, [],
    "desktop-lab registers commands no Lab page invokes — each is IPC surface serving " +
      "no feature. Remove it from generate_handler! (and its fn), or wire the page that needs it");
  const unregistered = [...invoked].filter(([c]) => !registered.has(c)).map(([c, f]) => `${c} (${f})`).sort();
  assert.deepStrictEqual(unregistered, [],
    "a Lab page invokes a command desktop-lab never registers — that call rejects in the app");
});
