'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';

describe('GET /api/v1/info', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('returns device info with expected fields', async () => {
    const res = await client.get('/api/v1/info');
    assert.equal(res.status, 200);
    assert.equal(res.json.device_id, 'canary-a3f7');
    assert.equal(res.json.name, 'Front Porch');
    assert.equal(res.json.model, 'XIAO ESP32S3');
    assert.equal(res.json.firmware_version, '0.4.1');
    assert.equal(typeof res.json.uptime_s, 'number');
    assert.ok(res.json.last_seen);
    assert.equal(res.json.wifi_rssi, -42);
    assert.equal(res.json.ip, '192.168.1.47');
    assert.ok(Array.isArray(res.json.capabilities));
    assert.ok(res.json.capabilities.includes('camera_peek'));
    assert.ok(res.json.capabilities.includes('ota_update'));
    assert.ok(res.json.capabilities.includes('mqtt'));
    assert.ok(res.json.capabilities.includes('witness_log'));
  });
});
