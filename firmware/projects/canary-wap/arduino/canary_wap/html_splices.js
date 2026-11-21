'use strict';

// What a dashboard's script splices into markup unescaped (sweep F443).
//
// Both dashboards build rows as strings and hand them to innerHTML. A value
// spliced into such a string as it came is markup: a `<` in it opens an
// element, a quote in an on*= attribute ends the handler's string. F407,
// F422 and F423 moved the rows they touched to the escape-and-index pattern
// (escapeHtml() for text, a row index in place of a value in an onclick), and
// F443 is the rest. This module finds every splice into markup and names the
// ones that are neither; the suites that read it hold the two pages' sets to
// a recorded ledger that only shrinks:
//
//   web_ui_logic.test.js                 the WAP's web_ui.h (viewer.yml)
//   firmware/tests_host/test_canary_mesh_alerts.test.js
//                                        the flagship's securacv_webui.cpp
//                                        (firmware.yml)
//
// What counts as markup-bound. An expression is bound for markup when it is
//   * the right side of `.innerHTML =` / `+=` or `.outerHTML =` / `+=` (or
//     the same through `['innerHTML']`), the markup argument of
//     insertAdjacentHTML(), or an argument of document.write(); or
//   * an expression whose static text holds markup (`<` and a letter, `/` or
//     `!`), wherever it stands: a row a helper returns, a template split
//     across lines (it is one token). Its static text is its string literals
//     and templates, the templates spliced into those, an array literal's
//     elements, the arguments of a `.concat()` on any receiver, and the value
//     of a const or an accumulator (below) it names as a value (alone, or
//     before a `.concat()` or a `.join()`; not `h.length`, and not a const
//     that holds a function);
// and everything spliced into those, through ternaries, ||, ??, && (only
// its last operand can be text), parentheses, nested templates, an array
// literal (its elements; joined, with the separator between them),
// `R.concat(...)` (R, then each argument), String(), `.padStart()` and
// `.padEnd()` with a literal pad, and a `.map(cb).join(s)` (what cb
// returns), is markup-bound too.
//
// Markup built one statement at a time. A let, var, parameter or global is an
// accumulator when a sink takes it by name, or when any statement that builds
// it holds markup (its declaration's value, `X = ...`, `X += ...`,
// `X.push(...)`; naming another accumulator counts). Its statements are read
// in source order with one running HTML state: `X = ...` starts it afresh
// unless it is `X = X + ...` or `X = X.concat(...)`, and `+=` and push() carry
// it on. So `h += '<li>'; h += x.n;` reports x.n as text, and a handler that
// one statement opens holds the next statement to the handler's rule. An
// accumulator spliced by name into more markup carries its state there,
// inside an on*= handler it is a raw splice, and `X.join(s)` of an array rows
// were pushed onto is X's text.
//
// What a splice may be. Inside an on*= attribute only a row index: the
// attribute's text is decoded before the handler is compiled, so
// escapeHtml()'s &#39; is a quote again by then (F407). Elsewhere a row
// index or escapeHtml(...). Beside those, what cannot carry text of its own:
// a string or number literal (the static text itself), true/false/null, a
// comparison or a `!` (a boolean), arithmetic (`- * / % **`, bitwise, unary
// `-`/`+`/`~`), Math.*(), Number(), parseInt(), parseFloat() and
// `.toFixed()` (a number), typeof/void, and the branch of
// `typeof M === 'number' ? M : ...` (or `!==` with the branches swapped) that
// is M itself.
//
// What a name means. A bare name is its nearest binding: a parameter, a
// let/var/const (to the end of its block; a var to the end of its function)
// or a for-head name shadows an outer one of the same name. A row index is a
// name bound as the second parameter of a callback written inline as the
// first argument of `.map()`/`.forEach()` (a plain name: no name in a
// destructuring pattern is an index), or as the counter of an enclosing
// `for (let i = <number>; ...)`. A Map's or a Set's forEach passes a key or
// the value again there, not an index, and the scanner cannot tell those
// apart (neither page has a Map or a Set). A name bound `const` with a value
// stands for that value: what it holds is checked as if it were spliced where
// the name is (its markup moves the state the name lands in, so a const that
// opens a handler holds what follows to the handler's rule), and reported
// where it is written (so `const t = x.name; ... '<b>' + t` reports x.name,
// and a const of literals is no splice at all); a const array rows are pushed
// onto is an accumulator. Any other let, parameter or global is reported by
// its name.
//
// Anything else is a raw splice, reported with the page function it sits in,
// where it lands (`on:<attr>` for a handler, `attr:<name>`, `tag`, `text`)
// and its source text: a member (`peer.state`), a local (`icon`), a call to
// anything else (`truncateHash(...)`, an Array.join() of values the page did
// not map), an index.
//
// What it does not see. A value reaches markup only through the shapes
// above. A call to a page function at a sink (`= renderRows(d)`) is not
// itself a splice, and what a function's callers pass is not followed: a
// parameter a sink takes (`function show(el, html) { el.innerHTML = html; }`)
// is checked only where the function builds it, so `show(box, x.why)` is not
// seen. An accumulator's statements are read in source order whichever branch
// of an if they sit in, so a branch's state carries into the next branch.
// setAttribute(), an `.on<event> =` property, DOMParser,
// createContextualFragment and srcdoc are not sinks here (neither page puts
// markup or handler text through them). A style, src or href value is held to
// the text rule: escapeHtml() passes there, though it makes no URL or CSS
// safe. It reads JavaScript with a tokenizer, not a parser: a `/` after `)`
// is division, after `}` a regex. If that loses its place in a page (a
// template or ${} left open, unbalanced brackets) rawSplices() throws.

const MARKUP = /<[A-Za-z!/]/;
const ASSIGN = new Set(['=', '+=', '-=', '*=', '/=', '%=', '**=', '<<=', '>>=', '>>>=', '&=', '|=', '^=',
                        '&&=', '||=', '??=']);
const BEFORE_REGEX = new Set(['return', 'typeof', 'case', 'do', 'else', 'in', 'of', 'new', 'delete', 'void',
                              'throw', 'instanceof', 'yield', 'await']);
