'use strict';

// The built single-file viewer, as a page: its Content-Security-Policy is
// computed from the page itself, and what it renders from a bundle is text.
//
//   node --test viewer/evidence_viewer.test.js
//
// Two guards, each for a way the page could be turned against its reader:
//
//   1. The CSP. Every inline <script> and the <style> block are allowed by
//      sha256 and nothing else is — recomputed here from the built file, so a
//      stale hash (an edit to a core without `node viewer/build.mjs`) or a
//      policy that quietly widened fails before it ships. No inline style
//      attribute may remain in the markup either: the policy would refuse it
//      at runtime, and the page would lay out wrong without a word.
//   2. The sinks. The page builds markup as strings from fields nobody here
//      controls — every name, zone, label and key in the file it is handed.
//      The renderers are actually RUN against a bundle whose every string is
//      `<img src=x onerror=…>`, with the verifier stubbed to pass it, and the
//      markup they produce is inspected: the hostile field must appear
//      escaped, and no element it names may exist. Running it is what makes
//      "the viewer escapes" a fact rather than a reading of the source.

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');

const BUILT = path.join(__dirname, 'evidence_viewer.html');
const html = fs.readFileSync(BUILT, 'utf8');
const FIXTURES = path.join(__dirname, '..', 'tests', 'fixtures');
const loadJson = (p) => JSON.parse(fs.readFileSync(path.join(FIXTURES, p), 'utf8'));

const bodies = (tag) => Array.from(html.matchAll(new RegExp(`<${tag}>([\\s\\S]*?)</${tag}>`, 'g')), (m) => m[1]);
const sha256 = (text) => `'sha256-${createHash('sha256').update(text, 'utf8').digest('base64')}'`;

