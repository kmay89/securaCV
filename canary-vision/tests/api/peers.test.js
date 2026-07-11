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

  it('peer entries advertise an mDNS hostname for stable pairing', async () => {
    // The SPA prefers mdns_hostname over ip when constructing base_url so
    // pairing survives DHCP lease changes.
    const res = await client.get('/api/v1/peers');
    for (const peer of res.json.peers) {
      assert.ok(peer.mdns_hostname, `peer ${peer.device_id} missing mdns_hostname`);
      assert.match(peer.mdns_hostname, /\.local$/);
    }
  });

  it('peer entries carry a canonical device_type', async () => {
    // The SPA branches its pairing wizard on this: WAP-class devices pair
    // over HTTP, vision/sense are MQTT-onboarded via Home Assistant.
    const res = await client.get('/api/v1/peers');
    const types = ['canary-wap', 'canary-vision', 'canary-sense'];
    for (const peer of res.json.peers) {
      assert.ok(types.includes(peer.device_type),
        `peer ${peer.device_id} has unexpected device_type ${peer.device_type}`);
    }
  });
});

describe('GET /api/v1/peers — dt normalization', () => {
  let server, client;

  before(async () => {
    // Firmware fleet scans relay the mDNS TXT `dt` key; the API must
    // normalize it to the long device_type spelling.
    server = await startServer({
      devMode: true,
      peers: [
        {
          device_id: 'canary-f6a7',
          name: 'Hallway',
          dt: 'canary-sense',
          ip: '192.168.1.120',
          mdns_hostname: 'canary-f6a7.local',
          last_seen: '2026-02-18T14:22:00Z',
        },
      ],
    });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('accepts the mDNS TXT `dt` spelling and serves device_type', async () => {
    const res = await client.get('/api/v1/peers');
    assert.equal(res.status, 200);
    assert.equal(res.json.peers.length, 1);
    assert.equal(res.json.peers[0].device_type, 'canary-sense');
    assert.equal(res.json.peers[0].dt, undefined);
  });
});