const PUNCT = ['>>>=', '...', '===', '!==', '**=', '<<=', '>>=', '>>>', '&&=', '||=', '??=', '=>', '==', '!=',
               '<=', '>=', '&&', '||', '??', '?.', '++', '--', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=',
               '<<', '>>', '**'];
// A statement that starts with one of these is not an expression.
const STATEMENT = new Set(['function', 'async', 'class', 'if', 'for', 'while', 'switch', 'try', 'catch', 'finally',
                           'const', 'let', 'var', 'return', 'throw', 'break', 'continue', 'else', 'do', 'case',
                           'default', 'import', 'export']);
const SAFE_IDS = new Set(['true', 'false', 'null', 'undefined', 'NaN', 'Infinity']);
const NUMERIC_CALLS = new Set(['Number', 'parseInt', 'parseFloat']);
const BOOLEAN_OPS = new Set(['===', '!==', '==', '!=', '<', '>', '<=', '>=', 'instanceof', 'in']);
const NUMERIC_OPS = new Set(['*', '/', '%', '**', '&', '|', '^', '<<', '>>', '>>>']);

// ── Tokens ────────────────────────────────────────────────────────────────

function cook(raw) {
  return raw.replace(/\\(u\{[0-9a-fA-F]+\}|u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|\r\n|[\s\S])/g, (m, e) => {
    if (e[0] === 'u' && e.length > 1) return String.fromCodePoint(parseInt(e[1] === '{' ? e.slice(2, -1) : e.slice(1), 16));
    if (e[0] === 'x' && e.length === 3) return String.fromCharCode(parseInt(e.slice(1), 16));
    return { n: '\n', t: '\t', r: '\r', b: '\b', f: '\f', v: '\v', 0: '\0', '\n': '', '\r\n': '' }[e] ?? e;
  });
}

// Tokens of src from `i`: { t: 'id'|'num'|'str'|'tpl'|'re'|'p', v, s, e }. A
// template carries its static parts (q, cooked) and its ${} tokens (x). With
// inBraces, stops at the `}` that closes the `${`; returns [tokens, end].
function tokenize(src, i = 0, inBraces = false) {
  const out = [];
  let depth = 0;
  const n = src.length;
  const prev = () => out[out.length - 1];
  const regexAllowed = () => {
    const p = prev();
    if (!p) return true;
    if (p.t === 'p') return p.v !== ')' && p.v !== ']' && p.v !== '++' && p.v !== '--';
    if (p.t === 'id') return BEFORE_REGEX.has(p.v);
    return false;
  };
  while (i < n) {
    const c = src[i];
    if (/\s/.test(c)) { i++; continue; }
    if (c === '/' && src[i + 1] === '/') { const j = src.indexOf('\n', i); i = j < 0 ? n : j; continue; }
    if (c === '/' && src[i + 1] === '*') { const j = src.indexOf('*/', i + 2); i = j < 0 ? n : j + 2; continue; }
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c && src[j] !== '\n') j += src[j] === '\\' ? 2 : 1;
      out.push({ t: 'str', v: cook(src.slice(i + 1, j)), s: i, e: j + 1 });
      i = j + 1;
      continue;
    }
    if (c === '`') {
      const q = [];
      const x = [];
      let j = i + 1;
      let k = j;
      while (j < n && src[j] !== '`') {
        if (src[j] === '\\') { j += 2; continue; }
        if (src[j] === '$' && src[j + 1] === '{') {
          q.push(cook(src.slice(k, j)));
          const [toks, end] = tokenize(src, j + 2, true);
          x.push({ tokens: toks, s: j + 2, e: end });
          j = end + 1;
          k = j;
          continue;
        }
        j++;
      }
      q.push(cook(src.slice(k, j)));
      out.push({ t: 'tpl', q, x, s: i, e: j + 1 });
      i = j + 1;
      continue;
    }
    if (/[A-Za-z_$]/.test(c)) {
      const m = /^[A-Za-z_$][\w$]*/.exec(src.slice(i, i + 200));
      out.push({ t: 'id', v: m[0], s: i, e: i + m[0].length });
      i += m[0].length;
      continue;
    }
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1] || ''))) {
      const m = /^(?:0[xXbBoO][0-9a-fA-F_]+|(?:\d[\d_]*)?\.?\d*(?:[eE][+-]?\d+)?)n?/.exec(src.slice(i, i + 100));
      const len = Math.max(1, m[0].length);
      out.push({ t: 'num', v: src.slice(i, i + len), s: i, e: i + len });
      i += len;
      continue;
    }
    if (c === '/' && regexAllowed()) {
      let j = i + 1;
      let cls = false;
      while (j < n && src[j] !== '\n') {
        if (src[j] === '\\') { j += 2; continue; }
        if (src[j] === '[') cls = true;
        else if (src[j] === ']') cls = false;
        else if (src[j] === '/' && !cls) break;
        j++;
      }
      j++;
      while (j < n && /[a-z]/i.test(src[j])) j++;
      out.push({ t: 're', v: src.slice(i, j), s: i, e: j });
      i = j;
      continue;
    }
    if (inBraces && c === '}' && depth === 0) return [out, i];
    const op = PUNCT.find((p) => src.startsWith(p, i)) || c;
    if (op === '{') depth++;
    else if (op === '}') depth--;
    out.push({ t: 'p', v: op, s: i, e: i + op.length });
    i += op.length;
  }
  return [out, n];
}

const isP = (tok, v) => tok && tok.t === 'p' && tok.v === v;
const isId = (tok, v) => tok && tok.t === 'id' && (v === undefined || tok.v === v);
const OPEN = { '(': ')', '[': ']', '{': '}' };

function closeOf(toks, i) {
  const open = toks[i].v;
  const close = OPEN[open];
  let d = 0;
  for (let j = i; j < toks.length; j++) {
    if (toks[j].t !== 'p') continue;
    if (toks[j].v === open) d++;
    else if (toks[j].v === close && --d === 0) return j;
  }
  return toks.length - 1;
}

// Indexes of the depth-0 tokens in [a, b) that satisfy pred.
function topLevel(toks, a, b, pred) {
  const at = [];
  for (let j = a; j < b; j++) {
    const tk = toks[j];
    if (tk.t === 'p' && OPEN[tk.v]) { j = closeOf(toks, j); continue; }
    if (pred(tk, j)) at.push(j);
  }
  return at;
}

