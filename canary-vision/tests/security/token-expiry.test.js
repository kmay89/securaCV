'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

describe('Server-side Token Expiry', () => {
  let server, client;

  before(async () => {
    // Set a very short session TTL for testing (1 second = 0.000278 hours)
    process.env.SECURACV_SESSION_TTL_HOURS = '0.000278';
    server = await startServer({ devMode: true });
    client = createClient(server.url, TEST_TOKEN);
  });

  after(async () => {
    delete process.env.SECURACV_SESSION_TTL_HOURS;
    await server.close();
  });

  it('accepts token within TTL window', async () => {
    const res = await client.get('/api/v1/info');
    assert.equal(res.status, 200);
  });

  it('rejects token after TTL expires', async () => {
    // First request establishes the session
    const res1 = await client.get('/api/v1/info');
    assert.equal(res1.status, 200);

    // Wait for the session to expire (TTL is ~1 second)
    await new Promise((resolve) => setTimeout(resolve, 1200));

    // Second request should be rejected
    const res2 = await client.get('/api/v1/info');
    assert.equal(res2.status, 401);
    assert.equal(res2.json.error, 'token_expired');
  });

  it('returns correct error code for expired token', async () => {
    // Wait to ensure previous session expired
    await new Promise((resolve) => setTimeout(resolve, 1200));

    // Make a fresh request — this will start a new session since the old
    // one was cleaned up. We need to verify the error format.
    const res1 = await client.get('/api/v1/info');
    // The previous session's hash was deleted on expiry, so this creates
    // a new session. Immediately check for correct format.
    assert.equal(res1.status, 200);

    // Now wait for this new session to expire
    await new Promise((resolve) => setTimeout(resolve, 1200));

    const res2 = await client.get('/api/v1/info');
    assert.equal(res2.status, 401);
    assert.ok(res2.json.error === 'token_expired');
    assert.ok(res2.json.message.includes('expired'));
  });
});
