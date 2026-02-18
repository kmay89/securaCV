'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer } = require('../helpers/start-server');
const { createClient, request } = require('../helpers/test-client');

const TOKEN = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';

describe('D9: Rate Limiting', () => {
  it('D9: Allows 30 requests within rate limit window', async () => {
    const server = await startServer({
      devMode: true,
      rateLimit: { generalLimit: 30, generalWindowMs: 60000 },
    });
    const client = createClient(server.url, TOKEN);

    try {
      for (let i = 0; i < 30; i++) {
        const res = await client.get('/api/v1/info');
        assert.equal(res.status, 200, `Request ${i + 1} should succeed`);
      }
    } finally {
      await server.close();
    }
  });

  it('D9: Blocks request 31 with 429', async () => {
    const server = await startServer({
      devMode: true,
      rateLimit: { generalLimit: 30, generalWindowMs: 60000 },
    });
    const client = createClient(server.url, TOKEN);

    try {
      for (let i = 0; i < 30; i++) {
        await client.get('/api/v1/info');
      }

      const res = await client.get('/api/v1/info');
      assert.equal(res.status, 429);
      assert.equal(res.json.error, 'rate_limited');
      assert.ok(res.headers['retry-after']);
    } finally {
      await server.close();
    }
  });

  it('D9: Auth lockout after 5 failed attempts', async () => {
    const server = await startServer({
      devMode: true,
      rateLimit: { authFailLimit: 5, authLockoutMs: 60000 },
    });

    try {
      // 5 failed auth attempts
      for (let i = 0; i < 5; i++) {
        await request(server.url + '/api/v1/info', {
          headers: { Host: '127.0.0.1', 'X-Canary-Token': 'wrong-token' },
        });
      }

      // Even valid token should now be blocked
      const res = await request(server.url + '/api/v1/info', {
        headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN },
      });
      assert.equal(res.status, 429);
      assert.equal(res.json.error, 'auth_locked');
    } finally {
      await server.close();
    }
  });

  it('D9: Lockout expires after window', async () => {
    const LOCKOUT_MS = 200; // Short window for testing
    const server = await startServer({
      devMode: true,
      rateLimit: { authFailLimit: 5, authLockoutMs: LOCKOUT_MS },
    });

    try {
      // Trigger lockout
      for (let i = 0; i < 5; i++) {
        await request(server.url + '/api/v1/info', {
          headers: { Host: '127.0.0.1', 'X-Canary-Token': 'wrong' },
        });
      }

      // Verify locked
      const locked = await request(server.url + '/api/v1/info', {
        headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN },
      });
      assert.equal(locked.status, 429);

      // Wait for lockout to expire
      await new Promise((r) => setTimeout(r, LOCKOUT_MS + 50));

      // Should now work
      const unlocked = await request(server.url + '/api/v1/info', {
        headers: { Host: '127.0.0.1', 'X-Canary-Token': TOKEN },
      });
      assert.equal(unlocked.status, 200);
    } finally {
      await server.close();
    }
  });

  it('D9: Reboot endpoint has its own 1-per-5-min limit', async () => {
    const server = await startServer({ devMode: true });
    const client = createClient(server.url, TOKEN);

    try {
      const first = await client.post('/api/v1/reboot');
      assert.equal(first.status, 200);

      const second = await client.post('/api/v1/reboot');
      assert.equal(second.status, 429);
    } finally {
      await server.close();
    }
  });
});
