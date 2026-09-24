// canary-local/tests/settings_rollers.test.js — every roller on the glass
// centers its options.
//
// The display runs without an LVGL theme (include/lv_conf.h
// LV_USE_THEME_DEFAULT 0). The default theme is what centers a roller;
// without it a roller's text_align is AUTO, which LVGL 8 and 9 both resolve
// to LEFT, so the option label sits flush-left under a selected band that
// spans the full width. The Quiet Hours wheels showed it on every touch
// flavor (the settings panel is built only where FEATURE_TOUCH is 1) until
// each roller helper set the alignment itself. So every lv_roller_create()
// in the display's sources must be followed, in the block that creates it,
// by
//   lv_obj_set_style_text_align(<roller>, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
// on the same variable. House style also keeps that line with the other
// styles, before lv_roller_set_selected(), as both helpers do. That order is
// for one shape across helpers, not for the rendering: LVGL re-places the
// label on any later style change too.
//
// What the scan reads as code: comments, the contents of string, char and
// raw-string literals, and `#if 0` regions are blanked first. Nothing else
// is evaluated, so a line under another preprocessor condition, under
// `if (false)` or after a `return` still counts. A roller's block ends where
// the block that created it closes, or where the same variable takes a new
// roller; a center line past that point does not count for it.
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

const blank = (s) => s.replace(/[^\n]/g, " ");

// The code a compiler reads, as far as a text scan can tell: comments, the
// contents of string / char / raw-string literals (quotes kept) and `#if 0`
// regions become spaces. Newlines are kept everywhere, so a line number in
// the result is the line number in the file.
function codeOnly(text) {
  let out = "";
  let i = 0;
  const n = text.length;
  while (i < n) {
    const c = text[i];
    const d = text[i + 1];
    const raw = c === "R" && d === '"' &&
      /(?:^|[^A-Za-z0-9_])(?:u8|u|U|L)?$/.test(text.slice(Math.max(0, i - 3), i))
      ? /^R"([^()\\\s]{0,16})\(/.exec(text.slice(i, i + 20)) : null;
    if (raw) {
      const close = `)${raw[1]}"`;
      const from = i + raw[0].length;
      const end = text.indexOf(close, from);
      const stop = end === -1 ? n : end;
      out += raw[0] + blank(text.slice(from, stop)) + (end === -1 ? "" : close);
      i = end === -1 ? n : end + close.length;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && text[j] !== c && text[j] !== "\n") j += text[j] === "\\" ? 2 : 1;
      out += c + blank(text.slice(i + 1, Math.min(j, n))) + (j < n ? text[j] : "");
      i = j + 1;
    } else if (c === "/" && d === "/") {
      const end = text.indexOf("\n", i);
      const stop = end === -1 ? n : end;
      out += blank(text.slice(i, stop));
      i = stop;
    } else if (c === "/" && d === "*") {
      const end = text.indexOf("*/", i + 2);
      const stop = end === -1 ? n : end + 2;
      out += blank(text.slice(i, stop));
      i = stop;
    } else {
      out += c;
      i++;
    }
  }
  // `#if 0` up to its own `#else` / `#elif` (the live branch) or `#endif`,
  // counting the conditionals nested inside it.
  const lines = out.split("\n");
  let dead = 0;
  for (let k = 0; k < lines.length; k++) {
    const pp = /^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$/.exec(lines[k]);
    if (dead === 0) {
      if (pp && pp[1] === "if" && /^\s*0\s*$/.test(pp[2])) {
        dead = 1;
        lines[k] = blank(lines[k]);
      }
      continue;
    }
    if (pp && pp[1].startsWith("if")) dead++;
    else if (pp && pp[1] === "endif") dead--;
    else if (pp && dead === 1) dead = 0;
    lines[k] = blank(lines[k]);
  }
  return lines.join("\n");
}

