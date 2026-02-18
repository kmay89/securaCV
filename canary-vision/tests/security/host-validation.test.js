'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer } = require('../helpers/start-server');
const { request } = require('../helpers/test-client');

const TOKEN = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';

describe('D2: Host Header Validation', () => {
  let server;

  before(async () => {
    server = await startServer({ devMode: false });
  });

  after(async () => {
    await server.close();
  });

  it('D2: Accepts request with device mDNS hostname as Host', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: 'canary-a3f7.local', 'X-Canary-Token': TOKEN },
    });
    assert.equal(res.status, 200);
  });

  it('D2: Accepts request with device IP as Host', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '192.168.1.47', 'X-Canary-Token': TOKEN },
    });
    assert.equal(res.status, 200);
  });

  it('D2: Accepts request with Host including port', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: 'canary-a3f7.local:80', 'X-Canary-Token': TOKEN },
    });
    assert.equal(res.status, 200);
  });

  it('D2: Rejects request with attacker domain as Host', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: 'evil-ad.com', 'X-Canary-Token': TOKEN },
    });
    assert.equal(res.status, 403);
    assert.equal(res.json.error, 'host_rejected');
  });

  it('D2: Rejects request with attacker subdomain as Host', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: 'canary-a3f7.local.evil.com', 'X-Canary-Token': TOKEN },
    });
    assert.equal(res.status, 403);
  });

  it('D2: Rejects request with IP that is not this device', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '192.168.1.99', 'X-Canary-Token': TOKEN },
    });
    assert.equal(res.status, 403);
  });

  it('D2: Rejects request with no Host header', async () => {
    const http = require('node:http');
    const parsed = new URL(server.url);
    const res = await new Promise((resolve, reject) => {
      const req = http.request({
        hostname: parsed.hostname,
        port: parsed.port,
        path: '/api/v1/info',
        method: 'GET',
        headers: { 'X-Canary-Token': TOKEN },
        setHost: false,
      }, (res) => {
        let body = '';
        res.on('data', c => body += c);
        res.on('end', () => {
          let json = null;
          try { json = JSON.parse(body); } catch (e) { /* may not be JSON */ }
          resolve({ status: res.statusCode, json });
        });
      });
      req.on('error', reject);
      req.end();
    });
    // Node.js HTTP/1.1 parser may reject with 400 before our middleware runs,
    // or our middleware catches it with 403. Either way, the request is rejected.
    assert.ok(res.status === 403 || res.status === 400,
      `Expected 403 or 400, got ${res.status}`);
  });

  it('D2: Host validation runs BEFORE auth', async () => {
    // Send request with bad Host and no token
    // Must get 403 (host rejected), NOT 401 (token required)
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: 'evil.com' },
      // Deliberately no X-Canary-Token
    });
    assert.equal(res.status, 403, 'Host validation should run before auth');
    assert.equal(res.json.error, 'host_rejected');
  });
});
