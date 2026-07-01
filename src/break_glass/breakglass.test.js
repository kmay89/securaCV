'use strict';

// Behavioral tests for the break-glass console's status polling.
//
// The console polls /breakglass/status while a request is open. Connecting
// (or manually refreshing) when a request is ALREADY open — status 200 —
// must resume that polling by itself, and repeated refreshes must not stack
// duplicate interval timers.
//
// The whole console lives in one <script> block inside breakglass.html; we
// extract and evaluate that exact block under a minimal stub DOM, so these
// tests exercise the same source the server ships — no duplicated copy to
// drift.
//
//   node --test src/break_glass/breakglass.test.js

const { describe, it, beforeEach } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const HTML = path.join(__dirname, 'breakglass.html');

function makeElement() {
  return {
    className: '',
    textContent: '',
    innerHTML: '',
    value: '',
    disabled: false,
    style: {},
    classList: { add() {}, remove() {} },
    firstElementChild: { style: {} },
    appendChild() {},
    addEventListener() {},
  };
}

// Load the console script into a fresh vm context with a stub DOM and
// instrumented interval timers. Returns handles to drive and observe it.
function loadConsole() {
  const src = fs.readFileSync(HTML, 'utf8');
  // Plain index-based extraction, not a regex: this pulls our own script
  // block out of the file we ship (exactly one, lowercase tags), and a
  // regex here trips CodeQL's HTML-filtering heuristics for no benefit.
  const openTag = '<script>';
  const open = src.indexOf(openTag);
  const close = src.lastIndexOf('</script>');
  if (open === -1 || close === -1 || close <= open) {
    throw new Error('no <script> block found in breakglass.html');
  }
  const script = src.slice(open + openTag.length, close);

  const elements = new Map();
  const timers = { active: new Map(), nextId: 1 };
  const sandbox = {
    document: {
      getElementById(id) {
        if (!elements.has(id)) elements.set(id, makeElement());
        return elements.get(id);
      },
      createElement: () => makeElement(),
      addEventListener() {},
    },
    window: { location: { origin: 'http://x', pathname: '/', hash: '' }, addEventListener() {} },
    location: { origin: 'http://x', pathname: '/', hash: '' },
    setInterval(fn, ms) {
      const id = timers.nextId++;
      timers.active.set(id, { fn, ms });
      return id;
    },
    clearInterval(id) {
      timers.active.delete(id);
    },
    setTimeout: () => 0,
    clearTimeout() {},
    fetch: async () => {
      throw new Error('fetch must be stubbed via api()');
    },
    confirm: () => true,
    navigator: {},
    crypto: { subtle: {} },
    console,
  };
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox, { filename: 'breakglass.extracted.js' });

  const drive = (code) => vm.runInContext(code, sandbox, { filename: 'test-driver.js' });
  const stubApi = (status, json) => {
    sandbox.__apiResult = { status, json };
    drive('api = async () => __apiResult;');
  };
  return { timers, drive, stubApi };
}

describe('status polling auto-resume', () => {
  let ctx;
  beforeEach(() => {
    ctx = loadConsole();
  });

  it('starts polling when refresh finds an already-open request (200)', async () => {
    ctx.stubApi(200, { needed: 2, collected: [] });
    await ctx.drive('(async () => { await refreshStatus(); })()');
    assert.equal(ctx.timers.active.size, 1, 'a poll interval must be running after a 200 status');
  });

  it('does not stack a second timer on repeated refreshes', async () => {
    ctx.stubApi(200, { needed: 2, collected: [] });
    await ctx.drive('(async () => { await refreshStatus(); })()');
    const [firstId] = ctx.timers.active.keys();
    await ctx.drive('(async () => { await refreshStatus(); })()');
    assert.equal(ctx.timers.active.size, 1, 'refresh must never stack interval timers');
    const [stillId] = ctx.timers.active.keys();
    assert.equal(stillId, firstId, 'an already-running timer must be left alone, not re-registered');
  });

  it('stops polling when no request is open (409)', async () => {
    ctx.stubApi(200, { needed: 2, collected: [] });
    await ctx.drive('(async () => { await refreshStatus(); })()');
    assert.equal(ctx.timers.active.size, 1);
    ctx.stubApi(409, {});
    await ctx.drive('(async () => { await refreshStatus(); })()');
    assert.equal(ctx.timers.active.size, 0, 'polling must stop once the request is gone');
  });
});
