'use strict';

// What a browser makes of an innerHTML string, for the page tests beside this
// file (web_ui_logic.test.js, dashboard_gates.test.js). Test support only: the
// sketch compiles none of it, and nothing here is served.
//
//   parseFragment(html)  the HTML tokenizer's data, tag, attribute, comment and
//                        character-reference states (WHATWG HTML 13.2.5), and
//                        enough of tree building to give each element its
//                        text: every element and attribute a browser would
//                        create, every comment, and the text as it reads.
//   textElement()        a <div> whose textContent goes in and innerHTML comes
//                        out serialized as a browser serializes a text node
//                        (13.3: & U+00A0 < > escaped, quotes kept), the pair
//                        the pages' escapeHtml() and escapeHTML() rely on.
//   HOSTILE              the text the tests feed every field they draw.
//
// It covers what an innerHTML assignment of a page's own markup can hold; it
// does no foster parenting, no implied end tags and no raw-text elements
// (script, style, textarea, title). A test that finds one of those in a
// fragment has already failed on the element itself. When it was written, its
// reading of every fragment these tests draw, from the pages before and after
// sweep F296, F401, F405 and F407, matched Chromium's element for element
// (a local Playwright run; CI runs only the Node suites).

const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta',
  'source', 'track', 'wbr']);
// The named references the pages' markup and escapers write. Those with a
// legacy form also decode without their ';' (outside an attribute, or before a
// character that is not '=' or alphanumeric inside one). Any other name reads
// as the text it is, which a test comparing text would show.
const NAMED = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: '\u00a0', mdash: '\u2014',
  ndash: '\u2013' };
const LEGACY = new Set(['amp', 'lt', 'gt', 'quot', 'nbsp']);

const isWs = (ch) => ch === '\t' || ch === '\n' || ch === '\f' || ch === '\r' || ch === ' ';
const isAlpha = (ch) => ch !== undefined && /[A-Za-z]/.test(ch);

