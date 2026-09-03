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

// every file the app would ship, minus the ones the staging step prunes and the
// test/tooling trees that never reach a browser
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