// `x => ...`, `(a, b) => ...`: an arrow function, not a value.
function isArrow(toks, j) {
  if (isId(toks[j]) && isP(toks[j + 1], '=>')) return true;
  return isP(toks[j], '(') && isP(toks[closeOf(toks, j) + 1], '=>');
}

// Whether the value that starts at j is a function (`function`, an arrow).
function isFunctionValue(toks, j) {
  if (isId(toks[j], 'async')) j++;
  return isId(toks[j], 'function') || isArrow(toks, j);
}

// A `+` or `-` at j is binary when what precedes it ends an operand.
function endsOperand(tk) {
  if (!tk) return false;
  if (tk.t === 'id') return !BEFORE_REGEX.has(tk.v);
  if (tk.t === 'p') return tk.v === ')' || tk.v === ']' || tk.v === '}' || tk.v === '++' || tk.v === '--';
  return true;
}

// ── Where a splice lands ──────────────────────────────────────────────────

// Just enough of the HTML tokenizer to say whether a position is text, inside
// a tag, or inside which attribute's value.
function htmlState() { return { st: 'text', attr: '', q: '' }; }
function feed(h, text) {
  for (let k = 0; k < text.length; k++) {
    const c = text[k];
    switch (h.st) {
      case 'text':
        if (c === '<' && text.startsWith('<!--', k)) { h.st = 'comment'; k += 3; }
        else if (c === '<' && /[A-Za-z/]/.test(text[k + 1] || '')) h.st = 'tagname';
        break;
      case 'comment':
        if (text.startsWith('-->', k)) { h.st = 'text'; k += 2; }
        break;
      case 'tagname':
        if (/\s/.test(c)) h.st = 'tag';
        else if (c === '>') h.st = 'text';
        break;
      case 'tag':
        if (c === '>') h.st = 'text';
        else if (!/[\s/]/.test(c)) { h.st = 'attrname'; h.attr = c.toLowerCase(); }
        break;
      case 'attrname':
        if (c === '=') h.st = 'beforevalue';
        else if (c === '>') h.st = 'text';
        else if (/\s/.test(c)) h.st = 'afterattr';
        else h.attr += c.toLowerCase();
        break;
      case 'afterattr':
        if (c === '=') h.st = 'beforevalue';
        else if (c === '>') h.st = 'text';
        else if (!/\s/.test(c)) { h.st = 'attrname'; h.attr = c.toLowerCase(); }
        break;
      case 'beforevalue':
        if (c === '"' || c === "'") { h.st = 'value'; h.q = c; }
        else if (c === '>') h.st = 'text';
        else if (!/\s/.test(c)) { h.st = 'value'; h.q = ''; }
        break;
      case 'value':
        if (h.q ? c === h.q : /\s/.test(c)) h.st = 'tag';
        else if (!h.q && c === '>') h.st = 'text';
        break;
    }
  }
}
function where(h) {
  if (h.st === 'value' || h.st === 'beforevalue') return (/^on/.test(h.attr) ? 'on:' : 'attr:') + h.attr;
  if (h.st === 'text') return 'text';
  if (h.st === 'comment') return 'comment';
  return 'tag';
}
// A splice is a value: it moves an unquoted attribute from before to inside.
function feedValue(h) { if (h.st === 'beforevalue') { h.st = 'value'; h.q = ''; } }
const clone = (h) => ({ ...h });

// ── The page's script ─────────────────────────────────────────────────────

// The one <script> block of a page (both dashboards have one), with where it
// starts in the file.
function pageScript(text) {
  const a = text.indexOf('<script>');
  const b = a < 0 ? -1 : text.indexOf('</script>', a);
  if (a < 0 || b < 0) throw new Error('no <script> block in the page');
  return { code: text.slice(a + 8, b), at: a + 8 };
}

function eachList(toks, fn) {
  fn(toks);
  for (const tk of toks) if (tk.t === 'tpl') for (const x of tk.x) eachList(x.tokens, fn);
}

// Where the expression that starts at j ends: the first depth-0 `;`, `,` or
// closer.
function exprEnd(toks, j) {
  const stop = topLevel(toks, j, toks.length, (t) => t.t === 'p' && [';', ',', ')', ']', '}'].includes(t.v))[0];
  return stop === undefined ? toks.length : stop;
}

// The {...} block a token at j sits in, by offset (the whole list when none).
function blockOf(toks, j) {
  let depth = 0;
  for (let k = j - 1; k >= 0; k--) {
    if (isP(toks[k], '}')) depth++;
    else if (isP(toks[k], '{') && depth-- === 0) return [toks[k].s, toks[closeOf(toks, k)].e];
  }
  return [toks.length ? toks[0].s : 0, toks.length ? toks[toks.length - 1].e : 0];
}

// The `(` that a closer at i closes.
function openOf(toks, i) {
  const close = toks[i].v;
  const open = { ')': '(', ']': '[', '}': '{' }[close];
  let d = 0;
  for (let j = i; j >= 0; j--) {
    if (toks[j].t !== 'p') continue;
    if (toks[j].v === close) d++;
    else if (toks[j].v === open && --d === 0) return j;
  }
  return 0;
}

// The parameters between a function's parens, one per depth-0 parameter: a
// plain name (with or without a default) is { name }, and a destructuring
// pattern or a rest is { names } with no single name of its own, so none of
// them is a row index.
function paramList(list, a, b) {
  const out = [];
  let from = a;
  for (const c of [...topLevel(list, a, b, (t) => isP(t, ',')), b]) {
    if (c > from) {
      if (isId(list[from]) && (c - from === 1 || isP(list[from + 1], '='))) {
        out.push({ name: list[from].v, names: [list[from].v] });
      } else {
        const names = [];
        for (let m = from; m < c; m++) {
          if (isId(list[m]) && !isP(list[m - 1], '.') && !isP(list[m - 1], '?.') && !isP(list[m + 1], ':')) names.push(list[m].v);
        }
        out.push({ name: null, names });
      }
    }
    from = c + 1;
  }
  return out;
}

