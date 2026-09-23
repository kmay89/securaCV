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
  it('a malformed 200 body is NOT success — ok must be explicitly true', () => {
    for (const body of [{}, { ok: 'yes' }, [1, 2], 'ok', { message: 'hi' }]) {
      const out = L.connectOutcome(true, body, false);
      assert.equal(out.action, 'fail', JSON.stringify(body));
    }
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

describe('connectBody (household time zone seed, repo sweep F28)', () => {
  it('carries the phone zone as tz_iana beside the credentials', () => {
    const b = L.connectBody('Home', 'pw', 'a'.repeat(64), 'America/New_York');
    assert.deepEqual(Object.keys(b).sort(), ['password', 'ssid', 'token', 'tz_iana']);
    assert.equal(b.tz_iana, 'America/New_York');
    assert.equal(b.ssid, 'Home');
    assert.equal(b.password, 'pw');
  });
  it('accepts the table shapes: three-part names, UTC, Etc/UTC', () => {
    for (const z of ['America/Argentina/Buenos_Aires', 'UTC', 'Etc/UTC', 'Europe/Kyiv']) {
      assert.equal(L.connectBody('s', '', 't', z).tz_iana, z, z);
    }
  });
  it('leaves the zone out when the browser could not tell, or sent something odd — the join never depends on it', () => {
    for (const z of ['', undefined, null, 42, 'America/New York', '../etc', 'Europe/"x"',
                     'a'.repeat(48), '/Europe', 'Europe/']) {
      const b = L.connectBody('Home', 'pw', 't', z);
      assert.ok(!('tz_iana' in b), JSON.stringify(z));
      assert.deepEqual(Object.keys(b).sort(), ['password', 'ssid', 'token']);
    }
  });
});

describe('tzNotice (what the success card says about the zone, repo sweep F28)', () => {
  it('names the fallback when the Canary does not know the phone zone', () => {
    const t = L.tzNotice('unknown_zone', 'Africa/Johannesburg');
    assert.match(t, /Africa\/Johannesburg/);
    assert.match(t, /world time \(UTC\)/);
  });
  it('says the zone was not stored when the device could not store it', () => {
    const t = L.tzNotice('not_set', 'Europe/Kyiv');
    assert.match(t, /couldn't store/);
    assert.match(t, /Europe\/Kyiv/);
    assert.match(t, /UTC/);
  });
  it('still reads as a sentence when the phone gave no name', () => {
    assert.match(L.tzNotice('unknown_zone', ''), /^Your phone's time zone isn't/);
  });
  it('says nothing when the zone was set, none was sent, or an older firmware answered without "tz"', () => {
    for (const o of ['set', 'not_sent', undefined, null, '', 'something-new']) {
      assert.equal(L.tzNotice(o, 'America/New_York'), '', String(o));
    }
  });
});
