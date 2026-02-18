'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { startServer } = require('../helpers/start-server');
const { request } = require('../helpers/test-client');

const TOKEN = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';

describe('D5: No Network Scanning', () => {
  let server;

  before(async () => {
    server = await startServer({ devMode: true });
  });

  after(async () => {
    await server.close();
  });

  it('D5: No network scan endpoint exists', async () => {
    const endpoints = [
      { method: 'GET', path: '/api/v1/scan' },
      { method: 'POST', path: '/api/v1/scan' },
      { method: 'GET', path: '/api/v1/discover' },
    ];

    for (const ep of endpoints) {
      const res = await request(server.url + ep.path, {
        method: ep.method,
        headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN },
      });
      assert.equal(res.status, 404, `${ep.method} ${ep.path} should be 404`);
    }
  });

  it('D5: SPA contains no subnet scanning code', () => {
    const spaPath = path.join(__dirname, '..', '..', 'spa', 'app.js');
    const content = fs.readFileSync(spaPath, 'utf8');

    // Must not contain subnet scanning patterns
    const forbidden = [
      /\.1\.\d+/,             // e.g., .1.1 through .1.254 (IP iteration patterns)
      /subnet/i,
      /probe/i,
      /scan\s*\(/,
    ];

    // Check for IP iteration pattern like 192.168.1.${i}
    assert.ok(!content.includes('192.168.1.${'), 'SPA must not contain IP iteration patterns');
    assert.ok(!/subnet/i.test(content), 'SPA must not contain "subnet"');
    assert.ok(!/\bprobe\b/i.test(content), 'SPA must not contain "probe"');
    assert.ok(!/\bscan\s*\(/.test(content), 'SPA must not contain "scan("');

    // Check it doesn't construct IPs in a loop - look for the .255 broadcast pattern
    assert.ok(!content.includes('.255'), 'SPA must not reference .255 broadcast');
  });
});
