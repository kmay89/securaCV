// Gates for the Lab shell (assets/lab-shell.js + lab-shell.css) — the app's
// own frame: sidebar, phone tab bar, and the pane that holds a bench.
//
// The shell used to answer a bench selection with a CARD about the bench and
// an "Open bench" button that navigated away. In the native app that read as a
// table of contents rather than a workshop, and it left most of a Mac window
// empty. Now the bench itself is framed in the pane and the shell keeps a slim
// toolbar above it. Four things that redesign depends on can rot silently —
// each fails a real surface, none fails loudly on its own — so each gets a
// test here:
//
//   1. every page the shell frames must be FRAMEABLE. A bench that grows a
//      frame-buster or a frame-ancestors CSP renders as a blank pane inside
//      the app, and nothing in the build would say so;
//   2. the inner page's own "still on SecuraCV" bar must be hidden with an
//      important inline style, not `hidden` alone. Every page styles
//      `.scv-bar { display: flex }`, and a class selector outranks the
//      [hidden] UA rule — the attribute alone reports hidden === true while
//      the bar stays on screen, stealing a strip off every bench;
//   3. a depth label's HEAD (the part before the em dash) has to fit a tab.
//      The tail is authored as a sentence, so the strip shows the head only —
//      a new depth with a long head clips instead of switching;
//   4. the phone tab bar's height lives in ONE token, because a full-bleed
//      bench has no padding of its own and must reserve exactly it. Hand-
//      guessing that reserve is what put the tab bar on top of the framed
//      page's last strip.
//
// Runs under "page logic tests" (.github/workflows/canary-local.yml). Reads
// source text only — no browser, no toolchain beyond the test runner.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const CANARY = join(__dirname, "..");
const read = (p) => readFileSync(p, "utf8");

const manifest = JSON.parse(read(join(CANARY, "build-line.json")));
const shellJs = read(join(CANARY, "assets/lab-shell.js"));
const shellCss = read(join(CANARY, "assets/lab-shell.css"));
// Rules only. These files explain themselves at length, and the comments name
// the very selectors some of these tests forbid — matching against them would
// fail on the sentence that says why the thing is banned.
const shellRules = shellCss.replace(/\/\*[\s\S]*?\*\//g, "");
const navJs = read(join(CANARY, "assets/lab-nav.js"));

// Every page the shell can frame: a bench's own page and each of its depths.
// (The onramp and the isometric room are framed too, so they come along.)
const framedPages = new Set([manifest.onramp.lab, "room.html"]);
for (const stage of manifest.stages) {
  const benches = stage.tracks ? stage.tracks.flatMap((t) => t.benches) : stage.benches || [];
  for (const b of benches) {
    framedPages.add(b.lab);
    for (const d of b.depths || []) framedPages.add(d.lab);
  }
}

test("the shell frames the bench itself rather than describing it", () => {
  assert.match(shellJs, /class: "bench-frame"/,
    "benchView must build the <iframe class=\"bench-frame\"> that IS the bench");
  assert.match(shellRules, /\.bench-frame\{/,
    "lab-shell.css must size .bench-frame, or the bench renders at the iframe default 150px");
  // The full-bleed toggle is a class, not `.content:has(.bench-wrap)`: the app
  // supports macOS 10.15 (tauri.conf.json minimumSystemVersion), whose WebView
  // predates :has(), where the selector no-ops and leaves the bench in a
  // reading column. A "simplification" back to :has() would look right on the
  // machine that made it and be wrong on the oldest Mac we ship to.
  assert.match(shellJs, /classList\.toggle\("content-full"/,
    "the full-bleed bench must be toggled by class");
  assert.doesNotMatch(shellRules, /:has\(/,
    "lab-shell.css must not rely on :has() — macOS 10.15's WebView silently ignores it");
});

test("every page the shell frames can actually be framed", () => {
  const busters = [/\btop\.location\s*=/, /window\.top\s*!==\s*window\.self/, /self\s*!==\s*top\b/];
  for (const page of [...framedPages].sort()) {
    const p = join(CANARY, page);
    assert.ok(existsSync(p), `${page} is in the manifest but not on disk`);
    const src = read(p);
    for (const re of busters) {
      assert.doesNotMatch(src, re,
        `${page} looks like it busts out of frames — the shell would show a blank pane`);
    }
    // A meta CSP cannot set frame-ancestors at all (it is ignored there), so
    // one appearing in a directive list is a promise the browser won't keep.
    const meta = src.match(/http-equiv="Content-Security-Policy"[^>]*content="([^"]*)"/);
    if (meta) {
      assert.doesNotMatch(meta[1], /frame-ancestors/,
        `${page}'s <meta> CSP names frame-ancestors, which meta CSP ignores — ` +
        `if framing must be restricted it has to be a real header`);
    }
  }
});

test("an embedded page's own bar is hidden by more than the hidden attribute", () => {
  const embedded = navJs.slice(navJs.indexOf("window !== window.top"));
  assert.match(embedded, /setProperty\("display",\s*"none",\s*"important"\)/,
    "lab-nav.js must force the inner .scv-bar down with an important inline style: " +
    "canary-local.css sets .scv-bar{display:flex}, which outranks [hidden], so the " +
    "attribute alone leaves the bar visible while reporting hidden === true");
});

test("every depth head fits the tab strip it is shown in", () => {
  // The strip shows the head only; the full label rides the title attribute.
  const MAX = 28;
  const heads = [];
  for (const stage of manifest.stages) {
    const benches = stage.tracks ? stage.tracks.flatMap((t) => t.benches) : stage.benches || [];
    for (const b of benches) {
      if (!b.depths || !b.depths.length) continue;
      // depthsFor() synthesizes a parent option labeled with the bench's noun
      // when no depth points back at the bench's own page, so the noun lands in
      // the same strip and is held to the same bar.
      if (!b.depths.some((d) => d.lab === b.lab)) heads.push([b.slug, b.noun]);
      for (const d of b.depths) heads.push([b.slug, d.label.split(" — ")[0]]);
    }
  }
  assert.ok(heads.length > 0, "no depths found — the manifest shape changed under this test");
  for (const [slug, head] of heads) {
    assert.ok(head.length <= MAX,
      `${slug}: depth tab "${head}" is ${head.length} chars (max ${MAX}). ` +
      `Put the long form after " — " — the strip shows the head, the tooltip shows all of it.`);
  }
});

test("the phone tab bar's height is one token, used by both sides", () => {
  assert.match(shellRules, /--tabbar-h:/,
    "the tab bar's box must be declared once as --tabbar-h");
  assert.match(shellRules, /\.tabbar\{[^}]*height:var\(--tabbar-h\)/,
    "the tab bar must take its height FROM the token, or the token is a description that drifts");
  assert.match(shellRules, /\.content\.content-full\{padding-bottom:var\(--tabbar-h\);?\}/,
    "a full-bleed bench must reserve exactly --tabbar-h — it has no padding of its own, " +
    "and a guessed reserve puts the tab bar on top of the framed page");
});
