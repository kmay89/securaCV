// canary-local/tests/dash_orientation.test.js — the dash glass says which
// turn it wears (F245), pinned to the firmware sources.
//
// lvgl_port_set_rotation() refuses a turn on a glass with no turn buffer and
// the glass stays landscape (F205), its face too (F223). The owner's choice
// stays saved; what the glass and the app are told follows the turn worn,
// through one pure header, firmware/projects/canary-display/include/canary/
// ui/orientation_page.h. tests_host holds that header: test_display_settings
// over every saved/worn pair, test_lvgl_port_turn fed from the real
// lvgl_port.cpp with its turn buffer granted and refused. settings_ui.cpp and
// glass_web.cpp compile on no host, so these read their source. What can rot:
//   · the Orientation page checks the saved turn again, or keeps
//     one footer whatever the glass wears                      → "page"
//   · the root row names the saved turn again                  → "root row"
//   · a new read of the saved rotation shows it on the glass   → "saved reads"
//   · the GET drops orientation_worn, serves it from the store,
//     or stops serving the saved choice as orientation         → "GET"
//   · the header grows a dependency the host cannot build      → "header"
//   · the host tests stop reading the header the way the page
//     does, or fall out of the suite's run                     → "host tests"
// Node only; reads source text.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const PROJ = join(__dirname, "../../firmware/projects/canary-display");
const read = (rel) => readFileSync(join(PROJ, rel), "utf8");
const SETTINGS_UI = read("src/ui/settings_ui.cpp");
const GLASS_WEB = read("src/net/glass_web.cpp");
const HEADER = read("include/canary/ui/orientation_page.h");

// Walk C source a character at a time, string and character literals whole:
// `visit(i, c, inLiteral)` sees each character; it returns the index to
// resume at (or nothing for the next one).
function scan(src, visit) {
  for (let i = 0; i < src.length;) {
    const c = src[i];
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < src.length && src[j] !== c) j += src[j] === "\\" ? 2 : 1;
      for (let k = i; k <= j && k < src.length; k++) visit(k, src[k], true);
      i = j + 1;
      continue;
    }
    const next = visit(i, c, false);
    i = next === undefined ? i + 1 : next;
  }
}

