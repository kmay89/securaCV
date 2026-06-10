'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('Provisioning receipt (BOOT gate)', () => {
  let server, anonClient;

  before(async () => {
    server = await startServer({ devMode: true });
    anonClient = createClient(server.url, null);
  });

  after(async () => {
    await server.close();
  });

  it('GET /api/provisioning-receipt is 403 with gate_ttl_seconds while gate is closed', async () => {
    const res = await anonClient.get('/api/provisioning-receipt');
    assert.equal(res.status, 403);
    assert.equal(res.json.error, 'gate_closed');
    assert.ok(Number.isFinite(res.json.gate_ttl_seconds));
    assert.ok(res.json.gate_ttl_seconds > 0);
  });

  it('press-boot opens the gate and the receipt is issued without a token', async () => {
    const press = await anonClient.post('/api/dev/press-boot', {});
    assert.equal(press.status, 200);
    assert.equal(press.json.ok, true);

    const res = await anonClient.get('/api/provisioning-receipt');
    assert.equal(res.status, 200);
    assert.equal(res.json.device_id, server.state.device.device_id);
    assert.equal(res.json.token, TOKEN);
    assert.ok(res.json.base_url.startsWith('http://'));
  });

  it('receipt is one-shot: a second read after success is 403 again', async () => {
    await anonClient.post('/api/dev/press-boot', {});
    const first = await anonClient.get('/api/provisioning-receipt');
    assert.equal(first.status, 200);

    const second = await anonClient.get('/api/provisioning-receipt');
    assert.equal(second.status, 403);
  });

  it('press-boot is hidden outside dev mode', async () => {
    server.state.device.devMode = false;
    try {
      const res = await anonClient.post('/api/dev/press-boot', {});
      assert.equal(res.status, 404);
    } finally {
      server.state.device.devMode = true;
    }
  });
});

describe('Identify', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('requires auth', async () => {
    const anon = createClient(server.url, null);
    const res = await anon.post('/api/identify', {});
    assert.equal(res.status, 401);
  });

  it('defaults to 15000 ms and reports visual_only', async () => {
    const res = await client.post('/api/identify', {});
    assert.equal(res.status, 200);
    assert.equal(res.json.ok, true);
    assert.equal(res.json.duration_ms, 15000);
    assert.equal(res.json.visual_only, true);
  });

  it('clamps duration_ms to 1000-60000', async () => {
    const low = await client.post('/api/identify', { duration_ms: 5 });
    assert.equal(low.json.duration_ms, 1000);

    const high = await client.post('/api/identify', { duration_ms: 999999 });
    assert.equal(high.json.duration_ms, 60000);
  });
});

describe('Device name', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('requires auth', async () => {
    const anon = createClient(server.url, null);
    const res = await anon.post('/api/device-name', { name: 'kitchen' });
    assert.equal(res.status, 401);
  });

  it('renames the device and returns the new mdns host', async () => {
    const res = await client.post('/api/device-name', { name: 'kitchen' });
    assert.equal(res.status, 200);
    assert.equal(res.json.ok, true);
    assert.equal(res.json.device_name, 'kitchen');
    assert.equal(res.json.mdns_host, 'canary-kitchen');
    assert.equal(server.state.device.name, 'kitchen');
    assert.equal(server.state.device.mdns_hostname, 'canary-kitchen.local');
  });

  it('sanitizes names to lowercase a-z, 0-9, hyphen', async () => {
    const res = await client.post('/api/device-name', { name: '  Living Room! ' });
    assert.equal(res.status, 200);
    assert.equal(res.json.device_name, 'living-room');
    assert.equal(res.json.mdns_host, 'canary-living-room');
  });

  it('rejects empty and unsalvageable names', async () => {
    const empty = await client.post('/api/device-name', { name: '   ' });
    assert.equal(empty.status, 400);

    const junk = await client.post('/api/device-name', { name: '!!!' });
    assert.equal(junk.status, 400);

    const missing = await client.post('/api/device-name', {});
    assert.equal(missing.status, 400);
  });
});
