// canary-local/tests/js_scan.js — the small JavaScript reader the source
// scans over the Lab's probes share (loaded by tests, never a test itself).
//
// csp.test.js holds every probe's waitForFunction to a function predicate
// and to options Playwright reads (sweep A44, A45); probe_server.test.js
// holds every probe's server to the tree's index (A46). Both need to cut a
// call into its arguments and to find the functions a file defines, past
// strings, template literals, comments and regex literals. This is that
// reader: bracket pairs and top-level commas, not a parser. What it cannot
// see, each scan refuses by name rather than trusts.

"use strict";

// A small reader of JavaScript: where each bracket closes and where each
// bracket's top-level commas sit, past strings, template literals, comments
// and regex literals. Enough to cut a call into its arguments; not a parser.
const readJsMemo = new Map();
function readJs(src) {
  if (readJsMemo.has(src)) return readJsMemo.get(src);
  const pairs = new Map();             // open index → close index
  const commas = new Map([[-1, []]]);  // open index (-1: top level) → its commas
  const semis = new Map([[-1, []]]);   // same, for semicolons
  const stack = [];                    // [kind, index]; kind "${" is a template substitution
  const REGEX_AFTER = "(,=:[!&|?{};+-*%<>~^";
  const REGEX_WORDS = new Set(["return", "typeof", "case", "void", "yield", "await", "in", "of", "delete", "throw", "else", "do"]);
  let prev = "", word = "";
  const template = (j) => {            // template text from j; the index code resumes at
    for (; j < src.length; j++) {
      if (src[j] === "\\") j++;
      else if (src[j] === "`") return j + 1;
      else if (src[j] === "$" && src[j + 1] === "{") { stack.push(["${", j + 1]); return j + 2; }
    }
    return src.length;
  };
  const quoted = (j, q) => {
    for (j++; j < src.length; j++) {
      if (src[j] === "\\") j++;
      else if (src[j] === q || src[j] === "\n") return j + 1;
    }
    return src.length;
  };
  const regex = (j) => {               // past the regex literal at j, or j when there is none
    let cls = false;
    for (let k = j + 1; k < src.length; k++) {
      const d = src[k];
      if (d === "\\") k++;
      else if (d === "\n") return j;
      else if (cls) { if (d === "]") cls = false; }
      else if (d === "[") cls = true;
      else if (d === "/") { k++; while (/[a-z]/i.test(src[k] || "")) k++; return k; }
    }
    return j;
  };
  const top = () => (stack.length ? stack[stack.length - 1][1] : -1);
  for (let i = 0; i < src.length;) {
    const c = src[i], n = src[i + 1];
    if (c === "/" && n === "/") { const e = src.indexOf("\n", i); i = e < 0 ? src.length : e; continue; }
    if (c === "/" && n === "*") { const e = src.indexOf("*/", i + 2); i = e < 0 ? src.length : e + 2; continue; }
    if (c === '"' || c === "'") { i = quoted(i, c); prev = c; word = ""; continue; }
    if (c === "`") { i = template(i + 1); prev = "`"; word = ""; continue; }
    if (c === "/" && (prev === "" || REGEX_AFTER.includes(prev) || REGEX_WORDS.has(word))) {
      const e = regex(i);
      if (e > i) { i = e; prev = "x"; word = ""; continue; }
    }
    if (c === "(" || c === "[" || c === "{") {
      stack.push([c, i]);
      commas.set(i, []);
      semis.set(i, []);
    } else if (c === ")" || c === "]" || c === "}") {
      const t = stack.pop();
      if (t && t[0] === "${") { i = template(i + 1); prev = "`"; word = ""; continue; }
      if (t) pairs.set(t[1], i);
    } else if (c === ",") commas.get(top()).push(i);
    else if (c === ";") semis.get(top()).push(i);
    if (/[\w$]/.test(c)) word = (/[\w$]/.test(src[i - 1] || "") ? word : "") + c;
    else if (!/\s/.test(c)) word = "";
    if (!/\s/.test(c)) prev = c;
    i++;
  }
  const read = { pairs, commas, semis };
  readJsMemo.set(src, read);
  return read;
}

const IDENT = String.raw`[A-Za-z_$][\w$]*`;
const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const lineOf = (src, at) => src.slice(0, at).split("\n").length;
// leading whitespace and comments off, trailing whitespace off
const trivia = (s) => s.replace(/^(?:\s|\/\/[^\n]*(?:\n|$)|\/\*[\s\S]*?\*\/)+/, "").trimEnd();

