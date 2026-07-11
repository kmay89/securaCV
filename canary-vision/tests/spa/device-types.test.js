'use strict';

// Unit tests for the SPA's device-type awareness. app.js is browser-only with no module
// exports, so we load it in a VM sandbox with minimal browser-global stubs (the same
// approach as verified-timeline.test.js) and exercise the pure logic directly: the
// DEVICE_TYPES registry, dt canonicalization, the wizard branch decision, and
// device_type persistence through the CanaryStorage allowlist.

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// In-memory Storage stand-in so CanaryStorage round-trips actually run.
function memStorage() {
  const store = new Map();
  return {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => { store.set(k, String(v)); },
    removeItem: (k) => { store.delete(k); },
    _raw: (k) => store.get(k),
  };
}

function loadApp() {
  const code = fs.readFileSync(path.join(__dirname, '..', '..', 'spa', 'app.js'), 'utf8');
  const noop = () => {};
  const sandbox = {
    document: { addEventListener: noop, getElementById: () => null, createElement: () => ({}) },
    window: { addEventListener: noop, location: { hash: '' }, history: {}, matchMedia: () => ({ matches: false, addEventListener: noop }) },
    localStorage: memStorage(),
    sessionStorage: memStorage(),
    navigator: { onLine: true },
    setInterval: noop, clearInterval: noop, setTimeout: noop, clearTimeout: noop,
    fetch: noop, console,
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(code, sandbox);
  return sandbox;
}

describe('SPA device types', () => {
  let app;
  before(() => { app = loadApp(); });

  it('registry covers the three witness variants plus an unknown fallback', () => {
    ['canary-wap', 'canary-vision', 'canary-sense', 'unknown'].forEach((dt) => {
      const info = app.DEVICE_TYPES[dt];
      assert.ok(info, `missing DEVICE_TYPES entry: ${dt}`);
      assert.equal(typeof info.label, 'string');
      assert.equal(typeof info.tagline, 'string');
      assert.ok(Array.isArray(info.whatsDifferent));
    });
    assert.equal(app.DEVICE_TYPES['canary-wap'].pairing, 'http');
    assert.equal(app.DEVICE_TYPES['canary-vision'].pairing, 'mqtt');
    assert.equal(app.DEVICE_TYPES['canary-sense'].pairing, 'mqtt');
    assert.equal(app.DEVICE_TYPES.unknown.pairing, 'http');
  });

  it('canonicalDeviceType folds any spelling onto the hyphenated key', () => {
    assert.equal(app.canonicalDeviceType('canary_sense'), 'canary-sense');
    assert.equal(app.canonicalDeviceType('Canary Vision'), 'canary-vision');
    assert.equal(app.canonicalDeviceType('  CANARY-WAP  '), 'canary-wap');
    assert.equal(app.canonicalDeviceType(''), '');
    assert.equal(app.canonicalDeviceType(null), '');
    assert.equal(app.canonicalDeviceType(42), '');
  });

  it('deviceTypeInfo falls back to the unknown entry', () => {
    assert.equal(app.deviceTypeInfo('canary-vision').label, 'Canary Vision');
    assert.equal(app.deviceTypeInfo('canary-mystery'), app.DEVICE_TYPES.unknown);
    assert.equal(app.deviceTypeInfo(''), app.DEVICE_TYPES.unknown);
  });

  it('wizardPathForPeer routes vision/sense to MQTT and everything else to HTTP', () => {
    // Peers relay the mDNS TXT `dt` key…
    assert.equal(app.wizardPathForPeer({ dt: 'canary-vision' }), 'mqtt');
    assert.equal(app.wizardPathForPeer({ dt: 'canary-sense' }), 'mqtt');
    assert.equal(app.wizardPathForPeer({ dt: 'canary-wap' }), 'http');
    // …but the long device_type spelling (and sloppy casing) must work too.
    assert.equal(app.wizardPathForPeer({ device_type: 'canary_sense' }), 'mqtt');
    assert.equal(app.wizardPathForPeer({ device_type: 'Canary-Vision' }), 'mqtt');
    // Untyped peers keep today's HTTP BOOT-tap flow.
    assert.equal(app.wizardPathForPeer({ device_id: 'canary-old1' }), 'http');
    assert.equal(app.wizardPathForPeer(null), 'http');
  });

  it('CanaryStorage persists device_type through the save/load allowlist', () => {
    app.CanaryStorage.saveDevices([{
      id: 'canary-b1c2',
      name: 'Garage',
      base_url: 'http://canary-b1c2.local',
      room: 'Garage',
      device_type: 'canary-vision',
      token: 'cv_b1c2_secret',
      last_info: { device_id: 'canary-b1c2' },
      added_at: '2026-02-18T15:30:00.000Z',
    }]);

    const loaded = app.CanaryStorage.getDevices();
    assert.equal(loaded.length, 1);
    assert.equal(loaded[0].device_type, 'canary-vision');

    // The allowlist must not leak the token into localStorage.
    const raw = app.localStorage._raw('canary_devices');
    assert.ok(raw.includes('"device_type":"canary-vision"'));
    assert.ok(!raw.includes('cv_b1c2_secret'));
  });
});
