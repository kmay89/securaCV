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

  // Unknown keys must be rejected, not merged: a typo'd key was previously
  // stored verbatim while the real setting silently kept its default —
  // {"privacy":{"auto_purge_hour":1}} left auto-purge on the longer window.

  it('PUT /api/v1/config rejects a typo of a known key', async () => {
    const res = await client.put('/api/v1/config', {
      privacy: { auto_purge_hour: 1 },
    });
    assert.equal(res.status, 400);
    assert.equal(res.json.error, 'invalid_config');
    assert.match(res.json.message, /auto_purge_hour/);
    // The typo'd key must not have been stored either.
    const after = await client.get('/api/v1/config/privacy');
    assert.equal(after.json.auto_purge_hour, undefined);
  });

  it('PUT /api/v1/config rejects an unknown top-level section', async () => {
    const res = await client.put('/api/v1/config', {
      privacyy: { auto_purge_hours: 1 },
    });
    assert.equal(res.status, 400);
    assert.match(res.json.message, /privacyy/);
  });

  it('PUT /api/v1/config/:section rejects unknown keys in the section', async () => {
    const res = await client.put('/api/v1/config/detection', {
      motion_sensitivty: 5,
    });
    assert.equal(res.status, 400);
    assert.match(res.json.message, /motion_sensitivty/);
  });

  it('PUT /api/v1/config rejects unknown suppression sub-keys', async () => {
    const res = await client.put('/api/v1/config', {
      detection: { suppression: { cooldown_secondz: 10 } },
    });
    assert.equal(res.status, 400);
    assert.match(res.json.message, /cooldown_secondz/);
  });

  // typeof [] === 'object': a JSON array must not slip past the body-object
  // check and have its indices merged into the stored config.

  it('PUT /api/v1/config/:section rejects a JSON array body', async () => {
    const res = await client.put('/api/v1/config/detection', [1, 2, 3]);
    assert.equal(res.status, 400);
    // Array indices must not have been merged into the section.
    const after = await client.get('/api/v1/config/detection');
    assert.equal(after.json['0'], undefined);
  });

  it('PUT /api/v1/config rejects a JSON array body', async () => {
    const res = await client.put('/api/v1/config', [1, 2, 3]);
    assert.equal(res.status, 400);
    assert.match(res.json.message, /array/i);
  });

  it('PUT /api/v1/config rejects an array where a section object belongs', async () => {
    const res = await client.put('/api/v1/config', { privacy: [1, 2] });
    assert.equal(res.status, 400);
  });
});

describe('Webhook URL hardening', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('accepts private LAN and .local webhook URLs', async () => {
    const lan = await client.put('/api/v1/config/integrations', {
      webhook_url: 'http://192.168.1.20:8123/api/webhook/canary#secret-fragment',
    });
    assert.equal(lan.status, 200);
    assert.equal(lan.json.config.integrations.webhook_url, 'http://192.168.1.20:8123/api/webhook/canary');

    const mdns = await client.put('/api/v1/config/integrations', {
      webhook_url: 'https://homeassistant.local/api/webhook/canary',
    });
    assert.equal(mdns.status, 200);
  });

  it('rejects public, loopback, link-local, credentialed, and non-HTTP webhook URLs', async () => {
    const rejected = [
      'https://example.com/hook',
      'http://localhost:3000/api/v1/config',
      'http://127.0.0.1:3000/api/v1/info',
      'http://169.254.169.254/latest/meta-data',
      'https://user:pass@192.168.1.20/hook',
      'file:///etc/passwd',
    ];

    for (const webhook_url of rejected) {
      const res = await client.put('/api/v1/config/integrations', { webhook_url });
      assert.equal(res.status, 400, webhook_url);
      assert.equal(res.json.error, 'invalid_config');
    }
  });
});
