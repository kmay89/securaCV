'use strict';

// Behavior rows for the BOOT-tap receipt capture (spa/app.js
// startReceiptCapture, and the Add Canary wizard's capture step that drives
// it). The step must not tell the owner to press BOOT before the device has
// answered an ask: a WAP hands a tap only to the one address asking (sweep
// F342), so a press the first ask has not reached yet is one no page of the
// owner's is asking for, and goes to whichever device asks first. And a WAP's
// `409 tap_contended` (another address asked during the same tap) is not a
// missed tap: the capture stops and says another device asked.
//
// magic-pairing.test.js only finds the step's strings in the source; these
// rows run the code. app.js is browser-only with no module exports, so it
// loads in a VM sandbox (as device-types.test.js does) with a small fake DOM,
// a stub CanaryAPI.request that answers from a script, and a setTimeout the
// test steps by hand.

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const APP_JS = fs.readFileSync(path.join(__dirname, '..', '..', 'spa', 'app.js'), 'utf8');

// ── A fake DOM: just what the wizard's capture step touches ──────────────
class FakeNode {
  constructor(tag, text) {
    this.tagName = tag;
    this.children = [];
    this.parentNode = null;
    this.className = '';
    this._text = text === undefined ? '' : String(text);
    this.attrs = {};
    this.listeners = {};
    this.style = {};
    this.value = '';
    this.classList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
  }
  get firstChild() { return this.children[0] || null; }
  appendChild(child) {
    if (child.parentNode) child.parentNode.removeChild(child);
    child.parentNode = this;
    this.children.push(child);
    return child;
  }
  removeChild(child) {
    const i = this.children.indexOf(child);
    if (i >= 0) this.children.splice(i, 1);
    child.parentNode = null;
    return child;
  }
  set textContent(v) {
    this._text = String(v);
    this.children.forEach((c) => { c.parentNode = null; });
    this.children = [];
  }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(''); }
  addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return this.attrs[k]; }
  querySelector() { return null; }
  // Every node of this subtree whose className includes `cls`.
  byClass(cls) {
    const out = [];
    const walk = (n) => {
      if (String(n.className).split(/\s+/).includes(cls)) out.push(n);
      n.children.forEach(walk);
    };
    walk(this);
    return out;
  }
}

function memStorage(initial) {
  const store = new Map(Object.entries(initial || {}));
  return {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => { store.set(k, String(v)); },
    removeItem: (k) => { store.delete(k); },
  };
}