// Every function in the script, every name it binds and the extent it binds
// it over (the ledger's key is the named functions): a parameter, a
// let/var/const (to the end of its block, a var to the end of its function),
// a for-loop's own name, and the row indexes, each a binding of its own kind:
//   row    the second parameter of a .map()/.forEach() callback written
//          inline, or the counter of a `for (let i = <number>; ...)`;
//   const  a `const name = value` (it stands for its value);
//   let    a let or var, a for-of name, a destructured name;
//   param  any other parameter.
function scopes(toks) {
  const fns = [];
  const funcs = [];
  const binds = [];
  const bind = (b) => { binds.push(b); return b; };
  eachList(toks, (list) => {
    for (let j = 0; j < list.length; j++) {
      const tk = list[j];
      let fn = null;
      if (isId(tk, 'function')) {
        const p = isId(list[j + 1]) ? j + 2 : j + 1;
        if (isP(list[p], '(')) {
          const pc = closeOf(list, p);
          if (isP(list[pc + 1], '{')) {
            const bc = closeOf(list, pc + 1);
            if (p === j + 2) fns.push({ name: list[j + 1].v, s: tk.s, e: list[bc].e });
            fn = { start: j, params: paramList(list, p + 1, pc), s: list[p].s, e: list[bc].e };
          }
        }
      } else if (isP(tk, '=>')) {
        let start;
        let params;
        if (isP(list[j - 1], ')')) { start = openOf(list, j - 1); params = paramList(list, start + 1, j - 1); } else if (isId(list[j - 1])) { start = j - 1; params = paramList(list, j - 1, j); } else continue;
        const end = isP(list[j + 1], '{') ? closeOf(list, j + 1) : exprEnd(list, j + 1) - 1;
        fn = { start, params, s: list[start].s, e: (list[end] || list[list.length - 1]).e };
      }
      if (fn) {
        funcs.push([fn.s, fn.e]);
        // A callback written inline as the first argument of .map()/.forEach().
        const pre = isId(list[fn.start - 1], 'async') ? fn.start - 2 : fn.start - 1;
        const rowCb = isP(list[pre], '(') && isP(list[pre - 2], '.') &&
          (isId(list[pre - 1], 'map') || isId(list[pre - 1], 'forEach'));
        fn.params.forEach((p, k) => {
          for (const name of p.names) bind({ name, kind: rowCb && k === 1 && p.name ? 'row' : 'param', r0: fn.s, r1: fn.e });
        });
      }
      // for (let i = 0; ...) is a row counter; any other for-head name is a let.
      if (isId(tk, 'for') && isP(list[j + 1], '(') && isId(list[j + 2]) && ['let', 'var', 'const'].includes(list[j + 2].v)) {
        const head = closeOf(list, j + 1);
        const body = isP(list[head + 1], '{') ? closeOf(list, head + 1) : exprEnd(list, head + 1);
        const r1 = (list[body] || list[list.length - 1]).e;
        const counter = list[j + 2].v !== 'const' && isId(list[j + 3]) && isP(list[j + 4], '=') &&
          list[j + 5] && list[j + 5].t === 'num' && isP(list[j + 6], ';');
        if (isId(list[j + 3])) {
          bind({ name: list[j + 3].v, kind: counter ? 'row' : 'let', r0: list[j + 3].s, r1,
                 init: isP(list[j + 4], '=') ? { list, a: j + 5, b: exprEnd(list, j + 5) } : null });
        } else if (isP(list[j + 3], '{') || isP(list[j + 3], '[')) {
          for (const name of paramList(list, j + 3, closeOf(list, j + 3) + 1)[0].names) bind({ name, kind: 'let', r0: list[j + 3].s, r1 });
        }
      }
    }
  });
  // Declarations, once every function's extent is known (a var's scope).
  eachList(toks, (list) => {
    for (let j = 0; j < list.length; j++) {
      const tk = list[j];
      if (!isId(tk) || !['let', 'var', 'const'].includes(tk.v)) continue;
      if (isP(list[j - 1], '(') && isId(list[j - 2], 'for')) continue;
      let r1;
      if (tk.v === 'var') {
        let best = null;
        for (const f of funcs) if (f[0] <= tk.s && tk.s < f[1] && (!best || f[0] >= best[0])) best = f;
        r1 = best ? best[1] : Infinity;
      } else {
        r1 = blockOf(list, j)[1];
      }
      let k = j + 1;
      while (k < list.length) {
        if (isId(list[k])) {
          const init = isP(list[k + 1], '=') ? { list, a: k + 2, b: exprEnd(list, k + 2) } : null;
          const kind = tk.v === 'const' && init ? 'const' : 'let';
          bind({ name: list[k].v, kind, r0: list[k].s, r1, init, list, a: init && init.a, b: init && init.b });
          const next = init ? init.b : k + 1;
          if (isP(list[next], ',')) { k = next + 1; continue; }
        } else if (isP(list[k], '{') || isP(list[k], '[')) {
          const c = closeOf(list, k);
          for (const name of paramList(list, k, c + 1)[0].names) bind({ name, kind: 'let', r0: list[k].s, r1 });
          const next = isP(list[c + 1], '=') ? exprEnd(list, c + 2) : c + 1;
          if (isP(list[next], ',')) { k = next + 1; continue; }
        }
        break;
      }
    }
  });
  const byName = new Map();
  for (const b of binds) {
    if (!byName.has(b.name)) byName.set(b.name, []);
    byName.get(b.name).push(b);
  }
  return { fns, byName };
}

// ── The analysis ──────────────────────────────────────────────────────────

