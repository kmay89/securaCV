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
function loadConsole(opts) {
  opts = opts || {};
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
      // Trustee mode touches these at load (body class, header copy).
      body: makeElement(),
      querySelector: () => makeElement(),
      addEventListener() {},
    },
    window: { location: { origin: 'http://x', pathname: '/', hash: opts.hash || '' }, addEventListener() {} },
    location: { origin: 'http://x', pathname: '/', hash: opts.hash || '' },
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
    // Real WebCrypto: the signer page recomputes the request hash with SHA-256.
    crypto: require('node:crypto').webcrypto,
    TextEncoder,
    BigInt,
    Date,
    console,
  };
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox, { filename: 'breakglass.extracted.js' });

  const drive = (code) => vm.runInContext(code, sandbox, { filename: 'test-driver.js' });
  const stubApi = (status, json) => {
    sandbox.__apiResult = { status, json };
    drive('api = async () => __apiResult;');
  };
  const el = (id) => elements.get(id);
  return { timers, drive, stubApi, el, sandbox };
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

// The trustee signer recomputes the request hash from the link's fields. The
// framing must match src/break_glass/core.rs byte for byte — these vectors
// are pinned there too (request_hash_vectors_match_the_console_signer).
const VECTOR = {
  envelope: 'vault:vector',
  purpose: 'incident',
  ruleset_hash: '31553a5efe89a5f10beb3a4e662b85ec529b5a13329e790cd5d353ca860bd130', // sha256("ruleset:test")
  start_epoch_s: 1700000000,
  size_s: 600,
};
const HASH_BARE = '5b53aba1db18b602be473b5b2710805f0ed4874fca5cf7f70b0ec90be9c34f73';
const HASH_CTX = 'd9c3c3667e4176e50c332cec28eae6d8fb04f6c175861a51df77e64be7ef262d';

describe('request-hash recomputation (served-path WYSIWYS)', () => {
  it('matches the kernel framing without operator context', async () => {
    const ctx = loadConsole();
    const h = await ctx.drive('computeRequestHashHex(' + JSON.stringify(VECTOR) + ')');
    assert.equal(h, HASH_BARE);
  });

  it('matches the kernel framing with operator context', async () => {
    const ctx = loadConsole();
    const f = Object.assign({}, VECTOR, {
      requested_by: 'Alice Operator', reason: 'incident-review', case_ref: 'case-2026-0042',
    });
    const h = await ctx.drive('computeRequestHashHex(' + JSON.stringify(f) + ')');
    assert.equal(h, HASH_CTX);
  });

  it('share fragment round-trips through the signer parser and verifies', async () => {
    const open = Object.assign({}, VECTOR, {
      request_hash: HASH_CTX,
      time_bucket: { start_epoch_s: VECTOR.start_epoch_s, size_s: VECTOR.size_s },
      requested_by: 'Alice Operator', reason: 'incident-review', case_ref: 'case-2026-0042',
    });
    const maker = loadConsole();
    const frag = await maker.drive('shareFragment(' + JSON.stringify(open) + ')');
    assert.ok(frag.startsWith('#sign&hash=' + HASH_CTX));
    const signer = loadConsole({ hash: frag });
    const result = await signer.drive('verifySignLink(parseSignFragment())');
    assert.equal(result.ok, true, JSON.stringify(result));
    assert.equal(signer.el('ts-btn-sign').disabled, false, 'a verified link enables signing');
    assert.equal(signer.el('ts-by').textContent, 'Alice Operator');
    assert.equal(signer.el('ts-reason').textContent, 'incident-review');
  });

  it('refuses a link whose fields do not produce its hash (swapped meaning)', async () => {
    const open = Object.assign({}, VECTOR, {
      request_hash: HASH_CTX,
      time_bucket: { start_epoch_s: VECTOR.start_epoch_s, size_s: VECTOR.size_s },
      requested_by: 'Alice Operator', reason: 'incident-review', case_ref: 'case-2026-0042',
    });
    const maker = loadConsole();
    const frag = await maker.drive('shareFragment(' + JSON.stringify(open) + ')');
    const swapped = frag.replace(encodeURIComponent('Alice Operator'), encodeURIComponent('Mallory'));
    const signer = loadConsole({ hash: swapped });
    const result = await signer.drive('verifySignLink(parseSignFragment())');
    assert.equal(result.ok, false);
    assert.equal(signer.el('ts-btn-sign').disabled, true, 'a mismatching link must not enable signing');
    assert.match(signer.el('ts-verify').textContent, /DO NOT SIGN/);
  });

  it('labels a hash-only link as blind signing', async () => {
    const signer = loadConsole({ hash: '#sign&hash=' + HASH_CTX + '&envelope=vault%3Avector' });
    const result = await signer.drive('verifySignLink(parseSignFragment())');
    assert.equal(result.blind, true);
    assert.match(signer.el('ts-verify').textContent, /blind signing/);
  });

  it('status pane shows the bound operator context', async () => {
    const ctx = loadConsole();
    ctx.stubApi(200, { needed: 2, collected: [], requested_by: 'Alice Operator', reason: 'incident-review', case_ref: 'case-9', purpose: 'incident' });
    await ctx.drive('(async () => { await refreshStatus(); })()');
    assert.match(ctx.el('status-context').textContent, /Alice Operator/);
    assert.match(ctx.el('status-context').textContent, /incident-review/);
  });
});
