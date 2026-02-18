'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('Config API', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('GET /api/v1/config returns all sections', async () => {
    const res = await client.get('/api/v1/config');
    assert.equal(res.status, 200);
    assert.ok(res.json.network);
    assert.ok(res.json.privacy);
    assert.ok(res.json.detection);
    assert.ok(res.json.integrations);
  });

  it('PUT /api/v1/config validates auto_purge_hours', async () => {
    const res = await client.put('/api/v1/config', {
      privacy: { auto_purge_hours: 999 },
    });
    assert.equal(res.status, 400);
    assert.equal(res.json.error, 'invalid_config');
  });

  it('PUT /api/v1/config validates motion_sensitivity range', async () => {
    const tooHigh = await client.put('/api/v1/config', {
      detection: { motion_sensitivity: 11 },
    });
    assert.equal(tooHigh.status, 400);

    const tooLow = await client.put('/api/v1/config', {
      detection: { motion_sensitivity: 0 },
    });
    assert.equal(tooLow.status, 400);
  });

  it('PUT /api/v1/config/network updates only network section', async () => {
    // Save current privacy config
    const before = await client.get('/api/v1/config');
    const privacyBefore = JSON.stringify(before.json.privacy);

    // Update network
    const res = await client.put('/api/v1/config/network', {
      mqtt_port: 1884,
    }, { host: '127.0.0.1' });
    assert.equal(res.status, 200);

    // Verify privacy unchanged
    const after = await client.get('/api/v1/config');
    assert.equal(JSON.stringify(after.json.privacy), privacyBefore);
    assert.equal(after.json.network.mqtt_port, 1884);
  });

  it('PUT /api/v1/config/:section returns 404 for unknown section', async () => {
    const res = await client.put('/api/v1/config/nonexistent', {});
    assert.equal(res.status, 404);
  });

  it('PUT /api/v1/config validates mqtt_port range', async () => {
    const res = await client.put('/api/v1/config', {
      network: { mqtt_port: 99999 },
    });
    assert.equal(res.status, 400);
  });

  it('PUT /api/v1/config validates wifi_ssid is non-empty', async () => {
    const res = await client.put('/api/v1/config', {
      network: { wifi_ssid: '' },
    });
    assert.equal(res.status, 400);
  });

  it('GET /api/v1/config/:section returns section data', async () => {
    const res = await client.get('/api/v1/config/privacy');
    assert.equal(res.status, 200);
    assert.equal(typeof res.json.camera_enabled, 'boolean');
    assert.equal(typeof res.json.camera_peek_enabled, 'boolean');
  });
});
