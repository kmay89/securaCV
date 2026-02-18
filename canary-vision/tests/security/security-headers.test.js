'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer } = require('../helpers/start-server');
const { createClient, request } = require('../helpers/test-client');

const TOKEN = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';

describe('Security Headers', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  function assertSecurityHeaders(headers) {
    assert.equal(headers['x-content-type-options'], 'nosniff');
    assert.equal(headers['x-frame-options'], 'DENY');
    assert.ok(headers['content-security-policy'].startsWith("default-src 'self'"));
    assert.equal(headers['cache-control'], 'no-store');
    assert.equal(headers['referrer-policy'], 'no-referrer');
  }

  it('Security headers present on API responses', async () => {
    const res = await client.get('/api/v1/info');
    assert.equal(res.status, 200);
    assertSecurityHeaders(res.headers);
  });

  it('Security headers present on static file responses', async () => {
    const res = await request(server.url + '/', {
      headers: { Host: '127.0.0.1' },
    });
    assert.equal(res.status, 200);
    assertSecurityHeaders(res.headers);
  });

  it('X-Frame-Options DENY prevents iframe embedding', async () => {
    const res = await client.get('/api/v1/info');
    assert.equal(res.headers['x-frame-options'], 'DENY',
      'X-Frame-Options must be exactly DENY, not SAMEORIGIN');
  });
});