describe('the built viewer carries a Content-Security-Policy computed from itself', () => {
  const meta = html.match(/<meta http-equiv="Content-Security-Policy" content="([^"]*)">/);
  const directives = Object.fromEntries((meta ? meta[1] : '').split(';').map((d) => {
    const [name, ...rest] = d.trim().split(/\s+/);
    return [name, rest];
  }));

  it('is declared in <head>, before the inline style and scripts', () => {
    assert.ok(meta, 'no CSP <meta> in the built viewer');
    assert.ok(meta.index < html.indexOf('<style>'), 'the CSP must precede the <style> block');
    assert.ok(meta.index < html.indexOf('<script>'), 'the CSP must precede the first <script>');
    assert.ok(!html.includes('/*__CSP__*/'), 'the CSP placeholder was not filled');
  });

  it('allows exactly the inline scripts and the style block, by hash', () => {
    const scripts = bodies('script');
    const styles = bodies('style');
    assert.equal(scripts.length, 4, 'three cores and the page script');
    assert.equal(styles.length, 1);
    assert.deepEqual(directives['script-src'], scripts.map(sha256), 'script-src must be the sha256 of each inline script, in order');
    assert.deepEqual(directives['style-src'], styles.map(sha256), 'style-src must be the sha256 of the style block');
  });

  it('permits nothing else: no network, no eval, no base or form target', () => {
    assert.deepEqual(directives['default-src'], ["'none'"]);
    assert.deepEqual(directives['connect-src'], ["'none'"]);
    assert.deepEqual(directives['base-uri'], ["'none'"]);
    assert.deepEqual(directives['form-action'], ["'none'"]);
    assert.deepEqual(directives['img-src'], ['data:'], 'only the data: favicon');
    for (const [name, values] of Object.entries(directives)) {
      for (const v of values) assert.doesNotMatch(v, /unsafe-inline|unsafe-eval|unsafe-hashes|https?:/, `${name} widened: ${v}`);
    }
  });

  it('carries no inline style attribute, which the policy would refuse at runtime', () => {
    assert.doesNotMatch(html, / style="/, 'use a class, or data-style painted through paint()');
    // Computed positions take the sanctioned route.
    assert.match(html, /data-style="/);
    assert.match(html, /n\.style\.cssText = n\.dataset\.style/);
  });
});

// ---- the sinks -----------------------------------------------------------

const TAG = '<img src=x onerror="alert(1)">';

// Every readable string in the fixture becomes the hostile tag. The JSON
// carriers the ledger stores (payload_json, approvals_json) keep their shape,
// so the strings INSIDE them are hostile too rather than the carrier becoming
// unparseable and skipped.
function hostile(v) {
  if (typeof v === 'string') return TAG;
  if (Array.isArray(v)) return v.map(hostile);
  if (v && typeof v === 'object') {
    const o = {};
    for (const [k, x] of Object.entries(v)) {
      o[k] = (k === 'payload_json' || k === 'approvals_json') && typeof x === 'string'
        ? JSON.stringify(hostile(JSON.parse(x)))
        : hostile(x);
    }
    return o;
  }
  return v;
}

/* A DOM just large enough to run the page script against: ids resolve to
 * nodes, every innerHTML write is recorded, and layout reads answer zero.
 * The timeline's live wiring (initTimeline) measures real layout, so its
 * map id resolves to null and the rendered rows are inspected, not wired. */
function fakeDom() {
  const writes = [];
  const node = () => {
    const el = {
      _html: '', value: '', textContent: '', hidden: false, style: {}, dataset: {},
      clientHeight: 0, offsetHeight: 0, files: [],
      classList: { contains: () => false, toggle() {}, add() {}, remove() {} },
      addEventListener() {}, removeEventListener() {}, setAttribute() {}, getAttribute: () => null,
      focus() {}, click() {}, querySelector: () => null, querySelectorAll: () => [],
      getBoundingClientRect: () => ({ top: 0, left: 0, width: 0, height: 0 }),
      insertAdjacentHTML(_, h) { el.innerHTML = el._html + h; },
      get firstElementChild() { return node(); },
    };
    Object.defineProperty(el, 'innerHTML', {
      get: () => el._html,
      set: (v) => { el._html = String(v); writes.push(String(v)); },
    });
    return el;
  };
  const byId = new Map();
  const document = {
    body: node(),
    getElementById: (id) => {
      if (id === 'tlmap') return null;
      if (!byId.has(id)) byId.set(id, node());
      return byId.get(id);
    },
    addEventListener() {}, removeEventListener() {},
  };
  return { byId, writes, document };
}

// Instantiate the page script from the BUILT file (the one that ships), with
// the cores loaded for real and the verifier's verdict optionally replaced.
function loadPage(stubVerify) {
  require('./verify_core');
  require('./timeline_core');
  require('./zip_core');
  const realV = globalThis.SecuraCVVerify;
  if (stubVerify) globalThis.SecuraCVVerify = { ...realV, ...stubVerify };
  const dom = fakeDom();
  const page = bodies('script')[3];
  const window = { addEventListener() {}, removeEventListener() {}, scrollTo() {}, print() {} };
  const run = new Function(
    'document', 'window', 'matchMedia', 'ResizeObserver', 'requestAnimationFrame', 'cancelAnimationFrame',
    'sessionStorage', 'navigator', 'location',
    page + '\nreturn { process, processBundle };',
  );
  try {
    const api = run(dom.document, window, () => ({ matches: true }), class { observe() {} disconnect() {} },
      () => 0, () => {}, { getItem: () => null, setItem() {} }, {}, { search: '' });
    return { ...dom, ...api };
  } finally {
    globalThis.SecuraCVVerify = realV;
  }
}

const settle = () => new Promise((r) => setTimeout(r, 20));

function assertInert(markup, what) {
  assert.ok(markup.length > 0, 'the renderer must have run');
  assert.ok(!/<(?:img|script|iframe|object|embed)\b/i.test(markup), `${what} became an element:\n${markup.slice(0, 2000)}`);
  assert.ok(!/"\s*on[a-z]+\s*=/i.test(markup), `${what} broke out of an attribute:\n${markup.slice(0, 2000)}`);
  assert.ok(!/javascript:/i.test(markup), `${what} supplied its own URL scheme`);
  assert.match(markup, /&lt;img src=x/, 'the hostile field should still be shown, escaped');
}

describe('a hostile bundle renders as text, never as markup', () => {
  it('an evidence envelope whose every string is a tag', async () => {
    const envelope = hostile(loadJson('envelope/valid_envelope.json'));
    const report = {
      ok: true, status: 'valid_with_warnings', authorship: 'envelope', inconclusive: false,
      sealed_events: 3, export_receipts: 1, break_glass_granted: 0, break_glass_denied: 0,
      checks: [TAG], warnings: [TAG], whole_envelope_digest: TAG, device_public_key: TAG,
    };
    const page = loadPage({ verifyEnvelope: async () => report });
    await page.process(envelope, TAG);
    await settle();
    const markup = page.writes.join('\n');
    assertInert(markup, 'an envelope field');
    assert.match(markup, /Self-consistent/, 'the envelope path rendered its verdict');
    assert.match(markup, /Timeline/, 'the envelope path rendered its timeline');
  });

  it('an event export whose every string is a tag', async () => {
    const bundle = hostile(loadJson('export_bundle/valid_bundle.json'));
    const report = {
      ok: true, status: 'valid_with_warnings', authorship: 'bundle', inconclusive: false,
      checks: [TAG], warnings: [TAG], events: 3, failures: 0, buckets: 1, batches: 1,
      device_public_key: TAG, artifact_hash: TAG, entry_hash: TAG,
    };
    const page = loadPage({ verifyExportBundle: async () => report });
    await page.process(bundle, TAG);
    await settle();
    const markup = page.writes.join('\n');
    assertInert(markup, 'an export field');
    assert.match(markup, /Disclosure record/, 'the export path rendered its receipt panel');
  });

  it('a rejected file shows the verifier’s reason as text', async () => {
    const page = loadPage({ verifyEnvelope: async () => ({ ok: false, status: 'compromised', checks: [TAG], warnings: [], error: TAG }) });
    await page.process(loadJson('envelope/valid_envelope.json'), TAG);
    await settle();
    assertInert(page.writes.join('\n'), 'a failure reason');
  });
});

// ---- the headline ---------------------------------------------------------
// "Verified" is reserved for signatures checked against a key the reader
// supplied. With the box blank, the real verifier's pass on the real sample
// must read as self-consistent, and say what that means.

describe('the headline a pass earns', () => {
  const SELF = 'Self-consistent — signed by the key it names (identity not confirmed)';

  it('without a trusted key an envelope is self-consistent, never "Verified"', async () => {
    const page = loadPage();
    await page.process(loadJson('envelope/valid_envelope.json'), 'sample');
    await settle();
    const report = page.byId.get('report').innerHTML;
    assert.match(report, new RegExp(SELF.replace(/[()]/g, '\\$&')));
    assert.doesNotMatch(report, /Verified —/, 'a pass under the file\'s own key is not "Verified"');
    assert.match(report, /authorship is self-attested, not confirmed/, 'the explainer must say what self-consistent means');
    assert.match(report, /reads “self-consistent”, not “verified”/);
  });

  it('with the matching trusted key the same envelope is Verified', async () => {
    const envelope = loadJson('envelope/valid_envelope.json');
    const page = loadPage();
    page.document.getElementById('trustkey').value = envelope.provenance.device_public_key;
    await page.process(envelope, 'sample');
    await settle();
    const report = page.byId.get('report').innerHTML;
    assert.match(report, /Verified — with notes/);
    assert.match(report, /authorship confirmed/);
    assert.doesNotMatch(report, /Self-consistent/);
  });

  it('with a different trusted key the same envelope is rejected, with the reason', async () => {
    const page = loadPage();
    page.document.getElementById('trustkey').value = '00'.repeat(32);
    await page.process(loadJson('envelope/valid_envelope.json'), 'sample');
    await settle();
    const report = page.byId.get('report').innerHTML;
    assert.match(report, /Not verified — bundle rejected/);
    assert.match(report, /device key mismatch/);
  });

  it('an event export follows the same rule', async () => {
    const bundle = loadJson('export_bundle/valid_bundle.json');
    const blank = loadPage();
    await blank.process(bundle, 'sample');
    await settle();
    assert.match(blank.byId.get('report').innerHTML, new RegExp(SELF.replace(/[()]/g, '\\$&')));
    assert.doesNotMatch(blank.byId.get('report').innerHTML, /Verified —/);

    const keyed = loadPage();
    keyed.document.getElementById('trustkey').value = bundle.device_public_key.map((b) => b.toString(16).padStart(2, '0')).join('');
    await keyed.process(bundle, 'sample');
    await settle();
    assert.match(keyed.byId.get('report').innerHTML, /Verified — integrity intact/, 'no post-quantum note on the export fixture, so a clean pass');
  });
});
