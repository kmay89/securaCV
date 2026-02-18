'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer } = require('../helpers/start-server');
const { request } = require('../helpers/test-client');

const TOKEN = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';

describe('D3: Restrictive CORS', () => {
  let server;
  const peers = [
    { device_id: 'canary-b1c2', name: 'Garage', ip: '192.168.1.103', last_seen: '2026-02-18T14:20:00Z' },
  ];

  before(async () => {
    server = await startServer({ devMode: true, peers });
  });

  after(async () => {
    await server.close();
  });

  it('D3: No CORS headers on same-origin request (no Origin header)', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN },
    });
    assert.equal(res.status, 200);
    assert.equal(res.headers['access-control-allow-origin'], undefined);
  });

  it('D3: CORS headers present for registered peer origin', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        Origin: 'http://canary-b1c2.local',
      },
    });
    assert.equal(res.status, 200);
    assert.equal(res.headers['access-control-allow-origin'], 'http://canary-b1c2.local');
  });

  it('D3: No CORS headers for unknown origin', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        Origin: 'http://evil.com',
      },
    });
    // Response may still be 200 but no CORS headers
    assert.equal(res.headers['access-control-allow-origin'], undefined,
      'Unknown origin should not get CORS headers');
  });

  it('D3: No CORS headers for wildcard origin', async () => {
    // Verify we never set * as ACAO
    const res = await request(server.url + '/api/v1/info', {
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        Origin: 'http://canary-b1c2.local',
      },
    });
    assert.notEqual(res.headers['access-control-allow-origin'], '*',
      'Access-Control-Allow-Origin must NEVER be *');
  });

  it('D3: OPTIONS preflight for registered peer returns correct headers', async () => {
    const res = await request(server.url + '/api/v1/info', {
      method: 'OPTIONS',
      headers: {
        Host: '127.0.0.1',
        Origin: 'http://canary-b1c2.local',
        'Access-Control-Request-Method': 'GET',
      },
    });
    assert.equal(res.status, 204);
    assert.equal(res.headers['access-control-allow-origin'], 'http://canary-b1c2.local');
    assert.ok(res.headers['access-control-allow-methods'].includes('GET'));
    assert.ok(res.headers['access-control-allow-headers'].includes('X-Canary-Token'));
  });

  it('D3: OPTIONS preflight for unknown origin returns no CORS', async () => {
    const res = await request(server.url + '/api/v1/info', {
      method: 'OPTIONS',
      headers: {
        Host: '127.0.0.1',
        Origin: 'http://evil.com',
        'Access-Control-Request-Method': 'GET',
      },
    });
    assert.equal(res.headers['access-control-allow-origin'], undefined,
      'Unknown origin OPTIONS should not get CORS headers');
  });
});
