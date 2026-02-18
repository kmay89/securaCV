'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient, request } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('D1: Mandatory API Token', () => {
  let server, client;

  before(async () => {
    server = await startServer({
      devMode: true,
      rateLimit: { authFailLimit: 100, generalLimit: 200 },
    });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('D1: Rejects request without X-Canary-Token header', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '127.0.0.1' },
    });
    assert.equal(res.status, 401);
    assert.equal(res.json.error, 'token_required');
  });

  it('D1: Rejects request with wrong token', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '127.0.0.1', 'X-Canary-Token': 'wrong' },
    });
    assert.equal(res.status, 401);
    assert.equal(res.json.error, 'token_invalid');
  });

  it('D1: Rejects request with empty token', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '127.0.0.1', 'X-Canary-Token': '' },
    });
    assert.equal(res.status, 401);
  });

  it('D1: Accepts request with valid token', async () => {
    const res = await client.get('/api/v1/info');
    assert.equal(res.status, 200);
  });

  it('D1: Static files do NOT require token', async () => {
    // GET /
    const htmlRes = await request(server.url + '/', {
      headers: { Host: '127.0.0.1' },
    });
    assert.equal(htmlRes.status, 200);
    assert.ok(htmlRes.headers['content-type'].includes('html'));

    // GET /app.js
    const jsRes = await request(server.url + '/app.js', {
      headers: { Host: '127.0.0.1' },
    });
    assert.equal(jsRes.status, 200);

    // GET /styles.css
    const cssRes = await request(server.url + '/styles.css', {
      headers: { Host: '127.0.0.1' },
    });
    assert.equal(cssRes.status, 200);

    // GET /app.webmanifest
    const manifestRes = await request(server.url + '/app.webmanifest', {
      headers: { Host: '127.0.0.1' },
    });
    assert.equal(manifestRes.status, 200);
  });

  it('D1: ALL API routes require token', async () => {
    const routes = [
      { method: 'GET', path: '/api/v1/info' },
      { method: 'GET', path: '/api/v1/config' },
      { method: 'PUT', path: '/api/v1/config' },
      { method: 'GET', path: '/api/v1/config/network' },
      { method: 'PUT', path: '/api/v1/config/network' },
      { method: 'GET', path: '/api/v1/logs' },
      { method: 'GET', path: '/api/v1/witness' },
      { method: 'GET', path: '/api/v1/peers' },
      { method: 'POST', path: '/api/v1/reboot' },
      { method: 'GET', path: '/api/v1/update/check' },
      { method: 'POST', path: '/api/v1/update' },
    ];

    for (const route of routes) {
      const res = await request(server.url + route.path, {
        method: route.method,
        headers: { Host: '127.0.0.1' },
      });
      assert.equal(res.status, 401, `${route.method} ${route.path} should require token`);
    }
  });

  it('D1: Token comparison is constant-time', async () => {
    // Completely wrong token (first char differs)
    const wrongFirst = 'xv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';
    // Almost correct token (last char differs)
    const wrongLast = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7x';

    const iterations = 20;
    const timesFirst = [];
    const timesLast = [];

    for (let i = 0; i < iterations; i++) {
      const startA = performance.now();
      await request(server.url + '/api/v1/info', {
        headers: { Host: '127.0.0.1', 'X-Canary-Token': wrongFirst },
      });
      timesFirst.push(performance.now() - startA);

      const startB = performance.now();
      await request(server.url + '/api/v1/info', {
        headers: { Host: '127.0.0.1', 'X-Canary-Token': wrongLast },
      });
      timesLast.push(performance.now() - startB);
    }

    const avgFirst = timesFirst.reduce((a, b) => a + b, 0) / iterations;
    const avgLast = timesLast.reduce((a, b) => a + b, 0) / iterations;
    const diff = Math.abs(avgFirst - avgLast);

    assert.ok(diff < 10, `Timing difference ${diff.toFixed(2)}ms exceeds 10ms threshold (possible timing leak)`);
  });
});
