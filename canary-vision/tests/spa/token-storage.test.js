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
});
