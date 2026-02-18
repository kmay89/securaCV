'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

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

    // Verify token is part of device object, not stored separately
    assert.ok(js.includes('.token'), 'SPA must access token as a device property');
  });
});
