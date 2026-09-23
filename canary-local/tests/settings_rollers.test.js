// canary-local/tests/settings_rollers.test.js — every roller on the glass
// centers its options.
//
// The display runs without an LVGL theme (include/lv_conf.h
// LV_USE_THEME_DEFAULT 0). The default theme is what centers a roller;
// without it a roller's text_align is AUTO, which LVGL 8 and 9 both resolve
// to LEFT, so the option label sits flush-left under a selected band that
// spans the full width. The Quiet Hours wheels showed it on every flavor
// until each roller helper set the alignment itself. So every
// lv_roller_create() in the display's sources must be followed, before its
// helper returns, by
//   lv_obj_set_style_text_align(<roller>, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
// and that line must come before lv_roller_set_selected() (which re-runs the
// label placement). A commented-out line does not count.
//
// What this does not prove: how it renders. The geometry was measured by a
// host harness on LVGL 8.4.0 and 9.5.0; the emulator dist and the PlatformIO
// and Arduino builds are CI's. Node only; reads source text.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync, existsSync, statSync } = require("node:fs");
const { join, relative, basename } = require("node:path");

const REPO = join(__dirname, "..", "..");
const PROJ = join(REPO, "firmware/projects/canary-display");
const SKETCH = join(PROJ, "arduino/canary_display");
const RULE = "on a themeless glass a roller must center itself";

// Comments out, string and char literals kept, newlines kept (so a line
// number in the stripped text is the line number in the file).
function stripComments(text) {
  let out = "";
  let i = 0;
  const n = text.length;
  while (i < n) {
    const c = text[i];
    const d = text[i + 1];
    const raw = c === "R" && d === '"' && !/[A-Za-z0-9_]/.test(text[i - 1] || "")
      ? /^R"([^()\\\s]{0,16})\(/.exec(text.slice(i, i + 20)) : null;
    if (raw) {
      const close = `)${raw[1]}"`;
      const end = text.indexOf(close, i + raw[0].length);
      const stop = end === -1 ? n : end + close.length;
      out += text.slice(i, stop);
      i = stop;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && text[j] !== c && text[j] !== "\n") j += text[j] === "\\" ? 2 : 1;
      out += text.slice(i, j + 1);
      i = j + 1;
    } else if (c === "/" && d === "/") {
      while (i < n && text[i] !== "\n") i++;
    } else if (c === "/" && d === "*") {
      const end = text.indexOf("*/", i + 2);
      const stop = end === -1 ? n : end + 2;
      out += text.slice(i, stop).replace(/[^\n]/g, "");
      i = stop;
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

// Each `<var> = lv_roller_create(` site, with the code up to the next
// `return` (the end of the helper that builds it).
function rollerSites(text) {
  const code = stripComments(text);
  const sites = [];
  const re = /\blv_roller_create\s*\(/g;
  let m;
  while ((m = re.exec(code))) {
    const line = code.slice(0, m.index).split("\n").length;
    const lhs = /([A-Za-z_]\w*)\s*=\s*$/.exec(code.slice(Math.max(0, m.index - 80), m.index));
    const ret = /\breturn\b/g;
    ret.lastIndex = m.index;
    const end = ret.exec(code);
    sites.push({ line, name: lhs ? lhs[1] : null, body: code.slice(m.index, end ? end.index : code.length) });
  }
  return sites;
}

const centerRe = (name) =>
  new RegExp(`\\blv_obj_set_style_text_align\\s*\\(\\s*${name}\\s*,\\s*LV_TEXT_ALIGN_CENTER\\s*,\\s*LV_PART_MAIN\\s*\\)\\s*;`);
const centered = (s) => s.name !== null && centerRe(s.name).test(s.body);

function sources(dir) {
  const out = [];
  for (const n of readdirSync(dir).sort()) {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) out.push(...sources(p));
    else if (/\.(c|cpp|h|hpp)$/.test(n)) out.push(p);
  }
  return out;
}

// Every display source that creates a roller: src/ and include/, the trees
// setup.sh flattens into the Arduino sketch.
const withRollers = [join(PROJ, "src"), join(PROJ, "include")]
  .flatMap(sources)
  .map((p) => ({ path: p, rel: relative(REPO, p), sites: rollerSites(readFileSync(p, "utf8")) }))
  .filter((f) => f.sites.length > 0);

test("the premise: the glass has no LVGL theme", () => {
  const conf = readFileSync(join(PROJ, "include/lv_conf.h"), "utf8");
  assert.match(stripComments(conf), /^#define LV_USE_THEME_DEFAULT 0\b/m,
    "lv_conf.h no longer says LV_USE_THEME_DEFAULT 0. If a theme is back, it " +
    "centers rollers itself: revisit this test and the comment above mk_wx_roller");
});

test("both settings wheels are found: the Quiet Hours and Location helpers", () => {
  const ui = withRollers.find((f) => f.rel.endsWith("src/ui/settings_ui.cpp"));
  assert.ok(ui, "src/ui/settings_ui.cpp creates no roller: the scan is looking in the wrong place");
  assert.ok(ui.sites.length >= 2,
    `expected mk_hour_roller and mk_wx_roller in settings_ui.cpp, found ${ui.sites.length} roller(s)`);
});

test("every roller on the glass centers its options", () => {
  const bare = [];
  const late = [];
  for (const f of withRollers) {
    for (const s of f.sites) {
      if (!centered(s)) {
        bare.push(`${f.rel}:${s.line}`);
        continue;
      }
      const c = s.body.search(centerRe(s.name));
      const sel = s.body.search(/\blv_roller_set_selected\s*\(/);
      if (sel !== -1 && sel < c) late.push(`${f.rel}:${s.line}`);
    }
  }
  assert.deepStrictEqual(bare, [],
    `${RULE}: add lv_obj_set_style_text_align(<roller>, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN); ` +
    `after its width, before the helper returns, at ${bare.join(", ")}`);
  assert.deepStrictEqual(late, [],
    `${RULE}, and before lv_roller_set_selected() (it re-runs the label placement): ${late.join(", ")}`);
});

test("the Arduino sketch mirror carries the same centered wheels", () => {
  for (const f of withRollers) {
    const flat = basename(f.path) === "main.cpp" ? "canary_display.ino" : basename(f.path);
    const mirror = join(SKETCH, flat);
    assert.ok(existsSync(mirror), `${f.rel} has no mirror at ${relative(REPO, mirror)}: run setup.sh regen`);
    const m = rollerSites(readFileSync(mirror, "utf8"));
    assert.strictEqual(m.length, f.sites.length,
      `${relative(REPO, mirror)}: ${m.length} roller(s), ${f.rel} has ${f.sites.length}: run setup.sh regen`);
    assert.strictEqual(m.filter(centered).length, f.sites.filter(centered).length,
      `${relative(REPO, mirror)}: centered-roller count differs from ${f.rel}: run setup.sh regen`);
  }
});
