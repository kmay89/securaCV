'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f';

describe('D10: Camera Peek Default Off', () => {
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

  it('D10: Cannot enable camera_peek via API alone', async () => {
    // Try to enable camera_peek
    const putRes = await client.put('/api/v1/config', {
      privacy: { camera_peek_enabled: true },
    });
    assert.equal(putRes.status, 200);
    assert.ok(putRes.json.pending_physical_confirm);
    assert.ok(putRes.json.pending_physical_confirm.includes('camera_peek_enabled'));

    // Verify it's still false
    const getRes = await client.get('/api/v1/config');
    assert.equal(getRes.json.privacy.camera_peek_enabled, false);
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
