'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('GET /api/v1/logs', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('returns log entries', async () => {
    const res = await client.get('/api/v1/logs');
    assert.equal(res.status, 200);
    assert.ok(Array.isArray(res.json.logs));
    assert.ok(res.json.logs.length > 0);
    // Each entry has ts, level, msg
    const entry = res.json.logs[0];
    assert.ok(entry.ts);
    assert.ok(entry.level);
    assert.ok(entry.msg);
  });

  it('respects tail parameter', async () => {
    const res = await client.get('/api/v1/logs?tail=2');
    assert.equal(res.status, 200);
    assert.ok(res.json.logs.length <= 2);
  });

  it('respects level filter', async () => {
    const res = await client.get('/api/v1/logs?level=INFO');
    assert.equal(res.status, 200);
    for (const entry of res.json.logs) {
      assert.equal(entry.level, 'INFO');
    }
  });

  it('caps tail at 500', async () => {
    const res = await client.get('/api/v1/logs?tail=9999');
    assert.equal(res.status, 200);
    assert.ok(res.json.logs.length <= 500);
  });
});
