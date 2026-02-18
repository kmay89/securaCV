'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('GET /api/v1/peers', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('returns peer list', async () => {
    const res = await client.get('/api/v1/peers');
    assert.equal(res.status, 200);
    assert.ok(Array.isArray(res.json.peers));
    assert.ok(res.json.peers.length > 0);
  });

  it('peer entries have expected fields', async () => {
    const res = await client.get('/api/v1/peers');
    const peer = res.json.peers[0];
    assert.ok(peer.device_id);
    assert.ok(peer.name);
    assert.ok(peer.ip);
    assert.ok(peer.last_seen);
  });
});
