'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient, request } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('Update API', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('GET /api/v1/update/check returns update info', async () => {
    const res = await client.get('/api/v1/update/check');
    assert.equal(res.status, 200);
    assert.equal(res.json.current_version, '0.4.1');
    assert.equal(res.json.available_version, '0.4.2');
    assert.equal(res.json.update_available, true);
    assert.ok(res.json.changelog);
    assert.ok(res.json.sha256);
    assert.ok(res.json.signature);
  });

  it('POST /api/v1/update rejects without firmware file', async () => {
    const res = await client.post('/api/v1/update', {});
    assert.equal(res.status, 400);
    assert.equal(res.json.error, 'no_firmware');
  });

  it('POST /api/v1/update accepts multipart firmware', async () => {
    const boundary = '----TestBoundary' + Date.now();
    const body = [
      `--${boundary}`,
      'Content-Disposition: form-data; name="firmware"; filename="firmware.bin"',
      'Content-Type: application/octet-stream',
      '',
      'fake-firmware-binary-data',
      `--${boundary}--`,
    ].join('\r\n');

    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': `multipart/form-data; boundary=${boundary}`,
      },
      body,
    });
    assert.equal(res.status, 200);
    assert.equal(res.json.ok, true);
  });
});