// The call's arguments, as source text, given the index of its "(".
function argsAt(src, open) {
  const { pairs, commas } = readJs(src);
  const close = pairs.get(open);
  if (close === undefined) return null;
  const cuts = [open, ...commas.get(open), close];
  const args = cuts.slice(1).map((to, k) => trivia(src.slice(cuts[k] + 1, to)));
  if (args.length && args[args.length - 1] === "") args.pop();   // f(a, b,) and f()
  return args;
}

// Every function in src: { name, params, from, to }. name is null for an
// unnamed one (a callback); params holds each plain parameter's name in its
// place (null for a destructured one); [from, to] spans parameters and body.
const functionsMemo = new Map();
function functionsIn(src) {
  if (functionsMemo.has(src)) return functionsMemo.get(src);
  const { pairs, commas, semis } = readJs(src);
  const out = [];
  const params = (open) => argsAt(src, open).map((p) => {
    const m = new RegExp(`^(?:\\.\\.\\.)?(${IDENT})\\s*(?:=|$)`).exec(p);
    return m ? m[1] : null;
  });
  const nameBefore = (at) => {
    const m = new RegExp(`(?:^|[^\\w$.])(${IDENT})\\s*[:=]\\s*(?:async\\s+)?$`).exec(src.slice(Math.max(0, at - 160), at));
    return m ? m[1] : null;
  };
  const enclosing = (at) => {
    let best = -1;
    for (const [o, c] of pairs) if (o < at && at < c && o > best) best = o;
    return best;
  };
  // an arrow's body: a block, or an expression to the next comma or
  // semicolon of the bracket around the arrow (or that bracket's close)
  const bodyEnd = (from, arrowAt) => {
    const b = /^\s*/.exec(src.slice(from))[0].length + from;
    if (src[b] === "{" && pairs.has(b)) return pairs.get(b);
    const o = enclosing(arrowAt);
    const ends = [...commas.get(o), ...semis.get(o)].filter((x) => x > from);
    return Math.min(o >= 0 ? pairs.get(o) : src.length, ...ends);
  };
  for (const [open] of pairs) {
    if (src[open] !== "(") continue;
    const close = pairs.get(open);
    const after = src.slice(close + 1);
    const arrow = /^\s*=>/.exec(after);
    const before = src.slice(Math.max(0, open - 160), open);
    const fnKw = new RegExp(`(?:^|[^\\w$.])function\\s*\\*?\\s*(${IDENT})?\\s*$`).exec(before);
    if (arrow) {
      const asyncAt = /async\s*$/.exec(before);
      const start = asyncAt ? open - asyncAt[0].length : open;
      out.push({ name: nameBefore(start), params: params(open), from: open, to: bodyEnd(close + 1 + arrow[0].length, open) });
    } else if (fnKw) {
      const brace = close + 1 + /^\s*/.exec(after)[0].length;
      if (src[brace] !== "{" || !pairs.has(brace)) continue;
      const start = open - (before.length - fnKw.index) + (fnKw[0].match(/^[^\w$]/) ? 1 : 0);
      const asyncAt = /async\s+$/.exec(src.slice(Math.max(0, start - 16), start));
      const name = fnKw[1] || nameBefore(asyncAt ? start - asyncAt[0].length : start);
      out.push({ name, params: params(open), from: open, to: pairs.get(brace) });
    }
  }
  // the one-parameter arrow without parentheses: x => ...
  for (const m of src.matchAll(new RegExp(`(?<![\\w$.])(${IDENT})\\s*=>`, "g"))) {
    const asyncAt = /async\s+$/.exec(src.slice(Math.max(0, m.index - 16), m.index));
    out.push({ name: nameBefore(asyncAt ? m.index - asyncAt[0].length : m.index), params: [m[1]], from: m.index,
      to: bodyEnd(m.index + m[0].length, m.index) });
  }
  functionsMemo.set(src, out);
  return out;
}

// The call's arguments with where each one starts in src (the index of its
// first character past leading whitespace and comments), given its "(".
function argSpansAt(src, open) {
  const { pairs, commas } = readJs(src);
  const close = pairs.get(open);
  if (close === undefined) return null;
  const cuts = [open, ...commas.get(open), close];
  const spans = cuts.slice(1).map((to, k) => {
    const raw = src.slice(cuts[k] + 1, to);
    const text = trivia(raw);
    return { text, at: cuts[k] + 1 + (raw.length - raw.replace(/^(?:\s|\/\/[^\n]*(?:\n|$)|\/\*[\s\S]*?\*\/)+/, "").length) };
  });
  if (spans.length && spans[spans.length - 1].text === "") spans.pop();   // f(a, b,) and f()
  return spans;
}

module.exports = { readJs, IDENT, esc, lineOf, trivia, argsAt, argSpansAt, functionsIn };