// C source with comments dropped (never inside a literal) and whitespace
// folded outside literals, so a re-wrapped line reads the same: one space
// between tokens, none just inside parentheses or before a comma.
function norm(src) {
  let out = "";
  scan(src, (i, c, lit) => {
    if (lit) { out += c; return; }
    if (c === "/" && src[i + 1] === "/") { out += " "; const e = src.indexOf("\n", i); return e < 0 ? src.length : e; }
    if (c === "/" && src[i + 1] === "*") { out += " "; const e = src.indexOf("*/", i + 2); return e < 0 ? src.length : e + 2; }
    out += /\s/.test(c) ? " " : c;
  });
  // Fold the spacing outside literals only: split on literals, fold the rest.
  return out.split(/("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')/).map((part, k) => k % 2 ? part
    : part.replace(/\s+/g, " ").replace(/\( /g, "(").replace(/ \)/g, ")").replace(/ ,/g, ","))
    .join("").trim();
}

// The body of the first function whose definition line matches `head`, from
// its opening brace to the brace that closes it (a brace in a literal or a
// comment is not one).
function body(src, head) {
  const m = head.exec(src);
  assert.ok(m, `no ${head} in the source`);
  const start = src.indexOf("{", m.index + m[0].length - 1);
  const rest = src.slice(start);
  let depth = 0;
  let end = -1;
  scan(rest, (i, c, lit) => {
    if (end >= 0) return Infinity;
    if (lit) return;
    if (c === "/" && rest[i + 1] === "/") { const e = rest.indexOf("\n", i); return e < 0 ? rest.length : e; }
    if (c === "/" && rest[i + 1] === "*") { const e = rest.indexOf("*/", i + 2); return e < 0 ? rest.length : e + 2; }
    if (c === "{") depth++;
    else if (c === "}" && --depth === 0) { end = start + i; return Infinity; }
  });
  assert.ok(end >= 0, `${head}: no closing brace`);
  return src.slice(start, end + 1);
}

// The arguments of the call to `fn` that starts at `at`, split at top-level
// commas (a comma inside a literal is the literal's).
function callArgs(src, at, fn) {
  const open = src.indexOf("(", at + fn.length - 1);
  let depth = 0;
  let done = false;
  const args = [];
  let cur = "";
  scan(src.slice(open), (i, c, lit) => {
    if (done) return Infinity;
    if (!lit && c === "(" && depth++ === 0) return;
    if (!lit && c === ")" && --depth === 0) { args.push(cur.trim()); done = true; return Infinity; }
    if (!lit && c === "," && depth === 1) { args.push(cur.trim()); cur = ""; return; }
    cur += c;
  });
  assert.ok(done, `${fn}: unclosed call`);
  return args;
}

const SAVED_READ = /(?:settings\(\)|\bgs)\.rotation\b/g;
const WORN = "lvgl_port_rotation()";

test("page (F245): Settings > Orientation checks the turn worn and says when the saved one was refused", () => {
  assert.match(SETTINGS_UI, /^#include "canary\/ui\/orientation_page\.h"/m,
    "settings_ui.cpp includes orientation_page.h");
  const page = norm(body(SETTINGS_UI, /^void build_display\(\) /m));
  assert.ok(page.includes(`const OrientationState os = orientation_state(settings().rotation, ${WORN});`),
    `build_display() builds its state from the saved rotation and ${WORN}: ${page}`);
  assert.ok(page.includes("for (uint8_t rot : kOrientationRows) row_check(orientation_label(rot), nullptr, " +
      "orientation_row_checked(os, rot), ID_OPT_BASE + rot);"),
    `each of the four rows is checked by orientation_row_checked: ${page}`);
  assert.strictEqual((page.match(/\brow_check\(/g) || []).length, 1,
    "the rows come from that one loop, none typed beside it");
  assert.ok(page.includes("char foot[kOrientationFooterCap]; group_end(orientation_footer(os, foot, sizeof(foot)));"),
    `the footer is orientation_footer's: ${page}`);
  // No literal under the rows: the footer's words are the header's, so the
  // page cannot keep "The layout follows the turn." over a refused turn.
  const literals = page.match(/"(?:[^"\\]|\\.)*"/g) || [];
  assert.deepStrictEqual(literals, ['"Orientation"', '"Settings"'], "the page types only its title and back label");
  assert.ok(!/\b(?:rot|os\.saved|os\.worn)\s*==/.test(page), "the page compares no turn of its own");
});

test("root row (F245): the Orientation row names the turn worn", () => {
  const root = norm(body(SETTINGS_UI, /^void build_root\(\) /m));
  const at = root.indexOf('row_nav("Orientation"');
  assert.ok(at >= 0, "build_root() still has its Orientation row");
  assert.strictEqual(root.indexOf('row_nav("Orientation"', at + 1), -1, "one Orientation row");
  const args = callArgs(root, at, "row_nav");
  assert.deepStrictEqual(args, ['"Orientation"',
    `orientation_row_value(orientation_state(gs.rotation, ${WORN}), rn, sizeof(rn))`, "ID_ROW_DISPLAY"],
  "the row's value is orientation_row_value of the saved rotation and the turn worn");
});

test("saved reads (F245): settings_ui.cpp reads the saved rotation only to store it, hand it to the port, or set it beside the turn worn", () => {
  const src = norm(SETTINGS_UI);
  const contexts = [];
  for (const m of src.matchAll(SAVED_READ)) {
    const before = src.slice(Math.max(0, m.index - 40), m.index);
    const after = src.slice(m.index + m[0].length, m.index + m[0].length + 40);
    if (/orientation_state\($/.test(before) && after.startsWith(`, ${WORN})`)) contexts.push("orientation_state");
    else if (/lvgl_port_set_rotation\($/.test(before) && after.startsWith(")")) contexts.push("lvgl_port_set_rotation");
    else if (after.startsWith(" = (uint8_t)(id - ID_OPT_BASE);")) contexts.push("store the tapped row");
    else contexts.push(`UNEXPECTED: …${before}${m[0]}${after}…`);
  }
  assert.deepStrictEqual(contexts.sort(), [
    "lvgl_port_set_rotation",  // the tap: wear the choice just stored
    "lvgl_port_set_rotation",  // the reset: wear the default
    "orientation_state",       // the root row
    "orientation_state",       // the Orientation page
    "store the tapped row",
  ], "every read of the saved rotation in settings_ui.cpp");
});

test("GET (F245): the dash's /api/settings serves the saved choice as orientation and the turn worn as orientation_worn", () => {
  assert.match(GLASS_WEB,
    /#ifdef CD_FLAVOR_DASH\n(?:\/\/[^\n]*\n)*#include "canary\/ui\/lvgl_port\.h"[^\n]*\n#include "canary\/ui\/orientation_page\.h"[^\n]*\n#endif/,
    "glass_web.cpp includes lvgl_port.h and orientation_page.h for the dash");
  const get = norm(body(GLASS_WEB, /^void handle_settings_get\(\) /m));
  assert.ok(get.includes("const canary::ui::OrientationState os = canary::ui::orientation_state(gs.rotation, " +
      `canary::ui::${WORN});`), "the GET builds the same state the page reads");
  const at = get.indexOf('\\"orientation_worn\\"');
  assert.ok(at >= 0, "the GET serves orientation_worn");
  assert.strictEqual(GLASS_WEB.split('\\"orientation_worn\\"').length, 2, "orientation_worn is served once, by the dash");
  const args = callArgs(get, get.lastIndexOf("snprintf(", at), "snprintf");
  assert.match(args[2], /^",\\"orientation\\":%u,\\"orientation_worn\\":%u,/, `the two keys lead the call: ${args[2]}`);
  assert.deepStrictEqual(args.slice(3, 5), ["os.saved", "os.worn"],
    "orientation is the saved choice and orientation_worn the turn worn");
  // Besides that state, only /api/set's setter touches the saved rotation.
  const src = norm(GLASS_WEB);
  const contexts = [...src.matchAll(SAVED_READ)].map((m) => {
    const before = src.slice(Math.max(0, m.index - 60), m.index);
    const after = src.slice(m.index + m[0].length, m.index + m[0].length + 60);
    if (/canary::ui::orientation_state\($/.test(before) && after.startsWith(`, canary::ui::${WORN});`))
      return "orientation_state";
    if (/else if \(k == "orientation" && v >= 0 && v <= 3\) $/.test(before) && after.startsWith(" = (uint8_t)v;"))
      return "/api/set stores it";
    return `UNEXPECTED: …${before}${m[0]}${after}…`;
  });
  assert.deepStrictEqual(contexts.sort(), ["/api/set stores it", "orientation_state"],
    "every read of the saved rotation in glass_web.cpp");
});

test("header (F245): orientation_page.h stays pure and the check sits on the turn worn", () => {
  const includes = HEADER.split("\n").filter((l) => l.startsWith("#include")).map((l) => l.replace(/\s*\/\/.*$/, ""));
  assert.deepStrictEqual(includes, ["#include <stddef.h>", "#include <stdint.h>", "#include <stdio.h>",
    '#include "canary/glass_settings.h"'], "no LVGL, no Arduino: the host builds it");
  const h = norm(HEADER);
  assert.ok(h.includes("bool refused() const { return saved != worn; }"), "refused is saved against worn");
  assert.ok(h.includes("return s.worn == (uint8_t)(rot & 3);"), "the check is the turn worn");
  assert.ok(h.includes("canary::glass::rotation_name(s.worn)"), "the root row names the turn worn");
});

test("host tests (F245): both suites read the header as the page does, and both run", () => {
  const mk = read("tests_host/Makefile");
  const rule = (bin) => {
    const m = new RegExp(`^\\$\\(${bin}\\):((?:[^\\n]*\\\\\\n)*[^\\n]*)`, "m").exec(mk);
    assert.ok(m, `no rule for $(${bin})`);
    return m[1];
  };
  assert.match(mk, /^DISPSET_HDR = [^\n]*\\\n\s*\.\.\/include\/canary\/ui\/orientation_page\.h$/m,
    "test_display_settings depends on the header");
  assert.ok(rule("DISPSET_BIN").includes("$(DISPSET_HDR)"), "…through its rule");
  assert.ok(rule("PORTTURN_BIN").includes("../include/canary/ui/orientation_page.h"),
    "test_lvgl_port_turn depends on the header");
  assert.match(mk, /^run:[^\n]*\$\(DISPSET_BIN\)/m, "make run builds test_display_settings");
  assert.match(mk, /^\t\.\/\$\(DISPSET_BIN\)$/m, "…and runs it");
  assert.match(mk, /^run: run-lvgl-port-turn$/m, "make run runs test_lvgl_port_turn");

  const dispset = read("tests_host/test_display_settings.cpp");
  assert.match(dispset, /^#include "canary\/ui\/orientation_page\.h"$/m);
  assert.match(norm(body(dispset, /^int main\(\) /m)), /test_orientation_page\(\);/,
    "test_display_settings' main runs test_orientation_page");

  const turn = read("tests_host/test_lvgl_port_turn.cpp");
  const page = norm(body(turn, /^static void check_orientation_page\(/m));
  assert.ok(page.includes(`orientation_state(canary::glass::settings().rotation, ${WORN});`),
    "test_lvgl_port_turn reads the state from the saved rotation and the port, as build_display() does");
  assert.match(norm(body(turn, /^static void check_face\(/m)),
    /lvgl_port_set_rotation\(canary::glass::settings\(\)\.rotation\);.*check_orientation_page\(tag, rot, turns \? rot : \(uint8_t\)ROT_LANDSCAPE\);/,
    "every boot check_face plays reads the page after main.cpp's boot call");
  assert.match(norm(body(turn, /^static void test_face_follows_the_worn_turn\(/m)),
    /check_face\("PSRAM turn buffer refused", false\);.*check_face\("fallback glass, turn buffer refused by both heaps", false\);/,
    "the refusing glasses are among them");
});
