// canary-local/tests/fixtures/fake_dom.js — the few lines of DOM the Lab's
// MQTT panes use, so a test can drive buildMqtt itself in node (no browser):
// createElement / createTextNode, append / prepend / remove, children,
// lastChild, textContent, innerHTML = "" (to clear), className + classList,
// setAttribute, dataset / style, a class lookup, and listeners a test can
// fire (addEventListener records them; click() and dispatch(type) call them).
// A canvas's getContext("2d") hands back a context that accepts every call
// and draws nothing, so a lab that paints can still run its logic. Not a
// DOM: just enough to read what a pane rendered. A test installs it on
// globalThis.document and restores the old one after.

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
    this.dataset = {};
    this.style = {};
    this.listeners = {};
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
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  dispatch(type, ev) { for (const fn of this.listeners[type] || []) fn({ target: this, preventDefault() {}, ...(ev || {}) }); }
  click() { this.dispatch("click"); }
  getContext() { return NULL_CTX; }
  getBoundingClientRect() { return { left: 0, top: 0, width: 0, height: 0 }; }
  setPointerCapture() {}
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

// A 2D context that takes any call or property and does nothing.
const NULL_CTX = new Proxy({}, { get: () => () => {}, set: () => true });

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

// A clock a test drives: setTimeout and requestAnimationFrame are replaced
// (and restored after) so a pane's sleeps and a lab's frame loop run on the
// test's time, not the wall's. advance(ms) fires every timer due in that
// span in order, letting promises settle after each; frame(t) runs the
// frame callbacks queued so far with timestamp t.
async function withFakeClock(fn) {
  const saved = { setTimeout: globalThis.setTimeout, raf: globalThis.requestAnimationFrame, window: globalThis.window };
  const settle = () => new Promise((r) => setImmediate(r));
  let now = 0;
  let timers = [];
  let frames = [];
  const clock = {
    get now() { return now; },
    async advance(ms) {
      const end = now + ms;
      for (;;) {
        timers.sort((a, b) => a.at - b.at || a.n - b.n);
        if (!timers.length || timers[0].at > end) break;
        const t = timers.shift();
        now = t.at;
        t.fn();
        await settle();
      }
      now = end;
      await settle();
    },
    frame(t) { const q = frames; frames = []; for (const f of q) f(t); },
  };
  let n = 0;
  globalThis.setTimeout = (f, ms) => { timers.push({ at: now + (ms || 0), fn: f, n: n++ }); return n; };
  globalThis.requestAnimationFrame = (f) => { frames.push(f); return frames.length; };
  globalThis.window = { devicePixelRatio: 1, addEventListener() {} };
  try { return await fn(clock); } finally {
    globalThis.setTimeout = saved.setTimeout;
    globalThis.requestAnimationFrame = saved.raf;
    globalThis.window = saved.window;
    timers = []; frames = [];
  }
}

module.exports = { El, fakeDocument, fakeBus, withFakeDom, withFakeClock };
