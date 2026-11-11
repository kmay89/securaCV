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
//   * the right side of `.innerHTML =`, `.innerHTML +=`, `.outerHTML =` /
//     `+=`, the markup argument of insertAdjacentHTML(), or an argument of
//     document.write(); or
//   * a template literal or a `+` (or `.concat()`) chain whose static text
//     holds markup (`<` and a letter, `/` or `!`), wherever it stands: a row
//     a helper returns, a string built with `+=` before it is assigned, a
//     template split across lines (it is one token);
// and everything spliced into those, through ternaries, ||, ??, && (only
// its last operand can be text), parentheses, nested templates and a
// `.map(cb).join(s)` (what cb returns), is markup-bound too.
//
// What a splice may be. Inside an on*= attribute only a row index: the
// attribute's text is decoded before the handler is compiled, so
// escapeHtml()'s &#39; is a quote again by then (F407). Elsewhere a row
// index or escapeHtml(...). Beside those, what cannot carry text of its own:
// a string or number literal (the static text itself), true/false/null, a
// comparison or a `!` (a boolean), arithmetic (`- * / % **`, bitwise, unary
// `-`/`+`/`~`), Math.*(), Number(), parseInt(), parseFloat() and
// `.toFixed()` (a number), and typeof/void. A row index is a bare identifier
// that is the second parameter of an enclosing `.map()`/`.forEach()`
// callback, or the counter of an enclosing `for (let i = 0; ...)`.
//
// A bare name declared `const` stands for its value: what it holds is checked
// as if it were spliced where the name is, and reported where it is written
// (so `const t = x.name; ... '<b>' + t` reports x.name, and a const of
// literals is no splice at all). A `let`, a parameter or a global is reported
// by its name.
//
// Anything else is a raw splice, reported with the page function it sits in,
// where it lands (`on:<attr>` for a handler, `attr:<name>`, `tag`, `text`)
// and its source text: a member (`peer.state`), a local (`icon`), a call to
// anything else (`truncateHash(...)`), an index.
//
// What it does not see. A value reaches markup only through the shapes
// above. The right side of a sink that is a `let` or a parameter, or a call
// to a page function (`list.innerHTML = html`, `= renderRows(d)`), is not
// itself a splice: what built it is checked where it was written when that
// holds markup, so a `let` built with none (`let t = x.name; ...
// el.innerHTML = t`) is not seen. Array.join() of values the page did not
// map is a call, so a raw splice; DOMParser and createContextualFragment are
// not sinks here (neither page uses them). A style, src or href value is held
// to the text rule: escapeHtml() passes there, though it makes no URL or CSS
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

// Named functions (the ledger's key), row-index bindings and `const`
// declarations, by offset.
function scopes(toks) {
  const fns = [];
  const rows = [];
  const consts = [];
  eachList(toks, (list) => {
    for (let j = 0; j < list.length; j++) {
      const tk = list[j];
      if (isId(tk, 'function') && isId(list[j + 1]) && isP(list[j + 2], '(')) {
        const pc = closeOf(list, j + 2);
        if (isP(list[pc + 1], '{')) fns.push({ name: list[j + 1].v, s: tk.s, e: list[closeOf(list, pc + 1)].e });
      }
      if (isP(tk, '.') && (isId(list[j + 1], 'map') || isId(list[j + 1], 'forEach')) && isP(list[j + 2], '(')) {
        const call = closeOf(list, j + 2);
        let k = j + 3;
        if (isId(list[k], 'async')) k++;
        if (isId(list[k], 'function')) k++;
        if (isId(list[k]) && isP(list[k + 1], '(')) k++;
        if (isP(list[k], '(')) {
          const names = [];
          let expectName = true;
          for (let m = k + 1; m < closeOf(list, k); m++) {
            if (isP(list[m], ',')) { expectName = true; continue; }
            if (expectName && isId(list[m])) names.push(list[m].v);
            expectName = false;
          }
          if (names.length >= 2) rows.push({ name: names[1], s: list[j].s, e: list[call].e });
        }
      }
      if (isId(tk, 'for') && isP(list[j + 1], '(') && isId(list[j + 2]) && ['let', 'var'].includes(list[j + 2].v) &&
          isId(list[j + 3]) && isP(list[j + 4], '=') && list[j + 5] && list[j + 5].t === 'num' && isP(list[j + 6], ';')) {
        const head = closeOf(list, j + 1);
        const body = isP(list[head + 1], '{') ? closeOf(list, head + 1) : exprEnd(list, head + 1);
        rows.push({ name: list[j + 3].v, s: tk.s, e: (list[body] || list[list.length - 1]).e });
      }
      if (isId(tk, 'const') && isId(list[j + 1]) && isP(list[j + 2], '=')) {
        consts.push({ name: list[j + 1].v, s: list[j + 1].s, list, a: j + 3, b: exprEnd(list, j + 3), block: blockOf(list, j) });
      }
    }
  });
  return { fns, rows, consts };
}