function analyzer(code, toks) {
  const { fns, byName } = scopes(toks);
  const globals = new Map();
  const resolving = new Set();
  const raw = [];
  const covered = [];
  const fnAt = (s) => {
    let best = null;
    for (const f of fns) if (f.s <= s && s < f.e && (!best || f.s >= best.s)) best = f;
    return best ? best.name : '(top level)';
  };
  // What a bare name at s refers to: the innermost binding of it whose extent
  // holds s (a nearer parameter or let shadows a const or a row index), or the
  // one global of that name.
  const bindingOf = (tk) => {
    let best = null;
    for (const b of byName.get(tk.v) || []) if (b.r0 <= tk.s && tk.s < b.r1 && (!best || b.r0 >= best.r0)) best = b;
    if (best) return best;
    if (!globals.has(tk.v)) globals.set(tk.v, { name: tk.v, kind: 'global' });
    return globals.get(tk.v);
  };
  const bare = (list, j) => isId(list[j]) && !isP(list[j - 1], '.') && !isP(list[j - 1], '?.');
  const isRowIndex = (tk) => tk.t === 'id' && bindingOf(tk).kind === 'row';
  const constOf = (tk) => {
    if (tk.t !== 'id') return null;
    const b = bindingOf(tk);
    return b.kind === 'const' ? b : null;
  };
  // Accumulators: a let, var, parameter or global that markup is built in,
  // statement by statement (`h += ...`), or that a sink takes by name. Filled
  // below, before anything is walked.
  const acc = new Set();
  const live = new Map();
  const accOf = (tk) => {
    if (tk.t !== 'id') return null;
    const b = bindingOf(tk);
    return acc.has(b) ? b : null;
  };
  const report = (list, a, b, h) => {
    const s = list[a].s;
    raw.push({ fn: fnAt(s), where: where(h), expr: code.slice(s, list[b - 1].e).replace(/\s+/g, ' '), at: s });
    feedValue(h);
  };
  const cover = (list, a, b) => { if (a < b) covered.push([list[a].s, list[b - 1].e]); };
  const textOf = (list, a, b) => code.slice(list[a].s, list[b - 1].e).replace(/\s+/g, '');

  // Whether the static text of [a, b) holds markup: its own string literals
  // and templates, the templates spliced into those, an array literal's
  // elements, the arguments of a .concat(), and the value of a const or an
  // accumulator it names; not the argument of any other call or a function
  // inside it (that is an expression of its own, found where it starts).
  function holdsMarkup(list, a, b, seen = new Set()) {
    for (let j = a; j < b; j++) {
      const t = list[j];
      if (t.t === 'str') { if (MARKUP.test(t.v)) return true; continue; }
      if (t.t === 'tpl') {
        if (t.q.some((q) => MARKUP.test(q)) || t.x.some((x) => holdsMarkup(x.tokens, 0, x.tokens.length, seen))) return true;
        continue;
      }
      if (isP(t, '[') && (j === a || !endsOperand(list[j - 1]))) {
        const c = closeOf(list, j);
        if (holdsMarkup(list, j + 1, c, seen)) return true;
        j = c;
        continue;
      }
      if (t.t === 'p' && OPEN[t.v]) { j = closeOf(list, j); continue; }
      if (isP(t, '.') && isId(list[j + 1], 'concat') && isP(list[j + 2], '(')) {
        const c = closeOf(list, j + 2);
        if (holdsMarkup(list, j + 3, c, seen)) return true;
        j = c;
        continue;
      }
      // A name counts as its value: standing alone, or as the receiver of a
      // .concat() or a .join(), never as the object of a member it reads
      // (`h.length`) or as a call (a const that holds a function is code).
      const next = list[j + 1];
      const asValue = !next || !(next.t === 'p' && ['.', '?.', '[', '(', '=>'].includes(next.v)) ||
        (isP(next, '.') && (isId(list[j + 2], 'concat') || isId(list[j + 2], 'join')) && isP(list[j + 3], '('));
      if (bare(list, j) && asValue) {
        const bd = bindingOf(t);
        if (acc.has(bd)) return true;
        if (bd.kind === 'const' && !seen.has(bd) && !isFunctionValue(bd.list, bd.a)) {
          seen.add(bd);
          if (holdsMarkup(bd.list, bd.a, bd.b, seen)) return true;
        }
      }
    }
    return false;
  }

  function template(tk, h) {
    covered.push([tk.s, tk.e]);
    for (let k = 0; k < tk.q.length; k++) {
      feed(h, tk.q[k]);
      if (k < tk.x.length) {
        const x = tk.x[k].tokens;
        expr(x, 0, x.length, h);
      }
    }
  }

  // What a .map() callback whose first token is at k returns (every
  // `return` in a block body, a function nested in it included). False when
  // the callback is not written inline (`rows.map(renderRow)`): what that
  // returns is not read here, so the whole operand is a raw splice.
  function callbackReturns(list, k, end, h) {
    let m = k;
    if (isId(list[m], 'async')) m++;
    if (isId(list[m], 'function')) {
      m++;
      if (isId(list[m])) m++;
    } else if (!isArrow(list, m)) {
      return false;
    }
    const params = isP(list[m], '(') ? closeOf(list, m) : m;
    let body = params + 1;
    if (isP(list[body], '=>')) body++;
    if (!isP(list[body], '{')) { expr(list, body, end, clone(h)); return true; }
    const close = closeOf(list, body);
    for (let r = body + 1; r < close; r++) {
      if (isId(list[r], 'return')) expr(list, r + 1, exprEnd(list, r + 1), clone(h));
    }
    return true;
  }

  // Each comma-separated item of [a, b), in order, into the same state.
  function items(list, a, b, h, between) {
    let from = a;
    const commas = topLevel(list, a, b, (t) => isP(t, ','));
    commas.forEach((c, k) => {
      expr(list, from, c, h);
      if (between && k < commas.length) between();
      from = c + 1;
    });
    expr(list, from, b, h);
  }

  // One operand: a primary and what follows it.
  function operand(list, a, b, h) {
    const first = list[a];
    const last = list[b - 1];
    if (b - a === 1) {
      if (first.t === 'str') { cover(list, a, b); feed(h, first.v); return; }
      if (first.t === 'tpl') { template(first, h); return; }
      if (first.t === 'num' || (first.t === 'id' && SAFE_IDS.has(first.v)) || first.t === 're') { feedValue(h); return; }
      if (isRowIndex(first)) { feedValue(h); return; }
      // An accumulator was checked statement by statement where it was
      // built; spliced by name it carries that markup's state, and inside an
      // on*= handler it is held to the handler's rule.
      const ab = accOf(first);
      if (ab) {
        if (where(h).startsWith('on:')) { report(list, a, b, h); return; }
        const st = live.get(ab);
        if (st && h.st === 'text') Object.assign(h, clone(st));
        else feedValue(h);
        return;
      }
      // A const stands for its value: what it holds is checked as if it were
      // spliced here (its markup moves this state too), and reported where it
      // is written.
      const c = first.t === 'id' ? constOf(first) : null;
      if (c && !resolving.has(c)) {
        resolving.add(c);
        expr(c.list, c.a, c.b, h);
        resolving.delete(c);
        feedValue(h);
        return;
      }
      report(list, a, b, h);
      return;
    }
    if (isP(first, '(') && closeOf(list, a) === b - 1) { cover(list, a, b); expr(list, a + 1, b - 1, h); return; }
    // An array literal: its elements in order; joined, with the separator
    // between them.
    if (isP(first, '[')) {
      const c = closeOf(list, a);
      const joined = isP(list[c + 1], '.') && isId(list[c + 2], 'join') && isP(list[c + 3], '(') && closeOf(list, c + 3) === b - 1;
      if (c === b - 1 || joined) {
        cover(list, a, b);
        const sep = joined && c + 4 < b - 1 ? () => expr(list, c + 4, b - 1, clone(h)) : null;
        items(list, a + 1, c, h, sep);
        return;
      }
    }
    if (isP(last, ')')) {
      const open = (() => { for (let j = b - 1; j >= a; j--) if (isP(list[j], '(') && closeOf(list, j) === b - 1) return j; return -1; })();
      if (open === a + 1 && isId(first, 'escapeHtml')) {
        if (where(h).startsWith('on:')) report(list, a, b, h);
        else feedValue(h);
        return;
      }
      if (open === a + 1 && NUMERIC_CALLS.has(first.v)) { feedValue(h); return; }
      // String(x) is x's text.
      if (open === a + 1 && isId(first, 'String') && open + 1 < b - 1) { cover(list, a, b); expr(list, open + 1, b - 1, h); return; }
      if (isId(first, 'Math') && isP(list[a + 1], '.') && open === a + 3) { feedValue(h); return; }
      if (open >= 2 && isId(list[open - 1], 'toFixed') && isP(list[open - 2], '.')) { feedValue(h); return; }
      // X.join(sep), X an accumulator rows were pushed onto: X's text.
      if (open === a + 3 && isId(list[a + 2], 'join') && isP(list[a + 1], '.') && accOf(first)) { operand(list, a, a + 1, h); return; }
      // R.concat(a, b, ...): R's text, then each argument's.
      if (open > a + 1 && isId(list[open - 1], 'concat') && isP(list[open - 2], '.')) {
        cover(list, a, b);
        expr(list, a, open - 2, h);
        items(list, open + 1, b - 1, h);
        return;
      }
      // R.padStart(n, 'c') / .padEnd(): R's text, padded with a literal.
      if (open > a + 1 && (isId(list[open - 1], 'padStart') || isId(list[open - 1], 'padEnd')) && isP(list[open - 2], '.') &&
          topLevel(list, open + 1, b - 1, (t) => !isP(t, ',')).every((k) => list[k].t === 'num' || (list[k].t === 'str' && !MARKUP.test(list[k].v)))) {
        cover(list, a, b);
        expr(list, a, open - 2, h);
        return;
      }
      // X.map(cb).join(sep): the rows cb returns, joined by sep.
      if (open >= 2 && isId(list[open - 1], 'join') && isP(list[open - 2], '.') && isP(list[open - 3], ')')) {
        const mapClose = open - 3;
        let mapOpen = -1;
        for (let j = mapClose; j >= a; j--) if (isP(list[j], '(') && closeOf(list, j) === mapClose) { mapOpen = j; break; }
        if (mapOpen > a + 1 && isId(list[mapOpen - 1], 'map') && isP(list[mapOpen - 2], '.') &&
            callbackReturns(list, mapOpen + 1, mapClose, h)) {
          expr(list, open + 1, b - 1, clone(h));
          feedValue(h);
          return;
        }
      }
    }
    report(list, a, b, h);
  }

  // `typeof M === 'number' ? M : ...` (or !== with the branches swapped):
  // the guarded branch, when it is M itself, is a number.
  function numberGuard(list, a, b) {
    if (isP(list[a], '(') && closeOf(list, a) === b - 1) return numberGuard(list, a + 1, b - 1);
    const ops = topLevel(list, a, b, (t) => t.t === 'p' && ['===', '==', '!==', '!='].includes(t.v));
    if (ops.length !== 1) return null;
    const op = ops[0];
    const then = list[op].v[0] === '=';
    const sides = [[a, op], [op + 1, b]];
    for (const [[ts, te], [ns, ne]] of [sides, [sides[1], sides[0]]]) {
      if (isId(list[ts], 'typeof') && te - ts > 1 && ne - ns === 1 && list[ns].t === 'str' && list[ns].v === 'number') {
        return { m: textOf(list, ts + 1, te), then };
      }
    }
    return null;
  }

  function expr(list, a, b, h) {
    if (a >= b) return;
    // Ternary: only its branches can be text.
    const q = topLevel(list, a, b, (t) => isP(t, '?'))[0];
    if (q !== undefined) {
      let depth = 0;
      let colon = -1;
      for (const j of topLevel(list, q + 1, b, (t) => isP(t, '?') || isP(t, ':'))) {
        if (isP(list[j], '?')) depth++;
        else if (depth === 0) { colon = j; break; } else depth--;
      }
      if (colon > 0) {
        cover(list, a, b);
        const g = numberGuard(list, a, q);
        const branch = (s, e, guarded) => { if (!(guarded && s < e && textOf(list, s, e) === g.m)) expr(list, s, e, clone(h)); };
        branch(q + 1, colon, g && g.then);
        branch(colon + 1, b, g && !g.then);
        feedValue(h);
        return;
      }
    }
    for (const ops of [['||', '??'], ['&&']]) {
      const at = topLevel(list, a, b, (t) => t.t === 'p' && ops.includes(t.v));
      if (at.length) {
        cover(list, a, b);
        const parts = [];
        let from = a;
        for (const j of at) { parts.push([from, j]); from = j + 1; }
        parts.push([from, b]);
        // a && b is a falsy a ("", 0, false, null, undefined, NaN) or b.
        for (const [s, e] of ops[0] === '&&' ? parts.slice(-1) : parts) expr(list, s, e, clone(h));
        feedValue(h);
        return;
      }
    }
    if (topLevel(list, a, b, (t) => (t.t === 'p' || t.t === 'id') && BOOLEAN_OPS.has(t.v)).length) { feedValue(h); return; }
    const adds = topLevel(list, a, b, (t, j) => t.t === 'p' && (t.v === '+' || t.v === '-') && j > a && endsOperand(list[j - 1]));
    if (adds.length) {
      cover(list, a, b);
      let from = a;
      const parts = [];
      for (const j of adds) { parts.push({ s: from, e: j, op: list[j].v }); from = j + 1; }
      parts.push({ s: from, e: b, op: null });
      // a - b is a number, and so is everything left of a `-`.
      const lastMinus = parts.map((p) => p.op).lastIndexOf('-');
      const start = lastMinus >= 0 ? lastMinus + 2 : 0;
      if (lastMinus >= 0) feedValue(h);
      for (let k = start; k < parts.length; k++) expr(list, parts[k].s, parts[k].e, h);
      return;
    }
    if (topLevel(list, a, b, (t, j) => t.t === 'p' && NUMERIC_OPS.has(t.v) && j > a).length) { feedValue(h); return; }
    const head = list[a];
    if (head.t === 'p' && ['!', '-', '+', '~'].includes(head.v)) { feedValue(h); return; }
    if (head.t === 'id' && ['typeof', 'void'].includes(head.v)) { feedValue(h); return; }
    if (head.t === 'id' && ['await', 'new'].includes(head.v)) { report(list, a, b, h); return; }
    operand(list, a, b, h);
  }

  function sinkRhs(list, a, b) {
    const h = htmlState();
    // = html: a let, var or parameter is an accumulator, checked where it is
    // built (a const is its value: see operand()).
    if (b - a === 1 && list[a].t === 'id' && !constOf(list[a])) return;
    if (isId(list[a]) && isP(list[a + 1], '(') && closeOf(list, a + 1) === b - 1 && !isId(list[a], 'escapeHtml')) return;
    expr(list, a, b, h);
  }

  const roots = [];
  const pieces = [];
  const sinkBound = new Set();
  const sink = (list, a, b) => {
    roots.push({ list, a, b, sink: true });
    if (b - a === 1 && bare(list, a)) sinkBound.add(bindingOf(list[a]));
  };
  eachList(toks, (list) => {
    for (let j = 0; j < list.length; j++) {
      const tk = list[j];
      // el.innerHTML = / += ..., el['innerHTML'] = ...
      if (isP(tk, '.') && (isId(list[j + 1], 'innerHTML') || isId(list[j + 1], 'outerHTML')) &&
          list[j + 2] && (isP(list[j + 2], '=') || isP(list[j + 2], '+='))) {
        sink(list, j + 3, exprEnd(list, j + 3));
      }
      if (isP(tk, '[') && list[j + 1] && list[j + 1].t === 'str' && ['innerHTML', 'outerHTML'].includes(list[j + 1].v) &&
          isP(list[j + 2], ']') && (isP(list[j + 3], '=') || isP(list[j + 3], '+='))) {
        sink(list, j + 4, exprEnd(list, j + 4));
      }
      if (isP(tk, '.') && isId(list[j + 1], 'insertAdjacentHTML') && isP(list[j + 2], '(')) {
        const close = closeOf(list, j + 2);
        const comma = topLevel(list, j + 3, close, (t) => isP(t, ','))[0];
        if (comma !== undefined) sink(list, comma + 1, close);
      }
      if (isId(tk, 'document') && isP(list[j + 1], '.') && (isId(list[j + 2], 'write') || isId(list[j + 2], 'writeln')) &&
          isP(list[j + 3], '(')) {
        sink(list, j + 4, closeOf(list, j + 3));
      }
      // What a let, var, parameter or global is built from, statement by
      // statement: its declaration's value, `X = ...`, `X += ...`, and
      // `X.push(...)`.
      if (bare(list, j) && !isId(list[j - 1], 'let') && !isId(list[j - 1], 'var') && !isId(list[j - 1], 'const') && !isP(list[j - 1], ',')) {
        if (isP(list[j + 1], '=') || isP(list[j + 1], '+=')) {
          pieces.push({ bind: bindingOf(tk), op: list[j + 1].v, list, a: j + 2, b: exprEnd(list, j + 2), at: tk.s });
        } else if (isP(list[j + 1], '.') && isId(list[j + 2], 'push') && isP(list[j + 3], '(')) {
          pieces.push({ bind: bindingOf(tk), op: 'push', list, a: j + 4, b: closeOf(list, j + 3), at: tk.s, from: j });
        }
      }
      if (isId(tk) && ['let', 'var', 'const'].includes(tk.v) && !(isP(list[j - 1], '(') && isId(list[j - 2], 'for'))) {
        let k = j + 1;
        while (isId(list[k]) && isP(list[k + 1], '=')) {
          const e = exprEnd(list, k + 2);
          pieces.push({ bind: bindingOf(list[k]), op: '=', list, a: k + 2, b: e, at: list[k].s });
          if (!isP(list[e], ',')) break;
          k = e + 1;
        }
      }
      // An expression whose static text holds markup, from where it starts.
      const prev = list[j - 1];
      const starts = !prev || (prev.t === 'p' && (ASSIGN.has(prev.v) || ['(', '[', '{', ',', ';', '=>', ':', '?', '}', '&&', '||', '??'].includes(prev.v))) ||
        (prev.t === 'id' && ['return', 'throw', 'case', 'else', 'do'].includes(prev.v));
      if (starts && !(tk.t === 'p' && [')', ']', '}', ';', ',', '{'].includes(tk.v)) &&
          !(tk.t === 'id' && STATEMENT.has(tk.v)) && !isArrow(list, j)) {
        const end = exprEnd(list, j);
        // An assignment, an object key or a label: the value after it is an
        // expression of its own, found from where it starts.
        const top = topLevel(list, j, end, (t) => t.t === 'p' && (ASSIGN.has(t.v) || t.v === ':' || t.v === '?'));
        if (top.some((k) => ASSIGN.has(list[k].v)) ||
            (top.some((k) => list[k].v === ':') && !top.some((k) => list[k].v === '?'))) continue;
        if (end > j) roots.push({ list, a: j, b: end, sink: false });
      }
    }
  });

  // Which bindings are accumulators: one a sink takes by name, or one that
  // any statement building it gives markup (which may name another).
  const byBind = new Map();
  for (const p of pieces) {
    if (p.bind.kind === 'row') continue;
    if (!byBind.has(p.bind)) byBind.set(p.bind, []);
    byBind.get(p.bind).push(p);
  }
  // A const stands for its value (operand()), unless it is an array that
  // rows are pushed onto.
  for (const [b, ps] of byBind) if (b.kind === 'const' && !ps.some((p) => p.op === 'push')) byBind.delete(b);
  for (const b of sinkBound) if (b.kind !== 'row' && b.kind !== 'const') acc.add(b);
  for (let changed = true; changed;) {
    changed = false;
    for (const [b, ps] of byBind) {
      if (!acc.has(b) && ps.some((p) => p.a < p.b && holdsMarkup(p.list, p.a, p.b))) { acc.add(b); changed = true; }
    }
  }
  // Walk every accumulator's statements in source order, each binding with
  // its own running state: `X = ...` starts it afresh, unless it is
  // `X = X + ...` or `X = X.concat(...)`; `+=` and push() carry it on.
  const stream = pieces.filter((p) => acc.has(p.bind) && p.a < p.b).sort((x, y) => x.at - y.at);
  for (const p of stream) {
    const { list, a, b } = p;
    let h = live.get(p.bind);
    const self = bare(list, a) && bindingOf(list[a]) === p.bind;
    if (p.op === 'push') {
      h = h || htmlState();
      items(list, a, b, h);
    } else if (p.op === '+=') {
      h = h || htmlState();
      expr(list, a, b, h);
    } else if (self && h && isP(list[a + 1], '+') &&
               !topLevel(list, a, b, (t) => t.t === 'p' && ['?', '||', '&&', '??'].includes(t.v)).length) {
      expr(list, a + 2, b, h);
    } else if (self && h && isP(list[a + 1], '.') && isId(list[a + 2], 'concat') && isP(list[a + 3], '(') && closeOf(list, a + 3) === b - 1) {
      items(list, a + 4, b - 1, h);
    } else {
      h = htmlState();
      expr(list, a, b, h);
    }
    live.set(p.bind, h);
    // (X.push(...) is the statement itself: it is no root of its own.)
    covered.push(p.from === undefined ? [list[a].s, list[b - 1].e] : [list[p.from].s, list[b].e]);
  }

  const kept = roots.filter((r) => r.a < r.b && (r.sink || holdsMarkup(r.list, r.a, r.b)));
  kept.sort((x, y) => x.list[x.a].s - y.list[y.a].s || (y.sink - x.sink) || (y.list[y.b - 1].e - x.list[x.b - 1].e));
  const inside = (s, e) => covered.some(([cs, ce]) => cs <= s && e <= ce);
  for (const r of kept) {
    const s = r.list[r.a].s;
    const e = r.list[r.b - 1].e;
    if (inside(s, e)) continue;
    if (r.sink) sinkRhs(r.list, r.a, r.b);
    else expr(r.list, r.a, r.b, htmlState());
    covered.push([s, e]);
  }
  return raw;
}

