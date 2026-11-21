// canary-local/tests/sideways_scroll.test.js — the causes of a page that
// scrolls sideways, held at the source.
//
// Measured in Chromium on 2026-11-21, fresh load at each width: the Lab's
// landing page scrolled 142px sideways at 1280 wide and 99px on a 390px
// phone; the break-glass bench 168px on a phone and 295px on a tablet; the
// House 80px, the Vision 54px and the radar school 49px on a phone; the case
// finder 21px on a tablet; and every bench 22px at 320. Five causes, each a
// one-line fix, and each one easy to undo without noticing: nothing on the
// page looks wrong until it is opened at that width.
//
// This file reads source text only. It cannot measure a layout; it keeps the
// five fixes from being edited away. The measurement is a browser's.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ASSETS = join(__dirname, "..", "assets");
const css = (f) => readFileSync(join(ASSETS, f), "utf8").replace(/\/\*[\s\S]*?\*\//g, "");
const rule = (text, selector) => {
  // The declarations of the first rule whose selector list is exactly `selector`.
  const esc = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&").replace(/\s+/g, "\\s+");
  const m = new RegExp(`(?:^|[}\\s])${esc}\\s*\\{([^}]*)\\}`).exec(text);
  assert.ok(m, `no rule for "${selector}"`);
  return m[1].replace(/\s+/g, " ");
};

test("a decoration that bleeds past its box is clipped by the column, not left to widen the page", () => {
  // .ov-glow is absolutely placed with a negative inset on purpose. The main
  // column runs to the viewport's edge at every width, so clipping there
  // keeps the bleed on screen and stops it widening the document. clip, not
  // hidden: hidden would make the column a scroll container and unstick the
  // bars inside it.
  const shell = css("lab-shell.css");
  assert.match(rule(shell, ".ov-glow"), /inset:\s*-\d+px -\d+px/, "the glow no longer bleeds; this guard may be retired with it");
  assert.match(rule(shell, ".main"), /overflow-x:\s*clip/);
});

test("a grid track is not allowed to be as wide as its widest item", () => {
  // A bare 1fr is minmax(auto, 1fr): one long nowrap line sets the track's
  // minimum and the page scrolls. The fix is min-width: 0 on the item, or
  // minmax(0, 1fr) on the track.
  const base = css("canary-local.css");
  assert.match(rule(base, ".op-cmd"), /min-width:\s*0/, "the four command cards: their usage line is nowrap and scrolls inside itself");
  assert.match(rule(base, ".op-check"), /grid-template-columns:\s*150px minmax\(0, 1fr\)/);
  assert.match(rule(base, ".wap-fact"), /min-width:\s*0/);
  assert.match(css("house.css"), /@media \(max-width: 940px\)\s*\{\s*\.house-main\s*\{\s*grid-template-columns:\s*minmax\(0, 1fr\)/,
    "the House's one-column layout must not take the panel's width");
});

test("a value that is one long token may break", () => {
  const base = css("canary-local.css");
  for (const sel of [".fineprint", ".wap-fact-v", ".hub-link code", ".op-check-line"]) {
    assert.match(rule(base, sel), /overflow-wrap:\s*anywhere/, `${sel}: a topic, a path or a model name is one word to the browser`);
  }
});

test("a row of controls that cannot shrink may wrap, and its grid column is as wide as the row needs", () => {
  const base = css("canary-local.css");
  assert.match(rule(base, ".op-ctl"), /flex-wrap:\s*wrap/);
  assert.match(rule(base, ".slab-knob"), /flex-wrap:\s*wrap/);
  // 118 (label) + 80 (slider) + 76 (value) + two 8px gaps = 290.
  assert.match(rule(base, ".slab-knob-grid"), /minmax\(min\(29\dpx, 100%\), 1fr\)/);
  assert.match(rule(base, ".slab-frames"), /overflow-x:\s*auto/);
});

test("the Lab bar, which never wraps, has something to give up at each width", () => {
  const nav = css("lab-nav.css");
  const js = readFileSync(join(ASSETS, "lab-nav.js"), "utf8");
  // A depth page carries three crumbs. Below 900px the stage crumb goes and
  // the page's own name may shorten; at 320px the Glossary link goes.
  assert.match(js, /class: "scv-crumb scv-stage"/);
  assert.match(js, /at && at\.depth \? " scv-deep" : ""/);
  assert.match(js, /class: "scv-gloss"/);
  assert.match(nav, /@media \(max-width: 900px\)\s*\{[^@]*\.scv-bar-lab\.scv-deep \.scv-stage[^@]*display:\s*none/);
  assert.match(nav, /\.scv-bar-lab\.scv-deep \.scv-cur\s*\{[^}]*flex:\s*0 1 auto/);
  assert.match(nav, /@media \(max-width: 350px\)\s*\{\s*\.scv-bar-lab \.scv-gloss\s*\{\s*display:\s*none/);
  // The bar's height is pinned because it never wraps; a wrap would need the
  // sticky offsets under it measured instead.
  assert.match(rule(nav, ".scv-bar-lab"), /flex-wrap:\s*nowrap/);
});
