// canary-local/tests/fixtures/fake_dom.js — the few lines of DOM the Lab's
// MQTT panes use, so a test can drive buildMqtt itself in node (no browser):
// createElement / createTextNode, append / prepend / remove, children,
// lastChild, textContent, innerHTML = "" (to clear), className + classList,
// setAttribute, and a class lookup. Not a DOM: just enough to read what a
// pane rendered. A test installs it on globalThis.document and restores the
// old one after.

class El {
  constructor(tag) {
    this.tagName = String(tag).toUpperCase();
    this.children = [];
    this.parent = null;
    this.className = "";
    this._text = "";
    this.scrollTop = 0;
    this.offsetWidth = 0;
    this.attrs = {};
    this.classList = {
      add: (...cs) => { for (const c of cs) if (!this.className.split(" ").includes(c)) this.className = (this.className + " " + c).trim(); },
      remove: (...cs) => { this.className = this.className.split(" ").filter((x) => x && !cs.includes(x)).join(" "); },
      contains: (c) => this.className.split(" ").includes(c),
      toggle: (c, on) => {
        const has = this.className.split(" ").includes(c);
        const want = on === undefined ? !has : !!on;
        if (want && !has) this.classList.add(c);
        if (!want && has) this.classList.remove(c);
        return want;
      },
    };
  }
  _adopt(k) {
    if (typeof k === "string") { const t = new El("#text"); t._text = k; k = t; }
    if (k.parent) k.remove();
    k.parent = this;
    return k;
  }
  append(...kids) { for (const k of kids) this.children.push(this._adopt(k)); }
  prepend(...kids) { this.children.unshift(...kids.map((k) => this._adopt(k))); }
  remove() {
    if (this.parent) this.parent.children = this.parent.children.filter((c) => c !== this);
    this.parent = null;
  }
  get lastChild() { return this.children[this.children.length - 1] || null; }
  get firstChild() { return this.children[0] || null; }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(""); }
  set textContent(v) { this._text = String(v); this.children = []; }
  set innerHTML(v) { if (v !== "") throw new Error("fake DOM: innerHTML only clears"); this._text = ""; this.children = []; }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  addEventListener() {}
  // every descendant carrying class `cls`, in document order
  all(cls) {
    const out = [];
    for (const c of this.children) {
      if (c.className.split(" ").includes(cls)) out.push(c);
      out.push(...c.all(cls));
    }
    return out;
  }
  querySelector(sel) { return this.all(sel.replace(/^\./, ""))[0] || null; }
}

function fakeDocument() {
  return {
    createElement: (t) => new El(t),
    createTextNode: (s) => { const t = new El("#text"); t._text = String(s); return t; },
    body: { contains: () => true },
  };
}

// A bus like the pages' (on / emit / has), synchronous.
function fakeBus() {
  const fns = {};
  const seen = new Set();
  return {
    on: (t, f) => (fns[t] ||= []).push(f),
    emit: (t, p) => { seen.add(t); (fns[t] || []).forEach((f) => f(p || {})); },
    has: (t) => seen.has(t),
  };
}

// Run fn with the fake document installed; restores the old one after.
async function withFakeDom(fn) {
  const saved = globalThis.document;
  globalThis.document = fakeDocument();
  try { return await fn(); } finally { globalThis.document = saved; }
}

module.exports = { El, fakeDocument, fakeBus, withFakeDom };