// A device that answers the receipt route from `script`: each entry is one
// answer, in order — { receipt } resolves, { status, data } rejects the way
// CanaryAPI.request does (an Error carrying .status and .data), and
// { network: true } rejects with no status (the device did not answer).
// Past the end of the script every ask gets `fallback`.
function loadApp(script, fallback) {
  const body = new FakeNode('body');
  const appRoot = body.appendChild(new FakeNode('div'));
  const noop = () => {};
  const timers = [];
  const asks = [];
  const document = {
    body: {
      contains(node) {
        for (let n = node; n; n = n.parentNode) if (n === body) return true;
        return false;
      },
    },
    addEventListener: noop,
    getElementById: (id) => (id === 'app' ? appRoot : null),
    createElement: (tag) => new FakeNode(tag),
    createTextNode: (text) => new FakeNode('#text', text),
    querySelector: () => null,
  };
  const sandbox = {
    document,
    window: {
      addEventListener: noop,
      location: { hash: '' },
      history: { back: noop },
      matchMedia: () => ({ matches: false, addEventListener: noop }),
    },
    localStorage: memStorage(),
    sessionStorage: memStorage({ canary_prefill_host: '192.168.1.50' }),
    navigator: { onLine: true },
    setInterval: noop,
    clearInterval: noop,
    setTimeout: (fn) => { timers.push(fn); return timers.length; },
    clearTimeout: noop,
    fetch: noop,
    URL,
    console,
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(APP_JS, sandbox);

  const queue = script.slice();
  sandbox.CanaryAPI.request = (baseUrl, route) => {
    asks.push({ baseUrl, route });
    const answer = queue.length ? queue.shift() : fallback;
    if (!answer) return new Promise(noop);
    if (answer.receipt) return Promise.resolve(answer.receipt);
    const err = new Error('device answered ' + (answer.status || 'nothing'));
    if (answer.status) err.status = answer.status;
    if (answer.data) err.data = answer.data;
    return Promise.reject(err);
  };
  return { app: sandbox, appRoot, timers, asks };
}

// Let the stub's promise and the capture's .then/.catch run.
function settle() { return new Promise((r) => setImmediate(r)); }

// Run the poll timer the capture scheduled, then let its ask settle.
async function nextPoll(h) {
  assert.ok(h.timers.length > 0, 'the capture scheduled no further poll');
  const fn = h.timers.shift();
  fn();
  await settle();
}

const REFUSED = { status: 403, data: { error: 'physical_confirmation_required', gate_ttl_seconds: 30 } };
const CONTENDED = { status: 409, data: { error: 'tap_contended' } };
const UNANSWERED = { network: true };

function recorder() {
  const calls = { ticks: [], timeouts: [], contended: 0, receipts: [], unsupported: 0 };
  const handlers = {
    onTick: (secondsLeft, gateTtl, asked) => calls.ticks.push({ secondsLeft, gateTtl, asked }),
    onReceipt: (r) => calls.receipts.push(r),
    onTimeout: (asked) => calls.timeouts.push(asked),
    onContended: () => { calls.contended++; },
    onUnsupported: () => { calls.unsupported++; },
  };
  return { calls, handlers };
}

describe('startReceiptCapture: the press line waits for the device to answer an ask', () => {
  it('reports asked=false until the first 403, then asked=true', async () => {
    const h = loadApp([UNANSWERED, REFUSED, REFUSED]);
    const { calls, handlers } = recorder();
    h.app.startReceiptCapture('http://192.168.1.50', handlers);
    // The first tick is the step's opening line, before any ask is answered.
    assert.equal(calls.ticks.length, 1);
    assert.equal(calls.ticks[0].asked, false, 'the step said to press before the device answered an ask');
    await settle();
    // An ask the device did not answer is not a 403: still no press line.
    assert.equal(calls.ticks.at(-1).asked, false, 'an unanswered ask must not unlock the press line');
    await nextPoll(h);
    assert.equal(calls.ticks.at(-1).asked, true, "the device's 403 must unlock the press line");
    assert.equal(calls.ticks.at(-1).gateTtl, 30);
    await nextPoll(h);
    assert.equal(calls.ticks.at(-1).asked, true, 'once answered, the press line stays');
    assert.equal(calls.contended, 0);
    assert.deepEqual(calls.timeouts, []);
    assert.ok(h.asks.every((a) => a.route === '/api/provisioning-receipt'));
  });

  it("takes the device's gate TTL from the 403 body", async () => {
    const h = loadApp([{ status: 403, data: { gate_ttl_seconds: 12 } }]);
    const { calls, handlers } = recorder();
    h.app.startReceiptCapture('http://192.168.1.50', handlers);
    await settle();
    assert.equal(calls.ticks.at(-1).gateTtl, 12);
    assert.equal(calls.ticks.at(-1).asked, true);
  });

  it('a receipt ends the capture with onReceipt', async () => {
    const receipt = { device_id: 'canary-ab12', base_url: 'http://192.168.1.50', token: 'cv_x' };
    const h = loadApp([REFUSED, { receipt }]);
    const { calls, handlers } = recorder();
    h.app.startReceiptCapture('http://192.168.1.50', handlers);
    await settle();
    await nextPoll(h);
    assert.deepEqual(calls.receipts, [receipt]);
    assert.equal(h.timers.length, 0, 'no poll after the receipt');
  });
});

describe('startReceiptCapture: a 409 tap_contended stops the capture', () => {
  it('calls onContended once and asks no more', async () => {
    const h = loadApp([REFUSED, CONTENDED], REFUSED);
    const { calls, handlers } = recorder();
    h.app.startReceiptCapture('http://192.168.1.50', handlers);
    await settle();
    await nextPoll(h);
    assert.equal(calls.contended, 1, 'a 409 must reach onContended');
    assert.equal(h.timers.length, 0, 'a contended tap must not keep polling');
    const asks = h.asks.length;
    const ticks = calls.ticks.length;
    // Nothing is left to run: the capture never asks again on its own, and
    // never ends in "Didn't catch a tap".
    await settle();
    assert.equal(h.asks.length, asks);
    assert.equal(calls.ticks.length, ticks, 'no tick after the 409');
    assert.deepEqual(calls.timeouts, [], 'a contended tap is not a timeout');
  });
});

describe('startReceiptCapture: the timeout says whether the device ever answered', () => {
  it('onTimeout(false) when no ask was answered with a 403 in the window', async () => {
    const h = loadApp([], UNANSWERED);
    const { calls, handlers } = recorder();
    h.app.startReceiptCapture('http://192.168.1.50', handlers);
    await settle();
    while (h.timers.length) await nextPoll(h);
    assert.deepEqual(calls.timeouts, [false]);
    assert.ok(calls.ticks.every((t) => t.asked === false));
    assert.equal(h.asks.length, h.app.RECEIPT_POLL_TICKS, 'one ask per poll for the whole window');
  });

  it('onTimeout(true) when the device answered and no tap came', async () => {
    const h = loadApp([], REFUSED);
    const { calls, handlers } = recorder();
    h.app.startReceiptCapture('http://192.168.1.50', handlers);
    await settle();
    while (h.timers.length) await nextPoll(h);
    assert.deepEqual(calls.timeouts, [true]);
  });

  it('a 404 is firmware without the gate: onUnsupported, no more polls', async () => {
    const h = loadApp([{ status: 404 }], REFUSED);
    const { calls, handlers } = recorder();
    h.app.startReceiptCapture('http://192.168.1.50', handlers);
    await settle();
    assert.equal(calls.unsupported, 1);
    assert.equal(h.timers.length, 0);
  });

  it('cancel() stops it: the next poll asks nothing', async () => {
    const h = loadApp([REFUSED], REFUSED);
    const { calls, handlers } = recorder();
    const handle = h.app.startReceiptCapture('http://192.168.1.50', handlers);
    await settle();
    handle.cancel();
    const asks = h.asks.length;
    await nextPoll(h);
    assert.equal(h.asks.length, asks);
    assert.deepEqual(calls.timeouts, []);
  });
});

// ── The wizard's capture step, rendered ─────────────────────────────────
function stepTexts(h) {
  const pick = (cls) => h.appRoot.byClass(cls).map((n) => n.textContent);
  return { titles: pick('pair-step-title'), subtitles: pick('pair-step-subtitle'), status: pick('pair-status') };
}

describe('the Add Canary capture step', () => {
  it("says \"Don't press BOOT yet\" until the device's first 403, then to short-tap", async () => {
    const h = loadApp([UNANSWERED, REFUSED]);
    h.app.renderAddCanaryView();
    let t = stepTexts(h);
    assert.deepEqual(t.titles, ['Asking the Canary first']);
    assert.deepEqual(t.subtitles, ["Don't press BOOT yet."]);
    assert.match(t.status[0], /^Asking the Canary first\. Don't press BOOT yet/);
    await settle();
    t = stepTexts(h);
    assert.deepEqual(t.titles, ['Asking the Canary first'], 'an unanswered ask must not show the press line');
    assert.match(t.status[0], /Don't press BOOT yet/);
    await nextPoll(h);
    t = stepTexts(h);
    assert.deepEqual(t.titles, ['Short-tap the BOOT button']);
    assert.match(t.status[0], /^Listening for \d+s — the device holds a tap for 30s\.$/);
  });

  it('a 409 shows that another device asked, not a missed tap', async () => {
    const h = loadApp([REFUSED, CONTENDED], REFUSED);
    h.app.renderAddCanaryView();
    await settle();
    await nextPoll(h);
    const t = stepTexts(h);
    assert.deepEqual(t.titles, ['Another device asked at the same time']);
    assert.ok(!t.titles.includes("Didn't catch a tap"));
    assert.equal(h.timers.length, 0);
  });

  it("a window the device never answered ends in \"The Canary didn't answer\"", async () => {
    const h = loadApp([], UNANSWERED);
    h.app.renderAddCanaryView();
    await settle();
    while (h.timers.length) await nextPoll(h);
    const t = stepTexts(h);
    assert.ok(t.titles.includes("The Canary didn't answer"), JSON.stringify(t.titles));
    assert.ok(!t.titles.includes("Didn't catch a tap"));
  });

  it("an answered window with no tap ends in \"Didn't catch a tap\"", async () => {
    const h = loadApp([], REFUSED);
    h.app.renderAddCanaryView();
    await settle();
    while (h.timers.length) await nextPoll(h);
    const t = stepTexts(h);
    assert.ok(t.titles.includes("Didn't catch a tap"), JSON.stringify(t.titles));
  });
});