// Whether the tokens kept their place in the script: every template closed by
// its backtick and every ${} by its brace, and the script's (), [] and {}
// balanced. A tokenizer that lost its place (a regex read as division, say)
// fails one of these, and rawSplices() refuses to answer rather than pass.
function lostPlace(code, toks) {
  const problems = [];
  const bal = { '(': 0, '[': 0, '{': 0 };
  const shut = { ')': '(', ']': '[', '}': '{' };
  eachList(toks, (list) => {
    for (const tk of list) {
      if (tk.t === 'tpl') {
        if (code[tk.e - 1] !== '`' || tk.e > code.length) problems.push(`a template at ${tk.s} is not closed`);
        for (const x of tk.x) if (code[x.e] !== '}') problems.push(`a \${} at ${x.s} is not closed`);
      }
    }
  });
  for (const tk of toks) {
    if (tk.t !== 'p') continue;
    if (bal[tk.v] !== undefined) bal[tk.v]++;
    else if (shut[tk.v]) bal[shut[tk.v]]--;
  }
  for (const [k, n] of Object.entries(bal)) if (n !== 0) problems.push(`${k} is off by ${n}`);
  return problems;
}

// Every raw splice in a page: { fn, where, expr, line } in source order.
function rawSplices(pageText) {
  const { code, at } = pageScript(pageText);
  const [toks] = tokenize(code);
  const lost = lostPlace(code, toks);
  if (lost.length) throw new Error(`html_splices.js lost its place in the script: ${lost.slice(0, 5).join('; ')}`);
  const found = analyzer(code, toks);
  // One splice is reached more than once when a const it sits in is used
  // (and is a root of its own): it is listed once, where it lands strictest.
  const rank = (w) => (w.startsWith('on:') ? 0 : w.startsWith('attr:') ? 1 : w === 'tag' ? 2 : 3);
  const best = new Map();
  for (const r of found) {
    const key = `${r.at}|${r.expr}`;
    const had = best.get(key);
    if (!had || rank(r.where) < rank(had.where)) best.set(key, r);
  }
  return [...best.values()].sort((x, y) => x.at - y.at).map((r) => (
    { fn: r.fn, where: r.where, expr: r.expr, line: pageText.slice(0, at + r.at).split('\n').length }));
}

// The ledger's form: "fn | where | expr" → how many times.
function tally(splices) {
  const t = {};
  for (const r of splices) {
    const k = `${r.fn} | ${r.where} | ${r.expr}`;
    t[k] = (t[k] || 0) + 1;
  }
  return t;
}

// What a page's set says against its ledger: the splices that are new (more
// than recorded) and the recorded ones that are gone (fewer than recorded).
function against(ledger, splices) {
  const now = tally(splices);
  const added = [];
  const gone = [];
  for (const [k, n] of Object.entries(now)) if (n > (ledger[k] || 0)) added.push(`${k} (${n} now, ${ledger[k] || 0} recorded)`);
  for (const [k, n] of Object.entries(ledger)) if ((now[k] || 0) < n) gone.push(`${k} (${now[k] || 0} now, ${n} recorded)`);
  return { added, gone };
}

module.exports = { rawSplices, tally, against, pageScript, tokenize, lostPlace };
