#!/usr/bin/env node
// canary-local/tools/lint_css_selectors.mjs — no class or id selector in the
// Lab's stylesheets that no page or script uses (sweep CI16).
//
//   node canary-local/tools/lint_css_selectors.mjs              # the lint
//   node canary-local/tools/lint_css_selectors.mjs --self-test  # its probes only
//
// The 2026-10 cleanup removed unused CSS from canary-local/assets by hand, and
// CI12's ESLint job keeps the JavaScript half clean; nothing kept a selector
// that no page or script uses from growing back. This reads every
// canary-local/assets/*.css (first-party: the glob stops before vendor/),
// takes the class and id names out of each rule's selector (never out of a
// declaration, so a #fff or a url(#x) is not a name; never out of an at-rule's
// prelude; quoted attribute values and strings are dropped first), and requires each name to be USED: written as a whole token in a
// page (canary-local/*.html) or a first-party script
// (canary-local/assets/*.js, *.mjs). Those are the only files that put a
// class or id on an element the stylesheets reach; nested pages
// (voice/, witness/, the emulator harness) load stylesheets of their own.
//
// Classes built at run time are read from the scripts, not skipped. A script
// that writes a class prefix and then a value — `wire-${m.dir}`,
// "flash-mode flash-mode-" + mode — makes that prefix BUILT, and a selector
// `prefix + suffix` is used when the suffix is itself a whole token somewhere
// in the pages or scripts (the value the script feeds in: "lwt", "download",
// a grade "A"). A built name whose suffix appears nowhere — its value comes
// from data, transformed — is listed in ALLOW below with the reason, and an
// ALLOW entry that is no longer needed (or names no selector any more) fails
// the lint too, so the list cannot rot.
//
// Known limits, on purpose: a name that appears in a comment, or as a word in
// prose, counts as used (the lint cannot tell markup from text without a
// parser per language); a built name passes when its suffix is any token in
// the pages or scripts, so a dead variant whose suffix is a common word
// (flash-map-ghost: "ghost" is written elsewhere) still passes; and a used
// name proves the selector can match something, not that the rule is the one
// that wins. It catches the case that matters: a rule nothing names.
//
// CI: .github/workflows/lab-lint.yml (job unused-css).

import { readFileSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const LAB = join(HERE, '..');
const ASSETS = join(LAB, 'assets');

// Built at run time from data the scripts transform, so the suffix is in no
// page or script. Each entry: the selector name, and where it is built.
export const ALLOW = new Map([
  ['.sense-err-falsepositive', 'sense-ui.js builds "sense-err-" + kind.replace("-", "") from '
    + 'devices/sense.json tuning.errors[].kind ("false-positive")'],
  ['.sense-err-falsenegative', 'sense-ui.js builds "sense-err-" + kind.replace("-", "") from '
    + 'devices/sense.json tuning.errors[].kind ("false-negative")'],
  ['.sense-err-wrongvalue', 'sense-ui.js builds "sense-err-" + kind.replace("-", "") from '
    + 'devices/sense.json tuning.errors[].kind ("wrong-value")'],
]);

const NAME = /([.#])(-?[A-Za-z_][\w-]*)/g;

/** The class and id names in a stylesheet's selectors, as ".x" / "#x". */
export function selectorNames(css) {
  const text = css.replace(/\/\*[\s\S]*?\*\//g, ' ');
  const names = new Set();
  let buf = '';
  let quote = null;
  for (const ch of text) {
    if (quote) {
      buf += ch;
      if (ch === quote) quote = null;
      continue;
    }
    if (ch === '"' || ch === "'") {
      quote = ch;
      buf += ch;
    } else if (ch === '{') {
      const prelude = buf.trim();
      buf = '';
      // An at-rule's prelude is not a selector (@supports selector(.x) names
      // .x without styling it); the rules inside it are read as usual, and a
      // keyframe step (from, 50%) holds no name.
      if (prelude.startsWith('@')) continue;
      // A quoted attribute value or string (`[href$=".pdf"]`) holds no name.
      const bare = prelude.replace(/"[^"]*"|'[^']*'/g, ' ');
      for (const m of bare.matchAll(NAME)) names.add(m[1] + m[2]);
    } else if (ch === '}') {
      buf = '';
    } else if (ch === ';') {
      buf = '';
    } else {
      buf += ch;
    }
  }
  return names;
}

/** Class prefixes the scripts build at run time: `pre-${...}` in a template,
 * or a string literal ending in `pre-` followed by `+`. */
export function builtPrefixes(text) {
  const out = new Set();
  for (const m of text.matchAll(/([A-Za-z_][\w-]*-)\$\{/g)) out.add(m[1]);
  for (const m of text.matchAll(/["'`][^"'`\n]*?([A-Za-z_][\w-]*-)["'`]\s*\+/g)) out.add(m[1]);
  return out;
}

const escape = (s) => s.replace(/[.*+?^${}()|[\]\\-]/g, '\\$&');
const tokenIn = (text, word) => new RegExp(`(?<![\\w-])${escape(word)}(?![\\w-])`).test(text);

/** Check `sheets` ([{file, text}]) against the `consumers` text.
 * Returns {unused: ["file: .x"], stale: ["ALLOW .x: why"]}. */
export function check(sheets, consumers, allow = ALLOW) {
  const prefixes = builtPrefixes(consumers);
  const used = (name) => {
    const bare = name.slice(1);
    if (tokenIn(consumers, bare)) return true;
    for (const p of prefixes) {
      if (bare.length > p.length && bare.startsWith(p) && tokenIn(consumers, bare.slice(p.length))) {
        return true;
      }
    }
    return false;
  };
  const unused = [];
  const seen = new Set();
  for (const { file, text } of sheets) {
    for (const name of [...selectorNames(text)].sort()) {
      seen.add(name);
      if (used(name)) continue;
      if (!allow.has(name)) unused.push(`${file}: ${name}`);
    }
  }
  const stale = [];
  for (const [name] of allow) {
    if (!seen.has(name)) stale.push(`ALLOW ${name}: no stylesheet has this selector any more`);
    else if (used(name)) stale.push(`ALLOW ${name}: a page or script now names it; drop the entry`);
  }
  return { unused, stale };
}

// The lint's own probes: each must come out as written, or the lint refuses
// to report the real stylesheets clean.
const PROBES = [
  { why: 'a class a page names is used',
    css: '.a { color: red; }', use: '<div class="a"></div>', unused: [] },
  { why: 'a class nothing names is unused',
    css: '.a { } .dead { }', use: '<div class="a">', unused: ['.dead'] },
  { why: 'an id is held like a class',
    css: '#panel { } #gone { }', use: 'el.id = "panel"', unused: ['#gone'] },
  { why: 'a whole token, not a substring',
    css: '.wire { } .wire-x { }', use: 'el("div", "wire-xy wire")', unused: ['.wire-x'] },
  { why: 'a declaration is not a selector',
    css: '.a { color: #fff; background: url(#mask); }', use: 'class="a"', unused: [] },
  { why: 'attribute values and strings are not names',
    css: '.a[data-x=".ghost"], .a::after { content: "#nope"; }', use: 'class="a"', unused: [] },
  { why: 'keyframe steps and at-rule preludes are not selectors; rules inside an at-rule are',
    css: '@keyframes spin { 0% { opacity: 0 } 50.5% { opacity: 1 } } '
      + '@supports selector(.only-in-prelude) { .a { } .inner-dead { } }',
    use: 'class="a"', unused: ['.inner-dead'] },
  { why: 'a name inside :not() counts',
    css: '.a:not(.never) { }', use: 'class="a"', unused: ['.never'] },
  { why: 'a template-built class is used when its value is fed in',
    css: '.wire-lwt { } .wire-ghost { }', use: 'el("div", `wire wire-${m.dir}`); const D = { lwt: 1 };',
    unused: ['.wire-ghost'] },
  { why: 'a concatenated class is used when its value is fed in',
    css: '.mode-download { } .mode-flying { }', use: 'el("span", "mode mode-" + m); m = "download";',
    unused: ['.mode-flying'] },
  { why: 'a stylesheet is not a consumer of another',
    css: '.only-in-css { }', use: '', unused: ['.only-in-css'] },
];

export function selfTest() {
  const wrong = [];
  for (const p of PROBES) {
    const got = check([{ file: 'probe.css', text: p.css }], p.use, new Map())
      .unused.map((u) => u.replace(/^probe\.css: /, ''));
    if (JSON.stringify(got) !== JSON.stringify(p.unused)) {
      wrong.push(`${p.why}: expected ${JSON.stringify(p.unused)}, got ${JSON.stringify(got)}`);
    }
  }
  // The allow-list rules: an entry hides its name, and a stale entry fails.
  const allow = new Map([['.built-x', 'probe'], ['.gone', 'probe'], ['.a', 'probe']]);
  const r = check([{ file: 'p.css', text: '.a { } .built-x { }' }], 'class="a"', allow);
  if (r.unused.length) wrong.push(`an ALLOW entry hides its name: got ${JSON.stringify(r.unused)}`);
  const stale = r.stale.map((s) => s.split(':')[0]).sort();
  if (JSON.stringify(stale) !== JSON.stringify(['ALLOW .a', 'ALLOW .gone'])) {
    wrong.push(`a stale ALLOW entry fails: got ${JSON.stringify(r.stale)}`);
  }
  return wrong;
}

function main(argv) {
  const wrong = selfTest();
  if (wrong.length) {
    console.error('lint_css_selectors.mjs: the lint failed its own probes, so it cannot '
      + `vouch for the stylesheets:\n  ${wrong.join('\n  ')}`);
    return 1;
  }
  if (argv.includes('--self-test')) {
    console.log(`lint_css_selectors.mjs: ${PROBES.length + 2} probes OK`);
    return 0;
  }
  const sheets = readdirSync(ASSETS).filter((f) => f.endsWith('.css')).sort()
    .map((f) => ({ file: `canary-local/assets/${f}`, text: readFileSync(join(ASSETS, f), 'utf8') }));
  const consumerFiles = [
    ...readdirSync(LAB).filter((f) => f.endsWith('.html')).map((f) => join(LAB, f)),
    ...readdirSync(ASSETS).filter((f) => /\.m?js$/.test(f)).map((f) => join(ASSETS, f)),
  ];
  const consumers = consumerFiles.map((f) => readFileSync(f, 'utf8')).join('\n');
  const { unused, stale } = check(sheets, consumers);
  if (unused.length || stale.length) {
    if (unused.length) {
      console.error(`lint_css_selectors.mjs: ${unused.length} selector name(s) no page or script `
        + `uses:\n  ${unused.join('\n  ')}\nDelete the rule, or if a script builds the name from `
        + 'data, add it to ALLOW with where it is built.');
    }
    if (stale.length) console.error(`lint_css_selectors.mjs:\n  ${stale.join('\n  ')}`);
    return 1;
  }
  const n = sheets.reduce((acc, s) => acc + selectorNames(s.text).size, 0);
  console.log(`unused CSS OK — ${n} class/id names in ${sheets.length} stylesheets, each named by `
    + `one of ${consumerFiles.length} pages and scripts (${ALLOW.size} built from data, allowed)`);
  return 0;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  process.exitCode = main(process.argv.slice(2));
}