// The character reference at s[i] ('&'): [what it reads as, the index after it].
function charRef(s, i, inAttr) {
  const rest = s.slice(i, i + 40);
  let m = /^&#[xX]([0-9a-fA-F]+);?/.exec(rest) || /^&#([0-9]+);?/.exec(rest);
  if (m) {
    const hex = /^&#[xX]/.test(rest);
    let cp = parseInt(m[1], hex ? 16 : 10);
    if (cp === 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    return [String.fromCodePoint(cp), i + m[0].length];
  }
  m = /^&([A-Za-z]+)(;?)/.exec(rest);
  if (m && NAMED[m[1]] !== undefined && (m[2] === ';' || LEGACY.has(m[1]))) {
    const after = s[i + m[0].length];
    if (m[2] === ';' || !inAttr || !(after === '=' || /[A-Za-z0-9]/.test(after || ''))) {
      return [NAMED[m[1]], i + m[0].length];
    }
  }
  return ['&', i + 1];
}

function decodeAttr(raw) {
  let out = '';
  for (let i = 0; i < raw.length;) {
    if (raw[i] === '&') { const [t, j] = charRef(raw, i, true); out += t; i = j; } else { out += raw[i++]; }
  }
  return out;
}

// A tag from its name's first character at s[i]: { name, attrs, next }, or
// null when the input ends inside it (a browser then emits nothing).
function readTag(s, i) {
  let name = '';
  while (i < s.length && !isWs(s[i]) && s[i] !== '/' && s[i] !== '>') name += s[i++].toLowerCase();
  const attrs = [];
  for (;;) {
    // before attribute name ('/' not before '>' is a parse error, skipped)
    while (i < s.length && (isWs(s[i]) || s[i] === '/')) i++;
    if (i >= s.length) return null;
    if (s[i] === '>') return { name, attrs, next: i + 1 };
    // attribute name: a leading '=' is part of it; quotes and '<' are kept
    let an = s[i++].toLowerCase();
    while (i < s.length && !isWs(s[i]) && s[i] !== '/' && s[i] !== '>' && s[i] !== '=') an += s[i++].toLowerCase();
    while (i < s.length && isWs(s[i])) i++;
    let av = '';
    if (s[i] === '=') {
      i++;
      while (i < s.length && isWs(s[i])) i++;
      if (s[i] === '"' || s[i] === "'") {
        const end = s.indexOf(s[i], i + 1);
        if (end < 0) return null;
        av = decodeAttr(s.slice(i + 1, end));
        i = end + 1;
      } else {
        let raw = '';
        while (i < s.length && !isWs(s[i]) && s[i] !== '>') raw += s[i++];
        av = decodeAttr(raw);
      }
    }
    if (!attrs.some(([n]) => n === an)) attrs.push([an, av]);   // a repeated attribute is dropped
  }
}

function parseFragment(html) {
  const root = { tag: '#fragment', attrs: [], text: '' };
  const stack = [root];
  const elements = [];
  const comments = [];
  const addText = (t) => { for (const e of stack) e.text += t; };
  const bogus = (from) => {             // a bogus comment, to the next '>'
    const end = html.indexOf('>', from);
    comments.push(html.slice(from, end < 0 ? html.length : end));
    return end < 0 ? html.length : end + 1;
  };
  let i = 0;
  while (i < html.length) {
    const c = html[i];
    if (c === '&') { const [t, j] = charRef(html, i, false); addText(t); i = j; continue; }
    if (c !== '<') { addText(c); i++; continue; }
    const d = html[i + 1];
    if (d === '!') {
      if (html.startsWith('<!--', i)) {
        const from = i + 4;
        if (html[from] === '>') { comments.push(''); i = from + 1; continue; }        // <!-->
        if (html.startsWith('->', from)) { comments.push(''); i = from + 2; continue; } // <!--->
        let end = -1;
        let close = 0;
        for (let k = from; k < html.length; k++) {
          if (html.startsWith('-->', k)) { end = k; close = 3; break; }
          if (html.startsWith('--!>', k)) { end = k; close = 4; break; }
        }
        if (end < 0) { comments.push(html.slice(from)); i = html.length; continue; }   // to the end
        comments.push(html.slice(from, end));
        i = end + close;
        continue;
      }
      i = bogus(i + 2);
      continue;
    }
    if (d === '/') {
      const e = html[i + 2];
      if (e === '>') { i += 3; continue; }                  // </> is dropped
      if (e === undefined) { addText('</'); i = html.length; continue; }
      if (!isAlpha(e)) { i = bogus(i + 2); continue; }
      const t = readTag(html, i + 2);
      if (!t) { i = html.length; continue; }
      for (let k = stack.length - 1; k > 0; k--) {
        if (stack[k].tag === t.name) { stack.length = k; break; }
      }
      i = t.next;
      continue;
    }
    if (d === '?') { i = bogus(i + 1); continue; }
    if (!isAlpha(d)) { addText('<'); i++; continue; }
    const t = readTag(html, i + 1);
    if (!t) { i = html.length; continue; }
    const el = { tag: t.name, attrs: t.attrs, text: '' };
    elements.push(el);
    if (!VOID.has(t.name)) stack.push(el);
    i = t.next;
  }
  return { elements, comments, text: root.text };
}

// An element's shape: its tag and its attribute names, in order.
const shape = (el) => `${el.tag}[${el.attrs.map(([n]) => n).join(' ')}]`;
const attr = (el, name) => (el.attrs.find(([n]) => n === name) || [])[1];

function textElement() {
  let text = '';
  return {
    get textContent() { return text; },
    set textContent(v) { text = v == null ? '' : String(v); },
    get innerHTML() {
      return text.replace(/&/g, '&amp;').replace(/\u00a0/g, '&nbsp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    },
  };
}

// Text a radio peer, a bond or a route could send that reads as markup to a
// page that writes it into innerHTML: an element with a handler, a quote that
// would close an attribute, a script string, an already-escaped name, a name
// far past any field's size, and a comment opener (which takes the rest of a
// list with it).
const HOSTILE = Object.freeze([
  '<img src=x onerror=alert(1)>',
  '"><svg onload=x>',
  "'); alert(1);//",
  '&lt;b&gt;',
  'N'.repeat(4096),
  "aa:bb'\"<!--",
]);

module.exports = { parseFragment, textElement, shape, attr, HOSTILE };
