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
// prelude; never out of a string or a comment), and requires each name to be
// USED: written as a whole token in a page
// (canary-local/*.html) or a first-party script
// (canary-local/assets/*.js, *.mjs). Those are the only files that put a
// class or id on an element the stylesheets reach; nested pages
// (voice/, witness/, the emulator harness) load stylesheets of their own.
//
// The stylesheet is read in ONE pass that knows strings (with their backslash
// escapes) and comments, so a quote inside a comment or a comment opener
// inside a string is text, and cannot make the rules after it vanish. What
// that pass cannot read fails the lint instead of being skipped: a string or
// a comment that never closes, a brace with no partner, a selector with a
// backslash escape, a sheet set with no class or id selector at all, and no
// page or script to read names from.
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

/** Read a stylesheet in one pass: comments (outside strings) and strings
 * (with their backslash escapes, outside comments) are inert, so a quote in a
 * comment or a comment opener in a string cannot hide the rules after it.
 * Returns {names: Set of ".x"/"#x" from rule selectors, problems: [why]}.
 * What the pass cannot read is a problem, never a silent skip: a string or a
 * comment that never closes, a brace with no partner, and a backslash escape
 * in a selector (the name it spells is not the bytes a page writes). */
export function parseSheet(css) {
  const names = new Set();
  const problems = [];
  const line = (at) => css.slice(0, at).split('\n').length;
  let buf = '';
  let depth = 0;
  let i = 0;
  while (i < css.length) {
    const ch = css[i];
    if (ch === '/' && css[i + 1] === '*') {
      const end = css.indexOf('*/', i + 2);
      if (end < 0) {
        problems.push(`line ${line(i)}: a comment that never closes`);
        return { names, problems };
      }
      buf += ' ';
      i = end + 2;
      continue;
    }
    if (ch === '"' || ch === "'") {
      // A string holds no name (`[href$=".pdf"]`, `content: "#nope"`). It
      // ends at its own unescaped quote; a newline first is a bad string.
      let j = i + 1;
      while (j < css.length && css[j] !== ch && css[j] !== '\n') j += css[j] === '\\' ? 2 : 1;
      if (j >= css.length || css[j] !== ch) {
        problems.push(`line ${line(i)}: a string that never closes`);
        return { names, problems };
      }
      buf += ' ';
      i = j + 1;
      continue;
    }
    if (ch === '\\') {
      buf += css.slice(i, i + 2);
      i += 2;
      continue;
    }
    if (ch === '{') {
      depth += 1;
      const prelude = buf.trim();
      buf = '';
      // An at-rule's prelude is not a selector (@supports selector(.x) names
      // .x without styling it); the rules inside it are read as usual, and a
      // keyframe step (from, 50%) holds no name.
      if (!prelude.startsWith('@')) {
        if (prelude.includes('\\')) {
          problems.push(`line ${line(i)}: a backslash escape in the selector "${prelude}"`);
        }
        for (const m of prelude.matchAll(NAME)) names.add(m[1] + m[2]);
      }
    } else if (ch === '}') {
      depth -= 1;
      if (depth < 0) {
        problems.push(`line ${line(i)}: a "}" with no "{"`);
        return { names, problems };
      }
      buf = '';
    } else if (ch === ';') {
      buf = '';
    } else {
      buf += ch;
    }
    i += 1;
  }
  if (depth > 0) problems.push(`${depth} "{" never closed`);
  return { names, problems };
}

/** The class and id names in a stylesheet's selectors, as ".x" / "#x". */
export const selectorNames = (css) => parseSheet(css).names;

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
 * Returns {unused: ["file: .x"], stale: ["ALLOW .x: why"],
 * unreadable: ["file: why"]}; a sheet the parser cannot read is unreadable,
 * and so is an empty set (no stylesheet, no selector name, no consumer). */
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
  const unreadable = [];
  const seen = new Set();
  if (!sheets.length) unreadable.push('no stylesheet found');
  if (!consumers.trim()) unreadable.push('no page or script found to read names from');
  for (const { file, text } of sheets) {
    const { names, problems } = parseSheet(text);
    for (const why of problems) unreadable.push(`${file}: ${why}`);
    for (const name of [...names].sort()) {
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
  if (sheets.length && !seen.size) unreadable.push('no class or id selector found in any stylesheet');
  return { unused, stale, unreadable };
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
    css: '.only-in-css { }', use: 'class="a"', unused: ['.only-in-css'] },
  // Strings and comments are read in one pass: neither can hide what follows.
  { why: 'an escaped double quote in a string does not end it',
    css: '.a::after { content: "\\""; } .dead-after-dq { }', use: 'class="a"',
    unused: ['.dead-after-dq'] },
  { why: 'an escaped single quote in a string does not end it',
    css: ".a::after { content: '\\''; } .dead-after-sq { }", use: 'class="a"',
    unused: ['.dead-after-sq'] },
  { why: 'a comment opener in a string is text',
    css: '.a::after { content: "/*"; } .dead-one { } /* a real comment */ .dead-two { }',
    use: 'class="a"', unused: ['.dead-one', '.dead-two'] },
  { why: 'a quote in a comment is text',
    css: "/* don't */ .a { } /* \" */ .dead-q { }", use: 'class="a"', unused: ['.dead-q'] },
  { why: 'a name in a comment inside a selector is not a selector',
    css: '.a /* .ghost */, .dead-c { }', use: 'class="a"', unused: ['.dead-c'] },
  // What the pass cannot read is refused, never skipped.
  { why: 'a string that never closes is refused',
    css: '.a::after { content: "x; } .dead { }', use: 'class="a"', unreadable: 'never closes' },
  { why: 'a comment that never closes is refused',
    css: '.a { } /* .dead { }', use: 'class="a"', unreadable: 'never closes' },
  { why: 'a brace with no partner is refused',
    css: '.a { color: red; } } .dead { }', use: 'class="a"', unreadable: 'with no' },
  { why: 'a rule that never closes is refused',
    css: '.a { color: red;', use: 'class="a"', unreadable: 'never closed' },
  { why: 'an escaped selector is refused',
    css: '.md\\:wide { }', use: 'md:wide', unreadable: 'backslash escape' },
  { why: 'a sheet with no class or id selector is refused',
    css: 'div { color: red; }', use: 'class="a"', unreadable: 'no class or id selector' },
  { why: 'no page or script to read names from is refused',
    css: '.a { }', use: '', unreadable: 'no page or script' },
];

