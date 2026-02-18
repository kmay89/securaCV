'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

describe('GET /api/v1/witness', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  it('Witness chain has valid hash linkage', async () => {
    const res = await client.get('/api/v1/witness?last=10');
    assert.equal(res.status, 200);
    assert.ok(Array.isArray(res.json.records));
    assert.ok(res.json.records.length > 1);

    const records = res.json.records;
    for (let i = 1; i < records.length; i++) {
      assert.equal(records[i].prev_hash, records[i - 1].hash,
        `Record ${i} prev_hash should equal record ${i - 1} hash`);
    }
  });

  it('Witness records have valid Ed25519 signatures', async () => {
    const res = await client.get('/api/v1/witness?last=10');
    const records = res.json.records;
    const publicKey = server.state.publicKey;

    for (const record of records) {
      const valid = crypto.verify(
        null,
        Buffer.from(record.hash),
        publicKey,
        Buffer.from(record.signature, 'hex')
      );
      assert.ok(valid, `Signature invalid for seq ${record.seq}`);
    }
  });

  it('Witness chain has strictly increasing sequence numbers', async () => {
    const res = await client.get('/api/v1/witness?last=10');
    const records = res.json.records;

    for (let i = 1; i < records.length; i++) {
      assert.equal(records[i].seq, records[i - 1].seq + 1,
        `Seq ${records[i].seq} should be ${records[i - 1].seq + 1}`);
    }
  });

  it('Witness records have expected fields', async () => {
    const res = await client.get('/api/v1/witness?last=1');
    const record = res.json.records[0];
    assert.ok(record.seq);
    assert.ok(record.hash);
    assert.ok(record.prev_hash);
    assert.ok(record.timestamp);
    assert.ok(record.event_type);
    assert.ok(record.zone);
    assert.ok(record.signature);
  });
});