// Each `<var> = lv_roller_create(` site: its line, its variable, and the code
// that can still style it — from the create to the close of the block that
// holds it, or to the next roller the same variable takes.
function rollerSites(text) {
  const code = codeOnly(text);
  const found = [];
  const re = /\blv_roller_create\s*\(/g;
  let m;
  while ((m = re.exec(code))) {
    const lhs = /([A-Za-z_]\w*)\s*=\s*$/.exec(code.slice(Math.max(0, m.index - 80), m.index));
    found.push({ at: m.index, line: code.slice(0, m.index).split("\n").length, name: lhs ? lhs[1] : null });
  }
  return found.map((s, k) => {
    const again = found.slice(k + 1).find((t) => s.name !== null && t.name === s.name);
    const limit = again ? again.at : code.length;
    let depth = 0;
    let end = limit;
    for (let i = s.at; i < limit; i++) {
      if (code[i] === "{") depth++;
      else if (code[i] === "}" && --depth < 0) {
        end = i;
        break;
      }
    }
    return { line: s.line, name: s.name, body: code.slice(s.at, end) };
  });
}

const centerRe = (name) =>
  new RegExp(`\\blv_obj_set_style_text_align\\s*\\(\\s*${name}\\s*,\\s*LV_TEXT_ALIGN_CENTER\\s*,\\s*LV_PART_MAIN\\s*\\)\\s*;`);
const selectRe = (name) => new RegExp(`\\blv_roller_set_selected\\s*\\(\\s*${name}\\s*,`);
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

// The scan on small fixtures: the shapes a new wheel is likely to take, and
// the places a center line can sit without running.
const CENTER = (v) => `  lv_obj_set_style_text_align(${v}, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);`;
const verdicts = (lines) =>
  rollerSites(lines.join("\n")).map((s) => `${s.line}:${centered(s) ? "centered" : "bare"}`);

test("the scan: a roller built in a void function does not borrow a later helper's line", () => {
  assert.deepStrictEqual(verdicts([
    "void build_extra(lv_obj_t* p) {",
    "  lv_obj_t* r = lv_roller_create(p);",
    '  lv_roller_set_options(r, "a\\nb", LV_ROLLER_MODE_NORMAL);',
    "}",
    "lv_obj_t* mk_centered(lv_obj_t* p) {",
    "  lv_obj_t* r = lv_roller_create(p);",
    CENTER("r"),
    "  return r;",
    "}",
  ]), ["2:bare", "6:centered"]);
});

test("the scan: an early return or a brace in a literal does not end the block", () => {
  assert.deepStrictEqual(verdicts([
    "lv_obj_t* mk_guarded(lv_obj_t* p) {",
    "  lv_obj_t* r = lv_roller_create(p);",
    "  if (!r) return nullptr;",
    '  lv_roller_set_options(r, "}\\n{", LV_ROLLER_MODE_NORMAL);',
    CENTER("r"),
    "  return r;",
    "}",
  ]), ["2:centered"]);
});

test("the scan: a center line in a comment, a string or #if 0 does not count", () => {
  assert.deepStrictEqual(verdicts([
    "lv_obj_t* a(lv_obj_t* p) {",
    "  lv_obj_t* r = lv_roller_create(p);",
    "  //" + CENTER("r"),
    "  return r;",
    "}",
    "lv_obj_t* b(lv_obj_t* p) {",
    "  lv_obj_t* r = lv_roller_create(p);",
    `  (void)"${CENTER("r").trim()}";`,
    "  return r;",
    "}",
    "lv_obj_t* c(lv_obj_t* p) {",
    "  lv_obj_t* r = lv_roller_create(p);",
    "#if 0",
    CENTER("r"),
    "#endif",
    "  return r;",
    "}",
    "lv_obj_t* d(lv_obj_t* p) {",
    "  lv_obj_t* r = lv_roller_create(p);",
    "#if 0",
    "  lv_obj_set_width(r, 1);",
    "#else",
    CENTER("r"),
    "#endif",
    "  return r;",
    "}",
  ]), ["2:bare", "7:bare", "12:bare", "19:centered"]);
});

test("the scan: each roller answers for its own variable", () => {
  assert.deepStrictEqual(verdicts([
    "void build_pair(lv_obj_t* p) {",
    "  lv_obj_t* a = lv_roller_create(p);",
    "  lv_obj_t* b = lv_roller_create(p);",
    CENTER("a"),
    "  lv_obj_t* c = lv_roller_create(p);",
    CENTER("b"),
    "  lv_obj_t* r = lv_roller_create(p);",
    "  r = lv_roller_create(p);",
    CENTER("r"),
    "}",
  ]), ["2:centered", "3:centered", "5:bare", "7:bare", "8:centered"]);
});

test("the premise: the glass has no LVGL theme", () => {
  const conf = readFileSync(join(PROJ, "include/lv_conf.h"), "utf8");
  assert.match(codeOnly(conf), /^#define LV_USE_THEME_DEFAULT 0\b/m,
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
      const sel = s.body.search(selectRe(s.name));
      if (sel !== -1 && sel < c) late.push(`${f.rel}:${s.line}`);
    }
  }
  assert.deepStrictEqual(bare, [],
    `${RULE}: add lv_obj_set_style_text_align(<roller>, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN); ` +
    `after its width, in the block that creates it, at ${bare.join(", ")}`);
  assert.deepStrictEqual(late, [],
    "house style: keep the center line with the other styles, before lv_roller_set_selected(), " +
    `as mk_hour_roller and mk_wx_roller do: ${late.join(", ")}`);
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
