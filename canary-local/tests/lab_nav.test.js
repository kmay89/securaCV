// Parity gate for the Lab's shared navigation (assets/lab-nav.js).
//
// The rot this exists to prevent, in full: every bench page used to hand-write
// its own "still on SecuraCV" bar and its own back-links, and they drifted —
// fleet.html ended up with no path back to the Lab at all, and inside the
// native app (which has no browser chrome, so no Back button) an external
// link in that bar navigated the webview clean off the bundled Lab and
// stranded the user. The fix is ONE module, rendered from build-line.json —
// the same manifest the shell, the isometric room and the sitemap render
// from — upgrading ONE canonical static bar. This test pins every half of
// that contract so a new page can't quietly opt out of it:
//
//   1. every root Lab page that carries the static bar carries it
//      byte-identical (room.html's target="_top" variant is the one
//      documented exception — it is iframed by the shell);
//   2. every page the manifest reaches (onramp, benches, folded depths)
//      exists on disk and includes the shared module;
//   3. every bar-carrying page includes the module (index.html excepted:
//      it meta-redirects before any module could run);
//   4. every manifest page's <main> declares a width cap in CSS it links, or
//      is allowlisted here with the container that caps it instead — the
//      "hub-section content mounted into an uncapped <main>" bug class hit
//      five pages at once (start, vault, vision, operator, wap): text
//      against the window edge, card grids clipping offscreen;
//   5. the browser tagline in the static bar and the app tagline in the
//      module stay a matched pair, so copy edits move both surfaces.
//
// Runs under "page logic tests" (.github/workflows/canary-local.yml). Reads
// source text only — no browser, no toolchain beyond the test runner.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const CANARY = join(__dirname, "..");
const read = (p) => readFileSync(p, "utf8");

const manifest = JSON.parse(read(join(CANARY, "build-line.json")));
const navJs = read(join(CANARY, "assets/lab-nav.js"));

const rootPages = readdirSync(CANARY).filter((f) => f.endsWith(".html"));
const barOf = (src) => {
  const m = src.match(/<nav class="scv-bar"[\s\S]*?<\/nav>/);
  return m ? m[0] : null;
};

// The canonical bar is whatever lab.html ships — one source, not a copy of
// the markup pasted into this test to drift on its own.
const canonicalBar = barOf(read(join(CANARY, "lab.html")));

// Every page the manifest can route to: the onramp, each bench, each folded
// depth. This is the set the shell, the room and the sitemap all link.
const manifestPages = new Set([manifest.onramp.lab]);
for (const stage of manifest.stages) {
  const benches = stage.tracks ? stage.tracks.flatMap((t) => t.benches) : stage.benches || [];
  for (const b of benches) {
    manifestPages.add(b.lab);
    for (const d of b.depths || []) manifestPages.add(d.lab);
  }
}

test("static bar: byte-identical on every page that carries it", () => {
  assert.ok(canonicalBar, "lab.html lost its scv-bar — the canonical copy");
  for (const f of rootPages) {
    const bar = barOf(read(join(CANARY, f)));
    if (!bar) continue; // pages without a bar are covered by the manifest test
    // room.html is iframed by the shell, so its links need target="_top";
    // normalizing that attribute away, the bar must match everywhere.
    const normalized = bar.replace(/ target="_top"/g, "");
    assert.strictEqual(
      normalized, canonicalBar,
      `${f}: its scv-bar drifted from lab.html's. The bar is one shared block ` +
      `upgraded at runtime by assets/lab-nav.js — edit it on every page or not at all.`
    );
  }
});

test("manifest: every routed page exists and includes the shared nav", () => {
  for (const page of manifestPages) {
    const p = join(CANARY, page);
    assert.ok(existsSync(p), `build-line.json routes to ${page}, which does not exist`);
    const src = read(p);
    assert.ok(
      src.includes('src="assets/lab-nav.js"'),
      `${page}: reachable from build-line.json but missing the shared nav ` +
      `(<script type="module" src="assets/lab-nav.js"> after its scv-bar) — ` +
      `without it the page has no back-to-Lab, no build-line walk, and its ` +
      `external links strand the native app.`
    );
  }
});