// Whether the static text of [a, b) holds markup: its own string literals and
// templates, and the templates spliced into those, not an argument of a call
// or a function inside it (that is an expression of its own, found where it
// starts).
function holdsMarkup(toks, a, b) {
  for (const j of topLevel(toks, a, b, (t) => t.t === 'str' || t.t === 'tpl')) {
    const t = toks[j];
    if (t.t === 'str' && MARKUP.test(t.v)) return true;
    if (t.t === 'tpl' && (t.q.some((q) => MARKUP.test(q)) || t.x.some((x) => holdsMarkup(x.tokens, 0, x.tokens.length)))) return true;
    // 'text'.concat(...): its arguments are its text too.
    if (isP(toks[j + 1], '.') && isId(toks[j + 2], 'concat') && isP(toks[j + 3], '(') &&
        holdsMarkup(toks, j + 4, closeOf(toks, j + 3))) return true;
  }
  return false;
}

// ── The analysis ──────────────────────────────────────────────────────────

function analyzer(code, toks) {
  const { fns, rows, consts } = scopes(toks);
  const resolving = new Set();
  const raw = [];
  const covered = [];
  const fnAt = (s) => {
    let best = null;
    for (const f of fns) if (f.s <= s && s < f.e && (!best || f.s >= best.s)) best = f;
    return best ? best.name : '(top level)';
  };
  const isRowIndex = (tk) => tk.t === 'id' && rows.some((r) => r.name === tk.v && r.s <= tk.s && tk.s < r.e);
  // The `const` a bare name refers to: the nearest one before it in a block
  // that holds it.
  const constOf = (tk) => {
    let best = null;
    for (const c of consts) {
      if (c.name === tk.v && c.s < tk.s && c.block[0] <= tk.s && tk.s < c.block[1] && (!best || c.s > best.s)) best = c;
    }
    return best;
  };
  const report = (list, a, b, h) => {
    const s = list[a].s;
    raw.push({ fn: fnAt(s), where: where(h), expr: code.slice(s, list[b - 1].e).replace(/\s+/g, ' '), at: s });
    feedValue(h);
  };
  const cover = (list, a, b) => { if (a < b) covered.push([list[a].s, list[b - 1].e]); };

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

  // One operand: a primary and what follows it.
  function operand(list, a, b, h) {
    const first = list[a];
    const last = list[b - 1];
    if (b - a === 1) {
      if (first.t === 'str') { cover(list, a, b); feed(h, first.v); return; }
      if (first.t === 'tpl') { template(first, h); return; }
      if (first.t === 'num' || (first.t === 'id' && SAFE_IDS.has(first.v)) || first.t === 're') { feedValue(h); return; }
      if (isRowIndex(first)) { feedValue(h); return; }
      // A const stands for its value: what it holds is checked as if it were
      // spliced here, and reported where it is written.
      const c = first.t === 'id' ? constOf(first) : null;
      if (c && !resolving.has(c)) {
        resolving.add(c);
        expr(c.list, c.a, c.b, clone(h));
        resolving.delete(c);
        feedValue(h);
        return;
      }
      report(list, a, b, h);
      return;
    }
    if (isP(first, '(') && closeOf(list, a) === b - 1) { cover(list, a, b); expr(list, a + 1, b - 1, h); return; }
    // 'literal'.concat(a, b, ...)
    if ((first.t === 'str' || first.t === 'tpl') && isP(list[a + 1], '.') && isId(list[a + 2], 'concat') &&
        isP(list[a + 3], '(') && closeOf(list, a + 3) === b - 1) {
      cover(list, a, b);
      operand(list, a, a + 1, h);
      const close = b - 1;
      const commas = topLevel(list, a + 4, close, (t) => isP(t, ','));
      let from = a + 4;
      for (const c of [...commas, close]) { expr(list, from, c, h); from = c + 1; }
      return;
    }
    if (isP(last, ')')) {
      const open = (() => { for (let j = b - 1; j >= a; j--) if (isP(list[j], '(') && closeOf(list, j) === b - 1) return j; return -1; })();
      if (open === a + 1 && isId(first, 'escapeHtml')) {
        if (where(h).startsWith('on:')) report(list, a, b, h);
        else feedValue(h);
        return;
      }
      if (open === a + 1 && NUMERIC_CALLS.has(first.v)) { feedValue(h); return; }
      if (isId(first, 'Math') && isP(list[a + 1], '.') && open === a + 3) { feedValue(h); return; }
      if (open >= 2 && isId(list[open - 1], 'toFixed') && isP(list[open - 2], '.')) { feedValue(h); return; }
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
        expr(list, q + 1, colon, clone(h));
        expr(list, colon + 1, b, clone(h));
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
    // = html, a let built elsewhere (a const is its value: see operand()).
    if (b - a === 1 && list[a].t === 'id' && !constOf(list[a])) return;
    if (isId(list[a]) && isP(list[a + 1], '(') && closeOf(list, a + 1) === b - 1 && !isId(list[a], 'escapeHtml')) return;
    expr(list, a, b, h);
  }

  const roots = [];
  eachList(toks, (list) => {
    for (let j = 0; j < list.length; j++) {
      const tk = list[j];
      if (isP(tk, '.') && (isId(list[j + 1], 'innerHTML') || isId(list[j + 1], 'outerHTML')) &&
          list[j + 2] && (isP(list[j + 2], '=') || isP(list[j + 2], '+='))) {
        roots.push({ list, a: j + 3, b: exprEnd(list, j + 3), sink: true });
      }
      if (isP(tk, '.') && isId(list[j + 1], 'insertAdjacentHTML') && isP(list[j + 2], '(')) {
        const close = closeOf(list, j + 2);
        const comma = topLevel(list, j + 3, close, (t) => isP(t, ','))[0];
        if (comma !== undefined) roots.push({ list, a: comma + 1, b: close, sink: true });
      }
      if (isId(tk, 'document') && isP(list[j + 1], '.') && (isId(list[j + 2], 'write') || isId(list[j + 2], 'writeln')) &&
          isP(list[j + 3], '(')) {
        roots.push({ list, a: j + 4, b: closeOf(list, j + 3), sink: true });
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
        if (end > j && holdsMarkup(list, j, end)) roots.push({ list, a: j, b: end, sink: false });
      }
    }
  });
  roots.sort((x, y) => x.list[x.a].s - y.list[y.a].s || (y.sink - x.sink) || (y.list[y.b - 1].e - x.list[x.b - 1].e));
  const inside = (s, e) => covered.some(([cs, ce]) => cs <= s && e <= ce);
  for (const r of roots) {
    if (r.a >= r.b) continue;
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
