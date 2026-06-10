'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { request } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

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

  it('D3: No CORS headers for peer origin (lateral movement prevention)', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        Origin: 'http://canary-b1c2.local',
      },
    });
    assert.equal(res.status, 200);
    assert.equal(res.headers['access-control-allow-origin'], undefined,
      'Peer origin must NOT get CORS headers (lateral movement vector)');
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

  it('D3: OPTIONS preflight for peer origin returns no CORS (lateral movement prevention)', async () => {
    const res = await request(server.url + '/api/v1/info', {
      method: 'OPTIONS',
      headers: {
        Host: '127.0.0.1',
        Origin: 'http://canary-b1c2.local',
        'Access-Control-Request-Method': 'GET',
      },
    });
    assert.equal(res.status, 204);
    assert.equal(res.headers['access-control-allow-origin'], undefined,
      'Peer origin preflight must NOT get CORS headers');
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

describe('Trust-on-pair CORS', () => {
  let server;
  const APP_ORIGIN = 'http://canary-kitchen.local';

  before(async () => {
    server = await startServer({ devMode: true });
  });

  after(async () => {
    await server.close();
  });

  it('provisioning-receipt answers CORS for a private origin (gate closed)', async () => {
    const res = await request(server.url + '/api/provisioning-receipt', {
      headers: { Host: '127.0.0.1', Origin: APP_ORIGIN },
    });
    assert.equal(res.status, 403);
    assert.equal(res.headers['access-control-allow-origin'], APP_ORIGIN,
      'Private origins must be able to read the gate-closed 403 (gate_ttl_seconds)');
  });

  it('provisioning-receipt answers CORS for IPv6 ULA and link-local origins', async () => {
    for (const origin of ['http://[fd12:3456::1]', 'http://[fe80::1]', 'http://169.254.10.20']) {
      const res = await request(server.url + '/api/provisioning-receipt', {
        headers: { Host: '127.0.0.1', Origin: origin },
      });
      assert.equal(res.headers['access-control-allow-origin'], origin,
        origin + ' is a private-network scope and must be able to pair');
    }
  });

  it('provisioning-receipt gives no CORS to public origins', async () => {
    const res = await request(server.url + '/api/provisioning-receipt', {
      headers: { Host: '127.0.0.1', Origin: 'http://evil.com' },
    });
    assert.equal(res.headers['access-control-allow-origin'], undefined);
  });

  it('an unpaired private origin gets no CORS on authenticated endpoints', async () => {
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN, Origin: APP_ORIGIN },
    });
    assert.equal(res.headers['access-control-allow-origin'], undefined,
      'Lateral movement defense must hold until a BOOT press enrolls the origin');
  });

  it('a BOOT press enrolls the receiving origin for the rest of the API', async () => {
    await request(server.url + '/api/dev/press-boot', {
      method: 'POST',
      headers: { Host: '127.0.0.1' },
    });
    const receipt = await request(server.url + '/api/provisioning-receipt', {
      headers: { Host: '127.0.0.1', Origin: APP_ORIGIN },
    });
    assert.equal(receipt.status, 200);

    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN, Origin: APP_ORIGIN },
    });
    assert.equal(res.headers['access-control-allow-origin'], APP_ORIGIN,
      'The origin that received the receipt must be durably allowed');
  });

  it('a public origin is never enrolled, even with the gate open', async () => {
    await request(server.url + '/api/dev/press-boot', {
      method: 'POST',
      headers: { Host: '127.0.0.1' },
    });
    await request(server.url + '/api/provisioning-receipt', {
      headers: { Host: '127.0.0.1', Origin: 'http://evil.com' },
    });
    const res = await request(server.url + '/api/v1/info', {
      headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN, Origin: 'http://evil.com' },
    });
    assert.equal(res.headers['access-control-allow-origin'], undefined,
      'Public origins must never be enrolled by trust-on-pair');
  });
});
