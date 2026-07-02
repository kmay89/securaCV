'use strict';

// Unit tests for the onboarding wizard's pure decision logic.
//
// The token handling and connect-retry decisions live in a pure, DOM-free
// block inside companion_pwa.h, fenced by `WIZARD_LOGIC:BEGIN/END` markers
// and self-exported via `module.exports` under Node (same pattern as
// selftest_ui.test.js). We extract that exact block and evaluate it here,
// so these tests exercise the SAME source the firmware ships.
//
//   node --test firmware/projects/canary-wap/arduino/canary_wap/wizard_logic.test.js

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const PWA = path.join(__dirname, 'companion_pwa.h');

function loadLogic() {
  const src = fs.readFileSync(PWA, 'utf8');
  const m = src.match(/\/\*\s*WIZARD_LOGIC:BEGIN[\s\S]*?WIZARD_LOGIC:END\s*\*\//);
  if (!m) throw new Error('WIZARD_LOGIC block not found in companion_pwa.h');
  const sandbox = { module: { exports: {} } };
  vm.runInNewContext(m[0], sandbox, { filename: 'wizard_logic.extracted.js' });
  const L = sandbox.module.exports;
  if (!L || typeof L.connectOutcome !== 'function') {
    throw new Error('extracted block did not export the expected WizardLogic API');
  }
  return L;
}

let L;
before(() => { L = loadLogic(); });

describe('isPairToken', () => {
  it('accepts exactly 64 hex chars', () => {
    assert.ok(L.isPairToken('a'.repeat(64)));
    assert.ok(L.isPairToken('0123456789abcdefABCDEF'.padEnd(64, '0')));
  });
  it('rejects everything else', () => {
    assert.ok(!L.isPairToken(null));
    assert.ok(!L.isPairToken(undefined));
    assert.ok(!L.isPairToken(''));
    assert.ok(!L.isPairToken('a'.repeat(63)));
    assert.ok(!L.isPairToken('a'.repeat(65)));
    assert.ok(!L.isPairToken('g'.repeat(64)));   // non-hex
    assert.ok(!L.isPairToken(42));
  });
});

describe('connectOutcome', () => {
  it('proceeds on an accepted submit', () => {
    // Field-wise compare: the object comes from another vm realm, so
    // deepStrictEqual would fail on prototype identity alone.
    const out = L.connectOutcome(true, { ok: true }, false);
    assert.equal(out.action, 'proceed');
    assert.equal(out.isTokenErr, false);
  });
  it('retries with a fresh token on the FIRST invalid_token', () => {
    const out = L.connectOutcome(true, { ok: false, code: 'invalid_token' }, false);
    assert.equal(out.action, 'refresh-retry');
    assert.equal(out.isTokenErr, true);
  });
  it('does not loop: the SECOND invalid_token is a failure', () => {
    const out = L.connectOutcome(true, { ok: false, code: 'invalid_token' }, true);
    assert.equal(out.action, 'fail');
    assert.equal(out.isTokenErr, true);
  });
  it('non-token errors fail without a retry', () => {
    const out = L.connectOutcome(true, { ok: false, error: 'Invalid SSID' }, false);
    assert.equal(out.action, 'fail');
    assert.equal(out.isTokenErr, false);
  });
  it('HTTP-level failure without a body is a plain failure', () => {
    const out = L.connectOutcome(false, null, false);
    assert.equal(out.action, 'fail');
    assert.equal(out.isTokenErr, false);
  });
});

describe('capabilityNotice', () => {
  it('is fully hidden in wizard (token) mode — Web Bluetooth is irrelevant there', () => {
    assert.equal(L.capabilityNotice(true, false, false).show, false);
    assert.equal(L.capabilityNotice(true, true, true).show, false);
  });
  it('insecure origin is an informational note, and never mentions failure', () => {
    const n = L.capabilityNotice(false, false, false);
    assert.equal(n.show, true);
    assert.match(n.text, /work fine/i);
    assert.doesNotMatch(n.text, /insecure origin/i);
  });
  it('missing Web Bluetooth points at Bluefy', () => {
    const n = L.capabilityNotice(false, true, false);
    assert.equal(n.show, true);
    assert.match(n.text, /Bluefy/);
  });
  it('capable browser gets no banner', () => {
    assert.equal(L.capabilityNotice(false, true, true).show, false);
  });
});
