'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

describe('SPA Token Storage', () => {
  it('Token is stored per-device in structured format', () => {
    const js = fs.readFileSync(path.join(__dirname, '..', '..', 'spa', 'app.js'), 'utf8');

    // Verify localStorage key is "canary_devices"
    assert.ok(js.includes("'canary_devices'") || js.includes('"canary_devices"'),
      'SPA must use "canary_devices" as the localStorage key');

    // Verify token is stored as property of device entries
    // The storage module should have getItem/setItem with the key
    assert.ok(js.includes('localStorage.getItem'), 'SPA must read from localStorage');
    assert.ok(js.includes('localStorage.setItem'), 'SPA must write to localStorage');

    // A loaded device carries its token as a property (rehydrated from
    // sessionStorage by getDevices(), the row below)
    assert.ok(js.includes('.token'), 'SPA must access token as a device property');
  });

  // docs/security.md's SPA list: a token is held in sessionStorage under
  // canary_token:<device id>, and localStorage's canary_devices list keeps
  // none. Run in a VM sandbox with in-memory storages.
  it('keeps each token in sessionStorage under canary_token:<id>, never in localStorage', () => {
    const mem = () => {
      const store = new Map();
      return { getItem: (k) => (store.has(k) ? store.get(k) : null), setItem: (k, v) => { store.set(k, String(v)); },
        removeItem: (k) => { store.delete(k); }, keys: () => [...store.keys()], raw: (k) => store.get(k) };
    };
    const noop = () => {};
    const sandbox = {
      document: { addEventListener: noop, getElementById: () => null, createElement: () => ({}) },
      window: { addEventListener: noop, location: { hash: '' }, history: {},
        matchMedia: () => ({ matches: false, addEventListener: noop }) },
      localStorage: mem(), sessionStorage: mem(), navigator: { onLine: true },
      setInterval: noop, clearInterval: noop, setTimeout: noop, clearTimeout: noop, fetch: noop, console,
    };
    sandbox.globalThis = sandbox;
    vm.createContext(sandbox);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '..', '..', 'spa', 'app.js'), 'utf8'), sandbox);

    sandbox.CanaryStorage.addDevice({ id: 'canary-b1c2', name: 'Garage', base_url: 'http://canary-b1c2.local',
      token: 'cv_b1c2_secret' });
    assert.equal(sandbox.sessionStorage.raw('canary_token:canary-b1c2'), 'cv_b1c2_secret');
    assert.deepEqual(sandbox.localStorage.keys(), ['canary_devices']);
    assert.ok(!sandbox.localStorage.raw('canary_devices').includes('cv_b1c2_secret'),
      'a token reached localStorage');
    assert.equal(sandbox.CanaryStorage.getDevice('canary-b1c2').token, 'cv_b1c2_secret');
  });

  // The same list names every key app.js writes to localStorage: the device
  // list and two view preferences, none a credential. A key app.js starts
  // writing without a word there fails, and so does a key the doc forgets
  // (it once said localStorage keeps only the device list).
  it("every localStorage key app.js writes is one docs/security.md's SPA list names", () => {
    const js = fs.readFileSync(path.join(__dirname, '..', '..', 'spa', 'app.js'), 'utf8');
    const constant = (re, what) => {
      const m = js.match(re);
      assert.ok(m, what + ' is gone');
      return m[1];
    };
    const named = {
      'CanaryStorage.KEY': constant(/^\s*KEY:\s*'([^']+)'/m, 'CanaryStorage.KEY'),
      NOTIF_PREFS_KEY: constant(/^var NOTIF_PREFS_KEY = '([^']+)';/m, 'NOTIF_PREFS_KEY'),
    };
    const written = [...js.matchAll(/localStorage\.setItem\(\s*([^,]+?)\s*,/g)].map((m) => {
      const arg = m[1];
      const literal = arg.match(/^'([^']+)'$/);
      if (literal) return literal[1];
      assert.ok(arg in named, 'localStorage.setItem() with a key this row cannot read: ' + arg);
      return named[arg];
    });
    assert.deepEqual([...new Set(written)].sort(),
      ['canary_devices', 'canary_notif_prefs', 'cv_coverage_hint_dismissed']);

    const doc = fs.readFileSync(path.join(__dirname, '..', '..', 'docs', 'security.md'), 'utf8');
    const line = doc.split('\n').find((l) => l.startsWith('- Tokens held per device in `sessionStorage`'));
    assert.ok(line, "docs/security.md's SPA list lost its token line");
    for (const key of new Set(written)) {
      assert.ok(line.includes('`' + key + '`'), 'docs/security.md does not name localStorage key ' + key);
    }
    assert.ok(line.includes('`canary_token:<device id>`'), 'docs/security.md does not name the token key');
  });
});