test("every bar-carrying page includes the shared nav (index.html excepted)", () => {
  for (const f of rootPages) {
    const src = read(join(CANARY, f));
    if (!barOf(src)) continue;
    if (f === "index.html") {
      // The legacy front door meta-redirects immediately; a module would
      // never get to run there. Keep it static on purpose.
      continue;
    }
    assert.ok(
      src.includes('src="assets/lab-nav.js"'),
      `${f}: carries the static scv-bar but not assets/lab-nav.js — the bar ` +
      `will sit stale next to pages whose bars are alive.`
    );
  }
});

// ---- width caps -----------------------------------------------------------

// Pages whose <main> deliberately has no width cap of its own because a
// container inside it carries the measure. Adding a page here is a conscious
// decision — name the container that caps it.
const CAPPED_INSIDE = {
  "fleet.html": ".cards / #hero (hero + card grid carry their own measure)",
  "boards.html": ".broom-* stage blocks carry their own measure",
  "catalog.html": ".cat-* browse layout carries its own measure",
  "find.html": ".funnel question cards carry their own measure",
  "choose.html": ".quiz carries the measure",
  "house.html": ".house-main carries the measure",
  "scenes.html": ".scenes-main carries the measure",
  "witness-wall.html": ".wall-host embeds the emulator full-bleed on purpose",
};

// All CSS text a page can see: its <style> blocks + every local stylesheet
// it links.
function cssFor(src) {
  let css = [...src.matchAll(/<style>([\s\S]*?)<\/style>/g)].map((m) => m[1]).join("\n");
  for (const m of src.matchAll(/<link rel="stylesheet" href="([^"]+)"/g)) {
    const p = join(CANARY, m[1]);
    if (existsSync(p)) css += "\n" + read(p);
  }
  return css;
}

// Does any rule block whose selector list names `token` set a max-width?
function capsWidth(css, token) {
  for (const m of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    if (!/max-width/.test(m[2])) continue;
    const sels = m[1].split(",").map((s) => s.trim());
    if (sels.some((s) => s.split(/[\s>+~:]/).includes(token))) return true;
  }
  return false;
}

test("every manifest page's <main> declares its measure (or is allowlisted)", () => {
  for (const page of manifestPages) {
    const src = read(join(CANARY, page));
    const main = src.match(/<main([^>]*)>/);
    assert.ok(main, `${page}: no <main> element`);
    if (page in CAPPED_INSIDE) continue;
    const id = (main[1].match(/id="([^"]+)"/) || [])[1];
    const cls = ((main[1].match(/class="([^"]+)"/) || [])[1] || "").split(/\s+/)[0];
    const css = cssFor(src);
    const capped = (id && capsWidth(css, "#" + id)) || (cls && capsWidth(css, "." + cls));
    assert.ok(
      capped,
      `${page}: its <main${id ? ` id="${id}"` : ""}> has no max-width rule in any ` +
      `CSS it links. Pages that mount .hub-section content share #hub's measure ` +
      `(see the #start,#vault,… rule in canary-local.css); cap the main, or add ` +
      `the page to CAPPED_INSIDE in this test naming the container that caps it.`
    );
  }
});

// ---- copy parity ----------------------------------------------------------

test("bar tagline: browser copy in the bar, device copy in the module", () => {
  assert.ok(
    canonicalBar.includes("runs in your browser, nothing uploaded"),
    "the static bar's tagline changed — update lab-nav.js's app tagline in the same edit"
  );
  assert.ok(
    navJs.includes("runs on this device, nothing uploaded"),
    "lab-nav.js lost the app tagline that replaces 'runs in your browser' inside the native Lab"
  );
});

test("module contract: manifest-driven, styled by a real stylesheet", () => {
  assert.ok(navJs.includes('"build-line.json"'),
    "lab-nav.js must render from build-line.json — the one manifest every surface shares");
  assert.ok(existsSync(join(CANARY, "assets/lab-nav.css")),
    "assets/lab-nav.css is missing — lab-nav.js links it (flash.html's CSP forbids injected <style>)");
});
