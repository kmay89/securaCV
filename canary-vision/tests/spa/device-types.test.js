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

// The canary-wap card must not say this SPA pairs a WAP (sweep F355). The
// canary-wap firmware does not answer the SPA's pairing: its receipt route
// sends no Access-Control-Allow-Origin, so a browser hides its 403 and its
// receipt from this page (recorded in Chromium against a stub that answers
// the way the sketch does: the capture never saw asked=true and ended
// onTimeout(false)), it reads `Authorization: Bearer` and never
// `X-Canary-Token`, and it registers no /api/v1/info or /api/v1/peers. The
// card is held to those facts, read from the sketch: if a WAP ever answers
// this SPA's pairing, the premise row fails and names what to revisit.
const WAP_SKETCH_DIR = path.join(__dirname, '..', '..', '..',
  'firmware', 'projects', 'canary-wap', 'arduino', 'canary_wap');

// Every card line that says or implies the WAP pairs in this SPA.
function spaPairingClaims(lines) {
  return lines.filter((line) => /\b(?:right )?here\b/i.test(line)
    || /\bover HTTP\b/i.test(line)
    || /\bBOOT\b/.test(line)
    || /\bpairs? (?:in|from|with) this app\b/i.test(line));
}

function functionBody(src, name) {
  const start = src.indexOf(`static esp_err_t ${name}(`);
  assert.ok(start >= 0, `canary_wap.ino no longer defines ${name}()`);
  const end = src.indexOf('\n}\n', start);
  return src.slice(start, end);
}

describe('SPA device types: the canary-wap card makes no SPA-pairing claim', () => {
  let app;
  before(() => { app = loadApp(); });

  it('premise: the canary-wap firmware does not answer this SPA\'s pairing', () => {
    const files = fs.readdirSync(WAP_SKETCH_DIR).filter((f) => /\.(?:ino|h|cpp)$/.test(f));
    const sketch = files.map((f) => fs.readFileSync(path.join(WAP_SKETCH_DIR, f), 'utf8')).join('\n');
    const ino = fs.readFileSync(path.join(WAP_SKETCH_DIR, 'canary_wap.ino'), 'utf8');
    const revisit = ' — the WAP may now answer this SPA\'s pairing: revisit '
      + "DEVICE_TYPES['canary-wap'] and docs/discovery.md §2-§4";
    assert.ok(!/\.uri\s*=\s*"\/api\/v1\/(?:info|peers)"/.test(sketch),
      'the sketch registers /api/v1/info or /api/v1/peers' + revisit);
    assert.ok(!/X-Canary-Token/i.test(sketch), 'the sketch reads X-Canary-Token' + revisit);
    for (const fn of ['handle_provisioning_receipt', 'send_provisioning_receipt']) {
      assert.ok(!functionBody(ino, fn).includes('Access-Control-Allow-Origin'),
        `${fn}() sends Access-Control-Allow-Origin` + revisit);
    }
  });

  it('no line of the card says it pairs in this SPA, and one says where it pairs', () => {
    const info = app.DEVICE_TYPES['canary-wap'];
    const lines = [info.tagline, ...info.whatsDifferent, info.proof].filter(Boolean);
    assert.deepEqual(spaPairingClaims(lines), [],
      'the canary-wap card claims SPA pairing its firmware does not answer');
    const where = lines.find((l) => /dashboard/i.test(l) && /iPhone app/.test(l) && /Home Assistant/.test(l));
    assert.ok(where, 'the canary-wap card must say where a WAP pairs (its dashboard, the iPhone app, Home Assistant)');
    assert.match(where, /not\b.*\bthis app\b/i, 'and that it does not pair in this app');
  });

  it('the claim check catches the line the card used to carry', () => {
    assert.equal(spaPairingClaims(['Pairs right here over HTTP with a short tap of its BOOT button']).length, 1);
    assert.equal(spaPairingClaims(['Pairs right here']).length, 1);
    assert.equal(spaPairingClaims(['Pairs with this app over HTTP']).length, 1);
  });

  it('keeps the http wizard branch: the copy changed, not the flow', () => {
    assert.equal(app.DEVICE_TYPES['canary-wap'].pairing, 'http');
  });
});
