'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';

describe('POST /api/v1/reboot', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('returns success on first reboot request', async () => {
    const res = await client.post('/api/v1/reboot');
    assert.equal(res.status, 200);
    assert.equal(res.json.ok, true);
    assert.ok(res.json.message.includes('Rebooting'));
  });

  it('rate limits subsequent reboot requests', async () => {
    // First reboot already done in previous test, but state is shared
    // Try another reboot
    const res = await client.post('/api/v1/reboot');
    assert.equal(res.status, 429);
    assert.ok(res.headers['retry-after']);
  });
});
