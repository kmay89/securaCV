'use strict';

// Unit tests for the admin dashboard's pure helpers (camera peek URL,
// bounded-retry policy, BLE chirp endpoint). The logic lives in a DOM-free
// block inside web_ui.h fenced by `WEBUI_LOGIC:BEGIN/END` and self-exported
// under Node — same extraction pattern as selftest_ui.test.js — so these
// tests exercise the SAME source the firmware ships.
//
//   node --test firmware/projects/canary-wap/arduino/canary_wap/web_ui_logic.test.js

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const UI = path.join(__dirname, 'web_ui.h');

function loadLogic() {
  const src = fs.readFileSync(UI, 'utf8');
  const m = src.match(/\/\*\s*WEBUI_LOGIC:BEGIN[\s\S]*?WEBUI_LOGIC:END\s*\*\//);
  if (!m) throw new Error('WEBUI_LOGIC block not found in web_ui.h');
  const sandbox = { module: { exports: {} } };
  vm.runInNewContext(m[0], sandbox, { filename: 'web_ui_logic.extracted.js' });
  const L = sandbox.module.exports;
  if (!L || typeof L.peekStreamUrl !== 'function') {
    throw new Error('extracted block did not export the expected WebUiLogic API');
  }
  return L;
}

let L;
before(() => { L = loadLogic(); });

describe('peekStreamUrl', () => {
  it('omits the token param entirely when there is no real token', () => {
    // The old code appended "&token=null" (a literal string) on a
    // cookie-authenticated session; the cookie path validated first so it
    // "worked", but a real token=null query attempt is nonsense. Now the
    // param is dropped so the cookie is used cleanly.
    const u = L.peekStreamUrl('', '/api/peek/stream', null, 1234);
    assert.equal(u, '/api/peek/stream?t=1234');
    assert.doesNotMatch(u, /token/);
  });
  it('appends an encoded token when one is present', () => {
    const u = L.peekStreamUrl('', '/api/peek/stream', 'cv_abc 123', 9);
    assert.match(u, /&token=cv_abc%20123$/);
  });
  it('respects an undefined/empty token as no-token', () => {
    assert.doesNotMatch(L.peekStreamUrl('', '/api/peek/snapshot', undefined, 1), /token/);
    assert.doesNotMatch(L.peekStreamUrl('', '/api/peek/snapshot', '', 1), /token/);
  });
});

describe('shouldRetryPeek', () => {
  it('retries up to the cap, then stops (no infinite reload loop)', () => {
    assert.ok(L.shouldRetryPeek(0));
    assert.ok(L.shouldRetryPeek(L.PEEK_MAX_RETRIES - 1));
    assert.ok(!L.shouldRetryPeek(L.PEEK_MAX_RETRIES));
    assert.ok(!L.shouldRetryPeek(L.PEEK_MAX_RETRIES + 10));
  });
  it('has a finite, small cap', () => {
    assert.ok(Number.isInteger(L.PEEK_MAX_RETRIES));
    assert.ok(L.PEEK_MAX_RETRIES >= 1 && L.PEEK_MAX_RETRIES <= 20);
  });
});

describe('BLE_CHIRP_ENDPOINT', () => {
  it('points at the BLE handler, not the ESP-NOW community chirp', () => {
    assert.equal(L.BLE_CHIRP_ENDPOINT, '/api/ble/chirp/send');
    assert.notEqual(L.BLE_CHIRP_ENDPOINT, '/api/chirp/send');
  });
});