export function selfTest() {
  const wrong = [];
  for (const p of PROBES) {
    const r = check([{ file: 'probe.css', text: p.css }], p.use, new Map());
    const got = r.unused.map((u) => u.replace(/^probe\.css: /, ''));
    if (p.unreadable) {
      if (!r.unreadable.some((u) => u.includes(p.unreadable))) {
        wrong.push(`${p.why}: expected a refusal naming "${p.unreadable}", got `
          + `${JSON.stringify(r.unreadable)}`);
      }
    } else if (r.unreadable.length) {
      wrong.push(`${p.why}: expected it read, got ${JSON.stringify(r.unreadable)}`);
    } else if (JSON.stringify(got) !== JSON.stringify(p.unused)) {
      wrong.push(`${p.why}: expected ${JSON.stringify(p.unused)}, got ${JSON.stringify(got)}`);
    }
  }
  if (!check([], 'class="a"').unreadable.includes('no stylesheet found')) {
    wrong.push('no stylesheet at all is refused');
  }
  // The verdict main prints: an unreadable sheet fails it, even when every
  // name it could read is used; a clean set passes.
  const v = (css) => verdict([{ file: 'v.css', text: css }], 'class="a"', new Map()).errors.length;
  if (!v('.a { } .a::after { content: "x; }') || v('.a { }')) {
    wrong.push('the verdict fails an unreadable sheet and passes a clean one');
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

/** What the lint says about `sheets` and `consumers`: {errors: [message],
 * names: n}. Any unreadable sheet, unused name or stale ALLOW entry is an
 * error; an unreadable sheet is reported alone (its names are not trusted). */
export function verdict(sheets, consumers, allow = ALLOW) {
  const { unused, stale, unreadable } = check(sheets, consumers, allow);
  const names = sheets.reduce((acc, s) => acc + selectorNames(s.text).size, 0);
  if (unreadable.length) {
    return { names, errors: ['lint_css_selectors.mjs: cannot read the stylesheets, so it cannot '
      + `vouch for them:\n  ${unreadable.join('\n  ')}`] };
  }
  const errors = [];
  if (unused.length) {
    errors.push(`lint_css_selectors.mjs: ${unused.length} selector name(s) no page or script `
      + `uses:\n  ${unused.join('\n  ')}\nDelete the rule, or if a script builds the name from `
      + 'data, add it to ALLOW with where it is built.');
  }
  if (stale.length) errors.push(`lint_css_selectors.mjs:\n  ${stale.join('\n  ')}`);
  return { names, errors };
}

function main(argv) {
  const wrong = selfTest();
  if (wrong.length) {
    console.error('lint_css_selectors.mjs: the lint failed its own probes, so it cannot '
      + `vouch for the stylesheets:\n  ${wrong.join('\n  ')}`);
    return 1;
  }
  if (argv.includes('--self-test')) {
    console.log(`lint_css_selectors.mjs: ${PROBES.length + 4} probes OK`);
    return 0;
  }
  const sheets = readdirSync(ASSETS).filter((f) => f.endsWith('.css')).sort()
    .map((f) => ({ file: `canary-local/assets/${f}`, text: readFileSync(join(ASSETS, f), 'utf8') }));
  const consumerFiles = [
    ...readdirSync(LAB).filter((f) => f.endsWith('.html')).map((f) => join(LAB, f)),
    ...readdirSync(ASSETS).filter((f) => /\.m?js$/.test(f)).map((f) => join(ASSETS, f)),
  ];
  const consumers = consumerFiles.map((f) => readFileSync(f, 'utf8')).join('\n');
  const { errors, names } = verdict(sheets, consumers);
  if (errors.length) {
    console.error(errors.join('\n'));
    return 1;
  }
  console.log(`unused CSS OK — ${names} class/id names in ${sheets.length} stylesheets, each named `
    + `by one of ${consumerFiles.length} pages and scripts (${ALLOW.size} built from data, allowed)`);
  return 0;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  process.exitCode = main(process.argv.slice(2));
}
