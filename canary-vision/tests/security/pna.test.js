'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer } = require('../helpers/start-server');
const { request } = require('../helpers/test-client');

describe('D4: Chrome PNA Preflight', () => {
  let server;

  before(async () => {
    server = await startServer({ devMode: true });
  });

  after(async () => {
    await server.close();
  });

  it('D4: PNA preflight returns correct headers', async () => {
    const res = await request(server.url + '/api/v1/info', {
      method: 'OPTIONS',
      headers: {
        Host: '127.0.0.1',
        'Access-Control-Request-Private-Network': 'true',
      },
    });
    assert.equal(res.status, 204);
    assert.equal(res.headers['access-control-allow-private-network'], 'true');
    assert.equal(res.headers['private-network-access-name'], 'Canary Vision (Front Porch)');
    assert.equal(res.headers['private-network-access-id'], '497c8741e5287cde');
  });

  it('D4: PNA preflight from an allowed origin carries CORS grants too', async () => {
    // Chrome rejects a PNA preflight that lacks the regular CORS grants,
    // so both header sets must appear on one response.
    const origin = 'http://192.168.1.47'; // the device's own origin
    const res = await request(server.url + '/api/v1/info', {
      method: 'OPTIONS',
      headers: {
        Host: '127.0.0.1',
        Origin: origin,
        'Access-Control-Request-Method': 'GET',
        'Access-Control-Request-Private-Network': 'true',
      },
    });
    assert.equal(res.status, 204);
    assert.equal(res.headers['access-control-allow-private-network'], 'true');
    assert.equal(res.headers['access-control-allow-origin'], origin);
    assert.ok(res.headers['access-control-allow-headers'].includes('X-Canary-Token'));
  });

  it('D4: PNA preflight from a disallowed origin grants PNA but no CORS', async () => {
    const res = await request(server.url + '/api/v1/info', {
      method: 'OPTIONS',
      headers: {
        Host: '127.0.0.1',
        Origin: 'http://evil.com',
        'Access-Control-Request-Private-Network': 'true',
      },
    });
    assert.equal(res.status, 204);
    assert.equal(res.headers['access-control-allow-origin'], undefined,
      'CORS grants must still be withheld from disallowed origins');
  });

  it('D4: Non-PNA OPTIONS does not include PNA headers', async () => {
    const res = await request(server.url + '/api/v1/info', {
      method: 'OPTIONS',
      headers: {
        Host: '127.0.0.1',
        Origin: 'http://192.168.1.47',
      },
    });
    assert.equal(res.headers['private-network-access-name'], undefined);
    assert.equal(res.headers['private-network-access-id'], undefined);
  });
});
