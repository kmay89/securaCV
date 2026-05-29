'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const http = require('node:http');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient, request } = require('../helpers/test-client');
const { EVENT_TYPE_MAP } = require('../../device-api/lib/envelope-bridge');

const TOKEN = TEST_TOKEN;
const ALLOWED = Object.keys(EVENT_TYPE_MAP);

// Open the SSE stream and resolve with the first `witness` event whose record has `wantSeq`
// (or reject on timeout). EventSource can't set headers, so the stream authenticates via a
// single-use ticket passed as a query param.
function waitForWitnessSse(baseUrl, ticket, wantSeq, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    const url = new URL(baseUrl + '/api/v1/witness/stream?ticket=' + encodeURIComponent(ticket));
    const req = http.get({
      hostname: url.hostname, port: url.port, path: url.pathname + url.search,
      headers: { Host: '127.0.0.1' },
    }, (res) => {
      if (res.statusCode !== 200) { reject(new Error('stream status ' + res.statusCode)); return; }
      let buf = '';
      res.on('data', (chunk) => {
        buf += chunk.toString();
        // SSE frames are separated by a blank line.
        let idx;
        while ((idx = buf.indexOf('\n\n')) !== -1) {
          const frame = buf.slice(0, idx);
          buf = buf.slice(idx + 2);
          if (frame.includes('event: witness')) {
            const dataLine = frame.split('\n').find((l) => l.startsWith('data: '));
            if (dataLine) {
              try {
                const record = JSON.parse(dataLine.slice('data: '.length));
                if (record.seq === wantSeq) { req.destroy(); resolve(record); return; }
              } catch { /* ignore non-JSON frames */ }
            }
          }
        }
      });
    });
    req.on('error', (e) => { /* destroyed after success */ if (!req.destroyed) reject(e); });
    const t = setTimeout(() => { req.destroy(); reject(new Error('SSE timeout')); }, timeoutMs);
    if (t.unref) t.unref();
  });
}

describe('POST /api/v1/witness/simulate', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
    // Generous limiter so the functional tests don't trip the trigger throttle (covered separately).
    server.state.simulateRateLimit = { limit: 1000, windowMs: 1000 };
  });

  after(async () => {
    await server.close();
  });

  it('requires a token', async () => {
    const res = await request(server.url + '/api/v1/witness/simulate', {
      method: 'POST',
      headers: { Host: '127.0.0.1', 'Content-Type': 'application/json' },
      body: JSON.stringify({ event_type: 'person_detected', zone: 'front' }),
    });
    assert.equal(res.status, 401);
  });

  it('rejects an unsupported / forbidden event_type', async () => {
    const res = await client.post('/api/v1/witness/simulate', { event_type: 'face_recognized', zone: 'front' });
    assert.equal(res.status, 400);
    assert.equal(res.json.error, 'invalid_event_type');
    assert.deepEqual(res.json.allowed.sort(), ALLOWED.slice().sort());
  });

  it('rejects a zone that is not a short local id', async () => {
    const res = await client.post('/api/v1/witness/simulate', { event_type: 'person_detected', zone: '123 Main St' });
    assert.equal(res.status, 400);
    assert.equal(res.json.error, 'invalid_zone');
  });

  it('accepts every allowlisted event_type', async () => {
    for (const t of ALLOWED) {
      const res = await client.post('/api/v1/witness/simulate', { event_type: t, zone: 'front', force: true });
      assert.equal(res.status, 201, `event_type ${t} should be accepted`);
      assert.equal(res.json.record.event_type, t);
    }
  });

  it('appends a chain-linked, validly signed record (force)', async () => {
    const before = await client.get('/api/v1/witness?last=1');
    const prev = before.json.records[before.json.records.length - 1];

    const res = await client.post('/api/v1/witness/simulate', { event_type: 'person_detected', zone: 'front', force: true });
    assert.equal(res.status, 201);
    const rec = res.json.record;

    assert.equal(rec.prev_hash, prev.hash, 'new record links to previous hash');
    assert.equal(rec.seq, prev.seq + 1, 'sequence increments by one');

    const valid = crypto.verify(null, Buffer.from(rec.hash), server.state.publicKey, Buffer.from(rec.signature, 'hex'));
    assert.ok(valid, 'Ed25519 signature verifies against the device public key');
  });

  it('delivers the simulated event over the SSE stream', async () => {
    const ticketRes = await client.post('/api/v1/witness/stream/ticket');
    assert.equal(ticketRes.status, 200);
    const ticket = ticketRes.json.ticket;

    // Open the stream, then trigger; the record should arrive as a `witness` SSE frame.
    let captured = null;
    const next = (server.state.witnessRecords[server.state.witnessRecords.length - 1].seq) + 1;
    const ssePromise = waitForWitnessSse(server.url, ticket, next).then((r) => { captured = r; });

    // Give the stream a moment to register before emitting.
    await new Promise((r) => setTimeout(r, 100));
    const res = await client.post('/api/v1/witness/simulate', { event_type: 'motion_detected', zone: 'front', force: true });
    assert.equal(res.status, 201);
    assert.equal(res.json.record.seq, next);

    await ssePromise;
    assert.ok(captured, 'received a witness SSE frame');
    assert.equal(captured.seq, next);
  });

  it('suppresses a repeat event within the activity session (no force)', async () => {
    const first = await client.post('/api/v1/witness/simulate', { event_type: 'animal_detected', zone: 'yard' });
    assert.equal(first.status, 201);

    const second = await client.post('/api/v1/witness/simulate', { event_type: 'animal_detected', zone: 'yard' });
    assert.equal(second.status, 202);
    assert.equal(second.json.suppressed, true);
  });

  it('keeps the evidence envelope buildable after simulating each type', async () => {
    for (const t of ALLOWED) {
      const r = await client.post('/api/v1/witness/simulate', { event_type: t, zone: 'front', force: true });
      assert.equal(r.status, 201);
    }
    const verify = await client.post('/api/v1/witness/verify');
    assert.equal(verify.status, 200);
    assert.ok(verify.json.evidence_envelope, 'verify response carries an evidence-envelope report');
    assert.equal(verify.json.evidence_envelope.ok, true, 'envelope still coarsens — no unmappable type leaked in');
  });
});

describe('POST /api/v1/witness/simulate — trigger throttle', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
    server.state.simulateRateLimit = { limit: 2, windowMs: 60000 };
  });

  after(async () => {
    await server.close();
  });

  it('returns 429 with Retry-After once the trigger cap is exceeded', async () => {
    const a = await client.post('/api/v1/witness/simulate', { event_type: 'person_detected', zone: 'front', force: true });
    const b = await client.post('/api/v1/witness/simulate', { event_type: 'person_detected', zone: 'front', force: true });
    const c = await client.post('/api/v1/witness/simulate', { event_type: 'person_detected', zone: 'front', force: true });

    assert.equal(a.status, 201);
    assert.equal(b.status, 201);
    assert.equal(c.status, 429);
    assert.equal(c.json.error, 'rate_limited');
    assert.ok(c.headers['retry-after'], 'sets a Retry-After header');
  });
});
