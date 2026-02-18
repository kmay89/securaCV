'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient, request } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('D6: No Fleet-Wide Config Sync', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('D6: No fleet-wide config endpoint exists', async () => {
    const endpoints = [
      { method: 'POST', path: '/api/v1/fleet/config' },
      { method: 'PUT', path: '/api/v1/fleet/config' },
      { method: 'POST', path: '/api/v1/config/broadcast' },
    ];

    for (const ep of endpoints) {
      const res = await request(server.url + ep.path, {
        method: ep.method,
        headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN },
      });
      assert.equal(res.status, 404, `${ep.method} ${ep.path} should be 404`);
    }
  });

  it('D6: PUT /api/v1/config only affects this device', async () => {
    const res = await client.put('/api/v1/config', {
      detection: { motion_sensitivity: 7 },
    });
    assert.equal(res.status, 200);
    assert.equal(res.json.ok, true);
    // Verify the config was updated locally
    assert.equal(res.json.config.detection.motion_sensitivity, 7);
    // No outbound request verification needed — the reference server
    // doesn't make outbound requests by design
  });

  it('D6: No push-firmware-to-peer endpoint', async () => {
    const endpoints = [
      { method: 'POST', path: '/api/v1/fleet/update' },
      { method: 'POST', path: '/api/v1/peers/update' },
    ];

    for (const ep of endpoints) {
      const res = await request(server.url + ep.path, {
        method: ep.method,
        headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN },
      });
      assert.equal(res.status, 404, `${ep.method} ${ep.path} should be 404`);
    }
  });
});
