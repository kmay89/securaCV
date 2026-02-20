'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('D10: Camera Peek Immutable via API (Invariant I)', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('D10: camera_peek_enabled defaults to false', async () => {
    const res = await client.get('/api/v1/config');
    assert.equal(res.status, 200);
    assert.equal(res.json.privacy.camera_peek_enabled, false);
  });

  it('D10: camera_peek_enabled is immutable via full config PUT', async () => {
    // Try to enable camera_peek via full config update
    const putRes = await client.put('/api/v1/config', {
      privacy: { camera_peek_enabled: true },
    });
    assert.equal(putRes.status, 200);

    // Key should be rejected as immutable
    assert.ok(putRes.json.rejected_immutable);
    assert.ok(putRes.json.rejected_immutable.includes('camera_peek_enabled'));

    // Verify it's still false
    const getRes = await client.get('/api/v1/config');
    assert.equal(getRes.json.privacy.camera_peek_enabled, false);
  });

  it('D10: camera_peek_enabled is immutable via section PUT', async () => {
    // Try to enable camera_peek via privacy section update
    const putRes = await client.put('/api/v1/config/privacy', {
      camera_peek_enabled: true,
    });
    assert.equal(putRes.status, 200);

    // Key should be rejected as immutable
    assert.ok(putRes.json.rejected_immutable);
    assert.ok(putRes.json.rejected_immutable.includes('camera_peek_enabled'));

    // Verify it's still false
    const getRes = await client.get('/api/v1/config/privacy');
    assert.equal(getRes.json.camera_peek_enabled, false);
  });

  it('D10: No API endpoint can enable camera streaming', async () => {
    // After all attempts, peek must still be false
    const res = await client.get('/api/v1/config');
    assert.equal(res.json.privacy.camera_peek_enabled, false);
    assert.equal(res.json.privacy.video_storage, 'none');
    assert.equal(res.json.privacy.semantic_events_only, true);
    assert.equal(res.json.privacy.local_processing_only, true);
  });

  it('D10: Config shows peek is off in fresh state', async () => {
    // Use a fresh server
    const fresh = await startServer({ devMode: true });
    const freshClient = createClient(fresh.url, TOKEN);

    try {
      const res = await freshClient.get('/api/v1/config');
      assert.equal(res.json.privacy.camera_peek_enabled, false);
      assert.equal(res.json.privacy.video_storage, 'none');
      assert.equal(res.json.privacy.semantic_events_only, true);
      assert.equal(res.json.privacy.local_processing_only, true);
    } finally {
      await fresh.close();
    }
  });
});
