'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const SPA_PATH = path.join(__dirname, '..', '..', 'spa', 'app.js');

describe('SPA Magic Pairing', () => {
  const js = fs.readFileSync(SPA_PATH, 'utf8');

  it('leads with the BOOT-tap receipt capture', () => {
    assert.ok(js.includes("'/api/provisioning-receipt'") || js.includes('"/api/provisioning-receipt"'),
      'SPA must poll the provisioning-receipt endpoint');
    assert.ok(/Short-tap the BOOT button/.test(js),
      'SPA must instruct the user to tap BOOT (zero-typing path)');
    assert.ok(/gate_ttl_seconds/.test(js),
      'SPA must honor the gate TTL advertised in the 403 body');
  });

  it('every pairing path funnels through private-URL validation', () => {
    assert.ok(/function completePairing/.test(js),
      'SPA must centralize pairing in completePairing()');
    assert.ok(/isPrivateUrl/.test(js),
      'SPA must keep the private-network guard');
  });

  it('QR scanning is capability-gated behind a secure context', () => {
    assert.ok(/isSecureContext/.test(js),
      'QR path must check isSecureContext before offering the camera');
    assert.ok(/BarcodeDetector/.test(js),
      'QR path must use the native BarcodeDetector (no decoding deps)');
  });

  it('offers identify and rename so the user can tell devices apart', () => {
    assert.ok(js.includes("'/api/identify'") || js.includes('"/api/identify"'),
      'SPA must offer the Identify (blink) affordance');
    assert.ok(js.includes("'/api/device-name'") || js.includes('"/api/device-name"'),
      'SPA must offer renaming via the device endpoint');
  });

  it('encourages the flock: chained add and room grouping stay client-side', () => {
    assert.ok(/Add another Canary/.test(js),
      'Successful pairing must invite adding the next device');
    assert.ok(/setDeviceRoom/.test(js),
      'Rooms must be stored client-side via CanaryStorage');
    assert.ok(!/\/api\/v?\d*\/?room/.test(js),
      'Rooms must NOT be synced to devices (no fleet-wide state, per D6)');
  });
});
